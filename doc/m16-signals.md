# Signals in the Linux-compat guest

This is the design of record for signals in the `bin` guest (guest/libos/linuxelf.c).
It covers the model, the one hard constraint, the staged plan, and what has
landed so far.

## The substrate

The pieces signals need already exist:

- A single return choke point. Every path back to CPL 3 (`restore_tf` in
  arch/x86_64_linuxsys.S) rebuilds the IRETQ frame from `lproc_cur->tf`. That is
  the one place a thread can be diverted into a handler.
- A saved per-thread trapframe (`struct lproc.tf`): the user register set,
  including the user rip and rsp. Exactly the context a signal frame and
  `sigreturn` need.
- Blocking points that drop the giant lock (`lx_block`/`lx_yield` in pipe read
  and write, poll, wait4, futex) plus `task_wake(id)`. The hook for interrupting
  a parked thread.
- A CPU fault path (`linux_fault_entry`, reached from arch/x86_64_fault.S) that
  routes a CPL-3 fault to a handler or, failing that, to a "terminated by signal
  N" that `wait4` reports as `WIFSIGNALED` through `sh->exit_sig`.

## The one hard constraint

User code runs with interrupts off and is never preempted (the scheduler is
cooperative, user rflags has IF clear by design). The kernel only regains
control from a running user thread at a `SYSCALL` or a CPU fault. There is no
timer reaching CPL 3.

Consequences:

- Delivery is only possible at syscall-return and fault boundaries. A signal to
  a thread blocked in a syscall is fine (wake it, it returns through the
  boundary). A signal to the currently running thread is fine (it is mid-syscall
  by definition).
- A pure compute loop that makes no syscalls cannot be interrupted. This is the
  real ceiling. It rules out faithful asynchronous `SIGALRM` against a
  CPU-bound loop without adding a preemption timer. Syscall-gated alarms (check a
  deadline at each boundary) are possible and cover "time out a blocking read",
  not "interrupt a tight loop".
- There is no asynchronous console input, so interactive `SIGINT` (Ctrl-C) has
  no source yet.

This is acceptable for the build and sandbox goal: `SIGCHLD` reaping, `SIGPIPE`,
catchable `SIGSEGV` for a GC or JIT guard page, and a tool's own handlers all
fire at syscall or fault boundaries, which is where these programs already are.

## State model

- Per process (`struct lshared`): `sigact[NSIG]` (`handler`, `flags`,
  `restorer`, `mask`) and a process-directed `sig_pending` bitmask. Inherited
  across fork; caught dispositions reset to default on execve (ignored kept);
  the mask and pending set are preserved.
- Per thread (`struct lproc`): the blocked `sig_blocked` mask (inherited on
  clone), a thread-directed `sig_pending` (from tkill/tgkill), and the alternate
  stack (used when an SA_ONSTACK handler is delivered). This matches Linux:
  dispositions shared, mask and thread-pending per thread.

Signal sets are 64-bit (bit `s-1` for signal `s`), which is the x86-64 kernel
`sigset_t` the `rt_*` syscalls pass.

## Staged plan

| Stage | Scope | Status |
|-------|-------|--------|
| A | Real disposition and mask state (`rt_sigaction`, `rt_sigprocmask`, `sigaltstack`) | done |
| B | Generation (`kill`, `tkill`, `tgkill`) and the default terminate action | done |
| C | Blocking syscalls notice a fatal signal and terminate (killable while blocked) | done |
| D | Handler delivery: build a frame on the user stack, divert the return, `rt_sigreturn`, `SA_RESTART`, userspace `EINTR` | done |
| E | Catchable faults: save the user context in the fault stub so `SIGSEGV`/`SIGFPE`/`SIGILL` reach a handler | done |
| F | Timers: syscall-gated `alarm`/`setitimer` -> `SIGALRM`, `timerfd` (coarse, boundary-checked) | planned |

## What stages A to C do

- Dispositions and masks are real. `rt_sigaction` stores the kernel
  `struct sigaction` ({handler, flags, restorer, mask}); `rt_sigprocmask`
  maintains the per-thread mask and refuses to block SIGKILL or SIGSTOP;
  `sigaltstack` records the alternate stack for stage D.
- `kill`/`tkill`/`tgkill` post a pending signal and `task_wake` the target so a
  blocked thread re-checks. `kill` supports pid>0 (group forms are a no-op),
  and signal 0 is an existence probe (ESRCH if the pid is absent).
- `take_fatal_signal` computes `(pending & ~blocked)`, adds SIGKILL
  unconditionally, consumes any ignored signal (SIG_IGN or a default-ignore
  disposition), leaves a caught signal pending for `deliver_signal`, and returns the
  lowest signal whose action is to terminate. `signal_killpoint` acts on it by
  terminating the process; it runs at the syscall boundary (in
  `linux_sys_dispatch`) and after each cooperative sleep in the blocking loops,
  so a thread blocked in read, write, poll, wait4, or futex is killable.
- The default action table treats the stop signals as ignored (there is no stop
  state). `SIGPIPE`'s default terminate still runs through the broken-pipe write
  path, now reading the disposition from `sigact` rather than a separate flag.

