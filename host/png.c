/* png.c : a minimal, dependency-free PNG encoder (RGBA8888 -> base64 PNG).
 *
 * The image is written as a truecolor-with-alpha PNG. The IDAT payload is a
 * zlib stream: a single fixed-Huffman DEFLATE block with hash-chain LZ77 over
 * the image rows, wrapped with a zlib header and an Adler-32 trailer, and each
 * chunk guarded by a CRC-32. Each row is first run through an adaptively chosen
 * PNG filter (None, Sub, Up, Average or Paeth), which turns smooth or repeated
 * content into small deltas the LZ77 and Huffman stages then compress well. */
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
 * DEFLATE (dynamic Huffman, LZ77)
 *
 * A single dynamic-Huffman block over the filtered image data. LZ77 (hash
 * chains) first turns repeated content into length/distance matches; the
 * literal/length and distance symbols are then counted and given per-frame
 * Huffman codes, length-limited to 15 bits, which beats the fixed codes by
 * fitting the actual frequencies. Bits pack least-significant-first into bytes;
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

/* Canonical length-limited Huffman over an alphabet of n symbols with the given
 * frequencies. huff_lengths fills len[] with each symbol's code length (0 for
 * unused), capped at maxbits; huff_codes then assigns the canonical codes. */
#define HUFF_MAXSYM 288

static void
huff_lengths(const uint32_t *freq, int n, int maxbits, uint8_t *len)
{
    for (int i = 0; i < n; i++)
        len[i] = 0;

    /* Build the Huffman tree: leaves for used symbols, then repeated merges of
     * the two lowest-weight active nodes. Small alphabets, so linear scans. */
    uint32_t w[2 * HUFF_MAXSYM];
    int lc[2 * HUFF_MAXSYM], rc[2 * HUFF_MAXSYM], sym[2 * HUFF_MAXSYM];
    int act[2 * HUFF_MAXSYM];
    int nn = 0, na = 0;
    for (int i = 0; i < n; i++) {
        if (freq[i]) {
            w[nn] = freq[i]; lc[nn] = -1; rc[nn] = -1; sym[nn] = i;
            act[na++] = nn; nn++;
        }
    }
    if (na == 0)
        return;
    if (na == 1) {
        len[sym[act[0]]] = 1;           /* a lone symbol still needs one bit */
        return;
    }
    while (na > 1) {
        int i1 = 0;
        for (int k = 1; k < na; k++)
            if (w[act[k]] < w[act[i1]]) i1 = k;
        int a = act[i1]; act[i1] = act[--na];
        int i2 = 0;
        for (int k = 1; k < na; k++)
            if (w[act[k]] < w[act[i2]]) i2 = k;
        int b = act[i2]; act[i2] = act[--na];
        int p = nn++;
        w[p] = w[a] + w[b]; lc[p] = a; rc[p] = b; sym[p] = -1;
        act[na++] = p;
    }

    /* Leaf depths are the unlimited code lengths; tally them per length. */
    int bl_count[2 * HUFF_MAXSYM];
    for (int i = 0; i < 2 * HUFF_MAXSYM; i++) bl_count[i] = 0;
    int stk[2 * HUFF_MAXSYM], dep[2 * HUFF_MAXSYM], sp = 0, maxdepth = 0;
    stk[sp] = act[0]; dep[sp] = 0; sp++;
    while (sp > 0) {
        sp--;
        int node = stk[sp], d = dep[sp];
        if (lc[node] < 0) {
            if (d < 1) d = 1;
            if (d > maxdepth) maxdepth = d;
            bl_count[d]++;
        } else {
            stk[sp] = lc[node]; dep[sp] = d + 1; sp++;
            stk[sp] = rc[node]; dep[sp] = d + 1; sp++;
        }
    }

    /* Cap lengths at maxbits by redistributing the overflow (zlib's method),
     * which keeps the length multiset a complete prefix code. */
    int overflow = 0;
    for (int d = maxbits + 1; d <= maxdepth; d++) {
        bl_count[maxbits] += bl_count[d];
        overflow += bl_count[d];
        bl_count[d] = 0;
    }
    while (overflow > 0) {
        int b = maxbits - 1;
        while (b > 0 && bl_count[b] == 0) b--;
        if (b == 0) break;
        bl_count[b]--;
        bl_count[b + 1] += 2;
        bl_count[maxbits]--;
        overflow -= 2;
    }

    /* Hand the shortest lengths to the most frequent symbols. */
    int us[HUFF_MAXSYM], m = 0;
    for (int i = 0; i < n; i++)
        if (freq[i]) us[m++] = i;
    for (int i = 1; i < m; i++) {
        int s = us[i], j = i - 1;
        while (j >= 0 && freq[us[j]] < freq[s]) { us[j + 1] = us[j]; j--; }
        us[j + 1] = s;
    }
    int idx = 0;
    for (int l = 1; l <= maxbits; l++)
        for (int k = 0; k < bl_count[l] && idx < m; k++)
            len[us[idx++]] = (uint8_t)l;
}

