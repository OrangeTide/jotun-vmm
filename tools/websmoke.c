/* websmoke.c : an end-to-end smoke test of the web display path.
 *
 * It spawns the webterm guest with the draw server open, connects a WebSocket,
 * waits for the first rendered frame, types "echo <token>" and Enter as
 * DEV_PASTE/DEV_TEXT input events, and checks the token comes back rendered at
 * least twice (the echoed command line and the shell's output), which proves
 * the whole chain works: HTTP serve, WebSocket upgrade, frame stream, input
 * events, and the guest shell. Exits 0 on success, 1 on failure. It is a test
 * harness, so it speaks just enough of RFC 6455 by hand and uses a fixed key
 * (the client need not verify the server's accept hash). */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define TOKEN       "SMOKEPASS"
#define DEV_TEXT    5
#define DEV_PASTE   6

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

static int
dial(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_in a = { 0 };
    a.sin_family = AF_INET;
    a.sin_port = htons((unsigned short)port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Send a masked client text frame carrying "guest node type\nvalue". */
static int
send_event(int fd, int node, int type, const char *value)
{
    char payload[128];
    int n = snprintf(payload, sizeof payload, "0 %d %d\n%s", node, type, value);
    if (n < 0 || n >= 126)
        return -1;
    unsigned char frame[160];
    unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    frame[0] = 0x81;                    /* FIN + text */
    frame[1] = (unsigned char)(0x80 | n);
    memcpy(frame + 2, mask, 4);
    for (int i = 0; i < n; i++)
        frame[6 + i] = (unsigned char)payload[i] ^ mask[i & 3];
    size_t total = 6 + (size_t)n;
    return write(fd, frame, total) == (ssize_t)total ? 0 : -1;
}

/* Pull complete server frames (unmasked) out of rbuf, appending text payloads
 * to acc. Returns the number of frames consumed. */
static int
drain(unsigned char *rbuf, size_t *rlen, char *acc, size_t *alen, size_t acccap)
{
    int got = 0;
    size_t i = 0;
    for (;;) {
        if (*rlen - i < 2)
            break;
        unsigned op = rbuf[i] & 0x0f;
        unsigned l = rbuf[i + 1] & 0x7f;
        size_t off = i + 2;
        unsigned long len = l;
        if (l == 126) {
            if (*rlen - i < 4)
                break;
            len = ((unsigned long)rbuf[i + 2] << 8) | rbuf[i + 3];
            off = i + 4;
        } else if (l == 127) {
            if (*rlen - i < 10)
                break;
            len = 0;
            for (int k = 0; k < 8; k++)
                len = (len << 8) | rbuf[i + 2 + k];
            off = i + 10;
        }
        if (*rlen - off < len)
            break;
        if ((op == 0x1 || op == 0x0) && *alen + len < acccap) {
            memcpy(acc + *alen, rbuf + off, len);
            *alen += len;
        }
        got++;
        i = off + len;
    }
    if (i > 0) {
        memmove(rbuf, rbuf + i, *rlen - i);
        *rlen -= i;
    }
    return got;
}

/* Find needle within the first n bytes of hay; returns a pointer or NULL.
 * (memmem is a GNU extension not exposed under the build's feature macros.) */
static const char *
mem_find(const char *hay, size_t n, const char *needle, size_t nl)
{
    if (nl > n)
        return NULL;
    for (size_t i = 0; i + nl <= n; i++)
        if (memcmp(hay + i, needle, nl) == 0)
            return hay + i;
    return NULL;
}

static int
count_occurrences(const char *hay, size_t n, const char *needle)
{
    size_t nl = strlen(needle);
    int c = 0;
    for (size_t i = 0; i + nl <= n; i++)
        if (memcmp(hay + i, needle, nl) == 0)
            c++;
    return c;
}

static unsigned char rbuf[1 << 20];
static char acc[1 << 20];

static int
run_check(int port)
{
    int fd = dial(port);
    if (fd < 0) {
        fprintf(stderr, "websmoke: connect failed\n");
        return 1;
    }

    static const char req[] =
        "GET /ws HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n\r\n";
    if (write(fd, req, sizeof req - 1) != (ssize_t)(sizeof req - 1)) {
        fprintf(stderr, "websmoke: handshake write failed\n");
        close(fd);
        return 1;
    }

    size_t rlen = 0, alen = 0;
    int handshook = 0, sent = 0, result = 1;
    long deadline = now_ms() + 5000;

    while (now_ms() < deadline) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int pr = poll(&p, 1, 300);
        if (pr > 0 && (p.revents & POLLIN)) {
            ssize_t n = read(fd, rbuf + rlen, sizeof rbuf - rlen);
            if (n <= 0)
                break;
            rlen += (size_t)n;
        }

        if (!handshook) {
            /* Find the end of the HTTP response, verify 101, keep the rest. */
            const char *end = mem_find((char *)rbuf, rlen, "\r\n\r\n", 4);
            if (!end)
                continue;
            if (!mem_find((char *)rbuf, (size_t)(end - (char *)rbuf), "101", 3)) {
                fprintf(stderr, "websmoke: no 101 upgrade\n");
                break;
            }
            size_t hlen = (size_t)(end - (char *)rbuf) + 4;
            memmove(rbuf, rbuf + hlen, rlen - hlen);
            rlen -= hlen;
            handshook = 1;
        }

        int frames = drain(rbuf, &rlen, acc, &alen, sizeof acc);

        /* After the first rendered frame the guest is up; type the command. */
        if (handshook && !sent && frames > 0) {
            sent = 1;
            if (send_event(fd, 1, DEV_PASTE, "echo " TOKEN) != 0 ||
                send_event(fd, 1, DEV_TEXT, "\n") != 0) {
                fprintf(stderr, "websmoke: event write failed\n");
                break;
            }
        }

        if (sent && count_occurrences(acc, alen, TOKEN) >= 2) {
            result = 0;
            break;
        }
    }

    close(fd);
    if (result != 0)
        fprintf(stderr, "websmoke: token not rendered (handshook=%d sent=%d "
                "bytes=%zu count=%d)\n", handshook, sent, alen,
                count_occurrences(acc, alen, TOKEN));
    return result;
}

int
main(int argc, char **argv)
{
    const char *jotun = argc > 1 ? argv[1] : "./jotun";
    const char *bundle = argc > 2 ? argv[2] : "bundle-depot";
    int port = argc > 3 ? atoi(argv[3]) : 9299;
    char portbuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", port);

    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return 1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        setenv("JOTUN_BUNDLE", bundle, 1);
        setenv("JOTUN_REF", "root", 1);
        setenv("JOTUN_NAME", "smoke", 1);
        setenv("JOTUN_DRAW_PORT", portbuf, 1);
        char *args[] = { (char *)jotun, (char *)"libos.elf",
                         (char *)"webterm", NULL };
        execv(jotun, args);
        perror("execv");
        _exit(127);
    }

    /* Wait for the draw server to listen. */
    int fd = -1;
    long deadline = now_ms() + 4000;
    while (now_ms() < deadline) {
        fd = dial(port);
        if (fd >= 0) {
            close(fd);
            break;
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    int rc;
    if (fd < 0) {
        fprintf(stderr, "websmoke: draw server never came up on %d "
                "(is the bundle built? make bundle BUNDLE_ARCHS=<arch>)\n", port);
        rc = 1;
    } else {
        rc = run_check(port);
    }

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (rc == 0)
        printf("websmoke: ok\n");
    return rc;
}
