/* task.c : cooperative scheduler with spawn, exec, await, and notes.
 *
 * Two contexts ping-pong: the scheduler (on the boot stack) and whichever task
 * it switched into. A task hands control back by switching to the saved
 * scheduler context, so the scheduler loop resumes right after the switch that
 * entered the task. When no task is ready the loop ends and sched_start
 * returns to the boot code, which halts.
 *
 * A task that exits becomes a zombie holding its exit status until a parent
 * awaits it; the stack is freed as soon as the task is gone, the slot when the
 * status is collected. A task with no awaiting parent simply lingers as a
 * zombie, which is fine at shutdown.
 */
#include "task.h"

#include "arch.h"
#include "mem.h"
#include "print.h"

#define MAX_TASKS   8
#define TASK_STACK  8192
#define NO_PARENT   (-1)

enum task_state {
    TASK_FREE,
    TASK_READY,
    TASK_RUNNING,
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
    char                status[STATUS_MAX];
    char                notes[NNOTES][NOTE_MAX];
    int                 note_head;
    int                 note_tail;
};

static struct task tasks[MAX_TASKS];
static struct task *runq_head;
static struct task *runq_tail;
static struct task *cur;            /* the running task, or NULL in scheduler */
static void *sched_sp;              /* scheduler context, saved across switches */
static unsigned char *reap_stack;   /* an exec'd task's old stack, freed by the
                                       scheduler once it is no longer in use */

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
 * switching in, so cur->fn is this task's function. If the function returns,
 * the task exits with success. */
static void
task_trampoline(void)
{
    cur->fn();
    task_exits(NULL);
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
        t->parent = cur ? cur->id : NO_PARENT;
        t->name = name;
        t->fn = fn;
        t->stack = stack;
        t->sp = stack_init(stack, TASK_STACK, task_trampoline);
        t->status[0] = '\0';
        t->note_head = t->note_tail = 0;
        t->state = TASK_READY;
        enqueue(t);
        return i;
    }
    return -1;
}

int
task_exec(task_fn fn)
{
    struct task *t = cur;

    /* We are running on t's current stack, so we cannot lay the fresh program
     * down on it without clobbering our own live frame. Allocate a new stack,
     * point the task at it, and hand the old one to the scheduler to free once
     * we have switched off it. This context is discarded on the switch out, so
     * its frames are never resumed. */
    unsigned char *stack = kmalloc(TASK_STACK);
    if (!stack)
        return -1;

    reap_stack = t->stack;
    t->fn = fn;
    t->stack = stack;
    t->sp = stack_init(stack, TASK_STACK, task_trampoline);
    t->status[0] = '\0';
    t->state = TASK_READY;
    enqueue(t);
    ctx_switch(NULL, sched_sp);     /* discard this context, run fresh */
    for (;;)
        ;       /* not reached */
}

void
sched_start(void)
{
    struct task *t;

    while ((t = dequeue()) != NULL) {
        cur = t;
        t->state = TASK_RUNNING;
        /* Save the scheduler context and run the task. Control returns here
         * when the task yields, exits, or exec's. */
        ctx_switch(&sched_sp, t->sp);
        /* Now off the task's stack: reap an exec'd task's old stack, and free
         * a zombie's stack (its status and slot linger until a parent awaits
         * it). */
        if (reap_stack) {
            kfree(reap_stack);
            reap_stack = NULL;
        }
        if (cur->state == TASK_ZOMBIE && cur->stack) {
            kfree(cur->stack);
            cur->stack = NULL;
        }
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
task_exits(const char *status)
{
    scopy(cur->status, status, STATUS_MAX);
    cur->state = TASK_ZOMBIE;
    /* Nothing to save: this context is finished, so pass NULL as oldsp. */
    ctx_switch(NULL, sched_sp);
    for (;;)
        ;       /* not reached */
}

int
task_await(char *buf, int nbuf)
{
    for (;;) {
        int live = 0;

        for (int i = 0; i < MAX_TASKS; i++) {
            struct task *t = &tasks[i];
            if (t->state == TASK_FREE || t->parent != cur->id)
                continue;
            if (t->state == TASK_ZOMBIE) {
                int id = t->id;
                if (buf && nbuf > 0)
                    scopy(buf, t->status, nbuf);
                t->state = TASK_FREE;
                return id;
            }
            live = 1;
        }
        if (!live)
            return -1;
        task_yield();       /* let the children make progress */
    }
}

int
task_note(int id, const char *note)
{
    struct task *t;

    if (id < 0 || id >= MAX_TASKS)
        return -1;
    t = &tasks[id];
    if (t->state == TASK_FREE)
        return -1;
    if (t->note_tail - t->note_head >= NNOTES)
        return -1;      /* queue full */
    scopy(t->notes[t->note_tail % NNOTES], note, NOTE_MAX);
    t->note_tail++;
    return 0;
}

int
task_recv_note(char *buf, int nbuf)
{
    struct task *t = cur;

    if (t->note_head == t->note_tail)
        return 0;
    if (buf && nbuf > 0)
        scopy(buf, t->notes[t->note_head % NNOTES], nbuf);
    t->note_head++;
    return 1;
}

static const char *
state_name(enum task_state s)
{
    switch (s) {
    case TASK_FREE:     return "free";
    case TASK_READY:    return "ready";
    case TASK_RUNNING:  return "running";
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
