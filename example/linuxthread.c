/* linuxthread : exercise pthreads (clone with CLONE_VM, futex, join) in the
 * Linux binary-compat guest. Four threads each add their id to a shared counter
 * under a mutex; the main thread joins them and prints the total, which must be
 * (1+2+3+4) * 1000 = 10000. Built static with musl. */
#include <pthread.h>
#include <unistd.h>
#include <string.h>

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static long counter;

static void
out(const char *s)
{
    write(1, s, strlen(s));
}

static void *
worker(void *arg)
{
    long n = (long)arg;
    for (int i = 0; i < 1000; i++) {
        pthread_mutex_lock(&lock);
        counter += n;
        pthread_mutex_unlock(&lock);
    }
    return 0;
}

int
main(void)
{
    pthread_t th[4];

    for (int i = 0; i < 4; i++)
        if (pthread_create(&th[i], 0, worker, (void *)(long)(i + 1)) != 0) {
            out("pthread_create failed\n");
            return 1;
        }
    for (int i = 0; i < 4; i++)
        pthread_join(th[i], 0);

    char buf[32];
    int p = 0;
    long v = counter;
    char tmp[32];
    int t = 0;
    if (v == 0)
        tmp[t++] = '0';
    while (v) {
        tmp[t++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (t)
        buf[p++] = tmp[--t];
    buf[p++] = '\n';
    out("counter=");
    write(1, buf, (size_t)p);
    return counter == 10000 ? 0 : 1;
}
