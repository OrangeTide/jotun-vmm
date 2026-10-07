/* threads.h : a C11 <threads.h> subset over the task scheduler.
 *
 * Maps the standard thread, mutex, and condition-variable API onto the library
 * OS task primitives (task.c). A thrd_t is a task id; a thread is a task that is
 * a child of its creator, so joining reaps it. Mutexes are compare-and-swap
 * spinlocks that yield while contended (correct under the preemptive, possibly
 * multi-vcpu scheduler), and condition variables park waiters with task_block.
 *
 * Scope and limits, as a libc-lite subset until a real consumer shapes it:
 *   - A thread is joined by the thread that created it (task_await reaps a
 *     caller's own children).
 *   - There is no guest clock, so the timed variants (mtx_timedlock,
 *     cnd_timedwait) are not provided, and thrd_sleep yields the CPU without
 *     honouring the requested duration.
 *   - tss_* thread-specific storage is not provided.
 */
#ifndef LIBOS_THREADS_H
#define LIBOS_THREADS_H

#include "task.h"       /* TASK_MAX bounds the per-thread tables */

/* The freestanding guest has no <time.h>; thrd_sleep takes this minimal form. */
#ifndef LIBOS_TIMESPEC_DEFINED
#define LIBOS_TIMESPEC_DEFINED
struct timespec {
    long tv_sec;
    long tv_nsec;
};
#endif

/* Result codes. The names are what C11 fixes; the values are arbitrary. */
enum {
    thrd_success = 0,
    thrd_nomem,
    thrd_timedout,
    thrd_busy,
    thrd_error,
};

/* Mutex types. mtx_timed is accepted but behaves as mtx_plain (no clock). */
enum {
    mtx_plain = 0,
    mtx_recursive = 1,
    mtx_timed = 2,
};

typedef int thrd_t;                     /* a task id */
typedef int (*thrd_start_t)(void *);

typedef struct {
    volatile int state;                 /* 0 unlocked, 1 locked */
    int type;
    int owner;                          /* holder's task id, or -1 */
    int count;                          /* recursion depth for mtx_recursive */
} mtx_t;

typedef struct {
    mtx_t lk;                           /* guards the waiter list */
    int waiters[TASK_MAX];              /* parked task ids, oldest first */
    int n;
} cnd_t;

typedef struct {
    volatile int state;                 /* 0 unstarted, 1 running, 2 done */
} once_flag;
#define ONCE_FLAG_INIT { 0 }

/* Threads. thrd_create returns thrd_success, thrd_nomem, or thrd_error. */
int    thrd_create(thrd_t *thr, thrd_start_t func, void *arg);
int    thrd_equal(thrd_t a, thrd_t b);
thrd_t thrd_current(void);
int    thrd_sleep(const struct timespec *duration, struct timespec *remaining);
void   thrd_yield(void);
void   thrd_exit(int res) __attribute__((noreturn));
int    thrd_detach(thrd_t thr);
int    thrd_join(thrd_t thr, int *res);

/* Mutexes. */
int    mtx_init(mtx_t *m, int type);
int    mtx_lock(mtx_t *m);
int    mtx_trylock(mtx_t *m);
int    mtx_unlock(mtx_t *m);
void   mtx_destroy(mtx_t *m);

/* Condition variables. */
int    cnd_init(cnd_t *c);
int    cnd_signal(cnd_t *c);
int    cnd_broadcast(cnd_t *c);
int    cnd_wait(cnd_t *c, mtx_t *m);
void   cnd_destroy(cnd_t *c);

/* One-time initialisation. */
void   call_once(once_flag *flag, void (*func)(void));

#endif /* LIBOS_THREADS_H */
