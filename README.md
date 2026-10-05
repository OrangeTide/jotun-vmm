# Jotun

A hypervisor-hosted unikernel sandbox. This repository currently holds the host
side: a small hypervisor abstraction over Linux KVM and macOS
Hypervisor.framework. It creates a VM, maps guest RAM, runs a vcpu, and loads a
guest from an ELF file, with a minimal UART console device. See
`doc/unikernel-design.md` for where this is going.

## Supported hosts

Native only, guest architecture equals host architecture:

- linux/x86-64 (KVM, x86 guest)
- linux/arm64 (KVM, arm64 guest)
- macos/arm64 (Hypervisor.framework, arm64 guest)

## Prerequisites

### linux/x86-64

- `make` and `gcc`
- 32-bit toolchain support to build the x86 guest (`-m32`):
  Debian/Ubuntu `gcc-multilib`, Fedora `glibc-devel.i686 libgcc.i686`
- access to `/dev/kvm` (see KVM permissions below)

### linux/arm64

- `make` and `gcc`
- access to `/dev/kvm`

### macos/arm64

- the Xcode command line tools (`xcode-select --install`) for `clang` and `make`
- `lld` to link the arm64 guest as ELF, since Apple's `ld` emits Mach-O:
  `brew install lld`
- an `entitlements.plist` granting `com.apple.security.hypervisor`; the build
  ad-hoc code-signs `jotun` with it (Hypervisor.framework refuses to run an
  unsigned, unentitled binary)

## Build and run

```sh
make
./jotun [guest.elf]    # defaults to test_guest.elf
```

`make` builds `jotun` and the matching `test_guest.elf` for the host. The x86
guest talks to a 16550 UART over port I/O; the arm64 guest uses an MMIO console.
Both print a line and stop.

```sh
make clean
```

### Environment variables

- `JOTUN_DEBUG` - if set, print one line per vcpu exit to stderr (PC, exit
  reason, and the port/address details). Off by default, with no cost on the
  normal path.
- `JOTUN_MAX_EXITS=N` - stop after N exits and report, as a guard against a
  guest that spins. Unset or `0` runs without a cap.

```sh
JOTUN_DEBUG=1 ./jotun
JOTUN_MAX_EXITS=1000 ./jotun
```

### KVM permissions (Linux)

Running needs read/write on `/dev/kvm`. Add yourself to the `kvm` group once:

```sh
sudo usermod -aG kvm "$USER"    # then log out and back in
```

## Layout

- `vmm.h` - the backend-neutral hypervisor interface
- `vmm_kvm.c`, `vmm_hvf.c` - the two backends
- `elf.c` - ELF32/ELF64 loader into guest RAM
- `uart.c` - 16550 (port I/O) and PL011-subset (MMIO) consoles
- `main.c` - load, run loop, device dispatch
- `test_guest_x86.c`, `test_guest_arm64.c` - the sample guests
- `doc/unikernel-design.md` - the design this is built toward

The `vmm.*` files are the internal hypervisor-abstraction module; the built
host binary is `jotun`.
