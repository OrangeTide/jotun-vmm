/* brokersmoke.c : an end-to-end smoke test of the brokerd supervisor.
 *
 * It spawns brokerd (which in turn forks one jotun child per session), then
 * drives two independent browser-like sessions through it. Each session first
 * does a cookieless GET / to obtain its own jsess cookie (brokerd spawns a child
 * and replies 302 Set-Cookie), then opens /ws carrying that cookie, types a
 * distinct token into its webterm, and reads back the rendered frames. The test
 * asserts isolation: each session sees only its own token, never the other's.
 * That proves cookie routing, per-session children, and address-space isolation.
 * Exits 0 on success, 1 on failure. Like websmoke, it speaks just enough of
 * RFC 6455 by hand and uses a fixed key. */
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

#define DEV_TEXT    5
#define DEV_PASTE   6
#define WORD_A      "ALPHA42"
#define WORD_B      "BRAVO99"

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

static int
write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

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

/* Masked client text frame carrying "guest node type\nvalue". */
static int
send_event(int fd, int node, int type, const char *value)
{
    char payload[128];
    int n = snprintf(payload, sizeof payload, "0 %d %d\n%s", node, type, value);
    if (n < 0 || n >= 126)
        return -1;
    unsigned char frame[160];
    unsigned char mask[4] = { 0x12, 0x34, 0x56, 0x78 };
    frame[0] = 0x81;
    frame[1] = (unsigned char)(0x80 | n);
    memcpy(frame + 2, mask, 4);
    for (int i = 0; i < n; i++)
        frame[6 + i] = (unsigned char)payload[i] ^ mask[i & 3];
    return write_all(fd, (char *)frame, 6 + (size_t)n);
}

/* Pull complete server frames out of rbuf, appending text payloads to acc. */
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

/* A browser-like session held open across the test. */
struct sess {
    int    fd;
    char   cookie[64];
    const char *word;
    unsigned char rbuf[1 << 20];
    size_t rlen;
    char   acc[1 << 20];
    size_t alen;
    int    handshook;
    int    sent;
};

/* Cookieless GET / to obtain a jsess cookie; brokerd spawns the child and
 * replies 302 Set-Cookie. Returns 0 and fills cookie, or -1. */
static int
get_cookie(int listen_port, char cookie[64])
{
    int fd = dial(listen_port);
    if (fd < 0)
        return -1;
    static const char req[] =
        "GET / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    if (write_all(fd, req, sizeof req - 1) != 0) {
        close(fd);
        return -1;
    }
    char buf[4096];
    size_t len = 0;
    long deadline = now_ms() + 5000;
    while (len < sizeof buf - 1 && now_ms() < deadline) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 300) <= 0)
            continue;
        ssize_t n = read(fd, buf + len, sizeof buf - 1 - len);
        if (n <= 0)
            break;
        len += (size_t)n;
    }
    close(fd);
    const char *c = mem_find(buf, len, "jsess=", 6);
    if (!c)
        return -1;
    c += 6;
    int k = 0;
    const char *end = buf + len;
    while (c < end && k < 63 && *c != ';' && *c != '\r' && *c != '\n')
        cookie[k++] = *c++;
    cookie[k] = '\0';
    return k > 0 ? 0 : -1;
}

