/* http.c : a minimal HTTP/1.1 GET client for the bundle pull path. */
#define _GNU_SOURCE     /* getaddrinfo, memmem */
#include "http.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define HTTP_TIMEOUT    15      /* seconds for connect, send, and each recv */
#define HTTP_MAXBODY    (64u * 1024 * 1024)

/****************************************************************
 * Response buffer
 ****************************************************************/

void
http_buf_free(struct http_buf *b)
{
    free(b->data);
    b->data = NULL;
    b->len = b->cap = 0;
}

static int
buf_append(struct http_buf *b, const char *data, size_t n)
{
    if (b->len + n > HTTP_MAXBODY)
        return -1;
    if (b->len + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap * 2 : 8192;
        while (cap < b->len + n + 1)
            cap *= 2;
        char *p = realloc(b->data, cap);
        if (!p)
            return -1;
        b->data = p;
        b->cap = cap;
    }
    memcpy(b->data + b->len, data, n);
    b->len += n;
    b->data[b->len] = '\0';
    return 0;
}

/****************************************************************
 * URL parsing
 ****************************************************************/

int
http_url_parse(const char *url, struct http_url *out)
{
    memset(out, 0, sizeof *out);
    strcpy(out->port, "80");

    if (strncmp(url, "http://", 7) != 0)
        return -1;
    const char *p = url + 7;

    /* host[:port] runs up to the first '/' or end. */
    const char *slash = strchr(p, '/');
    const char *hostend = slash ? slash : p + strlen(p);
    const char *colon = memchr(p, ':', (size_t)(hostend - p));

    size_t hostlen = (size_t)((colon ? colon : hostend) - p);
    if (hostlen == 0 || hostlen >= sizeof out->host)
        return -1;
    memcpy(out->host, p, hostlen);
    out->host[hostlen] = '\0';

    if (colon) {
        size_t portlen = (size_t)(hostend - (colon + 1));
        if (portlen == 0 || portlen >= sizeof out->port)
            return -1;
        memcpy(out->port, colon + 1, portlen);
        out->port[portlen] = '\0';
    }

    /* The base path, with any trailing slash trimmed. */
    if (slash) {
        size_t plen = strlen(slash);
        while (plen > 0 && slash[plen - 1] == '/')
            plen--;
        if (plen >= sizeof out->path)
            return -1;
        memcpy(out->path, slash, plen);
        out->path[plen] = '\0';
    }
    return 0;
}

/****************************************************************
 * Transport
 ****************************************************************/

/* Connect to host:port with a bounded connect time, then switch the socket to
 * blocking with send and recv timeouts. Returns the fd, or -1. */
static int
dial(const char *host, const char *port)
{
    struct addrinfo hints, *res, *ai;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, port, &hints, &res) != 0)
        return -1;

    int fd = -1;
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0)
            continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            struct timeval tv = { HTTP_TIMEOUT, 0 };
            rc = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (rc > 0) {
                int err = 0;
                socklen_t el = sizeof err;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el);
                rc = err ? -1 : 0;
            } else {
                rc = -1;
            }
        }
        if (rc != 0) {
            close(fd);
            fd = -1;
            continue;
        }
        fcntl(fd, F_SETFL, fl);         /* back to blocking */
        struct timeval tv = { HTTP_TIMEOUT, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
        break;
    }
    freeaddrinfo(res);
    return fd;
}

static int
send_all(int fd, const char *data, size_t n)
{
    size_t off = 0;
    while (off < n) {
        ssize_t w = send(fd, data + off, n - off, 0);
        if (w <= 0)
            return -1;
        off += (size_t)w;
    }
    return 0;
}

/****************************************************************
 * Response parsing
 ****************************************************************/

/* Decode a Transfer-Encoding: chunked body in [p, end) into buf. */
static int
dechunk(const char *p, const char *end, struct http_buf *buf)
{
    while (p < end) {
        char *stop;
        long n = strtol(p, &stop, 16);
        if (stop == p || n < 0)
            return -1;
        const char *eol = memchr(stop, '\n', (size_t)(end - stop));
        if (!eol)
            return -1;
        p = eol + 1;                    /* start of chunk data */
        if (n == 0)
            break;                      /* last chunk */
        if (p + n > end)
            return -1;
        if (buf_append(buf, p, (size_t)n) < 0)
            return -1;
        p += n;
        /* skip the CRLF after the chunk data */
        if (p + 2 <= end && p[0] == '\r' && p[1] == '\n')
            p += 2;
    }
    return 0;
}

