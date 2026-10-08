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
        host/p9smolvfs.c host/p9overlay.c host/p9graft.c host/manifest.c host/jimage.c host/net.c host/draw.c host/drawws.c host/png.c host/http.c host/bundlepull.c host/main.c $(SMOLVFS_SRC)
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
        host/p9smolvfs.c host/p9overlay.c host/p9graft.c host/manifest.c host/jimage.c host/net.c host/draw.c host/drawws.c host/png.c host/http.c host/bundlepull.c host/main.c $(SMOLVFS_SRC)
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

# Freestanding guests share one link line; only the source differs.
GUEST_LINK = $(GUEST_CC) $(GUEST_CFLAGS) -Wl,-Ttext=$(GUEST_TEXT) -o $@ $<

test_guest.elf: $(GUEST_SRC)
	$(GUEST_LINK)

echo.elf: guest/echo.c
	$(GUEST_LINK)

# The library OS is multiple translation units: the portable core plus the
# selected arch file. It needs its own include path and links all objects.
LIBOS_SRC_COMMON = guest/libos/main.c guest/libos/mem.c guest/libos/task.c \
    guest/libos/boot.c guest/libos/p9.c guest/libos/elfload.c \
    guest/libos/prog.c guest/libos/jsh.c guest/libos/ns.c guest/libos/net.c \
    guest/libos/draw.c guest/libos/string.c guest/libos/fmt.c \
    guest/libos/print.c guest/libos/webterm.c guest/libos/threads.c
LIBOS_SRC = $(LIBOS_SRC_COMMON) $(LIBOS_ARCH)

# triton.c is the only guest translation unit built with the FPU enabled: it is
# the software rasterizer and uses floating point. The rest of the guest stays
# integer-only (-mgeneral-regs-only). Compile it to an object on its own with
# that flag dropped, then link it with the integer-only sources. The guest
# enables the FPU at boot and preserves FP state across task switches, so this
# object's floating point is safe to run.
GUEST_FP_CFLAGS = $(filter-out -mgeneral-regs-only -Wl%,$(GUEST_CFLAGS))
LIBOS_FP_OBJ = build/triton.o build/fmtfloat.o

build:
	mkdir -p build

build/triton.o: guest/libos/triton.c guest/libos/triton.h | build
	$(GUEST_CC) $(GUEST_FP_CFLAGS) -Iguest/libos -c -o $@ $<

# fmtfloat.c is the format engine's floating-point unit, built with the FPU on
# for the same reason as triton.o and linked into the integer-only formatter.
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
    build/libc_io.o build/libc_stdlib.o
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

# Standalone programs, linked at the program base and loaded from the bundle,
# with the program C library and dead-code elimination of its unused parts.
hello.elf: guest/prog/hello.c $(PROG_LIBC_OBJS)
	$(GUEST_CC) $(GUEST_CFLAGS) -Wl,--gc-sections $(PROG_INC) \
	    -Wl,-Ttext=$(PROG_TEXT) -o $@ guest/prog/hello.c $(PROG_LIBC_OBJS)

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

CROSSCC_x86_64  ?= clang --target=x86_64-unknown-none-elf -fuse-ld=lld
CROSSCC_aarch64 ?= clang --target=aarch64-unknown-none-elf -fuse-ld=lld
TEXT_x86_64  := 0x100000
TEXT_aarch64 := 0x200000
AFLAGS_x86_64  :=
AFLAGS_aarch64 := -mno-outline-atomics
ASRC_x86_64  := guest/libos/arch/x86_64.c guest/libos/arch/x86_64_ctx.S \
    guest/libos/arch/x86_64_isr.S guest/libos/arch/x86_64_boot.S
ASRC_aarch64 := guest/libos/arch/arm64.c guest/libos/arch/arm64_ctx.S \
    guest/libos/arch/arm64_boot.S

# Generate the cross-build rules for one arch: the kernel (libos) and the hello
# program, under build/img/<arch>/. Used only for arches other than this host's.
define IMG_ARCH_template
build/img/$(1):
	mkdir -p build/img/$(1)
build/img/$(1)/triton.o: guest/libos/triton.c guest/libos/triton.h | build/img/$(1)
	$(2) $$(GUEST_BASE_FP) $(4) -Iguest/libos -c -o $$@ $$<
build/img/$(1)/fmtfloat.o: guest/libos/fmtfloat.c guest/libos/fmtfloat.h | build/img/$(1)
	$(2) $$(GUEST_BASE_FP) $(4) -Iguest/libos -c -o $$@ $$<
