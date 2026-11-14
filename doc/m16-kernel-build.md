# Plan: build the Linux kernel from source inside the sandbox

This is a plan, not a record of work done. It describes how to bring a full
Linux kernel build up inside the guest, what already covers the job, the gaps
to expect, and the order to attack them.

## Goal

Build an unmodified Linux kernel from source inside the guest, using a distro
toolchain mounted from the host. The headline target is the hardest and most
useful case:

> An aarch64 guest on macOS (Hypervisor.framework) builds an aarch64 kernel
> natively.

Native means target arch equals guest arch, so there is one toolchain and no
cross-compile. That is the common case an operator wants (compile for the
machine you are on) and it keeps the first milestone focused on the build
itself rather than on cross-toolchain plumbing.

Secondary targets, for the usual three-arch matrix: x86-64 guest on Linux/KVM
building an x86-64 kernel, and an aarch64 guest on Linux/KVM (the Pi4) building
an aarch64 kernel.

## Why this is the right next use case

The kernel is the most demanding "real project" build the sandbox can attempt,
and it stresses parts of the toolchain path that Node, Redis, and the autotools
projects did not:

- Kbuild is recursive make plus a large amount of shell, run over tens of
  thousands of files.
- The build compiles its own host tools (fixdep, modpost, genksyms, sorttable,
  dtc, the kconfig frontends) with the host compiler, then executes those
  freshly built ELF binaries from the output tree partway through the build.
- It exercises the full binutils set (as, ld, ar, nm, objcopy, strip) and one
  very large final link (vmlinux).

Everything underneath it is already proven: see `doc/m16-dynamic.md` for make,
bash, gcc compile-and-link, fork/exec/posix_spawn, exec from the writable
overlay and from a host mount, dynamic glibc and musl, autotools on a host
mount, and `doc/sandbox-build.md` for writable host mounts and running a distro
rootfs. Guest RAM and the per-process window are now 3.5 GiB on both arches,
well above any single kernel translation unit (kernel C peaks far below V8's
2.36 GiB cc1plus, which already built in-guest).

## What a kernel build needs

Toolchain and host tools, all supplied by the distro mount:

- gcc, binutils (as, ld, ar, nm, objcopy, strip), make
- bash, coreutils, sed, gawk, grep, find, xargs
- flex and bison (kconfig frontends, and the in-tree dtc)
- bc (a few generated headers), perl (some scripts)
- dev libraries that only some configs need: libelf (objtool, modpost BTF),
  openssl (module signing, certificate extraction). Both are avoidable by
  turning the matching config options off, see below.

Filesystem:

- The output tree is multiple GB. It must live on a writable host mount
  (`mount out host:<dir> rw`), exactly as the Node build did. The guest overlay
  is capped and in-RAM, so it is not a place for a kernel build tree.
- The kernel source can sit on a read-only mount, or inside the distro rootfs.

Scheduling and memory:

- `make -j` auto-detection reads `sched_getaffinity`, which the guest
  implements, but the guest presents few CPUs and the libOS caps processes
  (NPROC/TASK). Pass an explicit `-j` sized to that budget rather than letting
  make guess high.
- No preemption on macOS: run the build with `JOTUN_HZ` unset. The HVF
  preemption bug corrupts a process when an async interrupt lands during
  fork/exec/reap (see the memory note and `doc/sandbox-build.md`), and a kernel
  build is almost entirely fork/exec. A batch build with the timer off takes no
  async interrupts and avoids the bug, the same way the Node and autotools
  builds were run. On Linux/KVM the timer is safe.

## Config: start minimal, widen later

A full `defconfig` pulls in the most dependency-heavy parts of the build. For
the first milestone, start from `defconfig` and turn those off with
`scripts/config`, then `make olddefconfig`:

- `CONFIG_MODULES=n` for the very first pass: skips the whole modpost and
  modules path, so only `vmlinux` plus the arch image are built.
- `CONFIG_MODULE_SIG=n`, `CONFIG_MODULE_SIG_ALL=n`: drops openssl and
  extract-cert.
- `CONFIG_DEBUG_INFO_BTF=n`: drops pahole/dwarves.
- `CONFIG_DEBUG_INFO=n` (or reduced): smaller, faster, less link memory.
- `CONFIG_GDB_SCRIPTS=n`.
- Build only the compressed image first (`make Image` on arm64, `make bzImage`
  on x86), not dtbs, so the in-tree dtc (flex/bison) is not on the first path.

This narrows the first attempt to: kconfig, the core host tools, the C
compiler and assembler over the tree, and the final link. Each later milestone
re-enables one dependency and closes its gap.

## Gaps to expect

