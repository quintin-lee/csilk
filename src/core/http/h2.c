/**
 * @file h2.c
 * @brief HTTP/2 integration for csilk.
 *
 * @copyright MIT License
 */

#include "h2.h"
#include "csilk/csilk.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief Called by nghttp2 when a HEADERS frame is received and parsing begins.
 *
 * Triggered at the start of header block processing for any HEADERS frame
 * (request, response, or push-promise). This implementation is a no-op stub
 * that always succeeds; actual header processing is deferred to
 * on_header_callback() for individual header name/value pairs.
 *
 * @param session The nghttp2 session receiving the frame.
 * @param frame   The incoming nghttp2 frame (type will be NGHTTP2_HEADERS
 *                or NGHTTP2_PUSH_PROMISE).
 * @param user_data Opaque pointer set during session creation (csilk_client_t*).
 * @return 0 on success, or a negative nghttp2 error code to abort the session.
 */
static int
on_begin_headers_callback(nghttp2_session* session, const nghttp2_frame* frame, void* user_data)
{
    (void)session;
    (void)frame;
    (void)user_data;
    return 0;
}

/** @brief Called by nghttp2 for each header name/value pair in a HEADERS frame.
 *
 * Triggered once per header during header block decomposition. Pseudo-headers
 * (those starting with ':') are parsed for :method and :path; :path is further
 * split into path and query-string components. All other headers are stored as
 * regular request headers on the per-stream context via csilk_set_request_header().
 *
 * @param session  The nghttp2 session receiving the frame.
 * @param frame    The incoming nghttp2 frame (must be NGHTTP2_HEADERS).
 * @param name     Pointer to the header name (not null-terminated).
 * @param namelen  Length of the header name in bytes.
 * @param value    Pointer to the header value (not null-terminated).
 * @param valuelen Length of the header value in bytes.
 * @param flags    nghttp2 header flags (unused in this callback).
 * @param user_data Opaque pointer (csilk_client_t*).
 * @return 0 on success, or NGHTTP2_ERR_CALLBACK_FAILURE if the per-stream
 *         context could not be created or allocated.
 */
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
                free(query); // query is malloc'd by split_url
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

/** @brief Called by nghttp2 after a complete frame has been received.
 *
 * Triggered after the entire frame (including any header block or data payload)
 * has been fully reassembled and validated. When a HEADERS or DATA frame with
 * the END_STREAM flag is received, the per-stream request context is dispatched
 * for application-level processing via _csilk_dispatch_request().
 *
 * @param session  The nghttp2 session receiving the frame.
 * @param frame    The fully received nghttp2 frame.
 * @param user_data Opaque pointer (csilk_client_t*).
 * @return 0 on success. A non-zero return aborts the nghttp2 session.
 */
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

/** @brief Called by nghttp2 for each chunk of DATA frame payload.
 *
 * Triggered repeatedly as DATA frame payload bytes arrive, allowing the
 * application to accumulate the request body incrementally. Each chunk is
 * appended to the per-stream request body buffer via realloc(). The total
 * accumulated body size is checked against the server's max_body_size
 * configuration to prevent resource exhaustion.
 *
 * @param session   The nghttp2 session receiving the data.
 * @param flags     nghttp2 data flags (unused).
 * @param stream_id The stream ID this data chunk belongs to.
 * @param data      Pointer to the data chunk (not retained after callback).
 * @param len       Length of the data chunk in bytes.
 * @param user_data Opaque pointer (csilk_client_t*).
 * @return 0 on success, or NGHTTP2_ERR_CALLBACK_FAILURE if the body exceeds
 *         the configured limit or memory allocation fails.
 */
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

/** @brief nghttp2 data_provider read callback for streaming response bodies.
 *
 * Called by nghttp2 when it needs more response body data to serialize onto
 * the wire. The response body is served from the per-stream context's
 * response.body buffer, with an internal offset stored via csilk_set/get to
 * track progress across multiple invocations. When all bytes have been
 * transferred, NGHTTP2_DATA_FLAG_EOF is set to signal end-of-stream.
 *
 * @param session    The nghttp2 session sending the response.
 * @param stream_id  The stream ID for which data is being requested.
 * @param buf        Output buffer to copy response data into.
 * @param length     Maximum number of bytes that can be written to buf.
 * @param data_flags Output flags; set NGHTTP2_DATA_FLAG_EOF when all
 *                   response data has been consumed.
 * @param source     The nghttp2 data source (source->ptr points to the
 *                   csilk_ctx_t* for this stream).
 * @param user_data  Opaque pointer (unused).
 * @return Number of bytes written to buf, or 0 if no data is available.
 *         Returning a negative value signals an error to nghttp2.
 */
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

    /* For simplicity in Phase 2, we assume the whole body fits or we track offset.
       Let's add a temporary storage item for offset. */
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

