# Makefile
# smolvfs provides the bundle store the host 9P server reads from. Its sources
# are vendored in-tree under third_party/smolvfs (no external checkout); the
# host compiles them directly. The read path backs the 9P server; the sign path
# (cas-sign/cas-topic with the bundled monocypher EdDSA-BLAKE2b backend) verifies
# pulled refs for M8; the miniz codec reads DEFLATE-compressed objects.
SMOLVFS_DIR = third_party/smolvfs
SMOLVFS_READ = $(SMOLVFS_DIR)/cas.c $(SMOLVFS_DIR)/cas-tree.c \
    $(SMOLVFS_DIR)/cas-pack.c $(SMOLVFS_DIR)/cas-codec.c
SMOLVFS_SIGN = $(SMOLVFS_DIR)/cas-sign.c $(SMOLVFS_DIR)/cas-topic.c \
    $(SMOLVFS_DIR)/cas-sign-monocypher.c $(SMOLVFS_DIR)/third_party/monocypher.c
SMOLVFS_CODEC = $(SMOLVFS_DIR)/cas-codec-miniz.c $(SMOLVFS_DIR)/third_party/miniz.c
SMOLVFS_SRC = $(SMOLVFS_READ) $(SMOLVFS_SIGN) $(SMOLVFS_CODEC)

# The vendored crypto and codec select their backends at compile time.
SMOLVFS_DEFS = -DCAS_WITH_MONOCYPHER -DCAS_WITH_MINIZ -DMINIZ_NO_STDIO

# Expose the BSD names (flock, LOCK_EX in <sys/file.h>) the smolvfs sources use;
# a strict -std hides them otherwise. _DARWIN_C_SOURCE unhides them on macOS,
# _DEFAULT_SOURCE on glibc/musl. Both are harmless on the other platform.
# _DEFAULT_SOURCE is defined empty to match cas.c's own definition (no warning).
HOST_DEFS = -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE=

CFLAGS = -Wall -Wextra -std=c11 -pthread -Iabi -I$(SMOLVFS_DIR) \
    $(SMOLVFS_DEFS) $(HOST_DEFS)
LDFLAGS =
ifeq ($(OS),)
    OS := $(shell uname -s)
endif
ARCH := $(shell uname -m)

# Common freestanding guest flags; per-host blocks add the compiler, the
# source, and the text base.
# -mgeneral-regs-only keeps the compiler from emitting FP/SIMD (SSE on x86, NEON
# on arm64) for things like struct copies. The kernel is integer-only, so
# forbidding them costs nothing and avoids a fault from an unexpected FP
# instruction. The one exception is the software rasterizer (build/triton.o),
# built with the FPU on; the guest enables the FPU at boot and preserves FP
# state across task switches. Works the same under gcc and clang, on both arches.
GUEST_CFLAGS = -ffreestanding -fno-pic -fno-pie -nostdlib -fno-builtin -Iabi \
    -fno-asynchronous-unwind-tables -mgeneral-regs-only \
    -Wl,-e_start -Wl,--build-id=none

GUEST =

ifeq ($(OS), Darwin)
    # macos/arm64: Hypervisor.framework, arm64 guest.
    CFLAGS += -D__APPLE__
    LDFLAGS += -framework Hypervisor
    SRC = host/vmm_hvf.c host/elf.c host/uart.c host/p9srv.c host/p9mem.c \
        host/p9smolvfs.c host/p9overlay.c host/manifest.c host/net.c host/draw.c host/drawws.c host/png.c host/http.c host/bundlepull.c host/main.c $(SMOLVFS_SRC)
    CC = clang
    GUEST = test_guest.elf echo.elf libos.elf
    GUEST_SRC = guest/test_guest_arm64.c
    LIBOS_ARCH = guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S \
        guest/libos/arch/arm64_boot.S
    # Keep atomic builtins inline (LL/SC or LSE) rather than calling the libgcc
    # outline-atomic helpers, which are absent under -nostdlib.
    GUEST_CFLAGS += -mno-outline-atomics
    # Apple's toolchain emits Mach-O; produce an ELF with an LLVM ELF target
    # and lld (install with e.g. `brew install lld`).
    GUEST_CC = clang --target=aarch64-unknown-none-elf -fuse-ld=lld
    # At or above lld's default image base, so no linker-specific flag is needed.
    GUEST_TEXT = 0x200000
else
    CFLAGS += -D__linux__
    SRC = host/vmm_kvm.c host/elf.c host/uart.c host/p9srv.c host/p9mem.c \
        host/p9smolvfs.c host/p9overlay.c host/manifest.c host/net.c host/draw.c host/drawws.c host/png.c host/http.c host/bundlepull.c host/main.c $(SMOLVFS_SRC)
    CC = gcc
    GUEST_CFLAGS += -no-pie
    ifeq ($(ARCH), x86_64)
        # linux/x86-64: KVM, 64-bit long-mode x86 guest at 1 MiB.
        GUEST = test_guest.elf echo.elf libos.elf
        GUEST_SRC = guest/test_guest_x86.c
        LIBOS_ARCH = guest/libos/arch/x86_64.c guest/libos/arch/x86_64_ctx.S \
            guest/libos/arch/x86_64_isr.S guest/libos/arch/x86_64_boot.S
        GUEST_CC = $(CC)
        GUEST_TEXT = 0x100000
    endif
    ifeq ($(ARCH), aarch64)
        # linux/arm64: KVM, arm64 guest.
        GUEST = test_guest.elf echo.elf libos.elf
        GUEST_SRC = guest/test_guest_arm64.c
        LIBOS_ARCH = guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S \
            guest/libos/arch/arm64_boot.S
        # Inline atomics, not the libgcc outline helpers (absent, -nostdlib).
        GUEST_CFLAGS += -mno-outline-atomics
        GUEST_CC = $(CC)
        GUEST_TEXT = 0x200000
    endif
