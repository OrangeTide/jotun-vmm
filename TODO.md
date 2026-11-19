# TODO

## Self-containment: no references to paths outside this project

Constraint: nothing in this project may refer to directories on the filesystem
outside the project root, and the build must not depend on any external
checkout. Upstream projects are named, never cited by a filesystem path.

- [x] Vendor the smolvfs dependency into the tree under `third_party/smolvfs/`
      and change the Makefile to build those in-tree sources. The external
      home-directory checkout dependency is removed. Vendored the CAS read path
      the host compiles (cas, cas-tree, cas-pack, cas-codec) plus the sources
      castool needs for the `bundle` target (cas-gc, cas-sign, cas-topic,
      castool, version.h), all built in-tree. The ref-signing crypto backend
      (monocypher) is still to be vendored when M8 lands.
- [x] Remove external home-directory path references from tracked files: the
      Makefile dependency variable, and the "Sources" citations in
      `doc/roadmap.md` and `doc/unikernel-design.md`. Upstream projects are now
      named without a filesystem path.
- [x] Rewrite the entire git history to remove these external path references
      from historical file contents (and one commit message), so none remain in
      any commit. Done with git filter-repo over a replacement map; every commit
      hash changed. A pre-rewrite backup was kept outside the repo. Verified: no
      external home-directory path token remains in any blob or message.

Self-containment is complete. Relates to M8: the pull client vendors the
remaining smolvfs sources (sign path, monocypher, and miniz for compressed
depots) in-tree the same way.

## M16 (prospective): Linux binary compatibility

Scoped 2026-10-07 as a research milestone. Not scheduled, and no work has
started. The question: can the libOS run unmodified static Linux binaries, so a
session guest is any `CGO_ENABLED=0` Go program or any statically linked musl
program rather than something compiled against our own `jlibc`? This turns the
libOS into a small binary-compatible kernel, the pattern Hermitux, OSv, and
Unikraft's syscall-shim already demonstrate. It is attractive here because the
guest already is a single-address-space kernel with the subsystems such a layer
sits on: an ELF loader, a scheduler with threads and a context-switch and
interrupt path, a 9P-backed VFS, a net stack, memory management, timers, and a
console. The missing piece is the Linux ABI veneer: let a foreign binary issue
real syscalls and service them from those subsystems.

Design decisions settled in discussion:

- Ring-0 collapse, not a user/kernel split. The binary runs in the same ring as
  the libOS, with `SYSCALL` trapped through `LSTAR` (x86-64) or `SVC` routed
  through the exception vector (arm64). The VM (KVM/HVF) is already the security
  boundary and the guest is one trust domain, so intra-guest memory protection
  is not needed for host safety. A ring-3 split stays possible later if we ever
  want to protect the libOS from a buggy app, at the cost of a user GDT/TSS,
  `SYSCALL`/`SYSRET` stacks, user page bits, and argument copy-in and copy-out.
- Static binaries only. A fully static binary needs no dynamic linker. The moment
  a program wants `dlopen` or NSS it needs `ld.so`, which is a far larger project
  and out of scope.
- Dual ABI. Keep the native `jlibc` program type and add a second "Linux static
  ELF" type, selected by the manifest or the ELF OS/ABI field. The two share one
  syscall-dispatch layer.
- The dispatch table forwards into existing subsystems: `read`/`write`/`openat`/
  `close`/`fstat`/`lseek` to the 9P VFS; `mmap`/`munmap`/`mprotect`/`madvise`/
  `brk` to `mem.c`; `socket`/`connect`/`bind`/`sendto`/`epoll_*` to the net stack
  and vswitch; `clone`/`futex`/`set_tid_address` to the scheduler and threads;
  `clock_gettime`/`nanosleep` to the timer; `arch_prctl(ARCH_SET_FS)` or
  `TPIDR_EL0` to TLS; `getrandom`, `exit_group`, `ioctl(TCGETS)`,
  `rt_sigaction`/`sigaltstack` as small or near-no-op stubs at first.

