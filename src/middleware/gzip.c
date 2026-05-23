/**
 * @file gzip.c
 * @brief Gzip compression middleware implementation.
 * MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "csilk.h"

#define CSILK_GZIP_CHUNK 16384
#define CSILK_GZIP_MIN_LENGTH 1024

void csilk_gzip_middleware(csilk_ctx_t* c) {
    if (!c) return;

    csilk_next(c);

    if (!c->response.body || c->response.body_len == 0) return;

    const char* accept_encoding = csilk_get_header(c, "Accept-Encoding");
    if (!accept_encoding || !strstr(accept_encoding, "gzip")) return;

    if (c->response.body_len < CSILK_GZIP_MIN_LENGTH) return;

    if (c->response.body_is_managed == 0) {
        char* managed = strdup(c->response.body);
        if (!managed) return;
        c->response.body = managed;
        c->response.body_is_managed = 1;
    }

    size_t src_len = c->response.body_len;

    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return;
    }

    size_t dest_cap = deflateBound(&strm, (uLong)src_len);
    uint8_t* dest = malloc(dest_cap);
    if (!dest) {
        deflateEnd(&strm);
        return;
    }

    strm.next_in = (Bytef*)c->response.body;
    strm.avail_in = (uInt)src_len;
    strm.next_out = dest;
    strm.avail_out = (uInt)dest_cap;

    int ret = deflate(&strm, Z_FINISH);
    deflateEnd(&strm);

    if (ret != Z_STREAM_END) {
        free(dest);
        return;
    }

    size_t compressed_len = dest_cap - strm.avail_out;
    free((void*)c->response.body);
    c->response.body = (const char*)dest;
    c->response.body_len = compressed_len;
    c->response.body_is_managed = 1;

    csilk_set_header(c, "Content-Encoding", "gzip");
    csilk_set_header(c, "Vary", "Accept-Encoding");
}
