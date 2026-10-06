/* drawws.c : the host WebSocket front end (Linux).
 *
 * The SHA-1 and WebSocket framing are lifted from quaoar-server.c; the SHA-1 is
 * the single-shot form based on Jon Mayo's public-domain sha1tiny. The server
 * thread polls a listener, the browser connection, and a self-pipe nudge. A GET
 * without an Upgrade header is answered with the embedded client page; an
 * upgrade completes the handshake and the browser becomes the one display. A
 * push from the draw subsystem stores the latest scene and writes the pipe, so
 * the connection fd stays on this thread alone. */

#define _GNU_SOURCE

#include "drawws.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define WS_GUID    "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
#define WS_INMAX   8192        /* largest inbound (event) frame we accept */

struct drawws {
    int listen_fd;
    int ws_fd;                 /* connected browser, or -1 */
    int nudge[2];              /* self-pipe: [0] read in poll, [1] written to */
    pthread_t thread;
    pthread_mutex_t lock;      /* guards latest and dirty */
    char *latest;              /* most recent scene SVG, malloc'd */
    int dirty;                 /* latest not yet sent to ws_fd */
    volatile int stop;
    drawws_event_fn on_event;
    void *ctx;
};

/****************************************************************
 * SHA-1 (single-shot), based on sha1tiny by Jon Mayo (public domain)
 ****************************************************************/

#define ROL32(v, b) (((v) << (b)) | ((v) >> (32 - (b))))

