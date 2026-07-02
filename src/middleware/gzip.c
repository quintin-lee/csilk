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

#include "csilk/csilk.h"
#include "csilk/core/internal.h"
#include "core/ctx_internal.h"

/* See include/csilk/core/internal.h for CSILK_GZIP_CHUNK and CSILK_GZIP_MIN_LENGTH. */

/**
 * @brief State for asynchronous gzip compression offloading.
 *
 * Carries the compressed output buffer, capacity, actual compressed length,
 * and the zlib return status between the work callback (running on the
 * thread pool) and the after-work callback (running on the event loop).
 */
typedef struct {
    uint8_t* dest;           /**< Compressed output buffer. */
    size_t   dest_cap;       /**< Capacity of the output buffer. */
    int      ret;            /**< zlib return status. */
    size_t   compressed_len; /**< Actual compressed data length. */
} gzip_async_state_t;

/**
 * @brief Work callback: perform gzip compression off the event loop.
 *
 * Initializes a zlib deflate stream with gzip wrapper (windowBits = 15 + 16),
 * compresses the response body into a dynamically allocated buffer, then
 * stores the result in the gzip_async_state_t attached to the context.
 *
 * @param req  I/O work request. req->data points to the csilk_ctx_t.
 *
 * @note Runs on a thread-pool thread. Must not touch non-thread-safe
 *       resources.
 */
static void
gzip_work_cb(csilk_io_work_t* req)
{
    csilk_ctx_t*        c = (csilk_ctx_t*)req->data;
    gzip_async_state_t* state = (gzip_async_state_t*)csilk_get(c, "gzip_state");
    if (!state) {
        CSILK_LOG_E("Gzip: async state missing in work callback");
        return;
    }

    size_t      src_len = 0;
    const char* src_body = csilk_get_response_body(c, &src_len);
    z_stream    strm;
    memset(&strm, 0, sizeof(strm));

    CSILK_LOG_T("Gzip: starting deflation of %zu bytes in thread-pool", src_len);

    if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) !=
        Z_OK) {
        CSILK_LOG_E("Gzip: deflateInit2 failed");
        state->ret = Z_ERRNO;
        return;
    }

    state->dest_cap = deflateBound(&strm, (uLong)src_len);
    state->dest = malloc(state->dest_cap);
    if (!state->dest) {
        CSILK_LOG_E("Gzip: failed to allocate compressed destination buffer of %zu bytes",
                    state->dest_cap);
        deflateEnd(&strm);
        state->ret = Z_MEM_ERROR;
        return;
    }

    strm.next_in = (Bytef*)src_body;
    strm.avail_in = (uInt)src_len;
    strm.next_out = state->dest;
    strm.avail_out = (uInt)state->dest_cap;

    state->ret = deflate(&strm, Z_FINISH);
    state->compressed_len = state->dest_cap - strm.avail_out;
    deflateEnd(&strm);

    if (state->ret == Z_STREAM_END) {
        CSILK_LOG_D("Gzip: deflation complete, ratio: %.2f%% (%zu -> %zu bytes)",
                    (double)state->compressed_len / (src_len ? src_len : 1) * 100.0,
                    src_len,
                    state->compressed_len);
    } else {
        CSILK_LOG_E("Gzip: deflation failed with zlib code %d", state->ret);
    }
}

/**
 * @brief After-work callback: apply compressed body and send response.
 *
 * Runs on the event loop after compression finishes. If compression succeeded
 * (Z_STREAM_END), it replaces the original response body with the compressed
 * data, sets Content-Encoding: gzip and Vary: Accept-Encoding headers, then
 * sends the response. On failure the original uncompressed body is sent
 * instead.
 *
 * @param req     I/O work request. req->data points to the csilk_ctx_t.
 * @param status  Work status — 0 on success, negative on cancellation.
 *
 * @note The original (uncompressed) response body is freed only if it was
 *       heap-managed (body_is_managed == 1).
 */
static void
gzip_after_work_cb(csilk_io_work_t* req, int status)
{
    (void)status;
    csilk_ctx_t*        c = (csilk_ctx_t*)req->data;
    gzip_async_state_t* state = (gzip_async_state_t*)csilk_get(c, "gzip_state");

    if (state && state->ret == Z_STREAM_END) {
        CSILK_LOG_D("Gzip: applying compressed response body for request %p (len: %zu)",
                    (void*)c,
                    state->compressed_len);
        csilk_set_response_body(c, (const char*)state->dest, state->compressed_len, 1);
        state->dest = nullptr; // Ownership transferred to context

        csilk_set_header(c, "Content-Encoding", "gzip");
        csilk_set_header(c, "Vary", "Accept-Encoding");
    } else {
        CSILK_LOG_W("Gzip: compression failed or state missing (ret: %d), sending original "
                    "response",
                    state ? state->ret : -1);
        if (state && state->dest) {
            free(state->dest);
        }
    }

    if (state) {
        free(state);
    }

    _csilk_send_response(c);
    _csilk_ctx_async_ref_decr(c);
}

