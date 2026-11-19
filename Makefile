# Makefile
# The version is kept in version.mk (a single tracked line) and stamped into the
# binary through the generated build/version.h. Bump version.mk to change it.
include version.mk

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

CFLAGS = -Wall -Wextra -std=c11 -pthread -Iabi -Ibuild -I$(SMOLVFS_DIR) \
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

all: $(TARGET) $(GUEST)

$(TARGET): $(SRC) build/version.h
	$(CC) $(CFLAGS) -o $@ $(SRC) $(LDFLAGS)
ifeq ($(OS), Darwin)
	codesign --force --entitlements entitlements.plist --sign - $@
endif

# Stamp the version into a generated header, so only a version change triggers a
# rebuild. FORCE reruns the recipe every build to track version.mk; the cmp
# guard leaves the header's mtime alone when the string is unchanged. The header
# is build output: it lives under build/ (gitignored) and is never committed.
build/version.h: version.mk FORCE | build
	@printf '#ifndef JOTUN_VERSION\n#define JOTUN_VERSION "%s"\n#endif\n' \
	    '$(JOTUN_VERSION)' > $@.tmp
	@cmp -s $@.tmp $@ || mv $@.tmp $@
	@rm -f $@.tmp
FORCE:

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

# Multi-arch image build. The native arch reuses the tested libos.elf; every
# other arch is cross-built with clang + lld to a freestanding ELF. The per-arch
# guest flags are self-contained here, independent of the native single-arch
# GUEST_CFLAGS above. BUNDLE_ARCHS selects which arches go into the image; the
# default is all supported arches, so a plain `make bundle` produces a
# multi-arch image (which needs clang + lld for the non-native arch; set
# BUNDLE_ARCHS to this host's arch to build a single-arch image with no cross
# toolchain).
BUNDLE_ARCHS ?= x86_64 aarch64

GUEST_BASE = -ffreestanding -fno-pic -fno-pie -nostdlib -fno-builtin -Iabi \
    -fno-asynchronous-unwind-tables -mgeneral-regs-only \
    -Wl,-e_start -Wl,--build-id=none
GUEST_BASE_FP = $(filter-out -mgeneral-regs-only,$(GUEST_BASE))

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

# Generate the cross-build rule for one arch: the kernel (libos) under
# build/img/<arch>/. Used only for arches other than this host's.
define IMG_ARCH_template
build/img/$(1):
	mkdir -p build/img/$(1)
build/img/$(1)/fmtfloat.o: guest/libos/fmtfloat.c guest/libos/fmtfloat.h | build/img/$(1)
	$(2) $$(GUEST_BASE_FP) $(4) -Iguest/libos -c -o $$@ $$<
build/img/$(1)/kernel: $$(LIBOS_SRC_COMMON) $(5) build/img/$(1)/fmtfloat.o | build/img/$(1)
	$(2) $$(GUEST_BASE) $(4) -Iguest/libos -Wl,-Ttext=$(3) -o $$@ \
	    $$(LIBOS_SRC_COMMON) $(5) build/img/$(1)/fmtfloat.o
endef

NONNATIVE_ARCHS := $(filter-out $(GUEST_ARCH),x86_64 aarch64)
$(foreach a,$(NONNATIVE_ARCHS),$(eval $(call IMG_ARCH_template,$(a),$(CROSSCC_$(a)),$(TEXT_$(a)),$(AFLAGS_$(a)),$(ASRC_$(a)))))

# The kernel for one arch: the native arch reuses the top-level build, every
# other arch comes from its cross-build directory.
img_kernel = $(if $(filter $(1),$(GUEST_ARCH)),libos.elf,build/img/$(1)/kernel)
BUNDLE_DEPS = $(foreach a,$(BUNDLE_ARCHS),$(call img_kernel,$(a)))