/** @brief Submit an HTTP/2 response for a stream and trigger immediate send.
 *
 * Public API: constructs an nghttp2 response from a csilk_ctx_t's response
 * headers and body. The :status pseudo-header is set from c->response.status
 * (defaulting to 200). All response headers stored in the context's header
 * hash table are appended as nghttp2 name/value pairs. If the response body
 * is non-empty, an nghttp2_data_provider is configured using body_read_callback
 * for streaming. The response is submitted and flushed immediately via
 * nghttp2_session_send().
 *
 * After flushing, the CSILK_HOOK_REQUEST_END hook chain is triggered on the
 * server.
 *
 * @param c The per-stream request context containing response headers, body,
 *          stream_id, and a reference to the internal client.
 *
 * @note Must only be called after the request has been fully processed and
 *       response headers/body have been set. Not thread-safe; must be called
 *       from the event loop thread owning the client connection.
 */
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

    nghttp2_data_provider  prd;
    nghttp2_data_provider* p_prd = nullptr;

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

/** @brief Submit an HTTP/2 server push promise and dispatch the pushed stream.
 *
 * Sends a PUSH_PROMISE frame on the current stream, then creates a new
 * per-stream context for the promised stream ID. The pushed resource's
 * request is routed through the server's router and dispatched; the handler
 * chain populates the response, which csilk_h2_send_response then sends on
 * the promised stream.
 *
 * Pseudo-headers :authority and :scheme are copied from the original request
 * if available, falling back to defaults ("localhost" / "https").
 *
 * @param c      The original request context (client-initiated stream).
 * @param method The HTTP method for the pushed resource.
 * @param path   The path to push (e.g., "/style.css").
 * @return The promised stream ID on success, or a negative nghttp2 error code.
 */
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

    /* Submit the PUSH_PROMISE. nghttp2 returns the new stream ID on success.
     * The new stream is in "reserved" state — we can immediately submit a
     * response to it. */
    int32_t promised_id =
        nghttp2_submit_push_promise(client->h2_session,
                                    NGHTTP2_FLAG_NONE,
                                    c->stream_id,
                                    push_headers,
                                    sizeof(push_headers) / sizeof(push_headers[0]),
                                    nullptr);

    if (promised_id < 0) {
        return promised_id;
    }

    /* Create a request context for the promised stream */
    csilk_ctx_t* pushed_c = csilk_h2_get_or_create_stream(client, promised_id);
    if (!pushed_c) {
        return -1;
    }

    /* Set up the synthesized request */
    pushed_c->request.method = csilk_arena_strdup(pushed_c->arena, method);
    char* path_heap = nullptr;
    char* query_heap = nullptr;
    csilk_split_url(path, &path_heap, &query_heap);
    pushed_c->request.path = path_heap;
    if (query_heap) {
        csilk_parse_query(pushed_c, query_heap);
        free(query_heap);
    }

    /* Increment push counter on the original context */
    csilk_set(c, "_h2_push_count", (void*)(uintptr_t)(push_count + 1));

    /* Dispatch the pushed request through the router.
     * The handler chain will set pushed_c->response, and the response
     * will be sent by _csilk_send_response → csilk_h2_send_response
     * on the promised stream ID. */
    _csilk_dispatch_request(pushed_c);

    /* Flush everything: PUSH_PROMISE and the pushed response */
    int rv = nghttp2_session_send(client->h2_session);

    return promised_id;
}

/** @brief Called by nghttp2 when a stream is closed.
 *
 * Triggered when either peer closes a stream (RST_STREAM, END_STREAM
 * processed, or error). Removes the associated csilk_ctx_t from the client's
 * linked list of active streams, cleans up the context via csilk_ctx_cleanup(),
 * frees the arena allocator, and deallocates the context struct itself.
 *
 * @param session    The nghttp2 session closing the stream.
 * @param stream_id  The ID of the stream being closed.
 * @param error_code nghttp2 error code indicating why the stream closed
 *                   (e.g. NGHTTP2_NO_ERROR).
 * @param user_data  Opaque pointer (csilk_client_t*).
 * @return 0 on success (return value ignored by nghttp2 for this callback).
 */
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
                found->arena = nullptr;
            }

            free(found);
            return 0;
        }
        curr = &((*curr)->next_stream);
    }

    return 0;
}

