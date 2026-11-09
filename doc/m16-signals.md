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
- Partial state already: `sh->exit_sig` and `linux_fault_kill` map a fatal fault
  to a "terminated by signal N" that `wait4` reports as `WIFSIGNALED`.

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
  stack (stored now, used by stage D). This matches Linux: dispositions shared,
  mask and thread-pending per thread.

Signal sets are 64-bit (bit `s-1` for signal `s`), which is the x86-64 kernel
`sigset_t` the `rt_*` syscalls pass.

## Staged plan

| Stage | Scope | Status |
|-------|-------|--------|
| A | Real disposition and mask state (`rt_sigaction`, `rt_sigprocmask`, `sigaltstack`) | done |
| B | Generation (`kill`, `tkill`, `tgkill`) and the default terminate action | done |
| C | Blocking syscalls notice a fatal signal and terminate (killable while blocked) | done |
| D | Handler delivery: build a frame on the user stack, divert the return, `rt_sigreturn`, `SA_RESTART`, userspace `EINTR` | planned |
| E | Catchable faults: save the user context in the fault stub so `SIGSEGV`/`SIGFPE`/`SIGILL` reach a handler | planned |
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
  disposition), leaves a caught signal pending for stage D, and returns the
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

## What stages A to C deliberately do not do

- No user handler runs. A caught signal (a handler is installed) is recorded as
  pending and otherwise ignored until stage D. There is therefore no userspace
  `EINTR` yet: a blocking syscall is interrupted only to terminate, never to run
  a handler and resume.
- No `SIGCHLD` is generated on child exit (stage D territory, since its only
  effect is through a handler or `sigsuspend`).
- Thread-group signal semantics are minimal: a process-directed fatal signal is
  shared, so each thread terminates at its own next boundary, rather than being
  torn down together at once.
