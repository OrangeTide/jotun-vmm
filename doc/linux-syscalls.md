# Linux syscall support (M16 binary-compat guest)

This is the inventory of Linux x86-64 syscalls for the `bin` guest, which runs
unmodified static Linux x86-64 binaries under the libOS SYSCALL trap
(guest/libos/linuxelf.c). It records what is implemented today, how each handled
call behaves, and a prioritized plan for the rest.

The goal is not 100 percent Linux coverage. It is to raise the ceiling of
programs that run, in phases, so each release handles a wider set of real static
musl and glibc binaries (shells, busybox applets, build tools) without chasing
the obscure tail.

## Dispatch model

`linux_sys_dispatch` is a switch on the call number. Anything not in the switch
falls through to a default that logs `[bin] unhandled syscall N` and returns
`-ENOSYS`. Many programs tolerate `ENOSYS` with a fallback path (for example
busybox `cat` tries `sendfile` then falls back to a read/write loop), so an
unimplemented call is not always fatal. The log line is the signal that a
program wanted something we do not provide.

Statuses used below:

- **full**: real behavior backed by the VFS, memory arena, or process model.
- **partial**: works for the common case but ignores some arguments or modes.
- **stub**: accepted and ignored (returns 0 or a fixed value) with no real
  effect; present so startup and common idioms do not fail.

## Implemented (114 calls)

### File and directory I/O