/** @brief nghttp2 send callback for writing serialized frames to the client.
 *
 * Called by nghttp2 when it has serialized HTTP/2 frames ready to transmit
 * over the wire. Delegates directly to csilk_client_write() to write the raw
 * bytes to the client's connection.
 *
 * @param session   The nghttp2 session producing the data.
 * @param data      Pointer to the serialized frame bytes to send.
 * @param length    Number of bytes to send.
 * @param flags     nghttp2 send flags (unused).
 * @param user_data Opaque pointer (csilk_client_t*).
 * @return The number of bytes written (always equals @p length on success).
 *         Returning a negative value signals a write error to nghttp2.
 */
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

/** @brief Look up an existing per-stream context, or create a new one.
 *
 * Public API: searches the client's h2_streams linked list for a context
 * matching the given @p stream_id. If found, returns the existing context.
 * If not found, allocates a new csilk_ctx_t, initializes it via
 * _csilk_ctx_init(), assigns it a fresh arena allocator, records the
 * stream_id, and prepends it to the client's stream list.
 *
 * This function is called from multiple nghttp2 callbacks (on_header_callback,
 * on_frame_recv_callback, on_data_chunk_recv_callback) and must handle
 * concurrent creation for different streams within the same client session.
 *
 * @param client    The client connection owning the stream list.
 * @param stream_id The HTTP/2 stream identifier to look up or create.
 * @return Pointer to the csilk_ctx_t for the given stream, or nullptr if memory
 *         allocation failed.
 *
 * @note The returned context is owned by the client's stream list and will
 *       be freed in on_stream_close_callback or csilk_h2_free_streams().
 */
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
        return nullptr;
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

void
csilk_h2_free_streams(csilk_client_t* client)
{
    csilk_ctx_t* curr = client->h2_streams;
    while (curr) {
        csilk_ctx_t* next = curr->next_stream;
        csilk_ctx_cleanup(curr);
        if (curr->arena) {
            csilk_arena_free(curr->arena);
            curr->arena = nullptr;
        }
        free(curr);
        curr = next;
    }
    client->h2_streams = nullptr;
}

/** @brief Initialize an HTTP/2 server session for a client connection.
 *
 * Public API: creates an nghttp2 server session bound to the given client.
 * Registers all internal nghttp2 callbacks (send, on_frame_recv,
 * on_data_chunk_recv, on_stream_close, on_header, on_begin_headers).
 * Submits initial SETTINGS frames (MAX_CONCURRENT_STREAMS = 100) and
 * immediately flushes the settings to the client.
 *
 * The session is stored in client->h2_session. All subsequent HTTP/2 data
 * received from this client should be fed to csilk_h2_process_data().
 *
 * @param client The client connection to initialize an H2 session for.
 *               Must have a valid client->server pointer.
 * @return 0 on success, or -1 if callback allocation, session creation, or
 *         settings submission fails.
 *
 * @note Must be called once after the HTTP/2 preface (PRI * HTTP/2.0) has
 *       been received from the client.
 */
int
csilk_h2_init_session(csilk_client_t* client)
{
    nghttp2_session_callbacks* callbacks;
    if (nghttp2_session_callbacks_new(&callbacks) != 0) {
        return -1;
    }

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

/** @brief Feed incoming HTTP/2 data to the nghttp2 session for processing.
 *
 * Public API: passes raw bytes received from the client to nghttp2's
 * session_mem_recv() for protocol parsing and callback dispatch. After
 * processing, calls nghttp2_session_send() to flush any pending outgoing
 * frames (e.g., SETTINGS ACK, HEADERS, DATA, RST_STREAM).
 *
 * @param client The client connection owning the nghttp2 session.
 * @param data   Pointer to the raw bytes received from the client.
 * @param len    Number of bytes to process.
 * @return 0 on success, or -1 if nghttp2 session processing fails or
 *         frame sending fails. On failure the session should be considered
 *         corrupted and the connection should be closed.
 *
 * @note Must be called from the event loop that owns the client connection.
 * @note data must remain valid for the duration of the call; nghttp2 may
 *       access it during callback dispatch.
 */
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
