# Preemption in the Linux-compat guest

This is the design of record for adding preemption to the kernel. Today Linux
user code runs with interrupts masked and nothing preempts it, so a compute loop
that makes no syscalls starves every other task, and an asynchronous signal
cannot reach a running loop. Preemption lifts that one ceiling.

## What it unlocks

- Fairness. A CPU-bound Linux thread or process no longer starves its peers.
  Today a `while (1) {}` with no syscalls hangs the whole guest.
- Asynchronous signals. This completes the signal work (stage F and the
  asynchronous half of C and D): a signal reaches a running loop, and
  `SIGALRM`/`setitimer` become possible. See doc/m16-signals.md.
- Real time. A timer gives a measured time base, replacing the assumed 1 GHz
  cycle-counter approximation in `clock_gettime`, `time`, and `gettimeofday`, and
  enabling real `nanosleep` and `timerfd`.

## What the code already provides

- An IDT and a per-cpu TSS (arch/x86_64.c) with `rsp0` and IST stacks already
  wired for the fault path. Adding a timer vector is one `idt_set` call.
- A context switch (`ctx_switch`) that saves `rflags` and the six callee-saved
  registers, and a resume hook (`linux_on_resume`) that already sets `lproc_cur`,
  CR3, and the FS base on every Linux-thread switch.
- SSE enabled (CR0.MP, CR4.OSFXSR) and a syscall path that already saves the full
  user trapframe into `lproc_cur->tf`. The timer ISR mirrors that save.
- No device interrupts yet. The timer is the first, so this also means enabling
  the local APIC and masking the legacy PIC.

## The one hard constraint

The scheduler is cooperative on a single Linux vcpu, and the kernel's shared
tables are protected only by the giant lock held across each Linux syscall, with
the syscall path running with interrupts masked. The design keeps that intact.

## Core design: preempt ring 3 only, keep the kernel cooperative

- Linux user code runs with interrupts enabled (the initial `rflags` becomes
  0x202 in `linux_jump` and in the fork and thread initial frames).
- Everything in ring 0 stays non-preemptible. The syscall path keeps interrupts
  masked (SFMASK already does this), so the timer never fires mid-syscall and the
  giant lock is never interrupted. The libOS kernel tasks already run with
  interrupts enabled, but the ISR declines to reschedule when it interrupts ring
  0 (it checks the saved CS privilege level, as the fault stub does): there it
  only does timekeeping and sends EOI.

The timer therefore preempts a Linux user loop, while the kernel, the syscall
handlers, demand paging, and the cooperative libOS tasks are untouched. No new
locking is needed, because on a single vcpu the interrupts-masked kernel is
already a critical section.

## The mechanism

1. Timer source. Map the local APIC page (0xFEE00000, unmapped today) into the
   kernel's device region and run the APIC timer in periodic mode at about 100 to
   1000 Hz. The PIT and 8259 by port I/O are the fallback if the APIC is awkward
   under this KVM configuration. Set the spurious-interrupt vector (software
   enable) and the timer vector (0x20), and mask the legacy PIC.
2. Per-thread `rsp0`. A ring 3 to ring 0 interrupt lands on `TSS.rsp0`. For the
   ISR to reschedule safely it must run on the preempted thread's own kernel
   stack, not the shared `rsp0_stack`. So `linux_on_resume` also sets
   `cpu_tss[cpu].rsp0 = lproc_cur->kstack_top`. This is safe because that stack is
   idle while the thread runs in user mode (a syscall, which also uses it, runs
   with interrupts masked and cannot be preempted).