| nr | name | status | notes |
|----|------|--------|-------|
| 0 | read | full | console EOF, pipe (blocking), ramfs, 9P |
| 1 | write | full | console, pipe (blocking), ramfs; 9P bundle is read-only |
| 2 | open | full | 9P bundle (RO) and ramfs overlay for writes/creates; O_CREAT/O_TRUNC/O_APPEND/O_EXCL honored (O_EXCL over an existing overlay name is EEXIST, so mkstemp retries) |
| 3 | close | full | releases pipe and 9P references, refcounted |
| 4 | stat | full | path via ramfs or 9P |
| 5 | fstat | full | pipes as FIFO, console, ramfs, 9P; stable per-file inode (9P qid) so the dynamic linker dedups libraries correctly |
| 6 | lstat | full | like stat, but reports an overlay symlink as `S_IFLNK` instead of following it |
| 8 | lseek | full | file and ramfs cursors |
| 16 | ioctl | stub | returns ENOTTY so libc falls back |
| 17 | pread64 | full | read at an explicit offset, cursor unchanged; ramfs and 9P |
| 20 | writev | full | loops over do_write |
| 21 | access | full | existence check via meta_path; the dynamic linker probes ld.so.cache and library dirs this way |
| 22 | pipe | full | in-memory byte ring |
| 32 | dup | full | refcounted share of the backing object |
| 33 | dup2 | full | any target; the old target (even a std fd holding a pipe) is closed before the rebind |
| 72 | fcntl | partial | validates the fd first (a closed fd is EBADF, which a shell relies on to probe a redirect target); F_DUPFD/F_DUPFD_CLOEXEC duplicate to the lowest free fd; F_GETFL reports the fd's access mode (glibc fdopen checks it); other commands return 0 |
| 79 | getcwd | full | returns the process's current directory (a leading slash plus the stored key) |
| 80 | chdir | full | sets a per-process working directory; resolves the target against the current cwd and requires it be a directory |
| 81 | fchdir | full | sets the working directory from a directory fd (coreutils walk trees this way); the fd carries its normalized key |
| 82 | rename | partial | within the writable overlay (a directory carries its subtree); a read-only source is EXDEV |
| 83 | mkdir | full | creates a writable-overlay directory; EEXIST if anything already resolves there |
| 84 | rmdir | partial | removes an empty overlay directory (ENOTEMPTY otherwise); a read-only bundle directory is EROFS |
| 86 | link | stub | EPERM (no hard links); callers fall back to copying |
| 87 | unlink | partial | removes a writable overlay file (open-unlink: the inode is held until the last fd closes); a read-only path returns EROFS |
| 88 | symlink | partial | creates an overlay symlink (target verbatim); followed on open/exec/stat at the final component (leaf), overlay only |
| 89 | readlink | partial | returns an overlay symlink's stored target; EINVAL for a non-link, ENOENT if absent |
| 90 | chmod | stub | no permission bits to enforce; returns 0 |
| 91 | fchmod | stub | as chmod |
| 95 | umask | stub | reports a conventional prior mask (022) |
| 137 | statfs | stub | quiet ENOSYS; no real filesystem to describe (libselinux probes this) |
| 138 | fstatfs | stub | quiet ENOSYS, as statfs |
| 217 | getdents64 | full | merges the 9P directory stream with the overlay's children |
| 257 | openat | full | as open; dirfd honored via at_path (a relative path resolves against the open directory fd), AT_FDCWD against the cwd |
| 258 | mkdirat | full | as mkdir; dirfd honored via at_path |
| 262 | newfstatat | full | AT_EMPTY_PATH maps to fstat; dirfd honored via at_path |
| 263 | unlinkat | partial | as unlink; AT_REMOVEDIR routes to rmdir; dirfd honored via at_path |
| 264 | renameat | partial | as rename; dirfds honored via at_path |
| 265 | linkat | stub | as link (EPERM); dirfds AT_FDCWD |
| 266 | symlinkat | partial | as symlink (overlay link created and followed at the leaf) |
| 267 | readlinkat | partial | as readlink (returns an overlay symlink's stored target) |
| 268 | fchmodat | stub | as chmod |
| 269 | faccessat | full | existence check via meta_path; no permission bits, dirfd ignored |
| 292 | dup3 | full | like dup2 |
| 293 | pipe2 | full | flags (O_CLOEXEC, O_NONBLOCK) ignored |
| 316 | renameat2 | partial | as rename; dirfds honored via at_path; flags ignored |
| 326 | copy_file_range | stub | quiet ENOSYS; cp and mv fall back to a read/write loop |
| 332 | statx | full | basic stats mask |
| 439 | faccessat2 | full | same as faccessat (the flags variant) |

Path components `.` and `..` are normalized before a 9P walk (`split_path` in
ns.c), so `.../13/.` and `a/b/../c` resolve. This is what lets gcc's
`is_directory()` probe (which appends `/.`) recognize a library directory.

The writable overlay is directory-aware: entries carry an is_dir flag and a
normalized key, do_open resolves overlay and 9P directories (and their merge),
and getdents lists both sides. Each process has a working directory (a stored
key, "" at the root); `resolve_path` applies it to a relative path before the
overlay lookup or 9P walk, so a relative open, stat, exec, mkdir, or rename
resolves against the cwd. It is copied on fork and the posix_spawn child and
preserved across execve.

### Memory

| nr | name | status | notes |
|----|------|--------|-------|
| 9 | mmap | partial | anonymous (bump arena, down) and file-backed (eager copy from the fd); a MAP_FIXED overlay zeroes the range first (so a .bss overlay reads as zero), then copies any file bytes; prot not enforced; no COW |
| 10 | mprotect | stub | flat RW map, nothing to enforce |
| 11 | munmap | stub | returns 0; the bump arena does not reclaim |
| 12 | brk | full | grows the heap within the arena |
| 27 | mincore | stub | quiet ENOSYS; no page-residency to report |
| 25 | mremap | stub | quiet ENOSYS; glibc realloc then copies into a fresh mmap itself, which is all a real mremap could do against the non-reclaiming bump arena |
| 28 | madvise | stub | returns 0 |
| 221 | fadvise64 | stub | returns 0; advisory only (coreutils calls posix_fadvise on its input) |

### Process and thread lifecycle

| nr | name | status | notes |
|----|------|--------|-------|
| 56 | clone | full | CLONE_VM\|CLONE_VFORK is posix_spawn; CLONE_VM alone a thread; else the fork path |
| 435 | clone3 | full | parses clone_args and routes like clone (posix_spawn, thread, or fork) |
| 57 | fork | full | eager address-space copy, shared fds |
| 58 | vfork | full | treated as fork |
| 59 | execve | full | replaces the image from a bundle/host file (9P) or a writable-overlay file (in-memory); releases a vfork parent and keeps a borrowed space |
| 60 | exit | full | ends the calling thread |
| 61 | wait4 | partial | child processes only; pid treated as 32-bit (so -1 matches any); rusage ignored |
| 231 | exit_group | full | ends the process |
| 39 | getpid | full | process pid |
| 110 | getppid | full | parent pid |
| 111 | getpgrp | stub | returns 1, a single process group |
| 109 | setpgid | stub | returns 0, one process group |
| 112 | setsid | stub | returns the pid as the session id |
| 115 | getgroups | stub | returns 0, no supplementary groups |
| 116 | setgroups | stub | returns 0, accepted and ignored |
| 105 | setuid | stub | returns 0; one root identity, so a set to it succeeds |
| 106 | setgid | stub | returns 0, as setuid |
| 113 | setreuid | stub | returns 0, as setuid |
| 114 | setregid | stub | returns 0, as setuid |
| 117 | setresuid | stub | returns 0; posix_spawn's child sets ids before exec |
| 119 | setresgid | stub | returns 0, as setresuid |
| 131 | sigaltstack | stub | returns 0; no signal delivery, so accepted and ignored |
| 186 | gettid | full | thread id |
| 218 | set_tid_address | full | records the clear-on-exit tid address |

### Synchronization

| nr | name | status | notes |
|----|------|--------|-------|
| 202 | futex | partial | private FUTEX_WAIT and FUTEX_WAKE; no requeue, no timeout |
| 24 | sched_yield | full | cooperative yield |
| 204 | sched_getaffinity | stub | reports a single online cpu (cpu 0) so a tool sizes its pool to one |
| 230 | clock_nanosleep | stub | returns 0 (no timers; a cooperative guest has nothing to sleep on) |

### Signals (generation and default actions; no handler delivery yet)

Stages A to C: real disposition and mask state, signal generation, and the
default terminate action, so a process is killable (even while blocked in a
syscall) and a fatal signal is reported as `WIFSIGNALED`. Invoking a
user-installed handler, and returning `EINTR` to a caught syscall, are a later
stage; a caught signal is recorded as pending but not yet delivered. See
doc/m16-signals.md.

| nr | name | status | notes |
|----|------|--------|-------|
| 13 | rt_sigaction | partial | stores the per-process disposition (handler/flags/restorer/mask); reset to default on execve; no handler invocation yet |
| 14 | rt_sigprocmask | partial | maintains the per-thread blocked mask; SIGKILL/SIGSTOP cannot be blocked |
| 131 | sigaltstack | partial | stores the alternate stack for a future delivery stage |
| 62 | kill | partial | posts a process-directed signal and wakes the target; acts on the default terminate action; pid>0 only (group forms are a no-op); sig 0 is an existence probe (ESRCH if absent) |
| 200 | tkill | partial | thread-directed, two-argument form (musl raise/abort) |
| 234 | tgkill | partial | thread-directed, three-argument form |

### Sockets

| nr | name | status | notes |
|----|------|--------|-------|
| 41 | socket | stub | quiet ENOSYS; no sockets in this guest (bash probes for one at startup) |
| 51 | getsockname | stub | quiet ENOSYS; no sockets in this guest |
| 52 | getpeername | stub | quiet ENOSYS; gcc probes whether its output fd is a socket |

### Extended attributes

| nr | name | status | notes |
|----|------|--------|-------|
| 188 | setxattr | stub | quiet ENOTSUP; no extended attributes (cp and install then copy without them) |
| 189 | lsetxattr | stub | quiet ENOTSUP |
| 190 | fsetxattr | stub | quiet ENOTSUP |
| 191 | getxattr | stub | quiet ENOTSUP |
| 192 | lgetxattr | stub | quiet ENOTSUP |
| 193 | fgetxattr | stub | quiet ENOTSUP |

### Thread-local storage, identity, time, misc

| nr | name | status | notes |
|----|------|--------|-------|
| 158 | arch_prctl | full | ARCH_SET_FS sets the per-thread FS base |
| 102 | getuid | stub | returns 0 |
| 104 | getgid | stub | returns 0 |
| 107 | geteuid | stub | returns 0 |
| 108 | getegid | stub | returns 0 |
| 157 | prctl | stub | returns 0 |
| 201 | time | partial | derived from the cycle counter (approximate) |
| 96 | gettimeofday | partial | a fixed epoch plus the cycle counter, split into seconds and microseconds |
| 98 | getrusage | stub | zeroed counters; no CPU-time accounting (gcc, as query it) |
| 99 | sysinfo | stub | reports total RAM and a plausible free fraction; cc1 sizes its allocator from this |
| 228 | clock_gettime | partial | a fixed epoch plus the cycle counter at an assumed 1 GHz; shares the epoch with file timestamps so a freshly built file is not in the future |
| 63 | uname | full | fixed Linux/jotun/x86_64 fields |
| 318 | getrandom | partial | cycle-counter PRNG, not cryptographic |
| 302 | prlimit64 | partial | reports RLIMIT_NOFILE as the real fd cap (LINUX_NFD), RLIM_INFINITY for other resources; a new limit is ignored |
| 273 | set_robust_list | stub | returns 0 |
| 334 | rseq | stub | returns 0 |

## Unimplemented, by phase

Each entry lists the x86-64 number, a priority, the anticipated need, and the
difficulty. Priority reflects how often real static binaries reach for it and
how much it unblocks, not just difficulty.

### Phase 1: cheap wins and common fallbacks

These are low-difficulty and remove the most common `unhandled syscall` logs and
`ENOSYS` fallbacks. Most are a few lines over the existing VFS, arena, or time
source.

| nr | name | need | difficulty | notes |
|----|------|------|------------|-------|
| 19 | readv | high | easy | mirror of writev |
| 17 | pread64 | high | easy | positioned read over the existing fd read |
| 18 | pwrite64 | med | easy | positioned write (ramfs) |
| 40 | sendfile | high | easy | return ENOSYS cleanly, or a read/write loop; silences the cat/cp path |
| 35 | nanosleep | high | medium | cooperative: yield or a timed block; busybox sleep, retries |
| 21 | access | high | easy | stat-based existence/permission check |
| 269 | faccessat | high | easy | access with a dirfd (ignored like openat) |
| 97 | getrlimit | med | easy | alias of the prlimit64 stub |
| 160 | setrlimit | med | easy | accept and ignore |
| 98 | getrusage | med | easy | zeroed struct |
| 95 | umask | med | easy | store and return the previous mask |
| 74 | fsync | med | easy | no-op (no persistence) |
| 75 | fdatasync | med | easy | no-op |
| 77 | ftruncate | med | easy | resize a ramfs file |
| 76 | truncate | low | easy | path form of ftruncate |
| 99 | sysinfo | low | easy | zeroed/plausible struct |
| 100 | times | low | easy | cycle-counter tick totals |
| 229 | clock_getres | low | easy | fixed resolution |
| 78 | getdents | low | easy | legacy layout over getdents64 |

### Phase 2: a mutable namespace and I/O multiplexing

These make shells and tools that write, rename, change directory, or wait on
several descriptors work. They need new state (a cwd, ramfs mutation,
poll-aware blocking) but stay within the cooperative model.

| nr | name | need | difficulty | notes |
|----|------|------|------------|-------|
| 7 | poll | high | medium | cooperative readiness over pipes and the console |
| 271 | ppoll | high | medium | poll with a timeout mask |
| 23 | select | med | medium | poll expressed as fd sets |
| 270 | pselect6 | med | medium | select with a mask |
| 85 | creat | low | easy | open with O_CREAT, O_WRONLY, O_TRUNC |
| 137 | statfs | low | medium | plausible filesystem stats (df) |
| 138 | fstatfs | low | medium | fd form of statfs |
| 247 | waitid | med | medium | wait4 variant some libcs prefer |
| 109 | setpgid | med | easy | job-control stub for shells |
| 121 | getpgid | med | easy | stub |
| 111 | getpgrp | med | easy | stub |
| 112 | setsid | med | easy | stub |
| 124 | getsid | low | easy | stub |
| 280 | utimensat | low | medium | timestamps on ramfs files |

### Phase 3: signals, sockets, and event descriptors

Larger subsystems. Each is a project on its own and should land only when a
target workload needs it.

| nr | name | need | difficulty | notes |
|----|------|------|------------|-------|
| 13 | rt_sigaction (real) | med | hard | requires a delivery mechanism on the SYSCALL return path |
| 15 | rt_sigreturn | med | hard | pairs with real signal delivery |
| 62 | kill | med | hard | inter-process signal delivery |
| 131 | sigaltstack | low | medium | needed once signals deliver |
| 41 | socket | med | hard | currently a quiet ENOSYS stub; real support bridges to the guest net layer (net.c, dial strings) |
| 42 | connect | med | hard | net layer |
| 43 | accept | med | hard | net layer |
| 44-47 | sendto/recvfrom/sendmsg/recvmsg | med | hard | net layer datagram and stream I/O |
| 49 | bind | med | hard | net layer |
| 50 | listen | med | hard | net layer |
| 288 | accept4 | low | hard | accept with flags |
| 213 | epoll_create | low | hard | event loops; servers |
| 291 | epoll_create1 | low | hard | the flags form; CPython's selector probes it, then falls back to poll on ENOSYS |
| 232 | epoll_wait | low | hard | with epoll_ctl |
| 233 | epoll_ctl | low | hard | |
| 290 | eventfd2 | low | medium | cross-thread wakeups (284 eventfd is the older form) |
| 283 | timerfd_create | low | medium | timers as descriptors (with timerfd_settime 286) |
| 10 | mprotect (enforce) | low | hard | real page-permission changes in the vmspace |

### Phase 4: out of scope for now

Returned as `ENOSYS` and not planned unless a concrete need appears: io_uring
(425 and up), inotify/fanotify, keyctl, bpf, ptrace, the namespace and cgroup
calls (setns, unshare, mount, pivot_root), process_vm_readv/writev, userfaultfd,
seccomp, kexec, perf_event_open, and the rest of the administrative tail.
`clone3` (435) is also deferred; musl and glibc fall back to `clone` when
`clone3` returns `ENOSYS`.

## Notes on the current stubs

A few handled calls are stubs that a future phase should make real:

- **fcntl** implements F_DUPFD/F_DUPFD_CLOEXEC (a real new fd); other commands
  return 0, so F_GETFL/F_GETFD do not report flags and a program that checks
  O_NONBLOCK may misbehave.
- **time, clock_gettime, getrandom** are derived from the cycle counter. Fine
  for startup and seeding, not for real timekeeping or cryptography.
- **the uid/gid family** returns 0 (a single root-ish identity), which suits the
  single trust domain.

## Fidelity gaps, by effort

The phase tables above sort unimplemented calls by priority. This section is a
different cut: an accounting of the Linux behaviors the guest currently fakes,
ignores, or omits because no workload has forced the issue, sorted by the effort
to make each faithful. It covers behavior glossed over inside calls we already
handle, not just missing call numbers.

Two notes on scope. A few of these (permission and ownership enforcement,
multi-user access control) are arguably non-goals given the single trust domain:
faithful reporting is cheap, faithful enforcement is high effort and may never be
wanted. And the infrastructure-scaling items (guest RAM into the GiB range,
overlay caps, `LINUX_NPROC`/`TASK_MAX`) are left out here. Those are tracked in
TODO.md under "Scaling to a full build" and are about size, not Linux semantics.

### Low effort

A few lines each, over the VFS, arena, or time source already present. Mostly
missing cheap calls or thin fakes.

Missing calls that are near-trivial over existing machinery:

- `readv` (19): a mirror of the existing `writev`.
- `pwrite64` (18), `ftruncate` (77), `truncate` (76): positioned write and resize
  over ramfs. `ftruncate` is wanted by anything that preallocates.
- `fsync` (74), `fdatasync` (75), `sync` (162): no persistence, so a `return 0`
  no-op is correct.
- `creat` (85): just `open(O_CREAT|O_WRONLY|O_TRUNC)`.
- `getdents` (78, legacy): a repack of `getdents64`.
- `getrlimit` (97), `setrlimit` (160): alias the `prlimit64` stub.
- `clock_getres` (229), `times` (100): fixed resolution, cycle-counter tick totals.
- `sendfile` (40): a read/write loop, or a clean `ENOSYS` (the fallback works).

Thin fakes worth making real:

- **File mode bits.** `chmod`, `fchmod`, and `fchmodat` are no-ops; `umask`
  reports a hardcoded `022` without storing it; overlay and ramfs files carry no
  mode at all. We do not enforce permissions (single trust domain, so enforcement
  is arguably a non-goal), but reporting a real mode and honoring `umask` on
  create is cheap. Only the writable-mount create path honors the open mode today.
- **uid/gid identity.** `getuid`, `geteuid`, and the rest return 0; `set*id`
  return 0. Fine for one domain. Making it configurable is low value but low effort.
- **`/dev` leftovers.** `/dev/fd/N`, the `std*` nodes as real-fd symlinks (they
  route to the console, not to the process's actual fd 0, 1, or 2), and a real
  `st_rdev`.
- **`sysinfo` and `getrusage`** are zeroed or plausible; filling a few real-ish
  fields is cheap.
- **xattr family, `copy_file_range`, `splice`.** Return `ENOTSUP` or `ENOSYS`;
  callers fall back. Leave as-is unless a workload appears.

### Medium effort

New state or a new mechanism, but inside the cooperative single-vcpu model.

- **Real timekeeping and sleeping.** `time`, `clock_gettime`, and `gettimeofday`
  are cycle-counter approximations at an assumed 1 GHz; `nanosleep` and
  `clock_nanosleep` do not actually sleep (they return 0 immediately). A build
  that times itself, or a `sleep` and retry-with-backoff, is wrong. Needs a
  calibrated timebase and a timed-yield.
- **`select`, `pselect6`, `ppoll`.** `poll` exists; these are the same readiness
  model expressed as fd sets, or with a timeout mask.
- **tty, termios, `ioctl`.** `ioctl` always returns `ENOTTY`, so `TIOCGWINSZ`
  (window size), `tcgetattr` and `tcsetattr` (raw mode), and `isatty` all fail.
  Interactive tools (readline, editors, anything checking a terminal) silently
  drop to dumb or non-interactive mode. This is why Node's configure block-buffered
  its output. Presenting the console as a minimal tty is medium.
- **Advisory file locking.** `flock` and `fcntl(F_SETLK/F_GETLK)` are absent
  entirely. A tool coordinating with lockfiles (sqlite, dpkg-style builds, some
  `make` jobserver edges) gets no locking.
- **`fcntl` flags.** Beyond `F_DUPFD` and cloexec, `F_GETFL`/`F_SETFL` do not
  report or toggle `O_NONBLOCK` or `O_APPEND`; a program that flips nonblock on an
  fd is ignored.
- **`O_NONBLOCK` end to end.** Pipes and the console always block; honoring
  nonblock (returning `EAGAIN`) is medium.
- **`futex` completeness.** Only private `WAIT` and `WAKE`; no requeue
  (`CMP_REQUEUE`, which some glibc condvars use), no timeout, no PI, no
  cross-process (shared) futex.
- **`munmap` and arena reclaim.** `munmap` is a no-op and the bump arena never
  reclaims, so a long-lived process that maps and unmaps leaks address space.
  `mremap` is a stub. (Enforcing `prot` is separate; that is high effort.)
- **Hard links.** `link` and `linkat` return `EPERM`. Real overlay hardlinks (one
  inode, several names, refcounted) would unblock builds that hardlink instead of
  copy.
- **`waitid` (247), `utimensat` (280), `statfs`/`fstatfs` real values,
  `eventfd`/`eventfd2`.** `utimensat` would set a real mtime (today mtime is
  seconds-only and atime and ctime just mirror it). `statfs` and `fstatfs` are
  quiet `ENOSYS` now; `df` and some probes want them.
- **9P fid aliasing across fork.** Cloning an open 9P fid across `fork` shares the
  one server-side fid and its offset (pool `GFID_MAX` is 64). Two processes
  reading the same inherited fd interfere. This is a real correctness gloss, not
  just a missing feature.

### High effort

Whole subsystems. Each is a project; land only when a target workload needs it.

- **Signals.** Stages A to C have landed: real disposition and mask state,
  generation (`kill`/`tkill`/`tgkill`), and the default terminate action, so a
  process is killable even while blocked in a syscall and a fatal signal reports
  `WIFSIGNALED` (see doc/m16-signals.md). What remains is the harder half:
  invoking a user-installed handler (stage D: build a frame on the user stack,
  divert the return, implement `rt_sigreturn`, honor `SA_RESTART`), catchable
  faults (stage E: save the user context in the fault stub so `SIGSEGV` can reach
  a handler), and timers (`SIGALRM`/`setitimer`). Still absent regardless:
  `SIGCHLD` generation, `SIGINT`/Ctrl-C (no async console input), and
  interrupting a no-syscall compute loop (no preemption timer).
- **Sockets in the Linux ABI.** `socket`, `bind`, `listen`, `accept`, `connect`,
  the `send*` and `recv*` family all stub `ENOSYS`. The guest has a net layer
  (net.c, dial strings, vswitch); it is just not bridged to the Linux socket
  calls. Bridging is the work.
- **epoll** (`epoll_create1`, `epoll_ctl`, `epoll_wait`). Event loops and servers;
  Node and the net poller want it. It could ride on the existing `poll` readiness
  model, which lowers it toward medium, but it is still a real build-out. Today
  `epoll_create1` is the last unhandled call Node's configure touches, and CPython
  falls back to `poll`.
- **Real page permissions.** The vmspace maps every user page RW and user: no
  `mprotect` enforcement, no W^X or NX, no COW fork (fork is an eager copy).
  Enforcing `prot` means real per-page permission changes and fault handling.
- **Job control and sessions.** `setpgid`, `getpgid`, `setsid`, and `getsid` are
  fixed-value stubs; no controlling terminal, no `tcsetpgrp`, no `SIGTTOU` or
  `SIGTTIN`. A real interactive shell's job control is faked. This is coupled to
  signals and the tty work above.
- **Multi-vcpu / SMP guest.** `lproc_cur` is a single global, the SYSCALL MSRs are
  armed on cpu0 only, and `sched_getaffinity` reports one cpu. True SMP is a
  structural change.
- **The administrative tail.** Namespaces and cgroups (`unshare`, `setns`,
  `mount`, `pivot_root`), `seccomp`, `ptrace`, io_uring, inotify and fanotify,
  userfaultfd, keyctl, bpf. Out of scope; `ENOSYS` until a concrete need appears.
