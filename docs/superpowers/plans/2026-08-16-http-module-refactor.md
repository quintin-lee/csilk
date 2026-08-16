# HTTP Module Refactoring Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Split `h2.c` (696 lines) and `http1_response.c` (567 lines) into 6 focused modules.

**Architecture:** Extract callbacks, session management, and response handling into separate files. Maintain backward compatibility with thin wrapper files.

**Tech Stack:** C23, CMake, nghttp2, libuv/io_uring

---

## File Structure

### New Files to Create
- `src/core/http/h2_callbacks.c` — nghttp2 callbacks
- `src/core/http/h2_session.c` — session/stream management
- `src/core/http/h2_response.c` — HTTP/2 response handling
- `src/core/http/http1_serialize.c` — response serialization
- `src/core/http/http1_write.c` — write pipeline
- `src/core/http/http1_pipeline.c` — post-response logic

### Files to Modify
- `src/core/http/h2.c` — reduce to thin wrapper
- `src/core/http/http1_response.c` — reduce to thin wrapper
- `src/core/internal/srv_impl.h` — update declarations
- `cmake/sources.cmake` — add new source files

---

## Task 1: Create h2_callbacks.c

**Files:**
- Create: `src/core/http/h2_callbacks.c`
- Modify: `src/core/http/h2.c` (remove callbacks)

- [ ] **Step 1: Create h2_callbacks.c**

```c
/**
 * @file h2_callbacks.c
 * @brief nghttp2 callback implementations for HTTP/2.
 */

#include "h2.h"
#include "csilk/csilk.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- Header callbacks --- */

static int
on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
    (void)session;
    (void)frame;
    (void)user_data;
    return 0;
}

static int
on_header_callback(nghttp2_session*     session,
                   const nghttp2_frame* frame,
                   const uint8_t*       name,
                   size_t               namelen,
                   const uint8_t*       value,
                   size_t               valuelen,
                   uint8_t              flags,
                   void*                user_data)
{
    (void)flags;
    csilk_client_t* client = (csilk_client_t*)user_data;
    if (frame->hd.type != NGHTTP2_HEADERS) {
        return 0;
    }

    csilk_ctx_t* c = csilk_h2_get_or_create_stream(client, frame->hd.stream_id);
    if (!c) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    if (name[0] == ':') {
        /* Pseudo-headers */
        if (strncmp((const char*)name, ":method", namelen) == 0) {
            c->request.method = csilk_arena_strndup(c->arena, (const char*)value, valuelen);
        } else if (strncmp((const char*)name, ":path", namelen) == 0) {
            /* Split path and query */
            char* full_path = csilk_arena_strndup(c->arena, (const char*)value, valuelen);
            char* path;
            char* query;
            csilk_split_url(full_path, &path, &query);
            c->request.path = path;
            if (query) {
                csilk_parse_query(c, query);
                free(query);
            }
        }
    } else {
        /* Regular headers */
        char* h_name = csilk_arena_strndup(c->arena, (const char*)name, namelen);
        char* h_value = csilk_arena_strndup(c->arena, (const char*)value, valuelen);
        csilk_set_request_header(c, h_name, h_value);
    }

    return 0;
}

/* --- Frame callbacks --- */

static int
on_frame_recv_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
    csilk_client_t* client = (csilk_client_t*)user_data;

    if (frame->hd.type == NGHTTP2_HEADERS && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        csilk_ctx_t* c = csilk_h2_get_or_create_stream(client, frame->hd.stream_id);
        if (c) {
            _csilk_dispatch_request(c);
        }
    } else if (frame->hd.type == NGHTTP2_DATA && (frame->hd.flags & NGHTTP2_FLAG_END_STREAM)) {
        csilk_ctx_t* c = csilk_h2_get_or_create_stream(client, frame->hd.stream_id);
        if (c) {
            _csilk_dispatch_request(c);
        }
    }

    return 0;
}

/* --- Data callbacks --- */

static int
on_data_chunk_recv_callback(nghttp2_session* session,
                            uint8_t          flags,
                            int32_t          stream_id,
                            const uint8_t*   data,
                            size_t           len,
                            void*            user_data)
{
    (void)session;
    (void)flags;
    csilk_client_t* client = (csilk_client_t*)user_data;
    csilk_ctx_t*    c = csilk_h2_get_or_create_stream(client, stream_id);
    if (!c) {
        return 0;
    }

    /* Accumulate body */
    if (c->request.body_len + len > client->server->config.max_body_size) {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    char* new_body = realloc(c->request.body, c->request.body_len + len + 1);
    if (new_body) {
        memcpy(new_body + c->request.body_len, data, len);
        c->request.body_len += len;
        new_body[c->request.body_len] = '\0';
        c->request.body = new_body;
        c->request.body_is_managed = 1;
    } else {
        return NGHTTP2_ERR_CALLBACK_FAILURE;
    }

    return 0;
}

/* --- Response body streaming --- */

static ssize_t
body_read_callback(nghttp2_session*     session,
                   int32_t              stream_id,
                   uint8_t*             buf,
                   size_t               length,
                   uint32_t*            data_flags,
                   nghttp2_data_source* source,
                   void*                user_data)
{
    (void)session;
    (void)stream_id;
    (void)user_data;
    csilk_ctx_t* c = (csilk_ctx_t*)source->ptr;

    size_t      body_len = c->response.body_len;
    const char* body = (const char*)c->response.body;

    if (!body) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
        return 0;
    }

    size_t offset = 0;
    void*  offset_ptr = csilk_get(c, "_h2_body_offset");
    if (offset_ptr) {
        offset = (size_t)(uintptr_t)offset_ptr;
    }

    size_t remaining = body_len - offset;
    size_t to_copy = remaining < length ? remaining : length;

    memcpy(buf, body + offset, to_copy);
    offset += to_copy;

    csilk_set(c, "_h2_body_offset", (void*)(uintptr_t)offset);

    if (offset >= body_len) {
        *data_flags |= NGHTTP2_DATA_FLAG_EOF;
    }

    return (ssize_t)to_copy;
}

/* --- Stream close --- */

static int
on_stream_close_callback(nghttp2_session* session,
                         int32_t          stream_id,
                         uint32_t         error_code,
                         void*            user_data)
{
    (void)session;
    (void)error_code;
    csilk_client_t* client = (csilk_client_t*)user_data;

    /* Find and remove context from list */
    csilk_ctx_t** curr = &client->h2_streams;
    while (*curr) {
        if ((*curr)->stream_id == stream_id) {
            csilk_ctx_t* found = *curr;
            *curr = found->next_stream;

            csilk_ctx_cleanup(found);
            if (found->arena) {
                csilk_arena_free(found->arena);
                found->arena = NULL;
            }

            free(found);
            return 0;
        }
        curr = &((*curr)->next_stream);
    }

    return 0;
}

/* --- Send callback --- */

static ssize_t
send_callback(
    nghttp2_session* session, const uint8_t* data, size_t length, int flags, void* user_data)
{
    (void)session;
    (void)flags;
    csilk_client_t* client = (csilk_client_t*)user_data;
    csilk_client_write(client, data, length);
    return (ssize_t)length;
}
```

