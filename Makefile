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

# Linker selection for freestanding ELF links (the Darwin native guest and every
# cross-built arch go through clang + lld). Apple's clang rejects the bare
# "-fuse-ld=lld" name and wants a path, and Homebrew installs lld keg-only so its
# ld.lld is off PATH. Resolve an absolute ld.lld when one is findable, which both
# Apple and LLVM clang accept, and fall back to the bare name for toolchains that
# resolve it themselves (an LLVM clang with lld already on PATH). Override by
# passing FUSE_LD= on the command line.
LLD := $(shell command -v ld.lld 2>/dev/null)
ifeq ($(LLD),)
    LLD := $(firstword $(wildcard /opt/homebrew/opt/lld/bin/ld.lld \
                                  /usr/local/opt/lld/bin/ld.lld))
endif
ifeq ($(LLD),)
    FUSE_LD := -fuse-ld=lld
else
    FUSE_LD := -fuse-ld=$(LLD)
endif

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
        host/p9smolvfs.c host/p9overlay.c host/p9graft.c host/p9hostdir.c host/manifest.c host/jimage.c host/net.c host/http.c host/bundlepull.c host/main.c $(SMOLVFS_SRC)
    CC = clang
    GUEST = libos.elf
    GUEST_SRC = guest/test_guest_arm64.c
    LIBOS_ARCH = guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S \
        guest/libos/arch/arm64_boot.S guest/libos/arch/arm64_linuxsys.S \
        guest/libos/arch/arm64_vmspace.c
    # Keep atomic builtins inline (LL/SC or LSE) rather than calling the libgcc
    # outline-atomic helpers, which are absent under -nostdlib.
    GUEST_CFLAGS += -mno-outline-atomics
    # Apple's toolchain emits Mach-O; produce an ELF with an LLVM ELF target
    # and lld (install with e.g. `brew install lld`). See FUSE_LD above.
    GUEST_CC = clang --target=aarch64-unknown-none-elf $(FUSE_LD)
    # At or above lld's default image base, so no linker-specific flag is needed.
    GUEST_TEXT = 0x200000
else
    CFLAGS += -D__linux__
    SRC = host/vmm_kvm.c host/elf.c host/uart.c host/p9srv.c host/p9mem.c \
        host/p9smolvfs.c host/p9overlay.c host/p9graft.c host/p9hostdir.c host/manifest.c host/jimage.c host/net.c host/http.c host/bundlepull.c host/main.c $(SMOLVFS_SRC)
    CC = gcc
    GUEST_CFLAGS += -no-pie
    ifeq ($(ARCH), x86_64)
        # linux/x86-64: KVM, 64-bit long-mode x86 guest at 1 MiB.
        GUEST = libos.elf
        GUEST_SRC = guest/test_guest_x86.c
        LIBOS_ARCH = guest/libos/arch/x86_64.c guest/libos/arch/x86_64_ctx.S \
            guest/libos/arch/x86_64_isr.S guest/libos/arch/x86_64_boot.S \
            guest/libos/arch/x86_64_linuxsys.S guest/libos/arch/x86_64_fault.S \
            guest/libos/arch/x86_64_vmspace.c
        GUEST_CC = $(CC)
        GUEST_TEXT = 0x100000
    endif
    ifeq ($(ARCH), aarch64)
        # linux/arm64: KVM, arm64 guest.
        GUEST = libos.elf
        GUEST_SRC = guest/test_guest_arm64.c
        LIBOS_ARCH = guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S \
            guest/libos/arch/arm64_boot.S guest/libos/arch/arm64_linuxsys.S \
            guest/libos/arch/arm64_vmspace.c
        # Inline atomics, not the libgcc outline helpers (absent, -nostdlib).
        GUEST_CFLAGS += -mno-outline-atomics
        GUEST_CC = $(CC)
        GUEST_TEXT = 0x200000
    endif
endif

TARGET = jotun

# Canonical guest arch token (uname-style, matching the host's JOTUN_ARCH),
# normalising Darwin's arm64 to aarch64. This is the arch this box builds
# natively; other arches in an image are cross-built below.
ifeq ($(OS),Darwin)
GUEST_ARCH := aarch64
else ifeq ($(ARCH),x86_64)
GUEST_ARCH := x86_64
else
GUEST_ARCH := aarch64
endif

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