3. The ISR, a new assembly stub parallel to `linux_syscall_entry`. If the
   interrupt came from ring 3, save the user registers into `lproc_cur->tf` (the
   user rip, rsp, and rflags come from the CPU's interrupt frame), send EOI, call
   `preempt_tick()`, then jump to `restore_tf` (reuse the existing syscall-return
   path, which returns to user with interrupts enabled). If it came from ring 0,
   send EOI, advance the tick, and return.
4. `preempt_tick()` advances the monotonic tick, runs any due timers (a later
   stage), and if the quantum has expired marks the current task ready and
   `ctx_switch`es to the scheduler, exactly as `task_yield` does. The thread parks
   inside the ISR; when it is rescheduled it returns up through the ISR and
   returns to its later user rip. A thread parked in a syscall, in a cooperative
   yield, or in this ISR all resume uniformly.

## The one large hidden cost: FPU and vector state

Cooperative switches happen only at syscall boundaries, where the SysV ABI
treats every `xmm` and `ymm` register as dead, so `ctx_switch` saving no FPU
state is correct today. Preemption switches mid-instruction-stream, where those
registers are live, and the host toolchain and V8 use SSE and AVX heavily. So a
preemptive switch between two user threads must save and restore the full FPU and
vector state.

Add a per-thread save area in `struct lproc` and `FXSAVE`/`FXRSTOR` (512 bytes),
or `XSAVE`/`XRSTOR` with CR4.OSXSAVE and XCR0 set if AVX is in use (it is for the
host toolchain, so plan on XSAVE and a 1 to 2.5 KB area). Eager save on a
user-to-user preemptive switch is the simpler and correct first cut; a lazy
scheme (CR0.TS and `#NM`) is an optimization for later. This is the single
largest piece of new work and the main correctness risk.

## Real time without losing ticks

The cycle counter keeps running regardless of the interrupt flag, so the
accurate clock is not the tick count (which stalls during a long interrupts-off
stretch) but the cycle counter divided by its measured frequency. The timer's
known period calibrates that frequency: count the cycle-counter delta over a
fixed number of timer ticks, then report time as the epoch plus
`cycles / measured_hz`. This fixes the assumed 1 GHz constant and stays accurate
even during an interrupts-masked user loop.

## Staged plan, by effort

| Stage | Scope | Effort |
|-------|-------|--------|
| P1 | Timer and ISR, timekeeping only: map the APIC, periodic timer, vector 0x20, an ISR that advances a tick and sends EOI (ring 0 and ring 3 both just return). User stays interrupts-masked. Proves the plumbing with no reschedule risk. | medium |
| P2 | Real clock: calibrate the cycle-counter frequency against the timer period and report measured time from `clock_gettime`, `time`, and `gettimeofday`. No preemption yet. | low |
| P3 | FPU and vector state on the switch path: a per-thread save area, saved and restored on a preemptive switch. Land this before P4 so the first preemptive switch is FPU-safe. | medium to high |
| P4 | Preempt ring 3: user runs interrupts-enabled, `linux_on_resume` sets per-thread `rsp0`, the ISR reschedules on quantum expiry for ring-3 interrupts only. The core. | medium |
| P5 | Real sleeps and timers: `nanosleep` and `clock_nanosleep` block until a deadline tick; `setitimer`, `alarm`, and `timerfd`. Feeds signal stage F. | medium |

P1 and P2 are independently shippable and low risk. P3 is the prerequisite that
makes P4 correct. P4 is the keystone.

## Risks and testing

- FPU corruption if P3 is wrong: wrong floating-point or vector results in a
  preempted computation. Test an SSE and AVX heavy user loop that is repeatedly
  preempted and checks its own results.
- Nondeterminism: preemption surfaces any latent race. Linux-visible state is
  already serialized by the giant lock and the kernel stays non-preemptible, so
  the exposure is small, but the spike suite should run repeatedly to shake out
  ordering assumptions.
- A spinning child becomes killable, the exact gap from the signal work: a child
  in a no-syscall loop that the parent kills hangs today and should terminate
  with P4.
- `rsp0` discipline: a mistake here triple-faults. The invariant (the kstack is
  idle during user execution) must hold, and the resume hook is the only writer.

## arm64 parallel track

Mirror the design with the generic timer (`CNTV_CTL_EL0` and `CNTV_TVAL_EL0`,
system registers with no MMIO map) and a GIC for routing, an IRQ vector in the
VBAR table (only the synchronous vector is handled today), and running the EL1t
"user" context with IRQ unmasked. The arm64 port is untested, so build it to
parity and validate when arm64 hardware is in play.

## What P1 and P2 actually built

The host already drives a periodic timer (JOTUN_HZ) that injects the event
vector, and the guest's `isr_event` already saves the FPU and can reschedule, so
the plumbing largely existed. P1 and P2 added a real clock on top of it and
separated the clock from preemption:

- The host timer thread increments a monotonic tick counter in the boot block
  (`timer_ticks`, with `timer_hz`) once per tick in wall time, independent of the
  guest's interrupt state (host/main.c `run_timer`). The boot block version is
  now 3.
- The guest reads it (`boot_timer_ticks`, `boot_timer_hz`) and `guest_mono_ns`
  returns nanoseconds from it. `clock_gettime` (realtime and monotonic),
  `gettimeofday`, and `time` now report measured time; file mtimes share the same
  source. Without a timer they fall back to the cycle counter as before.
- Injection (the actual preemption) is now opt-in behind `JOTUN_PREEMPT`. The
  clock needs only the tick counter, so `JOTUN_HZ` alone gives a Linux guest a
  real clock with no injection and no reschedule. `JOTUN_PREEMPT` additionally
  injects the vector, which preempts plain kernel tasks (the spin demo), and is
  the switch stage P4 will make safe for Linux threads.

Proof: example/linuxtime.c, run as `JOTUN_HZ=1000 make linux-spike
SPIKE_PROG=linuxtime`. All 16 spikes stay green, arm64 cross-compiles clean.

## What P4 debugging already surfaced

Injecting the event vector into a guest that is executing a Linux process
crashes or hangs it today (a page fault to a garbage rip with a trashed stack
during Linux loading, and a livelock at high rates during the 9P-backed load).
So preempting the Linux path is not a flip of the user interrupt flag; it needs
the interrupt-context work this document describes (per-thread `rsp0`, the FPU
save that `isr_event` already has, and a reschedule that is safe under the
per-process CR3). As an interim guard, `irq_event` already declines to
reschedule a task that carries a proc (a Linux thread), so kernel-task
preemption is unaffected. This is the crux of P4.

## Status

- P1 (timer tick and clock plumbing): done.
- P2 (real clock from the tick): done.
- P3 (FPU and vector state on the switch path): done. `arch_fpu_init` now
  enables XSAVE and AVX (CR4.OSXSAVE, XCR0 = x87 + SSE + AVX, guarded by CPUID,
  AVX-512 left out to bound the area), so a Linux program's glibc uses its AVX
  code paths. `isr_event` saves and restores the wider state with XSAVE/XRSTOR
  when it is enabled (fpu_use_xsave), falling back to FXSAVE otherwise, from a
  fixed 1024-byte 64-byte-aligned area that x87 + SSE + AVX (832 bytes) fits.
  Validated: the glibc gcc toolchain compiles and links end to end with AVX on
  (AVX2 routines run with no #UD), and kernel-task preemption (the spin demo
  under JOTUN_PREEMPT) runs through the XSAVE path without fault. A strict "AVX
  survives a preemptive switch" test needs an AVX Linux program to be preempted,
  which arrives with P4.
- P4 (preempt ring 3 safely): planned; the injection crash above is the first
  thing it must fix.
- P5 (real sleeps and timers): planned.