/**
 * @brief Gzip response compression middleware (offloaded to thread pool).
 *
 * Calls csilk_next() first, then inspects the response. Compression is
 * skipped if:
 *   - The response body is empty,
 *   - Content-Encoding is already set,
 *   - The Content-Type is binary / incompressible (image, video, audio,
 *     pdf, zip, gzip),
 *   - The client does not advertise gzip in Accept-Encoding,
 *   - The response body is smaller than CSILK_GZIP_MIN_LENGTH (1 KB).
 *
 * When eligible, the response body is made managed (copied if necessary) and
 * compression is offloaded to the thread pool via a work request.
 *
 * @param c  The request context.
 *
 * @note This middleware must be registered AFTER handlers that produce the
 *       response body (since it calls csilk_next() first).
 * @warning The response body is replaced in-place; subsequent middleware or
 *          cleanup code must not free the original pointer after compression.
 */
void
csilk_gzip_middleware(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    CSILK_LOG_T("Gzip: middleware invoked for request %p", (void*)c);

    /* Call csilk_next() first so downstream handlers produce the response body.
     This middleware runs AFTER the route handler, not before. */
    csilk_next(c);

    size_t      body_len = 0;
    const char* body = csilk_get_response_body(c, &body_len);
    if (!body || body_len == 0) {
        CSILK_LOG_D("Gzip: skipping response compression - empty body");
        return;
    }

    /* Skip if already encoded */
    const char* current_encoding = csilk_get_header(c, "Content-Encoding");
    if (current_encoding) {
        CSILK_LOG_D("Gzip: skipping response compression - already encoded as '%s'",
                    current_encoding);
        return;
    }

    /* Skip non-compressible content types — binary formats (images, video,
     audio) and already-compressed archives yield negligible gains. */
    const char* content_type = csilk_get_header(c, "Content-Type");
    if (content_type) {
        if (strstr(content_type, "image/") || strstr(content_type, "video/") ||
            strstr(content_type, "audio/") || strstr(content_type, "application/pdf") ||
            strstr(content_type, "application/zip") || strstr(content_type, "application/x-gzip")) {
            CSILK_LOG_D("Gzip: skipping response compression - non-compressible "
                        "Content-Type '%s'",
                        content_type);
            return;
        }
    }

    /* Client capability check: only compress if the client advertises support
     for gzip content-encoding. */
    const char* accept_encoding = csilk_get_header(c, "Accept-Encoding");
    if (!accept_encoding || !strstr(accept_encoding, "gzip")) {
        CSILK_LOG_D("Gzip: skipping response compression - client Accept-Encoding '%s' "
                    "does not include gzip",
                    accept_encoding ? accept_encoding : "");
        return;
    }

    if (body_len < CSILK_GZIP_MIN_LENGTH) {
        CSILK_LOG_D("Gzip: skipping response compression - body length %zu is below "
                    "minimum threshold of %d bytes",
                    body_len,
                    CSILK_GZIP_MIN_LENGTH);
        return;
    }

    /* Offload compression to thread pool so the event loop is not
     blocked. State is attached to the context via csilk_set() and picked up
     by gzip_work_cb / gzip_after_work_cb. */
    gzip_async_state_t* state = calloc(1, sizeof(gzip_async_state_t));
    if (!state) {
        CSILK_LOG_E("Gzip: failed to allocate memory for gzip_async_state_t");
        return;
    }

    csilk_set(c, "gzip_state", state);
    csilk_io_work_t* req = csilk_get_work_req(c);
    req->data = c;
    csilk_ctx_set_async(c, 1);

    CSILK_LOG_D("Gzip: scheduling asynchronous deflation of %zu bytes on thread pool", body_len);

    csilk_io_loop_t* loop = _csilk_ctx_loop(c);
    _csilk_ctx_async_ref_incr(c);
    int rc = csilk_io_queue_work(loop, req, gzip_work_cb, gzip_after_work_cb);
    if (rc != 0) {
        CSILK_LOG_E("Gzip: failed to queue work: %d", rc);
        _csilk_ctx_async_ref_decr(c);
        if (state) {
            free(state);
        }
        csilk_set_status(c, CSILK_STATUS_INTERNAL_SERVER_ERROR);
        _csilk_send_response(c);
    }
}
