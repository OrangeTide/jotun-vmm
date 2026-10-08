/* brokerd.c : a process-per-session web hosting supervisor.
 *
 * The host-mechanism half of M15.2. It fronts the Jotun draw server for
 * multiple browsers at once: each session gets its own jotun child process (the
 * ordinary single-guest stack with its own loopback draw port), and brokerd
 * proxies the browser's connections to that child. A jsess cookie ties a
 * browser's page request and its WebSocket to the same child. Teardown is
 * kill-and-reap, so a session leaks nothing into the others and the trusted VMM
 * core needs no changes.
 *
 * The policy here is trivial and inline: a fixed bundle and guest per session.
 * The M15.2 plan lifts policy into a sandboxed broker guest later; the spawn and
 * proxy mechanism below is what that guest will drive.
 *
 * TLS and any real authentication live in a reverse proxy in front (Caddy), per
 * the M10 and M15.3 decisions; brokerd binds loopback only. */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SESS_MAX        64              /* hard ceiling on the table */
#define HEAD_MAX        8192            /* most of an HTTP request head */
#define RLIM_AS_BYTES   (1024UL << 20)  /* 1 GiB address space per child */
#define RLIM_CPU_SECS   3600            /* 1 hour of CPU per child */
#define RLIM_NOFILE     256

#define USER_MAX        64              /* authenticated user name, with NUL */

struct session {
    int    inuse;
    char   token[33];                   /* 32 hex digits + NUL */
    char   user[USER_MAX];              /* owning user when auth is on, else "" */
    pid_t  pid;
    int    port;
    time_t last_active;
};

static struct session sess[SESS_MAX];
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int sigchld_pipe[2];
static volatile sig_atomic_t quitting;
static volatile sig_atomic_t dump_req;  /* SIGUSR1: dump the session table */

/* Configuration, resolved once at startup from the environment. */
static const char *cfg_jotun;
static const char *cfg_kernel;
static const char *cfg_guest;
static const char *cfg_bundle;
static const char *cfg_ref;
static int cfg_listen_port;
static int cfg_port_base;
static int cfg_max;
static int cfg_idle;
static const char *cfg_auth_secret;     /* shared secret with authd; "" = off */
static int cfg_user_max;                /* default per-user session cap */
static int urandom_fd = -1;

/****************************************************************
 * Small helpers
 ****************************************************************/

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Connect to a loopback port; returns a fd or -1. */
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

/* Poll a child's draw port until it accepts, or time out. */
static int
wait_ready(int port, int timeout_ms)
{
    long deadline = now_ms() + timeout_ms;
    for (;;) {
        int fd = dial(port);
        if (fd >= 0) {
            close(fd);
            return 0;
        }
        if (now_ms() >= deadline)
            return -1;
        struct timespec ts = { 0, 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
}

static void
make_token(char out[33])
{
    unsigned char b[16];
    if (urandom_fd < 0 || read(urandom_fd, b, sizeof b) != (ssize_t)sizeof b) {
        /* Fallback: still unpredictable enough to not collide in practice. */
        long t = now_ms();
        for (size_t i = 0; i < sizeof b; i++)
            b[i] = (unsigned char)(t >> (i % 8 * 8)) ^ (unsigned char)rand();
    }
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 0xf];
    }
    out[32] = '\0';
}

static int
write_all(int fd, const char *buf, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, buf + off, n - off);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

/****************************************************************
 * Session table (all access under lock)
 ****************************************************************/

static int
port_in_use(int port)
{
    for (int i = 0; i < SESS_MAX; i++)
        if (sess[i].inuse && sess[i].port == port)
            return 1;
    return 0;
}

/* Find a session by token; returns index or -1. */
static int
find_token(const char *token)
{
    for (int i = 0; i < SESS_MAX; i++)
        if (sess[i].inuse && strcmp(sess[i].token, token) == 0)
            return i;
    return -1;
}