static void
sha1(const void *data, size_t len, uint8_t md[20])
{
    uint32_t h[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
    uint32_t v[5], w[80];
    uint64_t cnt = 0;
    unsigned i, tail = 0, length = 0, done = 0;

    while (!done) {
        for (i = 0; i < 64; i++) {
            uint32_t n = w[i / 4], d = 0;
            if (len > i) {
                d = *(const uint8_t *)data;
                data = (const uint8_t *)data + 1;
                cnt += 8;
            } else if (!tail && i == len) {
                d = 0x80;
                tail = 1;
            } else if (tail && i >= 56) {
                if (i == 56) length = 1;
                if (length) {
                    d = (cnt >> (64 - (i - 56 + 1) * 8)) & 255;
                    if (i == 63) done = 1;
                }
            }
            switch (i % 4) {
            case 0: n = (n & 0x00ffffff) | (d << 24); break;
            case 1: n = (n & 0xff00ffff) | (d << 16); break;
            case 2: n = (n & 0xffff00ff) | (d << 8); break;
            case 3: n = (n & 0xffffff00) | d; break;
            }
            w[i / 4] = n;
        }
        len = len > 64 ? len - 64 : 0;
        for (i = 16; i < 80; i++)
            w[i] = ROL32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        for (i = 0; i < 5; i++) v[i] = h[i];
        for (i = 0; i < 80; i++) {
            uint32_t f, k, t;
            if      (i < 20) { f = (v[1] & v[2]) | (~v[1] & v[3]); k = 0x5a827999; }
            else if (i < 40) { f = v[1] ^ v[2] ^ v[3];             k = 0x6ed9eba1; }
            else if (i < 60) { f = (v[1] & v[2]) | (v[1] & v[3]) | (v[2] & v[3]); k = 0x8f1bbcdc; }
            else             { f = v[1] ^ v[2] ^ v[3];             k = 0xca62c1d6; }
            t = ROL32(v[0], 5) + f + v[4] + k + w[i];
            v[4] = v[3]; v[3] = v[2]; v[2] = ROL32(v[1], 30); v[1] = v[0]; v[0] = t;
        }
        for (i = 0; i < 5; i++) h[i] += v[i];
    }
    for (i = 0; i < 5; i++) {
        *md++ = h[i] >> 24; *md++ = h[i] >> 16;
        *md++ = h[i] >> 8;  *md++ = h[i];
    }
}

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void
base64_encode(const uint8_t *in, int len, char *out)
{
    int i, j = 0;

    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = b64[in[i] >> 2];
        out[j++] = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[j++] = b64[((in[i + 1] & 0xf) << 2) | (in[i + 2] >> 6)];
        out[j++] = b64[in[i + 2] & 0x3f];
    }
    if (i < len) {
        out[j++] = b64[in[i] >> 2];
        if (i + 1 < len) {
            out[j++] = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            out[j++] = b64[(in[i + 1] & 0xf) << 2];
        } else {
            out[j++] = b64[(in[i] & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = 0;
}

/****************************************************************
 * Byte I/O and WebSocket framing
 ****************************************************************/

static int
read_all(int fd, void *buf, int n)
{
    int got = 0;
    while (got < n) {
        int r = (int)read(fd, (uint8_t *)buf + got, (size_t)(n - got));
        if (r <= 0)
            return -1;
        got += r;
    }
    return got;
}

static int
write_all(int fd, const void *buf, size_t n)
{
    size_t sent = 0;
    while (sent < n) {
        ssize_t w = write(fd, (const uint8_t *)buf + sent, n - sent);
        if (w <= 0)
            return -1;
        sent += (size_t)w;
    }
    return 0;
}

/* Read one WebSocket frame into payload. Returns payload length or -1. */
static int
ws_read(int fd, uint8_t *payload, int max, int *opcode)
{
    uint8_t hdr[2];
    if (read_all(fd, hdr, 2) < 0)
        return -1;
    *opcode = hdr[0] & 0x0f;
    int masked = (hdr[1] & 0x80) != 0;
    uint64_t len = hdr[1] & 0x7f;
    if (len == 126) {
        uint8_t ext[2];
        if (read_all(fd, ext, 2) < 0)
            return -1;
        len = (uint16_t)ext[0] << 8 | ext[1];
    } else if (len == 127) {
        uint8_t ext[8];
        if (read_all(fd, ext, 8) < 0)
            return -1;
        len = 0;
        for (int i = 0; i < 8; i++)
            len = (len << 8) | ext[i];
    }
    if (len > (uint64_t)max)
        return -1;
    uint8_t mask[4] = { 0 };
    if (masked && read_all(fd, mask, 4) < 0)
        return -1;
    if (len && read_all(fd, payload, (int)len) < 0)
        return -1;
    if (masked)
        for (uint64_t i = 0; i < len; i++)
            payload[i] ^= mask[i % 4];
    return (int)len;
}

/* Write one unmasked text frame (server to client). */
static int
ws_write_text(int fd, const char *payload, size_t len)
{
    uint8_t hdr[10];
    int hlen = 0;
    hdr[hlen++] = 0x81;         /* FIN + text */
    if (len < 126) {
        hdr[hlen++] = (uint8_t)len;
    } else if (len < 65536) {
        hdr[hlen++] = 126;
        hdr[hlen++] = (len >> 8) & 0xff;
        hdr[hlen++] = len & 0xff;
    } else {
        hdr[hlen++] = 127;
        for (int i = 7; i >= 0; i--)
            hdr[hlen++] = (uint8_t)((uint64_t)len >> (i * 8));
    }
    if (write_all(fd, hdr, (size_t)hlen) < 0)
        return -1;
    return write_all(fd, payload, len);
}

static void
ws_pong(int fd, const uint8_t *data, int len)
{
    uint8_t hdr[2] = { 0x8a, (uint8_t)len };
    if (write_all(fd, hdr, 2) == 0 && len > 0)
        write_all(fd, data, (size_t)len);
}

/****************************************************************
 * The embedded browser client
 ****************************************************************/

static const char CLIENT_HTML[] =
"<!DOCTYPE html>\n"
"<html lang=\"en\"><head><meta charset=\"utf-8\">\n"
"<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
"<title>Jotun Display</title>\n"
"<style>\n"
"  html,body{margin:0;height:100%;background:#1e1e22;"
"font-family:system-ui,sans-serif}\n"
"  #display{width:100%;height:100%}\n"
"  #status{position:fixed;top:8px;right:10px;color:#ccc;font-size:13px;\n"
"    background:rgba(0,0,0,.5);border-radius:6px;padding:3px 10px}\n"
"  #dot{display:inline-block;width:9px;height:9px;border-radius:50%;\n"
"    background:#c44;margin-left:6px;vertical-align:middle}\n"
"  #dot.on{background:#4c4}\n"
"</style></head><body>\n"
"<svg id=\"display\" xmlns=\"http://www.w3.org/2000/svg\"></svg>\n"
"<div id=\"status\"><span id=\"txt\">connecting</span><span id=\"dot\"></span></div>\n"
"<script>\n"
"'use strict';\n"
"var display=document.getElementById('display');\n"
"var dot=document.getElementById('dot');\n"
"var txt=document.getElementById('txt');\n"
"var ws;\n"
"function stat(on){dot.classList.toggle('on',on);txt.textContent=on?'connected':'disconnected';}\n"
"function nodeId(el){var m=/^qu-\\d+-(\\d+)$/.exec(el.id||'');return m?+m[1]:0;}\n"
"function send(node,type){if(ws&&ws.readyState===1)ws.send(node+' '+type);}\n"
"function wire(root){\n"
"  root.querySelectorAll('[data-qu-listen=\\'click\\']').forEach(function(el){\n"
"    el.style.cursor='pointer';\n"
"    el.addEventListener('click',function(e){e.stopPropagation();send(nodeId(el),1);});\n"
"  });\n"
"  root.querySelectorAll('[data-qu-listen=\\'close\\']').forEach(function(el){\n"
"    el.style.cursor='pointer';\n"
"    el.addEventListener('click',function(e){e.stopPropagation();\n"
"      var s=el.closest('.qu-surface');send(s?nodeId(s):0,2);});\n"
"  });\n"
"}\n"
"function render(text){\n"
"  var doc=new DOMParser().parseFromString(text,'image/svg+xml');\n"
"  var root=doc.documentElement;\n"
"  if(!root||root.getElementsByTagName('parsererror').length||root.nodeName==='parsererror')return;\n"
"  var el=document.importNode(root,true);\n"
"  display.replaceChildren.apply(display,[].slice.call(el.childNodes));\n"
"  if(el.getAttribute('viewBox'))display.setAttribute('viewBox',el.getAttribute('viewBox'));\n"
"  wire(display);\n"
"}\n"
"function connect(){\n"
"  ws=new WebSocket((location.protocol==='https:'?'wss:':'ws:')+'//'+location.host+'/ws');\n"
"  ws.onopen=function(){stat(true);};\n"
"  ws.onmessage=function(e){render(e.data);};\n"
"  ws.onclose=function(){stat(false);setTimeout(connect,1000);};\n"
"  ws.onerror=function(){ws.close();};\n"
"}\n"
"connect();\n"
"</script></body></html>\n";

static void
serve_html(int fd)
{
    char hdr[256];
    int n = snprintf(hdr, sizeof hdr,
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/html; charset=utf-8\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     sizeof CLIENT_HTML - 1);
    if (write_all(fd, hdr, (size_t)n) == 0)
        write_all(fd, CLIENT_HTML, sizeof CLIENT_HTML - 1);
}

/* Read an HTTP request header block (up to the blank line) into buf. Returns
 * the length, or -1. */
static int
read_request(int fd, char *buf, int cap)
{
    int pos = 0;
    while (pos < cap - 1) {
        int r = (int)read(fd, buf + pos, 1);
        if (r <= 0)
            return -1;
        pos++;
        if (pos >= 4 && memcmp(buf + pos - 4, "\r\n\r\n", 4) == 0)
            break;
    }
    buf[pos] = '\0';
    return pos;
}

/* Complete the WebSocket upgrade using the already-read request. Returns 0 on
 * success. */
static int
ws_accept(int fd, const char *req)
{
    const char *kp = strcasestr(req, "Sec-WebSocket-Key:");
    if (!kp)
        return -1;
    kp += 18;
    while (*kp == ' ')
        kp++;
    char key[128];
    int kl = 0;
    while (kp[kl] && kp[kl] != '\r' && kp[kl] != '\n' && kl < (int)sizeof key - 1) {
        key[kl] = kp[kl];
        kl++;
    }
    key[kl] = '\0';

    char combined[192];
    int clen = snprintf(combined, sizeof combined, "%s%s", key, WS_GUID);
    uint8_t digest[20];
    sha1(combined, (size_t)clen, digest);
    char accept[32];
    base64_encode(digest, 20, accept);

    char resp[256];
    int rlen = snprintf(resp, sizeof resp,
                        "HTTP/1.1 101 Switching Protocols\r\n"
                        "Upgrade: websocket\r\n"
                        "Connection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: %s\r\n\r\n", accept);
    return write_all(fd, resp, (size_t)rlen);
}

/****************************************************************
 * Connection lifecycle and the server thread
 ****************************************************************/

static void
ws_close(struct drawws *w)
{
    if (w->ws_fd >= 0) {
        close(w->ws_fd);
        w->ws_fd = -1;
    }
}

/* Send the stored latest scene to the browser, clearing the dirty flag. */
static void
send_latest(struct drawws *w)
{
    pthread_mutex_lock(&w->lock);
    char *svg = w->latest ? strdup(w->latest) : NULL;
    w->dirty = 0;
    pthread_mutex_unlock(&w->lock);

    if (svg) {
        if (w->ws_fd >= 0 && ws_write_text(w->ws_fd, svg, strlen(svg)) < 0)
            ws_close(w);
        free(svg);
    }
}

static void
set_rcvtimeo(int fd, int secs)
{
    struct timeval tv = { .tv_sec = secs, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
}

/* Handle an accepted TCP connection: serve the page or complete the upgrade.
 * The header read runs under a receive timeout so a connection that never
 * sends a complete request cannot wedge this single server thread. */
static void
accept_conn(struct drawws *w)
{
    int cfd = accept(w->listen_fd, NULL, NULL);
    if (cfd < 0)
        return;
    set_rcvtimeo(cfd, 3);

    char req[4096];
    if (read_request(cfd, req, sizeof req) < 0) {
        close(cfd);
        return;
    }

    if (strcasestr(req, "upgrade: websocket") || strcasestr(req, "sec-websocket-key:")) {
        if (ws_accept(cfd, req) < 0) {
            close(cfd);
            return;
        }
        set_rcvtimeo(cfd, 0);   /* an upgraded browser may idle indefinitely */
        ws_close(w);            /* one browser at a time: replace the old */
        w->ws_fd = cfd;
        send_latest(w);         /* first paint */
    } else {
        serve_html(cfd);
        close(cfd);
    }
}

/* Parse an inbound event frame "node type" with an optional "\n<value>" tail
 * and dispatch it. */
static void
dispatch_event(struct drawws *w, uint8_t *payload, int plen)
{
    char *nl = memchr(payload, '\n', (size_t)plen);
    int hlen = nl ? (int)(nl - (char *)payload) : plen;
    char hdr[64];
    if (hlen >= (int)sizeof hdr)
        hlen = (int)sizeof hdr - 1;
    memcpy(hdr, payload, (size_t)hlen);
    hdr[hlen] = '\0';

    unsigned node = 0, type = 0;
    if (sscanf(hdr, "%u %u", &node, &type) != 2)
        return;

    const char *value = nl ? nl + 1 : NULL;
    int vlen = nl ? plen - (int)(nl + 1 - (char *)payload) : 0;
    if (vlen < 0)
        vlen = 0;
    if (w->on_event)
        w->on_event(w->ctx, node, (uint16_t)type, value, vlen);
}

static void
handle_ws_in(struct drawws *w)
{
    uint8_t payload[WS_INMAX];
    int opcode;
    int plen = ws_read(w->ws_fd, payload, sizeof payload, &opcode);
    if (plen < 0 || opcode == 0x8) {    /* error or close */
        ws_close(w);
        return;
    }
    if (opcode == 0x9) {                /* ping */
        ws_pong(w->ws_fd, payload, plen);
        return;
    }
    if (opcode == 0x1 && plen > 0)      /* text */
        dispatch_event(w, payload, plen);
}

static void *
run_server(void *arg)
{
    struct drawws *w = arg;

    while (!__atomic_load_n(&w->stop, __ATOMIC_SEQ_CST)) {
        struct pollfd fds[3];
        int n = 0;
        int i_listen = n;
        fds[n++] = (struct pollfd){ .fd = w->listen_fd, .events = POLLIN };
        int i_nudge = n;
        fds[n++] = (struct pollfd){ .fd = w->nudge[0], .events = POLLIN };
        int i_ws = -1;
        if (w->ws_fd >= 0) {
            i_ws = n;
            fds[n++] = (struct pollfd){ .fd = w->ws_fd, .events = POLLIN };
        }

        if (poll(fds, (nfds_t)n, -1) < 0) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (fds[i_nudge].revents & POLLIN) {
            uint8_t drain[64];
            while (read(w->nudge[0], drain, sizeof drain) > 0)
                ;
            if (w->ws_fd >= 0) {
                int dirty;
                pthread_mutex_lock(&w->lock);
                dirty = w->dirty;
                pthread_mutex_unlock(&w->lock);
                if (dirty)
                    send_latest(w);
            }
        }
        if (i_ws >= 0 && (fds[i_ws].revents & (POLLIN | POLLHUP | POLLERR)))
            handle_ws_in(w);
        if (fds[i_listen].revents & POLLIN)
            accept_conn(w);
    }

    ws_close(w);
    return NULL;
}

/****************************************************************
 * Public API
 ****************************************************************/

struct drawws *
drawws_start(int port, drawws_event_fn on_event, void *ctx)
{
    struct drawws *w = calloc(1, sizeof *w);
    if (!w)
        return NULL;
    w->ws_fd = -1;
    w->on_event = on_event;
    w->ctx = ctx;
    pthread_mutex_init(&w->lock, NULL);

    if (pipe(w->nudge) < 0) {
        free(w);
        return NULL;
    }
    /* Non-blocking both ends: the drain read must return rather than block when
     * the pipe empties, and a push must not block when it is already full. */
    fcntl(w->nudge[0], F_SETFL, O_NONBLOCK);
    fcntl(w->nudge[1], F_SETFL, O_NONBLOCK);

    w->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (w->listen_fd < 0)
        goto fail;
    int opt = 1;
    setsockopt(w->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons((uint16_t)port),
        .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
    };
    if (bind(w->listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0) {
        fprintf(stderr, "[draw] bind 127.0.0.1:%d failed\n", port);
        goto fail;
    }
    if (listen(w->listen_fd, 1) < 0)
        goto fail;

    if (pthread_create(&w->thread, NULL, run_server, w) != 0)
        goto fail;

    fprintf(stderr, "[draw] display at http://127.0.0.1:%d/\n", port);
    return w;

fail:
    if (w->listen_fd >= 0)
        close(w->listen_fd);
    close(w->nudge[0]);
    close(w->nudge[1]);
    free(w);
    return NULL;
}

void
drawws_push(struct drawws *w, const char *svg)
{
    if (!w || !svg)
        return;
    char *copy = strdup(svg);
    if (!copy)
        return;
    pthread_mutex_lock(&w->lock);
    free(w->latest);
    w->latest = copy;
    w->dirty = 1;
    pthread_mutex_unlock(&w->lock);

    uint8_t one = 1;
    if (write(w->nudge[1], &one, 1) < 0) {
        /* A full pipe already carries a pending nudge; nothing to do. */
    }
}

void
drawws_stop(struct drawws *w)
{
    if (!w)
        return;
    __atomic_store_n(&w->stop, 1, __ATOMIC_SEQ_CST);
    uint8_t one = 1;
    if (write(w->nudge[1], &one, 1) < 0) {
        /* The join below still proceeds; the thread also checks the stop flag. */
    }
    pthread_join(w->thread, NULL);

    close(w->listen_fd);
    close(w->nudge[0]);
    close(w->nudge[1]);
    free(w->latest);
    pthread_mutex_destroy(&w->lock);
    free(w);
}
