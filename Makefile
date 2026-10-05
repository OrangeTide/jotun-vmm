# Makefile
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS =
ifeq ($(OS),)
    OS := $(shell uname -s)
endif
ARCH := $(shell uname -m)

# Common freestanding guest flags; per-host blocks add the compiler, the
# source, and the text base.
GUEST_CFLAGS = -ffreestanding -fno-pic -fno-pie -nostdlib \
    -fno-asynchronous-unwind-tables -Wl,-e_start -Wl,--build-id=none

GUEST =

ifeq ($(OS), Darwin)
    # macos/arm64: Hypervisor.framework, arm64 guest.
    CFLAGS += -D__APPLE__
    LDFLAGS += -framework Hypervisor
    SRC = vmm_hvf.c elf.c uart.c main.c
    CC = clang
    GUEST = test_guest.elf
    GUEST_SRC = test_guest_arm64.c
    # Apple's toolchain emits Mach-O; produce an ELF with an LLVM ELF target
    # and lld (install with e.g. `brew install lld`).
    GUEST_CC = clang --target=aarch64-unknown-none-elf -fuse-ld=lld
    GUEST_TEXT = 0x80000
else
    CFLAGS += -D__linux__
    SRC = vmm_kvm.c elf.c uart.c main.c
    CC = gcc
    GUEST_CFLAGS += -no-pie
    ifeq ($(ARCH), x86_64)
        # linux/x86-64: KVM, 32-bit protected-mode x86 guest at 1 MiB.
        GUEST = test_guest.elf
        GUEST_SRC = test_guest_x86.c
        GUEST_CC = $(CC) -m32
        GUEST_TEXT = 0x100000
    endif
    ifeq ($(ARCH), aarch64)
        # linux/arm64: KVM, arm64 guest.
        GUEST = test_guest.elf
        GUEST_SRC = test_guest_arm64.c
        GUEST_CC = $(CC)
        GUEST_TEXT = 0x80000
    endif
endif

TARGET = vmm_example

all: $(TARGET) $(GUEST)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
ifeq ($(OS), Darwin)
	codesign --force --entitlements entitlements.plist --sign - $@
endif

test_guest.elf: $(GUEST_SRC)
	$(GUEST_CC) $(GUEST_CFLAGS) -Wl,-Ttext=$(GUEST_TEXT) -o $@ $<

clean:
	rm -f $(TARGET) test_guest.elf

.PHONY: all clean
