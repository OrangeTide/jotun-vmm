# Setting up a sandbox build per architecture

This describes how to prepare a build environment, build the sandbox, and run
unmodified Linux binaries inside it on each supported host. The guest
architecture always equals the host architecture (native only):

- linux/x86-64 (KVM, x86-64 guest)
- linux/arm64 (KVM, arm64 guest)
- macos/arm64 (Hypervisor.framework, arm64 guest)

The pieces a full sandbox run uses are:

- `jotun`, the host hypervisor binary, built from `host/`.
- `libos.elf`, the freestanding guest kernel (the libOS) that runs the Linux
  binaries, built from `guest/libos/`.
- a bundle (a content-addressed store under `bundle-depot/`) that holds the
  guest programs. The `bin` guest reads its programs from the bundle over 9P.

`make` builds `jotun` and the native test guest. The sandbox targets below build
`libos.elf` and a bundle as needed.

## Toolchains

Two separate toolchains are involved and they are easy to confuse.

1. The build toolchain compiles the host and the guest kernel. The host is built
   with the native compiler (`gcc` on Linux, Apple `clang` on macOS). The guest
   kernel is freestanding. On Linux it is built with the native `gcc`; on macOS
   it is built with `clang` targeting ELF, which needs `lld` because Apple's
   linker only emits Mach-O. Cross-built arches in a multi-arch bundle always use
   `clang` + `lld`.

2. The sandbox-program toolchain builds the Linux binaries that run inside the
   guest. These must be static, non-PIE `ET_EXEC` that load at `0x400000`. The
   test programs under `example/linux*.c` are built with a static musl
   compiler, selected per arch (see each section). A host without a musl
   cross-compiler can instead point `SPIKE_BIN_DIR` at a directory of pre-built
   binaries for this arch.

### lld resolution

The Makefile resolves `lld` for the clang ELF links automatically. It prefers an
absolute `ld.lld` found on `PATH`, then the Homebrew keg locations
(`/opt/homebrew/opt/lld/bin/ld.lld`, `/usr/local/opt/lld/bin/ld.lld`), and falls
back to the bare `-fuse-ld=lld` name. An absolute path is accepted by both Apple
and LLVM clang, so the same setting works everywhere. Override it with
`FUSE_LD=` on the command line if needed.

## linux/x86-64

### Prerequisites

- `make` and `gcc`.
- `musl-gcc` (static musl) to build the Linux test binaries. Debian and Ubuntu
  package it as `musl-tools`.
- `lld` only if you build a multi-arch bundle that cross-compiles the arm64
  guest. The native x86-64 build does not need it. Debian and Ubuntu package it
  as `lld`.
- read/write access to `/dev/kvm` (see KVM permissions below).

### Build and run

```sh
make                           # jotun + native test guest
make linux-smoke               # build libos.elf, bundle the tests, run all of them
make linux-spike SPIKE_PROG=linuxcat   # run one test program
```

Run an arbitrary static Linux x86-64 binary:

```sh
make linux-run BIN=/path/to/prog ARGS="a b c"
```

## linux/arm64

### Prerequisites

- `make` and `gcc`.
- `aarch64-linux-musl-gcc` (static musl for arm64) to build the Linux test
  binaries. Install a musl arm64 cross-toolchain, or override `SPIKE_CC` with the
  name your toolchain uses.
- `lld` only if you build a multi-arch bundle that cross-compiles the x86-64
  guest. The native arm64 build does not need it.
- read/write access to `/dev/kvm`.

### Build and run

Same targets as x86-64:

```sh
make
make linux-smoke
make linux-spike SPIKE_PROG=linuxcat
make linux-run BIN=/path/to/prog ARGS="a b c"
```

If the checkout was seeded from another arch, run `make clean` first so stale
native build artifacts (the bundle tools) are rebuilt for this host.

## macos/arm64

### Prerequisites

- the Xcode command line tools for `clang` and `make`:

  ```sh
  xcode-select --install
  ```

- `lld`, because Apple's `ld` emits Mach-O and the guest kernel must be ELF:

  ```sh
  brew install lld
  ```

  Homebrew installs `lld` keg-only, so its `ld.lld` is not on `PATH`. The
  Makefile finds it at the Homebrew keg location automatically, so no `PATH`
  change or manual linker flag is needed.

- the Linux test binaries. macOS has no musl cross-compiler by default, so build
  the `example/linux*.c` programs on a Linux arm64 box (static, non-PIE, loading
  at `0x400000`) and copy them over, then point `SPIKE_BIN_DIR` at that
  directory. By convention this project keeps them in an untracked `a64bins/`
  directory.

