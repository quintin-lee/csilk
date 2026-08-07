/**
 * @file context.c
 * @brief Request/response context lifecycle and body I/O.
 *
 * Provides the per-request csilk_ctx_t lifecycle: initialization, cleanup,
 * request/body management, response status and body setup, async/chunked
 * response support, and driver configuration.
 *
 * Key design points:
 *   - The context carries an arena (bump allocator) for all request-scoped
 *     allocations — path strings, query params, handler chains, header
 *     entries. The entire arena is freed in one shot at request end.
 *   - Async mode is signalled via ctx->is_async: when true, the response
 *     is NOT sent in on_message_complete; the handler must call
 *     csilk_send() or csilk_stream() explicitly.
 *   - The context also holds driver pointers (storage, crypto, cipher)
 *     inherited from the server at connection time.
 *
 * @copyright MIT License
 */

#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "ctx_internal.h"
#include "../primitives/header_map.h"
#include "../primitives/query.h"
#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"

/** @brief Invoke the next handler in the middleware/route chain.
 *
 * Advances the internal handler index and calls the next non-null handler.
 * If the context has been aborted (via csilk_abort()) or no handlers are
 * registered, this is a no-op.  Each handler is responsible for calling
 * csilk_next() to continue the chain — handlers that set a terminal response
 * (e.g. csilk_string) typically do NOT call csilk_next().
 *
 * @param c The request context. */
void
csilk_next(csilk_ctx_t* c)
{
    if (c->aborted || c->panicked || c->handlers == nullptr) {
        return;
    }
    c->handler_index++;
    if (c->handlers[c->handler_index] != nullptr) {
        c->handlers[c->handler_index](c);
    }
}

/** @brief Abort the handler chain immediately.
 *
 * Sets the aborted flag on the context. Subsequent calls to csilk_next()
 * are ignored. The response is still sent once the current handler returns.
 *
 * @param c The request context.
 * @note This does NOT close the connection — it only prevents further
 *       handlers from executing. */
void
csilk_abort(csilk_ctx_t* c)
{
    c->aborted = 1;
}

static _Thread_local char* tls_large_body_pool = nullptr;

static void
tls_large_body_pool_cleanup(void)
{
    if (tls_large_body_pool) {
        free(tls_large_body_pool);
        tls_large_body_pool = nullptr;
    }
}

/** @brief Clean up request context resources between requests.
 *
 * Resets the arena allocator for reuse, frees URL path parameters, request
 * body, and path strings. Clears all header maps (request, response, query,
 * form). Cleans up storage items and resets all state flags for the next
 * request. Called after each HTTP request is fully processed.
 *
 * @param c The request context. */
void
csilk_ctx_cleanup(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    csilk_ctx_defer_free(c);

    if (c->arena) {
        csilk_arena_reset(c->arena);
    } else {
        for (int i = 0; i < c->params_count; i++) {
            free(c->params[i].key);
            free(c->params[i].value);
        }
    }
    c->params_count = 0;

    /*
     * request.path is always strdup'd (malloc'd) by
     * csilk_split_url (or test_utils).  csilk_arena_reset
     * above does NOT free it — we must free it here.
     */
    free(c->request.path);
    c->request.path = nullptr;

    if (c->request.body && c->request.body_is_managed) {
        if (!tls_large_body_pool && c->request.body_len >= 65536) {
            tls_large_body_pool = c->request.body;
            static _Thread_local int cleanup_registered = 0;
            if (!cleanup_registered) {
                atexit(tls_large_body_pool_cleanup);
                cleanup_registered = 1;
            }
        } else {
            free(c->request.body);
        }
    }
    c->request.body = nullptr;
    c->request.body_len = 0;
    c->request.body_is_managed = 0;

    for (int i = 0; i < c->read_buffers_count; i++) {
        if (c->read_buffers[i]) {
            free(c->read_buffers[i]);
            c->read_buffers[i] = nullptr;
        }
    }
    c->read_buffers_count = 0;

    memset(&c->request.headers, 0, sizeof(csilk_header_map_t));
    memset(&c->request.query_params, 0, sizeof(csilk_header_map_t));
    memset(&c->request.form_params, 0, sizeof(csilk_header_map_t));
    memset(&c->response.headers, 0, sizeof(csilk_header_map_t));

    if (c->response.body && c->response.body_is_managed) {
        free((void*)c->response.body);
        c->response.body = nullptr;
        c->response.body_is_managed = 0;
    }

    if (c->file_fd >= 0) {
        csilk_io_fs_t close_req;
        csilk_io_fs_close(nullptr, &close_req, c->file_fd, nullptr);
        csilk_io_fs_req_cleanup(&close_req);
        c->file_fd = -1;
    }
    c->file_offset = 0;
    c->file_size = 0;

    if (c->storage_driver && c->storage_driver->clear) {
        c->storage_driver->clear(c);
    }
    c->storage_head = nullptr;

    c->aborted = 0;
    c->panicked = 0;
    c->is_websocket = 0;
    c->is_sse = 0;
    c->is_async = 0;
    c->response_started = 0;
    c->handler_index = -1;
    c->current_handler = nullptr;
    c->on_ws_message = nullptr;
    memset(c->request_id, 0, sizeof(c->request_id));
}

