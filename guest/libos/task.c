/* task.c : per-cpu preemptive scheduler with spawn, exec, await, and notes.
 *
 * Every vcpu runs its own copy of the scheduler loop (sched_start) on its boot
 * stack, finding its private state through arch_get_percpu. A single run queue
 * and the task table are shared across cpus and guarded by one spinlock, so a
 * ready task runs on whichever cpu reaches it first. That is the whole of the
 * load balancing: push ready tasks onto the shared queue, let idle cpus pull.
 *
 * A task hands control back by switching to its cpu's saved scheduler context.
 * The crucial SMP rule is that a task is never placed on the run queue until
 * its stack pointer is saved: task_yield and task_exec only mark the outgoing
 * task READY and switch away, and the scheduler enqueues it afterwards, once
 * the switch has stored its sp. Otherwise another cpu could dequeue and resume
 * a task whose context was still being written.
 *
 * When a cpu finds the queue empty it blocks in the host (smp_idle) until some
 * cpu with new work wakes it (smp_wake_all). When the queue is empty and no cpu
 * is running a task, the system has quiesced: the discovering cpu sets the
 * shutdown flag and wakes the others so every scheduler loop returns.
 *
 * A task that exits becomes a zombie holding its exit status until a parent
 * awaits it; the stack is freed as soon as the task is gone, the slot when the
 * status is collected.
 */
#include "task.h"

#include <stdint.h>

#include "arch.h"
#include "jotun_abi.h"
#include "mem.h"
#include "print.h"

#define MAX_TASKS   8
#define MAX_CPUS    8
#define TASK_STACK  8192
#define NO_PARENT   (-1)

enum task_state {
    TASK_FREE,
    TASK_READY,
    TASK_RUNNING,
    TASK_BLOCKED,       /* parked on an event, not runnable until woken */
    TASK_ZOMBIE,        /* exited, awaiting reap */
};

struct task {
    int                 id;
    int                 parent;
    const char          *name;
    enum task_state     state;
    task_fn             fn;
    void                *sp;        /* saved stack pointer when not running */
    unsigned char       *stack;     /* kmalloc'd base, freed at exit */
    struct task         *qnext;     /* ready-queue link */
    int                 parked;     /* BLOCKED and its sp is saved: safe to wake */
    int                 wake_pending; /* a wake arrived before the task parked */
    char                status[STATUS_MAX];
    char                notes[NNOTES][NOTE_MAX];
    int                 note_head;
    int                 note_tail;
};

/* Per-cpu state, reached through arch_get_percpu. self lets a caller that
 * holds a struct percpu * recover the pointer the arch register hands back. */
struct percpu {
    struct percpu       *self;
    int                 id;
    struct task         *cur;       /* running task on this cpu, or NULL */
    void                *sched_sp;  /* this cpu's scheduler context */
    unsigned char       *reap;      /* an exec'd task's old stack, freed by
                                       this cpu's scheduler after the switch */
};

static struct percpu cpus[MAX_CPUS];

static struct task tasks[MAX_TASKS];
static struct task *runq_head;
static struct task *runq_tail;
static volatile int runq_lock;      /* guards runq + tasks table + counters */
static int running_count;           /* tasks currently executing on some cpu */
static int blocked_count;           /* tasks parked on an event */
static volatile int shutdown;       /* set once the system has quiesced */

static struct percpu *
this_cpu(void)
{
    return arch_get_percpu();
}

/* Acquire the scheduler lock with interrupts masked. Masking on this cpu is
 * what keeps a tick from re-entering the scheduler and spinning on a lock this
 * same cpu already holds. */