- [ ] **Step 2: Remove callbacks from h2.c**

In `src/core/http/h2.c`, remove lines 14-531 (all callback functions).

- [ ] **Step 3: Build and test**

```bash
CCACHE_DISABLE=1 cmake --build build --target csilk_http -j4
CCACHE_DISABLE=1 ctest --test-dir build -R test_connection --timeout 10 --output-on-failure
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/core/http/h2_callbacks.c src/core/http/h2.c
git commit -m "refactor(http): 🔄 extract nghttp2 callbacks to h2_callbacks.c"
```

---

## Task 2: Create h2_session.c

**Files:**
- Create: `src/core/http/h2_session.c`
- Modify: `src/core/http/h2.c` (remove session functions)

- [ ] **Step 1: Create h2_session.c**

```c
/**
 * @file h2_session.c
 * @brief HTTP/2 session and stream management.
 */

#include "h2.h"
#include "csilk/csilk.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- Stream lookup/creation --- */

csilk_ctx_t*
csilk_h2_get_or_create_stream(csilk_client_t* client, int32_t stream_id)
{
    csilk_ctx_t* curr = client->h2_streams;
    while (curr) {
        if (curr->stream_id == stream_id) {
            return curr;
        }
        curr = curr->next_stream;
    }

    /* Create new context for stream */
    csilk_ctx_t* ctx = malloc(sizeof(csilk_ctx_t));
    if (!ctx) {
        return NULL;
    }

    _csilk_ctx_init(ctx, client->server, client);
    ctx->stream_id = stream_id;
    ctx->arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
    if (client->server->config.enable_arena_alignment) {
        csilk_arena_set_alignment(ctx->arena, 1);
    }

    /* Prepend to list */
    ctx->next_stream = client->h2_streams;
    client->h2_streams = ctx;

    return ctx;
}

/**
 * @brief Free all HTTP/2 streams associated with a client connection.
 * @param[in] client Client whose h2_streams list is torn down.
 */
void
csilk_h2_free_streams(csilk_client_t* client)
{
    csilk_ctx_t* curr = client->h2_streams;
    while (curr) {
        csilk_ctx_t* next = curr->next_stream;
        csilk_ctx_cleanup(curr);
        if (curr->arena) {
            csilk_arena_free(curr->arena);
            curr->arena = NULL;
        }
        free(curr);
        curr = next;
    }
    client->h2_streams = NULL;
}

/* --- Session initialization --- */

int
csilk_h2_init_session(csilk_client_t* client)
{
    nghttp2_session_callbacks* callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        return -1;
    }

    extern int on_begin_headers_callback(nghttp2_session*, const nghttp2_frame*, void*);
    extern int on_header_callback(nghttp2_session*, const nghttp2_frame*,
                                  const uint8_t*, size_t, const uint8_t*, size_t,
                                  uint8_t, void*);
    extern int on_frame_recv_callback(nghttp2_session*, const nghttp2_frame*, void*);
    extern int on_data_chunk_recv_callback(nghttp2_session*, uint8_t, int32_t,
                                           const uint8_t*, size_t, void*);
    extern int on_stream_close_callback(nghttp2_session*, int32_t, uint32_t, void*);
    extern ssize_t send_callback(nghttp2_session*, const uint8_t*, size_t, int, void*);

    nghttp2_session_callbacks_set_send_callback(callbacks, send_callback);
    nghttp2_session_callbacks_set_on_frame_recv_callback(callbacks, on_frame_recv_callback);
    nghttp2_session_callbacks_set_on_data_chunk_recv_callback(callbacks,
                                                              on_data_chunk_recv_callback);
    nghttp2_session_callbacks_set_on_stream_close_callback(callbacks, on_stream_close_callback);
    nghttp2_session_callbacks_set_on_header_callback(callbacks, on_header_callback);
    nghttp2_session_callbacks_set_on_begin_headers_callback(callbacks, on_begin_headers_callback);

    if (nghttp2_session_server_new(&client->h2_session, callbacks, client) != 0) {
        nghttp2_session_callbacks_del(callbacks);
        return -1;
    }

    nghttp2_session_callbacks_del(callbacks);

    nghttp2_settings_entry iv[1] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}
    };

    if (nghttp2_submit_settings(client->h2_session, NGHTTP2_FLAG_NONE, iv, 1) != 0) {
        return -1;
    }

    nghttp2_session_send(client->h2_session);

    return 0;
}

/* --- Data processing --- */

int
csilk_h2_process_data(csilk_client_t* client, const uint8_t* data, size_t len)
{
    ssize_t rv = nghttp2_session_mem_recv(client->h2_session, data, len);
    if (rv < 0) {
        return -1;
    }

    if (nghttp2_session_send(client->h2_session) != 0) {
        return -1;
    }

    return 0;
}
```

