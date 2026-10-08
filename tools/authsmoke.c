/* authsmoke.c : an end-to-end check of M15.3 admission and auth.
 *
 * It mints two users into a token store with jmint (userA cap 2, userB cap 1),
 * starts authd over that store with a shared secret, and starts brokerd with the
 * same secret. First it checks authd directly: a valid bearer token returns 200
 * with the expected X-Jotun-Auth identity, a bad token 401, a jauth cookie works
 * too, and /login sets the cookie. Then it checks brokerd's admission by playing
 * the part of Caddy, injecting the X-Jotun-Auth header authd would have vouched
 * for: each user may open sessions up to its cap and is refused (503) past it,
 * users are independent, and a request with no or a wrong secret is refused
 * (403). Exits 0 on success, 1 on failure.
 *
 * Driving brokerd directly with the trusted header, rather than standing up
 * Caddy's forward_auth, keeps the admission logic under test on its own; the
 * Caddyfile wiring is validated separately (example/web/Caddyfile-auth). */
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

#define SECRET      "s3cr3t-shared-abc"
#define AUTH_PORT   9251
#define BROK_PORT   9250
#define BROK_BASE   9260
#define STORE       "build/authsmoke.tokens"

static int fails;

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
write_all(int fd, const char *b, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, b + off, n - off);
        if (w < 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

static int
wait_ready(int port)
{
    long deadline = now_ms() + 4000;
    while (now_ms() < deadline) {
        int fd = dial(port);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        struct timespec ts = { 0, 100 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    return -1;
}

/* Send one request to a loopback port and read the whole response (the servers
 * all set Connection: close). Returns length, response NUL-terminated in buf. */
static int
http(int port, const char *path, const char *extra, char *buf, int cap)
{
    int fd = dial(port);
    if (fd < 0)
        return -1;
    char req[1024];
    int n = snprintf(req, sizeof req,
                     "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n%sConnection: close\r\n\r\n",
                     path, extra ? extra : "");
    if (write_all(fd, req, (size_t)n) != 0) {
        close(fd);
        return -1;
    }
    int len = 0;
    long deadline = now_ms() + 6000;
    while (len < cap - 1 && now_ms() < deadline) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        if (poll(&p, 1, 500) <= 0)
            continue;
        ssize_t r = read(fd, buf + len, cap - 1 - len);
        if (r <= 0)
            break;
        len += (int)r;
    }
    buf[len] = '\0';
    close(fd);
    return len;
}

static int
status_is(const char *resp, const char *code)
{
    /* First line: HTTP/1.1 <code> ... */
    const char *sp = strchr(resp, ' ');
    return sp && strncmp(sp + 1, code, strlen(code)) == 0;
}

static void
expect(int cond, const char *what)
{
    if (!cond) {
        fails++;
        printf("  FAIL %s\n", what);
    }
}

/* Mint a token for user with cap via jmint, returning it in tok (33 bytes). */
static int
mint(const char *jmint, const char *user, const char *cap, char tok[33])
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        setenv("JOTUN_AUTH_TOKENS", STORE, 1);
        char *args[] = { (char *)jmint, (char *)user, (char *)cap, NULL };
        execv(jmint, args);
        _exit(127);
    }
    close(fds[1]);
    char b[64];
    int len = 0;
    ssize_t r;
    while (len < 33 && (r = read(fds[0], b + len, sizeof b - len)) > 0)
        len += (int)r;
    close(fds[0]);
    waitpid(pid, NULL, 0);
    if (len < 32)
        return -1;
    memcpy(tok, b, 32);
    tok[32] = '\0';
    return 0;
}

static pid_t
spawn(char *const argv[], char *const envp[])
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_WRONLY);
        if (nul >= 0) {
            dup2(nul, 1);
            dup2(nul, 2);
        }
        for (char *const *e = envp; *e; e++)
            putenv(*e);
        execv(argv[0], argv);
        _exit(127);
    }
    return pid;
}

