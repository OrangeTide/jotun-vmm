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
void draw_line(uint32_t id, uint32_t parent, int x1, int y1, int x2, int y2,
               const struct draw_style *st);
void draw_text(uint32_t id, uint32_t parent, int x, int y, int size,
               const char *s, const struct draw_style *st);
void draw_set_text(uint32_t id, const char *s);
void draw_remove(uint32_t id);
void draw_listen(uint32_t id, int event);
void draw_click(uint32_t id);                       /* clickable: DI_CLICK */
void draw_hover_fill(uint32_t id, uint32_t to);     /* DI_HOVER_FILL */

/* Send the accumulated ops. Parks the task if the host's queue is momentarily
 * full, retrying when a draw event rouses it. */
void draw_flush(void);

/* Block the task until an input event arrives, filling ev. Returns 0, or -1 on
 * a channel error. Flushes any pending ops first. */
int draw_next_event(struct draw_event *ev);

#endif /* LIBOS_DRAW_H */
