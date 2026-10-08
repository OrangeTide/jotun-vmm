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
