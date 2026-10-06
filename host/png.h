/* png.h : a minimal, dependency-free PNG encoder for the pixel path.
 *
 * Encodes an RGBA8888 framebuffer to a PNG and returns it base64-encoded, ready
 * to drop into an SVG <image> data URI. The DEFLATE stream uses stored
 * (uncompressed) blocks, which keeps the encoder small and is adequate for the
 * non-realtime pixel path; real compression can replace the stored blocks later
 * without changing this interface. */
#ifndef PNG_H
#define PNG_H

#include <stddef.h>
#include <stdint.h>

/* Encode a width x height RGBA8888 image (row stride in bytes; 0 means
 * width*4) as a PNG, base64-encoded. Returns a malloc'd NUL-terminated string
 * the caller frees, or NULL on allocation failure or invalid geometry. */
char *png_base64_rgba(const uint8_t *rgba, int width, int height, int stride);

#endif /* PNG_H */