Non-goals: dynamic linking and `ld.so`; glibc static binaries (NSS, dlopen, TLS
quirks); CGO; `fork`+`exec` of external programs; full syscall coverage at
gVisor scale. The target is the common static core, not every Linux feature.

Milestones, smallest first, each with its own acceptance test:

- [x] M16.1 (spike): a Linux static-ELF loader and the syscall entry path. Load
      `PT_LOAD` segments and build the initial stack with `argc`/`argv`/`envp`
      and the auxv (`AT_PAGESZ`, `AT_RANDOM`). Trap `SYSCALL` into a dispatch
      table. Implement the startup syscalls a thin libc needs. Done: a
      `musl-gcc -no-pie -static` hello reaches `exit_group(0)` and prints to the
      console. See `doc/linux-spike.md`, `guest/libos/linuxelf.c`,
      `guest/libos/arch/x86_64_linuxsys.S`, and `make linux-spike`. The one host
      change needed was advertising the SYSCALL CPUID feature (`KVM_SET_CPUID2`
      in `host/vmm_kvm.c`), without which KVM refuses the `EFER.SCE` write. This
      is a spike, x86-64 only; before M16.2, add a `#GP`/`#PF` diagnostic
      handler (a bad guest instruction currently triple-faults silently).
- [~] M16.2: broaden the VFS and process syscalls. Done for musl: a
      file-descriptor table over the read-only 9P bundle with
      openat/open/close/read/lseek/stat family/fstat/getdents64, dup/dup2/dup3,
      in-memory pipe/pipe2, and the process/startup syscalls mprotect/
      clock_gettime/getrandom/prlimit64/getuid family/prctl; plus a
      CPU-exception diagnostic handler and a register-ABI fix in the SYSCALL
      trampoline. Committed musl tests cover it (example/linuxcat.c, linuxls.c,
      linuxpipe.c, linuxmem.c; see doc/linux-spike.md). The glibc static
      busybox (manual probe, not committed) now runs to exit_group and prints
      its full banner with zero unhandled syscalls. argv delivery is in (a
      glibc busybox applet runs: `bin busybox echo hello world` prints it), and
      a writable in-memory ramfs overlays the read-only bundle for files a
      program creates or writes (example/linuxargs.c, linuxwrite.c). Remaining
      for busybox applets proper: statx for `ls`, and clone/execve for shell
      pipelines (M16.4+). The committed acceptance is the musl tests.
- [ ] M16.3: static-PIE self-relocation. Apply the `R_*_RELATIVE` entries a
      static-PIE binary carries in its own `PT_DYNAMIC`, with no interpreter.
      Acceptance: a stock Alpine static binary (built static-PIE by default)
      loads and runs.
- [ ] M16.4: threads. Implement `clone` (address-space-sharing task), `futex`,
      and `set_tid_address`, mapping onto the existing scheduler. Acceptance: a
      multi-threaded musl pthreads program runs correctly (1:1 threads, simpler
      than an M:N runtime).
- [ ] M16.5: the Go runtime. Satisfy its sharper demands: `AT_RANDOM` is
      mandatory, large lazily committed `mmap` arenas, `futex` at scale, and TLS.
      Acceptance: a `CGO_ENABLED=0` Go hello world runs with
      `GODEBUG=asyncpreemptoff=1` (cooperative preemption only). A basic heap is
      already in (brk + eager anonymous mmap over a ~20 MiB bump arena, guest RAM
      at 32 MiB); the hard part left here is Go's large, lazily committed sparse
      reservations, which the eager bump arena cannot satisfy and which need real
      demand-paged mmap.
- [ ] M16.6: the network poller. Nonblocking sockets and an `epoll` emulation
      over the vswitch. Acceptance: a Go or musl TCP echo server is reachable
      through the net name service.
