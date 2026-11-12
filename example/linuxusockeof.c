/* Regression probe: a peer that exits without close() must still deliver EOF to
 * the other end. The child connects, sends one byte, then _exit()s without
 * closing its socket. The parent accepts, reads the byte, then reads again and
 * must see 0 (EOF) because process exit releases the child's socket and marks
 * the parent's end peer-closed. Prints "usockeof ok" on the EOF. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

int
main(void)
{
    const char *path = "usockeof.sock";
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    strcpy(a.sun_path, path);
    bind(ls, (struct sockaddr *)&a, sizeof a);
    listen(ls, 4);

    pid_t c = fork();
    if (c == 0) {
        int s = socket(AF_UNIX, SOCK_STREAM, 0);
        connect(s, (struct sockaddr *)&a, sizeof a);
        write(s, "x", 1);
        _exit(0);                          /* exit WITHOUT close(s) */
    }

    int cs = accept(ls, 0, 0);
    char buf[8];
    ssize_t n = read(cs, buf, sizeof buf); /* the one byte */
    if (n != 1 || buf[0] != 'x') {
        printf("FAIL: first read n=%zd\n", n);
        return 1;
    }
    n = read(cs, buf, sizeof buf);         /* must be EOF after child exit */
    if (n == 0)
        printf("usockeof ok\n");
    else
        printf("FAIL: expected EOF, got n=%zd\n", n);
    waitpid(c, 0, 0);
    fflush(stdout);
    return 0;
}
