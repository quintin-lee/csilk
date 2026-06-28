/**
 * @file uring_connection.c
 * @brief Connection pool, accept, I/O, timers, and lifecycle callbacks using io_uring.
 */

#include <openssl/ssl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <liburing.h>
#include <llhttp.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "csilk/core/sys_io.h"
#include "core/srv_internal.h"
#include "core/ctx_internal.h"
#include "../h2.h"
#include "../srv_impl.h"
#include "uring_internal.h"

void csilk_client_close(csilk_client_t* client);

/* --- Connection pool (per-worker, lock-free) --- */

static csilk_client_t*
pool_get(worker_pool_t* wp)
{
	csilk_client_t* client;
	if (wp->client_pool_count > 0) {
		client = wp->client_pool[--wp->client_pool_count];
	} else {
		client = calloc(1, sizeof(csilk_client_t));
	}
	if (client) {
		client->ctx.file_fd = -1;
	}
	return client;
}

static void
pool_put(worker_pool_t* wp, csilk_client_t* client)
{
	if (client->ssl) {
		SSL_free(client->ssl);
		client->ssl = nullptr;
		client->read_bio = nullptr;
		client->write_bio = nullptr;
	}
	if (client->h2_session) {
		nghttp2_session_del(client->h2_session);
		client->h2_session = nullptr;
	}
	csilk_h2_free_streams(client);
	memset(client, 0, sizeof(*client));
	if (wp->client_pool_count < CSILK_CLIENT_POOL_SIZE) {
		wp->client_pool[wp->client_pool_count++] = client;
	} else {
		free(client);
	}
}

static csilk_arena_t*
pool_get_arena(worker_pool_t* wp)
{
	csilk_arena_t* arena;
	if (wp->arena_pool_count > 0) {
		arena = wp->arena_pool[--wp->arena_pool_count];
	} else {
		arena = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
		if (arena && wp->server->config.enable_arena_alignment) {
			csilk_arena_set_alignment(arena, 1);
		}
	}
	return arena;
}

static void
pool_put_arena(worker_pool_t* wp, csilk_arena_t* arena)
{
	csilk_arena_reset(arena);
	if (wp->arena_pool_count < CSILK_CLIENT_POOL_SIZE) {
		wp->arena_pool[wp->arena_pool_count++] = arena;
	} else {
		csilk_arena_free(arena);
	}
}

void
_csilk_worker_init_arena_pool(worker_pool_t* wp)
{
	int align = wp->server->config.enable_arena_alignment;
	for (int i = 0; i < CSILK_CLIENT_POOL_SIZE; i++) {
		csilk_arena_t* a = csilk_arena_new(CSILK_DEFAULT_ARENA_SIZE);
		if (!a) {
			break;
		}
		if (align) {
			csilk_arena_set_alignment(a, 1);
		}
		void* p = csilk_arena_alloc(a, 1);
		if (!p) {
			csilk_arena_free(a);
			break;
		}
		csilk_arena_reset(a);
		wp->arena_pool[wp->arena_pool_count++] = a;
	}
}

static void
client_list_add(csilk_server_t* server, csilk_client_t* client)
{
	csilk_mutex_lock(&server->clients_mutex);
	client->next = server->active_clients;
	client->prev = nullptr;
	if (server->active_clients) {
		server->active_clients->prev = client;
	}
	server->active_clients = client;
	csilk_mutex_unlock(&server->clients_mutex);
}

static void
client_list_remove_internal(csilk_server_t* server, csilk_client_t* client)
{
	if (client->prev) {
		client->prev->next = client->next;
	} else if (server->active_clients == client) {
		server->active_clients = client->next;
	}
	if (client->next) {
		client->next->prev = client->prev;
	}
	client->next = client->prev = nullptr;
}

static void
client_list_remove(csilk_server_t* server, csilk_client_t* client)
{
	csilk_mutex_lock(&server->clients_mutex);
	client_list_remove_internal(server, client);
	csilk_mutex_unlock(&server->clients_mutex);
}

static void
client_destroy(csilk_client_t* client)
{
	if (client->server) {
		atomic_fetch_sub(&client->server->active_connections, 1);
	}
	csilk_ctx_cleanup(&client->ctx);
	if (client->ctx.arena) {
		pool_put_arena(client->owner_pool, client->ctx.arena);
	}
	pool_put(client->owner_pool, client);
}