int
main(int argc, char **argv)
{
    const char *brokerd = argc > 1 ? argv[1] : "./brokerd";
    const char *authd = argc > 2 ? argv[2] : "./authd";
    const char *jmint = argc > 3 ? argv[3] : "./jmint";
    const char *jotun = argc > 4 ? argv[4] : "./jotun";
    const char *bundle = argc > 5 ? argv[5] : "bundle-depot";

    unlink(STORE);
    char tokA[33], tokB[33];
    if (mint(jmint, "alice", "2", tokA) != 0 ||
        mint(jmint, "bob", "1", tokB) != 0) {
        fprintf(stderr, "authsmoke: mint failed\n");
        return 1;
    }

    char e_store[128], e_secret[128], e_aport[32];
    snprintf(e_store, sizeof e_store, "JOTUN_AUTH_TOKENS=%s", STORE);
    snprintf(e_secret, sizeof e_secret, "JOTUN_BROKER_AUTH_SECRET=%s", SECRET);
    snprintf(e_aport, sizeof e_aport, "JOTUN_AUTH_PORT=%d", AUTH_PORT);
    char *authenv[] = { e_store, e_secret, e_aport, NULL };
    char *authargv[] = { (char *)authd, NULL };
    pid_t apid = spawn(authargv, authenv);

    char e_jotun[256], e_bundle[256], e_bport[32], e_base[32], e_max[32];
    snprintf(e_jotun, sizeof e_jotun, "JOTUN=%s", jotun);
    snprintf(e_bundle, sizeof e_bundle, "JOTUN_BUNDLE=%s", bundle);
    snprintf(e_bport, sizeof e_bport, "JOTUN_BROKER_PORT=%d", BROK_PORT);
    snprintf(e_base, sizeof e_base, "JOTUN_BROKER_PORTBASE=%d", BROK_BASE);
    snprintf(e_max, sizeof e_max, "JOTUN_BROKER_MAX=%d", 8);
    char *brokenv[] = {
        e_jotun, e_bundle, e_secret, e_bport, e_base, e_max,
        (char *)"JOTUN_REF=root", (char *)"JOTUN_BROKER_GUEST=webterm",
        (char *)"JOTUN_BROKER_IDLE=600", NULL,
    };
    char *brokargv[] = { (char *)brokerd, NULL };
    pid_t bpid = spawn(brokargv, brokenv);

    if (apid < 0 || bpid < 0 || wait_ready(AUTH_PORT) != 0 ||
        wait_ready(BROK_PORT) != 0) {
        fprintf(stderr, "authsmoke: authd/brokerd did not come up\n");
        if (apid > 0) kill(apid, SIGTERM);
        if (bpid > 0) kill(bpid, SIGTERM);
        return 1;
    }

    char buf[8192];
    char hdr[256], want[128];

    /* authd: valid bearer -> 200 with the expected identity. */
    snprintf(hdr, sizeof hdr, "Authorization: Bearer %s\r\n", tokA);
    http(AUTH_PORT, "/auth", hdr, buf, sizeof buf);
    snprintf(want, sizeof want, "X-Jotun-Auth: %s alice 2", SECRET);
    expect(status_is(buf, "200") && strstr(buf, want) != NULL,
           "authd bearer alice -> 200 + identity");

    /* authd: bad token -> 401. */
    http(AUTH_PORT, "/auth",
         "Authorization: Bearer 00000000000000000000000000000000\r\n",
         buf, sizeof buf);
    expect(status_is(buf, "401"), "authd bad token -> 401");

    /* authd: jauth cookie also works. */
    snprintf(hdr, sizeof hdr, "Cookie: jauth=%s\r\n", tokB);
    http(AUTH_PORT, "/auth", hdr, buf, sizeof buf);
    snprintf(want, sizeof want, "X-Jotun-Auth: %s bob 1", SECRET);
    expect(status_is(buf, "200") && strstr(buf, want) != NULL,
           "authd cookie bob -> 200 + identity");

    /* authd: /login sets the cookie. */
    snprintf(hdr, sizeof hdr, "/login?token=%s", tokA);
    http(AUTH_PORT, hdr, NULL, buf, sizeof buf);
    snprintf(want, sizeof want, "Set-Cookie: jauth=%s", tokA);
    expect(status_is(buf, "302") && strstr(buf, want) != NULL,
           "authd /login -> 302 + Set-Cookie");

    /* brokerd admission, playing Caddy with the trusted header. */
    char ha[128], hb[128];
    snprintf(ha, sizeof ha, "X-Jotun-Auth: %s alice 2\r\n", SECRET);
    snprintf(hb, sizeof hb, "X-Jotun-Auth: %s bob 1\r\n", SECRET);

    http(BROK_PORT, "/", ha, buf, sizeof buf);
    expect(status_is(buf, "302"), "alice session 1 -> 302");
    http(BROK_PORT, "/", ha, buf, sizeof buf);
    expect(status_is(buf, "302"), "alice session 2 -> 302");
    http(BROK_PORT, "/", ha, buf, sizeof buf);
    expect(status_is(buf, "503"), "alice session 3 -> 503 (cap 2)");

    http(BROK_PORT, "/", hb, buf, sizeof buf);
    expect(status_is(buf, "302"), "bob session 1 -> 302 (independent)");
    http(BROK_PORT, "/", hb, buf, sizeof buf);
    expect(status_is(buf, "503"), "bob session 2 -> 503 (cap 1)");

    http(BROK_PORT, "/", NULL, buf, sizeof buf);
    expect(status_is(buf, "403"), "no identity -> 403");
    http(BROK_PORT, "/", "X-Jotun-Auth: wrong-secret eve 9\r\n", buf, sizeof buf);
    expect(status_is(buf, "403"), "wrong secret -> 403");

    kill(bpid, SIGTERM);
    kill(apid, SIGTERM);
    waitpid(bpid, NULL, 0);
    waitpid(apid, NULL, 0);
    unlink(STORE);

    if (fails == 0)
        printf("authsmoke: ok\n");
    else
        printf("authsmoke: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
