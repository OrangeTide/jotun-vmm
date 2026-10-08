/* authd.c : a loopback bearer-token validator for the hosting front end.
 *
 * The admission half of M15.3. Authentication stays off the box in the reverse
 * proxy, as M10 decided; this is the small validator Caddy delegates to with
 * forward_auth. It maps a bearer token to a user and a per-user session cap, and
 * hands the identity to brokerd through a single trusted header so brokerd never
 * sees a token. Two endpoints, dispatched by path:
 *
 *   /auth   The forward_auth check Caddy runs on every request. Reads the token
 *           from an `Authorization: Bearer <t>` header (programmatic clients) or
 *           a `jauth` cookie (browsers, which cannot set headers on a WebSocket).
 *           A valid token returns 200 with
 *               X-Jotun-Auth: <secret> <user> <cap>
 *           which Caddy copies onto the request to brokerd. An invalid or absent
 *           token returns 401.
 *   /login  A one-time exchange so a browser can obtain the cookie: GET
 *           /login?token=<t> (or an Authorization header) sets
 *               Set-Cookie: jauth=<t>; HttpOnly; Secure; SameSite=Lax
 *           and 302s to /. Thereafter the cookie rides every request, /ws too.
 *
 * The leading token of X-Jotun-Auth is a shared secret only authd and brokerd
 * know, so a client cannot forge the identity even by sending its own
 * X-Jotun-Auth: brokerd trusts only the line whose first token is the secret.
 * authd binds loopback only; TLS and the public edge are Caddy's.
 *
 * Config (environment): JOTUN_AUTH_TOKENS (store path, default "tokens"),
 * JOTUN_AUTH_PORT (default 9231), JOTUN_BROKER_AUTH_SECRET (the shared secret). */
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define TOK_MAX     4096            /* most token lines the store may hold */
#define USER_MAX    64
#define HEAD_MAX    8192

struct tok {
    char token[33];
    char user[USER_MAX];
    int  cap;                       /* per-user session cap, or -1 for default */
};

static struct tok toks[TOK_MAX];
static int ntok;
static time_t store_mtime;
static pthread_mutex_t store_lock = PTHREAD_MUTEX_INITIALIZER;

static const char *store_path;
static const char *secret;
static int listen_port;

static long
now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
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
 * Token store
 ****************************************************************/

/* Reload the store if its mtime changed. Call with store_lock held. */
static void
reload_locked(void)
{
    struct stat st;
    if (stat(store_path, &st) != 0)
        return;
    if (st.st_mtime == store_mtime && ntok > 0)
        return;

    FILE *f = fopen(store_path, "r");
    if (!f)
        return;
    int n = 0;
    char line[256];
    while (n < TOK_MAX && fgets(line, sizeof line, f)) {
        char tok[64], user[USER_MAX];
        int cap = -1;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        int got = sscanf(line, "%63s %63s %d", tok, user, &cap);
        if (got < 2 || strlen(tok) != 32)
            continue;
        memcpy(toks[n].token, tok, 33);
        snprintf(toks[n].user, sizeof toks[n].user, "%s", user);
        toks[n].cap = (got >= 3 && cap > 0) ? cap : -1;
        n++;
    }
    fclose(f);
    ntok = n;
    store_mtime = st.st_mtime;
}

/* Look a token up. On a hit, fills user (>= USER_MAX bytes) and *cap, returns 1. */
static int
lookup(const char *token, char *user, int *cap)
{
    int found = 0;
    pthread_mutex_lock(&store_lock);
    reload_locked();
    for (int i = 0; i < ntok; i++)
        if (strcmp(toks[i].token, token) == 0) {
            snprintf(user, USER_MAX, "%s", toks[i].user);
            *cap = toks[i].cap;
            found = 1;
            break;
        }
    pthread_mutex_unlock(&store_lock);
    return found;
}

/****************************************************************
 * Request parsing
 ****************************************************************/

static int
head_complete(const char *buf, int len)
{
    for (int i = 0; i + 4 <= len; i++)
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return 1;
    return 0;
}

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

/* Copy the hex token at src (up to 32 lowercase hex) into out, stopping at any
 * non-hex byte. Returns 1 if a full 32-char token was read. */
static int
take_token(const char *src, const char *end, char out[33])
{
    int k = 0;
    while (src < end && k < 32 &&
           ((*src >= '0' && *src <= '9') || (*src >= 'a' && *src <= 'f') ||
            (*src >= 'A' && *src <= 'F')))
        out[k++] = *src++;
    out[k] = '\0';
    return k == 32;
}