Proof: example/linuxsignal.c. It kills a child blocked in read with SIGTERM
(reported WIFSIGNALED), confirms abort self-terminates with SIGABRT, confirms
SIGKILL defeats a full signal block, confirms a default-ignore signal (SIGURG)
does not kill, confirms a blocked SIGTERM is held off so the child exits
normally, and checks the existence probe.

## What stage D does

Handler delivery turns a caught signal into a real handler call. It happens at
the syscall-return boundary, in `linux_sys_dispatch`, right after the
fatal-signal check:

- `maybe_deliver_signal` picks the lowest pending, unblocked signal with a real
  handler (`next_caught_signal`) and calls `deliver_signal`.
- `deliver_signal` builds an `rt_sigframe` on the user stack (the alternate
  stack if `SA_ONSTACK` and one is installed), saving the interrupted trapframe
  into the frame's `sigcontext` and re-pointing `lproc_cur->tf` at the handler
  with `handler(sig, &info, &uc)` and a clean rflags. The return path
  (`restore_tf`) then enters the handler instead of the interrupted code. It
  masks the signal (and the handler's `sa_mask`) for the duration unless
  `SA_NODEFER`, and resets the disposition to `SIG_DFL` first if `SA_RESETHAND`.
- The frame's return address is the handler's restorer (`sa_restorer`, which
  glibc and musl both supply on x86-64; AArch64 has no `SA_RESTORER`, so a short
  trampoline is written into the frame). The restorer calls `rt_sigreturn`, and
  `do_rt_sigreturn` reads the frame back, restores the trapframe and the saved
  mask, and resumes the interrupted code.
- Userspace `EINTR` works: the blocking loops (pipe read/write, poll, wait4,
  futex, timed sleep, pause) return the internal `LERR_RESTARTSYS` marker when a
  caught signal is ready. `maybe_deliver_signal` then either restarts the trapped
  syscall (an `SA_RESTART` handler: the saved rip is rolled back to the `SYSCALL`
  and the saved rax to the call number) or reports `EINTR`.
- The floating-point state is not saved in the frame. Delivery is only at a
  syscall boundary, where the SysV ABI already treats the vector registers as
  call-clobbered, so no live vector state crosses it. Delivering at an arbitrary
  preemption point would need the FP save the preemption ISR already performs.

The arch-specific frame assembly (the `sigcontext`/`ucontext`/`siginfo` layout
and the build/restore helpers) is in arch/x86_64_linuxabi.h and
arch/arm64_linuxabi.h; the selection, masking, and flag logic is arch-neutral in
linuxelf.c. The AArch64 path is structurally complete but not yet exercised at
runtime.

Proof: example/linuxsighandler.c. It confirms a plain handler runs on `raise`
and the signal is masked inside it, an `SA_SIGINFO` handler reads the siginfo, an
`SA_ONSTACK` handler runs on the alternate stack, `SA_RESETHAND` resets the
disposition after one delivery, a handler interrupts a blocked read as `EINTR`,
`SA_RESTART` restarts the interrupted read instead, and `pause` returns `EINTR`
after the handler runs.

## What stage E does

Catchable faults route a synchronous CPU exception to a handler. The x86-64 fault
stub (arch/x86_64_fault.S) already redirected a CPL-3 fault's IRETQ into a ring-0
routine on the faulting process's handler stack. It now first copies the full
faulting user register set (the 15 GP registers, the user rip, rsp, and rflags)
from the fault frame into `lproc_cur->tf`, before the redirect reuses that stack,
and passes the trap vector and `cr2` to `linux_fault_entry`.

`linux_fault_entry` maps the vector to a signal and a fault `si_code`
(`#DE`->SIGFPE, `#UD`->SIGILL, `#PF`/`#GP`/other->SIGSEGV, with `cr2` as the
`si_addr` for a page fault). If a handler is installed for that signal and it is
not blocked, `deliver_signal` builds the handler frame from the saved context
(the fault `si_code` and address go into the siginfo) and `linux_resume` enters
the handler. Otherwise the fault is forced to terminate: a synchronous fault that
is ignored, defaulted, or blocked cannot be resumed past, so the process exits
with the signal (reported `WIFSIGNALED`), as before. A handler that returns
normally re-runs the faulting instruction, matching Linux; a recovering handler
uses `siglongjmp` or exits.

This is x86-64 only. The AArch64 fault path does not route to a handler yet (it
never called the old terminate routine either).

Proof: example/linuxfault.c. A SIGSEGV handler runs on an out-of-window access
and reads the fault address from the siginfo, a SIGFPE handler catches an integer
divide by zero, a SIGILL handler catches an illegal instruction (all recovering
with `siglongjmp`), and an uncaught fault still terminates the process with
SIGSEGV.

## What stages D and E deliberately do not do

- No `SIGCHLD` is generated on child exit, and `rt_sigsuspend` is not
  implemented.
- Thread-group signal semantics are minimal: a process-directed signal is
  delivered to whichever thread next reaches a boundary, not steered to one that
  has it unblocked.
- The AArch64 handler-delivery and fault-routing paths are not exercised (the
  frame builder compiles, but the arm64 guest is not run).
