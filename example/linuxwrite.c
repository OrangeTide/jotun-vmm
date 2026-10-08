/* linuxwrite.c : a static Linux test for the in-memory (ramfs) write path.
 *
 * Creates a file with O_CREAT|O_WRONLY, writes two chunks, stats it to confirm
 * the size, then reopens it read-only and reads the contents back. The 9P
 * bundle is read-only, so this exercises the guest's writable ramfs overlay.
 * Run with `make linux-spike SPIKE_PROG=linuxwrite`. An ordinary Linux binary. */
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
    const char *path = "/tmp/scratch.txt";

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        puts2("create failed\n");
        return 1;
    }
    const char *a = "written to ramfs\n";
    const char *b = "and a second line\n";
    write(fd, a, strlen(a));
    write(fd, b, strlen(b));
    close(fd);

    struct stat st;
    if (stat(path, &st) == 0) {
        puts2("stat size=");
        putnum((long)st.st_size);
        puts2("\n");
    } else {
        puts2("stat failed\n");
    }

    int rfd = open(path, O_RDONLY);
    if (rfd < 0) {
        puts2("reopen failed\n");
        return 1;
    }
    puts2("read back:\n");
    char buf[128];
    long n;
    while ((n = read(rfd, buf, sizeof buf)) > 0)
        write(1, buf, (size_t)n);
    close(rfd);

    return 0;
}