- [ ] **Step 2: Remove session functions from h2.c**

In `src/core/http/h2.c`, remove lines 533-696 (csilk_h2_get_or_create_stream through csilk_h2_process_data).

- [ ] **Step 3: Build and test**

```bash
CCACHE_DISABLE=1 cmake --build build --target csilk_http -j4
CCACHE_DISABLE=1 ctest --test-dir build -R test_connection --timeout 10 --output-on-failure
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/core/http/h2_session.c src/core/http/h2.c
git commit -m "refactor(http): 🔄 extract session management to h2_session.c"
```

---

## Task 3: Create h2_response.c

**Files:**
- Create: `src/core/http/h2_response.c`
- Modify: `src/core/http/h2.c` (remove response functions)

- [ ] **Step 1: Create h2_response.c**

```c
/**
 * @file h2_response.c
 * @brief HTTP/2 response sending and server push.
 */

#include "h2.h"
#include "csilk/csilk.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

/* --- Send response --- */

void
csilk_h2_send_response(csilk_ctx_t* c)
{
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client || !client->h2_session) {
        return;
    }

    int header_count = 1; /* :status */
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
            header_count++;
        }
    }

    nghttp2_nv  stack_hdrs[32];
    nghttp2_nv* nva = stack_hdrs;
    if (header_count > 32) {
        nva = malloc(sizeof(nghttp2_nv) * (size_t)header_count);
        if (!nva) {
            return;
        }
    }

    char status_str[16];
    snprintf(status_str, sizeof(status_str), "%d", c->response.status ? c->response.status : 200);

    nva[0].name = (uint8_t*)":status";
    nva[0].namelen = 7;
    nva[0].value = (uint8_t*)status_str;
    nva[0].valuelen = strlen(status_str);
    nva[0].flags = NGHTTP2_NV_FLAG_NONE;

    int idx = 1;
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = c->response.headers.buckets[i]; h; h = h->next) {
            nva[idx].name = (uint8_t*)h->key;
            nva[idx].namelen = h->key_len;
            nva[idx].value = (uint8_t*)h->value;
            nva[idx].valuelen = h->value_len;
            nva[idx].flags = NGHTTP2_NV_FLAG_NONE;
            idx++;
        }
    }

    /* Forward declaration for body_read_callback */
    extern ssize_t body_read_callback(nghttp2_session*, int32_t, uint8_t*, size_t,
                                      uint32_t*, nghttp2_data_source*, void*);

    nghttp2_data_provider  prd;
    nghttp2_data_provider* p_prd = NULL;

    if (c->response.body_len > 0) {
        prd.source.ptr = c;
        prd.read_callback = body_read_callback;
        p_prd = &prd;
    }

    nghttp2_submit_response(client->h2_session, c->stream_id, nva, (size_t)header_count, p_prd);
    if (nva != stack_hdrs) {
        free(nva);
    }

    nghttp2_session_send(client->h2_session);

    _csilk_trigger_hooks(client->server, c, CSILK_HOOK_REQUEST_END);
}

/* --- Server push --- */

int32_t
csilk_h2_submit_push(csilk_ctx_t* c, const char* method, const char* path)
{
    if (!c || !method || !path) {
        return -1;
    }

    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client || !client->h2_session) {
        return -1;
    }

    csilk_server_t* server = client->server;
    if (!server) {
        return -1;
    }

    if (!server->config.h2_push_enable) {
        return -1;
    }

    /* Enforce per-request push limit */
    int max_push = server->config.h2_max_push_per_request;
    if (max_push <= 0) {
        max_push = 10;
    }
    int   push_count = 0;
    void* count_ptr = csilk_get(c, "_h2_push_count");
    if (count_ptr) {
        push_count = (int)(uintptr_t)count_ptr;
    }
    if (push_count >= max_push) {
        return -1;
    }

    /* Extract :authority and :scheme from the original request, falling back */
    const char* authority = csilk_get_header(c, ":authority");
    const char* scheme = csilk_get_header(c, ":scheme");
    if (!authority) {
        authority = csilk_get_header(c, "host");
    }
    if (!authority || authority[0] == '\0') {
        authority = "localhost";
    }
    if (!scheme || scheme[0] == '\0') {
        scheme = (server->config.enable_tls) ? "https" : "http";
    }

    /* Build the request pseudo-headers for the pushed resource */
    nghttp2_nv push_headers[] = {
        {(uint8_t*)":method",    (uint8_t*)method, 7, (uint8_t)strlen(method),    NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)":path",      (uint8_t*)path,   5, (uint8_t)strlen(path),      NGHTTP2_NV_FLAG_NONE},
        {(uint8_t*)":authority",
         (uint8_t*)authority,
         10,                                          (uint8_t)strlen(authority),
         NGHTTP2_NV_FLAG_NONE                                                                         },
        {(uint8_t*)":scheme",    (uint8_t*)scheme, 7, (uint8_t)strlen(scheme),    NGHTTP2_NV_FLAG_NONE},
    };

    /* Check if push is enabled by client */
    if (nghttp2_session_get_remote_settings(client->h2_session, NGHTTP2_SETTINGS_ENABLE_PUSH) ==
        0) {
        return -1;
    }

    /* Submit the PUSH_PROMISE */
    int32_t promised_id =
        nghttp2_submit_push_promise(client->h2_session,
                                    NGHTTP2_FLAG_NONE,
                                    c->stream_id,
                                    push_headers,
                                    sizeof(push_headers) / sizeof(push_headers[0]),
                                    NULL);

    if (promised_id < 0) {
        return promised_id;
    }

    /* Create a request context for the promised stream */
    extern csilk_ctx_t* csilk_h2_get_or_create_stream(csilk_client_t* client, int32_t stream_id);
    csilk_ctx_t* pushed_c = csilk_h2_get_or_create_stream(client, promised_id);
    if (!pushed_c) {
        return -1;
    }

    /* Set up the synthesized request */
    pushed_c->request.method = csilk_arena_strdup(pushed_c->arena, method);
    char* path_heap = NULL;
    char* query_heap = NULL;
    csilk_split_url(path, &path_heap, &query_heap);
    pushed_c->request.path = path_heap;
    if (query_heap) {
        csilk_parse_query(pushed_c, query_heap);
        free(query_heap);
    }

    /* Increment push counter on the original context */
    csilk_set(c, "_h2_push_count", (void*)(uintptr_t)(push_count + 1));

    /* Dispatch the pushed request through the router */
    extern void _csilk_dispatch_request(csilk_ctx_t* c);
    _csilk_dispatch_request(pushed_c);

    /* Flush everything */
    int rv = nghttp2_session_send(client->h2_session);

    return promised_id;
}
```