# The library OS is multiple translation units: the portable core plus the
# selected arch file. It needs its own include path and links all objects. The
# guest is the Linux binary-compatibility emulation; networking (net.c, ns.c)
# backs its sockets and 9P (p9.c) its filesystem.
LIBOS_SRC_COMMON = guest/libos/main.c guest/libos/mem.c guest/libos/task.c \
    guest/libos/boot.c guest/libos/p9.c guest/libos/elfload.c \
    guest/libos/ns.c guest/libos/net.c \
    guest/libos/string.c guest/libos/fmt.c \
    guest/libos/print.c \
    guest/libos/linuxelf.c
LIBOS_SRC = $(LIBOS_SRC_COMMON) $(LIBOS_ARCH)

# The guest is integer-only (-mgeneral-regs-only) except fmtfloat.c, the format
# engine's floating-point unit, which needs the FPU for printf's f/e/g. Compile
# it on its own with that flag dropped, then link it with the integer-only
# sources. The guest enables the FPU at boot and preserves FP state across task
# switches, so this object's floating point is safe to run.
GUEST_FP_CFLAGS = $(filter-out -mgeneral-regs-only -Wl%,$(GUEST_CFLAGS))
LIBOS_FP_OBJ = build/fmtfloat.o

build:
	mkdir -p build

# fmtfloat.c is the format engine's floating-point unit, built with the FPU on
# and linked into the integer-only formatter.
build/fmtfloat.o: guest/libos/fmtfloat.c guest/libos/fmtfloat.h | build
	$(GUEST_CC) $(GUEST_FP_CFLAGS) -Iguest/libos -c -o $@ $<

libos.elf: $(LIBOS_SRC) $(LIBOS_FP_OBJ)
	$(GUEST_CC) $(GUEST_CFLAGS) -Iguest/libos -Wl,-Ttext=$(GUEST_TEXT) -o $@ $(LIBOS_SRC) $(LIBOS_FP_OBJ)

# The program C library (jlibc): the formatter and string primitives shared
# with the libOS, plus program-side printf and numeric parsing, compiled to
# objects and linked into each bundle program. Kept as plain objects rather than
# an ar archive, for cross-toolchain portability (no ranlib step); building each
# function into its own section and linking with --gc-sections drops whatever a
# given program does not use, so unused library code costs it nothing.
PROG_INC = -Iguest/prog/libc -Iguest/prog -Iguest/libos -Iabi
PROG_LIBC_OBJS = build/libc_fmt.o build/libc_fmtfloat.o build/libc_string.o \
    build/libc_io.o build/libc_stdlib.o build/libc_malloc.o build/libc_sbrk.o
GUEST_COBJ_CFLAGS = $(filter-out -Wl%,$(GUEST_CFLAGS)) \
    -ffunction-sections -fdata-sections
# The float unit needs the FPU, so it drops -mgeneral-regs-only like triton.o.
GUEST_COBJ_FP_CFLAGS = $(filter-out -mgeneral-regs-only,$(GUEST_COBJ_CFLAGS))

build/libc_fmt.o: guest/libos/fmt.c | build
	$(GUEST_CC) $(GUEST_COBJ_CFLAGS) $(PROG_INC) -c -o $@ $<
build/libc_fmtfloat.o: guest/libos/fmtfloat.c | build
	$(GUEST_CC) $(GUEST_COBJ_FP_CFLAGS) $(PROG_INC) -c -o $@ $<
build/libc_string.o: guest/libos/string.c | build
	$(GUEST_CC) $(GUEST_COBJ_CFLAGS) $(PROG_INC) -c -o $@ $<
build/libc_io.o: guest/prog/libc/progio.c | build
	$(GUEST_CC) $(GUEST_COBJ_CFLAGS) $(PROG_INC) -c -o $@ $<
build/libc_stdlib.o: guest/prog/libc/progstdlib.c | build
	$(GUEST_CC) $(GUEST_COBJ_CFLAGS) $(PROG_INC) -c -o $@ $<
build/libc_malloc.o: guest/prog/libc/jmalloc.c | build
	$(GUEST_CC) $(GUEST_COBJ_CFLAGS) $(PROG_INC) -c -o $@ $<
