/* draw.h : the host draw sink.
 *
 * Receives the guest's structured draw ops over the draw channel, holds the
 * authoritative per-guest scene, and authors the SVG that reaches the browser.
 * Image nodes reference a framebuffer in guest RAM; the host reads those pixels
 * through the supplied address translator and encodes them as a PNG data URI.
 * A self-test event source (test != 0) can queue a synthetic click so the
 * guest's POLL/event path runs with no browser attached. */
#ifndef DRAW_H
#define DRAW_H

#include <stdint.h>

#include "jotun_draw.h"

struct drawdev;

/* Translate a guest-physical range [gpa, gpa+len) to a host pointer, or NULL if
 * it does not lie entirely within guest RAM. */
typedef void *(*draw_gpa_fn)(void *ctx, uint64_t gpa, uint64_t len);

/* Create the sink. chan is the guest draw channel in host memory. gid is the
 * owning guest's id, which namespaces the browser DOM ids this sink authors.
 * wake (with wake_ctx) rouses the guest that owns this sink when an input event
 * is queued (the net provider's contract). test, when nonzero, enables the
 * self-test event source. gpa (with gpa_ctx) reads image pixels from guest RAM
 * and may be NULL to disable image nodes. Returns NULL on failure. */
struct drawdev *draw_init(struct jotun_draw *chan, int gid,
                          void (*wake)(void *ctx), void *wake_ctx, int test,
                          draw_gpa_fn gpa, void *gpa_ctx);

/* Handle one request already written into the channel, writing the result
 * back. Called from the draw doorbell. */
void draw_handle(struct drawdev *dd, struct jotun_draw *chan);

/* Release the sink. */
void draw_shutdown(struct drawdev *dd);

#endif /* DRAW_H */
