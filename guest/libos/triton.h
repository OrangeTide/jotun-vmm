/* triton.h : a guest-side software 3D rasterizer.
 *
 * A subset of a Glide-style immediate-mode API: gouraud-shaded, z-buffered
 * triangles rendered into an RGBA8888 framebuffer in guest RAM, with
 * perspective-correct texturing, the Glide color-combine modes, point and
 * bilinear filtering, mip-mapped (trilinear) textures, and source-over alpha
 * blending. The rasterizer runs entirely inside the guest; the host only ever
 * receives the resulting pixels, through the draw pixel path. This is the raster
 * side of the security split in the design doc (section 9.2): no rendering
 * command stream and no rasterizer cross into the trusted host.
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
 * 0..1. w is the clip-space w the projection divided x and y by; the rasterizer
 * uses it to interpolate the texture coordinates perspective-correctly. Set w
 * to 1 for an orthographic vertex, where the result is the same as affine.
 * Color and alpha interpolate affinely (gouraud). */
struct tr_vertex {
    float x, y, z, w;
    float r, g, b;
    float u, v;
    float a;
};

/* Texture filtering. Point takes the nearest texel; bilinear blends the four
 * surrounding texels. With more than one mip level the two bracketing levels
 * are each sampled this way and blended by the level-of-detail fraction
 * (trilinear). */
enum tr_filter {
    TR_FILTER_POINT = 0,
    TR_FILTER_BILINEAR,
};

/* A source texture: a mip chain of RGBA8888 images, row-major and tightly
 * packed. level[0] is the base at (w, h); level i is level 0 halved i times,
 * floored at 1 in each axis. nlevels is 1 for a texture with no mips.
 * tr_build_mips fills the chain. Coordinates are clamped to the edge. */
#define TR_MAXMIP 12
struct tr_texture {
    const uint8_t *level[TR_MAXMIP];
    int w, h;                       /* base (level 0) dimensions */
    int nlevels;
};

/* Build a mip chain for a base image by repeated 2x2 box filtering, down to
 * 1x1. level[0] aliases `base` (not copied); levels 1.. are written into `buf`,
 * which must hold at least (w*h*4)/3 + w*h bytes (a safe upper bound is
 * 2*w*h*4). w and h should be powers of two for clean halving. Returns the
 * number of levels. */
int tr_build_mips(struct tr_texture *t, const uint8_t *base, int w, int h,
                  uint8_t *buf, int bufcap);

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
    int      filter;            /* enum tr_filter */
};

/* Clear the color buffer to (r,g,b) and the depth buffer to the far plane. */
void tr_clear(struct tr_target *t, uint8_t r, uint8_t g, uint8_t b);

/* Set the render state, Glide style. A texture of NULL renders untextured. */
void tr_tex_source(struct tr_target *t, const struct tr_texture *tex);
void tr_color_combine(struct tr_target *t, int mode);
void tr_blend(struct tr_target *t, int enable);
void tr_tex_filter(struct tr_target *t, int filter);

/* Rasterize one triangle with a less-or-equal depth test, applying the target's
 * texture, combine mode, and blend state. An opaque triangle writes depth; a
 * blended one tests but does not write it, the usual translucency convention. */
void tr_triangle(struct tr_target *t, const struct tr_vertex *a,
                 const struct tr_vertex *b, const struct tr_vertex *c);

/* The demo: render a spinning, shaded cube and show it through the pixel path.
 * Called from the init task (the "triton" boot argument). */
void triton_demo(void);

#endif /* LIBOS_TRITON_H */