build/libc_sbrk.o: guest/prog/libc/jsbrk.c | build
	$(GUEST_CC) $(GUEST_COBJ_CFLAGS) $(PROG_INC) -c -o $@ $<

# Standalone programs, linked at the program base and loaded from the bundle,
# with the program C library and dead-code elimination of its unused parts.
hello.elf: guest/prog/hello.c $(PROG_LIBC_OBJS)
	$(GUEST_CC) $(GUEST_CFLAGS) -Wl,--gc-sections $(PROG_INC) \
	    -Wl,-Ttext=$(PROG_TEXT) -o $@ guest/prog/hello.c $(PROG_LIBC_OBJS)

build/malloctest: guest/prog/malloctest.c $(PROG_LIBC_OBJS) | build
	$(GUEST_CC) $(GUEST_CFLAGS) -Wl,--gc-sections $(PROG_INC) \
	    -Wl,-Ttext=$(PROG_TEXT) -o $@ guest/prog/malloctest.c $(PROG_LIBC_OBJS)

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

# jbundle assembles a jotun guest image: it imports a staging directory per
# arch into the depot and writes the jimage manifest object that ties the
# per-arch roots together. It links the same smolvfs sources as the host.
jbundle: host/jbundle.c $(SMOLVFS_SRC)
	$(CC) $(CFLAGS) -o $@ $^

# Multi-arch image build. The native arch reuses the tested libos.elf/hello.elf;
# every other arch is cross-built with clang + lld to a freestanding ELF. The
# per-arch guest flags are self-contained here, independent of the native
# single-arch GUEST_CFLAGS above. BUNDLE_ARCHS selects which arches go into the
# image; the default is all supported arches, so a plain `make bundle` produces
# a multi-arch image (which needs clang + lld for the non-native arch; set
# BUNDLE_ARCHS to this host's arch to build a single-arch image with no cross
# toolchain).
BUNDLE_ARCHS ?= x86_64 aarch64

GUEST_BASE = -ffreestanding -fno-pic -fno-pie -nostdlib -fno-builtin -Iabi \
    -fno-asynchronous-unwind-tables -mgeneral-regs-only \
    -Wl,-e_start -Wl,--build-id=none
GUEST_BASE_FP = $(filter-out -mgeneral-regs-only,$(GUEST_BASE))
GUEST_BASE_OBJ = $(filter-out -Wl%,$(GUEST_BASE)) -ffunction-sections \
    -fdata-sections
GUEST_BASE_OBJ_FP = $(filter-out -mgeneral-regs-only,$(GUEST_BASE_OBJ))

CROSSCC_x86_64  ?= clang --target=x86_64-unknown-none-elf $(FUSE_LD)
CROSSCC_aarch64 ?= clang --target=aarch64-unknown-none-elf $(FUSE_LD)
TEXT_x86_64  := 0x100000
TEXT_aarch64 := 0x200000
AFLAGS_x86_64  :=
AFLAGS_aarch64 := -mno-outline-atomics
ASRC_x86_64  := guest/libos/arch/x86_64.c guest/libos/arch/x86_64_ctx.S \
    guest/libos/arch/x86_64_isr.S guest/libos/arch/x86_64_boot.S \
    guest/libos/arch/x86_64_linuxsys.S guest/libos/arch/x86_64_fault.S \
            guest/libos/arch/x86_64_vmspace.c
ASRC_aarch64 := guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S \
    guest/libos/arch/arm64_boot.S guest/libos/arch/arm64_linuxsys.S \
    guest/libos/arch/arm64_vmspace.c

# Generate the cross-build rules for one arch: the kernel (libos) and the hello
# program, under build/img/<arch>/. Used only for arches other than this host's.
define IMG_ARCH_template
build/img/$(1):
	mkdir -p build/img/$(1)
build/img/$(1)/fmtfloat.o: guest/libos/fmtfloat.c guest/libos/fmtfloat.h | build/img/$(1)
	$(2) $$(GUEST_BASE_FP) $(4) -Iguest/libos -c -o $$@ $$<
build/img/$(1)/kernel: $$(LIBOS_SRC_COMMON) $(5) build/img/$(1)/fmtfloat.o | build/img/$(1)
	$(2) $$(GUEST_BASE) $(4) -Iguest/libos -Wl,-Ttext=$(3) -o $$@ \
	    $$(LIBOS_SRC_COMMON) $(5) build/img/$(1)/fmtfloat.o
