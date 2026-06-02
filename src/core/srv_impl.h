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

void alloc_buffer(uv_handle_t* handle, size_t suggested_size, uv_buf_t* buf);
void on_close(uv_handle_t* handle);
void on_read(uv_stream_t* stream, ssize_t nread, const uv_buf_t* buf);
void on_new_connection(uv_stream_t* server_stream, int status);
void on_idle_timeout(uv_timer_t* handle);
void on_read_timeout(uv_timer_t* handle);
void on_write_timeout(uv_timer_t* handle);

/* --- HTTP/1.1 callbacks & response (http1.c) --- */

void on_write(uv_write_t* req, int status);
int on_message_begin(llhttp_t* p);
int on_url(llhttp_t* p, const char* at, size_t length);
int on_header_field(llhttp_t* p, const char* at, size_t length);
int on_header_value(llhttp_t* p, const char* at, size_t length);
int on_headers_complete(llhttp_t* p);
int on_body(llhttp_t* p, const char* at, size_t length);
int on_message_complete(llhttp_t* p);

/* --- TLS (tls.c) --- */

void init_tls(csilk_server_t* s);
void cleanup_tls(csilk_server_t* s);
int setup_client_tls(csilk_client_t* client);
void process_tls_read(csilk_client_t* client);
void flush_tls_write(csilk_client_t* client);

#endif /* SRV_IMPL_H */
