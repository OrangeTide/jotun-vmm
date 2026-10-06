/* task.c : cooperative round-robin scheduler.
 *
 * Two contexts ping-pong: the scheduler (running on the boot stack) and
 * whichever task it has switched into. A task hands control back by switching
 * to the saved scheduler context, so the scheduler loop resumes right after
 * the switch that entered the task. When no task is ready, the loop ends and
 * sched_start returns to the boot code, which halts.
 */
#include "task.h"

#include "arch.h"
#include "mem.h"
#include "print.h"

#define MAX_TASKS   8
#define TASK_STACK  8192

enum task_state {
    TASK_FREE,
    TASK_READY,
    TASK_RUNNING,
    TASK_DEAD,
};

struct task {
    int                 id;
    const char          *name;
    enum task_state     state;
    task_fn             fn;
    void                *sp;        /* saved stack pointer when not running */
    unsigned char       *stack;     /* kmalloc'd base, for reference */
    struct task         *qnext;     /* ready-queue link */
};

static struct task tasks[MAX_TASKS];
static struct task *runq_head;
static struct task *runq_tail;
static struct task *cur;            /* the running task, or NULL in scheduler */
static void *sched_sp;              /* scheduler context, saved across switches */

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

/* First code to run on a new task's stack. The scheduler sets cur before
 * switching in, so cur->fn is this task's function. */
static void
task_trampoline(void)
{
    cur->fn();
    task_exit();
}

int
task_spawn(const char *name, task_fn fn)
{
    for (int i = 0; i < MAX_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state != TASK_FREE)
            continue;

        unsigned char *stack = kmalloc(TASK_STACK);
        if (!stack)
            return -1;

        t->id = i;
        t->name = name;
        t->fn = fn;
        t->stack = stack;
        t->sp = stack_init(stack, TASK_STACK, task_trampoline);
        t->state = TASK_READY;
        enqueue(t);
        return i;
    }
    return -1;
}

void
sched_start(void)
{
    struct task *t;

    while ((t = dequeue()) != NULL) {
        cur = t;
        t->state = TASK_RUNNING;
        /* Save the scheduler context and run the task. Control returns here
         * when the task yields or exits. */
        ctx_switch(&sched_sp, t->sp);
        cur = NULL;
    }
}

void
task_yield(void)
{
    struct task *t = cur;

    t->state = TASK_READY;
    enqueue(t);
    ctx_switch(&t->sp, sched_sp);
}

void
task_exit(void)
{
    cur->state = TASK_DEAD;
    /* Nothing to save: this context is finished, so pass NULL as oldsp. */
    ctx_switch(NULL, sched_sp);
    for (;;)
        ;       /* not reached */
}

static const char *
state_name(enum task_state s)
{
    switch (s) {
    case TASK_FREE:     return "free";
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
    case TASK_DEAD:     return "dead";
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
