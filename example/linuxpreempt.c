/* linuxpreempt: prove that a Linux user compute loop is preempted.
 *
 * The main thread spins in a pure userspace loop waiting on a shared flag, and a
 * worker thread spins in its own userspace loop before setting that flag. On one
 * vcpu with a cooperative scheduler, whichever thread runs first keeps the cpu
 * forever and the program hangs. With preemption the timer rotates them, the
 * worker reaches its flag, and main escapes the wait. So a clean "preempt ok"
 * means the timer interrupted a running user loop. */
#include <pthread.h>
#include <unistd.h>

static volatile int done;

static void *
worker(void *arg)
{
    (void)arg;
    for (volatile long i = 0; i < 20000000; i++)
        ;
    done = 1;
    return 0;
}

int
main(void)
{
    pthread_t t;
    if (pthread_create(&t, 0, worker, 0) != 0) {
        write(2, "spawn failed\n", 13);
        return 1;
    }
    /* Spin in user mode with no system call, so only preemption can ever hand
     * the cpu to the worker. */
    while (!done)
        for (volatile int k = 0; k < 1000; k++)
            ;
    write(1, "preempt ok\n", 11);
    pthread_join(t, 0);
    return 0;
}
