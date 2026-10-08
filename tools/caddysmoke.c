/* caddysmoke.c : a smoke test of the Caddy TLS front end over brokerd.
 *
 * make web-multi puts brokerd behind Caddy for TLS, exactly like make web does
 * for the single-session draw server. This test exercises that whole chain. It
 * forks brokerd on its default loopback port (9230) and Caddy with the committed
 * example/web/Caddyfile (TLS internal on 8443 -> 127.0.0.1:9230), then acts as a
 * browser reaching the site over TLS: it opens two cookieless GET / requests
 * through Caddy using openssl s_client as the TLS transport. Each must come back
 * 302 from brokerd (Server: Caddy, proving the request traversed the proxy) with
 * its own distinct jsess cookie, which means Caddy terminated TLS, reached
 * brokerd, and brokerd allocated a fresh per-session child for each. The deep
 * per-session isolation is covered by brokersmoke over the plain loopback port;
 * this test adds only the TLS front end. Exits 0 on success, 1 on failure.
 *
 * The listen port (9230) and TLS port (8443) are fixed to match the committed
 * Caddyfile. openssl s_client is the TLS client so the test needs no TLS code of
 * its own; it does not verify the internal CA, as a browser would after clicking
 * through the one-time warning. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define LISTEN_PORT 9230        /* brokerd loopback port (Caddyfile upstream) */
#define TLS_PORT    8443        /* Caddy TLS port (Caddyfile https://) */

#define STR2(x) #x
#define TOSTR(x) STR2(x)        /* stringify TLS_PORT into the openssl command */

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* GET / over TLS through Caddy, using openssl s_client as the transport. The
 * response head is captured into buf. Returns its length, or -1. */
static int
tls_get(char *buf, size_t cap)
{
    static const char cmd[] =
        "printf 'GET / HTTP/1.1\\r\\nHost: localhost\\r\\n"
        "Connection: close\\r\\n\\r\\n' | "
        "openssl s_client -quiet -connect 127.0.0.1:" TOSTR(TLS_PORT)
        " -servername localhost 2>/dev/null";
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    size_t len = 0;
    size_t n;
    while (len < cap - 1 &&
           (n = fread(buf + len, 1, cap - 1 - len, p)) > 0)
        len += n;
    buf[len] = '\0';
    pclose(p);
    return (int)len;
}

/* Lift the jsess cookie token out of a response head. Returns 0 and fills
 * cookie, or -1 if there is no Set-Cookie: jsess=... in the response. */
static int
parse_cookie(const char *resp, char cookie[64])
{
    const char *c = strstr(resp, "jsess=");
    if (!c)
        return -1;
    c += 6;
    int k = 0;
    while (*c && k < 63 && *c != ';' && *c != '\r' && *c != '\n')
        cookie[k++] = *c++;
    cookie[k] = '\0';
    return k > 0 ? 0 : -1;
}

static int
run_check(void)
{
    char a[8192], b[8192], ca[64], cb[64];

    if (tls_get(a, sizeof a) < 0 || tls_get(b, sizeof b) < 0) {
        fprintf(stderr, "caddysmoke: TLS GET failed (openssl s_client)\n");
        return 1;
    }
    if (!strstr(a, "HTTP/1.1 302") || !strstr(b, "HTTP/1.1 302")) {
        fprintf(stderr, "caddysmoke: no 302 from brokerd through Caddy\n");
        return 1;
    }
    if (!strstr(a, "Server: Caddy") || !strstr(b, "Server: Caddy")) {
        fprintf(stderr, "caddysmoke: response did not traverse Caddy\n");
        return 1;
    }
    if (parse_cookie(a, ca) != 0 || parse_cookie(b, cb) != 0) {
        fprintf(stderr, "caddysmoke: no jsess cookie set\n");
        return 1;
    }
    if (strcmp(ca, cb) == 0) {
        fprintf(stderr, "caddysmoke: both sessions share a cookie\n");
        return 1;
    }
    printf("caddysmoke: ok (two TLS sessions through Caddy, jsess %.8s / %.8s)\n",
           ca, cb);
    return 0;
}

/* fork + exec a child with stdio to /dev/null and an optional env override. */
static pid_t
spawn(char *const argv[], char *const env_key, const char *env_val)
{
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) {
            dup2(devnull, 0);           /* Caddy must not read the terminal */
            dup2(devnull, 1);           /* (its CA-install step may prompt) */
            dup2(devnull, 2);
        }
        if (env_key)
            setenv(env_key, env_val, 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

int
main(int argc, char **argv)
{
    const char *jotun = argc > 1 ? argv[1] : "./jotun";
    const char *bundle = argc > 2 ? argv[2] : "bundle-depot";
    const char *caddyfile = argc > 3 ? argv[3] : "example/web/Caddyfile";

    /* brokerd on 9230, children from 9300, the Caddyfile's upstream. */
    setenv("JOTUN", jotun, 1);
    setenv("JOTUN_BUNDLE", bundle, 1);
    setenv("JOTUN_REF", "root", 1);
    setenv("JOTUN_BROKER_GUEST", "webterm", 1);
    setenv("JOTUN_BROKER_IDLE", "600", 1);
    char *bargv[] = { "./brokerd", NULL };
    pid_t bpid = spawn(bargv, NULL, NULL);

    /* Caddy with its storage under build/ so the test touches no shared state. */
    char *cargv[] = { "caddy", "run", "--config", (char *)caddyfile, NULL };
    pid_t cpid = spawn(cargv, "XDG_DATA_HOME", "build/caddy");
    if (bpid < 0 || cpid < 0)
        return 1;

    /* Readiness: retry the TLS GET until Caddy answers with an HTTP status. */
    char buf[8192];
    int up = 0;
    long deadline = now_ms() + 15000;
    while (now_ms() < deadline) {
        if (tls_get(buf, sizeof buf) > 0 && strstr(buf, "HTTP/")) {
            up = 1;
            break;
        }
        struct timespec ts = { 0, 300 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    int rc;
    if (!up) {
        fprintf(stderr, "caddysmoke: Caddy never served TLS on %d "
                "(is caddy installed?)\n", TLS_PORT);
        rc = 1;
    } else {
        rc = run_check();
    }

    kill(cpid, SIGTERM);
    kill(bpid, SIGTERM);                /* brokerd kills its children */
    waitpid(cpid, NULL, 0);
    waitpid(bpid, NULL, 0);
    return rc;
}