Syscall coverage is close. Present and relevant: clone, vfork, execve, wait4,
futex, pipe2, dup3, statx, getdents64, renameat2, symlinkat, readlinkat,
fchmodat, utimensat, ppoll, sched_getaffinity, sysinfo, prlimit64, getrandom,
madvise, mprotect, mremap. The build is expected to need work in these areas:

- `flock` (util-linux, and the flock syscall) is not implemented. Some Kbuild
  steps and tools take an advisory lock. Probe for it; it may need a real
  `flock` syscall or an fcntl `F_SETLK` path. Watch for a tool blocking or
  failing on a `.lock` file.
- `select`/`pselect6` are not implemented (only `ppoll`/`poll`). Most of the
  toolchain uses poll, but a stray tool may use select. Probe and add if hit.
- `objtool` needs libelf headers. Avoided by config at first; when re-enabled,
  provide `libelf`/`elfutils` from the distro mount.
- The in-tree `dtc` (flex/bison) is avoided by building only the image at
  first; re-enable with dtbs.
- Wall clock will be long under the cooperative single-vCPU libOS with a capped
  `-j`. Expect a multi-hour build. The writable host mount keeps partial output
  across a run so progress is not lost if a run is stopped.

## Toolchain sourcing

Two routes, both already supported:

1. Distro rootfs (recommended). `rootfs host:<aarch64 rootfs>` serves the
   guest root from a distro tree so `/usr`, `/lib`, and the toolchain resolve
   at `/`, then add `mount src host:<kernel source> rw` (or ro) and
   `mount out host:<build dir> rw`. A Debian aarch64 rootfs is the lowest
   friction: glibc and bash are already proven, and `gcc binutils make flex
   bison bc perl libelf-dev` install cleanly. Alpine works too (musl is proven)
   but needs `apk add build-base bash flex bison bc perl elfutils-dev
   openssl-dev linux-headers` and an explicit bash, since its default shell is
   busybox ash.

2. Piecemeal mounts over a bundle root, grafting the distro `/usr` and `/lib`
   plus the source and output trees. This is how the first gcc and Node builds
   were done. It works but the rootfs route resolves distro-absolute symlinks
   more cleanly (see the chroot-symlink note in `doc/sandbox-build.md`).

## Staged milestones

Each step is independently provable and gates the next.

- S0 config. Mount the rootfs, source, and output. Run
  `make O=<out> ARCH=arm64 defconfig` and confirm a `.config` is written. This
  alone proves the kconfig host tools build with HOSTCC and run.
- S1 host tools. `make O=<out> ARCH=arm64 prepare` builds `scripts/` (fixdep
  and friends). Proves the host-tool round trip: compile an ELF to the output
  mount, then execute it from there mid-build.
- S2 image (headline). With the minimal config (MODULES=n, no BTF/sig/debug),
  `make O=<out> ARCH=arm64 -j<N> Image`. Compiles the tree, links vmlinux, and
  produces `arch/arm64/boot/Image`. Confirm with `file` on the host that the
  image is an aarch64 kernel. This is the headline success.
- S3 modules and dtbs. Re-enable `CONFIG_MODULES` (modpost, genksyms) and build
  `Image modules dtbs`, closing the libelf and dtc (flex/bison) gaps.
- S4 matrix. Repeat S2 on x86-64/KVM (`ARCH=x86_64`, `bzImage`) and on the Pi4
  aarch64/KVM. The Mac run keeps `JOTUN_HZ` unset; the KVM runs may use the
  timer.

## Verification and caveats

- Success is the arch image existing on the host output mount with the right
  arch per `file`, and `make` exiting 0.
- Keep the build on a writable host mount so the artifact and partial progress
  persist across runs.
- macOS: `JOTUN_HZ` unset for the whole build (HVF preemption bug).
- Size the host output mount for several GB.
- `-j` must be explicit and matched to the libOS process budget, not left to
  make's auto-detection.

## Status

S0 (defconfig) is done on x86-64/KVM and aarch64/KVM. `make defconfig` for a
Linux 6.12 source tree runs end to end in the guest: fixdep, flex, bison, and
conf all build with the host toolchain and run, and conf writes a real `.config`
to the writable host mount (Linux/x86_64 and Linux/arm64 headers, the expected
symbol counts). The setup is the host root served read-only as the guest root
(`rootfs host:/`) plus the kernel tree on a writable mount, with a shell wrapper
that sets PATH and runs make. On aarch64 the host also needs `pahole` (the
`dwarves` package); without it `init/Kconfig`'s `PAHOLE_VERSION` line fails to
parse. This is the "complete distro toolchain" point above, not a guest bug.

