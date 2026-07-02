/**
 * @file test_utils.c
 * @brief Internal test helpers implementation.
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "core/ctx_internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

/** @brief Allocate and initialise a test context.
 *
 * Creates a zero-initialised csilk_ctx_t with a fresh arena,
 * handler index set to -1 and file descriptor set to -1.
 *
 * @return Pointer to the new context, or nullptr on allocation failure.
 */
csilk_ctx_t*
csilk_test_ctx_new(void)
{
    csilk_ctx_t* c = calloc(1, sizeof(csilk_ctx_t));
    if (c) {
        c->handler_index = -1;
        c->arena = csilk_arena_new(4096);
        c->file_fd = -1;
    }
    return c;
}

/** @brief Free a test context and all its resources.
 *
 * Calls csilk_ctx_cleanup, frees the arena, then frees the
 * context structure itself. Safe to call with nullptr.
 *
 * @param c The context to free (may be nullptr).
 */
void
csilk_test_ctx_free(csilk_ctx_t* c)
{
    if (c) {
        csilk_ctx_cleanup(c);
        if (c->arena) {
            csilk_arena_free(c->arena);
        }
        free(c);
    }
}

/** @brief Attach a handler array to a test context.
 *
 * @param c The context (may be nullptr).
 * @param handlers Pointer to the handler table to assign.
 */
void
csilk_test_ctx_set_handlers(csilk_ctx_t* c, csilk_handler_t* handlers)
{
    if (c) {
        c->handlers = handlers;
    }
}

/** @brief Set the HTTP method and path on a test context.
 *
 * The path is duplicated; any previously set path is freed
 * before replacement.
 *
 * @param c The context (may be nullptr).
 * @param method HTTP method string (assigned by pointer, not copied).
 * @param path Request path string (copied internally).
 */
void
csilk_test_ctx_set_request(csilk_ctx_t* c, const char* method, const char* path)
{
    if (c) {
        c->request.method = (char*)method;
        if (c->request.path) {
            free((void*)c->request.path);
        }
        c->request.path = path ? strdup(path) : nullptr;
    }
}

/** @brief Set permission metadata on the current handler.
 *
 * Allocates a new csilk_method_handler_t in the context's arena
 * if none exists yet, then sets the permission fields.
 *
 * @param c The context (may be nullptr).
 * @param perm_required Permission required string (assigned by pointer).
 * @param perm_resource Permission resource string (assigned by pointer).
 */
void
csilk_test_ctx_set_handler_metadata(csilk_ctx_t* c,
                                    const char*  perm_required,
                                    const char*  perm_resource)
{
    if (c) {
        if (!c->current_handler) {
            c->current_handler = csilk_arena_alloc(c->arena, sizeof(csilk_method_handler_t));
            if (c->current_handler) {
                memset(c->current_handler, 0, sizeof(csilk_method_handler_t));
            }
        }
        if (c->current_handler) {
            c->current_handler->perm_required = (char*)perm_required;
            c->current_handler->perm_resource = (char*)perm_resource;
        }
    }
}

/** @brief Set the request body on a test context.
 *
 * The body string is duplicated; any previously set body is
 * freed before replacement.
 *
 * @param c The context (may be nullptr).
 * @param body Body content (copied internally; may be nullptr).
 * @param len Length of the body data.
 */
void
csilk_test_ctx_set_body(csilk_ctx_t* c, const char* body, size_t len)
{
    if (c) {
        if (c->arena && body) {
            char* arena_body = csilk_arena_alloc(c->arena, len + 1);
            if (arena_body) {
                memcpy(arena_body, body, len);
                arena_body[len] = '\0';
                c->request.body = arena_body;
            } else {
                c->request.body = nullptr;
            }
        } else {
            c->request.body = (char*)body;
        }
        c->request.body_len = len;
    }
}

/** @brief Add a key-value parameter to the test context.
 *
 * Parameters are stored in a fixed-size array; excess
 * entries beyond CSILK_MAX_PARAMS are silently dropped.
 * Strings are arena-allocated when possible, otherwise
 * heap-allocated with strdup.
 *
 * @param c The context (may be nullptr).
 * @param key Parameter name.
 * @param value Parameter value.
 */
void
csilk_test_ctx_add_param(csilk_ctx_t* c, const char* key, const char* value)
{
    if (c && c->params_count < CSILK_MAX_PARAMS) {
        if (c->arena) {
            c->params[c->params_count].key = csilk_arena_strdup(c->arena, key);
            c->params[c->params_count].value = csilk_arena_strdup(c->arena, value);
        } else {
            c->params[c->params_count].key = strdup(key);
            c->params[c->params_count].value = strdup(value);
        }
        c->params_count++;
    }
}

/** @brief Count response headers matching a key and optional value.
 *
 * Iterates the response header hash table and counts entries
 * whose key matches (case-insensitive). If value_contains is
 * non-nullptr, only headers whose value contains that substring
 * are counted.
 *
 * @param c The context (must not be nullptr).
 * @param key Header key to match (case-insensitive, must not be nullptr).
 * @param value_contains Optional substring to match against the header value.
 * @return Number of matching headers.
 */
int
csilk_test_ctx_count_response_headers(csilk_ctx_t* c, const char* key, const char* value_contains)
{
    if (!c || !key) {
        return 0;
    }
    int count = 0;
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        csilk_header_t* h = c->response.headers.buckets[i];
        while (h) {
            if (strcasecmp(h->key, key) == 0) {
                if (!value_contains || strstr(h->value, value_contains)) {
                    count++;
                }
            }
            h = h->next;
        }
    }
    return count;
}
