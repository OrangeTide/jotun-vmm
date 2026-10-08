/* linuxcat.c : a static Linux test program for the M16.2 VFS syscalls.
 *
 * Opens two bundle files, prints their size from fstat, and cats their
 * contents, then stats a path directly and checks that a missing path fails.
 * It uses the raw open/read/write/close/fstat/stat wrappers, which musl issues
 * as direct syscalls with no heap, so it exercises the file syscalls the libOS
 * services over the read-only 9P bundle. Built by `make linux-spike`; run with
 * `make linux-spike SPIKE_PROG=linuxcat`. Not a Jotun program: it is an
 * ordinary Linux binary. */
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static void
putnum(long v)
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
    const char *names[] = { "hello.txt", "motd" };
    char buf[256];

    for (int i = 0; i < 2; i++) {
        int fd = open(names[i], O_RDONLY);
        if (fd < 0) {
            puts2("open failed: ");
            puts2(names[i]);
            puts2("\n");
            continue;
        }
        struct stat st;
        if (fstat(fd, &st) == 0) {
            puts2(names[i]);
            puts2(" (");
            putnum((long)st.st_size);
            puts2(" bytes):\n");
        }
        long n;
        while ((n = read(fd, buf, sizeof buf)) > 0)
            write(1, buf, (size_t)n);
        close(fd);
    }

    struct stat st;
    if (stat("motd", &st) == 0) {
        puts2("stat motd size=");
        putnum((long)st.st_size);
        puts2("\n");
    }
    if (stat("nonexistent", &st) < 0)
        puts2("stat nonexistent -> error (as expected)\n");

    return 0;
}
