/* cauthsmoke.c : an end-to-end check of the authenticated Caddy chain.
 *
 * auth-smoke tests brokerd's admission by injecting the trusted header directly,
 * so it never exercises Caddy's forward_auth wiring. This test fills that gap: it
 * forks authd and brokerd with a shared secret and Caddy with
 * example/web/Caddyfile-auth, mints a token with jmint, and drives the real TLS
 * chain with curl. It checks that a request with no token is refused at the edge
 * (401), a bearer token passes (302 from brokerd through forward_auth), /login
 * sets the jauth cookie, that cookie then passes on its own (the browser path,
 * since a WebSocket cannot carry a header), and a bad token is refused. Exits 0
 * on success, 1 on failure.
 *
 * The ports (brokerd 9230, authd 9231, TLS 8443) match the committed
 * Caddyfile-auth. Needs caddy and curl on the PATH and a bundle. */
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SECRET      "caddy-auth-smoke-secret"
#define STORE       "build/cauthsmoke.tokens"
#define COOKIEJAR   "build/cauthsmoke.cj"
#define BASEURL     "https://localhost:8443"

static int fails;

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Run curl with the common flags plus tail, returning the HTTP status code it
 * printed (via -w). code must hold at least 8 bytes. */
static void
curl_code(const char *tail, char *code, size_t n)
{
    char cmd[1024];
    snprintf(cmd, sizeof cmd,
             "curl -k -s -o /dev/null -w '%%{http_code}' %s", tail);
    FILE *p = popen(cmd, "r");
    size_t got = p ? fread(code, 1, n - 1, p) : 0;
    code[got] = '\0';
    if (p)
        pclose(p);
}

static void
expect_code(const char *tail, const char *want, const char *what)
{
    char code[16];
    curl_code(tail, code, sizeof code);
    if (strcmp(code, want) != 0) {
        fails++;
        printf("  FAIL %-28s got=%s want=%s\n", what, code, want);
    }
}

/* fork + exec with stdio to /dev/null and an optional single env override. */
static pid_t
spawn(char *const argv[], const char *env_key, const char *env_val)
{
    pid_t pid = fork();
    if (pid < 0)
        return -1;
    if (pid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) {
            dup2(nul, 0);
            dup2(nul, 1);
            dup2(nul, 2);
        }
        if (env_key)
            setenv(env_key, env_val, 1);
        execvp(argv[0], argv);
        _exit(127);
    }
    return pid;
}

/* Mint a token for user via jmint, returning it in tok (33 bytes). */
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
        execvp(jmint, args);
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

int
main(int argc, char **argv)
{
    const char *brokerd = argc > 1 ? argv[1] : "./brokerd";
    const char *authd = argc > 2 ? argv[2] : "./authd";
    const char *jmint = argc > 3 ? argv[3] : "./jmint";
    const char *jotun = argc > 4 ? argv[4] : "./jotun";
    const char *bundle = argc > 5 ? argv[5] : "bundle-depot";
    const char *caddyfile = argc > 6 ? argv[6] : "example/web/Caddyfile-auth";

    unlink(STORE);
    unlink(COOKIEJAR);
    char tok[33];
    if (mint(jmint, "demo", "5", tok) != 0) {
        fprintf(stderr, "cauthsmoke: mint failed\n");
        return 1;
    }

    char e_store[128], e_secret[128], e_jotun[256], e_bundle[256];
    snprintf(e_store, sizeof e_store, "JOTUN_AUTH_TOKENS=%s", STORE);
    snprintf(e_secret, sizeof e_secret, "JOTUN_BROKER_AUTH_SECRET=%s", SECRET);
    snprintf(e_jotun, sizeof e_jotun, "JOTUN=%s", jotun);
    snprintf(e_bundle, sizeof e_bundle, "JOTUN_BUNDLE=%s", bundle);

    /* authd: store + secret (loopback 9231, matching the Caddyfile). */
    char *authargv[] = { (char *)authd, NULL };
    pid_t apid = fork();
    if (apid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        putenv(e_store);
        putenv(e_secret);
        execvp(authargv[0], authargv);
        _exit(127);
    }

    /* brokerd: same secret, the bundle (loopback 9230). */
    char *brokargv[] = { (char *)brokerd, NULL };
    pid_t bpid = fork();
    if (bpid == 0) {
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        putenv(e_jotun);
        putenv(e_bundle);
        putenv(e_secret);
        putenv((char *)"JOTUN_REF=root");
        putenv((char *)"JOTUN_BROKER_GUEST=webterm");
        putenv((char *)"JOTUN_BROKER_IDLE=600");
        execvp(brokargv[0], brokargv);
        _exit(127);
    }

    /* Caddy with its storage under build/ so the test touches no shared state. */
    char *cargv[] = { "caddy", "run", "--config", (char *)caddyfile, NULL };
    pid_t cpid = spawn(cargv, "XDG_DATA_HOME", "build/caddy");

    if (apid < 0 || bpid < 0 || cpid < 0) {
        fprintf(stderr, "cauthsmoke: fork failed\n");
        return 1;
    }

    /* Readiness: retry an unauthenticated request until Caddy answers. */
    int up = 0;
    long deadline = now_ms() + 15000;
    while (now_ms() < deadline) {
        char code[16];
        curl_code(BASEURL "/", code, sizeof code);
        if (code[0] && strcmp(code, "000") != 0) {
            up = 1;
            break;
        }
        struct timespec ts = { 0, 300 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }

    int rc;
    if (!up) {
        fprintf(stderr, "cauthsmoke: Caddy never served (is caddy installed?)\n");
        rc = 1;
    } else {
        char t[1200];

        expect_code(BASEURL "/", "401", "no token -> 401");

        snprintf(t, sizeof t, "-H 'Authorization: Bearer %s' " BASEURL "/", tok);
        expect_code(t, "302", "bearer -> 302");

        snprintf(t, sizeof t, "-c %s '" BASEURL "/login?token=%s'", COOKIEJAR, tok);
        expect_code(t, "302", "login -> 302");

        FILE *cj = fopen(COOKIEJAR, "r");
        int have_cookie = 0;
        if (cj) {
            char line[512];
            while (fgets(line, sizeof line, cj))
                if (strstr(line, "jauth"))
                    have_cookie = 1;
            fclose(cj);
        }
        if (!have_cookie) {
            fails++;
            printf("  FAIL %-28s (no jauth in cookie jar)\n", "login sets cookie");
        }

        snprintf(t, sizeof t, "-b %s " BASEURL "/", COOKIEJAR);
        expect_code(t, "302", "cookie -> 302");

        expect_code("-H 'Authorization: Bearer deadbeef' " BASEURL "/", "401",
                    "bad token -> 401");

        rc = fails ? 1 : 0;
    }

    kill(cpid, SIGTERM);
    kill(bpid, SIGTERM);                /* brokerd kills its children */
    kill(apid, SIGTERM);
    waitpid(cpid, NULL, 0);
    waitpid(bpid, NULL, 0);
    waitpid(apid, NULL, 0);
    unlink(STORE);
    unlink(COOKIEJAR);

    if (rc == 0)
        printf("cauthsmoke: ok\n");
    else
        printf("cauthsmoke: %d failure(s)\n", fails);
    return rc;
}