- [ ] **Step 2: Remove response functions from h2.c**

In `src/core/http/h2.c`, remove lines 250-460 (csilk_h2_send_response and csilk_h2_submit_push).

- [ ] **Step 3: Build and test**

```bash
CCACHE_DISABLE=1 cmake --build build --target csilk_http -j4
CCACHE_DISABLE=1 ctest --test-dir build -R test_connection --timeout 10 --output-on-failure
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/core/http/h2_response.c src/core/http/h2.c
git commit -m "refactor(http): 🔄 extract response handling to h2_response.c"
```

---

## Task 4: Create http1_serialize.c

**Files:**
- Create: `src/core/http/http1_serialize.c`
- Modify: `src/core/http/http1_response.c` (remove serialize functions)

- [ ] **Step 1: Create http1_serialize.c**

```c
/**
 * @file http1_serialize.c
 * @brief HTTP/1.1 response serialization.
 */

#include <assert.h>
#include <llhttp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../ctx/ctx_internal.h"
#include "../primitives/header_map.h"
#include "h2.h"
#include "../internal/srv_impl.h"

/* --- Status text --- */

const char*
get_status_text(int status)
{
    switch (status) {
    case CSILK_STATUS_SWITCHING_PROTOCOLS:
        return "Switching Protocols";
    case CSILK_STATUS_OK:
        return "OK";
    case CSILK_STATUS_CREATED:
        return "Created";
    case CSILK_STATUS_NO_CONTENT:
        return "No Content";
    case CSILK_STATUS_BAD_REQUEST:
        return "Bad Request";
    case CSILK_STATUS_UNAUTHORIZED:
        return "Unauthorized";
    case CSILK_STATUS_FORBIDDEN:
        return "Forbidden";
    case CSILK_STATUS_NOT_FOUND:
        return "Not Found";
    case CSILK_STATUS_INTERNAL_SERVER_ERROR:
        return "Internal Server Error";
    default:
        return "OK";
    }
}

/* --- Serialization helpers --- */

static int
serialize_status_line(char*       buf,
                      size_t      buf_size,
                      int         status,
                      const char* status_text,
                      int         use_chunked,
                      const char* transfer_encoding,
                      size_t      body_len,
                      const char* connection_val)
{
    if (status == CSILK_STATUS_SWITCHING_PROTOCOLS) {
        return snprintf(buf, buf_size, "HTTP/1.1 101 Switching Protocols\r\n");
    } else if (use_chunked) {
        return snprintf(buf,
                        buf_size,
                        "HTTP/1.1 %d %s\r\n"
                        "%s"
                        "Connection: %s\r\n",
                        status,
                        status_text,
                        transfer_encoding,
                        connection_val);
    } else {
        return snprintf(buf,
                        buf_size,
                        "HTTP/1.1 %d %s\r\n"
                        "Content-Length: %zu\r\n"
                        "Connection: %s\r\n",
                        status,
                        status_text,
                        body_len,
                        connection_val);
    }
}

static size_t
append_custom_headers(csilk_header_map_t* headers, char* buf, size_t pos)
{
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = headers->buckets[i]; h; h = h->next) {
            memcpy(buf + pos, h->key, h->key_len);
            pos += h->key_len;
            buf[pos++] = ':';
            buf[pos++] = ' ';
            memcpy(buf + pos, h->value, h->value_len);
            pos += h->value_len;
            buf[pos++] = '\r';
            buf[pos++] = '\n';
        }
    }
    return pos;
}

/* --- Main response sender --- */

CSILK_INTERNAL void
_csilk_send_response(csilk_ctx_t* c)
{
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (!client) {
        return;
    }

    csilk_conn_set_state(client, CSILK_CONN_WRITING);

    if (client->protocol == CSILK_PROTO_HTTP2) {
        extern void csilk_h2_send_response(csilk_ctx_t* c);
        csilk_h2_send_response(c);
        return;
    }

    csilk_io_timer_stop(&client->request_timer);

    int         status = client->ctx.response.status ? client->ctx.response.status : 200;
    const char* status_text = get_status_text(status);

    int is_file = (c->file_fd >= 0 && !client->ssl);
    int use_chunked = (client->ctx.response.body_len == 0 && client->ctx.is_async && !is_file);
    const char* transfer_encoding = use_chunked ? "Transfer-Encoding: chunked\r\n" : "";

    size_t custom_headers_len = 0;
    for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
        for (csilk_header_t* h = client->ctx.response.headers.buckets[i]; h; h = h->next) {
            custom_headers_len += h->key_len + 2 + h->value_len + 2;
        }
    }

    size_t      body_len = is_file ? c->file_size : client->ctx.response.body_len;
    const char* body = client->ctx.response.body ? client->ctx.response.body : "";

    int keep_alive = llhttp_should_keep_alive(&client->parser);
    client->keep_alive = (int)keep_alive;
    const char* connection_val = keep_alive ? "keep-alive" : "close";

    /* Serialise status line (NULL/0 computes required length) */
    int header_len = serialize_status_line(
        NULL, 0, status, status_text, use_chunked, transfer_encoding, body_len, connection_val);
    if (header_len < 0) {
        return;
    }

    size_t response_len =
        (size_t)header_len + custom_headers_len + 2 + (use_chunked || is_file ? 0 : body_len);

    char* write_base = malloc(response_len + 1);
    if (write_base) {
        int snp = serialize_status_line(write_base,
                                        response_len + 1,
                                        status,
                                        status_text,
                                        use_chunked,
                                        transfer_encoding,
                                        body_len,
                                        connection_val);
        if (snp < 0) {
            free(write_base);
            return;
        }
        size_t pos = (size_t)snp;

        pos = append_custom_headers(&client->ctx.response.headers, write_base, pos);

        if (!use_chunked && !is_file) {
            size_t remain = response_len + 1 - pos;
            snprintf(write_base + pos, remain, "\r\n%s", body);
        } else {
            size_t remain = response_len + 1 - pos;
            snprintf(write_base + pos, remain, "\r\n");
        }

        extern void _csilk_send_data_owned(csilk_ctx_t* c, char* data, size_t len);
        _csilk_send_data_owned(
            c, write_base, (use_chunked || is_file ? (size_t)pos + 2 : response_len));
    }

    if (is_file) {
        return;
    }

    extern void _csilk_handle_post_response(csilk_client_t* client, int keep_alive);
    _csilk_handle_post_response(client, keep_alive);
}
```

