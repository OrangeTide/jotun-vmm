/* linuxpipe.c : a static Linux test program for the M16.2 pipe path.
 *
 * Creates a pipe, writes a string into the write end, reads it back from the
 * read end, and prints it. It uses the raw pipe/write/read/close wrappers (no
 * heap), exercising the in-memory pipe the libOS provides. musl's pipe() is a
 * pipe2 syscall. Built by `make linux-spike`; run with
 * `make linux-spike SPIKE_PROG=linuxpipe`. An ordinary Linux binary. */
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
    int fd[2];

    if (pipe(fd) < 0) {
        puts2("pipe failed\n");
        return 1;
    }

    const char *msg = "data through a pipe\n";
    write(fd[1], msg, strlen(msg));

    char buf[64];
    long n = read(fd[0], buf, sizeof buf);
    puts2("read back: ");
    if (n > 0)
        write(1, buf, (size_t)n);

    close(fd[0]);
    close(fd[1]);
    return 0;
}