static void
huff_codes(const uint8_t *len, int n, int maxbits, uint16_t *code)
{
    int bl_count[16];
    for (int i = 0; i <= maxbits; i++) bl_count[i] = 0;
    for (int i = 0; i < n; i++)
        if (len[i]) bl_count[len[i]]++;
    uint32_t next[16], c = 0;
    next[0] = 0;
    for (int b = 1; b <= maxbits; b++) {
        c = (c + (uint32_t)bl_count[b - 1]) << 1;
        next[b] = c;
    }
    for (int i = 0; i < n; i++)
        if (len[i]) code[i] = (uint16_t)next[len[i]]++;
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

/* The length symbol (0..28, mapping to litlen 257..285) for a match length,
 * and the distance symbol (0..29) for a match distance. */
static int
len_sym(int len)
{
    int ls = 28;
    while (ls > 0 && len < len_base[ls])
        ls--;
    return ls;
}

static int
dist_sym(int dist)
{
    int ds = 29;
    while (ds > 0 && dist < dist_base[ds])
        ds--;
    return ds;
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

/* One LZ77 token: len == 0 is a literal byte in `dist`; len > 0 is a match of
 * that length at that distance. */
struct tok {
    int len;
    int dist;
};

/* The 19 code-length-code symbols in the order their lengths are written. */
static const int cl_order[19] = {
    16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15,
};

/* Compress raw into a zlib stream (header, one dynamic-Huffman block, Adler-32)
 * appended to b. head/prev are scratch of PNG_HASHSIZE and PNG_WINDOW ints. The
 * first pass runs LZ77 into a token list and counts symbol frequencies; the
 * second builds the Huffman tables, writes the block header, and emits the
 * tokens. */
static void
zlib_deflate(struct buf *b, const uint8_t *raw, size_t rawlen,
             int *head, int *prev)
{
    buf_u8(b, 0x78);            /* zlib CMF: deflate, 32K window */
    buf_u8(b, 0x01);            /* FLG: makes CMF/FLG a multiple of 31 */

    for (int i = 0; i < PNG_HASHSIZE; i++)
        head[i] = -1;

    struct tok *toks = malloc((rawlen ? rawlen : 1) * sizeof *toks);
    if (!toks) {
        b->oom = 1;
        return;
    }
    size_t nt = 0;
    uint32_t fll[288], fd[30];
    for (int i = 0; i < 288; i++) fll[i] = 0;
    for (int i = 0; i < 30; i++) fd[i] = 0;

    /* Pass 1: LZ77 parse, collecting tokens and symbol frequencies. */
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
            toks[nt].len = best_len;
            toks[nt].dist = best_dist;
            nt++;
            fll[257 + len_sym(best_len)]++;
            fd[dist_sym(best_dist)]++;
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
            toks[nt].len = 0;
            toks[nt].dist = raw[i];
            nt++;
            fll[raw[i]]++;
            if (i + PNG_MINMATCH <= rawlen) {
                uint32_t h = png_hash(raw + i);
                prev[i & (PNG_WINDOW - 1)] = head[h];
                head[h] = (int)i;
            }
            i++;
        }
    }
    fll[256]++;                 /* end-of-block symbol */

    /* DEFLATE wants at least one distance code even when nothing matched. */
    int any_dist = 0;
    for (int k = 0; k < 30; k++)
        if (fd[k]) { any_dist = 1; break; }
    if (!any_dist)
        fd[0] = 1;

    uint8_t ll_len[288], d_len[30], cl_len[19];
    uint16_t ll_code[288], d_code[30], cl_code[19];
    huff_lengths(fll, 286, 15, ll_len);
    huff_codes(ll_len, 286, 15, ll_code);
    huff_lengths(fd, 30, 15, d_len);
    huff_codes(d_len, 30, 15, d_code);

    int hlit = 286;
    while (hlit > 257 && ll_len[hlit - 1] == 0) hlit--;
    int hdist = 30;
    while (hdist > 1 && d_len[hdist - 1] == 0) hdist--;

    /* Run-length-encode the concatenated code lengths into the code-length
     * alphabet (0..15 literal, 16 repeat-last, 17/18 repeat-zero). */
    uint8_t cl[286 + 30];
    int cln = 0;
    for (int k = 0; k < hlit; k++) cl[cln++] = ll_len[k];
    for (int k = 0; k < hdist; k++) cl[cln++] = d_len[k];

    int clsym[2 * (286 + 30)], clext[2 * (286 + 30)], clnb[2 * (286 + 30)];
    int cls = 0;
    uint32_t fcl[19];
    for (int k = 0; k < 19; k++) fcl[k] = 0;
    for (int p = 0; p < cln; ) {
        int cur = cl[p], run = 1;
        while (p + run < cln && cl[p + run] == cur) run++;
        if (cur == 0) {
            while (run >= 11) {
                int r = run > 138 ? 138 : run;
                clsym[cls] = 18; clext[cls] = r - 11; clnb[cls] = 7; cls++;
                fcl[18]++; run -= r; p += r;
            }
            while (run >= 3) {
                int r = run > 10 ? 10 : run;
                clsym[cls] = 17; clext[cls] = r - 3; clnb[cls] = 3; cls++;
                fcl[17]++; run -= r; p += r;
            }
            while (run-- > 0) {
                clsym[cls] = 0; clext[cls] = 0; clnb[cls] = 0; cls++;
                fcl[0]++; p++;
            }
        } else {
            clsym[cls] = cur; clext[cls] = 0; clnb[cls] = 0; cls++;
            fcl[cur]++; p++; run--;
            while (run >= 3) {
                int r = run > 6 ? 6 : run;
                clsym[cls] = 16; clext[cls] = r - 3; clnb[cls] = 2; cls++;
                fcl[16]++; run -= r; p += r;
            }
            while (run-- > 0) {
                clsym[cls] = cur; clext[cls] = 0; clnb[cls] = 0; cls++;
                fcl[cur]++; p++;
            }
        }
    }

    /* The code-length code itself must be a complete prefix code, so make sure
     * at least two of its symbols are present. */
    int cl_used = 0;
    for (int k = 0; k < 19; k++)
        if (fcl[k]) cl_used++;
    if (cl_used < 2) {
        for (int k = 0; k < 19 && cl_used < 2; k++)
            if (!fcl[k]) { fcl[k] = 1; cl_used++; }
    }
    huff_lengths(fcl, 19, 7, cl_len);
    huff_codes(cl_len, 19, 7, cl_code);

    int hclen = 19;
    while (hclen > 4 && cl_len[cl_order[hclen - 1]] == 0) hclen--;

    /* Pass 2: write the dynamic block header and the token stream. */
    struct bw w = { .b = b };
    bw_bits(&w, 1, 1);          /* BFINAL = 1 */
    bw_bits(&w, 2, 2);          /* BTYPE = 10, dynamic Huffman */
    bw_bits(&w, (uint32_t)(hlit - 257), 5);
    bw_bits(&w, (uint32_t)(hdist - 1), 5);
    bw_bits(&w, (uint32_t)(hclen - 4), 4);
    for (int k = 0; k < hclen; k++)
        bw_bits(&w, cl_len[cl_order[k]], 3);
    for (int k = 0; k < cls; k++) {
        bw_huff(&w, cl_code[clsym[k]], cl_len[clsym[k]]);
        if (clnb[k])
            bw_bits(&w, (uint32_t)clext[k], clnb[k]);
    }
    for (size_t k = 0; k < nt; k++) {
        if (toks[k].len == 0) {
            int lit = toks[k].dist;
            bw_huff(&w, ll_code[lit], ll_len[lit]);
        } else {
            int ls = len_sym(toks[k].len);
            bw_huff(&w, ll_code[257 + ls], ll_len[257 + ls]);
            bw_bits(&w, (uint32_t)(toks[k].len - len_base[ls]), len_extra[ls]);
            int ds = dist_sym(toks[k].dist);
            bw_huff(&w, d_code[ds], d_len[ds]);
            bw_bits(&w, (uint32_t)(toks[k].dist - dist_base[ds]), dist_extra[ds]);
        }
    }
    bw_huff(&w, ll_code[256], ll_len[256]);     /* end of block */
    bw_flush(&w);

    free(toks);
    buf_be32(b, adler32_bytes(raw, rawlen));
}