- [ ] **Step 2: Remove serialization from http1_response.c**

In `src/core/http/http1_response.c`, remove lines 148-567 (get_status_text through _csilk_send_response).

- [ ] **Step 3: Build and test**

```bash
CCACHE_DISABLE=1 cmake --build build --target csilk_http -j4
CCACHE_DISABLE=1 ctest --test-dir build -R test_connection --timeout 10 --output-on-failure
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/core/http/http1_serialize.c src/core/http/http1_response.c
git commit -m "refactor(http): 🔄 extract serialization to http1_serialize.c"
```

---

## Task 5: Create http1_write.c

**Files:**
- Create: `src/core/http/http1_write.c`
- Modify: `src/core/http/http1_response.c` (remove write functions)

- [ ] **Step 1: Create http1_write.c**

```c
/**
 * @file http1_write.c
 * @brief HTTP/1.1 write pipeline and sendfile handling.
 */

#include <assert.h>
#include <llhttp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../ctx/ctx_internal.h"
#include "../primitives/header_map.h"
#include "h2.h"
#include "../internal/srv_impl.h"

/* --- Sendfile completion --- */

static void
on_sendfile_complete(csilk_io_fs_t* req)
{
    csilk_ctx_t*    c = (csilk_ctx_t*)req->data;
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    csilk_io_fs_req_cleanup(req);
    free(req);

    if (!client) {
        return;
    }

    int keep_alive = llhttp_should_keep_alive(&client->parser);
    client->keep_alive = (int)keep_alive;

    if (client->server->config.write_timeout_ms > 0) {
        csilk_io_timer_stop(&client->write_timer);
    }

    extern void on_idle_timeout(csilk_io_timer_t* handle);
    extern void on_close(csilk_io_handle_t* handle);
    extern void csilk_client_read_start(csilk_client_t* client);
    extern void _csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type);
    extern void csilk_ctx_cleanup(csilk_ctx_t* c);

    if (keep_alive) {
        csilk_io_timer_start(
            &client->timer, on_idle_timeout, client->server->config.idle_timeout_ms, 0);
        csilk_client_read_start(client);
    } else {
        if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
            csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
        }
    }

    _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);
    csilk_ctx_cleanup(&client->ctx);
}

/* --- Write completion --- */

void
on_write(csilk_io_write_t* req, int status)
{
    if (status < 0) {
        CSILK_LOG_E("Write error: %s", csilk_io_strerror(status));
    }
    csilk_client_t* client = NULL;
    if (req->handle) {
        client = (csilk_client_t*)req->handle->data;
        if (client) {
            csilk_io_timer_stop(&client->write_timer);
        }
    }

    if (req->data) {
        free(req->data);
    }

    if (client && client->ctx.file_fd >= 0) {
        csilk_io_os_fd_t sock_fd;
        if (csilk_io_fileno((const csilk_io_handle_t*)&client->handle, &sock_fd) == 0) {
            csilk_io_fs_t* fs_req = malloc(sizeof(csilk_io_fs_t));
            if (fs_req) {
                fs_req->data = &client->ctx;
                int    fd = client->ctx.file_fd;
                size_t offset = client->ctx.file_offset;
                size_t size = client->ctx.file_size;
                client->ctx.file_fd = -1;

                extern int csilk_io_fs_sendfile(csilk_io_loop_t* loop,
                                                csilk_io_fs_t* req,
                                                csilk_io_os_fd_t sock_fd,
                                                int fd,
                                                size_t offset,
                                                size_t count,
                                                csilk_io_fs_cb cb);
                int r = csilk_io_fs_sendfile(csilk_io_default_loop(),
                                              fs_req,
                                              sock_fd,
                                              fd,
                                              offset,
                                              size,
                                              on_sendfile_complete);
                if (r < 0) {
                    free(fs_req);
                } else {
                    free(req);
                    return;
                }
            }
        }
    }

    free(req);

    if (client) {
        extern void _csilk_check_and_trigger_drain(csilk_client_t* client);
        _csilk_check_and_trigger_drain(client);
    }
}

/* --- Client write --- */

void
csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t len)
{
    if (!client || client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        return;
    }

    assert(len <= INT_MAX);

    if (client->ssl) {
        extern void flush_tls_write(csilk_client_t* client);
        SSL_write(client->ssl, data, (int)len);
        flush_tls_write(client);
        return;
    }

    csilk_io_write_t* req = malloc(sizeof(csilk_io_write_t));
    if (!req) {
        return;
    }

    char* buf_copy = malloc(len);
    if (!buf_copy) {
        free(req);
        return;
    }
    memcpy(buf_copy, data, len);

    csilk_io_buf_t buf = csilk_io_buf_init(buf_copy, (unsigned int)len);
    req->data = buf_copy;
    csilk_io_write(req, (csilk_io_stream_t*)&client->handle, &buf, 1, on_write);
}

/* --- Send data helpers --- */

CSILK_INTERNAL size_t
_csilk_client_get_write_queue_size(csilk_client_t* client)
{
    if (!client) {
        return 0;
    }
#ifndef CSILK_USE_URING
    return ((csilk_io_stream_t*)&client->handle)->write_queue_size;
#else
    return client->pending_write_bytes;
#endif
}

CSILK_INTERNAL void
_csilk_check_and_trigger_drain(csilk_client_t* client)
{
    if (!client) {
        return;
    }
    csilk_ctx_t* c = &client->ctx;
    if (c->write_paused) {
        size_t q = _csilk_client_get_write_queue_size(client);
        if (q <= c->write_low_water_mark) {
            c->write_paused = 0;
            if (c->on_drain) {
                void (*drain_cb)(csilk_ctx_t*, void*) = c->on_drain;
                void* drain_data = c->on_drain_data;
                drain_cb(c, drain_data);
            }
        }
    }
}

CSILK_INTERNAL void
_csilk_send_data(csilk_ctx_t* c, const uint8_t* data, size_t len)
{
    if (!c || c->conn_closed || !c->_internal_client) {
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        return;
    }
    csilk_client_write(client, data, len);
}

CSILK_INTERNAL void
_csilk_send_data_owned(csilk_ctx_t* c, char* data, size_t len)
{
    if (!data) {
        return;
    }
    if (!c || c->conn_closed || !c->_internal_client) {
        free(data);
        return;
    }
    csilk_client_t* client = (csilk_client_t*)c->_internal_client;
    if (client->state == CSILK_CONN_CLOSING || client->state == CSILK_CONN_CLOSED) {
        free(data);
        return;
    }

    if (client->ssl) {
        assert(len <= INT_MAX);
        extern void flush_tls_write(csilk_client_t* client);
        SSL_write(client->ssl, (const uint8_t*)data, (int)len);
        flush_tls_write(client);
        free(data);
        return;
    }

    csilk_io_write_t* req = malloc(sizeof(csilk_io_write_t));
    if (!req) {
        free(data);
        return;
    }

    req->data = data;
    csilk_io_buf_t buf = csilk_io_buf_init(data, (unsigned int)len);
    csilk_io_write(req, (csilk_io_stream_t*)&client->handle, &buf, 1, on_write);
}
```