CSILK_INTERNAL csilk_io_loop_t*
_csilk_ctx_loop(csilk_ctx_t* c)
{
	if (!c || !c->server || !c->_internal_client) {
		return NULL;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	return &client->owner_pool->loop;
}

CSILK_INTERNAL void
_csilk_ctx_async_ref_incr(csilk_ctx_t* c)
{
	if (!c || !c->server || !c->_internal_client) {
		return;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	client->async_ref++;
}

CSILK_INTERNAL void
_csilk_ctx_async_ref_decr(csilk_ctx_t* c)
{
	if (!c || !c->server || !c->_internal_client) {
		return;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	client->async_ref--;
	if (client->async_ref <= 0 && client->close_pending <= 0 && c->conn_closed) {
		client_destroy(client);
	}
}

/* --- I/O Operations --- */

static void
submit_timer(csilk_client_t* client, csilk_io_timer_t* tmr, uint64_t timeout_ms)
{
	struct io_uring* ring = &client->owner_pool->loop;
	struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		return;
	}

	if (tmr->data) {
		free(tmr->data);
	}
	struct __kernel_timespec* ts = malloc(sizeof(struct __kernel_timespec));
	ts->tv_sec = timeout_ms / 1000;
	ts->tv_nsec = (timeout_ms % 1000) * 1000000;
	tmr->data = ts;

	io_uring_prep_timeout(sqe, ts, 0, 0);
	io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_TIMEOUT, client));
}

void
csilk_client_read_start(csilk_client_t* client)
{
	struct io_uring* ring = &client->owner_pool->loop;
	struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		return;
	}

	size_t suggested_size = 65536;
	char* buf = malloc(suggested_size);
	client->ctx.work_req.data = buf;

	io_uring_prep_recv(sqe, client->handle.fd, buf, suggested_size, 0);
	io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_READ, client));
}

void
csilk_client_write(csilk_client_t* client, const uint8_t* data, size_t length)
{
	if (!client || client->ctx.conn_closed) {
		return;
	}

	struct io_uring* ring = &client->owner_pool->loop;
	struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		return;
	}

	io_uring_prep_send(sqe, client->handle.fd, data, length, 0);
	io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_WRITE, client));
	io_uring_submit(ring);
}

void
csilk_client_close(csilk_client_t* client)
{
	if (!client || client->ctx.conn_closed) {
		return;
	}

	CSILK_LOG_D("Connection: closed (client pointer: %p)", (void*)client);
	_csilk_trigger_hooks(client->server, &client->ctx, CSILK_HOOK_CONN_CLOSE);
	client_list_remove(client->server, client);
	client->ctx.conn_closed = 1;

	if (client->timer.data) {
		free(client->timer.data);
		client->timer.data = NULL;
	}
	if (client->read_timer.data) {
		free(client->read_timer.data);
		client->read_timer.data = NULL;
	}
	if (client->write_timer.data) {
		free(client->write_timer.data);
		client->write_timer.data = NULL;
	}
	if (client->request_timer.data) {
		free(client->request_timer.data);
		client->request_timer.data = NULL;
	}

	struct io_uring* ring = &client->owner_pool->loop;

	// Cancel pending reads/writes/timeouts
	uring_op_type_t ops_to_cancel[] = {URING_OP_READ, URING_OP_WRITE, URING_OP_TIMEOUT};
	for (int i = 0; i < 3; i++) {
		struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
		if (sqe) {
			io_uring_prep_cancel(
			    sqe, (void*)uring_encode_data(ops_to_cancel[i], client), 0);
			io_uring_sqe_set_data(sqe,
					      (void*)uring_encode_data(URING_OP_CLOSE, client));
		}
	}

	if (client->handle.fd >= 0) {
		struct io_uring_sqe* close_sqe = io_uring_get_sqe(ring);
		if (close_sqe) {
			io_uring_prep_close(close_sqe, client->handle.fd);
			io_uring_sqe_set_data(close_sqe,
					      (void*)uring_encode_data(URING_OP_CLOSE, client));
		} else {
			close(client->handle.fd);
		}
		client->handle.fd = -1;
	}

	io_uring_submit(ring);

	if (client->async_ref <= 0) {
		client_destroy(client);
	}
}

void
on_timeout(csilk_client_t* client)
{
	if (!client->ctx.conn_closed) {
		CSILK_LOG_D("Connection: closing connection due to timeout");
		csilk_client_close(client);
	}
}