build/img/$(1)/kernel: $$(LIBOS_SRC_COMMON) $(5) build/img/$(1)/triton.o build/img/$(1)/fmtfloat.o | build/img/$(1)
	$(2) $$(GUEST_BASE) $(4) -Iguest/libos -Wl,-Ttext=$(3) -o $$@ \
	    $$(LIBOS_SRC_COMMON) $(5) build/img/$(1)/triton.o build/img/$(1)/fmtfloat.o
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
build/img/$(1)/hello: guest/prog/hello.c build/img/$(1)/libc_fmt.o \
    build/img/$(1)/libc_fmtfloat.o build/img/$(1)/libc_string.o \
    build/img/$(1)/libc_io.o build/img/$(1)/libc_stdlib.o | build/img/$(1)
	$(2) $$(GUEST_BASE) $(4) -Wl,--gc-sections $$(PROG_INC) \
	    -Wl,-Ttext=$$(PROG_TEXT) -o $$@ guest/prog/hello.c \
	    build/img/$(1)/libc_fmt.o build/img/$(1)/libc_fmtfloat.o \
	    build/img/$(1)/libc_string.o \
	    build/img/$(1)/libc_io.o build/img/$(1)/libc_stdlib.o
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

## Run the web terminal guest with the draw server open on port 9230. Front it
## with Caddy for TLS (see example/web/Caddyfile and doc/web-hosting.md) and open
## https://localhost:8443/. Needs a bundle first: make bundle BUNDLE_ARCHS=$(ARCH)
web: $(TARGET) libos.elf
	JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root JOTUN_DRAW_PORT=9230 \
	    ./$(TARGET) libos.elf webterm

## Smoke-test the web path end to end. The test binary spawns the webterm guest
## with the draw server, drives it over a WebSocket, and checks the shell
## round-trip. Needs a bundle: make bundle BUNDLE_ARCHS=$(ARCH)
build/websmoke: tools/websmoke.c | build
	$(CC) $(CFLAGS) -o $@ $<
web-smoke: $(TARGET) libos.elf build/websmoke
	./build/websmoke ./$(TARGET) $(BUNDLE_DEPOT)

## brokerd: the process-per-session web hosting supervisor (M15.2 host
## mechanism). It forks one jotun child per browser session and proxies to it.
brokerd: host/brokerd.c
	$(CC) $(CFLAGS) -o $@ $<

## Multi-user web hosting: run the supervisor, front it with Caddy for TLS the
## same way as make web. Needs a bundle: make bundle BUNDLE_ARCHS=$(ARCH)
web-multi: brokerd $(TARGET) libos.elf
	JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root ./brokerd

## authd: the bearer-token validator Caddy delegates to (forward_auth), and
## jmint: the tool that mints a token into the store authd reads (M15.3).
authd: host/authd.c
	$(CC) $(CFLAGS) -o $@ $<
jmint: host/jmint.c
	$(CC) $(CFLAGS) -o $@ $<

## Authenticated multi-user hosting: brokerd + authd behind Caddy, with a shared
## secret so brokerd trusts only the identity authd vouches for. Mint tokens with
## ./jmint <user> [cap]; front with example/web/Caddyfile-auth. Needs a bundle.
web-auth: brokerd authd $(TARGET) libos.elf
	@test -n "$(JOTUN_BROKER_AUTH_SECRET)" || \
	    { echo "set JOTUN_BROKER_AUTH_SECRET (shared with authd)"; exit 1; }
	JOTUN_BROKER_AUTH_SECRET=$(JOTUN_BROKER_AUTH_SECRET) \
	    JOTUN_AUTH_TOKENS=$(JOTUN_AUTH_TOKENS) ./authd & \
	JOTUN_BROKER_AUTH_SECRET=$(JOTUN_BROKER_AUTH_SECRET) \
	    JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root ./brokerd; kill %1 2>/dev/null