- [ ] M16.7 (stretch): asynchronous preemption. Deliver `SIGURG` and set up
      `sigaltstack` so the Go scheduler can preempt tight loops, dropping the
      `asyncpreemptoff` workaround. This is the first real signal-delivery work;
      everything before it avoids signals.

Open questions and risks: `mmap` must tolerate large sparse reservations with
lazy commit on fault, which Go's heap init depends on; `futex` and `clone`
semantics are the unforgiving long tail; two architectures mean two syscall ABIs
and two TLS mechanisms; `epoll` emulation over the vswitch is new; the monotonic
clock may want a vDSO, though Go and musl both fall back to the syscall. The
cheapest proof that the whole idea holds is M16.1 alone: a ring-0 `SYSCALL` trap,
about a dozen startup syscalls, and a static musl hello world exiting cleanly.

## Dynamic Linux + Node build: open gaps and bugs

The milestone drifted well past the static-only scope above: the guest now runs
unmodified dynamic glibc binaries, and the live goal (2026-11-08) was building a
complex project, a NodeJS v26.13.0 checkout at `REF/node`. That is DONE
(2026-11-11): the full Node.js builds end to end in-guest on the host toolchain,
`make` exits 0, and the 103 MB `out/Release/node` runs (reports v26.13.0 and
executes JavaScript). 3745 objects, from a V8 translation unit that needs
2.36 GiB through to node running itself to build its startup snapshot. Getting
from Stage C (127 objects) to a finished binary took: 9p deep-path Twalk chunking
(f8bdc46), `pwrite64` (681563f), raising RAM and the user window 2->3.5 GiB for
V8's `pipeline.cc` (2d55f82), `FUTEX_WAIT_BITSET` + `pkey` so V8's `mksnapshot`
runs in-guest (833b522), and `eventfd2` + `capget`/`clock_getres` so node's
`node_mksnapshot` runs in-guest (f2bad85). The manifest, build environment, the
gcc/python gotchas, and this whole push are written up in doc/dynamic-binaries.md
("Compiling Node (Stage C)" and "Pushing the Node build through V8"). The items
below are the gaps and bugs that surfaced getting there.

Fixed on the way to Stage B (for the record):

- [x] Overlay symlinks. `symlink`/`symlinkat` create an overlay entry (a
      ramfile flagged `is_link` whose data holds the target, stored verbatim),
      `readlink`/`readlinkat` return it, `lstat` reports `S_IFLNK`. Hard links
      stay `EPERM`. Unblocked Node's `make_bin_override` (`os.symlink`).
      Commit b82c21c.
- [x] `p9_node.handle` was 65 bytes, sized for the CAS backend's 64-hex hash,
      but the host-dir backend stores a path relative to the mount root there,
      so `hd_walk` refused any host-mount file whose relative path reached 64
      bytes. Deep trees (Node reaches 175) returned `ENOENT` on `stat`/`open`
      while `readdir` still listed them and sibling directories walked fine.
      Raised to `P9_HANDLE_MAX` (1024) and widened the matching buffers.
      Commit b618b91.

Open gaps and bugs, Stage C and general correctness:

- [x] Leaf-symlink following on `open`/`execve`/`stat`/`statx`. `follow_link_key`
      rewrites a resolved key when its final component is an overlay symlink
      (absolute target from the root, relative target from the link's parent,
      chained up to a depth limit). Wired into `do_open` (unless `O_NOFOLLOW`),
      `do_execve`, stat, and statx; `lstat`/statx-`NOFOLLOW` report `S_IFLNK`;
      readlink/symlink/unlink stay nofollow. Verified: `out/tools/bin/python`
      (the link configure points at the interpreter) now execs through to
      `Python 3.12.3`. Commit 84929ee.
