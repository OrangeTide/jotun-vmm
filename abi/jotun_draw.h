/* jotun_draw.h : the display channel between the guest draw client and the
 * host draw subsystem.
 *
 * Shared verbatim by both sides, fixed-width integers only, no libc. The guest
 * emits a stream of structured draw ops (never markup); the host validates
 * every op and field and is the sole author of the SVG that reaches the
 * browser. See doc/draw-vocabulary.md for the vocabulary this encodes.
 *
 * Transport mirrors the net channel: one request at a time over a shared
 * buffer and a synchronous doorbell. A SUBMIT carries a chunk of op bytes; if
 * the host's outbound queue is full it returns DRAW_EAGAIN and the guest parks
 * the calling task until a draw event rouses it. Input events flow back: the
 * host sets `evt` when one is queued, and the guest fetches it with POLL. */

#ifndef JOTUN_DRAW_H
#define JOTUN_DRAW_H

#include <stdint.h>

/* The channel sits above the net channel (which ends near 0x120A0) and below
 * the guest image at 1 MiB. The doorbell follows the net doorbell. */
#define JOTUN_DRAW_BASE      0x13000u
#define JOTUN_DRAW_DOORBELL  0x0a000028ull
#define JOTUN_DRAW_MSIZE     8192u      /* op bytes per submit, event bytes per poll */

/* Channel operations (guest to host). */
enum {
    DRAW_SUBMIT = 1,    /* buf holds `len` bytes of draw ops */
    DRAW_POLL   = 2,    /* fetch one queued input event into buf */
};

/* status (host to guest): DRAW_OK, DRAW_EAGAIN, or an error. For POLL, DRAW_OK
 * with `len` set to the event byte count, or `len` 0 when nothing is queued. */
#define DRAW_OK       0
#define DRAW_EAGAIN  (-1)   /* outbound full: park and retry the submit */
#define DRAW_EINVAL  (-3)   /* malformed op stream */

struct jotun_draw {
    uint32_t op;            /* DRAW_SUBMIT / DRAW_POLL */
    uint32_t len;           /* SUBMIT: op bytes in buf (in); POLL: event bytes (out) */
    int32_t  status;        /* result code (out) */
    volatile uint32_t evt;  /* host sets when an input event is queued; guest clears */
    uint8_t  buf[JOTUN_DRAW_MSIZE];
};

/* Draw ops: the first byte of each op in a SUBMIT stream. */
enum {
    DOP_SURFACE  = 0x01,    /* id:u32 x:i16 y:i16 w:u16 h:u16 title:str */
    DOP_GROUP    = 0x02,    /* id:u32 parent:u32 tx:i16 ty:i16 */
    DOP_RECT     = 0x10,    /* id parent x:i16 y:i16 w:u16 h:u16 rx:u16 style */
    DOP_CIRCLE   = 0x11,    /* id parent cx:i16 cy:i16 r:u16 style */
    DOP_ELLIPSE  = 0x12,    /* id parent cx:i16 cy:i16 rx:u16 ry:u16 style */
    DOP_LINE     = 0x13,    /* id parent x1:i16 y1:i16 x2:i16 y2:i16 style */
    DOP_POLYLINE = 0x14,    /* id parent closed:u8 n:u16 pts:i16[2n] style */
    DOP_PATH     = 0x15,    /* id parent n:u16 seg[n] style */
    DOP_TEXT     = 0x16,    /* id parent x:i16 y:i16 size:u16 s:str style */
    DOP_IMAGE    = 0x17,    /* id parent x:i16 y:i16 w:u16 h:u16 fmt:u8 stride:u32 gpa:u64 */
    DOP_IMAGE_DAMAGE = 0x18,/* id:u32 x:u16 y:u16 w:u16 h:u16 (region in image pixels) */
    DOP_SET      = 0x20,    /* id:u32 attr:u16 value (typed by attr) */
    DOP_SET_TEXT = 0x21,    /* id:u32 s:str */
    DOP_REMOVE   = 0x22,    /* id:u32 */
    DOP_INTERACT = 0x23,    /* id:u32 kind:u16 params */
    DOP_LISTEN   = 0x24,    /* id:u32 event:u16 */
};

