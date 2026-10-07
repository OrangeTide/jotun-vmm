/* triton.c : a guest-side software 3D rasterizer and a cube demo.
 *
 * Gouraud-shaded, z-buffered triangles into an RGBA8888 framebuffer in guest
 * RAM, with perspective-correct texturing, the Glide-style color-combine modes,
 * point and bilinear filtering, mip-mapped (trilinear) textures, anisotropic
 * filtering, and source-over alpha blending. The host never sees a rendering
 * command, only the finished
 * pixels, which it reads through the draw pixel path. This file uses floating
 * point and so is built with the FPU enabled, unlike the integer-only rest of
 * the library OS.
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

void
tr_tex_source(struct tr_target *t, const struct tr_texture *tex)
{
    t->tex = tex;
}

void
tr_color_combine(struct tr_target *t, int mode)
{
    t->combine = mode;
}

void
tr_blend(struct tr_target *t, int enable)
{
    t->blend = enable;
}

void
tr_tex_filter(struct tr_target *t, int filter)
{
    t->filter = filter;
}

int
tr_build_mips(struct tr_texture *t, const uint8_t *base, int w, int h,
              uint8_t *buf, int bufcap)
{
    t->w = w;
    t->h = h;
    t->level[0] = base;
    int nlevels = 1;

    const uint8_t *src = base;
    int sw = w, sh = h, used = 0;
    while ((sw > 1 || sh > 1) && nlevels < TR_MAXMIP) {
        int dw = sw > 1 ? sw / 2 : 1;
        int dh = sh > 1 ? sh / 2 : 1;
        int bytes = dw * dh * 4;
        if (used + bytes > bufcap)
            break;                      /* out of scratch: stop the chain */
        uint8_t *dst = buf + used;
        for (int y = 0; y < dh; y++) {
            int sy0 = sh > 1 ? y * 2 : 0, sy1 = sh > 1 ? sy0 + 1 : sy0;
            for (int x = 0; x < dw; x++) {
                int sx0 = sw > 1 ? x * 2 : 0, sx1 = sw > 1 ? sx0 + 1 : sx0;
                const uint8_t *p00 = src + ((size_t)sy0 * sw + sx0) * 4;
                const uint8_t *p01 = src + ((size_t)sy0 * sw + sx1) * 4;
                const uint8_t *p10 = src + ((size_t)sy1 * sw + sx0) * 4;
                const uint8_t *p11 = src + ((size_t)sy1 * sw + sx1) * 4;
                uint8_t *d = dst + ((size_t)y * dw + x) * 4;
                for (int k = 0; k < 4; k++)
                    d[k] = (uint8_t)((p00[k] + p01[k] + p10[k] + p11[k] + 2) / 4);
            }
        }
        t->level[nlevels++] = dst;
        src = dst;
        sw = dw; sh = dh;
        used += bytes;
    }
    t->nlevels = nlevels;
    return nlevels;
}

/* Edge function: twice the signed area of the triangle (a, b, p). Positive when
 * p is on one side of the directed edge a->b, negative on the other. */
static float
edge(float ax, float ay, float bx, float by, float px, float py)
{
    return (bx - ax) * (py - ay) - (by - ay) * (px - ax);
}

/* Floor, without libm. */
static float
flr(float x)
{
    int i = (int)x;
    return (float)((x < 0.0f && (float)i != x) ? i - 1 : i);
}

/* Approximate log2 of a positive float: the exact exponent plus a linear
 * mantissa term. Monotonic and within about 0.09, ample for picking a mip
 * level. */
static float
log2_approx(float x)
{
    union { float f; uint32_t i; } u = { x };
    float e = (float)((int)((u.i >> 23) & 0xff) - 127);
    u.i = (u.i & 0x007fffffu) | 0x3f800000u;    /* mantissa in [1, 2) */
    return e + (u.f - 1.0f);
}