- [ ] **Step 2: Remove write functions from http1_response.c**

In `src/core/http/http1_response.c`, remove lines 24-280 (on_sendfile_complete through _csilk_send_data_owned).

- [ ] **Step 3: Build and test**

```bash
CCACHE_DISABLE=1 cmake --build build --target csilk_http -j4
CCACHE_DISABLE=1 ctest --test-dir build -R test_connection --timeout 10 --output-on-failure
```

Expected: PASS

- [ ] **Step 4: Commit**

```bash
git add src/core/http/http1_write.c src/core/http/http1_response.c
git commit -m "refactor(http): 🔄 extract write pipeline to http1_write.c"
```

---

## Task 6: Create http1_pipeline.c

**Files:**
- Create: `src/core/http/http1_pipeline.c`
- Modify: `src/core/http/http1_response.c` (remove pipeline functions)

- [ ] **Step 1: Create http1_pipeline.c**

```c
/**
 * @file http1_pipeline.c
 * @brief HTTP/1.1 post-response cleanup and pipeline logic.
 */

#include <assert.h>
#include <llhttp.h>
#include <openssl/ssl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../ctx/ctx_internal.h"
#include "../primitives/header_map.h"
#include "h2.h"
#include "../internal/srv_impl.h"

/* --- Post-response cleanup --- */

CSILK_INTERNAL void
_csilk_handle_post_response(csilk_client_t* client, int keep_alive)
{
    csilk_io_timer_stop(&client->read_timer);

    if (client->server->config.write_timeout_ms > 0) {
        extern void on_write_timeout(csilk_io_timer_t* handle);
        csilk_io_timer_start(
            &client->write_timer, on_write_timeout, client->server->config.write_timeout_ms, 0);
    }

    int   is_ws = client->ctx.is_websocket;
    void* ws_msg_cb = client->ctx.on_ws_message;
    void* ws_send_cb = client->ctx.on_ws_send;

    extern void _csilk_trigger_hooks(csilk_server_t* s, csilk_ctx_t* c, csilk_hook_type_t type);
    extern void csilk_ctx_cleanup(csilk_ctx_t* c);
    _csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_REQUEST_END);

    csilk_ctx_cleanup(&client->ctx);

    if (is_ws) {
        client->ctx.is_websocket = is_ws;
        client->ctx.on_ws_message = ws_msg_cb;
        client->ctx.on_ws_send = ws_send_cb;
    }

    if (client->ctx.is_websocket) {
        return;
    }

    CSILK_LOG_I("_csilk_handle_post_response called, keep_alive=%d", keep_alive);
    if (keep_alive) {
        CSILK_LOG_I("_csilk_handle_post_response: restarting read");
        extern void csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state);
        extern void on_idle_timeout(csilk_io_timer_t* handle);
        extern void csilk_client_read_start(csilk_client_t* client);
        csilk_conn_set_state(client, CSILK_CONN_READING);
        csilk_io_timer_start(
            &client->timer, on_idle_timeout, client->server->config.idle_timeout_ms, 0);
        llhttp_resume(&client->parser);
        csilk_client_read_start(client);
    } else {
        CSILK_LOG_I("_csilk_handle_post_response: closing handle");
        extern void csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state);
        extern void on_close(csilk_io_handle_t* handle);
        csilk_conn_set_state(client, CSILK_CONN_CLOSING);
        if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
            csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
        }
    }
}
```