static unsigned long
lock_irq(void)
{
    unsigned long f = arch_irq_save();
    while (__atomic_exchange_n(&runq_lock, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    return f;
}

static void
unlock_irq(unsigned long f)
{
    __atomic_store_n(&runq_lock, 0, __ATOMIC_RELEASE);
    arch_irq_restore(f);
}

/* Block this cpu in the host until another cpu wakes it. A store to the idle
 * doorbell is an MMIO exit the host parks the vcpu thread on. */
static void
smp_idle(void)
{
    *(volatile uint32_t *)JOTUN_IDLE_DOORBELL = 0;
}

/* Wake every idle cpu so each rechecks the run queue (new work, or shutdown). */
static void
smp_wake_all(void)
{
    *(volatile uint32_t *)JOTUN_WAKE_DOORBELL = JOTUN_WAKE_ALL;
}

/* Bounded string copy, always terminated. No libc in a freestanding guest. */
static void
scopy(char *dst, const char *src, int max)
{
    int i = 0;

    if (src == NULL)
        src = "";
    for (i = 0; i < max - 1 && src[i] != '\0'; i++)
        dst[i] = src[i];
    dst[i] = '\0';
}

/* Run-queue helpers. The caller holds runq_lock. */
static void
enqueue(struct task *t)
{
    t->qnext = NULL;
    if (runq_tail)
        runq_tail->qnext = t;
    else
        runq_head = t;
    runq_tail = t;
}

static struct task *
dequeue(void)
{
    struct task *t = runq_head;

    if (t) {
        runq_head = t->qnext;
        if (!runq_head)
            runq_tail = NULL;
        t->qnext = NULL;
    }
    return t;
}

void
cpu_init(unsigned long id)
{
    struct percpu *pc = &cpus[id];

    pc->self = pc;
    pc->id = (int)id;
    pc->cur = NULL;
    pc->sched_sp = NULL;
    pc->reap = NULL;
    arch_set_percpu(pc);
}

/* First code to run on a new task's stack. The scheduler sets cur before
 * switching in, so cur->fn is this task's function. If the function returns,
 * the task exits with success. */
static void
task_trampoline(void)
{
    this_cpu()->cur->fn();
    task_exits(NULL);
}

int
task_spawn(const char *name, task_fn fn)
{
    int id = -1;
    unsigned long f = lock_irq();

    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state != TASK_FREE)
            continue;

        unsigned char *stack = kmalloc(TASK_STACK);
        if (!stack)
            break;          /* table had room but the heap did not */

        t->id = i;
        t->parent = this_cpu()->cur ? this_cpu()->cur->id : NO_PARENT;
        t->name = name;
        t->fn = fn;
        t->stack = stack;
        t->sp = stack_init(stack, TASK_STACK, task_trampoline);
        t->status[0] = '\0';
        t->note_head = t->note_tail = 0;
        t->parked = 0;
        t->wake_pending = 0;
        t->state = TASK_READY;
        enqueue(t);
        id = i;
        break;
    }
    unlock_irq(f);

    if (id >= 0)
        smp_wake_all();     /* new work: rouse any cpu sitting idle */
    return id;
}

int
task_exec(task_fn fn)
{
    /* Allocate the fresh stack before touching scheduler state: we are running
     * on the current stack and cannot lay the new program over our own live
     * frame. kmalloc may block on the heap lock, so do it outside the switch. */
    unsigned char *stack = kmalloc(TASK_STACK);
    if (!stack)
        return -1;

    arch_irq_disable();             /* same-cpu guard; does not return */
    struct percpu *pc = this_cpu();
    struct task *t = pc->cur;

    pc->reap = t->stack;            /* scheduler frees the old stack after we go */
    t->fn = fn;
    t->stack = stack;
    t->sp = stack_init(stack, TASK_STACK, task_trampoline);
    t->status[0] = '\0';
    t->parked = 0;
    t->wake_pending = 0;
    t->state = TASK_READY;
    /* Discard this context. The scheduler re-enqueues t (now READY with a
     * fresh sp) and frees pc->reap once the switch has left this stack. */
    ctx_switch(NULL, pc->sched_sp);
    for (;;)
        ;       /* not reached */
}

void
sched_start(void)
{
    struct percpu *pc = this_cpu();

    for (;;) {
        unsigned long f = lock_irq();
        if (shutdown) {
            unlock_irq(f);
            return;
        }

        struct task *t = dequeue();
        if (!t) {
            if (running_count == 0 && blocked_count == 0) {
                /* Queue empty, nothing running, nothing parked: the system
                 * has quiesced. Signal every cpu to leave its scheduler. */
                shutdown = 1;
                unlock_irq(f);
                smp_wake_all();
                return;
            }
            unlock_irq(f);
            /* Either another cpu is busy and will queue work, or every task is
             * parked and a waker (another task, or a host event) will ready
             * one. Block until a wake doorbell rouses us, then recheck. */
            smp_idle();
            continue;
        }

        t->state = TASK_RUNNING;
        running_count++;
        pc->cur = t;
        unlock_irq(f);

        /* Save this cpu's scheduler context and run the task. Control returns
         * here, on the same cpu, when the task yields, exec's, or exits. */
        ctx_switch(&pc->sched_sp, t->sp);

        struct task *done = pc->cur;
        pc->cur = NULL;
        unsigned char *free1 = NULL, *free2 = NULL;

        f = lock_irq();
        running_count--;
        if (done->state == TASK_READY) {
            /* Yielded or exec'd. Its sp is saved now, so it is safe to let
             * another cpu pick it up. */
            enqueue(done);
        } else if (done->state == TASK_BLOCKED) {
            /* Parked on an event. Its sp is saved now, so a wake may re-queue
             * it. If a wake raced in before it parked, honour that instead of
             * parking. */
            if (done->wake_pending) {
                done->wake_pending = 0;
                done->state = TASK_READY;
                enqueue(done);
            } else {
                done->parked = 1;
                blocked_count++;
            }
        } else if (done->state == TASK_ZOMBIE && done->stack) {
            /* Exited: free the stack, leave status and slot for a reaper. */
            free1 = done->stack;
            done->stack = NULL;
        }
        if (pc->reap) {
            free2 = pc->reap;
            pc->reap = NULL;
        }
        unlock_irq(f);

        /* Free outside the lock: kfree takes the heap lock, and the ordering
         * is always scheduler-lock-then-heap-lock elsewhere, never the
         * reverse. Releasing first keeps that invariant simple. */
        if (free1)
            kfree(free1);
        if (free2)
            kfree(free2);
    }
}

