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
 * the top left, z is depth in 0..1 (smaller is nearer), and r, g, b are color
 * components in 0..1. Color is interpolated across the triangle (gouraud). */
struct tr_vertex {
    float x, y, z;
    float r, g, b;
};

/* The render target: an RGBA8888 framebuffer and a matching float depth buffer,
 * both w*h, with the framebuffer rows `stride` bytes apart. */
struct tr_target {
    uint8_t *fb;
    float   *zbuf;
    int      w, h;
    int      stride;
};

/* Clear the color buffer to (r,g,b) and the depth buffer to the far plane. */
void tr_clear(struct tr_target *t, uint8_t r, uint8_t g, uint8_t b);

/* Rasterize one gouraud-shaded triangle with a less-or-equal depth test. */
void tr_triangle(struct tr_target *t, const struct tr_vertex *a,
                 const struct tr_vertex *b, const struct tr_vertex *c);

/* The demo: render a spinning, shaded cube and show it through the pixel path.
 * Called from the init task (the "triton" boot argument). */
void triton_demo(void);

#endif /* LIBOS_TRITON_H */
