/* png.c : a minimal, dependency-free PNG encoder (RGBA8888 -> base64 PNG).
 *
 * The image is written as a truecolor-with-alpha PNG. The IDAT payload is a
 * zlib stream whose DEFLATE body is a sequence of stored (uncompressed) blocks,
 * so there is no compressor here, only framing: zlib header, stored blocks over
 * the filtered rows, and an Adler-32 trailer. CRC-32 guards each chunk. This is
 * enough for the non-realtime pixel path; swapping the stored blocks for a real
 * DEFLATE encoder later would not change the interface. */
#include "png.h"

#include <stdlib.h>
#include <string.h>

/****************************************************************
 * A growable byte buffer
 ****************************************************************/

struct buf {
    uint8_t *p;
    size_t len, cap;
    int oom;
};

static void
buf_need(struct buf *b, size_t extra)
{
    if (b->oom)
        return;
    if (b->len + extra <= b->cap)
        return;
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->len + extra)
        cap *= 2;
    uint8_t *np = realloc(b->p, cap);
    if (!np) {
        b->oom = 1;
        return;
    }
    b->p = np;
    b->cap = cap;
}

static void
buf_u8(struct buf *b, uint8_t v)
{
    buf_need(b, 1);
    if (!b->oom)
        b->p[b->len++] = v;
}

static void
buf_bytes(struct buf *b, const void *data, size_t n)
{
    buf_need(b, n);
    if (!b->oom) {
        memcpy(b->p + b->len, data, n);
        b->len += n;
    }
}

static void
buf_be32(struct buf *b, uint32_t v)
{
    buf_u8(b, (uint8_t)(v >> 24));
    buf_u8(b, (uint8_t)(v >> 16));
    buf_u8(b, (uint8_t)(v >> 8));
    buf_u8(b, (uint8_t)v);
}

/****************************************************************
 * Checksums
 ****************************************************************/

static uint32_t
crc32_bytes(const uint8_t *data, size_t len, uint32_t crc)
{
    crc = ~crc;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & -(crc & 1));
    }
    return ~crc;
}

static uint32_t
adler32_bytes(const uint8_t *data, size_t len)
{
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < len; i++) {
        a = (a + data[i]) % 65521;
        b = (b + a) % 65521;
    }
    return b << 16 | a;
}

/* Append a PNG chunk: length, type, data, and a CRC over type+data. */
static void
chunk(struct buf *b, const char type[4], const uint8_t *data, size_t len)
{
    buf_be32(b, (uint32_t)len);
    uint32_t crc = crc32_bytes((const uint8_t *)type, 4, 0);
    crc = crc32_bytes(data, len, crc);
    buf_bytes(b, type, 4);
    buf_bytes(b, data, len);
    buf_be32(b, crc);
}

/****************************************************************
 * Base64
 ****************************************************************/

static const char b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static char *
base64(const uint8_t *in, size_t len)
{
    char *out = malloc((len + 2) / 3 * 4 + 1);
    if (!out)
        return NULL;
    size_t j = 0, i;
    for (i = 0; i + 2 < len; i += 3) {
        out[j++] = b64[in[i] >> 2];
        out[j++] = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
        out[j++] = b64[((in[i + 1] & 0xf) << 2) | (in[i + 2] >> 6)];
        out[j++] = b64[in[i + 2] & 0x3f];
    }
    if (i < len) {
        out[j++] = b64[in[i] >> 2];
        if (i + 1 < len) {
            out[j++] = b64[((in[i] & 3) << 4) | (in[i + 1] >> 4)];
            out[j++] = b64[(in[i + 1] & 0xf) << 2];
        } else {
            out[j++] = b64[(in[i] & 3) << 4];
            out[j++] = '=';
        }
        out[j++] = '=';
    }
    out[j] = '\0';
    return out;
}

/****************************************************************
 * Encode
 ****************************************************************/

/* Build the zlib stream (header, stored DEFLATE blocks, Adler-32) over the
 * filtered raw image data, appending it to b. */
static void
zlib_stored(struct buf *b, const uint8_t *raw, size_t rawlen)
{
    buf_u8(b, 0x78);            /* zlib CMF: deflate, 32K window */
    buf_u8(b, 0x01);            /* FLG: makes CMF/FLG a multiple of 31 */

    size_t off = 0;
    do {
        size_t n = rawlen - off;
        if (n > 65535)
            n = 65535;
        int final = (off + n == rawlen);
        buf_u8(b, (uint8_t)(final ? 1 : 0));    /* BFINAL + BTYPE 00 (stored) */
        buf_u8(b, (uint8_t)(n & 0xff));         /* LEN, little-endian */
        buf_u8(b, (uint8_t)(n >> 8));
        buf_u8(b, (uint8_t)(~n & 0xff));        /* NLEN = ~LEN */
        buf_u8(b, (uint8_t)(~n >> 8));
        buf_bytes(b, raw + off, n);
        off += n;
    } while (off < rawlen);

    buf_be32(b, adler32_bytes(raw, rawlen));
}

char *
png_base64_rgba(const uint8_t *rgba, int width, int height, int stride)
{
    if (width <= 0 || height <= 0)
        return NULL;
    if (stride == 0)
        stride = width * 4;
    if (stride < width * 4)
        return NULL;

    /* Filtered raw data: each row is a filter byte (0 = none) then its pixels. */
    size_t rowbytes = (size_t)width * 4;
    size_t rawlen = (size_t)height * (1 + rowbytes);
    uint8_t *raw = malloc(rawlen);
    if (!raw)
        return NULL;
    for (int y = 0; y < height; y++) {
        uint8_t *dst = raw + (size_t)y * (1 + rowbytes);
        *dst++ = 0;
        memcpy(dst, rgba + (size_t)y * stride, rowbytes);
    }

    struct buf b = { 0 };
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    buf_bytes(&b, sig, 8);

    uint8_t ihdr[13];
    ihdr[0] = (uint8_t)(width >> 24);  ihdr[1] = (uint8_t)(width >> 16);
    ihdr[2] = (uint8_t)(width >> 8);   ihdr[3] = (uint8_t)width;
    ihdr[4] = (uint8_t)(height >> 24); ihdr[5] = (uint8_t)(height >> 16);
    ihdr[6] = (uint8_t)(height >> 8);  ihdr[7] = (uint8_t)height;
    ihdr[8] = 8;                /* bit depth */
    ihdr[9] = 6;                /* color type: truecolor + alpha */
    ihdr[10] = 0;               /* compression: deflate */
    ihdr[11] = 0;               /* filter method */
    ihdr[12] = 0;               /* interlace: none */
    chunk(&b, "IHDR", ihdr, sizeof ihdr);

    struct buf idat = { 0 };
    zlib_stored(&idat, raw, rawlen);
    free(raw);
    if (idat.oom) {
        free(idat.p);
        free(b.p);
        return NULL;
    }
    chunk(&b, "IDAT", idat.p, idat.len);
    free(idat.p);

    chunk(&b, "IEND", NULL, 0);

    if (b.oom) {
        free(b.p);
        return NULL;
    }
    char *out = base64(b.p, b.len);
    free(b.p);
    return out;
}
