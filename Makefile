# Makefile
CFLAGS = -Wall -Wextra -std=c11
LDFLAGS =
ifeq ($(OS),)
    OS := $(shell uname -s)
endif

ifeq ($(OS), Darwin)
    CFLAGS += -D__APPLE__
    LDFLAGS += -framework Hypervisor
    SRC = vmm_hvf.c main.c
    CC = clang
else
    CFLAGS += -D__linux__
    SRC = vmm_kvm.c main.c
    CC = gcc
endif

TARGET = vmm_example

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)
ifeq ($(OS), Darwin)
	codesign --force --entitlements entitlements.plist --sign - $@
endif

clean:
	rm -f $(TARGET)

.PHONY: all clean


ifeq ($(OS), Darwin)
sign :
endif