- no manual code-signing. Hypervisor.framework refuses an unsigned, unentitled
  binary, so the build ad-hoc signs `jotun` with `entitlements.plist`, which
  grants `com.apple.security.hypervisor`. This happens automatically in the
  `jotun` recipe.

### Build and run

```sh
make                                       # jotun (auto-signed) + guest
make linux-smoke SPIKE_BIN_DIR=a64bins     # run the suite from pre-built binaries
make linux-spike SPIKE_PROG=linuxcat SPIKE_BIN_DIR=a64bins
```

HVF does not need `sudo`.

## Running the sandbox directly

The make targets wrap a direct invocation. After `libos.elf` and a bundle exist,
you can run a bundled program yourself:

```sh
JOTUN_HZ=100 JOTUN_BUNDLE=bundle-depot JOTUN_REF=root \
    ./jotun libos.elf bin <program> [args...]
```

- With `JOTUN_HZ=100` the guest preempts running Linux user loops at 100 Hz by
  default. Set `JOTUN_NO_PREEMPT` to force a cooperative-only run for debugging.
- `JOTUN_BUNDLE` points at the content-addressed store; `JOTUN_REF` names the
  root to serve (the targets use `root`).
- `bin` is the guest that runs Linux binaries; `<program>` is a program name in
  the bundle.

Build the bundle alone, for one arch, with:

```sh
make bundle BUNDLE_ARCHS=<arch> EXTRA_BUNDLE_FILES="build/prog1 build/prog2"
```

`BUNDLE_ARCHS` defaults to all supported arches, which needs clang + lld for the
non-native arch. Set it to this host's arch to build a single-arch bundle with no
cross toolchain.

## Installing

`make install` copies the host binary, the guest kernel, and the man page into a
prefix:

```sh
make install                 # into $HOME/.local (no privilege needed)
make install PREFIX=/opt     # system-wide (may need sudo)
```

- `PREFIX` defaults to `$HOME/.local`; pass `PREFIX=/opt` or `PREFIX=/usr/local`
  for a system install, and `DESTDIR` to stage into a package root.
- `jotun` lands in `$PREFIX/bin`, the `libos.elf` kernel in
  `$PREFIX/libexec/jotun`, and `jotun.1` in `$PREFIX/share/man/man1`. An
  installed `jotun` finds that kernel automatically (it looks next to the binary
  and in `../libexec/jotun`), so you run it with just a manifest and no
  kernel-image argument. `JOTUN_KERNEL` overrides the search.
- `make uninstall` removes the same files. See `man jotun` (or
  `man ./man/jotun.1` before installing) for the full command and manifest
  reference.

## Running a distro rootfs

Instead of a bundle of individually built programs, the guest root can be served
straight from an extracted distro rootfs on the host, so its own `/bin`, `/lib`
and `/usr` are the guest's. The manifest directive is `rootfs host:<path>`, and
the kernel comes from the command-line `libos.elf`. This runs the distro's
userland binaries under the libOS; it does not boot the distro's kernel or init.

A minimal Alpine rootfs, for an x86-64 host (use the `aarch64` download on an arm
host):

```sh
ver=3.21.10
curl -fsSLO https://dl-cdn.alpinelinux.org/alpine/v3.21/releases/x86_64/alpine-minirootfs-$ver-x86_64.tar.gz
mkdir -p alpine/rootfs && tar -xzf alpine-minirootfs-$ver-x86_64.tar.gz -C alpine/rootfs
```

A manifest that serves it and runs a command, then exits (for an interactive
shell instead, see the next section):

```
rootfs host:/abs/path/to/alpine/rootfs
init bin /bin/busybox sh -c "uname -a; echo hi | grep hi; ls /bin | head"
```

```sh
JOTUN_HZ=100 ./jotun -m alpine.manifest libos.elf
```

busybox's applets are all absolute symlinks to `/bin/busybox`. They resolve
because the host mount treats the rootfs root as `/` (a chroot-style resolver),
not the host's real root. The same works for a Debian `debootstrap` tree or the
filesystem of an exported container (`docker export` or `podman export` of a
started container, extracted into a directory).

The rootfs is read-only. A guest write lands in the in-RAM overlay, so `/tmp` is
writable within the run but is not persisted. To write build output back to the
host, add a normal writable mount beside the rootfs:

```
rootfs host:/abs/path/to/rootfs
mount out host:/abs/path/to/outdir rw
init bin /bin/busybox sh /out/build.sh
```

## An interactive shell