static int
session_count(void)
{
    int n = 0;
    for (int i = 0; i < SESS_MAX; i++)
        if (sess[i].inuse)
            n++;
    return n;
}

/* Count live sessions owned by one user (called under lock). */
static int
user_session_count(const char *user)
{
    int n = 0;
    for (int i = 0; i < SESS_MAX; i++)
        if (sess[i].inuse && strcmp(sess[i].user, user) == 0)
            n++;
    return n;
}

extern char **environ;

/* Build the child's environment array: the parent's, minus the keys we set per
 * session, plus the caller's overrides. Built in the parent before fork, so the
 * forked child runs only async-signal-safe steps (no setenv, no malloc) before
 * execve; setenv in a forked multithreaded child can deadlock on the allocator
 * lock. Returns a NULL-terminated array to free() in the parent, or NULL. The
 * override strings must outlive the exec (they live on the caller's stack,
 * which the child forks its own copy of). */
static char **
build_child_env(char *const overrides[], int noverride)
{
    static const char *keys[] = {
        "JOTUN_BUNDLE=", "JOTUN_REF=", "JOTUN_NAME=", "JOTUN_DRAW_PORT=",
    };
    int nkeys = (int)(sizeof keys / sizeof keys[0]);
    int nenv = 0;
    for (char **e = environ; e && *e; e++)
        nenv++;
    char **out = malloc((size_t)(nenv + noverride + 1) * sizeof *out);
    if (!out)
        return NULL;
    int n = 0;
    for (char **e = environ; e && *e; e++) {
        int skip = 0;
        for (int k = 0; k < nkeys; k++)
            if (strncmp(*e, keys[k], strlen(keys[k])) == 0) {
                skip = 1;
                break;
            }
        if (!skip)
            out[n++] = *e;
    }
    for (int i = 0; i < noverride; i++)
        out[n++] = overrides[i];
    out[n] = NULL;
    return out;
}

/* Fork a jotun child serving one session on a free port, and record it. user is
 * the owning identity ("" when auth is off) and cap its per-user session limit
 * (<= 0 to use the default). Returns the token in out (33 bytes), or -1 if the
 * global or the per-user cap is reached or the spawn failed. */
