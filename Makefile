# Makefile
CFLAGS = -Wall -Wextra -std=c11 -pthread -Iabi
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
    SRC = host/vmm_hvf.c host/elf.c host/uart.c host/main.c
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
    SRC = host/vmm_kvm.c host/elf.c host/uart.c host/main.c
    CC = gcc
    GUEST_CFLAGS += -no-pie
    ifeq ($(ARCH), x86_64)
        # linux/x86-64: KVM, 64-bit long-mode x86 guest at 1 MiB.
        GUEST = test_guest.elf echo.elf libos.elf
        GUEST_SRC = guest/test_guest_x86.c
        LIBOS_ARCH = guest/libos/arch/x86_64.c guest/libos/arch/x86_64_ctx.S
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

all: $(TARGET) $(GUEST)

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
    guest/libos/boot.c $(LIBOS_ARCH)

libos.elf: $(LIBOS_SRC)
	$(GUEST_CC) $(GUEST_CFLAGS) -Iguest/libos -Wl,-Ttext=$(GUEST_TEXT) -o $@ $^

clean:
	rm -f $(TARGET) test_guest.elf echo.elf libos.elf

.PHONY: all clean