/****************************************************************
 * Adaptive PNG row filtering
 ****************************************************************/

#define PNG_BPP 4               /* RGBA, so the left neighbour is 4 bytes back */

/* The PNG Paeth predictor over the left (a), above (b) and above-left (c)
 * bytes: it returns whichever neighbour the gradient a+b-c is nearest to. */
static int
paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc)
        return a;
    return pb <= pc ? b : c;
}

/* The filtered byte for one of the five filter types, from the current byte x
 * and its neighbours a (left), b (above) and c (above-left). The neighbours are
 * the original bytes: the decoder reconstructs the same values in order. */
static uint8_t
filt_byte(int type, int x, int a, int b, int c)
{
    switch (type) {
    case 1: return (uint8_t)(x - a);
    case 2: return (uint8_t)(x - b);
    case 3: return (uint8_t)(x - ((a + b) >> 1));
    case 4: return (uint8_t)(x - paeth(a, b, c));
    default: return (uint8_t)x;
    }
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

    /* Filtered raw data: each row is a filter byte then its filtered pixels.
     * For each row we try all five filters into scratch and keep the one whose
     * bytes have the smallest total signed magnitude, the standard heuristic. */
    size_t rowbytes = (size_t)width * 4;
    size_t rawlen = (size_t)height * (1 + rowbytes);
    uint8_t *raw = malloc(rawlen);
    uint8_t *scratch = malloc(5 * rowbytes);
    if (!raw || !scratch) {
        free(raw);
        free(scratch);
        return NULL;
    }
    for (int y = 0; y < height; y++) {
        const uint8_t *cur = rgba + (size_t)y * stride;
        const uint8_t *up = y > 0 ? rgba + (size_t)(y - 1) * stride : NULL;
        int best = 0;
        long best_score = -1;
        for (int type = 0; type < 5; type++) {
            uint8_t *fb = scratch + (size_t)type * rowbytes;
            long score = 0;
            for (size_t x = 0; x < rowbytes; x++) {
                int cx = cur[x];
                int a = x >= PNG_BPP ? cur[x - PNG_BPP] : 0;
                int b = up ? up[x] : 0;
                int c = (up && x >= PNG_BPP) ? up[x - PNG_BPP] : 0;
                uint8_t f = filt_byte(type, cx, a, b, c);
                fb[x] = f;
                int sv = (int8_t)f;
                score += sv < 0 ? -sv : sv;
            }
            if (best_score < 0 || score < best_score) {
                best_score = score;
                best = type;
            }
        }
        uint8_t *dst = raw + (size_t)y * (1 + rowbytes);
        *dst++ = (uint8_t)best;
        memcpy(dst, scratch + (size_t)best * rowbytes, rowbytes);
    }
    free(scratch);

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
