/* linuxdev.c : a static Linux test for the guest's synthetic /dev.
 *
 * Exercises the character devices the libOS emulates in-guest (no host round
 * trip): null, zero, full, random, urandom, tty/console, and the std* nodes.
 * Also checks that stat reports a character device and that /dev lists its
 * nodes. Run with `make linux-example EXAMPLE_PROG=linuxdev`. An ordinary Linux
 * binary. */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

static int fails;

static void
check(int ok, const char *what)
{
    puts2(ok ? "[ok]   " : "[FAIL] ");
    puts2(what);
    puts2("\n");
    if (!ok)
        fails++;
}

int
main(void)
{
    char buf[64];

    /* /dev/null: write is swallowed, read is immediate EOF. */
    int fd = open("/dev/null", O_RDWR);
    check(fd >= 0, "open /dev/null");
    check(write(fd, "discard me", 10) == 10, "write /dev/null returns count");
    check(read(fd, buf, sizeof buf) == 0, "read /dev/null is EOF");
    close(fd);

    /* /dev/zero: an endless run of zero bytes. */
    fd = open("/dev/zero", O_RDONLY);
    check(fd >= 0, "open /dev/zero");
    memset(buf, 0xff, sizeof buf);
    long n = read(fd, buf, sizeof buf);
    int allzero = (n == (long)sizeof buf);
    for (long i = 0; i < n; i++)
        if (buf[i] != 0)
            allzero = 0;
    check(allzero, "read /dev/zero gives zeros");
    close(fd);

    /* /dev/full: reads zeros, but a write fails with ENOSPC. */
    fd = open("/dev/full", O_RDWR);
    check(fd >= 0, "open /dev/full");
    check(read(fd, buf, 8) == 8, "read /dev/full gives data");
    errno = 0;
    check(write(fd, "x", 1) < 0 && errno == ENOSPC, "write /dev/full is ENOSPC");
    close(fd);

    /* /dev/urandom: fills the buffer; not all bytes should be equal. */
    fd = open("/dev/urandom", O_RDONLY);
    check(fd >= 0, "open /dev/urandom");
    memset(buf, 0, sizeof buf);
    n = read(fd, buf, 32);
    int varied = (n == 32);
    int differ = 0;
    for (int i = 1; i < 32; i++)
        if (buf[i] != buf[0])
            differ = 1;
    check(varied && differ, "read /dev/urandom varies");
    close(fd);

    /* A shell redirect `>/dev/null` opens with O_CREAT|O_TRUNC. It must still
     * reach the device, not fall through and create an overlay regular file. */
    fd = open("/dev/null", O_WRONLY | O_CREAT | O_TRUNC, 0666);
    check(fd >= 0 && write(fd, "redir", 5) == 5, "redirect >/dev/null swallows");
    if (fd >= 0)
        close(fd);

    /* stat reports a character device, not a regular file. */
    struct stat st;
    check(stat("/dev/null", &st) == 0 && S_ISCHR(st.st_mode), "stat /dev/null is CHR");
    check(stat("/dev", &st) == 0 && S_ISDIR(st.st_mode), "stat /dev is a directory");

    /* /dev lists its nodes. */
    DIR *d = opendir("/dev");
    check(d != NULL, "opendir /dev");
    int saw_null = 0, saw_zero = 0, saw_random = 0;
    if (d) {
        struct dirent *de;
        while ((de = readdir(d))) {
            if (strcmp(de->d_name, "null") == 0)
                saw_null = 1;
            else if (strcmp(de->d_name, "zero") == 0)
                saw_zero = 1;
            else if (strcmp(de->d_name, "random") == 0)
                saw_random = 1;
        }
        closedir(d);
    }
    check(saw_null && saw_zero && saw_random, "readdir /dev sees the nodes");

    /* /dev/stdout reaches the console. */
    fd = open("/dev/stdout", O_WRONLY);
    check(fd >= 0, "open /dev/stdout");
    if (fd >= 0) {
        write(fd, "dev: hello via /dev/stdout\n", 27);
        close(fd);
    }

    puts2(fails ? "linuxdev: FAILED\n" : "linuxdev: all ok\n");
    return fails ? 1 : 0;
}
