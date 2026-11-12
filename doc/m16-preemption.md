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

- Linux user code runs with interrupts enabled (`linux_jump` builds the entry
  frame with `rflags` 0x202; forks and threads inherit it through the trapframe).
- The whole libOS kernel runs with interrupts masked. The initial `rflags` for a
  libOS task is 0x2 (IF clear, `stack_init`), and nothing in the kernel ever runs
  `sti`, so the scheduler, every context switch, demand paging, and the syscall
  handlers are all non-preemptible. The event vector is therefore delivered only
  while a Linux process is in user mode (IF set); it never fires in ring 0.

The timer preempts a Linux user loop and nothing else. Because the kernel is
always interrupts-masked on a single vcpu, it is one big critical section and
needs no new locking. The ISR still checks the saved CS and reschedules only for
a ring-3 interrupt; a stray ring-0 delivery would merely run the device hooks and
return. This replaced an earlier attempt to run kernel tasks preemptibly: a tick
landing in a ring-0 kernel compute loop corrupted the guest and raced the
scheduler's own non-locked windows, so the kernel is kept strictly cooperative.

## The mechanism (as built)

1. In-kernel APIC. The host gives the VM an in-kernel irqchip
   (`KVM_CREATE_IRQCHIP`, preceded by `KVM_SET_TSS_ADDR`), so KVM owns a per-vcpu
   local APIC and delivers interrupts to a running vcpu itself. The preemption
   tick and the device-event rouse both go out as a message-signalled interrupt
   (`KVM_SIGNAL_MSI`, address `0xFEE00000 | apic_id<<12`, data = vector 0x20);
   KVM routes it to the vcpu's APIC and kicks it out of guest mode to take it. No
   userspace injection, no signal, so nothing races `KVM_RUN`. The host timer
   thread still keeps the wall-clock tick counter in the boot block (P1/P2);
   unless `JOTUN_NO_PREEMPT` is set it also sends the MSI each tick.
2. Guest APIC in x2APIC mode. `arch_apic_init` enables the local APIC through the
   `IA32_APIC_BASE` MSR and the spurious-vector register, all by `wrmsr`, so a
   freestanding guest needs no APIC MMIO mapping. The ISR ends with an x2APIC EOI
   (`wrmsr 0x80B`) so the APIC will deliver the next tick.
3. Per-thread `rsp0`. A ring 3 to ring 0 interrupt lands on `TSS.rsp0`.
   `linux_on_resume` (and the main thread's first entry) set it to the running
   thread's `kstack_top`, so the preemption frame lands on that thread's own
   kernel stack, which makes the reschedule reentrant across threads. The stack is
   idle while the thread is in user mode (the syscall path, which also uses it,
   runs with interrupts masked and cannot be preempted).
4. The ISR (`isr_event`) is shared with device events. It saves the full register
   and FPU state, passes the interrupted CS's privilege level to `irq_event`,
   sends EOI, and returns. `irq_event` always runs the device hooks; for a ring-3
   interrupt it then `task_yield`s, parking the thread inside the ISR on its own
   kernel stack. When the scheduler resumes it, it returns up through the ISR and
   `iretq`s back to the user rip. A ring-0 interrupt returns without rescheduling.

Shutdown note: with the in-kernel APIC, KVM services `HLT` itself and never exits
to the host, so the guest signals a clean stop with a store to `JOTUN_POWEROFF`
rather than halting.

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
| P5 | Real sleeps and timers: `nanosleep` and `clock_nanosleep` block until a deadline tick (done); `setitimer`, `alarm`, and `timerfd` (deferred, need caught-signal delivery). Feeds signal stage F. | medium |

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
- Injection (the actual preemption) is on by default now; `JOTUN_NO_PREEMPT`
  disables it. The clock needs only the tick counter, so `JOTUN_HZ` with
  `JOTUN_NO_PREEMPT` gives a Linux guest a real clock with no injection and no
  reschedule. By default the timer also injects the vector, which preempts a
  running Linux user loop (P4 made this safe for Linux threads: the vector is
  delivered only on a return to ring 3, never inside a kernel critical section).

Proof: example/linuxtime.c, run as `JOTUN_HZ=1000 make linux-spike
SPIKE_PROG=linuxtime`. All 16 spikes stay green, arm64 cross-compiles clean.

## What P4 debugging surfaced (resolved)

The old injection crash (a page fault to a garbage rip with a trashed stack, and
a livelock at high rates) turned out to be host-side, not a guest interrupt-frame
problem. The host preempted a running vcpu by sending SIGUSR1 to break `KVM_RUN`
(EINTR) and then injecting the vector with `KVM_INTERRUPT` (no in-kernel APIC).
That signal-then-inject sequence intermittently corrupted the guest: even a
no-op `iretq`-only ISR crashed the same way, and disabling the SIGUSR1 kick made
it run cleanly. `KVM_SET_SIGNAL_MASK` removed the corruption but then starved the
guest. The fix was to stop injecting from userspace entirely and let the
in-kernel APIC deliver the vector (see "The mechanism"). The two guest-side
pieces still mattered, but as correctness, not as the crash fix: the kernel must
be strictly non-preemptible (IF clear in ring 0), and a ring-3 preemption must
land on the thread's own `rsp0` stack.

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
- P4 (preempt ring 3 safely): done. The host delivers the event vector through an
  in-kernel APIC (`KVM_CREATE_IRQCHIP` + `KVM_SIGNAL_MSI`); the guest enables its
  x2APIC and EOIs in the ISR. Linux user code runs with IF set (`linux_jump`
  0x202), the libOS kernel runs strictly with IF clear (`stack_init` 0x2, no
  `sti`), so ticks fire only in ring 3. `linux_on_resume` arms the per-thread
  `rsp0`, and `irq_event` reschedules only ring-3 interrupts. Shutdown is a
  `JOTUN_POWEROFF` store, since the in-kernel APIC keeps HLT from reaching the
  host. Validated: example/linuxpreempt.c (a user thread spinning on a flag that
  only another user thread can set) hangs without preemption and prints
  "preempt ok" with it; the full spike suite is green with and without preemption
  at HZ up to 1000; the clock still passes; the spin demo now runs cooperatively;
  arm64 cross-compiles clean.
- P5 (real sleeps and timers): blocking sleeps done; interval timers deferred.
  `nanosleep` and `clock_nanosleep` (relative and `TIMER_ABSTIME`, CLOCK_MONOTONIC
  and CLOCK_REALTIME) now really block. A thread records a deadline tick in
  `lproc->sleep_deadline` and parks with `lx_block` (dropping the giant lock); a
  `sleep_wake_hook` registered as an event hook wakes due sleepers, running both
  on ring-3 ticks and at scheduler idle. The host's `run_timer` rouses each idle
  vcpu once per tick, so a fully-asleep guest still wakes at its deadline rather
  than only when some other event arrives. Each sleep calls `signal_killpoint`
  after the block, so a fatal signal cuts it short. Validated: example/linuxsleep.c
  (a 100 ms `nanosleep` and an 80 ms absolute `clock_nanosleep`, checked against
  the monotonic clock) prints "nanosleep ok" and "abssleep ok" at HZ 100 and 1000,
  returns at once with no timer, and the full suite stays green with and without
  preemption; arm64 cross-compiles clean.
  Deferred: `setitimer`, `alarm`, and `timerfd`. Caught-signal delivery (signal
  stage D) has since landed, so `SIGALRM` can now reach a handler; what these
  still need is a per-process interval-timer source armed off the host tick and,
  for `timerfd`, the readable-fd/poll wiring. They are a separate increment (see
  doc/m16-signals.md, stage F).