/* Fetch one texel of a level (dimensions lw, lh), clamped to the edge. */
static void
texel(const uint8_t *tx, int lw, int lh, int x, int y, float o[4])
{
    if (x < 0) x = 0; else if (x >= lw) x = lw - 1;
    if (y < 0) y = 0; else if (y >= lh) y = lh - 1;
    const uint8_t *p = tx + ((size_t)y * lw + x) * 4;
    o[0] = p[0] / 255.0f; o[1] = p[1] / 255.0f;
    o[2] = p[2] / 255.0f; o[3] = p[3] / 255.0f;
}

/* Sample one mip level at (u, v), point or bilinear. */
static void
sample_level(const struct tr_texture *tx, int lvl, float u, float v,
             int filter, float o[4])
{
    int lw = tx->w >> lvl; if (lw < 1) lw = 1;
    int lh = tx->h >> lvl; if (lh < 1) lh = 1;
    const uint8_t *tex = tx->level[lvl];

    if (filter == TR_FILTER_POINT) {
        texel(tex, lw, lh, (int)(u * lw), (int)(v * lh), o);
        return;
    }
    /* Bilinear: blend the four texels around the sample at their centres. */
    float fx = u * lw - 0.5f, fy = v * lh - 0.5f;
    int x0 = (int)flr(fx), y0 = (int)flr(fy);
    float dx = fx - (float)x0, dy = fy - (float)y0;
    float c00[4], c10[4], c01[4], c11[4];
    texel(tex, lw, lh, x0,     y0,     c00);
    texel(tex, lw, lh, x0 + 1, y0,     c10);
    texel(tex, lw, lh, x0,     y0 + 1, c01);
    texel(tex, lw, lh, x0 + 1, y0 + 1, c11);
    for (int k = 0; k < 4; k++) {
        float top = c00[k] + (c10[k] - c00[k]) * dx;
        float bot = c01[k] + (c11[k] - c01[k]) * dx;
        o[k] = top + (bot - top) * dy;
    }
}

/* Sample a texture at (u, v) and level-of-detail lod. With mips, the two
 * bracketing levels are sampled and blended by the lod fraction (trilinear). */
static void
sample(const struct tr_texture *tx, float u, float v, float lod,
       int filter, float o[4])
{
    if (tx->nlevels <= 1 || lod <= 0.0f) {
        sample_level(tx, 0, u, v, filter, o);
        return;
    }
    float d = lod;
    if (d > (float)(tx->nlevels - 1))
        d = (float)(tx->nlevels - 1);
    int lo = (int)d;
    int hi = lo + 1 < tx->nlevels ? lo + 1 : lo;
    float f = d - (float)lo;
    float a[4], b[4];
    sample_level(tx, lo, u, v, filter, a);
    sample_level(tx, hi, u, v, filter, b);
    for (int k = 0; k < 4; k++)
        o[k] = a[k] + (b[k] - a[k]) * f;
}

/* Square root, without libm: a bit-hack initial guess refined by two Newton
 * steps. Used for footprint lengths, where a small error is harmless. */
static float
sqrt_approx(float x)
{
    if (x <= 0.0f)
        return 0.0f;
    union { float f; uint32_t i; } u = { x };
    u.i = (u.i >> 1) + 0x1fbd1df5u;
    float y = u.f;
    y = 0.5f * (y + x / y);
    y = 0.5f * (y + x / y);
    return y;
}

/* Sample a texture over an elliptical footprint given by the per-pixel texture-
 * coordinate derivatives (dudx, dvdx) and (dudy, dvdy), in texels. The footprint
 * is approximated by the longer and shorter of those two vectors: several
 * trilinear taps are spread along the long axis, each at a mip level matched to
 * the short axis, and averaged. With maxaniso == 1 (or a round footprint) this
 * is one ordinary trilinear tap. */