build/img/$(1)/libc_fmt.o: guest/libos/fmt.c | build/img/$(1)
	$(2) $$(GUEST_BASE_OBJ) $(4) $$(PROG_INC) -c -o $$@ $$<
build/img/$(1)/libc_fmtfloat.o: guest/libos/fmtfloat.c | build/img/$(1)
	$(2) $$(GUEST_BASE_OBJ_FP) $(4) $$(PROG_INC) -c -o $$@ $$<
build/img/$(1)/libc_string.o: guest/libos/string.c | build/img/$(1)
	$(2) $$(GUEST_BASE_OBJ) $(4) $$(PROG_INC) -c -o $$@ $$<
build/img/$(1)/libc_io.o: guest/prog/libc/progio.c | build/img/$(1)
	$(2) $$(GUEST_BASE_OBJ) $(4) $$(PROG_INC) -c -o $$@ $$<
build/img/$(1)/libc_stdlib.o: guest/prog/libc/progstdlib.c | build/img/$(1)
	$(2) $$(GUEST_BASE_OBJ) $(4) $$(PROG_INC) -c -o $$@ $$<
build/img/$(1)/libc_malloc.o: guest/prog/libc/jmalloc.c | build/img/$(1)
	$(2) $$(GUEST_BASE_OBJ) $(4) $$(PROG_INC) -c -o $$@ $$<
build/img/$(1)/libc_sbrk.o: guest/prog/libc/jsbrk.c | build/img/$(1)
	$(2) $$(GUEST_BASE_OBJ) $(4) $$(PROG_INC) -c -o $$@ $$<
build/img/$(1)/hello: guest/prog/hello.c build/img/$(1)/libc_fmt.o \
    build/img/$(1)/libc_fmtfloat.o build/img/$(1)/libc_string.o \
    build/img/$(1)/libc_io.o build/img/$(1)/libc_stdlib.o \
    build/img/$(1)/libc_malloc.o build/img/$(1)/libc_sbrk.o | build/img/$(1)
	$(2) $$(GUEST_BASE) $(4) -Wl,--gc-sections $$(PROG_INC) \
	    -Wl,-Ttext=$$(PROG_TEXT) -o $$@ guest/prog/hello.c \
	    build/img/$(1)/libc_fmt.o build/img/$(1)/libc_fmtfloat.o \
	    build/img/$(1)/libc_string.o \
	    build/img/$(1)/libc_io.o build/img/$(1)/libc_stdlib.o \
	    build/img/$(1)/libc_malloc.o build/img/$(1)/libc_sbrk.o
endef

NONNATIVE_ARCHS := $(filter-out $(GUEST_ARCH),x86_64 aarch64)
$(foreach a,$(NONNATIVE_ARCHS),$(eval $(call IMG_ARCH_template,$(a),$(CROSSCC_$(a)),$(TEXT_$(a)),$(AFLAGS_$(a)),$(ASRC_$(a)))))

# The kernel and program for one arch: the native arch reuses the top-level
# build, every other arch comes from its cross-build directory.
img_kernel = $(if $(filter $(1),$(GUEST_ARCH)),libos.elf,build/img/$(1)/kernel)
img_prog   = $(if $(filter $(1),$(GUEST_ARCH)),hello.elf,build/img/$(1)/hello)
BUNDLE_DEPS = $(foreach a,$(BUNDLE_ARCHS),$(call img_kernel,$(a)) $(call img_prog,$(a)))

