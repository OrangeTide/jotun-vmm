# Makefile
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS =
ifeq ($(OS),)
    OS := $(shell uname -s)
endif
ARCH := $(shell uname -m)

GUEST =

ifeq ($(OS), Darwin)
    CFLAGS += -D__APPLE__
    LDFLAGS += -framework Hypervisor
    SRC = vmm_hvf.c elf.c uart.c main.c
    CC = clang
else
    CFLAGS += -D__linux__
    SRC = vmm_kvm.c elf.c uart.c main.c
    CC = gcc
    # The sample guest is an x86 image, so build it only on an x86-64 host.
    ifeq ($(ARCH), x86_64)
        GUEST = test_guest.elf
    endif
endif

TARGET = vmm_example

all: $(TARGET) $(GUEST)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
ifeq ($(OS), Darwin)
	codesign --force --entitlements entitlements.plist --sign - $@
endif

# Freestanding 32-bit guest: no libc, no PIC, text based at 1 MiB, entry _start.
# Drop the build-id note and unwind tables so no stray segment lands outside
# the guest's RAM window.
test_guest.elf: test_guest.c
	$(CC) -m32 -ffreestanding -fno-pic -fno-pie -no-pie -nostdlib \
	    -fno-asynchronous-unwind-tables \
	    -Wl,-e_start -Wl,-Ttext=0x100000 -Wl,--build-id=none -o $@ $<

clean:
	rm -f $(TARGET) test_guest.elf

.PHONY: all clean