static void
sample_aniso(const struct tr_texture *tx, float u, float v,
             float dudx, float dvdx, float dudy, float dvdy,
             float tw, float th, int maxaniso, int filter, float o[4])
{
    float lx2 = dudx * dudx + dvdx * dvdx;      /* squared footprint lengths */
    float ly2 = dudy * dudy + dvdy * dvdy;

    float majdu, majdv, majlen2, minlen2;
    if (lx2 >= ly2) {
        majdu = dudx; majdv = dvdx; majlen2 = lx2; minlen2 = ly2;
    } else {
        majdu = dudy; majdv = dvdy; majlen2 = ly2; minlen2 = lx2;
    }
    float pmax = sqrt_approx(majlen2);
    float pmin = sqrt_approx(minlen2);
    if (pmin < 1e-4f)
        pmin = 1e-4f;

    int n = (int)(pmax / pmin + 0.5f);          /* taps = footprint aspect */
    if (n < 1) n = 1;
    if (n > maxaniso) n = maxaniso;

    /* Each tap covers about the short-axis extent, so pick its level there. */
    float lvl = pmax / (float)n;
    float lod = lvl > 1.0f ? log2_approx(lvl) : 0.0f;

    if (n == 1) {
        sample(tx, u, v, lod, filter, o);
        return;
    }

    /* The full long-axis footprint vector, in normalized uv, spread the taps
     * across [-0.5, 0.5) of it. */
    float mu = majdu / tw, mv = majdv / th;
    float acc[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        float s = ((float)i + 0.5f) / (float)n - 0.5f;
        float c[4];
        sample(tx, u + mu * s, v + mv * s, lod, filter, c);
        for (int k = 0; k < 4; k++)
            acc[k] += c[k];
    }
    float invn = 1.0f / (float)n;
    for (int k = 0; k < 4; k++)
        o[k] = acc[k] * invn;
}