/** @brief Get the request body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-nullptr, receives the body length in bytes.
 * @return Pointer to the raw request body, or nullptr if no body or nullptr context.
 * @note The returned pointer is heap-allocated and freed in
 * csilk_ctx_cleanup(). */
const char*
csilk_get_body(csilk_ctx_t* c, size_t* out_len)
{
    if (out_len) {
        *out_len = c ? c->request.body_len : 0;
    }
    return c ? c->request.body : nullptr;
}

/** @brief Get the length of the request body.
 *
 * @param c The request context.
 * @return Body length in bytes, or 0 if the context is nullptr or body is empty.
 */
size_t
csilk_get_body_len(csilk_ctx_t* c)
{
    return c ? c->request.body_len : 0;
}

void
csilk_set_status(csilk_ctx_t* c, int status)
{
    if (c) {
        c->response.status = status;
    }
}

/** @brief Get the response body data and optionally its length.
 *
 * @param c       The request context.
 * @param out_len [out] If non-nullptr, receives the response body length.
 * @return Pointer to the response body, or nullptr if no body or nullptr context.
 * @note The body may be managed (arena or heap) depending on how it was set.
 *       The caller must not free the returned pointer. */
const char*
csilk_get_response_body(csilk_ctx_t* c, size_t* out_len)
{
    if (!c) {
        if (out_len) {
            *out_len = 0;
        }
        return nullptr;
    }
    if (out_len) {
        *out_len = c->response.body_len;
    }
    return c->response.body;
}

/** @brief Set the response body directly with explicit ownership semantics.
 *
 * Replaces any existing response body. If the old body was marked as managed
 * it is freed before replacement. The caller specifies whether the new body
 * should be freed automatically during cleanup.
 *
 * @param c       The request context.
 * @param body    Pointer to the body data (may be nullptr).
 * @param len     Body length in bytes.
 * @param managed If non-zero, the framework will free @p body during cleanup.
 * @note Setting managed=1 transfers ownership to the framework. With
 *       managed=0 the caller retains ownership and must keep the pointer
 *       valid until the response is sent. */
void
csilk_set_response_body(csilk_ctx_t* c, const char* body, size_t len, int managed)
{
    if (!c) {
        return;
    }
    if (c->response.body && c->response.body_is_managed) {
        free((void*)c->response.body);
    }
    c->response.body = body;
    c->response.body_len = len;
    c->response.body_is_managed = managed;
}

/** @brief Configure zero-copy file transmission.
 *
 * @param c      The request context.
 * @param fd     Open file descriptor.
 * @param offset Byte offset to start sending.
 * @param size   Number of bytes to send. */
void
csilk_set_file_response(csilk_ctx_t* c, int fd, size_t offset, size_t size)
{
    if (c) {
        c->file_fd = fd;
        c->file_offset = offset;
        c->file_size = size;
    }
}

/** @brief Get the zero-copy file descriptor.
 *
 * @param c The request context.
 * @return File descriptor or -1. */
int
csilk_get_file_fd(csilk_ctx_t* c)
{
    return c ? c->file_fd : -1;
}

/** @brief Initialize a request context.
 *
 * Sets up default values for all fields. Should be called for both
 * static (embedded in client) and dynamic (H2 stream) contexts.
 *
 * @param c       The context to initialize.
 * @param s       The owning server instance.
 * @param client  The underlying connection object (csilk_client_t*). */
CSILK_INTERNAL void
_csilk_ctx_init(csilk_ctx_t* c, struct csilk_server_s* s, void* client)
{
    if (!c) {
        return;
    }
    memset(c, 0, sizeof(csilk_ctx_t));
    c->handler_index = -1;
    c->file_fd = -1;
    c->_internal_client = client;
    c->server = s;
    if (s) {
        c->storage_driver = s->storage_driver;
        c->crypto_driver = s->crypto_driver;
        c->cipher_driver = s->cipher_driver;
    }
}

/** @brief Set the storage driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_storage_driver(csilk_ctx_t* c, csilk_storage_driver_t* driver)
{
    if (c) {
        c->storage_driver = driver;
    }
}

/** @brief Set the crypto driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_crypto_driver(csilk_ctx_t* c, csilk_crypto_driver_t* driver)
{
    if (c) {
        c->crypto_driver = driver;
    }
}

/** @brief Set the cipher driver.
 *
 * @param c      The request context.
 * @param driver Pointer to driver vtable. */
void
csilk_ctx_set_cipher_driver(csilk_ctx_t* c, csilk_cipher_driver_t* driver)
{
    if (c) {
        c->cipher_driver = driver;
    }
}
