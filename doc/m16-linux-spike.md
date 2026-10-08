# M16.1 Linux binary-compat spike: findings

This records an experimental spike, not a supported feature. The goal was the
cheapest possible proof that the libOS can run an unmodified static Linux
binary: load a `musl-gcc -static -no-pie` hello, trap its syscalls, and reach
`exit_group(0)` with output on the console. That proof now passes.

## Result

```
[bin] loading linuxhello
[bin] loaded, entry=0x000000000040104b
[bin] syscall armed
[bin] stack built
[bin] entering guest
hello from musl[bin] exited, status=0
init: done
```

The binary is an ordinary Linux program (`example/linuxhello.c`, just
`printf("hello from musl\n")`). It was not recompiled against any Jotun header.
Run it with `make linux-spike` on an x86-64 host with `musl-gcc` on PATH.

## How it works

The guest already runs the loaded program and the libOS in the same ring (there
is no privilege boundary, which is the whole point of the unikernel model). So
the "trap" is not a ring transition. The pieces:

- `guest/libos/linuxelf.c` loads the ELF with the existing `elf_load_fid`. A
  static non-PIE binary is `ET_EXEC` linked at `0x400000`, which is exactly the
  window the native programs use (`JOTUN_PROG_BASE`), so it loads in place with
  no relocation. It then builds a System V initial stack (`argc`, `argv`, a null
  `envp`, and an auxv with `AT_PAGESZ` and `AT_RANDOM`), arms the SYSCALL MSRs,
  and jumps to the entry.
- `guest/libos/arch/x86_64_linuxsys.S` is the SYSCALL machinery. `EFER.SCE`
  turns on the `syscall` instruction, `LSTAR` points at our handler, and `STAR`
  names the ring-0 code selector. Because `syscall` here does not change ring,
  the handler must not return with `sysret`. It saves the caller's `rcx`/`r11`,
  switches to a private handler stack, calls the C dispatcher, then resumes with
  `jmp *%rcx` after reloading `RFLAGS`. The kernel-to-program handoff
  (`linux_enter`/`linux_exit`) saves a resume context so the program's
  `exit_group` returns cleanly back into the loader.
- `linux_sys_dispatch` services the syscalls the hello issues:
  `arch_prctl(ARCH_SET_FS)` (a `wrmsr` to `FS_BASE`), `set_tid_address`,
  `ioctl(TIOCGWINSZ)` answered with `-ENOTTY`, `write`/`writev` to the console,
  and `exit`/`exit_group`. Everything else returns `-ENOSYS`.

The whole program runs with interrupts disabled. It is short, its output is
polled MMIO, and this keeps the cooperative scheduler's timer from firing on the
program's stack. `linux_elf_run` re-enables interrupts when the program exits.

## The one host change that was required

The syscall trap needs one trusted-core edit: the host must advertise the
SYSCALL CPUID feature to the guest. KVM refuses a guest `wrmsr` that sets
`EFER.SCE` unless the guest CPUID reports `X86_FEATURE_SYSCALL`, and Jotun had
never called `KVM_SET_CPUID2` at all. `host/vmm_kvm.c` now copies
`KVM_GET_SUPPORTED_CPUID` into the vcpu with `KVM_SET_CPUID2`. The normal libOS
never executes the `CPUID` instruction, so this changes nothing for existing
guests; it only unblocks the SYSCALL enable. This is the single surprise the
spike turned up. Everything else lived in the experimental guest module.

## What this proves, and what it does not

Proven: the hypervisor model has no structural objection to running a foreign
static binary. The address-space layout already matches a non-PIE Linux
executable, the SYSCALL trap is a few dozen lines of assembly, and musl's
startup really is as calm as expected (five distinct syscalls, no `mmap`, no
threads, no signals, no relocation). No unhandled syscall appeared in the run.

Not yet addressed, and each is a known milestone, not a surprise:

