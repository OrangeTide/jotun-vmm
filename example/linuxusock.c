/* AF_UNIX stream round trip: a parent listens, a forked child connects, and
 * they exchange a request and reply over the socket. Exercises socket, bind,
 * listen, accept, connect, and read/write on the connected endpoints. Prints
 * "usock ok" on success. */
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include <stdio.h>

static const char *PATH = "/tmp/linuxusock.sock";

static void
fill_addr(struct sockaddr_un *a)
{
    memset(a, 0, sizeof *a);
    a->sun_family = AF_UNIX;
    strncpy(a->sun_path, PATH, sizeof a->sun_path - 1);
}

int
main(void)
{
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ls < 0) {
        printf("usock FAIL: socket\n");
        return 1;
    }
    struct sockaddr_un a;
    fill_addr(&a);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) < 0) {
        printf("usock FAIL: bind\n");
        return 1;
    }
    if (listen(ls, 8) < 0) {
        printf("usock FAIL: listen\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        printf("usock FAIL: fork\n");
        return 1;
    }
    if (pid == 0) {                     /* child: the client */
        int cs = socket(AF_UNIX, SOCK_STREAM, 0);
        struct sockaddr_un ca;
        fill_addr(&ca);
        if (cs < 0 || connect(cs, (struct sockaddr *)&ca, sizeof ca) < 0)
            _exit(2);
        if (write(cs, "ping", 4) != 4)
            _exit(3);
        char buf[8];
        ssize_t n = read(cs, buf, sizeof buf);
        if (n != 4 || memcmp(buf, "pong", 4) != 0)
            _exit(4);
        close(cs);
        _exit(0);
    }

    /* parent: the server */
    int as = accept(ls, NULL, NULL);
    if (as < 0) {
        printf("usock FAIL: accept\n");
        return 1;
    }
    char buf[8];
    ssize_t n = read(as, buf, sizeof buf);
    if (n != 4 || memcmp(buf, "ping", 4) != 0) {
        printf("usock FAIL: read ping (n=%ld)\n", (long)n);
        return 1;
    }
    if (write(as, "pong", 4) != 4) {
        printf("usock FAIL: write pong\n");
        return 1;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("usock FAIL: child status %d\n", status);
        return 1;
    }
    close(as);
    close(ls);
    printf("usock ok\n");
    return 0;
}