Add `console interactive` to the manifest to wire the guest console to your
terminal, so the init program gets a real tty: keystrokes reach the guest and
the shell shows a prompt and edits lines.

```
rootfs host:/abs/path/to/alpine/rootfs
console interactive
init bin /bin/busybox sh
```

```sh
./jotun -m alpine.manifest libos.elf
```

Type `exit` (or run `poweroff`) to leave the shell, which stops the sandbox.

What happens under the hood: the host puts your terminal in raw mode and feeds
every keystroke into the guest's console UART; the guest runs a small tty line
discipline (canonical line editing with echo, or raw mode when a program such as
a shell's line editor asks for it) and answers `TCGETS`/`TCSETS` so programs see
a tty. The guest also receives a default environment (`PATH`, `HOME`, `PS1`, and
`TERM` copied from your terminal).

Limitations in this first version:

- There is no terminal translation. Bytes pass through untouched and the guest's
  `TERM` is copied from the host, so a shell and curses programs behave on an
  xterm-family terminal (most Linux terminals, Terminal.app, iTerm). A terminal
  that is not xterm-compatible may mis-render some escape sequences.
- Ctrl-C stops the sandbox rather than interrupting the guest command. Terminal
  signals are not yet forwarded to the guest.
- There is no job control (`busybox` prints "can't access tty; job control
  turned off"). Pipelines and background jobs still work; `fg`/`bg` do not.
- `console interactive` applies to one guest. With several guests, the first
  interactive one owns the keyboard; the rest share the output log only.
- On macOS (Hypervisor.framework), a shell that spawns external commands can
  fault intermittently. This is a pre-existing bug, not specific to the
  interactive console: an asynchronous interrupt taken while a process forks,
  execs, and reaps corrupts a process on Apple Silicon. A plain `busybox sh`
  running a few external commands faults about half the time with `JOTUN_HZ` set
  (the preemption timer is the interrupt) and never with `JOTUN_NO_PREEMPT=1`.
  The interactive console hits the same bug through a different interrupt, the
  keystroke wake-up, which `JOTUN_NO_PREEMPT` does not gate, so an interactive
  shell that runs external commands is currently unreliable on macOS regardless.
  Shell builtins alone are fine. Non-interactive builds are fine as long as the
  preemption timer is off (leave `JOTUN_HZ` unset). Linux/KVM (both x86-64 and
  arm64) is solid in every case. This needs a fix in the arm64 interrupt
  save/restore path and is tracked separately.

## Kernel command line

The init program runs as pid 1. To pass it boot parameters the usual Linux way,
add a `cmdline` directive; the guest serves its value as `/proc/cmdline`.

```
rootfs host:/abs/path/to/rootfs
cmdline root=/dev/vda ro quiet mykey=value
init bin /sbin/init
```

The whole rest of the `cmdline` line is kept verbatim, including spaces, so a
multi-word command line needs no quoting. Inside the guest:

```sh
cat /proc/cmdline
# root=/dev/vda ro quiet mykey=value
```

The file is newline-terminated, as Linux writes it. When no `cmdline` directive
is given, `/proc/cmdline` is present but empty. Any process whose parent exits is
reparented to pid 1, matching Linux, so an orphan's `getppid()` reads 1.

## KVM permissions (Linux)

Running needs read and write on `/dev/kvm`. Add yourself to the `kvm` group once:

```sh
sudo usermod -aG kvm "$USER"    # then log out and back in
```

Where that is not possible, run `jotun` under `sudo` instead. The sandbox targets
run `jotun` directly, so prefix the whole command, for example
`sudo make linux-smoke`, or run the direct invocation above under `sudo`.

## Overridable knobs

- `SPIKE_CC` selects the musl compiler (default `musl-gcc` on x86-64,
  `aarch64-linux-musl-gcc` on arm64).
- `SPIKE_BIN_DIR` copies pre-built test binaries instead of compiling them.
- `SPIKE_PROG` picks the program for `make linux-spike` (default `linuxhello`).
- `SPIKE_ENV` sets the run environment (default `JOTUN_HZ=100`; preemption is on
  by default, disable with `JOTUN_NO_PREEMPT`).
- `SPIKE_SECS` caps each `linux-smoke` run when a `timeout` command is on PATH.
- `BUNDLE_ARCHS` selects which arches go into a bundle.
- `FUSE_LD` overrides the resolved linker flag for the clang ELF links.

See `doc/linux-spike.md` for what the test programs exercise and
`guest/libos/linuxelf.c` for the Linux ABI the `bin` guest implements.
