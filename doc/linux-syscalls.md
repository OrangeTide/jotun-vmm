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

## Implemented (56 calls)

### File and directory I/O

| nr | name | status | notes |
|----|------|--------|-------|
| 0 | read | full | console EOF, pipe (blocking), ramfs, 9P |
| 1 | write | full | console, pipe (blocking), ramfs; 9P bundle is read-only |
| 2 | open | full | 9P bundle (RO) and ramfs overlay for writes/creates |
| 3 | close | full | releases pipe and 9P references, refcounted |
| 4 | stat | full | path via ramfs or 9P |
| 5 | fstat | full | reports pipes as FIFO, console, ramfs, 9P |
| 6 | lstat | full | same as stat (no symlinks in the bundle) |
| 8 | lseek | full | file and ramfs cursors |
| 16 | ioctl | stub | returns ENOTTY so libc falls back |
| 20 | writev | full | loops over do_write |
| 22 | pipe | full | in-memory byte ring |
| 32 | dup | full | refcounted share of the backing object |
| 33 | dup2 | full | targets 0/1/2 allowed (shell redirects) |
| 72 | fcntl | partial | returns 0; does not honor F_DUPFD or report F_GETFL/F_GETFD |
| 79 | getcwd | stub | always returns "/" |
| 217 | getdents64 | full | packs 9P directory entries |
| 257 | openat | partial | dirfd ignored; namespace is effectively rooted |
| 262 | newfstatat | full | AT_EMPTY_PATH maps to fstat |
| 267 | readlinkat | stub | returns EINVAL (no symlinks) |
| 292 | dup3 | full | like dup2 |
| 293 | pipe2 | full | flags (O_CLOEXEC, O_NONBLOCK) ignored |
| 332 | statx | full | basic stats mask |

### Memory

| nr | name | status | notes |
|----|------|--------|-------|
| 9 | mmap | partial | anonymous only, bump arena growing down; no file-backed, no MAP_FIXED |
| 10 | mprotect | stub | flat RW map, nothing to enforce |
| 11 | munmap | stub | returns 0; the bump arena does not reclaim |
| 12 | brk | full | grows the heap within the arena |
| 28 | madvise | stub | returns 0 |

### Process and thread lifecycle

| nr | name | status | notes |
|----|------|--------|-------|
| 56 | clone | full | CLONE_VM makes a thread; otherwise the fork path |
| 57 | fork | full | eager address-space copy, shared fds |
| 58 | vfork | full | treated as fork |
| 59 | execve | full | replaces the image from a bundle program |
| 60 | exit | full | ends the calling thread |
| 61 | wait4 | partial | child processes only; rusage ignored |
| 231 | exit_group | full | ends the process |
| 39 | getpid | full | process pid |
| 110 | getppid | full | parent pid |
| 186 | gettid | full | thread id |
| 218 | set_tid_address | full | records the clear-on-exit tid address |

### Synchronization

| nr | name | status | notes |
|----|------|--------|-------|
| 202 | futex | partial | private FUTEX_WAIT and FUTEX_WAKE; no requeue, no timeout |
| 24 | sched_yield | full | cooperative yield |

### Signals (minimal)

| nr | name | status | notes |
|----|------|--------|-------|
| 13 | rt_sigaction | stub | returns 0; no delivery |
| 14 | rt_sigprocmask | stub | returns 0; no delivery |
| 234 | tgkill | stub | returns 0; no delivery |

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
| 228 | clock_gettime | partial | cycle counter at an assumed 1 GHz |
| 63 | uname | full | fixed Linux/jotun/x86_64 fields |
| 318 | getrandom | partial | cycle-counter PRNG, not cryptographic |
| 302 | prlimit64 | stub | reports RLIM_INFINITY |
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
| 96 | gettimeofday | high | easy | reuse the clock_gettime source |
| 35 | nanosleep | high | medium | cooperative: yield or a timed block; busybox sleep, retries |
| 230 | clock_nanosleep | med | medium | same mechanism as nanosleep |
| 21 | access | high | easy | stat-based existence/permission check |
| 269 | faccessat | high | easy | access with a dirfd (ignored like openat) |
| 72 | fcntl (real) | high | medium | honor F_DUPFD, F_GETFL/F_SETFL, F_GETFD/F_SETFD |
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
| 87 | unlink | high | medium | remove a ramfs file |
| 263 | unlinkat | high | medium | unlink with a dirfd |
| 83 | mkdir | med | medium | ramfs directories (needs a ramfs dir model) |
| 258 | mkdirat | med | medium | mkdir with a dirfd |
| 84 | rmdir | med | medium | remove a ramfs directory |
| 82 | rename | med | medium | rename within the ramfs |
| 264 | renameat | med | medium | rename with dirfds |
| 85 | creat | low | easy | open with O_CREAT, O_WRONLY, O_TRUNC |
| 80 | chdir | high | medium | needs a per-process cwd, then getcwd and relative paths become real |
| 81 | fchdir | med | medium | cwd from a dir fd |
| 137 | statfs | low | medium | plausible filesystem stats (df) |
| 138 | fstatfs | low | medium | fd form of statfs |
| 247 | waitid | med | medium | wait4 variant some libcs prefer |
| 109 | setpgid | med | easy | job-control stub for shells |
| 121 | getpgid | med | easy | stub |
| 111 | getpgrp | med | easy | stub |
| 112 | setsid | med | easy | stub |
| 124 | getsid | low | easy | stub |
| 280 | utimensat | low | medium | timestamps on ramfs files |
| 25 | mremap | med | hard | glibc malloc uses it; hard against a bump arena |

### Phase 3: signals, sockets, and event descriptors

Larger subsystems. Each is a project on its own and should land only when a
target workload needs it.

| nr | name | need | difficulty | notes |
|----|------|------|------------|-------|
| 13 | rt_sigaction (real) | med | hard | requires a delivery mechanism on the SYSCALL return path |
| 15 | rt_sigreturn | med | hard | pairs with real signal delivery |
| 62 | kill | med | hard | inter-process signal delivery |
| 131 | sigaltstack | low | medium | needed once signals deliver |
| 41 | socket | med | hard | bridge to the guest net layer (net.c, dial strings) |
| 42 | connect | med | hard | net layer |
| 43 | accept | med | hard | net layer |
| 44-47 | sendto/recvfrom/sendmsg/recvmsg | med | hard | net layer datagram and stream I/O |
| 49 | bind | med | hard | net layer |
| 50 | listen | med | hard | net layer |
| 288 | accept4 | low | hard | accept with flags |
| 213 | epoll_create | low | hard | event loops; servers |
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

- **fcntl** returns 0 unconditionally. F_DUPFD should allocate a new fd, and
  F_GETFL/F_GETFD should report flags. Programs that check O_NONBLOCK may
  misbehave today.
- **getcwd** always returns "/". A real cwd (Phase 2, with chdir) makes this and
  relative-path resolution correct.
- **time, clock_gettime, getrandom** are derived from the cycle counter. Fine
  for startup and seeding, not for real timekeeping or cryptography.
- **the uid/gid family** returns 0 (a single root-ish identity), which suits the
  single trust domain.
