# Jotun

A hypervisor-hosted sandbox that runs unmodified native Linux binaries. A small
hypervisor abstraction over Linux KVM and macOS Hypervisor.framework hosts a
freestanding guest kernel (a libOS), which runs ordinary Linux programs in a
separate privilege level, from a `/bin/sh` to a full language runtime. The goal
is to build and cross-compile real projects inside the sandbox, using a host
toolchain mounted into the guest. See `doc/unikernel-design.md` for the design
and `doc/sandbox-build.md` for how to build and run it.

## Status

Working today:

- Hypervisor over KVM (linux/x86-64, linux/arm64) and Hypervisor.framework
  (macos/arm64), guest architecture equal to the host.
- A guest libOS that runs unmodified, dynamically linked glibc and static musl
  Linux binaries with real privilege separation (ring 3 / EL0) and preemption.
  Around 100 syscalls: a VFS, per-process paging, `fork`/threads/`futex`,
  signals, pipes, `epoll`/`eventfd`, `splice`, and in-guest AF_UNIX sockets.
- Host filesystem mounts over 9P (read-only and writable), so a guest process
  reads the host toolchain and writes build output to the host.
- Real builds in-guest from a host-mounted toolchain: Node.js v26.13.0 compiles
  and runs; Redis 7.4.1 builds (including jemalloc through its autotools
  `./configure`) and serves clients; autotools `./configure` works on
  host-mounted sources.
- Multiple guests per process, single-file packaging, multi-arch bundles, a
  Plan 9 style network between guests, and a signed bundle pull.

Not done, by design or deferred: AF_INET and datagram sockets, snapshot and
restore, and anything that would make the guest a general-purpose VM rather than
a build-and-run sandbox.

## Supported hosts

Native only, guest architecture equals host architecture:

- linux/x86-64 (KVM, x86 guest)
- linux/arm64 (KVM, arm64 guest)
- macos/arm64 (Hypervisor.framework, arm64 guest)

To build the sandbox and run unmodified Linux binaries inside it, including the
per-architecture toolchain setup, see `doc/sandbox-build.md`.

## Prerequisites

### linux/x86-64

- `make` and `gcc`
- access to `/dev/kvm` (see KVM permissions below)
- `musl-gcc` to build the Linux binaries that run inside the sandbox (Debian and
  Ubuntu package it as `musl-tools`). Only needed for the sandbox targets below.

### linux/arm64

- `make` and `gcc`
- access to `/dev/kvm`
- `aarch64-linux-musl-gcc` (a static musl arm64 toolchain) to build the sandbox
  binaries. Only needed for the sandbox targets below.

### macos/arm64

- the Xcode command line tools (`xcode-select --install`) for `clang` and `make`
- `lld` to link the arm64 guest as ELF, since Apple's `ld` emits Mach-O:
  `brew install lld`
- an `entitlements.plist` granting `com.apple.security.hypervisor`; the build
  ad-hoc code-signs `jotun` with it (Hypervisor.framework refuses to run an
  unsigned, unentitled binary)
- macOS has no musl cross-compiler, so the sandbox binaries must be built on a
  Linux arm64 host and copied over. By convention they live in an untracked
  `a64bins/` directory, passed as `SPIKE_BIN_DIR=a64bins`.

## Quick start

Install the prerequisites for your platform (above), then build the hypervisor
and run the native test guest, which prints a line and stops:

```sh
make
./jotun
```

To run unmodified Linux binaries inside the sandbox, build the guest kernel and a
program bundle and run the test suite:

```sh
make linux-smoke                          # linux/x86-64 and linux/arm64
make linux-smoke SPIKE_BIN_DIR=a64bins    # macos/arm64 (uses pre-built arm64 binaries)
```

On Linux this needs `/dev/kvm` access (see KVM permissions below); macOS needs no
`sudo`. That is the only step that differs between platforms. The full
per-platform guide, including running your own binaries, is in
`doc/sandbox-build.md`.

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

- `host/` - the hypervisor and host services. `vmm_kvm.c` and `vmm_hvf.c` are the
  two backends, `elf.c` loads a guest, `uart.c` is the console, `p9*.c` serve the
  9P filesystem backends, and `main.c` is the load-and-run loop. Builds to
  `jotun`.
- `guest/libos/` - the freestanding guest kernel: the Linux-compat layer
  (`linuxelf.c`), per-process paging, the scheduler, and the 9P client. Builds to
  `libos.elf`.
- `guest/` - the sample native guests (`test_guest_x86.c`, `test_guest_arm64.c`)
  that `make` builds and `./jotun` runs.
- `abi/` - headers shared by host and guest (the boot block, the 9P, network, and
  draw channels).
- `example/` - the `linux*.c` test programs that run inside the sandbox.
- `doc/` - `unikernel-design.md` (the design), `sandbox-build.md` (per-platform
  build and run), and the `m16-*.md` notes on the Linux-compat work.
