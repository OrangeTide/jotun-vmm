/* linuxsplice.c : a static Linux test program for the splice() syscall.
 *
 * splice moves bytes between two descriptors, one of which must be a pipe,
 * without copying through userspace. This writes a known payload into a pipe,
 * splices it into an overlay file, and reads it back to confirm the bytes
 * survived. It then splices a second pipe's contents into /dev/null, which is
 * the idiom GNU grep uses to drain its input when the match result is all that
 * matters. Built by `make linux-example`; run with
 * `make linux-example EXAMPLE_PROG=linuxsplice`. An ordinary Linux binary. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    const char *msg = "spliced-payload-1234";
    size_t mlen = strlen(msg);

    int fd[2];
    if (pipe(fd) < 0) {
        puts2("pipe failed\n");
        return 1;
    }
    if (write(fd[1], msg, mlen) != (ssize_t)mlen) {
        puts2("pipe write failed\n");
        return 1;
    }
    close(fd[1]);                       /* writer gone: splice sees EOF after the data */

    int out = open("/sp.out", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (out < 0) {
        puts2("open failed\n");
        return 1;
    }

    for (;;) {
        ssize_t n = splice(fd[0], NULL, out, NULL, 4096, 0);
        if (n < 0) {
            puts2("splice to file failed\n");
            return 1;
        }
        if (n == 0)
            break;                      /* pipe drained */
    }
    close(fd[0]);

    char buf[64];
    ssize_t r = pread(out, buf, sizeof buf, 0);
    close(out);
    if (r != (ssize_t)mlen || memcmp(buf, msg, mlen) != 0) {
        puts2("splice content mismatch\n");
        return 1;
    }

    /* pipe -> /dev/null: the drain idiom, which must consume the bytes and
     * report the count without an error. */
    int p2[2];
    if (pipe(p2) < 0) {
        puts2("pipe2 failed\n");
        return 1;
    }
    write(p2[1], msg, mlen);
    close(p2[1]);
    int nul = open("/dev/null", O_WRONLY);
    if (nul < 0) {
        puts2("open /dev/null failed\n");
        return 1;
    }
    ssize_t d = splice(p2[0], NULL, nul, NULL, 4096, 0);
    close(p2[0]);
    close(nul);
    if (d != (ssize_t)mlen) {
        puts2("splice to /dev/null bad count\n");
        return 1;
    }

    puts2("splice ok\n");
    return 0;
}