# Assemble a per-arch staging tree (the shared files plus boot/kernel and the
# program for that arch) and build one multi-arch image from it with jbundle.
bundle: castool jbundle $(BUNDLE_DEPS)
	rm -rf $(BUNDLE_DEPOT) bundle-staging
	@args=""; \
	for a in $(BUNDLE_ARCHS); do \
	    d=bundle-staging/$$a; \
	    mkdir -p $$d/boot; \
	    cp example/bundle/* $$d/; \
	    [ -z "$(EXTRA_BUNDLE_FILES)" ] || cp $(EXTRA_BUNDLE_FILES) $$d/; \
	    if [ "$$a" = "$(GUEST_ARCH)" ]; then \
	        k=libos.elf; p=hello.elf; \
	    else \
	        k=build/img/$$a/kernel; p=build/img/$$a/hello; \
	    fi; \
	    cp $$k $$d/boot/kernel; \
	    cp $$p $$d/hello; \
	    args="$$args --arch $$a=$$d"; \
	    echo "bundle: staged $$a from $$k"; \
	done; \
	./jbundle -d $(BUNDLE_DEPOT) --ref root --name jotun-demo $$args
	./castool -d $(BUNDLE_DEPOT) pack

## --- Linux binary-compat test programs (example/linux*.c) ------------------
## These run unmodified static Linux binaries in the `bin` guest, exercising the
## VFS, heap, fork/exec, signals, threads, mmap, and preemption. They run
## natively, so on x86-64 (KVM) and on macos/arm64 (HVF). See
## guest/libos/linuxelf.c and doc/m16-linux-spike.md.
##
## Build with a static musl toolchain, picked by arch and overridable with
## SPIKE_CC (x86-64: musl-gcc; aarch64: aarch64-linux-musl-gcc). A host with no
## musl cross-compiler can instead point SPIKE_BIN_DIR at a directory of
## pre-built binaries (static non-PIE ET_EXEC at 0x400000, matching this arch).
## linuxtmp is excluded from the wildcard suite: it needs a writable host mount
## supplied by a manifest ("mount tmp host:<dir> rw"), which the bin-guest smoke
## harness does not provide. It has its own target, linux-tmp-mount, below.
LINUX_SPIKE_SRCS := $(filter-out example/linuxtmp.c,$(wildcard example/linux*.c))
LINUX_SPIKE_BINS := $(patsubst example/%.c,build/%,$(LINUX_SPIKE_SRCS))
LINUX_SPIKE_PROGS := $(patsubst build/%,%,$(LINUX_SPIKE_BINS))
## linuxchurn is a long-running stress reproducer, not a unit test: it is built
## and bundled like the others but kept out of the smoke loop, which runs each
## program with no arguments. Run it explicitly with linux-spike.
SMOKE_PROGS := $(filter-out linuxchurn,$(LINUX_SPIKE_PROGS))
SPIKE_CC_x86_64  ?= musl-gcc
SPIKE_CC_aarch64 ?= aarch64-linux-musl-gcc
SPIKE_CC ?= $(SPIKE_CC_$(ARCH))
SPIKE_BIN_DIR ?=

## The guest preempts Linux user loops by default now, so the preemptible-loop
## test makes progress and the rest tolerate the timer. SPIKE_SECS caps a run
## through a timeout command when one is on PATH, so a regression that hangs
## still fails the suite. The cap holds for the slowest supported host: linuxbigmem
## demand-pages 2.5 GiB and takes low-20s seconds on a Raspberry Pi 4, so 20 was
## too tight there; 45 leaves margin while still catching a real hang.
SPIKE_ENV  ?= JOTUN_HZ=100
SPIKE_SECS ?= 45
TIMEOUT    := $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null)

## Build one test program. A pattern rule, so editing one source rebuilds only
## that binary (the old rule rebuilt all of them on every run). With
## SPIKE_BIN_DIR set, copy a pre-built binary instead of compiling.
build/linux%: example/linux%.c | build
	@if [ -n "$(SPIKE_BIN_DIR)" ]; then \
	    cp "$(SPIKE_BIN_DIR)/$(@F)" "$@"; \
	else \
	    command -v $(SPIKE_CC) >/dev/null 2>&1 || \
	        { echo "need a static musl compiler on PATH (SPIKE_CC=, default $(SPIKE_CC)), or set SPIKE_BIN_DIR="; exit 1; }; \
	    $(SPIKE_CC) -static -no-pie -O2 -o "$@" "$<"; \
	fi

## Build every test program.
linux-progs: $(LINUX_SPIKE_BINS)

## Run one test program in the bin guest (default linuxhello):
##   make linux-spike SPIKE_PROG=linuxcat
## All programs are bundled, because some (linuxexec) run their peers.
SPIKE_PROG ?= linuxhello
linux-spike: $(TARGET) libos.elf $(LINUX_SPIKE_BINS)
	$(MAKE) bundle BUNDLE_ARCHS=$(GUEST_ARCH) EXTRA_BUNDLE_FILES="$(LINUX_SPIKE_BINS)"
	$(SPIKE_ENV) JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root \
	    ./$(TARGET) libos.elf bin $(SPIKE_PROG)

## Kernel-command-line test. A manifest sets "cmdline <string>"; linuxcmdline
## reads /proc/cmdline in the guest and prints it. The test passes only when the
## printed value matches the string the manifest supplied, which proves the host
## passes a command line through the boot block and the guest serves it. Separate
## from linux-smoke because it needs a manifest.
linux-cmdline: $(TARGET) libos.elf build/linuxcmdline
	@set -e; \
	cl='root=/dev/vda ro quiet jotun.test=1'; \
	mf=build/cmdline.manifest; \
	printf 'bundle %s\nref root\nsubtree /\ninit bin linuxcmdline\ncmdline %s\n' \
	    "$(BUNDLE_DEPOT)" "$$cl" > $$mf; \
	$(MAKE) bundle BUNDLE_ARCHS=$(GUEST_ARCH) EXTRA_BUNDLE_FILES="build/linuxcmdline"; \
	out=`$(SPIKE_ENV) $(if $(TIMEOUT),$(TIMEOUT) $(SPIKE_SECS)) \
	    ./$(TARGET) -m $$mf libos.elf 2>&1`; \
	echo "$$out"; \
	rm -f $$mf; \
	if echo "$$out" | grep -qF "cmdline: [$$cl]"; then echo "linux-cmdline: ok"; \
	else echo "linux-cmdline: FAILED"; exit 1; fi

## Writable-host-mount test. A manifest maps the guest's /tmp to a fresh host
## directory ("mount tmp host:<dir> rw"); linuxtmp then stats /tmp, creates a
## file under it, and reads the bytes back. The test passes only when the guest
## prints "tmp ok" AND the file lands on the host with the right contents, which
## proves a create/write under /tmp routes to the host directory rather than the
## in-RAM overlay. Separate from linux-smoke because it needs a manifest.
linux-tmp-mount: $(TARGET) libos.elf build/linuxtmp
	@set -e; \
	hostdir=`mktemp -d`; \
	mf=build/tmp-mount.manifest; \
	printf 'bundle %s\nref root\nsubtree /\ninit bin linuxtmp\nmount tmp host:%s rw\n' \
	    "$(BUNDLE_DEPOT)" "$$hostdir" > $$mf; \
	$(MAKE) bundle BUNDLE_ARCHS=$(GUEST_ARCH) EXTRA_BUNDLE_FILES="build/linuxtmp"; \
	out=`$(SPIKE_ENV) $(if $(TIMEOUT),$(TIMEOUT) $(SPIKE_SECS)) \
	    ./$(TARGET) -m $$mf libos.elf 2>&1`; \
	echo "$$out"; \
	ok=1; \
	echo "$$out" | grep -q 'tmp ok' || { echo "FAIL: guest did not print 'tmp ok'"; ok=0; }; \
	[ "`cat $$hostdir/jotest.txt 2>/dev/null`" = "tmp-mount-ok" ] || \
	    { echo "FAIL: host file missing or wrong contents"; ok=0; }; \
	rm -rf $$hostdir $$mf; \
	if [ $$ok -eq 1 ]; then echo "linux-tmp-mount: ok"; \
	else echo "linux-tmp-mount: FAILED"; exit 1; fi

## Run the whole suite and report pass/fail. A program passes when the guest
## reaches a clean halt with no unhandled trap; a hang (caught by the timeout)
## or a guest fault fails. Exits nonzero if any program fails, so it works as a
## regression check.
linux-smoke: $(TARGET) libos.elf $(LINUX_SPIKE_BINS)
	$(MAKE) bundle BUNDLE_ARCHS=$(GUEST_ARCH) EXTRA_BUNDLE_FILES="$(LINUX_SPIKE_BINS)"
	@fail=0; \
	for p in $(SMOKE_PROGS); do \
	    out=`$(SPIKE_ENV) JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root \
	        $(if $(TIMEOUT),$(TIMEOUT) $(SPIKE_SECS)) \
	        ./$(TARGET) libos.elf bin $$p 2>&1`; \
	    if echo "$$out" | grep -q 'libos halting' && \
	       ! echo "$$out" | grep -Eq 'unhandled|EL1 fault|panic'; then \
	        echo "  ok    $$p"; \
	    else \
	        echo "  FAIL  $$p"; fail=1; \
	    fi; \
	done; \
	if [ $$fail -eq 0 ]; then echo "linux-smoke: all $(words $(SMOKE_PROGS)) passed"; \
	else echo "linux-smoke: FAILURES above"; exit 1; fi

## Run an arbitrary host-built static Linux binary in the bin guest, for build
## experiments: build or copy an ELF elsewhere, then
##   make linux-run BIN=path/to/prog ARGS="a b c"
## The binary must match this host's arch (static non-PIE, loads at 0x400000).
BIN ?=
ARGS ?=
linux-run: $(TARGET) libos.elf | build
	@test -n "$(BIN)" || { echo "set BIN=<path to a static Linux ELF>"; exit 1; }
	@test -f "$(BIN)" || { echo "no such file: $(BIN)"; exit 1; }
	@[ "$(BIN)" -ef "build/$(notdir $(BIN))" ] || cp "$(BIN)" "build/$(notdir $(BIN))"
	$(MAKE) bundle BUNDLE_ARCHS=$(GUEST_ARCH) EXTRA_BUNDLE_FILES="build/$(notdir $(BIN))"
	$(SPIKE_ENV) JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root \
	    ./$(TARGET) libos.elf bin $(notdir $(BIN)) $(ARGS)

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

## Host check of the guest format engine against literal expectations. The
## formatter is portable C, so it is built for the host with snprintf renamed to
## jsnprintf (so it sits beside the host libc) and checked byte for byte.
build/host_fmt.o: guest/libos/fmt.c guest/libos/fmt.h guest/libos/fmtfloat.h | build
	$(CC) -std=c11 -Wall -Wextra -Iguest/libos \
	    -Dsnprintf=jsnprintf -Dvsnprintf=jvsnprintf -c -o $@ $<
build/host_fmtfloat.o: guest/libos/fmtfloat.c guest/libos/fmtfloat.h | build
	$(CC) -std=c11 -Wall -Wextra -Iguest/libos -c -o $@ $<
build/fmttest: tools/fmttest.c build/host_fmt.o build/host_fmtfloat.o | build
	$(CC) -std=c11 -Wall -Wextra -o $@ tools/fmttest.c \
	    build/host_fmt.o build/host_fmtfloat.o
fmt-test: build/fmttest
	./build/fmttest

## Installation. PREFIX is the install root; it defaults to a user-local
## $HOME/.local so "make install" needs no privilege. Pass PREFIX=/opt (or
## PREFIX=/usr/local) for a system install, and DESTDIR to stage into a package
## root. The host binary goes in bin, the guest kernel in libexec/jotun (pass
## its path as the kernel-image argument when running), and the man page in
## share/man/man1.
PREFIX ?= $(HOME)/.local
DESTDIR ?=
bindir = $(PREFIX)/bin
libexecdir = $(PREFIX)/libexec/jotun
mandir = $(PREFIX)/share/man/man1
INSTALL = install

install: $(TARGET) $(GUEST) man/jotun.1
	$(INSTALL) -d $(DESTDIR)$(bindir) $(DESTDIR)$(libexecdir) $(DESTDIR)$(mandir)
	$(INSTALL) -m 755 $(TARGET) $(DESTDIR)$(bindir)/$(TARGET)
	$(INSTALL) -m 644 $(GUEST) $(DESTDIR)$(libexecdir)/$(GUEST)
	$(INSTALL) -m 644 man/jotun.1 $(DESTDIR)$(mandir)/jotun.1
	@echo "installed to $(DESTDIR)$(PREFIX)"

uninstall:
	rm -f $(DESTDIR)$(bindir)/$(TARGET)
	rm -f $(DESTDIR)$(libexecdir)/$(GUEST)
	rm -f $(DESTDIR)$(mandir)/jotun.1
	-rmdir $(DESTDIR)$(libexecdir) 2>/dev/null

clean:
	rm -f $(TARGET) libos.elf $(PROGS) castool jbundle
	rm -rf $(BUNDLE_DEPOT) image-depot bundle-staging build

.PHONY: all clean install uninstall bundle macsyntax fmt-test linux-progs linux-spike linux-smoke linux-tmp-mount linux-cmdline linux-run