void
task_yield(void)
{
    unsigned long f = arch_irq_save();
    struct percpu *pc = this_cpu();

    pc->cur->state = TASK_READY;
    /* Do not enqueue here: another cpu could dequeue and resume this task
     * before ctx_switch finishes saving its sp. The scheduler enqueues it on
     * the way back, when the sp is safely stored. */
    ctx_switch(&pc->cur->sp, pc->sched_sp);
    arch_irq_restore(f);
}

void
task_block(void)
{
    unsigned long f = arch_irq_save();
    struct percpu *pc = this_cpu();

    /* A wake that arrived while we were still running (before parking) cancels
     * the block: consume it and stay runnable. */
    if (pc->cur->wake_pending) {
        pc->cur->wake_pending = 0;
        arch_irq_restore(f);
        return;
    }
    pc->cur->state = TASK_BLOCKED;
    /* Like yield, do not touch the run queue here. The scheduler parks the
     * task after the switch saves its sp; only then can a wake re-queue it. */
    ctx_switch(&pc->cur->sp, pc->sched_sp);
    arch_irq_restore(f);
}

void
task_wake(int id)
{
    if (id < 0 || id >= MAX_TASKS)
        return;

    unsigned long f = lock_irq();
    struct task *t = &tasks[id];
    if (t->state == TASK_BLOCKED && t->parked) {
        /* Parked and sp saved: safe to make runnable now. */
        t->parked = 0;
        t->state = TASK_READY;
        blocked_count--;
        enqueue(t);
    } else {
        /* Blocked but not yet parked, or not blocked yet: record the wake so
         * task_block (or the scheduler's park step) honours it and does not
         * sleep through it. */
        t->wake_pending = 1;
    }
    unlock_irq(f);

    smp_wake_all();     /* a newly runnable task: rouse any idle cpu */
}

/* Called from the event ISR. Reschedule the running task; if the scheduler
 * itself was interrupted (cur is NULL) there is nothing to preempt. */
void
irq_event(void)
{
    if (this_cpu()->cur)
        task_yield();
}

void
task_exits(const char *status)
{
    arch_irq_disable();             /* does not return, so no restore */
    struct percpu *pc = this_cpu();

    scopy(pc->cur->status, status, STATUS_MAX);
    pc->cur->state = TASK_ZOMBIE;
    /* Nothing to save: this context is finished, so pass NULL as oldsp. */
    ctx_switch(NULL, pc->sched_sp);
    for (;;)
        ;       /* not reached */
}

int
task_await(char *buf, int nbuf)
{
    int myid = this_cpu()->cur->id;

    for (;;) {
        int live = 0;
        unsigned long f = lock_irq();

        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t->state == TASK_FREE || t->parent != myid)
                continue;
            if (t->state == TASK_ZOMBIE) {
                int id = t->id;
                if (buf && nbuf > 0)
                    scopy(buf, t->status, nbuf);
                t->state = TASK_FREE;
                unlock_irq(f);
                return id;
            }
            live = 1;
        }
        unlock_irq(f);
        if (!live)
            return -1;
        task_yield();       /* let the children make progress */
    }
}

int
task_note(int id, const char *note)
{
    if (id < 0 || id >= MAX_TASKS)
        return -1;

    unsigned long f = lock_irq();
    struct task *t = &tasks[id];
    if (t->state == TASK_FREE || t->note_tail - t->note_head >= NNOTES) {
        unlock_irq(f);
        return -1;          /* gone, or queue full */
    }
    scopy(t->notes[t->note_tail % NNOTES], note, NOTE_MAX);
    t->note_tail++;
    unlock_irq(f);
    return 0;
}

int
task_recv_note(char *buf, int nbuf)
{
    unsigned long f = lock_irq();
    struct task *t = this_cpu()->cur;
    if (t->note_head == t->note_tail) {
        unlock_irq(f);
        return 0;
    }
    if (buf && nbuf > 0)
        scopy(buf, t->notes[t->note_head % NNOTES], nbuf);
    t->note_head++;
    unlock_irq(f);
    return 1;
}

static const char *
state_name(enum task_state s)
{
    switch (s) {
    case TASK_FREE:     return "free";
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_BLOCKED:  return "blocked";
    case TASK_ZOMBIE:   return "zombie";
    }
    return "?";
}

void
task_dump(void)
{
    con_puts("task  state    name\n");
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_FREE)
            continue;
        const char *st = state_name(t->state);
        int n = 0;

        con_putc('0' + (char)t->id);
        con_puts("     ");
        con_puts(st);
        /* pad the state column to a fixed width of nine */
        while (st[n])
            n++;
        for (int pad = 9 - n; pad > 0; pad--)
            con_putc(' ');
        con_puts(t->name);
        con_putc('\n');
    }
}
