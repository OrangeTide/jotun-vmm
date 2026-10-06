/* triton.c : a minimal guest-side software 3D rasterizer and a cube demo.
 *
 * Gouraud-shaded, z-buffered triangles into an RGBA8888 framebuffer in guest
 * RAM. The host never sees a rendering command, only the finished pixels, which
 * it reads through the draw pixel path. This file uses floating point and so is
 * built with the FPU enabled, unlike the integer-only rest of the library OS.
 */
#include "triton.h"

#include "draw.h"
#include "print.h"

/****************************************************************
 * Rasterizer
 ****************************************************************/

static float
clampf(float v, float lo, float hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void
tr_clear(struct tr_target *t, uint8_t r, uint8_t g, uint8_t b)
{
    for (int y = 0; y < t->h; y++) {
        uint8_t *row = t->fb + (size_t)y * t->stride;
        for (int x = 0; x < t->w; x++) {
            uint8_t *p = row + x * 4;
            p[0] = r; p[1] = g; p[2] = b; p[3] = 255;
        }
    }
    int n = t->w * t->h;
    for (int i = 0; i < n; i++)
        t->zbuf[i] = 1.0f;          /* far plane */
}

/* Edge function: twice the signed area of the triangle (a, b, p). Positive when
 * p is on one side of the directed edge a->b, negative on the other. */
static float
edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

void
tr_triangle(struct tr_target *t, const struct tr_vertex *a,
            const struct tr_vertex *b, const struct tr_vertex *c)
{
    float area = edge(a->x, a->y, b->x, b->y, c->x, c->y);
    if (area == 0.0f)
        return;                     /* degenerate: no coverage */
    float inv = 1.0f / area;

    /* Bounding box of the triangle, clamped to the target. A positive cast
     * truncates toward zero, which is the floor we want for non-negative
     * screen coordinates; widen the max by one to cover the fractional edge. */
    float minxf = a->x, maxxf = a->x, minyf = a->y, maxyf = a->y;
    if (b->x < minxf) minxf = b->x; if (b->x > maxxf) maxxf = b->x;
    if (c->x < minxf) minxf = c->x; if (c->x > maxxf) maxxf = c->x;
    if (b->y < minyf) minyf = b->y; if (b->y > maxyf) maxyf = b->y;
    if (c->y < minyf) minyf = c->y; if (c->y > maxyf) maxyf = c->y;

    int minx = (int)minxf, maxx = (int)maxxf + 1;
    int miny = (int)minyf, maxy = (int)maxyf + 1;
    if (minx < 0) minx = 0;
    if (miny < 0) miny = 0;
    if (maxx > t->w) maxx = t->w;
    if (maxy > t->h) maxy = t->h;

    for (int y = miny; y < maxy; y++) {
        uint8_t *row = t->fb + (size_t)y * t->stride;
        float *zrow = t->zbuf + (size_t)y * t->w;
        for (int x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            /* Barycentric weights, normalized so the inside test is a sign
             * check that works for either winding (inv carries area's sign). */
            float l0 = edge(b->x, b->y, c->x, c->y, px, py) * inv;
            float l1 = edge(c->x, c->y, a->x, a->y, px, py) * inv;
            float l2 = edge(a->x, a->y, b->x, b->y, px, py) * inv;
            if (l0 < 0.0f || l1 < 0.0f || l2 < 0.0f)
                continue;

            float z = l0 * a->z + l1 * b->z + l2 * c->z;
            if (z > zrow[x])            /* less-or-equal depth test */
                continue;
            zrow[x] = z;

            float r = l0 * a->r + l1 * b->r + l2 * c->r;
            float g = l0 * a->g + l1 * b->g + l2 * c->g;
            float bl = l0 * a->b + l1 * b->b + l2 * c->b;
            uint8_t *p = row + x * 4;
            p[0] = (uint8_t)(clampf(r, 0.0f, 1.0f) * 255.0f);
            p[1] = (uint8_t)(clampf(g, 0.0f, 1.0f) * 255.0f);
            p[2] = (uint8_t)(clampf(bl, 0.0f, 1.0f) * 255.0f);
            p[3] = 255;
        }
    }
}

/****************************************************************
 * Cube demo
 ****************************************************************/

#define TW 256
#define TH 192
static uint8_t tfb[TW * TH * 4];
static float   tzb[TW * TH];

/* Unit cube corners and the six faces as quads (two triangles each). */
static const float corner[8][3] = {
    { -1, -1, -1 }, {  1, -1, -1 }, {  1,  1, -1 }, { -1,  1, -1 },
    { -1, -1,  1 }, {  1, -1,  1 }, {  1,  1,  1 }, { -1,  1,  1 },
};
static const int face[6][4] = {
    { 0, 1, 2, 3 },     /* back   */
    { 5, 4, 7, 6 },     /* front  */
    { 4, 0, 3, 7 },     /* left   */
    { 1, 5, 6, 2 },     /* right  */
    { 4, 5, 1, 0 },     /* bottom */
    { 3, 2, 6, 7 },     /* top    */
};

/* Rotate each corner by the running angles, project orthographically to the
 * window, and derive depth and a per-corner color from its position. */
static void
project(struct tr_vertex out[8], float cx, float sx, float cy, float sy)
{
    float scale = (float)TW * 0.26f;
    float ox = (float)TW * 0.5f, oy = (float)TH * 0.5f;
    for (int i = 0; i < 8; i++) {
        float x = corner[i][0], y = corner[i][1], z = corner[i][2];
        /* rotate about Y, then about X */
        float x1 =  x * cy + z * sy;
        float z1 = -x * sy + z * cy;
        float y2 =  y * cx - z1 * sx;
        float z2 =  y * sx + z1 * cx;
        out[i].x = ox + x1 * scale;
        out[i].y = oy - y2 * scale;
        out[i].z = clampf((2.0f - z2) * 0.25f, 0.0f, 1.0f);
        out[i].r = (corner[i][0] + 1.0f) * 0.5f;
        out[i].g = (corner[i][1] + 1.0f) * 0.5f;
        out[i].b = (corner[i][2] + 1.0f) * 0.5f;
    }
}

static void
render_frame(struct tr_target *t, float cx, float sx, float cy, float sy)
{
    struct tr_vertex v[8];
    project(v, cx, sx, cy, sy);
    tr_clear(t, 0x10, 0x18, 0x30);
    for (int f = 0; f < 6; f++) {
        const int *q = face[f];
        tr_triangle(t, &v[q[0]], &v[q[1]], &v[q[2]]);
        tr_triangle(t, &v[q[0]], &v[q[2]], &v[q[3]]);
    }
}

void
triton_demo(void)
{
    struct tr_target t = { tfb, tzb, TW, TH, TW * 4 };

    /* Running rotation: start at identity and advance by a fixed delta each
     * frame using the angle-sum identities, so no sin/cos routine is needed. */
    const float dc = 0.97815f, ds = 0.20791f;   /* cos/sin of about 12 degrees */
    float cx = 1.0f, sx = 0.0f;                  /* rotation about X */
    float cy = 1.0f, sy = 0.0f;                  /* rotation about Y */

    render_frame(&t, cx, sx, cy, sy);
    draw_surface(1, 0, 0, TW + 20, TH + 20, "triton");
    draw_image(2, 1, 10, 10, TW, TH, tfb, 0);
    draw_click(2);
    draw_flush();
    con_puts("triton: cube submitted, click to spin\n");

    for (;;) {
        struct draw_event ev;
        if (draw_next_event(&ev) != 0) {
            con_puts("triton: event error\n");
            return;
        }
        if (ev.type == DEV_CLOSE)
            break;
        if (ev.type == DEV_CLICK && ev.node == 2) {
            /* advance both angles by the fixed delta */
            float ncx = cx * dc - sx * ds, nsx = sx * dc + cx * ds;
            float ncy = cy * dc - sy * ds, nsy = sy * dc + cy * ds;
            cx = ncx; sx = nsx; cy = ncy; sy = nsy;
            render_frame(&t, cx, sx, cy, sy);
            draw_image(2, 1, 10, 10, TW, TH, tfb, 0);   /* re-base the frame */
            draw_flush();
            con_puts("triton: frame submitted\n");
        }
    }
    con_puts("triton: done\n");
}
