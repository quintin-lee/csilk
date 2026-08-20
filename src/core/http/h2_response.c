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
    extern ssize_t body_read_callback(
        nghttp2_session*, int32_t, uint8_t*, size_t, uint32_t*, nghttp2_data_source*, void*);

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

    if (!_csilk_server_get_h2_push_enable(server)) {
        return -1;
    }

    /* Enforce per-request push limit */
    int   max_push = _csilk_server_get_h2_max_push(server);
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
    extern csilk_ctx_t* csilk_h2_get_or_create_stream(csilk_client_t * client, int32_t stream_id);
    csilk_ctx_t*        pushed_c = csilk_h2_get_or_create_stream(client, promised_id);
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
    extern void _csilk_dispatch_request(csilk_ctx_t * c);
    _csilk_dispatch_request(pushed_c);

    /* Flush everything */
    int rv = nghttp2_session_send(client->h2_session);

    return promised_id;
}