void
on_new_connection(worker_pool_t* wp, int client_fd)
{
	csilk_server_t* server = wp->server;

	int max_conn = server->config.max_connections;
	if (max_conn == 0) {
		max_conn = server->max_connections;
	}
	if (max_conn > 0 && atomic_load(&server->active_connections) >= max_conn) {
		close(client_fd);
		return;
	}

	csilk_client_t* client = pool_get(wp);
	if (!client) {
		close(client_fd);
		return;
	}

	client->server = server;
	client->owner_pool = wp;
	client->handle.fd = client_fd;
	client->handle.data = client;

	_csilk_ctx_init(&client->ctx, server, client);
	client->ctx.arena = pool_get_arena(wp);

	client_list_add(server, client);

	CSILK_LOG_D("Connection: accepted new TCP connection (client pointer: %p)", (void*)client);

	atomic_fetch_add(&server->active_connections, 1);
	client->protocol = CSILK_PROTO_HTTP1;
	llhttp_init(&client->parser, HTTP_REQUEST, &server->settings);
	client->parser.data = client;

	_csilk_trigger_hooks(server, &client->ctx, CSILK_HOOK_CONN_OPEN);

	if (server->ssl_ctx) {
		CSILK_LOG_D("Connection: setting up TLS for connection: %p", (void*)client);
		if (setup_client_tls(client) < 0) {
			csilk_client_close(client);
			return;
		}
	}

	CSILK_LOG_T("Connection: connection timers initialized, starting read listener");
	if (server->config.read_timeout_ms > 0) {
		submit_timer(client, &client->read_timer, server->config.read_timeout_ms);
	}
	if (server->config.request_timeout_ms > 0) {
		submit_timer(client, &client->request_timer, server->config.request_timeout_ms);
	}

	if (!client->read_paused) {
		csilk_client_read_start(client);
	}

	struct io_uring* ring = &client->owner_pool->loop;
	io_uring_submit(ring);
}

void
on_read(csilk_client_t* client, ssize_t nread)
{
	char* base = client->ctx.work_req.data;
	client->ctx.work_req.data = NULL;
	int is_registered = 0;

	if (client->server->config.read_timeout_ms > 0) {
		submit_timer(client, &client->read_timer, client->server->config.read_timeout_ms);
	}

	if (nread > 0) {
		if (client->ssl) {
			BIO_write(client->read_bio, base, (int)nread);
			process_tls_read(client);
		} else if (client->ctx.is_websocket) {
			csilk_ws_parse_frame(&client->ctx, (const uint8_t*)base, (size_t)nread);
		} else {
			if (client->ctx.read_buffers_count < 16) {
				client->ctx.read_buffers[client->ctx.read_buffers_count++] = base;
				is_registered = 1;
			} else {
				CSILK_LOG_W("Connection: read_buffers capacity exceeded, freeing "
					    "immediately");
				free(base);
				base = NULL;
			}

			if (base) {
				enum llhttp_errno err =
				    llhttp_execute(&client->parser, base, nread);
				if (err == HPE_CLOSED_CONNECTION) {
					llhttp_init(&client->parser,
						    HTTP_REQUEST,
						    &client->server->settings);
					client->parser.data = client;
				} else if (err != HPE_OK && err != HPE_PAUSED_UPGRADE) {
					CSILK_LOG_E("Connection: HTTP parse error: %s %s",
						    llhttp_errno_name(err),
						    client->parser.reason ? client->parser.reason
									  : "unknown reason");
					csilk_client_close(client);
				}
			}
		}
	} else if (nread < 0) {
		if (nread != -ECONNRESET) {
			CSILK_LOG_E("Connection: read error %zd", nread);
		}
		csilk_client_close(client);
	} else {
		csilk_client_close(client);
	}

	if (base && !is_registered) {
		free(base);
	}

	if (!client->ctx.conn_closed && nread > 0) {
		if (!client->read_paused) {
			csilk_client_read_start(client);
		}
	}

	struct io_uring* ring = &client->owner_pool->loop;
	io_uring_submit(ring);
}

const char*
csilk_get_client_ip(csilk_ctx_t* c)
{
	if (!c || !c->_internal_client) {
		return nullptr;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	struct sockaddr_storage addr;
	socklen_t len = sizeof(addr);
	if (getpeername(client->handle.fd, (struct sockaddr*)&addr, &len) == 0) {
		char ip[46];
		if (addr.ss_family == AF_INET) {
			inet_ntop(AF_INET, &((struct sockaddr_in*)&addr)->sin_addr, ip, sizeof(ip));
		} else {
			inet_ntop(
			    AF_INET6, &((struct sockaddr_in6*)&addr)->sin6_addr, ip, sizeof(ip));
		}
		return csilk_arena_strdup(c->arena, ip);
	}
	return nullptr;
}

void
csilk_client_read_stop(csilk_client_t* client)
{
	client->read_paused = 1;
}
