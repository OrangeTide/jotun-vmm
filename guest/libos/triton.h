/* triton.h : a minimal guest-side software 3D rasterizer.
 *
 * A small subset of a Glide-style immediate-mode API: gouraud-shaded,
 * z-buffered triangles rendered into an RGBA8888 framebuffer in guest RAM. The
 * rasterizer runs entirely inside the guest; the host only ever receives the
 * resulting pixels, through the draw pixel path. This is the raster side of the
 * security split in the design doc (section 9.2): no rendering command stream
 * and no rasterizer cross into the trusted host.
 *
 * This translation unit uses floating point, so it is built with the FPU
 * enabled, unlike the integer-only rest of the library OS. See arch_fpu_init.
 */
#ifndef LIBOS_TRITON_H
#define LIBOS_TRITON_H

#include <stdint.h>

/* A vertex in window space: x and y are pixel coordinates with the origin at
 * the top left, z is depth in 0..1 (smaller is nearer), r, g, b are color
 * components in 0..1, u, v are texture coordinates in 0..1, and a is alpha in
 * 0..1. All of these are interpolated across the triangle (gouraud for color,
 * affine for the texture coordinates, which is exact under the orthographic
 * projection this core uses). */
struct tr_vertex {
    float x, y, z;
    float r, g, b;
    float u, v;
    float a;
};

/* A source texture: RGBA8888 texels, w*h, row-major and tightly packed. Point
 * (nearest) sampled, coordinates clamped to the edge. */
struct tr_texture {
    const uint8_t *texels;
    int w, h;
};

/* How the texture color combines with the iterated (gouraud) color, named after
 * the Glide color-combine modes this core implements. Alpha combines the same
 * way: textured modes multiply the iterated and texel alpha, ITERATED keeps the
 * iterated alpha alone. */
enum tr_combine {
    TR_COMBINE_ITERATED = 0,    /* iterated color only; the texture is ignored */
    TR_COMBINE_TEXTURE,         /* texel color only (decal) */
    TR_COMBINE_MODULATE,        /* texel * iterated (the common lit-texture case) */
    TR_COMBINE_ADD,             /* texel + iterated, clamped */
};

/* The render target and its render state. The framebuffer is RGBA8888 with rows
 * `stride` bytes apart and the depth buffer is a matching w*h float array. The
 * state fields are a Glide-style state machine set between triangles: tex is the
 * bound texture (NULL to render untextured), combine selects the color-combine
 * mode, and blend turns on source-over alpha blending. Zero-initialising the
 * struct gives the minimal core's behaviour: no texture, iterated color, opaque. */
struct tr_target {
    uint8_t *fb;
    float   *zbuf;
    int      w, h;
    int      stride;
    const struct tr_texture *tex;
    int      combine;           /* enum tr_combine */
    int      blend;             /* 0 = opaque, 1 = source-over alpha blend */
};

/* Clear the color buffer to (r,g,b) and the depth buffer to the far plane. */
void tr_clear(struct tr_target *t, uint8_t r, uint8_t g, uint8_t b);

/* Set the render state, Glide style. A texture of NULL renders untextured. */
void tr_tex_source(struct tr_target *t, const struct tr_texture *tex);
void tr_color_combine(struct tr_target *t, int mode);
void tr_blend(struct tr_target *t, int enable);

/* Rasterize one triangle with a less-or-equal depth test, applying the target's
 * texture, combine mode, and blend state. An opaque triangle writes depth; a
 * blended one tests but does not write it, the usual translucency convention. */
void tr_triangle(struct tr_target *t, const struct tr_vertex *a,
                 const struct tr_vertex *b, const struct tr_vertex *c);

/* The demo: render a spinning, shaded cube and show it through the pixel path.
 * Called from the init task (the "triton" boot argument). */
void triton_demo(void);

#endif /* LIBOS_TRITON_H */