/* Issue one GET, optionally with a Range header (range may be "" for none), and
 * return the status code with the body in buf. When total_out is non-NULL and
 * the response carries a Content-Range, *total_out receives the resource's full
 * size. */
static int
do_get(const struct http_url *origin, const char *rel, const char *range,
       struct http_buf *buf, long *total_out)
{
    buf->len = 0;
    if (buf->data)
        buf->data[0] = '\0';
    if (total_out)
        *total_out = -1;

    int fd = dial(origin->host, origin->port);
    if (fd < 0)
        return -1;

    char req[2048];
    int n = snprintf(req, sizeof req,
                     "GET %s/%s HTTP/1.1\r\nHost: %s\r\n"
                     "User-Agent: jotun/0\r\n%sConnection: close\r\n\r\n",
                     origin->path, rel, origin->host, range);
    if (n <= 0 || n >= (int)sizeof req || send_all(fd, req, (size_t)n) < 0) {
        close(fd);
        return -1;
    }

    /* Read the whole response to EOF; Connection: close ends it cleanly. */
    struct http_buf raw = { 0 };
    char tmp[16384];
    ssize_t r;
    while ((r = recv(fd, tmp, sizeof tmp, 0)) > 0) {
        if (buf_append(&raw, tmp, (size_t)r) < 0) {
            close(fd);
            http_buf_free(&raw);
            return -1;
        }
    }
    close(fd);
    if (!raw.data) {                    /* nothing read: timeout or reset */
        http_buf_free(&raw);
        return -1;
    }

    /* Status line: "HTTP/1.x <code> ...". */
    int code = -1;
    if (sscanf(raw.data, "HTTP/%*d.%*d %d", &code) != 1 || code < 100) {
        http_buf_free(&raw);
        return -1;
    }

    /* Split headers from body at the blank line. */
    const char *split = strstr(raw.data, "\r\n\r\n");
    if (!split) {
        http_buf_free(&raw);
        return -1;
    }
    const char *body = split + 4;
    const char *end = raw.data + raw.len;

    /* Detect chunked transfer, and read the full resource size off a
     * Content-Range ("bytes START-END/TOTAL") when the caller wants it. */
    int chunked = 0;
    for (const char *h = raw.data; h < split; ) {
        const char *nl = memchr(h, '\n', (size_t)(split - h));
        size_t linelen = nl ? (size_t)(nl - h) : (size_t)(split - h);
        if (linelen >= 17 && strncasecmp(h, "transfer-encoding", 17) == 0
            && memmem(h, linelen, "chunked", 7))
            chunked = 1;
        if (total_out && linelen >= 13
            && strncasecmp(h, "content-range", 13) == 0) {
            const char *slash = memchr(h, '/', linelen);
            if (slash)
                *total_out = strtol(slash + 1, NULL, 10);
        }
        if (!nl)
            break;
        h = nl + 1;
    }

    int rc;
    if (chunked)
        rc = dechunk(body, end, buf);
    else
        rc = buf_append(buf, body, (size_t)(end - body));

    http_buf_free(&raw);
    return rc == 0 ? code : -1;
}

int
http_get(const struct http_url *origin, const char *rel, struct http_buf *buf)
{
    return do_get(origin, rel, "", buf, NULL);
}

int
http_get_range(const struct http_url *origin, const char *rel, long off,
               long len, struct http_buf *buf, long *total_out)
{
    char range[64];
    if (off < 0)                        /* suffix: the last `len` bytes */
        snprintf(range, sizeof range, "Range: bytes=-%ld\r\n", len);
    else
        snprintf(range, sizeof range, "Range: bytes=%ld-%ld\r\n",
                 off, off + len - 1);
    return do_get(origin, rel, range, buf, total_out);
}
