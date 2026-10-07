/* triton.h : a guest-side software 3D rasterizer.
 *
 * A subset of a Glide-style immediate-mode API: gouraud-shaded, z-buffered
 * triangles rendered into an RGBA8888 framebuffer in guest RAM, with
 * perspective-correct texturing, the Glide color-combine modes, point and
 * bilinear filtering, mip-mapped (trilinear) textures, anisotropic filtering,
 * and source-over alpha blending. In front of the rasterizer sits an optional
 * vertex transform and clip stage: it multiplies object-space vertices by a 4x4
 * matrix, clips the triangle against the near and far planes, and maps the
 * result to the viewport, so geometry may cross behind the camera safely. The
 * rasterizer runs entirely inside the guest; the host only ever
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
    int      maxaniso;          /* max anisotropic taps; 0 or 1 = isotropic */
};

/* Clear the color buffer to (r,g,b) and the depth buffer to the far plane. */
void tr_clear(struct tr_target *t, uint8_t r, uint8_t g, uint8_t b);

/* Set the render state, Glide style. A texture of NULL renders untextured. */
void tr_tex_source(struct tr_target *t, const struct tr_texture *tex);
void tr_color_combine(struct tr_target *t, int mode);
void tr_blend(struct tr_target *t, int enable);
void tr_tex_filter(struct tr_target *t, int filter);

/* Set the maximum number of anisotropic texture taps (1 = isotropic). With more
 * than one, a mipped texture viewed at a grazing angle is sampled several times
 * along the long axis of its footprint, each tap at a mip level matched to the
 * short axis, which keeps it sharp where isotropic sampling would blur it.
 * Needs a mip chain; without one it has no effect. */
void tr_tex_aniso(struct tr_target *t, int maxaniso);

/* Rasterize one triangle with a less-or-equal depth test, applying the target's
 * texture, combine mode, and blend state. An opaque triangle writes depth; a
 * blended one tests but does not write it, the usual translucency convention.
 * The vertices are in window space; the transform stage below feeds this. */
void tr_triangle(struct tr_target *t, const struct tr_vertex *a,
                 const struct tr_vertex *b, const struct tr_vertex *c);

/* A 4x4 matrix in row-major order: element (row, col) is m[row*4 + col], and it
 * multiplies a column vector [x y z 1] on the right. The builders below compose
 * with tr_mat4_mul, whose result applies b first and then a (o = a * b). */
struct tr_mat4 {
    float m[16];
};

void tr_mat4_identity(struct tr_mat4 *o);
void tr_mat4_mul(struct tr_mat4 *o, const struct tr_mat4 *a,
                 const struct tr_mat4 *b);
void tr_mat4_translate(struct tr_mat4 *o, float x, float y, float z);

/* Rotations take the cosine and sine of the angle directly, so the guest needs
 * no sin/cos routine; a spinning caller can advance them by the angle-sum
 * identities. rotx, roty and rotz rotate about the x, y and z axes. */
void tr_mat4_rotx(struct tr_mat4 *o, float c, float s);
void tr_mat4_roty(struct tr_mat4 *o, float c, float s);
void tr_mat4_rotz(struct tr_mat4 *o, float c, float s);

/* A perspective projection for a camera at the origin looking down +z. znear
 * and zfar are the clip-plane depths (0 < znear < zfar); sx and sy scale x and
 * y into normalized device coordinates (cot(fov/2) per axis, so a larger value
 * is a narrower field of view). It maps znear to window z 0 and zfar to 1, and
 * puts the view-space depth in the clip w for perspective-correct texturing. */
void tr_mat4_perspective(struct tr_mat4 *o, float znear, float zfar,
                         float sx, float sy);

/* A vertex entering the transform stage, in object (or world) space: position
 * x, y, z, gouraud color r, g, b with alpha a, and texture coordinates u, v. */
struct tr_vin {
    float x, y, z;
    float r, g, b, a;
    float u, v;
};

/* Transform three vertices by mvp into clip space, clip the triangle against
 * the near and far planes, divide by w, map to the viewport (the whole target),
 * and rasterize the result with the target's current texture, combine and blend
 * state. Clipping against the near plane is what lets geometry cross behind the
 * camera safely. One input triangle can emit up to three after clipping. */
void tr_draw_triangle(struct tr_target *t, const struct tr_mat4 *mvp,
                      const struct tr_vin *a, const struct tr_vin *b,
                      const struct tr_vin *c);

/* The demo: render a spinning, shaded cube and show it through the pixel path.
 * Called from the init task (the "triton" boot argument). */
void triton_demo(void);

#endif /* LIBOS_TRITON_H */