endif

TARGET = jotun

# Bundled programs are standalone ELFs whose image lies within the program
# region that begins at JOTUN_PROG_BASE (0x400000, abi/jotun_svc.h). The linker
# puts the ELF headers one page below -Ttext, so text goes at base + one page,
# which lands the headers exactly at the region base. Same on both arches.
PROG_TEXT = 0x401000
PROGS = hello.elf

all: $(TARGET) $(GUEST) $(PROGS)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
ifeq ($(OS), Darwin)
	codesign --force --entitlements entitlements.plist --sign - $@
endif

# Freestanding guests share one link line; only the source differs.
GUEST_LINK = $(GUEST_CC) $(GUEST_CFLAGS) -Wl,-Ttext=$(GUEST_TEXT) -o $@ $<

test_guest.elf: $(GUEST_SRC)
	$(GUEST_LINK)

echo.elf: guest/echo.c
	$(GUEST_LINK)

# The library OS is multiple translation units: the portable core plus the
# selected arch file. It needs its own include path and links all objects.
LIBOS_SRC = guest/libos/main.c guest/libos/mem.c guest/libos/task.c \
    guest/libos/boot.c guest/libos/p9.c guest/libos/elfload.c \
    guest/libos/prog.c guest/libos/jsh.c guest/libos/ns.c guest/libos/net.c \
    guest/libos/draw.c guest/libos/string.c $(LIBOS_ARCH)

# triton.c is the only guest translation unit built with the FPU enabled: it is
# the software rasterizer and uses floating point. The rest of the guest stays
# integer-only (-mgeneral-regs-only). Compile it to an object on its own with
# that flag dropped, then link it with the integer-only sources. The guest
# enables the FPU at boot and preserves FP state across task switches, so this
# object's floating point is safe to run.
GUEST_FP_CFLAGS = $(filter-out -mgeneral-regs-only -Wl%,$(GUEST_CFLAGS))
LIBOS_FP_OBJ = build/triton.o

build:
	mkdir -p build

build/triton.o: guest/libos/triton.c guest/libos/triton.h | build
	$(GUEST_CC) $(GUEST_FP_CFLAGS) -Iguest/libos -c -o $@ $<

libos.elf: $(LIBOS_SRC) $(LIBOS_FP_OBJ)
	$(GUEST_CC) $(GUEST_CFLAGS) -Iguest/libos -Wl,-Ttext=$(GUEST_TEXT) -o $@ $(LIBOS_SRC) $(LIBOS_FP_OBJ)

# Standalone programs, linked at the program base and loaded from the bundle.
hello.elf: guest/prog/hello.c
	$(GUEST_CC) $(GUEST_CFLAGS) -Iguest/prog -Wl,-Ttext=$(PROG_TEXT) -o $@ $<

# Build a test bundle depot from example/bundle/ using castool, then run with
# JOTUN_BUNDLE=bundle-depot ./jotun libos.elf. castool is built from the
# vendored smolvfs sources (no external checkout). Signing is not enabled here;
# the ref-signing path arrives with M8.
CASTOOL_SRC = $(SMOLVFS_DIR)/castool.c $(SMOLVFS_DIR)/cas-gc.c $(SMOLVFS_SRC)
BUNDLE_DEPOT = bundle-depot

# castool builds with the same backends as the host, so it can sign refs
# (keygen/publish) and write compressed packs.
castool: $(CASTOOL_SRC)
	$(CC) $(CFLAGS) -o $@ $^

bundle: $(PROGS) castool
	rm -rf $(BUNDLE_DEPOT) bundle-staging
	mkdir bundle-staging
	cp example/bundle/* bundle-staging/
	cp hello.elf bundle-staging/hello
	./castool -d $(BUNDLE_DEPOT) import root bundle-staging/*
	./castool -d $(BUNDLE_DEPOT) pack

# macsyntax: syntax-check the macOS-only net path from a Linux dev box, so the
# kqueue backend does not rot between real macOS builds. It compiles host/net.c
# with the __APPLE__ branch selected against a minimal shim of the kqueue API
# (macOS has <sys/event.h>; Linux does not; see test/macshim). SO_NOSIGPIPE is
# given its Darwin value so set_nosigpipe's setsockopt line is exercised too.
# This needs clang; building and running on macOS remains the real confirmation.
MACSHIM = test/macshim
macsyntax:
	clang -fsyntax-only -Wall -Wextra -std=c11 -D__APPLE__ \
	    -DSO_NOSIGPIPE=0x1022 -I$(MACSHIM) -Iabi -I$(SMOLVFS_DIR) host/net.c
	@echo "macsyntax: macOS net path OK"

clean:
	rm -f $(TARGET) test_guest.elf echo.elf libos.elf $(PROGS) castool
	rm -rf $(BUNDLE_DEPOT) bundle-staging build

.PHONY: all clean bundle macsyntax
