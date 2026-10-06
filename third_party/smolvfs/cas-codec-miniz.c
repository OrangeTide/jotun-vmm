/* cas-codec-miniz.c : bundled DEFLATE codec for CAS via miniz */
/* PUBLIC DOMAIN (CC0-1.0) */

/*
 * Optional. Compiled only when built with -DCAS_WITH_MINIZ, this
 * provides a zlib-wrapped DEFLATE codec that cas-codec.c places in
 * the codec table for the CAS_CODEC_DEFLATE ('Z') tag.  The zlib
 * wrapper makes the stream interoperable with a standard zlib, so
 * an application that would rather bring its own zlib can leave
 * this file out and supply compatible functions via CAS_CODEC_USER.
 *
 * Compression favours ratio over speed (objects are written once
 * and read many times), so the encoder runs at the maximum level.
 */

#include "cas-codec.h"
#include "cas.h"

#include "third_party/miniz.h"

#include <stddef.h>

int
cas_deflate_decode(const void *in, size_t inlen, void *out, size_t outlen)
{
    size_t n = tinfl_decompress_mem_to_mem(out, outlen, in, inlen,
                                           TINFL_FLAG_PARSE_ZLIB_HEADER);

    if (n == TINFL_DECOMPRESS_MEM_TO_MEM_FAILED || n != outlen)
        return CAS_ERR;
    return CAS_OK;
}

int
cas_deflate_encode(const void *in, size_t inlen, void *out, size_t *outlen)
{
    mz_uint flags = tdefl_create_comp_flags_from_zip_params(
            MZ_UBER_COMPRESSION, MZ_DEFAULT_WINDOW_BITS,
            MZ_DEFAULT_STRATEGY);
    size_t n = tdefl_compress_mem_to_mem(out, *outlen, in, inlen, flags);

    if (n == 0)
        return CAS_ERR;
    *outlen = n;
    return CAS_OK;
}
