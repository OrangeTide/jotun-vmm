/* draw.h : the guest draw client.
 *
 * A structured-op emitter over the draw channel. Scene ops accumulate and are
 * sent as a chunk by draw_flush (or automatically when the buffer fills); the
 * guest never emits markup. Input events are read with draw_next_event, which
 * parks the calling task until one arrives. The channel carries one request at
 * a time, so a lock serialises callers. */
#ifndef LIBOS_DRAW_H
#define LIBOS_DRAW_H

#include <stdint.h>

#include "jotun_draw.h"

/* A style for a shape op. mask selects which fields are present (DST_* bits);
 * absent fields inherit the SVG default. */
struct draw_style {
    uint32_t mask;
    uint32_t fill;          /* color: 0xRRGGBB or DRAW_COLOR_NONE */
    uint32_t stroke;        /* color */
    uint16_t stroke_width;
    uint8_t  opacity;       /* 0..255 */
    uint8_t  font;          /* 0 sans, 1 serif, 2 mono */
};

/* A decoded input event. */
struct draw_event {
    uint32_t node;
    uint16_t type;          /* DEV_* */
    uint16_t vlen;
    char     value[64];     /* event-specific, NUL-terminated */
};

/* One path segment: a command (DPATH_*) and up to six i16 arguments, of which
 * the command's fixed count are used. */
struct draw_seg {
    uint8_t cmd;
    int16_t a[6];
};

/* Register the draw event hook with the scheduler. Call once at boot. */
void draw_init(void);

/* Scene construction. ids are guest-assigned and nonzero; parent is a surface
 * or group id. Ops are buffered until draw_flush. */
void draw_surface(uint32_t id, int x, int y, int w, int h, const char *title);
void draw_group(uint32_t id, uint32_t parent, int tx, int ty);
void draw_rect(uint32_t id, uint32_t parent, int x, int y, int w, int h,
               int rx, const struct draw_style *st);
void draw_circle(uint32_t id, uint32_t parent, int cx, int cy, int r,
                 const struct draw_style *st);
void draw_ellipse(uint32_t id, uint32_t parent, int cx, int cy, int rx, int ry,
                  const struct draw_style *st);
void draw_line(uint32_t id, uint32_t parent, int x1, int y1, int x2, int y2,
               const struct draw_style *st);
/* A connected set of n points (x,y pairs in pts[0..2n)); closed selects a
 * filled polygon over an open polyline. n is capped by the host. */
void draw_polyline(uint32_t id, uint32_t parent, int closed,
                   const int16_t *pts, int n, const struct draw_style *st);
/* A path of n segments. The host re-emits the path string from the validated
 * commands and arguments; no path text crosses from the guest. */
void draw_path(uint32_t id, uint32_t parent, const struct draw_seg *segs, int n,
               const struct draw_style *st);
void draw_text(uint32_t id, uint32_t parent, int x, int y, int size,
               const char *s, const struct draw_style *st);
/* Place an image node showing a framebuffer in guest RAM. pixels is RGBA8888;
 * stride is the row length in bytes, or 0 for w*4. The host reads the pixels
 * from guest memory each time the scene is flushed, so re-rendering into the
 * same buffer and flushing again updates the node. */
void draw_image(uint32_t id, uint32_t parent, int x, int y, int w, int h,
                const void *pixels, int stride);
void draw_set_text(uint32_t id, const char *s);
/* Set one attribute of an existing node. attr is a DSA_* code; value is read at
 * the width that attribute implies (see jotun_draw.h). */
void draw_set(uint32_t id, int attr, long value);
void draw_remove(uint32_t id);
void draw_listen(uint32_t id, int event);
void draw_click(uint32_t id);                       /* clickable: DI_CLICK */
void draw_hover_fill(uint32_t id, uint32_t to);     /* DI_HOVER_FILL */

/* Make a node a drag handle. vertical selects the axis; track is the travel
 * length in device units. On release the client reports a DEV_SCROLL event
 * whose value is one norm u16 (0..65535 maps 0.0..1.0). */
void draw_drag(uint32_t id, int vertical, int track);

/* Make a node a 2D handle. On release the client reports a DEV_XY event whose
 * value is two norm u16 (x then y). */
void draw_xy(uint32_t id);

/* Decode the idx-th little-endian norm u16 from an event value (for DEV_SCROLL
 * and DEV_XY). Returns 0 when the value is shorter than that. */
unsigned draw_event_norm(const struct draw_event *ev, int idx);

/* Send the accumulated ops. Parks the task if the host's queue is momentarily
 * full, retrying when a draw event rouses it. */
void draw_flush(void);

/* Block the task until an input event arrives, filling ev. Returns 0, or -1 on
 * a channel error. Flushes any pending ops first. */
int draw_next_event(struct draw_event *ev);

#endif /* LIBOS_DRAW_H */