void
tr_tex_aniso(struct tr_target *t, int maxaniso)
{
    t->maxaniso = maxaniso;
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

    /* Perspective-correct interpolants: u/w, v/w and 1/w are linear in screen
     * space, so interpolate those and divide per pixel. With w == 1 (an
     * orthographic vertex) this reduces to the exact affine result. */
    float iwa = 1.0f / a->w, iwb = 1.0f / b->w, iwc = 1.0f / c->w;
    float uoa = a->u * iwa, uob = b->u * iwb, uoc = c->u * iwc;
    float voa = a->v * iwa, vob = b->v * iwb, voc = c->v * iwc;

    /* For a mipped texture, the screen-space derivatives of (u, v) pick the mip
     * level. The barycentric weights' gradients are constant per triangle, so
     * the gradients of u/w, v/w and 1/w are too; the quotient rule gives
     * du/dx etc. per pixel. */
    int textured = t->tex && t->combine != TR_COMBINE_ITERATED;
    int do_mip = textured && t->tex->nlevels > 1;
    float dUodx = 0, dUody = 0, dVodx = 0, dVody = 0, dIWdx = 0, dIWdy = 0;
    float tw = textured ? (float)t->tex->w : 1.0f;
    float th = textured ? (float)t->tex->h : 1.0f;
    if (do_mip) {
        float dl0dx = (b->y - c->y) * inv, dl0dy = (c->x - b->x) * inv;
        float dl1dx = (c->y - a->y) * inv, dl1dy = (a->x - c->x) * inv;
        float dl2dx = (a->y - b->y) * inv, dl2dy = (b->x - a->x) * inv;
        dUodx = dl0dx * uoa + dl1dx * uob + dl2dx * uoc;
        dUody = dl0dy * uoa + dl1dy * uob + dl2dy * uoc;
        dVodx = dl0dx * voa + dl1dx * vob + dl2dx * voc;
        dVody = dl0dy * voa + dl1dy * vob + dl2dy * voc;
        dIWdx = dl0dx * iwa + dl1dx * iwb + dl2dx * iwc;
        dIWdy = dl0dy * iwa + dl1dy * iwb + dl2dy * iwc;
    }

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
            if (!t->blend)              /* translucent surfaces do not write z */
                zrow[x] = z;

            /* Iterated (gouraud) color and alpha. */
            float cr = l0 * a->r + l1 * b->r + l2 * c->r;
            float cg = l0 * a->g + l1 * b->g + l2 * c->g;
            float cb = l0 * a->b + l1 * b->b + l2 * c->b;
            float ca = l0 * a->a + l1 * b->a + l2 * c->a;

            /* Fold in the texture per the combine mode. */
            if (textured) {
                float W = l0 * iwa + l1 * iwb + l2 * iwc;   /* 1/w */
                float invW = 1.0f / W;
                float uo = l0 * uoa + l1 * uob + l2 * uoc;
                float vo = l0 * voa + l1 * vob + l2 * voc;
                float u = uo * invW, v = vo * invW;

                float tc[4];
                if (do_mip) {
                    /* Texel-space derivatives of (u, v), by the quotient rule
                     * on the perspective-correct interpolants. */
                    float w2 = invW * invW;
                    float dudx = (dUodx * W - uo * dIWdx) * w2 * tw;
                    float dudy = (dUody * W - uo * dIWdy) * w2 * tw;
                    float dvdx = (dVodx * W - vo * dIWdx) * w2 * th;
                    float dvdy = (dVody * W - vo * dIWdy) * w2 * th;
                    int na = t->maxaniso < 1 ? 1 : t->maxaniso;
                    sample_aniso(t->tex, u, v, dudx, dvdx, dudy, dvdy,
                                 tw, th, na, t->filter, tc);
                } else {
                    sample(t->tex, u, v, 0.0f, t->filter, tc);
                }
                switch (t->combine) {
                case TR_COMBINE_TEXTURE:
                    cr = tc[0]; cg = tc[1]; cb = tc[2]; ca = tc[3];
                    break;
                case TR_COMBINE_ADD:
                    cr += tc[0]; cg += tc[1]; cb += tc[2]; ca *= tc[3];
                    break;
                case TR_COMBINE_MODULATE:
                default:
                    cr *= tc[0]; cg *= tc[1]; cb *= tc[2]; ca *= tc[3];
                    break;
                }
            }

            uint8_t *p = row + x * 4;
            if (t->blend) {
                /* Source-over: out = src*a + dst*(1-a). */
                float ia = 1.0f - ca;
                cr = cr * ca + (p[0] / 255.0f) * ia;
                cg = cg * ca + (p[1] / 255.0f) * ia;
                cb = cb * ca + (p[2] / 255.0f) * ia;
            }
            p[0] = (uint8_t)(clampf(cr, 0.0f, 1.0f) * 255.0f);
            p[1] = (uint8_t)(clampf(cg, 0.0f, 1.0f) * 255.0f);
            p[2] = (uint8_t)(clampf(cb, 0.0f, 1.0f) * 255.0f);
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

/* A procedural checkerboard texture with a mip chain, built once so the demo
 * needs no asset. The fine 4-texel squares alias badly without mips, which is
 * what makes the mip path worth exercising. */
#define CKW 64
#define CKH 64
static uint8_t checker[CKW * CKH * 4];
static uint8_t checker_mips[CKW * CKH * 4];     /* scratch for levels 1.. */
static struct tr_texture checker_tex;

static void
make_checker(void)
{
    for (int y = 0; y < CKH; y++) {
        for (int x = 0; x < CKW; x++) {
            uint8_t *p = &checker[(y * CKW + x) * 4];
            int light = ((x >> 2) ^ (y >> 2)) & 1;
            p[0] = light ? 245 : 40;
            p[1] = light ? 245 : 60;
            p[2] = light ? 255 : 120;
            p[3] = 255;
        }
    }
    tr_build_mips(&checker_tex, checker, CKW, CKH,
                  checker_mips, sizeof checker_mips);
}

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

/* Rotate each corner by the running angles and project in perspective: the cube
 * is pushed away from the camera by DIST and divided by its view-space depth,
 * which is stored as the vertex w so the rasterizer can texture it
 * perspective-correctly. Depth and a per-corner color come from its position. */
#define DIST  4.0f                      /* camera distance from the cube centre */
#define FOCAL ((float)TW * 0.9f)        /* focal length, pixels */
static void
project(struct tr_vertex out[8], float cx, float sx, float cy, float sy)
{
    float ox = (float)TW * 0.5f, oy = (float)TH * 0.5f;
    for (int i = 0; i < 8; i++) {
        float x = corner[i][0], y = corner[i][1], z = corner[i][2];
        /* rotate about Y, then about X */
        float x1 =  x * cy + z * sy;
        float z1 = -x * sy + z * cy;
        float y2 =  y * cx - z1 * sx;
        float z2 =  y * sx + z1 * cx;
        float zc = z2 + DIST;                   /* view-space depth, > 0 */
        out[i].w = zc;
        out[i].x = ox + x1 * FOCAL / zc;
        out[i].y = oy - y2 * FOCAL / zc;
        out[i].z = clampf((zc - 2.0f) * 0.25f, 0.0f, 1.0f);
        out[i].r = (corner[i][0] + 1.0f) * 0.5f;
        out[i].g = (corner[i][1] + 1.0f) * 0.5f;
        out[i].b = (corner[i][2] + 1.0f) * 0.5f;
        out[i].a = 1.0f;
    }
}

/* Draw one cube face as two triangles, assigning texture coordinates to its
 * four corners so the bound texture maps across the whole face. */
static void
draw_face(struct tr_target *t, const struct tr_vertex *v, const int *q)
{
    struct tr_vertex a = v[q[0]], b = v[q[1]], c = v[q[2]], d = v[q[3]];
    a.u = 0; a.v = 0;
    b.u = 1; b.v = 0;
    c.u = 1; c.v = 1;
    d.u = 0; d.v = 1;
    tr_triangle(t, &a, &b, &c);
    tr_triangle(t, &a, &c, &d);
}

static void
render_frame(struct tr_target *t, float cx, float sx, float cy, float sy)
{
    struct tr_vertex v[8];
    project(v, cx, sx, cy, sy);
    tr_clear(t, 0x10, 0x18, 0x30);

    /* The cube: the checker texture modulated by the gouraud color, so it is
     * both textured and lit. Bilinear and the mip chain keep the fine checker
     * smooth and alias-free as the faces foreshorten in perspective. */
    tr_tex_source(t, &checker_tex);
    tr_color_combine(t, TR_COMBINE_MODULATE);
    tr_tex_filter(t, TR_FILTER_BILINEAR);
    tr_tex_aniso(t, 8);
    tr_blend(t, 0);
    for (int f = 0; f < 6; f++)
        draw_face(t, v, face[f]);

    /* A translucent panel in front, to exercise the alpha-blend path: a flat
     * iterated color at half alpha, depth-tested against the cube but not
     * writing depth. It is screen-space (w = 1), so it needs no perspective. */
    tr_tex_source(t, NULL);
    tr_color_combine(t, TR_COMBINE_ITERATED);
    tr_blend(t, 1);
    struct tr_vertex p0 = { TW*0.46f, TH*0.30f, 0.05f, 1.0f, 1.0f, 0.4f, 0.1f, 0, 0, 0.5f };
    struct tr_vertex p1 = { TW*0.98f, TH*0.30f, 0.05f, 1.0f, 1.0f, 0.4f, 0.1f, 0, 0, 0.5f };
    struct tr_vertex p2 = { TW*0.98f, TH*0.95f, 0.05f, 1.0f, 1.0f, 0.4f, 0.1f, 0, 0, 0.5f };
    struct tr_vertex p3 = { TW*0.46f, TH*0.95f, 0.05f, 1.0f, 1.0f, 0.4f, 0.1f, 0, 0, 0.5f };
    tr_triangle(t, &p0, &p1, &p2);
    tr_triangle(t, &p0, &p2, &p3);
}

void
triton_demo(void)
{
    struct tr_target t = { tfb, tzb, TW, TH, TW * 4 };
    make_checker();

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