static int
spawn_session(const char *user, int cap, char out[33])
{
    pthread_mutex_lock(&lock);
    if (session_count() >= cfg_max) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    if (cfg_auth_secret[0]) {
        int limit = cap > 0 ? cap : cfg_user_max;
        if (user_session_count(user) >= limit) {
            pthread_mutex_unlock(&lock);
            return -1;
        }
    }
    int port = -1;
    for (int p = cfg_port_base; p < cfg_port_base + cfg_max; p++) {
        if (!port_in_use(p)) {
            port = p;
            break;
        }
    }
    int slot = -1;
    for (int i = 0; i < SESS_MAX; i++)
        if (!sess[i].inuse) {
            slot = i;
            break;
        }
    if (port < 0 || slot < 0) {
        pthread_mutex_unlock(&lock);
        return -1;
    }
    /* Reserve the slot and port before dropping the lock to fork. */
    sess[slot].inuse = 1;
    sess[slot].pid = 0;
    sess[slot].port = port;
    sess[slot].last_active = time(NULL);
    snprintf(sess[slot].user, sizeof sess[slot].user, "%s", user);
    make_token(sess[slot].token);
    memcpy(out, sess[slot].token, 33);
    pthread_mutex_unlock(&lock);

    /* Build everything the child needs before forking, so the child touches no
     * allocator between fork and execve (see build_child_env). The strings live
     * on this stack frame, which the child forks its own copy of. */
    char e_bundle[320], e_ref[160], e_name[64], e_port[32];
    snprintf(e_bundle, sizeof e_bundle, "JOTUN_BUNDLE=%s", cfg_bundle);
    snprintf(e_ref, sizeof e_ref, "JOTUN_REF=%s", cfg_ref);
    snprintf(e_name, sizeof e_name, "JOTUN_NAME=session");
    snprintf(e_port, sizeof e_port, "JOTUN_DRAW_PORT=%d", port);
    char *overrides[] = { e_bundle, e_ref, e_name, e_port };
    char **childenv = build_child_env(overrides, 4);
    if (!childenv) {
        pthread_mutex_lock(&lock);
        sess[slot].inuse = 0;
        pthread_mutex_unlock(&lock);
        return -1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        free(childenv);
        pthread_mutex_lock(&lock);
        sess[slot].inuse = 0;
        pthread_mutex_unlock(&lock);
        return -1;
    }
    if (pid == 0) {
        struct rlimit rl;
        rl.rlim_cur = rl.rlim_max = RLIM_AS_BYTES;
        setrlimit(RLIMIT_AS, &rl);
        rl.rlim_cur = rl.rlim_max = RLIM_CPU_SECS;
        setrlimit(RLIMIT_CPU, &rl);
        rl.rlim_cur = rl.rlim_max = RLIM_NOFILE;
        setrlimit(RLIMIT_NOFILE, &rl);
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, 1);
            dup2(devnull, 2);
        }
        char *args[] = { (char *)cfg_jotun, (char *)cfg_kernel,
                         (char *)cfg_guest, NULL };
        execve(cfg_jotun, args, childenv);
        _exit(127);
    }
    free(childenv);                     /* parent's copy; the child forked its */

    pthread_mutex_lock(&lock);
    sess[slot].pid = pid;
    pthread_mutex_unlock(&lock);

    if (wait_ready(port, 3000) != 0) {
        kill(pid, SIGKILL);             /* reaper frees the slot */
        return -1;
    }
    return 0;
}

static void
touch_session(const char *token)
{
    pthread_mutex_lock(&lock);
    int i = find_token(token);
    if (i >= 0)
        sess[i].last_active = time(NULL);
    pthread_mutex_unlock(&lock);
}

/* Reap exited children and free their slots. */
static void
reap_children(void)
{
    pid_t pid;
    while ((pid = waitpid(-1, NULL, WNOHANG)) > 0) {
        pthread_mutex_lock(&lock);
        for (int i = 0; i < SESS_MAX; i++)
            if (sess[i].inuse && sess[i].pid == pid)
                sess[i].inuse = 0;
        pthread_mutex_unlock(&lock);
    }
}

/* Kill sessions with no recent activity; an open connection keeps a session
 * fresh because its proxy thread touches last_active each second. */
static void
sweep_idle(void)
{
    time_t cutoff = time(NULL) - cfg_idle;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SESS_MAX; i++)
        if (sess[i].inuse && sess[i].pid > 0 && sess[i].last_active < cutoff)
            kill(sess[i].pid, SIGKILL);
    pthread_mutex_unlock(&lock);
}

/****************************************************************
 * Connection handling
 ****************************************************************/