/* Find "Bearer <token>" on an Authorization header. Case-insensitive on the
 * scheme. Returns 1 and fills out, or 0. */
static int
bearer_token(const char *head, int len, char out[33])
{
    const char *p = head, *e = head + len;
    while (p < e) {
        if ((size_t)(e - p) >= 7 && strncasecmp(p, "Bearer ", 7) == 0) {
            const char *t = p + 7;
            while (t < e && *t == ' ')
                t++;
            return take_token(t, e, out);
        }
        p++;
    }
    return 0;
}

/* Find the jauth cookie value. */
static int
cookie_token(const char *head, int len, char out[33])
{
    for (int i = 0; i + 7 <= len; i++)
        if (memcmp(head + i, "jauth=", 6) == 0 &&
            (i == 0 || head[i - 1] == ' ' || head[i - 1] == ';' ||
             head[i - 1] == '\n'))
            return take_token(head + i + 6, head + len, out);
    return 0;
}

/* Find token=<value> in the request-line query (first line only). */
static int
query_token(const char *head, int len, char out[33])
{
    int eol = 0;
    while (eol < len && head[eol] != '\r' && head[eol] != '\n')
        eol++;
    for (int i = 1; i + 6 <= eol; i++)
        if (memcmp(head + i, "token=", 6) == 0 &&
            (head[i - 1] == '?' || head[i - 1] == '&'))
            return take_token(head + i + 6, head + eol, out);
    return 0;
}

static int
path_is(const char *head, int len, const char *path)
{
    int i = 0;
    while (i < len && head[i] != ' ')      /* skip method */
        i++;
    i++;
    size_t pl = strlen(path);
    return i + (int)pl <= len && memcmp(head + i, path, pl) == 0;
}

/****************************************************************
 * Responses
 ****************************************************************/

static void
reply(int fd, const char *status, const char *extra)
{
    char buf[512];
    int n = snprintf(buf, sizeof buf,
                     "HTTP/1.1 %s\r\n%sContent-Length: 0\r\n"
                     "Connection: close\r\n\r\n",
                     status, extra ? extra : "");
    write_all(fd, buf, (size_t)n);
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

    int login = path_is(head, hlen, "/login");
    char token[33];
    int have = bearer_token(head, hlen, token) ||
               cookie_token(head, hlen, token) ||
               (login && query_token(head, hlen, token));

    char user[USER_MAX];
    int cap = -1;
    if (have && lookup(token, user, &cap)) {
        if (login) {
            /* Hand the browser its session cookie, then send it to the app. */
            char extra[256];
            snprintf(extra, sizeof extra,
                     "Location: /\r\n"
                     "Set-Cookie: jauth=%s; Path=/; HttpOnly; Secure; "
                     "SameSite=Lax\r\n", token);
            reply(cfd, "302 Found", extra);
        } else {
            char capstr[16];
            if (cap > 0)
                snprintf(capstr, sizeof capstr, "%d", cap);
            else
                snprintf(capstr, sizeof capstr, "-");
            char extra[256];
            snprintf(extra, sizeof extra, "X-Jotun-Auth: %s %s %s\r\n",
                     secret, user, capstr);
            reply(cfd, "200 OK", extra);
        }
    } else {
        reply(cfd, "401 Unauthorized",
              "WWW-Authenticate: Bearer\r\n");
    }
    close(cfd);
    return NULL;
}

static int
env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? (int)strtol(v, NULL, 0) : def;
}

int
main(void)
{
    store_path = getenv("JOTUN_AUTH_TOKENS");
    if (!store_path || !*store_path)
        store_path = "tokens";
    secret = getenv("JOTUN_BROKER_AUTH_SECRET");
    if (!secret)
        secret = "";
    listen_port = env_int("JOTUN_AUTH_PORT", 9231);

    signal(SIGPIPE, SIG_IGN);

    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    if (lfd < 0) {
        perror("socket");
        return 1;
    }
    int one = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons((unsigned short)listen_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(lfd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(lfd, 16) < 0) {
        perror("listen");
        return 1;
    }
    fprintf(stderr, "[authd] listening on http://127.0.0.1:%d/ "
            "(Caddy forward_auth target); tokens=%s\n",
            listen_port, store_path);

    for (;;) {
        int cfd = accept(lfd, NULL, NULL);
        if (cfd < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        pthread_t th;
        if (pthread_create(&th, NULL, handle_conn, (void *)(long)cfd) == 0)
            pthread_detach(th);
        else
            close(cfd);
    }
    return 0;
}
