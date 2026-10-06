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
 * DEFLATE (fixed Huffman, LZ77), enough to really compress the frame
 *
 * A single fixed-Huffman block over the filtered image data. Long runs of a
 * repeated pixel, which flat raster content is mostly made of, collapse into
 * length/distance matches. Bits pack least-significant-first into bytes;
 * Huffman codes pack most-significant-bit first, per RFC 1951.
 ****************************************************************/

/* A bit writer over the growable buffer. */
struct bw {
    struct buf *b;
    uint32_t acc;
    int n;
};

static void
bw_bits(struct bw *w, uint32_t v, int n)
{
    w->acc |= (v & ((n < 32 ? 1u << n : 0u) - 1)) << w->n;
    w->n += n;
    while (w->n >= 8) {
        buf_u8(w->b, (uint8_t)w->acc);
        w->acc >>= 8;
        w->n -= 8;
    }
}

/* Write a Huffman code most-significant-bit first. */
static void
bw_huff(struct bw *w, uint32_t code, int len)
{
    for (int i = len - 1; i >= 0; i--)
        bw_bits(w, (code >> i) & 1, 1);
}

static void
bw_flush(struct bw *w)
{
    if (w->n > 0) {
        buf_u8(w->b, (uint8_t)w->acc);
        w->acc = 0;
        w->n = 0;
    }
}

/* Emit a fixed-Huffman literal/length symbol (0..287). */
static void
emit_litlen(struct bw *w, int sym)
{
    if (sym <= 143)
        bw_huff(w, 0x30 + sym, 8);
    else if (sym <= 255)
        bw_huff(w, 0x190 + (sym - 144), 9);
    else if (sym <= 279)
        bw_huff(w, sym - 256, 7);
    else
        bw_huff(w, 0xc0 + (sym - 280), 8);
}

/* Length symbols 257..285: base length and extra-bit count. */
static const int len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258,
};
static const int len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0,
};
/* Distance symbols 0..29: base distance and extra-bit count. */
static const int dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577,
};
static const int dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13,
};

static void
emit_match(struct bw *w, int len, int dist)
{
    int ls = 28;
    while (ls > 0 && len < len_base[ls])
        ls--;
    emit_litlen(w, 257 + ls);
    bw_bits(w, (uint32_t)(len - len_base[ls]), len_extra[ls]);

    int ds = 29;
    while (ds > 0 && dist < dist_base[ds])
        ds--;
    bw_huff(w, (uint32_t)ds, 5);        /* fixed distance codes are 5 bits */
    bw_bits(w, (uint32_t)(dist - dist_base[ds]), dist_extra[ds]);
}

#define PNG_HASHBITS 15
#define PNG_HASHSIZE (1 << PNG_HASHBITS)
#define PNG_WINDOW   32768
#define PNG_MINMATCH 3
#define PNG_MAXMATCH 258
#define PNG_MAXCHAIN 128

static uint32_t
png_hash(const uint8_t *p)
{
    return ((p[0] << 10) ^ (p[1] << 5) ^ p[2]) & (PNG_HASHSIZE - 1);
}

/* Compress raw into a zlib stream (header, one fixed-Huffman block, Adler-32)
 * appended to b. head/prev are scratch of PNG_HASHSIZE and PNG_WINDOW ints. */
static void
zlib_deflate(struct buf *b, const uint8_t *raw, size_t rawlen,
             int *head, int *prev)
{
    buf_u8(b, 0x78);            /* zlib CMF: deflate, 32K window */
    buf_u8(b, 0x01);            /* FLG: makes CMF/FLG a multiple of 31 */

    for (int i = 0; i < PNG_HASHSIZE; i++)
        head[i] = -1;

    struct bw w = { .b = b };
    bw_bits(&w, 1, 1);          /* BFINAL = 1 */
    bw_bits(&w, 1, 2);          /* BTYPE = 01, fixed Huffman */

    size_t i = 0;
    while (i < rawlen) {
        int best_len = 0, best_dist = 0;
        if (i + PNG_MINMATCH <= rawlen) {
            uint32_t h = png_hash(raw + i);
            int cand = head[h];
            int chain = PNG_MAXCHAIN;
            int maxlen = (int)(rawlen - i);
            if (maxlen > PNG_MAXMATCH)
                maxlen = PNG_MAXMATCH;
            while (cand >= 0 && chain-- > 0) {
                int dist = (int)i - cand;
                if (dist > PNG_WINDOW)
                    break;
                int l = 0;
                while (l < maxlen && raw[cand + l] == raw[i + l])
                    l++;
                if (l > best_len) {
                    best_len = l;
                    best_dist = dist;
                    if (l >= maxlen)
                        break;
                }
                cand = prev[cand & (PNG_WINDOW - 1)];
            }
        }

        if (best_len >= PNG_MINMATCH) {
            emit_match(&w, best_len, best_dist);
            /* Insert the matched positions into the hash chains. */
            size_t end = i + best_len;
            while (i < end) {
                if (i + PNG_MINMATCH <= rawlen) {
                    uint32_t h = png_hash(raw + i);
                    prev[i & (PNG_WINDOW - 1)] = head[h];
                    head[h] = (int)i;
                }
                i++;
            }
        } else {
            emit_litlen(&w, raw[i]);
            if (i + PNG_MINMATCH <= rawlen) {
                uint32_t h = png_hash(raw + i);
                prev[i & (PNG_WINDOW - 1)] = head[h];
                head[h] = (int)i;
            }
            i++;
        }
    }

    emit_litlen(&w, 256);       /* end of block */
    bw_flush(&w);

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

    int *head = malloc(PNG_HASHSIZE * sizeof *head);
    int *prev = malloc(PNG_WINDOW * sizeof *prev);
    if (!head || !prev) {
        free(head);
        free(prev);
        free(raw);
        free(b.p);
        return NULL;
    }
    struct buf idat = { 0 };
    zlib_deflate(&idat, raw, rawlen, head, prev);
    free(head);
    free(prev);
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