- Static non-PIE only. A static-PIE binary (Alpine's default) needs
  `R_*_RELATIVE` self-relocation (M16.3). `elf_load_fid` loads at the fixed
  `p_vaddr`, with the `0x400000..0x600000` window as its only bound.
- `mmap`/`brk` are stubbed. The guest has a flat heap and an 8 MiB RAM identity
  map, no demand paging. Anything that reserves large sparse arenas (Go) needs
  real address-space management (M16.5).
- No exception handlers. Only the timer IDT vector exists. A bad guest
  instruction triple-faults to a bare "unknown exit" with no diagnostics, which
  made bring-up slower than it needed to be. A `#GP`/`#PF` handler that prints
  the faulting RIP is worth adding before M16.2.
- Interrupts are off for the whole run, so this cannot host anything that blocks
  or waits. That is fine for a batch hello and nothing more.
- One fixed program stack, single task. Threads (`clone`/`futex`) are M16.4.
- x86-64 only. The arm64 SVC path is unwritten; `linux_elf_run` is a stub there.

## Go / no-go

Go, with eyes open. The spike cost little and removed the central doubt: the
ring-0 SYSCALL trap works and musl runs unmodified. The sensible next step is
M16.2 (broaden the VFS and process syscalls toward a static `busybox`), but it
should open by adding the `#GP`/`#PF` diagnostic handler, because the next
binaries will fault in ways the hello never did and the current failure mode is
a silent triple fault. The larger risks (`mmap` with lazy commit, `futex`,
static-PIE relocation) are all still ahead and unchanged by this result; none of
them block the busybox step. The experimental module can stay as the seed of the
real loader, or be rewritten once the shape of M16.2 is clear.

## M16.2: VFS and process syscalls

Follow-on work broadened the syscall surface, driven by self-contained musl test
programs rather than an external binary. New pieces:

- A diagnostic handler on the CPU exception vectors (`x86_64_fault.S`,
  `fault_report` in `x86_64.c`). A fault now prints the vector, error code,
  faulting RIP, CR2, and the GP registers, then halts, instead of a silent
  triple fault. This was the prerequisite the go/no-go called for, and it paid
  off immediately during bring-up.
- A file-descriptor table over the read-only 9P bundle (`linuxelf.c`):
  `openat`/`open`, `close`, `read`, `lseek`, `stat`/`lstat`/`newfstatat`,
  `fstat`, and `getdents64`; `dup` with a per-fid refcount; in-memory
  `pipe`/`pipe2`; and the universal startup syscalls `mprotect` (a no-op on the
  flat map), `clock_gettime`, `getrandom`, and `prlimit64`. Writes to the bundle
  return `-EROFS`.
- Committed musl tests, run with `make linux-spike SPIKE_PROG=<name>`:
  `linuxcat` (open/read/fstat/stat), `linuxls` (getdents64), and `linuxpipe`
  (pipe/read/write). All run clean with no unhandled syscalls:

```
hello.txt (30 bytes): hello from the smolvfs bundle
entries in /: boot/ hello hello.txt ... motd rc net/
read back: data through a pipe
```

## M16.5: a heap, and glibc busybox running

The spike then grew a real heap for the foreign libc, and found a register-ABI
bug along the way.

- A Linux-specific address space (`linuxelf.c`), independent of the shared
  native program window: the binary loads in `0x400000..0x800000`, `brk` grows
  up from `0x800000`, and anonymous `mmap` grows down from the top of RAM less a
  reservation matching the host's per-vcpu stacks. They meet in the middle and
  fail there. `munmap` is accepted but does not reclaim (a bump arena is enough
  for a run-once program). Guest RAM rose from 8 to 32 MiB so the arena is
  ~20 MiB.
- The **register-ABI fix**: the SYSCALL trampoline preserved only `rcx`/`r11`,
  but the Linux ABI preserves every register except `rax`/`rcx`/`r11`. The SysV
  C dispatcher clobbered `rdi`/`rsi`/`rdx`/`r10`/`r8`/`r9`, so a program resumed
  from a syscall with those trashed. The musl hello survived it (each result was
  used at once), but a libc malloc keeping live values across a syscall then
  issued a follow-on syscall with a garbage number. The trampoline now saves and
  restores the six argument registers.
- More process syscalls: `dup2`/`dup3` (via a `clone_fd` helper that refcounts
  files and pipes correctly), `getuid`/`geteuid`/`getgid`/`getegid`, `prctl`.

A committed musl test (`linuxmem`) grows the break directly and allocates
through malloc (musl's mallocng gets memory from anonymous `mmap`), writing and
reading back 100 KiB plus 128 smaller blocks. The whole musl suite exits clean
with no unhandled syscalls.

The glibc static `/usr/bin/busybox`, a deliberately harder target run only as a
manual probe (not committed, since its make target would need an external path),
**now runs to `exit_group` and prints its full multi-call banner with zero
unhandled syscalls** on a guest that previously could not get it past startup.
That it executes at all, RELRO and TLS and heap and all, is the real evidence
that the model scales past the musl hello. The musl path stays the committed
one; busybox applets proper still want argv delivery and a broader VFS (M16.2
onward), but the heap wall (M16.5) is down.

## jlibc malloc (separate, native programs)

Independently, vmm's native bundle programs gained an allocator: skjegg's
boundary-tag malloc and its static-region sbrk, lifted into
`guest/prog/libc/jmalloc.c` and `jsbrk.c` (CC0). This is unrelated to the Linux
path, which serves the foreign binary's own malloc; it just fills a gap in the
native `jlibc`. See `make malloc-test`.

## argv delivery and a writable ramfs

Two more pieces let a multi-call binary actually do something.

- **argv**: `build_stack` now lays the full argv on the initial stack, the
  loaded file as argv[0] followed by the boot arguments after it. So
  `./jotun libos.elf bin busybox echo hello world` reaches busybox as
  `busybox echo hello world`, and it prints `hello world`: a glibc busybox
  applet running under the trap. The committed musl test `linuxargs` echoes its
  argc/argv.
- **ramfs**: the 9P bundle is read-only, so files a program creates or writes go
  to a small in-memory overlay (`linuxelf.c`), keyed by path, buffers kmalloc'd
  and grown by doubling. A write intent or `O_CREAT` opens the ramfs; a
  read-only open checks the ramfs first, then the bundle, so a written file
  reads back. `read`/`write`/`lseek`/`fstat`/`stat`/`dup`/`close` handle ramfs
  fds; `O_TRUNC`/`O_APPEND` are honoured. The committed musl test `linuxwrite`
  creates `/tmp/scratch.txt`, writes to it, stats it, and reads it back.

Not yet: `busybox ls /` wants `statx` (332), the glibc stat variant, which we do
not service. Applets that fork/exec (a shell pipeline) need `clone`/`execve`
(M16.4 and beyond). The ramfs is per-run and single trust domain, not a real
filesystem; it is the shortcut its name implies.
