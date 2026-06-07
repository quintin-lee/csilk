/**
 * @file srv_impl.h
 * @brief Internal declarations for server implementation files.
 *
 * This header declares functions that are shared across the split server
 * implementation files (server.c, connection.c, http1.c, tls.c).  These
 * functions were previously file-static in the monolithic server.c but
 * must be visible across translation units after the split.
 * @copyright MIT License
 */

#ifndef SRV_IMPL_H
#define SRV_IMPL_H

#include <llhttp.h>
#include <uv.h>
#include "csilk/csilk.h"
#include "core/srv_internal.h"

/* --- Connection pool & I/O (connection.c) --- */

CSILK_INTERNAL void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
CSILK_INTERNAL void on_close(uv_handle_t* handle);
CSILK_INTERNAL void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
CSILK_INTERNAL void on_new_connection(uv_stream_t* server_stream, int status);
CSILK_INTERNAL void _csilk_worker_init_arena_pool(worker_pool_t* wp);
CSILK_INTERNAL void csilk_arena_flush_free_list(void);
CSILK_INTERNAL void on_idle_timeout(uv_timer_t* handle);
CSILK_INTERNAL void on_read_timeout(uv_timer_t* handle);
CSILK_INTERNAL void on_write_timeout(uv_timer_t* handle);

/* --- HTTP/1.1 callbacks & response (http1.c) --- */

CSILK_INTERNAL void on_write(uv_write_t* req, int status);
CSILK_INTERNAL void _csilk_send_data_owned(csilk_ctx_t* c, char* data, size_t len);
CSILK_INTERNAL int on_message_begin(llhttp_t* p);
CSILK_INTERNAL int on_url(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int on_header_field(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int on_header_value(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int on_headers_complete(llhttp_t* p);
CSILK_INTERNAL int on_body(llhttp_t* p, const char* at, size_t length);
CSILK_INTERNAL int on_message_complete(llhttp_t* p);

/* --- TLS (tls.c) --- */

CSILK_INTERNAL void init_tls(csilk_server_t* s);
CSILK_INTERNAL void cleanup_tls(csilk_server_t* s);
CSILK_INTERNAL int setup_client_tls(csilk_client_t* client);
CSILK_INTERNAL void process_tls_read(csilk_client_t* client);
CSILK_INTERNAL void flush_tls_write(csilk_client_t* client);

#endif /* SRV_IMPL_H */