Bringing S0 up fixed four guest/host gaps (commit, "fixes to run the kernel
build's config stage in-guest"): short reads on host-mount files, the initial
stack packed flush against the unmapped ceiling, `dup()` of a std fd refused,
and `rename()` on a writable host mount (a new 9P rename op). linux-smoke stays
36/36 on x86-64/KVM and aarch64/KVM.

macOS/HVF note: `rootfs host:/` cannot be used there, because the Mac's
binaries are Mach-O, not Linux ELF. The headline aarch64-on-macOS run uses a
Linux aarch64 distro rootfs (Debian bookworm, built with debootstrap: gcc,
binutils, make, flex, bison, bc, dwarves/pahole, libelf-dev, libssl-dev) served
with `rootfs host:<debroot>`, the kernel tree on a writable `mount ksrc
host:<tree> rw`, and a shell wrapper that sets PATH and runs make.

S0 headline (aarch64 guest on macOS/HVF, 16 KiB granule) is done:
`make ARCH=arm64 defconfig` ran end to end, exit 0, a real 300 KB / 11164-line
`.config`. Bringing it up root-caused and fixed a deterministic fault that was
not a build gap: the per-process kernel stack (`LINUX_KSTACK`) was 16 KiB and
the `execve` chain overflowed it into the adjacent process kstack, so a victim
resumed on a clobbered return address. Fixed by raising `LINUX_KSTACK` to
64 KiB (a separate bug from the nondeterministic HVF async-IRQ preempt
corruption). linux-smoke stays 36/36 on all three configs.

S1 host tools is done on the headline (Mac). `make ARCH=arm64 prepare` (after
`olddefconfig`) ran end to end, exit 0: `fixdep` and `modpost` compiled with
HOSTCC and ran from the output mount, `include/generated/autoconf.h` and
`asm-offsets.h` (132 lines) were generated, and the arm64 VDSO was compiled,
linked, and `objcopy`'d in-guest. This proves the host-tool round trip (compile
an ELF to the mount, execute it mid-build) plus real arm64 codegen and binutils
`ld`/`objcopy`. The early `status=1`/`status=127` child exits are Kbuild's own
`cc-option`/`try-run` probes, expected to fail. No `flock` or `select` gap was
hit at this stage. Next: S2 (`make Image`) with the minimal config.

S2 image (headline) is done on the Mac (aarch64 guest on macOS/HVF, 16 KiB
granule). `make ARCH=arm64 -j4 Image` ran to exit 0 and produced a real aarch64
Linux 6.12 kernel from source in-guest: `arch/arm64/boot/Image` (about 78 MiB,
with the valid `ARM\x64` boot magic) and `vmlinux` (about 89 MiB, an aarch64
ELF). The whole back end ran in-guest: `NM`, `KSYMS`, the kallsyms passes,
`LD vmlinux`, `System.map`, `SORTTAB`, and `OBJCOPY Image`. The minimal config
is `defconfig` with `MODULES=n`, no `DEBUG_INFO`/DWARF/BTF, `GDB_SCRIPTS` off,
and `DRM_MSM` off. `DRM_MSM` is disabled because it builds its register headers
with `$(PYTHON3)`, and the Debian rootfs has no Python; this is a toolchain
gap, handled by not building that one driver, not a guest bug.

Bringing S2 up root-caused one real guest bug. The final `vmlinux` link failed
with undefined nouveau symbols, because `execve` silently truncated `argv` at
`LINUX_MAX_ARGS` (256). A directory's `built-in.a` recipe runs a single `ar`
with every member object as its own argument, and nouveau has 724, so `ar`
bundled only the first ~252 and the rest were dropped from the archive. The
objects themselves compiled fine (compile command lines stay under the cap).
The fix raises `LINUX_MAX_ARGS` to 2048 and moves `build_stack_vec`'s two
`argv`/`envp` pointer arrays off the 64 KiB kernel stack (the caller now passes
that scratch in), so the larger cap cannot overflow the kstack. linux-smoke is
37/37 on x86-64/KVM and aarch64/KVM, and 36/37 on the Mac with the timer off
(only `linuxpreempt`, which structurally needs the timer). A diagnosis note:
the macOS `ar` and `file` cannot read GNU thin archives (`!<thin>`) and
misreport a valid archive as corrupt, so thin-archive membership is checked
with `strings <archive> | grep <member>.o`, not `ar t`.

## Open risks

- `flock` and `select` gaps may each need a small syscall addition.
- The final vmlinux link is the largest single memory consumer; 3.5 GiB should
  cover a non-LTO, reduced-debug build, but an LTO or full-debug config could
  exceed it and is out of scope for the first attempt.
- Wall clock under the cooperative scheduler is the main practical limit, not
  correctness.
