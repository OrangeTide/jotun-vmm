# Bringing up the AArch64 guest: a runtime test plan

The guest's arm64 backend is written and cross-compiles clean, but it has never
run. Every result in the M16 work (the native libOS demos, the Linux-compat
path, dynamic glibc, the toolchain, autotools) was produced on linux/x86-64
under KVM. The arm64 side has only ever been compiled, not executed.

This is the plan to change that: a staged bring-up that starts at "does it
boot" and walks up to the full Linux-compat path, with the expected result at
each step and the arm64-specific things to watch. It is written to be run by
someone with real AArch64 hardware (a linux/arm64 box with KVM, or an Apple
Silicon Mac with Hypervisor.framework), which the original author's machine is
not.

## What is already built for arm64

Shared, arch-neutral code (the scheduler, the heap allocator, the 9P client,
the ELF loader, and all of `linuxelf.c`: the syscall dispatcher, fork, threads,
the overlay, exec) compiles for both arches from the same sources. The arm64
backend supplies the machine-specific half:

- `arch/arm64_boot.S`, `arch/arm64.c`: entry, trap-vector install, fault report.
- `arch/arm64_ctx.S`: task context switch.
- `arch/arm64_linuxsys.S`: the SVC trap and the two user-entry paths.
- `arch/arm64_vmspace.{c,h}`: ARMv8 translation tables, the frame allocator, the
  MMU enable, per-process address spaces, and demand paging.
- `arch/arm64_linuxabi.h`: the asm-generic syscall numbers and the trapframe.

The address space matches x86 after the kernel-heap work: 128 MiB RAM, a
per-process user window at `[0x400000, 0x7000000)`, an 8 MiB kernel-heap
bootstrap band above it at `[0x7000000, 0x7800000)`, the vcpu stacks in the 8
MiB above that, and a physmap of all RAM at 1 GiB. Once the Linux-compat path is
up, the heap also grows on demand into a window at 2 GiB, a shared page
directory that `vmspace_new` installs in every process. The guest boots with the
MMU off and turns it on at the first address-space switch; the growth window is
reachable only after that switch, which is why the band carries the boot.

## Hosts and prerequisites

- **linux/arm64 (KVM).** Build natively: a plain `make` on the arm64 box selects
  the aarch64 guest and builds `jotun` and `libos.elf` with the host gcc. For the
  Linux-compat spikes you also need `aarch64-linux-musl-gcc` on `PATH`.
- **macos/arm64 (Hypervisor.framework).** `make` selects the HVF host and the
  aarch64 guest, built with `clang` and `lld` (`brew install lld`). A musl
  cross-compiler for the spikes is usually not available here; use the
  pre-built-binary path (below) instead.

The kernel can also be cross-built from an x86 box to inspect it without running:
`make build/img/aarch64/kernel` (needs `clang` plus `lld`). That is a
compile-and-link check only, not a run.

## Phase A: boot and the native libOS

The first question is whether the guest reaches `kmain`, enables the MMU, and
runs the scheduler at all. None of this touches the Linux-compat path.

1. `./jotun libos.elf rc` (or any init; `rc` runs the shell script path).
   Expect the console banner `libos up`, then `heap ok` from the heap self-test,
   then the boot parameter dump (`boot: ram ... bytes, argc ...`).
2. `./jotun libos.elf spin`: two CPU-bound tasks. Their letters should
   interleave (timer preemption) on one vcpu, or run truly parallel with
   `JOTUN_VCPUS=2`.
3. `./jotun libos.elf smp`: the locked-counter test. Expect `smp: counter 200000
   ok`, evidence the spinlock and shared memory behave across vcpus.
4. `./jotun libos.elf block`: the sleep/wake handoff. Expect `waiter: woken`.

`heap ok` is the first direct check of the kernel-heap change on arm64: the heap
now lives in the band at `0x7000000`, written with the MMU off at boot and read
back through the identity map after the MMU comes on. If the self-test fails or
faults, suspect the band placement or the physmap, not the allocator.

## Phase B: the address-space and heap change

These confirm the 128 MiB layout this milestone introduced on arm64 (it was a
32 MiB guest before).

1. **Physmap reach.** Any workload that allocates enough to use high frames
   exercises the enlarged physmap (64 blocks, all 128 MiB, up from 16). A failure
   here shows as a fault touching a frame above 32 MiB. The draw/pixel demos
   (`./jotun libos.elf pixel`) allocate a framebuffer and are a quick exercise.
2. **Stack/heap/device adjacency.** RAM now tops at `0x8000000` (128 MiB), which
   is exactly where the guest marks the device band. The real MMIO sits higher
   (UART at `0x9000000`, doorbells at `0xa000000`), so the top of RAM and the
   devices do not overlap, but the vcpu stacks now sit just below the device-typed
   region. Run with the maximum vcpus (`JOTUN_VCPUS=8`) and confirm no stack
   access faults into the device band.
3. **The heap band under load.** Any Phase C/D test that writes files into the
   overlay exercises the band as the real heap. Watch for an out-of-memory report
   from `linux_tables_init` or a fault in `[0x7000000, 0x7800000)`.