/* Attributes for DOP_SET. The value that follows the attribute is typed by it:
 *   i16   coordinates: X Y CX CY X1 Y1 X2 Y2
 *   u16   dimensions:  W H R RX RY STROKE_WIDTH
 *   color FILL STROKE  (u32: 0xRRGGBB or DRAW_COLOR_NONE)
 *   u8    OPACITY (0..255), VISIBLE (0 or 1), FONT (0 sans, 1 serif, 2 mono)
 * Geometry attributes apply only to the node kinds that have that field; a
 * mismatch is a protocol error. Style attributes and VISIBLE apply to any node. */
enum {
    DSA_X = 1, DSA_Y, DSA_W, DSA_H,
    DSA_CX, DSA_CY, DSA_R, DSA_RX, DSA_RY,
    DSA_X1, DSA_Y1, DSA_X2, DSA_Y2,
    DSA_FILL, DSA_STROKE, DSA_STROKE_WIDTH,
    DSA_OPACITY, DSA_VISIBLE, DSA_FONT,
};

/* DOP_PATH segment commands (the cmd:u8), each followed by its fixed count of
 * i16 arguments. The set is closed; the host re-emits the path string itself. */
enum {
    DPATH_M = 'M',      /* moveto: 2 args (x y) */
    DPATH_L = 'L',      /* lineto: 2 args (x y) */
    DPATH_H = 'H',      /* horizontal lineto: 1 arg (x) */
    DPATH_V = 'V',      /* vertical lineto: 1 arg (y) */
    DPATH_C = 'C',      /* cubic bezier: 6 args */
    DPATH_Q = 'Q',      /* quadratic bezier: 4 args */
    DPATH_Z = 'Z',      /* closepath: 0 args */
};

/* Style presence-mask bits; present fields follow the u16 mask in bit order. */
enum {
    DST_FILL         = 1u << 0,     /* color */
    DST_STROKE       = 1u << 1,     /* color */
    DST_STROKE_WIDTH = 1u << 2,     /* u16 */
    DST_OPACITY      = 1u << 3,     /* u8, 0..255 */
    DST_FONT         = 1u << 4,     /* u8 enum: 0 sans, 1 serif, 2 mono */
};

/* color: low 24 bits are 0xRRGGBB; this sentinel means "none" (no paint). */
#define DRAW_COLOR_NONE  0x01000000u

/* Pixel formats for DOP_IMAGE. The pixels live in guest RAM at `gpa` (the host
 * reads them there, never over the channel); `stride` is the row length in
 * bytes, or 0 for the tight w*bpp. RGBA8888 is four bytes per pixel, R,G,B,A. */
enum {
    DIMG_RGBA8888 = 0,
};

/* DOP_IMAGE_DAMAGE marks a sub-rectangle of an existing image node changed since
 * its last submit, so the host re-encodes and re-sends only that region instead
 * of the whole frame. Coordinates are in image pixels, (0,0) at the top-left of
 * the image. The region is clamped to the image bounds. Re-submitting DOP_IMAGE
 * for the same id instead re-bases the whole frame. */

/* Interaction kinds for DOP_INTERACT. */
enum {
    DI_HOVER_FILL = 1,  /* params: to:color */
    DI_PRESS_FILL = 2,  /* params: to:color */
    DI_DRAG       = 3,  /* params: axis:u8 ('h'/'v') track:u16 */
    DI_XY         = 4,  /* no params */
    DI_CLICK      = 5,  /* no params */
};

/* Event types, shared by DOP_LISTEN and the POLL event stream. */
enum {
    DEV_CLICK  = 1,     /* no value */
    DEV_CLOSE  = 2,     /* no value */
    DEV_SCROLL = 3,     /* value: norm u16 (0..65535 maps 0.0..1.0) */
    DEV_XY     = 4,     /* value: norm u16 x, norm u16 y */
    DEV_TEXT   = 5,     /* value: UTF-8 */
    DEV_PASTE  = 6,     /* value: UTF-8 */
};

/* POLL returns one event in buf as: node:u32 type:u16 vlen:u16 value[vlen]. */

#endif /* JOTUN_DRAW_H */
