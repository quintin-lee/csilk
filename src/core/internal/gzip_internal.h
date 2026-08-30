/**
 * @file gzip_internal.h
 * @brief Internal gzip async compression state and callbacks.
 *
 * Exposes the thread-pool work/after callbacks so unit tests can drive them
 * synchronously without spawning the real (persistent) libuv thread pool,
 * which would keep worker threads alive past main() and crash ASAN's
 * exit-time global teardown.
 *
 * @copyright MIT License
 */
#ifndef CSILK_GZIP_INTERNAL_H
#define CSILK_GZIP_INTERNAL_H

#include "csilk/core/types.h"
#include "csilk/core/sys_io.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief State for gzip compression, shared between the work and after-work
 * callbacks. Owned by the request context via csilk_set("gzip_state", ...). */
typedef struct {
    uint8_t* dest;           /**< Compressed output buffer. */
    size_t   dest_cap;       /**< Capacity of the output buffer. */
    int      ret;            /**< zlib return status. */
    size_t   compressed_len; /**< Actual compressed data length. */
} gzip_async_state_t;

/** @brief Work callback: gzip-compress the response body (thread-pool side). */
CSILK_INTERNAL void _csilk_gzip_work_cb(csilk_io_work_t* req);

/** @brief After-work callback: apply compressed body + headers (loop side). */
CSILK_INTERNAL void _csilk_gzip_after_work_cb(csilk_io_work_t* req, int status);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_GZIP_INTERNAL_H */