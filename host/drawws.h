/* drawws.h : the host WebSocket front end for the draw subsystem.
 *
 * Serves a small browser client over HTTP on localhost and, on the same port,
 * upgrades a WebSocket connection to it. The host pushes whole-document SVG
 * (authored by the draw subsystem) to the browser, and the browser reports
 * input events back. One browser at a time; a new connection replaces the old.
 *
 * All socket operations run on one internal thread. A push from another thread
 * stores the latest scene and nudges that thread through a self-pipe, so the
 * connection fd is only ever touched by the server thread and there are no fd
 * races. This mirrors the net provider's single-owner design. */
#ifndef DRAWWS_H
#define DRAWWS_H

#include <stdint.h>

struct drawws;

/* Called on the server thread when the browser reports an input event. node is
 * the guest node id, type is a DEV_* code, and value is the optional payload
 * (vlen bytes, not NUL-terminated). The callback must be quick and thread-aware
 * (it rouses the guest). */
typedef void (*drawws_event_fn)(void *ctx, uint32_t node, uint16_t type,
                                const char *value, int vlen);

/* Start the server bound to 127.0.0.1:port. on_event (with ctx) is invoked for
 * each inbound event. Returns NULL on failure. */
struct drawws *drawws_start(int port, drawws_event_fn on_event, void *ctx);

/* Replace the current scene and send it to the connected browser, if any.
 * Thread-safe; svg is copied. A browser that connects later receives the most
 * recent scene as its first paint. */
void drawws_push(struct drawws *w, const char *svg);

/* Stop the server thread and close all sockets. */
void drawws_stop(struct drawws *w);

#endif /* DRAWWS_H */
