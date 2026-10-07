/* threads.c : the C11 <threads.h> subset, implemented over task.c.
 *
 * A thread is a task. thrd_create spawns a trampoline task and hands it the
 * start function and argument through a per-id table; the trampoline records
 * the return value and exits, and thrd_join reaps the task (task_await) and
 * collects that value. Mutexes are compare-and-swap spinlocks that yield while
 * contended, and condition variables park waiters with task_block and wake them
 * with task_wake, which is race-free against a wake that arrives just before a
 * task parks.
 */
#include "threads.h"

#include "arch.h"
#include "task.h"

/****************************************************************
 * Per-thread bookkeeping
 ****************************************************************/

struct thr {
    int          active;        /* slot in use (created, not yet reaped) */
    int          detached;      /* will not be joined; reaped opportunistically */
    volatile int ready;         /* the handoff of func/arg is complete */
    volatile int done;          /* the start function has returned */
    thrd_start_t func;
    void        *arg;
    int          result;        /* the start function's return value */
};

static struct thr thrtab[TASK_MAX];

/* A finished thread reaped while joining a different one is held here until its
 * own join collects it, so threads may be joined in any order. */
struct done_result {
    int valid;
    int id;
    int result;
};
static struct done_result donebuf[TASK_MAX];

/* A short spinlock over the tables above. Held only across table edits, never
 * across task_await or a yield, so it cannot stall the scheduler. */
static volatile int thr_lk;

