/**
 * @file gzip.c
 * @brief Gzip response compression middleware implementation.
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "csilk.h"
#include "csilk_internal.h"

#define CSILK_GZIP_CHUNK 16384
#define CSILK_GZIP_MIN_LENGTH 1024

typedef struct {
    uint8_t* dest;
    size_t dest_cap;
    int ret;
    size_t compressed_len;
} gzip_async_state_t;

static void gzip_work_cb(uv_work_t* req) {
    csilk_ctx_t* c = (csilk_ctx_t*)req->data;
    gzip_async_state_t* state = (gzip_async_state_t*)csilk_get(c, "gzip_state");
    if (!state) return;

    size_t src_len = c->response.body_len;
    z_stream strm;
    memset(&strm, 0, sizeof(strm));
    
    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        state->ret = Z_ERRNO;
        return;
    }

    state->dest_cap = deflateBound(&strm, (uLong)src_len);
    state->dest = malloc(state->dest_cap);
    if (!state->dest) {
        deflateEnd(&strm);
        state->ret = Z_MEM_ERROR;
        return;
    }

    strm.next_in = (Bytef*)c->response.body;
    strm.avail_in = (uInt)src_len;
    strm.next_out = state->dest;
    strm.avail_out = (uInt)state->dest_cap;

    state->ret = deflate(&strm, Z_FINISH);
    state->compressed_len = state->dest_cap - strm.avail_out;
    deflateEnd(&strm);
}

static void gzip_after_work_cb(uv_work_t* req, int status) {
    csilk_ctx_t* c = (csilk_ctx_t*)req->data;
    gzip_async_state_t* state = (gzip_async_state_t*)csilk_get(c, "gzip_state");
    
    if (state && state->ret == Z_STREAM_END) {
        if (c->response.body && c->response.body_is_managed) {
            free((void*)c->response.body);
        }
        c->response.body = (const char*)state->dest;
        c->response.body_len = state->compressed_len;
        c->response.body_is_managed = 1;

        csilk_set_header(c, "Content-Encoding", "gzip");
        csilk_set_header(c, "Vary", "Accept-Encoding");
    } else if (state) {
        if (state->dest) free(state->dest);
    }
    
    if (state) free(state);
    
    _csilk_send_response(c);
}

void csilk_gzip_middleware(csilk_ctx_t* c) {
    if (!c) return;

    csilk_next(c);

    if (!c->response.body || c->response.body_len == 0) return;

    const char* accept_encoding = csilk_get_header(c, "Accept-Encoding");
    if (!accept_encoding || !strstr(accept_encoding, "gzip")) return;

    if (c->response.body_len < CSILK_GZIP_MIN_LENGTH) return;

    if (c->response.body_is_managed == 0) {
        char* managed = malloc(c->response.body_len);
        if (!managed) return;
        memcpy(managed, c->response.body, c->response.body_len);
        c->response.body = managed;
        c->response.body_is_managed = 1;
    }

    gzip_async_state_t* state = calloc(1, sizeof(gzip_async_state_t));
    if (!state) return;

    csilk_set(c, "gzip_state", state);
    c->work_req.data = c;
    c->is_async = 1;

    uv_loop_t* loop = uv_default_loop();
    uv_queue_work(loop, &c->work_req, gzip_work_cb, gzip_after_work_cb);
}