- [x] Synthetic `/dev`. The guest now serves `/dev` entirely in-guest (no host
      round trip, so it works on a non-Linux host): `null`, `zero`, `full`
      (`ENOSPC` on write), `random`, `urandom`, `tty`, `console`, and
      `stdin`/`stdout`/`stderr`. A device opens to a character-device fd with
      read/write computed by kind, caught before the overlay so a redirect
      `>/dev/null` reaches the device instead of creating an overlay file.
      `stat` reports `S_IFCHR`; `/dev` lists its nodes and shows up in a root
      listing. Proof: example/linuxdev.c. Still missing: `/dev/fd/N` and
      `/dev/stdin`-as-symlink semantics (the std* nodes route to the console,
      not to the process's actual fd 0/1/2), `ptmx`/`pts`, and a real `rdev`.
- [x] Overlay large-file cap raised to 128 MiB (was 4 MiB), with the table at
      8192 entries (was 1024). Build outputs land on a writable host mount, not
      the overlay, so the cap now bounds only scratch and generated sources. See
      the "Scaling to a full build" section below.
- [x] `utimensat` (280/88) implemented, and `unlink`/`rmdir` route a writable
      host-mount path to a 9P `Tremove` instead of returning `EROFS`. gyp's
      compile rule writes an object, `touch`es its `.d.raw` dependency file, then
      removes it; both steps needed these. The same remove fix clears the ICU
      cleanup `unlinkat` that `--with-intl` hit. Commit 2619203.
- [x] `epoll` implemented over the poll readiness model (commit 8d5a7c9).
      `epoll_create1`/`epoll_create`/`epoll_ctl`/`epoll_wait`/`epoll_pwait` serve
      a refcounted per-epoll interest list that `epoll_wait` scans with the same
      `poll_ready` + cooperative rescan-and-yield loop as `poll`. Level-triggered
      (EPOLLET accepted as level), EPOLLONESHOT honored, EPOLLHUP on a closed
      watched fd; `struct epoll_event` packed on x86-64, aligned on aarch64. The
      Node configure no longer logs an unhandled syscall 291. New linuxepoll test;
      linux-smoke 27/27 on x86 and arm64 KVM. Still useful for the M16.6 poller.
- [ ] `/usr/bin/which` fails in the guest with "cannot execute: required file
      not found". Likely a `#!`-interpreter or missing-interp issue on that
      specific file. Minor (nothing depends on it yet), but it points at an
      `execve` or shebang-resolution corner worth pinning down.
- [x] Live-process / task ceiling under `make -j`. `LINUX_NPROC` is now 256
      (was 48) and `TASK_MAX` 320 (was 64), raised together so a parallel make
      no longer hits `fork: Cannot allocate memory` early. The procs/shareds
      tables are kmalloc'd from the growable heap (a slot's ~15 KiB lshared is
      the dominant cost); the `.bss` arrays that scale with these caps are small
      per entry, so the kernel image stays well under the 4 MiB VM_USER_LO floor.
      TASK_MAX stays above LINUX_NPROC (every Linux thread runs on a libOS task).
      linuxforkmax reaches its 200-live self-cap on all three configs (was ~62).
- [ ] `mount bin host:/bin` is a manual run-config requirement: Node's gyp
      shells out to `/bin/sh` (python `subprocess` with `shell=True`), and the
      guest has no `/bin` unless the manifest mounts it. Decide whether to
      provide a default `/bin` (or a `/bin/sh` the loader always resolves) so
      every shell-using program does not need the operator to add the mount.
### Scaling to a full build (the point of the sandbox)

Building and cross-compiling is the core purpose: a jotun sandbox is
OS-agnostic but runs native-arch Linux binaries (Linux/x86-64 on an x86-64
host, Linux/aarch64 on an aarch64 host), so the host's own toolchain compiles
inside it. A real target like V8 is the goal, not a thing to declare out of
scope. The guest is just undersized today; these are the expansions that make a
large build work. Roughly in dependency order:

