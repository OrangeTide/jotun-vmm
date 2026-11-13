/* linuxsendfile.c : a static Linux test program for the sendfile() syscall.
 *
 * sendfile copies bytes from a seekable input file to an output descriptor in
 * the kernel. This writes a payload to a file, sendfile()s it into a second
 * file (NULL offset, which advances the input position), reads it back, then
 * repeats with an explicit offset to confirm a partial copy and that the file
 * position is left untouched. Built by `make linux-spike`; run with
 * `make linux-spike SPIKE_PROG=linuxsendfile`. An ordinary Linux binary. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <sys/sendfile.h>
#include <unistd.h>

static void
puts2(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    const char *msg = "sendfile-payload-9876";
    size_t mlen = strlen(msg);

    int in = open("/sf.in", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (in < 0 || write(in, msg, mlen) != (ssize_t)mlen) {
        puts2("in setup failed\n");
        return 1;
    }
    lseek(in, 0, SEEK_SET);

    int out = open("/sf.out", O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (out < 0) {
        puts2("out open failed\n");
        return 1;
    }

    size_t done = 0;
    while (done < mlen) {
        ssize_t n = sendfile(out, in, NULL, mlen - done);   /* NULL: advance in pos */
        if (n < 0) {
            puts2("sendfile failed\n");
            return 1;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }

    char buf[64];
    ssize_t r = pread(out, buf, sizeof buf, 0);
    if (r != (ssize_t)mlen || memcmp(buf, msg, mlen) != 0) {
        puts2("sendfile content mismatch\n");
        return 1;
    }

    /* Explicit offset: copy 4 bytes from offset 2; the input position (now at
     * end of file from the loop above) must be left unchanged. */
    off_t off = 2;
    int out2 = open("/sf.out2", O_CREAT | O_RDWR | O_TRUNC, 0644);
    ssize_t n2 = sendfile(out2, in, &off, 4);
    if (n2 != 4 || off != 6) {
        puts2("sendfile offset wrong\n");
        return 1;
    }
    r = pread(out2, buf, sizeof buf, 0);
    if (r != 4 || memcmp(buf, msg + 2, 4) != 0) {
        puts2("sendfile offset content mismatch\n");
        return 1;
    }

    close(in);
    close(out);
    close(out2);
    puts2("sendfile ok\n");
    return 0;
}