/* A tiny "\r\n\r\n" finder (no GNU memmem dependency). */
static int
head_complete(const char *buf, int len)
{
    for (int i = 0; i + 4 <= len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    return 0;
}

/* Read an HTTP request head (through the blank line) into buf. Returns the
 * length, or -1 on error/timeout. */
static int
read_head(int fd, char *buf, int cap)
{
    int len = 0;
    long deadline = now_ms() + 5000;
    while (len < cap) {
        struct pollfd p = { .fd = fd, .events = POLLIN };
        int to = (int)(deadline - now_ms());
        if (to <= 0 || poll(&p, 1, to) <= 0)
            return -1;
        ssize_t n = read(fd, buf + len, cap - len);
        if (n <= 0)
            return -1;
        len += (int)n;
        if (len >= 4 && head_complete(buf, len))
            return len;
    }
    return -1;
}

/* Extract the jsess cookie value into out (<=32 chars) or return 0. */
static int
parse_cookie(const char *head, int len, char out[33])
{
    for (int i = 0; i + 7 <= len; i++) {
        if (memcmp(head + i, "jsess=", 6) == 0 &&
            (i == 0 || head[i - 1] == ' ' || head[i - 1] == ';' ||
             head[i - 1] == '\n')) {
            int j = i + 6, k = 0;
            while (j < len && k < 32 && head[j] != ';' && head[j] != '\r' &&
                   head[j] != '\n' && head[j] != ' ')
                out[k++] = head[j++];
            out[k] = '\0';
            return k > 0;
        }
    }
    return 0;
}

/* Read the trusted identity header authd set and Caddy copied onto the request:
 *     X-Jotun-Auth: <secret> <user> <cap>
 * The leading token is the shared secret only authd and brokerd know, so a
 * client cannot forge the identity: only a line whose first token equals our
 * secret is trusted. On a match, fills user and *cap (-1 if the field is "-")
 * and returns 1. Scans every such header, so a spurious client-sent one is
 * ignored. */
static int
parse_identity(const char *head, int len, char user_out[USER_MAX], int *cap_out)
{
    const char *p = head, *e = head + len;
    while (p < e) {
        if ((size_t)(e - p) >= 13 && strncasecmp(p, "X-Jotun-Auth:", 13) == 0) {
            const char *v = p + 13;
            const char *le = v;
            while (le < e && *le != '\r' && *le != '\n')
                le++;
            char line[256];
            int n = (int)(le - v);
            if (n > 0 && n < (int)sizeof line) {
                memcpy(line, v, (size_t)n);
                line[n] = '\0';
                char sec[160], usr[USER_MAX], capf[16];
                int got = sscanf(line, "%159s %63s %15s", sec, usr, capf);
                if (got >= 2 && strcmp(sec, cfg_auth_secret) == 0) {
                    snprintf(user_out, USER_MAX, "%s", usr);
                    *cap_out = (got >= 3 && capf[0] != '-') ? atoi(capf) : -1;
                    return 1;
                }
            }
        }
        while (p < e && *p != '\n')
            p++;
        if (p < e)
            p++;
    }
    return 0;
}

/* Is this request for the WebSocket endpoint? */
static int
is_ws_path(const char *head, int len)
{
    /* First line: METHOD SP path SP version. Find the path token. */
    int i = 0;
    while (i < len && head[i] != ' ')
        i++;
    i++;
    return i + 3 <= len && memcmp(head + i, "/ws", 3) == 0;
}

/* Copy bytes both ways between the browser and the child until either closes,
 * touching the session so it is not swept while connected. */
static void
splice_conn(int a, int b, const char *token)
{
    char buf[16384];
    long last_touch = 0;
    for (;;) {
        struct pollfd fds[2] = {
            { .fd = a, .events = POLLIN },
            { .fd = b, .events = POLLIN },
        };
        if (poll(fds, 2, 1000) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        long t = now_ms();
        if (t - last_touch > 900) {
            touch_session(token);
            last_touch = t;
        }
        if (fds[0].revents & POLLIN) {
            ssize_t n = read(a, buf, sizeof buf);
            if (n <= 0)
                break;
            if (write_all(b, buf, (size_t)n) != 0)
                break;
        }
        if (fds[1].revents & POLLIN) {
            ssize_t n = read(b, buf, sizeof buf);
            if (n <= 0)
                break;
            if (write_all(a, buf, (size_t)n) != 0)
                break;
        }
        if ((fds[0].revents | fds[1].revents) & (POLLERR | POLLHUP))
            break;
    }
}

static void *
handle_conn(void *arg)
{
    int cfd = (int)(long)arg;
    char head[HEAD_MAX];
    int hlen = read_head(cfd, head, sizeof head);
    if (hlen < 0) {
        close(cfd);
        return NULL;
    }

    /* Admission: when auth is on, every request must carry the identity authd
     * vouched for (Caddy copies it from forward_auth). No identity, no entry. */
    char auth_user[USER_MAX] = "";
    int auth_cap = -1;
    int auth_on = cfg_auth_secret[0] != '\0';
    if (auth_on && !parse_identity(head, hlen, auth_user, &auth_cap)) {
        static const char deny[] =
            "HTTP/1.1 403 Forbidden\r\n"
            "Connection: close\r\nContent-Length: 0\r\n\r\n";
        write_all(cfd, deny, sizeof deny - 1);
        close(cfd);
        return NULL;
    }

    char token[33];
    int have_cookie = parse_cookie(head, hlen, token);
    int port = -1;
    if (have_cookie) {
        pthread_mutex_lock(&lock);
        int i = find_token(token);
        /* A cookie is honoured only for its owner, so one user's token cannot
         * attach to another's session even if the cookie value leaks. */
        if (i >= 0 && sess[i].pid > 0 &&
            (!auth_on || strcmp(sess[i].user, auth_user) == 0))
            port = sess[i].port;
        pthread_mutex_unlock(&lock);
    }

    if (port > 0) {
        int bfd = dial(port);
        if (bfd < 0) {
            close(cfd);
            return NULL;
        }
        if (write_all(bfd, head, (size_t)hlen) == 0)
            splice_conn(cfd, bfd, token);
        close(bfd);
        close(cfd);
        return NULL;
    }

    /* No live session. A WebSocket request cannot be satisfied with a redirect,
     * so just close; the page will reconnect or be reloaded. */
    if (is_ws_path(head, hlen)) {
        close(cfd);
        return NULL;
    }

    char t[33];
    if (spawn_session(auth_user, auth_cap, t) != 0) {
        static const char busy[] =
            "HTTP/1.1 503 Service Unavailable\r\n"
            "Connection: close\r\nContent-Length: 21\r\n\r\n"
            "no sessions available";
        write_all(cfd, busy, sizeof busy - 1);
        close(cfd);
        return NULL;
    }
    char resp[256];
    int n = snprintf(resp, sizeof resp,
                     "HTTP/1.1 302 Found\r\n"
                     "Location: /\r\n"
                     "Set-Cookie: jsess=%s; Path=/; HttpOnly\r\n"
                     "Connection: close\r\nContent-Length: 0\r\n\r\n", t);
    write_all(cfd, resp, (size_t)n);
    close(cfd);
    return NULL;
}

/****************************************************************
 * Startup and the accept loop
 ****************************************************************/

static void
on_sigchld(int sig)
{
    (void)sig;
    char b = 1;
    ssize_t r = write(sigchld_pipe[1], &b, 1);
    (void)r;
}

static void
on_quit(int sig)
{
    (void)sig;
    quitting = 1;
}

static void
on_usr1(int sig)
{
    (void)sig;
    dump_req = 1;                       /* the loop dumps; the handler must not */
}

/* Print the live session table to stderr, for debugging admission and quotas
 * without a network surface. Triggered by SIGUSR1 (`kill -USR1 <brokerd pid>`),
 * but run from the main loop, not the handler, since it locks and uses stdio. */
static void
dump_sessions(void)
{
    time_t now = time(NULL);
    pthread_mutex_lock(&lock);
    int n = 0;
    fprintf(stderr, "[broker] session table:\n");
    for (int i = 0; i < SESS_MAX; i++) {
        if (!sess[i].inuse)
            continue;
        fprintf(stderr, "  user=%-16s pid=%-7d port=%-5d idle=%lds\n",
                sess[i].user[0] ? sess[i].user : "-", (int)sess[i].pid,
                sess[i].port, (long)(now - sess[i].last_active));
        n++;
    }
    fprintf(stderr, "[broker] %d session(s), max=%d, per-user default=%d\n",
            n, cfg_max, cfg_user_max);
    pthread_mutex_unlock(&lock);
}

/* Kill every live child so a shutdown orphans nothing. */
static void
kill_all_children(void)
{
    pthread_mutex_lock(&lock);
    for (int i = 0; i < SESS_MAX; i++)
        if (sess[i].inuse && sess[i].pid > 0)
            kill(sess[i].pid, SIGKILL);
    pthread_mutex_unlock(&lock);
}

static int
env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v ? (int)strtol(v, NULL, 0) : def;
}

static const char *
env_str(const char *name, const char *def)
{
    const char *v = getenv(name);
    return v && *v ? v : def;
}

int
main(void)
{
    cfg_jotun = env_str("JOTUN", "./jotun");
    cfg_kernel = env_str("JOTUN_KERNEL", "libos.elf");
    cfg_guest = env_str("JOTUN_BROKER_GUEST", "webterm");
    cfg_bundle = env_str("JOTUN_BUNDLE", "bundle-depot");
    cfg_ref = env_str("JOTUN_REF", "root");
    cfg_listen_port = env_int("JOTUN_BROKER_PORT", 9230);
    cfg_port_base = env_int("JOTUN_BROKER_PORTBASE", 9300);
    cfg_max = env_int("JOTUN_BROKER_MAX", 16);
    cfg_idle = env_int("JOTUN_BROKER_IDLE", 300);
    cfg_auth_secret = env_str("JOTUN_BROKER_AUTH_SECRET", "");
    cfg_user_max = env_int("JOTUN_BROKER_USER_MAX", 3);
    if (cfg_max < 1)
        cfg_max = 1;
    if (cfg_max > SESS_MAX)
        cfg_max = SESS_MAX;
    if (cfg_user_max < 1)
        cfg_user_max = 1;

    urandom_fd = open("/dev/urandom", O_RDONLY);
    signal(SIGPIPE, SIG_IGN);            /* writes to closed peers return EPIPE */
    if (pipe(sigchld_pipe) < 0) {
        perror("pipe");
        return 1;
    }
    struct sigaction sa = { 0 };
    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    struct sigaction sq = { 0 };
    sq.sa_handler = on_quit;              /* no SA_RESTART: break the poll */
    sigaction(SIGINT, &sq, NULL);
    sigaction(SIGTERM, &sq, NULL);
    struct sigaction su = { 0 };
    su.sa_handler = on_usr1;              /* no SA_RESTART: break the poll */
    sigaction(SIGUSR1, &su, NULL);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)cfg_listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 16) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "[broker] listening on http://127.0.0.1:%d/ "
            "(front with Caddy for TLS); guest=%s bundle=%s max=%d auth=%s\n",
            cfg_listen_port, cfg_guest, cfg_bundle, cfg_max,
            cfg_auth_secret[0] ? "on" : "off");

    while (!quitting) {
        struct pollfd fds[2] = {
            { .fd = lfd, .events = POLLIN },
            { .fd = sigchld_pipe[0], .events = POLLIN },
        };
        int pr = poll(fds, 2, 1000);
        if (pr < 0 && errno != EINTR) {
            perror("poll");
            break;
        }
        if (quitting)
            break;
        if (dump_req) {
            dump_req = 0;
            dump_sessions();
        }
        if (fds[1].revents & POLLIN) {
            char drain[64];
            while (read(sigchld_pipe[0], drain, sizeof drain) > 0)
                ;
            reap_children();
        }
        if (fds[0].revents & POLLIN) {
            int cfd = accept(lfd, NULL, NULL);
            if (cfd >= 0) {
                pthread_t th;
                if (pthread_create(&th, NULL, handle_conn,
                                   (void *)(long)cfd) == 0)
                    pthread_detach(th);
                else
                    close(cfd);
            }
        }
        sweep_idle();
    }
    fprintf(stderr, "[broker] shutting down; killing %d session(s)\n",
            session_count());
    kill_all_children();
    return 0;
}