- [ ] **Step 2: Remove pipeline from http1_response.c**

In `src/core/http/http1_response.c`, remove lines 420-473 (_csilk_handle_post_response).

- [ ] **Step 3: Replace http1_response.c with thin wrapper**

```c
/**
 * @file http1_response.c
 * @brief HTTP/1.1 response module - thin wrapper for backward compatibility.
 *
 * Implementation split across:
 *   http1_serialize.c - response serialization
 *   http1_write.c     - write pipeline
 *   http1_pipeline.c  - post-response cleanup
 */

#include "csilk/core/internal.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
```

- [ ] **Step 4: Build and test**

```bash
CCACHE_DISABLE=1 cmake --build build --target csilk_http -j4
CCACHE_DISABLE=1 ctest --test-dir build -R test_connection --timeout 10 --output-on-failure
```

Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add src/core/http/http1_pipeline.c src/core/http/http1_response.c
git commit -m "refactor(http): 🔄 extract pipeline to http1_pipeline.c"
```

---

## Task 7: Update Headers and CMake

**Files:**
- Modify: `src/core/internal/srv_impl.h`
- Modify: `cmake/sources.cmake`

- [ ] **Step 1: Add declarations to srv_impl.h**

Add after existing declarations:

```c
/* --- HTTP/2 (h2_callbacks.c, h2_session.c, h2_response.c) --- */
CSILK_INTERNAL int  csilk_h2_init_session(csilk_client_t* client);
CSILK_INTERNAL int  csilk_h2_process_data(csilk_client_t* client, const uint8_t* data, size_t len);
CSILK_INTERNAL csilk_ctx_t* csilk_h2_get_or_create_stream(csilk_client_t* client, int32_t stream_id);
CSILK_INTERNAL void csilk_h2_free_streams(csilk_client_t* client);
CSILK_INTERNAL void csilk_h2_send_response(csilk_ctx_t* c);
CSILK_INTERNAL int32_t csilk_h2_submit_push(csilk_ctx_t* c, const char* method, const char* path);