- [x] Raise guest RAM into the multi-GiB range, and scale the frame pool and the
      user address-space window with it. Done: RAM and the window went 256 MiB ->
      2 GiB -> 3.5 GiB (commits 759f51b, 2d55f82). V8's heaviest `cc1plus`
      translation unit (`pipeline.cc`) needs a measured 2.36 GiB and now compiles;
      the full node binary (103 MB) links. 3.5 GiB is the largest contiguous RAM
      that keeps the device MMIO band below the x86 IOAPIC/APIC hole at
      0xfec00000 without splitting it; going past ~3.9 GiB needs that split (a
      later step). The per-process page tables now span four top-level slots on
      both arches; see the per-arch vmspace headers and doc/dynamic-binaries.md.
- [x] A WRITABLE host-backed mount. A build's output tree (`out/`, object files,
      archives, the linked binary) now lands on the host filesystem instead of
      guest RAM. This was the key architectural piece: it removes the in-RAM
      overlay as the ceiling for build artifacts (a V8 build writes gigabytes),
      keeps guest RAM for process memory, and persists the result on the host.
      `p9hostdir` takes a `writable` flag and serves write/create/truncate/
      remove (mkdir via create with `P9_DMDIR`), under the same realpath
      confinement it already enforced. A manifest marks a host mount writable
      with a trailing `rw` token (`mount out host:/path rw`); the host
      advertises each writable mount's guest prefix in the boot block, and the
      guest routes a create or write under that prefix to 9P instead of the
      in-RAM overlay (`under_rw_mount`/`is9w` in linuxelf.c). Commit 0362fbd.
      Verified end to end: a guest write to `/out/proof.txt` lands on the host
      directory with the right contents.
- [x] Scale the in-RAM overlay caps. `RAMFS_FILE_MAX` is now 128 MiB (was 4 MiB)
      and `RAMFS_MAX` 8192 entries (was 1024). The buffers are `kmalloc`'d on
      demand from the growable kernel heap, so the real ceiling is guest RAM, not
      these numbers; the table cap stays below 0x10000 so overlay inodes
      (0x10000+index) never collide with the pipe range (0x20000). The strategy
      is to lean on a writable host mount for anything big (build output trees,
      and `/tmp` via `mount tmp host:/tmp/<dir> rw`), keeping the overlay for
      scratch and generated sources. Validated 26/26 linux-smoke on all three.
- [ ] Raise or auto-scale `LINUX_NPROC` (48) and `TASK_MAX` (64) for `make -j`.
      Both are cheap `kmalloc`'d tables (commit cd04ef5); size them to the
      parallelism a build driver asks for rather than a fixed small cap.
- [ ] Cross-compilation note, not a blocker: producing aarch64 output on an
      x86-64 host is just running a cross-toolchain, which the sandbox already
      runs as ordinary native-arch binaries, so it needs no guest change. A
      native aarch64 guest on an aarch64 host covers the other direction (the
      arm64 backend exists but its Linux run path is still unexercised locally).
      What scales a build is the memory and I/O work above, not the target arch.

## Investigations (unscheduled, feasibility unknown)

Research items captured so they are not lost. Each needs a feasibility pass
before it becomes real work.

- Single-buffer / front-buffer rendering with dirty-line tracking in the shader.
  The pixel path double-buffers and the frame-efficiency work sends only damaged
  sub-rectangles (see the frame-efficiency note in `doc/roadmap.md`). The question
  is whether the rasterizer could render straight to the front buffer and track
  dirty scanlines itself, so only changed lines are re-encoded and sent, cutting
  the per-frame cost further. May not be feasible given tearing and the way the
  rasterizer overwrites whole regions; find out before committing to it.

- Guest-side font loading for rendered text. Today the web terminal renders text
  as browser-drawn SVG, so the guest loads no fonts. If the guest ever draws
  glyphs itself (the pixel path), it needs a font format to load. The obvious
  fits are bitmap fonts, which need no rasterizer: `.pcf` (X11) and `.psf` /
  `.psfu` (PC Screen Font, the `.psfu` variant carrying a Unicode table). An
  alternative is extracting embedded bitmap strikes in the style of OpenType
  `sbix`. Investigate which is simplest to parse and license-clean to ship.
