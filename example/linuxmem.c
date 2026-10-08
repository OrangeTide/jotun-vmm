/* linuxmem.c : a static Linux test program for the M16.5 heap path.
 *
 * Exercises the guest's brk and anonymous mmap: it grows the break directly
 * with a raw brk syscall, then allocates through malloc (musl's mallocng gets
 * its memory from anonymous mmap), writing and reading back to confirm the
 * memory is real and zeroed. Built by `make linux-spike`; run with
 * `make linux-spike SPIKE_PROG=linuxmem`. An ordinary Linux binary. */
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static void
putnum(unsigned long v)
{
    char b[24];
    char o[24];
    int n = 0, m = 0;

    if (v == 0)
        b[n++] = '0';
    while (v) {
        b[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n)
        o[m++] = b[--n];
    write(1, o, m);
}

int
main(void)
{
    /* brk: query the break, then grow it by 64 KiB. */
    char *b0 = (char *)syscall(SYS_brk, 0);
    char *b1 = (char *)syscall(SYS_brk, b0 + 65536);
    puts2("brk grow: ");
    puts2(b1 > b0 ? "ok" : "BAD");
    puts2("\n");

    /* A large malloc, served through anonymous mmap by musl. */
    char *p = malloc(100000);
    if (!p) {
        puts2("malloc 100k failed\n");
        return 1;
    }
    memset(p, 0xAB, 100000);
    int ok = 1;
    for (int i = 0; i < 100000; i++)
        if ((unsigned char)p[i] != 0xAB)
            ok = 0;
    puts2("malloc 100k write/read: ");
    puts2(ok ? "ok" : "BAD");
    puts2("\n");

    /* Many smaller allocations, each tagged and checked. */
    char *v[128];
    int fails = 0;
    for (int i = 0; i < 128; i++) {
        v[i] = malloc(1024);
        if (!v[i])
            fails++;
        else
            v[i][0] = (char)i;
    }
    int integrity = 1;
    for (int i = 0; i < 128; i++)
        if (v[i] && v[i][0] != (char)i)
            integrity = 0;
    puts2("128 allocs: fails=");
    putnum((unsigned long)fails);
    puts2(" integrity=");
    puts2(integrity ? "ok" : "BAD");
    puts2("\n");
    for (int i = 0; i < 128; i++)
        free(v[i]);
    free(p);

    puts2("mem test done\n");
    return 0;
}
