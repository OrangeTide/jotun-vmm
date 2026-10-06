# Makefile
# smolvfs provides the bundle store the host 9P server reads from. Point this
# at a checkout of smolvfs; the host compiles the read-path sources directly.
SMOLVFS_DIR ?= third_party/smolvfs
SMOLVFS_SRC = $(SMOLVFS_DIR)/cas.c $(SMOLVFS_DIR)/cas-tree.c \
    $(SMOLVFS_DIR)/cas-pack.c $(SMOLVFS_DIR)/cas-codec.c

CFLAGS = -Wall -Wextra -std=c11 -pthread -Iabi -I$(SMOLVFS_DIR)
LDFLAGS =
ifeq ($(OS),)
    OS := $(shell uname -s)
endif
ARCH := $(shell uname -m)

# Common freestanding guest flags; per-host blocks add the compiler, the
# source, and the text base.
GUEST_CFLAGS = -ffreestanding -fno-pic -fno-pie -nostdlib -fno-builtin -Iabi \
    -fno-asynchronous-unwind-tables -Wl,-e_start -Wl,--build-id=none

GUEST =

ifeq ($(OS), Darwin)
    # macos/arm64: Hypervisor.framework, arm64 guest.
    CFLAGS += -D__APPLE__
    LDFLAGS += -framework Hypervisor
    SRC = host/vmm_hvf.c host/elf.c host/uart.c host/p9srv.c host/p9mem.c \
        host/p9smolvfs.c host/manifest.c host/main.c $(SMOLVFS_SRC)
    CC = clang
    GUEST = test_guest.elf echo.elf libos.elf
    GUEST_SRC = guest/test_guest_arm64.c
    LIBOS_ARCH = guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S
    # Apple's toolchain emits Mach-O; produce an ELF with an LLVM ELF target
    # and lld (install with e.g. `brew install lld`).
    GUEST_CC = clang --target=aarch64-unknown-none-elf -fuse-ld=lld
    # At or above lld's default image base, so no linker-specific flag is needed.
    GUEST_TEXT = 0x200000
else
    CFLAGS += -D__linux__
    SRC = host/vmm_kvm.c host/elf.c host/uart.c host/p9srv.c host/p9mem.c \
        host/p9smolvfs.c host/manifest.c host/main.c $(SMOLVFS_SRC)
    CC = gcc
    GUEST_CFLAGS += -no-pie
    ifeq ($(ARCH), x86_64)
        # linux/x86-64: KVM, 64-bit long-mode x86 guest at 1 MiB.
        GUEST = test_guest.elf echo.elf libos.elf
        GUEST_SRC = guest/test_guest_x86.c
        LIBOS_ARCH = guest/libos/arch/x86_64.c guest/libos/arch/x86_64_ctx.S \
            guest/libos/arch/x86_64_isr.S
        GUEST_CC = $(CC)
        GUEST_TEXT = 0x100000
    endif
    ifeq ($(ARCH), aarch64)
        # linux/arm64: KVM, arm64 guest.
        GUEST = test_guest.elf echo.elf libos.elf
        GUEST_SRC = guest/test_guest_arm64.c
        LIBOS_ARCH = guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S
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
    guest/libos/prog.c guest/libos/jsh.c guest/libos/ns.c $(LIBOS_ARCH)

libos.elf: $(LIBOS_SRC)
	$(GUEST_CC) $(GUEST_CFLAGS) -Iguest/libos -Wl,-Ttext=$(GUEST_TEXT) -o $@ $^

# Standalone programs, linked at the program base and loaded from the bundle.
hello.elf: guest/prog/hello.c
	$(GUEST_CC) $(GUEST_CFLAGS) -Iguest/prog -Wl,-Ttext=$(PROG_TEXT) -o $@ $<

# Build a test bundle depot from example/bundle/ using smolvfs's castool, then
# run with JOTUN_BUNDLE=bundle-depot ./jotun libos.elf.
BUNDLE_DEPOT = bundle-depot

bundle: $(PROGS)
	$(MAKE) -C $(SMOLVFS_DIR) castool
	rm -rf $(BUNDLE_DEPOT) bundle-staging
	mkdir bundle-staging
	cp example/bundle/* bundle-staging/
	cp hello.elf bundle-staging/hello
	$(SMOLVFS_DIR)/castool -d $(BUNDLE_DEPOT) import root bundle-staging/*
	$(SMOLVFS_DIR)/castool -d $(BUNDLE_DEPOT) pack

clean:
	rm -f $(TARGET) test_guest.elf echo.elf libos.elf $(PROGS)
	rm -rf $(BUNDLE_DEPOT) bundle-staging

.PHONY: all clean bundle