4. **The growth window.** A test that writes more than the 8 MiB band into the
   overlay (the x86 side uses a 36 MiB write/verify stress) forces the heap to
   grow into the window at 2 GiB. On arm64 the window L2 is installed by
   `vmspace_new` and the grower activates at the first switch, neither of which
   has ever run; a fault whose `far` lands at or above `0x80000000` points here,
   most likely a missing or wrong `L1[2]` install or a stale table walk needing
   the `dsb ish; isb` the grower issues.

## Phase C: Linux-compat, single process

The static musl spikes in `example/linux*.c` are the acceptance set. Build and
run them with the harness:

```
make linux-spike SPIKE_PROG=linuxhello    # needs aarch64-linux-musl-gcc
```

Or, without a musl cross-compiler (typically macos/arm64), build the `linux*`
binaries elsewhere for aarch64 (static, non-PIE, ET_EXEC linked at `0x400000`),
drop them in a directory, and point the harness at it:

```
make linux-spike-prebuilt SPIKE_BIN_DIR=/path/to/prebuilt SPIKE_PROG=linuxhello
```

Run the single-process spikes first, in rough order of surface area:

- `linuxhello`: entry, argv/auxv, write, exit. The smoke test.
- `linuxargs`, `linuxwrite`, `linuxcat`: argv delivery and file reads.
- `linuxmem`, `linuxmmap`: the heap arena and file-backed mmap (watch the 6th
  mmap argument, which arm64 passes in `x5` via `tf_arg6`).
- `linuxcwd`: chdir/getcwd and relative-path resolution.

Each should print its expected line and reach `libos halting` with no
`unhandled syscall`, no `#UD`/abort, and no fault report. These lean hardest on
the SVC trap path, the trapframe layout (280 bytes: `x[31]`, `sp`, `pc`,
`pstate`, `tpidr`), and TLS living in `TPIDR_EL0`.

## Phase D: fork, pipes, threads

This is the highest-risk area. The fork/pipe/thread logic in `linuxelf.c` is
arch-neutral and the arm64 backend supplies what it needs (`vmspace_clone`,
`tf_set_ret0`, `tf_set_sp`, `tf_set_tls`, the resume hook), but that combination
has never executed on arm64. The early arm64 bring-up note called these the
deferred pieces; treat them as unverified rather than known-good.

- `linuxfork`: fork, wait4, and a parent/child pipe. Confirm the child resumes
  with `x0 == 0` and the parent reaps the right status.
- `linuxpipe`: a pipe read/write round trip within one process.
- `linuxexec`, `linuxexecram`: execve of a bundle binary and of one built into
  the overlay. Exec rebuilds the address space, so a bug here tends to fault in
  the loader or on the first user instruction after `eret`.
- `linuxthread`: clone with `CLONE_VM`, a shared counter under a mutex, and
  join. Exercises the futex wait queue and the thread kstack handoff.

Likely failure modes to keep in mind: a wrong trapframe offset surfaces as the
child or thread resuming at the wrong `pc` or with a clobbered `sp`; a TLS bug
shows up once a thread dereferences thread-local state; a context-switch bug in
`arm64_ctx.S` shows as corruption only when more than one task runs.

## Phase E: dynamic glibc and real tools

The end state x86 reached: run unmodified host arm64 binaries (a dynamic glibc
`/usr/bin/sed`, `gawk`, `make`, `bash`, and ultimately gcc and an autotools
`./configure && make`) with their libraries served from live host mounts. This
needs an aarch64 host filesystem to mount and almost certainly uncovers more
arm64 work (the dynamic linker path, `posix_spawn` via `clone3` with
`CLONE_VM|CLONE_VFORK`, the per-process cwd across exec). Approach it the way the
x86 path was built up in `doc/dynamic-binaries.md`: a dynamic musl hello first, then a
single glibc tool, then a pipeline, then a build. Treat Phase E as the research
frontier, not a checklist.

## Capturing and reading failures

Run under a timeout and redirect the whole invocation to a log, then read from
the boot banner onward, the same as the x86 workflow:

```
timeout 60 ./jotun libos.elf <init...> > run.log 2>&1
sed -n '/entering guest/,$p' run.log
```

On an unrecoverable fault the arm64 backend prints from `arm64_fault_report`:
the exception syndrome (`esr`), the faulting address (`far`), and the return
address (`elr`), then halts. For a translation fault, decode the `far` against
the layout: an address in `[0x400000, 0x7000000)` is the user window (a genuine
demand-page miss that `pf_handle` should have serviced), one in `[0x7000000,
0x7800000)` is the kernel heap band, and anything at or above `0x8000000` is the
device-typed region. `esr` bits `[5:0]` (the DFSC) distinguish a translation
fault (`0x04`-`0x07`, demand-pageable) from a permission or alignment fault.

## Scope note

This plan verifies the arm64 backend against what the shared code already does
on x86. It is not a plan to add arm64 features; where a phase fails because a
piece was never implemented for arm64 (most likely somewhere in Phase D or E),
that becomes its own task. Phases A through C should pass or reveal small,
localized bugs; D and E are where real arm64 work probably remains.