# Assemble a per-arch staging tree (the shared root files plus boot/kernel) and
# build one multi-arch image from it with jbundle. Guest programs are Linux
# binaries added through EXTRA_BUNDLE_FILES (see linux-example and linux-smoke).
bundle: castool jbundle $(BUNDLE_DEPS)
	rm -rf $(BUNDLE_DEPOT) bundle-staging
	@args=""; \
	for a in $(BUNDLE_ARCHS); do \
	    d=bundle-staging/$$a; \
	    mkdir -p $$d/boot; \
	    cp example/bundle/* $$d/; \
	    [ -z "$(EXTRA_BUNDLE_FILES)" ] || cp $(EXTRA_BUNDLE_FILES) $$d/; \
	    if [ "$$a" = "$(GUEST_ARCH)" ]; then \
	        k=libos.elf; \
	    else \
	        k=build/img/$$a/kernel; \
	    fi; \
	    cp $$k $$d/boot/kernel; \
	    args="$$args --arch $$a=$$d"; \
	    echo "bundle: staged $$a from $$k"; \
	done; \
	./jbundle -d $(BUNDLE_DEPOT) --ref root --name jotun-demo $$args
	./castool -d $(BUNDLE_DEPOT) pack

## --- Linux binary-compat test programs (example/linux*.c) ------------------
## These run unmodified static Linux binaries in the `bin` guest, exercising the
## VFS, heap, fork/exec, signals, threads, mmap, and preemption. They run
## natively, so on x86-64 (KVM) and on macos/arm64 (HVF). See
## guest/libos/linuxelf.c and doc/linux-compat-bringup.md.
##
## Build with a static musl toolchain, picked by arch and overridable with
## EXAMPLE_CC (x86-64: musl-gcc; aarch64: aarch64-linux-musl-gcc). A host with no
## musl cross-compiler can instead point EXAMPLE_BIN_DIR at a directory of
## pre-built binaries (static non-PIE ET_EXEC at 0x400000, matching this arch).
## linuxtmp is excluded from the wildcard suite: it needs a writable host mount
## supplied by a manifest ("mount tmp host:<dir> rw"), which the bin-guest smoke
## harness does not provide. It has its own target, linux-tmp-mount, below.
LINUX_EXAMPLE_SRCS := $(filter-out example/linuxtmp.c,$(wildcard example/linux*.c))
LINUX_EXAMPLE_BINS := $(patsubst example/%.c,build/%,$(LINUX_EXAMPLE_SRCS))
LINUX_EXAMPLE_PROGS := $(patsubst build/%,%,$(LINUX_EXAMPLE_BINS))
## linuxchurn is a long-running stress reproducer, not a unit test: the smoke
## suite neither builds, bundles, nor runs it (SMOKE_BINS drops it), so a
## prebuilt EXAMPLE_BIN_DIR need not carry it. Run it explicitly with linux-example,
## which bundles the full LINUX_EXAMPLE_BINS set. SMOKE_PROGS is the run loop; it
## runs each program with no arguments.
SMOKE_PROGS := $(filter-out linuxchurn,$(LINUX_EXAMPLE_PROGS))
SMOKE_BINS  := $(patsubst %,build/%,$(SMOKE_PROGS))
EXAMPLE_CC_x86_64  ?= musl-gcc
EXAMPLE_CC_aarch64 ?= aarch64-linux-musl-gcc
EXAMPLE_CC ?= $(EXAMPLE_CC_$(ARCH))
EXAMPLE_BIN_DIR ?=

## The guest preempts Linux user loops by default now, so the preemptible-loop
## test makes progress and the rest tolerate the timer. EXAMPLE_SECS caps a run
## through a timeout command when one is on PATH, so a regression that hangs
## still fails the suite. The cap holds for the slowest supported host: linuxbigmem
## demand-pages 2.5 GiB and takes low-20s seconds on a Raspberry Pi 4, so 20 was
## too tight there; 45 leaves margin while still catching a real hang.
EXAMPLE_ENV  ?= JOTUN_HZ=100
EXAMPLE_SECS ?= 45
TIMEOUT    := $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null)

## Build one test program. A pattern rule, so editing one source rebuilds only
## that binary (the old rule rebuilt all of them on every run). With
## EXAMPLE_BIN_DIR set, copy a pre-built binary instead of compiling.
build/linux%: example/linux%.c | build
	@if [ -n "$(EXAMPLE_BIN_DIR)" ]; then \
	    cp "$(EXAMPLE_BIN_DIR)/$(@F)" "$@"; \
	else \
	    command -v $(EXAMPLE_CC) >/dev/null 2>&1 || \
	        { echo "need a static musl compiler on PATH (EXAMPLE_CC=, default $(EXAMPLE_CC)), or set EXAMPLE_BIN_DIR="; exit 1; }; \
	    $(EXAMPLE_CC) -static -no-pie -O2 -o "$@" "$<"; \
	fi

## Build every test program.
linux-progs: $(LINUX_EXAMPLE_BINS)

## Populate a directory of prebuilt example binaries for a toolchain-less host.
## macos has no static musl AArch64 cross-compiler, so it cannot compile the
## build/linux* programs; it copies them from EXAMPLE_BIN_DIR instead. Run this on
## a Linux host that has EXAMPLE_CC, then copy PREBUILT_DIR to the target host and
## pass it back as EXAMPLE_BIN_DIR:
##   make linux-prebuilts PREBUILT_DIR=a64bins EXAMPLE_CC=aarch64-linux-musl-gcc
##   # copy a64bins/ to the mac, then: make linux-smoke EXAMPLE_BIN_DIR=a64bins
## It builds every example/linux*.c (including linuxtmp and linuxchurn) so
## linux-example, linux-tmp-mount, and linux-cmdline work from the prebuilts too.
PREBUILT_DIR ?= prebuilts
linux-prebuilts:
	@command -v $(EXAMPLE_CC) >/dev/null 2>&1 || \
	    { echo "need a static musl compiler on PATH (EXAMPLE_CC=, default $(EXAMPLE_CC))"; exit 1; }
	@mkdir -p "$(PREBUILT_DIR)"
	@n=0; for c in $(wildcard example/linux*.c); do \
	    p=`basename "$$c" .c`; \
	    $(EXAMPLE_CC) -static -no-pie -O2 -o "$(PREBUILT_DIR)/$$p" "$$c" || exit 1; \
	    n=`expr $$n + 1`; \
	done; \
	echo "linux-prebuilts: $$n binaries in $(PREBUILT_DIR)"

## Run one test program in the bin guest (default linuxhello):
##   make linux-example EXAMPLE_PROG=linuxcat
## All programs are bundled, because some (linuxexec) run their peers.
EXAMPLE_PROG ?= linuxhello
linux-example: $(TARGET) libos.elf $(LINUX_EXAMPLE_BINS)
	$(MAKE) bundle BUNDLE_ARCHS=$(GUEST_ARCH) EXTRA_BUNDLE_FILES="$(LINUX_EXAMPLE_BINS)"
	$(EXAMPLE_ENV) JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root \
	    ./$(TARGET) libos.elf bin $(EXAMPLE_PROG)

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
	out=`$(EXAMPLE_ENV) $(if $(TIMEOUT),$(TIMEOUT) $(EXAMPLE_SECS)) \
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
	out=`$(EXAMPLE_ENV) $(if $(TIMEOUT),$(TIMEOUT) $(EXAMPLE_SECS)) \
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
linux-smoke: $(TARGET) libos.elf $(SMOKE_BINS)
	$(MAKE) bundle BUNDLE_ARCHS=$(GUEST_ARCH) EXTRA_BUNDLE_FILES="$(SMOKE_BINS)"
	@fail=0; \
	for p in $(SMOKE_PROGS); do \
	    out=`$(EXAMPLE_ENV) JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root \
	        $(if $(TIMEOUT),$(TIMEOUT) $(EXAMPLE_SECS)) \
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
	$(EXAMPLE_ENV) JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root \
	    ./$(TARGET) libos.elf bin $(notdir $(BIN)) $(ARGS)

## Demo: build a Linux kernel from source inside the guest. This is a
## demonstration of the in-guest toolchain over a real project, not part of the
## normal build or the regression suite. It serves a distro userland as the
## guest root, runs example/kbuild-demo.sh as pid 1 (configure + build a minimal
## kernel for this guest's arch), and writes the image to the writable KSRC
## mount. See doc/kernel-build.md for the prerequisites and the full runbook.
##
##   make linux-kernel-demo KSRC=<writable linux source tree> [ROOTFS=<dir>]
##
## KSRC is a writable, unmodified Linux source tree; the build output lands in
## it. ROOTFS is a Linux distro rootfs for this guest's arch carrying the
## toolchain (gcc binutils make flex bison bc). On Linux it defaults to the
## host's own "/"; on macOS it must be a Linux rootfs, because the Mac's own "/"
## is Mach-O rather than Linux ELF. The timer is left off (JOTUN_HZ unset), which
## macOS needs for the fork/exec-heavy build and which is harmless on Linux.
KSRC ?=
ifeq ($(OS),Darwin)
ROOTFS ?=
else
ROOTFS ?= /
endif
linux-kernel-demo: $(TARGET) libos.elf | build
	@test -n "$(KSRC)" || \
	    { echo "set KSRC=<writable Linux source tree> (see doc/kernel-build.md)"; exit 1; }
	@test -d "$(KSRC)" || { echo "no such directory: $(KSRC)"; exit 1; }
	@test -n "$(ROOTFS)" || \
	    { echo "macOS: set ROOTFS=<Linux $(GUEST_ARCH) distro rootfs with a toolchain> (see doc/kernel-build.md)"; exit 1; }
	cp example/kbuild-demo.sh "$(KSRC)/kbuild-demo.sh"
	chmod +x "$(KSRC)/kbuild-demo.sh"
	@printf 'rootfs host:%s\nmount ksrc host:%s rw\ninit bin /bin/sh /ksrc/kbuild-demo.sh\n' \
	    "$(ROOTFS)" "$(KSRC)" > build/kernel-demo.manifest
	@echo "=== linux-kernel-demo (demo): building a $(GUEST_ARCH) kernel from $(KSRC) ==="
	@echo "    output lands in $(KSRC); this is a long build (see doc/kernel-build.md)"
	JOTUN_HZ= ./$(TARGET) -m build/kernel-demo.manifest libos.elf

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
	rm -f $(TARGET) libos.elf castool jbundle
	rm -rf $(BUNDLE_DEPOT) image-depot bundle-staging build

.PHONY: all clean install uninstall bundle macsyntax fmt-test linux-progs linux-prebuilts linux-example linux-smoke linux-tmp-mount linux-cmdline linux-run linux-kernel-demo