/* --- HTTP/1.1 response (http1_serialize.c, http1_write.c, http1_pipeline.c) --- */
CSILK_INTERNAL const char* get_status_text(int status);
CSILK_INTERNAL void        on_write(csilk_io_write_t* req, int status);
CSILK_INTERNAL void        csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t len);
CSILK_INTERNAL void        _csilk_send_data_owned(csilk_ctx_t* c, char* data, size_t len);
CSILK_INTERNAL int         on_message_begin(llhttp_t* p);
CSILK_INTERNAL int         on_url(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_header_field(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_header_value(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_headers_complete(llhttp_t* p);
CSILK_INTERNAL int         on_body(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int         on_message_complete(llhttp_t* p);
CSILK_INTERNAL void        _csilk_handle_post_response(csilk_client_t* client, int keep_alive);
CSILK_INTERNAL size_t      _csilk_client_get_write_queue_size(csilk_client_t* client);
CSILK_INTERNAL void        _csilk_check_and_trigger_drain(csilk_client_t* client);
```

- [ ] **Step 2: Update CMakeLists**

In `cmake/sources.cmake`, replace:
```cmake
src/core/http/http1_response.c
```
with:
```cmake
src/core/http/http1_serialize.c
src/core/http/http1_write.c
src/core/http/http1_pipeline.c
src/core/http/http1_response.c
src/core/http/h2.c
src/core/http/h2_callbacks.c
src/core/http/h2_session.c
src/core/http/h2_response.c
```

- [ ] **Step 3: Build and run full test suite**

```bash
CCACHE_DISABLE=1 cmake -B build -S .
CCACHE_DISABLE=1 cmake --build build -j$(nproc)
CCACHE_DISABLE=1 ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: 166/166 PASS

- [ ] **Step 4: Format check**

```bash
cmake --build build --target check-format
```

Expected: PASS

- [ ] **Step 5: Final commit**

```bash
git add cmake/sources.cmake src/core/internal/srv_impl.h
git commit -m "build: 📦 add HTTP module split files to build"
```

---

## Task 8: Verify Both Backends

- [ ] **Step 1: Test libuv backend**

```bash
CCACHE_DISABLE=1 ctest --test-dir build -E test_integration --timeout 10 --output-on-failure
```

Expected: 166/166 PASS

- [ ] **Step 2: Test io_uring backend**

```bash
cmake -B build_uring -S . -DCMAKE_BUILD_TYPE=Debug -DCSILK_USE_URING=ON
CCACHE_DISABLE=1 cmake --build build_uring -j$(nproc)
CCACHE_DISABLE=1 ctest --test-dir build_uring -E test_workflow_retry --timeout 20 --output-on-failure
```

Expected: 167/167 PASS (excluding pre-existing test_workflow_retry failure)

- [ ] **Step 3: Verify file sizes**

```bash
wc -l src/core/http/h2*.c src/core/http/http1*.c
```

Expected:
```
   ~20 h2.c (thin wrapper)
  ~250 h2_callbacks.c
  ~200 h2_session.c
  ~250 h2_response.c
   ~10 http1_response.c (thin wrapper)
  ~200 http1_serialize.c
  ~200 http1_write.c
  ~150 http1_pipeline.c
```