/* Open /ws carrying the session cookie and complete the WebSocket handshake. */
static int
open_ws(struct sess *s, int listen_port)
{
    s->fd = dial(listen_port);
    if (s->fd < 0)
        return -1;
    char req[512];
    int n = snprintf(req, sizeof req,
                     "GET /ws HTTP/1.1\r\n"
                     "Host: 127.0.0.1\r\n"
                     "Upgrade: websocket\r\n"
                     "Connection: Upgrade\r\n"
                     "Cookie: jsess=%s\r\n"
                     "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                     "Sec-WebSocket-Version: 13\r\n\r\n", s->cookie);
    if (write_all(s->fd, req, (size_t)n) != 0) {
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    return 0;
}

/* Pump one session: consume readable bytes, finish the handshake, send the
 * echo command after the first frame. Returns -1 on a dead connection. */
static int
pump(struct sess *s)
{
    struct pollfd p = { .fd = s->fd, .events = POLLIN };
    if (poll(&p, 1, 200) > 0 && (p.revents & POLLIN)) {
        ssize_t n = read(s->fd, s->rbuf + s->rlen, sizeof s->rbuf - s->rlen);
        if (n <= 0)
            return -1;
        s->rlen += (size_t)n;
    }
    if (!s->handshook) {
        const char *end = mem_find((char *)s->rbuf, s->rlen, "\r\n\r\n", 4);
        if (!end)
            return 0;
        if (!mem_find((char *)s->rbuf, (size_t)(end - (char *)s->rbuf),
                      "101", 3)) {
            fprintf(stderr, "brokersmoke: no 101 upgrade for %s\n", s->word);
            return -1;
        }
        size_t hlen = (size_t)(end - (char *)s->rbuf) + 4;
        memmove(s->rbuf, s->rbuf + hlen, s->rlen - hlen);
        s->rlen -= hlen;
        s->handshook = 1;
    }
    int frames = drain(s->rbuf, &s->rlen, s->acc, &s->alen, sizeof s->acc);
    if (s->handshook && !s->sent && frames > 0) {
        s->sent = 1;
        char cmd[64];
        snprintf(cmd, sizeof cmd, "echo %s", s->word);
        if (send_event(s->fd, 1, DEV_PASTE, cmd) != 0 ||
            send_event(s->fd, 1, DEV_TEXT, "\n") != 0)
            return -1;
    }
    return 0;
}

static struct sess a, b;

static int
run_check(int listen_port)
{
    a.word = WORD_A;
    b.word = WORD_B;
    a.fd = b.fd = -1;

    /* Two cookieless GETs: brokerd spawns two independent children. */
    if (get_cookie(listen_port, a.cookie) != 0 ||
        get_cookie(listen_port, b.cookie) != 0) {
        fprintf(stderr, "brokersmoke: cookie fetch failed\n");
        return 1;
    }
    if (strcmp(a.cookie, b.cookie) == 0) {
        fprintf(stderr, "brokersmoke: both sessions share a cookie\n");
        return 1;
    }
    if (open_ws(&a, listen_port) != 0 || open_ws(&b, listen_port) != 0) {
        fprintf(stderr, "brokersmoke: /ws open failed\n");
        return 1;
    }

    long deadline = now_ms() + 8000;
    int ok = 0;
    while (now_ms() < deadline) {
        if (pump(&a) < 0 || pump(&b) < 0)
            break;
        int ca = count_occurrences(a.acc, a.alen, WORD_A);
        int cb = count_occurrences(b.acc, b.alen, WORD_B);
        if (ca >= 2 && cb >= 2) {
            ok = 1;
            break;
        }
    }

    /* Isolation: neither session may carry the other's token. */
    int a_leak = count_occurrences(a.acc, a.alen, WORD_B);
    int b_leak = count_occurrences(b.acc, b.alen, WORD_A);
    int ca = count_occurrences(a.acc, a.alen, WORD_A);
    int cb = count_occurrences(b.acc, b.alen, WORD_B);
    if (a.fd >= 0)
        close(a.fd);
    if (b.fd >= 0)
        close(b.fd);

    if (!ok) {
        fprintf(stderr, "brokersmoke: tokens not rendered "
                "(A %s=%d, B %s=%d)\n", WORD_A, ca, WORD_B, cb);
        fprintf(stderr, "  A: cookie=%.8s handshook=%d sent=%d bytes=%zu\n",
                a.cookie, a.handshook, a.sent, a.alen);
        fprintf(stderr, "  B: cookie=%.8s handshook=%d sent=%d bytes=%zu\n",
                b.cookie, b.handshook, b.sent, b.alen);
        return 1;
    }
    if (a_leak != 0 || b_leak != 0) {
        fprintf(stderr, "brokersmoke: ISOLATION BREACH "
                "(A saw %s %dx, B saw %s %dx)\n",
                WORD_B, a_leak, WORD_A, b_leak);
        return 1;
    }
    printf("brokersmoke: ok (A %s x%d, B %s x%d, no cross-leak)\n",
           WORD_A, ca, WORD_B, cb);
    return 0;
}

int
main(int argc, char **argv)
{
    const char *brokerd = argc > 1 ? argv[1] : "./brokerd";
    const char *jotun = argc > 2 ? argv[2] : "./jotun";
    const char *bundle = argc > 3 ? argv[3] : "bundle-depot";
    int listen_port = argc > 4 ? atoi(argv[4]) : 9240;
    char portbuf[16], basebuf[16];
    snprintf(portbuf, sizeof portbuf, "%d", listen_port);
    snprintf(basebuf, sizeof basebuf, "%d", listen_port + 60);

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
        setenv("JOTUN", jotun, 1);
        setenv("JOTUN_BUNDLE", bundle, 1);
        setenv("JOTUN_REF", "root", 1);
        setenv("JOTUN_BROKER_GUEST", "webterm", 1);
        setenv("JOTUN_BROKER_PORT", portbuf, 1);
        setenv("JOTUN_BROKER_PORTBASE", basebuf, 1);
        setenv("JOTUN_BROKER_MAX", "8", 1);
        setenv("JOTUN_BROKER_IDLE", "600", 1);
        char *args[] = { (char *)brokerd, NULL };
        execv(brokerd, args);
        perror("execv");
        _exit(127);
    }

    /* Wait for brokerd to listen. */
    int fd = -1;
    long deadline = now_ms() + 4000;
    while (now_ms() < deadline) {
        fd = dial(listen_port);
        if (fd >= 0) {
            close(fd);
            break;
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    int rc;
    if (fd < 0) {
        fprintf(stderr, "brokersmoke: brokerd never came up on %d\n",
                listen_port);
        rc = 1;
    } else {
        rc = run_check(listen_port);
    }

    kill(pid, SIGTERM);                 /* brokerd kills its children */
    waitpid(pid, NULL, 0);
    return rc;
}
