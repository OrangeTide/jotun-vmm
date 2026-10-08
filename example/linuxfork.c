/* linuxfork : exercise fork, waitpid, and a pipe across a fork in the Linux
 * binary-compat guest. Built static with musl. */
#include <unistd.h>
#include <sys/wait.h>
#include <string.h>

static void
out(const char *s)
{
    write(1, s, strlen(s));
}

int
main(void)
{
    /* 1: a plain fork, with the child's exit status collected by the parent. */
    pid_t p = fork();
    if (p == 0) {
        out("child: hello\n");
        _exit(7);
    }
    int st = 0;
    pid_t w = waitpid(p, &st, 0);
    if (w == p && WIFEXITED(st) && WEXITSTATUS(st) == 7)
        out("parent: child exited 7 ok\n");
    else
        out("parent: WAIT FAIL\n");

    /* 2: a pipe from a child writer to the parent reader. The parent's read
     * blocks until the child writes and closes its end. */
    int fd[2];
    if (pipe(fd) != 0) {
        out("pipe fail\n");
        return 1;
    }
    pid_t p2 = fork();
    if (p2 == 0) {
        close(fd[0]);
        const char *m = "data through a fork pipe\n";
        write(fd[1], m, strlen(m));
        close(fd[1]);
        _exit(0);
    }
    close(fd[1]);
    char buf[64];
    ssize_t n = read(fd[0], buf, sizeof buf);
    if (n > 0) {
        out("parent read: ");
        write(1, buf, (size_t)n);
    } else {
        out("parent: READ FAIL\n");
    }
    waitpid(p2, &st, 0);
    out("done\n");
    return 0;
}
