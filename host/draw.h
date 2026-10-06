/* draw.h : the host draw sink.
 *
 * Receives the guest's structured draw ops over the draw channel and is the
 * authority on what they mean. This step is mechanism only: it validates the
 * op stream structurally and discards the payload, since no SVG is authored and
 * no browser is attached yet (that is M6.2 and M6.3). An optional self-test
 * event source lets the guest's POLL/event path be exercised standalone: a
 * submitted scene that marks a node clickable gets a synthetic click queued
 * back, routed through the same wake path the real front end will use. */
#ifndef DRAW_H
#define DRAW_H

#include "jotun_draw.h"

struct drawdev;

/* Create the sink. chan is the guest draw channel in host memory. wake rouses
 * the guest when an input event is queued (the net provider's contract). test,
 * when nonzero, enables the self-test event source described above. policy is
 * reserved for later display confinement and may be NULL. Returns NULL on
 * failure. */
struct drawdev *draw_init(struct jotun_draw *chan, void (*wake)(void), int test);

/* Handle one request already written into the channel, writing the result
 * back. Called from the draw doorbell. */
void draw_handle(struct drawdev *dd, struct jotun_draw *chan);

/* Release the sink. */
void draw_shutdown(struct drawdev *dd);

#endif /* DRAW_H */