## One-command authenticated demo: brokerd + authd + Caddy together, so the whole
## chain is a single foreground command for local testing. Generates an ephemeral
## shared secret if none is set, mints a `demo` token into build/demo.tokens on
## first run and prints the login URL, keeps Caddy's CA under build/, and tears
## all three down on exit (Ctrl-C). Needs caddy on PATH and a bundle.
web-auth-demo: brokerd authd jmint $(TARGET) libos.elf
	@mkdir -p build/caddy; \
	secret=$${JOTUN_BROKER_AUTH_SECRET:-$$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')}; \
	store=build/demo.tokens; \
	if [ ! -s $$store ]; then \
	    tok=$$(JOTUN_AUTH_TOKENS=$$store ./jmint demo 5); \
	    echo "minted demo token: $$tok"; \
	else tok=$$(awk 'NR==1{print $$1}' $$store); fi; \
	echo "open in a browser: https://localhost:8443/login?token=$$tok"; \
	echo "(accept the local CA once; Ctrl-C stops authd, brokerd, and Caddy)"; \
	JOTUN_AUTH_TOKENS=$$store JOTUN_BROKER_AUTH_SECRET=$$secret ./authd & a=$$!; \
	JOTUN_BROKER_AUTH_SECRET=$$secret JOTUN_BUNDLE=$(BUNDLE_DEPOT) JOTUN_REF=root \
	    ./brokerd & b=$$!; \
	trap 'kill $$a $$b 2>/dev/null' INT TERM EXIT; \
	XDG_DATA_HOME=build/caddy XDG_CONFIG_HOME=build/caddy \
	    caddy run --config example/web/Caddyfile-auth

## Multi-session smoke test: brokerd opens two independent sessions and checks
## they get separate guests and tear down. Needs a bundle.
build/brokersmoke: tools/brokersmoke.c | build
	$(CC) $(CFLAGS) -o $@ $<
broker-smoke: brokerd $(TARGET) libos.elf build/brokersmoke
	./build/brokersmoke ./brokerd ./$(TARGET) $(BUNDLE_DEPOT)

## TLS front-end smoke test: run brokerd behind Caddy (the make web-multi recipe)
## and reach it over TLS with openssl s_client, checking two sessions get routed
## through the proxy. Needs caddy on PATH and a bundle: make bundle BUNDLE_ARCHS=$(ARCH)
build/caddysmoke: tools/caddysmoke.c | build
	$(CC) $(CFLAGS) -o $@ $<
caddy-smoke: brokerd $(TARGET) libos.elf build/caddysmoke
	./build/caddysmoke ./$(TARGET) $(BUNDLE_DEPOT) example/web/Caddyfile

## Admission + auth smoke test: mints two users, starts authd and brokerd with a
## shared secret, checks authd's token validation and brokerd's per-user session
## caps and identity gate. Needs a bundle: make bundle BUNDLE_ARCHS=$(ARCH)
build/authsmoke: tools/authsmoke.c | build
	$(CC) $(CFLAGS) -o $@ $<
auth-smoke: brokerd authd jmint $(TARGET) libos.elf build/authsmoke
	./build/authsmoke ./brokerd ./authd ./jmint ./$(TARGET) $(BUNDLE_DEPOT)

## End-to-end auth check through Caddy: runs brokerd + authd + Caddy with
## example/web/Caddyfile-auth and drives the real forward_auth chain with curl
## (no token 401, bearer 302, /login sets the cookie, cookie 302, bad token 401),
## so it guards the Caddyfile wiring that auth-smoke only simulates. Needs caddy
## and curl on the PATH and a bundle.
build/cauthsmoke: tools/cauthsmoke.c | build
	$(CC) $(CFLAGS) -o $@ $<
caddy-auth-smoke: brokerd authd jmint $(TARGET) libos.elf build/cauthsmoke
	./build/cauthsmoke ./brokerd ./authd ./jmint ./$(TARGET) $(BUNDLE_DEPOT) \
	    example/web/Caddyfile-auth

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

## Broker control-channel ABI check (M15.2 step two, defined ahead of wiring).
## Drives the host-side mock (host/brokerctl.c) over the shared struct the way a
## broker guest would: spawn/bind/destroy and lifecycle-event draining, with the
## quota and error paths. A pure host unit test, no VM.
build/brokerctl.o: host/brokerctl.c host/brokerctl.h abi/jotun_broker.h | build
	$(CC) $(CFLAGS) -Ihost -c -o $@ $<
build/brokerctltest: tools/brokerctltest.c build/brokerctl.o host/brokerctl.h abi/jotun_broker.h | build
	$(CC) $(CFLAGS) -Ihost -o $@ tools/brokerctltest.c build/brokerctl.o
brokerctl-test: build/brokerctltest
	./build/brokerctltest

clean:
	rm -f $(TARGET) test_guest.elf echo.elf libos.elf $(PROGS) castool jbundle \
	    brokerd authd jmint
	rm -rf $(BUNDLE_DEPOT) image-depot bundle-staging build

.PHONY: all clean bundle macsyntax web web-smoke web-multi broker-smoke caddy-smoke fmt-test web-auth web-auth-demo auth-smoke caddy-auth-smoke brokerctl-test
