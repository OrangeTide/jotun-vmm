/* http.h : a minimal HTTP/1.1 GET client for the bundle pull path.
 *
 * Plain http:// only. It fetches static files from an origin (no request body,
 * no redirects, no TLS); ref authenticity comes from the signature on the
 * pulled record, not from the transport. One connection per request, closed by
 * the server. Enough for the REEF HTTP download profile's loose-object GETs. */
#ifndef HTTP_H
#define HTTP_H

#include <stddef.h>

/* A growable response-body buffer. */
struct http_buf {
    char  *data;
    size_t len, cap;
};

void http_buf_free(struct http_buf *b);

/* A parsed http:// origin. `path` is the base path with no trailing slash
 * ("" for the host root). */
struct http_url {
    char host[256];
    char port[8];
    char path[1024];
};

/* Parse an "http://host[:port][/base/path]" URL. Returns 0 on success, -1 on a
 * malformed or non-http URL. */
int http_url_parse(const char *url, struct http_url *out);

/* GET origin's base path joined with `rel` (a relative path, no leading slash).
 * The body is placed in `buf` (reset first). Returns the HTTP status code on a
 * complete response, or -1 on a transport or protocol error. */
int http_get(const struct http_url *origin, const char *rel, struct http_buf *buf);

#endif /* HTTP_H */