static unsigned long
thr_lock(void)
{
    unsigned long f = arch_irq_save();
    while (__atomic_exchange_n(&thr_lk, 1, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    return f;
}

static void
thr_unlock(unsigned long f)
{
    __atomic_store_n(&thr_lk, 0, __ATOMIC_RELEASE);
    arch_irq_restore(f);
}

static void
buffer_put(int id, int result)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (!donebuf[i].valid) {
            donebuf[i].id = id;
            donebuf[i].result = result;
            donebuf[i].valid = 1;
            return;
        }
}

/* Take a buffered result for id, if present. Caller holds thr_lk. */
static int
buffer_take(int id, int *result)
{
    for (int i = 0; i < TASK_MAX; i++)
        if (donebuf[i].valid && donebuf[i].id == id) {
            if (result)
                *result = donebuf[i].result;
            donebuf[i].valid = 0;
            return 1;
        }
    return 0;
}

/* Reap any detached thread that has finished, so its task slot can be reused.
 * While a finished detached thread exists a zombie is present, so task_await
 * returns promptly; a joinable thread reaped along the way is buffered. */
static void
reap_detached(void)
{
    for (;;) {
        unsigned long f = thr_lock();
        int pending = 0;
        for (int i = 0; i < TASK_MAX; i++)
            if (thrtab[i].active && thrtab[i].detached && thrtab[i].done) {
                pending = 1;
                break;
            }
        thr_unlock(f);
        if (!pending)
            return;

        char st[STATUS_MAX];
        int rid = task_await(st, sizeof st);
        if (rid < 0)
            return;                 /* no child to reap (should not happen) */

        f = thr_lock();
        int result = thrtab[rid].result;
        int detached = thrtab[rid].detached;
        thrtab[rid].active = 0;
        if (!detached)
            buffer_put(rid, result);
        thr_unlock(f);
    }
}

/****************************************************************
 * Threads
 ****************************************************************/

/* The task that each thread runs. It waits for thrd_create to publish its start
 * function, calls it, records the result, and exits. */
static void
thread_trampoline(void)
{
    int id = task_self();
    while (!__atomic_load_n(&thrtab[id].ready, __ATOMIC_ACQUIRE))
        arch_cpu_relax();
    thrd_exit(thrtab[id].func(thrtab[id].arg));
}

int
thrd_create(thrd_t *thr, thrd_start_t func, void *arg)
{
    if (!func)
        return thrd_error;

    reap_detached();        /* recycle any finished detached slots first */

    int id = task_spawn("thrd", thread_trampoline);
    if (id < 0)
        return thrd_nomem;

    /* Publish the start function and argument, then release the trampoline.
     * The trampoline spins on `ready`, so it reads these only once set. */
    thrtab[id].func = func;
    thrtab[id].arg = arg;
    thrtab[id].result = 0;
    thrtab[id].detached = 0;
    __atomic_store_n(&thrtab[id].done, 0, __ATOMIC_RELAXED);

    unsigned long f = thr_lock();
    thrtab[id].active = 1;
    thr_unlock(f);

    __atomic_store_n(&thrtab[id].ready, 1, __ATOMIC_RELEASE);

    if (thr)
        *thr = id;
    return thrd_success;
}

int
thrd_equal(thrd_t a, thrd_t b)
{
    return a == b;
}

thrd_t
thrd_current(void)
{
    return task_self();
}

/* No guest clock: yield the CPU once and report the full duration remaining,
 * so a caller that sleeps in a loop becomes a cooperative yield loop. */
int
thrd_sleep(const struct timespec *duration, struct timespec *remaining)
{
    (void)duration;
    task_yield();
    if (remaining) {
        remaining->tv_sec = 0;
        remaining->tv_nsec = 0;
    }
    return 0;
}

void
thrd_yield(void)
{
    task_yield();
}

void
thrd_exit(int res)
{
    int id = task_self();
    if (id >= 0 && id < TASK_MAX) {
        unsigned long f = thr_lock();
        thrtab[id].result = res;
        __atomic_store_n(&thrtab[id].done, 1, __ATOMIC_RELEASE);
        thr_unlock(f);
    }
    task_exits("");
}

int
thrd_detach(thrd_t thr)
{
    if (thr < 0 || thr >= TASK_MAX)
        return thrd_error;
    unsigned long f = thr_lock();
    int active = thrtab[thr].active;
    if (active)
        thrtab[thr].detached = 1;
    thr_unlock(f);
    if (!active)
        return thrd_error;          /* unknown, or already joined */
    reap_detached();                /* if it already finished, reap it now */
    return thrd_success;
}

int
thrd_join(thrd_t thr, int *res)
{
    if (thr < 0 || thr >= TASK_MAX)
        return thrd_error;

    for (;;) {
        unsigned long f = thr_lock();
        if (buffer_take(thr, res)) {        /* finished earlier, out of order */
            thr_unlock(f);
            return thrd_success;
        }
        int active = thrtab[thr].active;
        thr_unlock(f);
        if (!active)
            return thrd_error;              /* unknown, detached, or joined */

        char st[STATUS_MAX];
        int rid = task_await(st, sizeof st);
        if (rid < 0)
            return thrd_error;

        f = thr_lock();
        int result = thrtab[rid].result;
        int detached = thrtab[rid].detached;
        thrtab[rid].active = 0;
        if (rid == thr) {
            thr_unlock(f);
            if (res)
                *res = result;
            return thrd_success;
        }
        if (!detached)
            buffer_put(rid, result);        /* some other thread: keep it */
        thr_unlock(f);
    }
}

/****************************************************************
 * Mutexes
 ****************************************************************/

int
mtx_init(mtx_t *m, int type)
{
    m->state = 0;
    m->type = type;
    m->owner = -1;
    m->count = 0;
    return thrd_success;
}

int
mtx_lock(mtx_t *m)
{
    int me = task_self();
    if (m->type == mtx_recursive && m->owner == me) {
        m->count++;
        return thrd_success;
    }
    while (__atomic_exchange_n(&m->state, 1, __ATOMIC_ACQUIRE))
        task_yield();               /* contended: let the holder run */
    m->owner = me;
    m->count = 1;
    return thrd_success;
}

int
mtx_trylock(mtx_t *m)
{
    int me = task_self();
    if (m->type == mtx_recursive && m->owner == me) {
        m->count++;
        return thrd_success;
    }
    if (__atomic_exchange_n(&m->state, 1, __ATOMIC_ACQUIRE))
        return thrd_busy;
    m->owner = me;
    m->count = 1;
    return thrd_success;
}

int
mtx_unlock(mtx_t *m)
{
    if (m->type == mtx_recursive && --m->count > 0)
        return thrd_success;
    m->owner = -1;
    m->count = 0;
    __atomic_store_n(&m->state, 0, __ATOMIC_RELEASE);
    return thrd_success;
}

void
mtx_destroy(mtx_t *m)
{
    (void)m;                        /* nothing to release */
}

/****************************************************************
 * Condition variables
 ****************************************************************/

int
cnd_init(cnd_t *c)
{
    mtx_init(&c->lk, mtx_plain);
    c->n = 0;
    return thrd_success;
}

/* Remove the first occurrence of id from the waiter list. Caller holds c->lk. */
static void
cnd_remove(cnd_t *c, int id)
{
    for (int i = 0; i < c->n; i++)
        if (c->waiters[i] == id) {
            for (int j = i; j < c->n - 1; j++)
                c->waiters[j] = c->waiters[j + 1];
            c->n--;
            return;
        }
}

int
cnd_wait(cnd_t *c, mtx_t *m)
{
    int me = task_self();

    mtx_lock(&c->lk);
    if (c->n < TASK_MAX)
        c->waiters[c->n++] = me;
    mtx_unlock(&c->lk);

    /* Release the caller's mutex and park. A signal that arrives between here
     * and task_block is not lost: task_wake sets a pending flag the block
     * honours. */
    mtx_unlock(m);
    task_block();

    /* Drop ourselves from the list in case the wakeup was spurious. */
    mtx_lock(&c->lk);
    cnd_remove(c, me);
    mtx_unlock(&c->lk);

    mtx_lock(m);
    return thrd_success;
}

int
cnd_signal(cnd_t *c)
{
    int id = -1;
    mtx_lock(&c->lk);
    if (c->n > 0) {
        id = c->waiters[0];
        for (int j = 0; j < c->n - 1; j++)
            c->waiters[j] = c->waiters[j + 1];
        c->n--;
    }
    mtx_unlock(&c->lk);
    if (id >= 0)
        task_wake(id);
    return thrd_success;
}

int
cnd_broadcast(cnd_t *c)
{
    int ids[TASK_MAX];
    int n;
    mtx_lock(&c->lk);
    n = c->n;
    for (int i = 0; i < n; i++)
        ids[i] = c->waiters[i];
    c->n = 0;
    mtx_unlock(&c->lk);
    for (int i = 0; i < n; i++)
        task_wake(ids[i]);
    return thrd_success;
}

void
cnd_destroy(cnd_t *c)
{
    (void)c;                        /* nothing to release */
}

/****************************************************************
 * call_once
 ****************************************************************/

void
call_once(once_flag *flag, void (*func)(void))
{
    if (__atomic_load_n(&flag->state, __ATOMIC_ACQUIRE) == 2)
        return;
    int expected = 0;
    if (__atomic_compare_exchange_n(&flag->state, &expected, 1, 0,
                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        func();
        __atomic_store_n(&flag->state, 2, __ATOMIC_RELEASE);
    } else {
        while (__atomic_load_n(&flag->state, __ATOMIC_ACQUIRE) != 2)
            task_yield();
    }
}
