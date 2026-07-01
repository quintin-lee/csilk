/**
 * @file uring_server.c
 * @brief Server lifecycle — create, configure, run, stop, free.
 *
 * Implements the server's public API: creation, configuration, driver
 * injection, hook registration, graceful shutdown, and the main event
 * loop (with optional multi-worker SO_REUSEPORT mode).
 *
 * This version uses io_uring instead of libuv.
 * @copyright MIT License
 */

#include <limits.h>
#include <llhttp.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <liburing.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <poll.h>
#include <pthread.h>
#ifndef _WIN32
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#endif

#include "core/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "core/srv_internal.h"
#include "../srv_impl.h"
#include "uring_internal.h"

typedef struct {
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int count;
	int waiting;
} csilk_barrier_t;

static void
barrier_init(csilk_barrier_t* b, int count)
{
	pthread_mutex_init(&b->mutex, NULL);
	pthread_cond_init(&b->cond, NULL);
	b->count = count;
	b->waiting = 0;
}
static void
barrier_wait(csilk_barrier_t* b)
{
	pthread_mutex_lock(&b->mutex);
	b->waiting++;
	if (b->waiting >= b->count) {
		pthread_cond_broadcast(&b->cond);
	} else {
		while (b->waiting < b->count) {
			pthread_cond_wait(&b->cond, &b->mutex);
		}
	}
	pthread_mutex_unlock(&b->mutex);
}
static void
barrier_destroy(csilk_barrier_t* b)
{
	pthread_mutex_destroy(&b->mutex);
	pthread_cond_destroy(&b->cond);
}

/* --- Signal handler --- */

static void
on_signal(csilk_server_t* server)
{
	csilk_server_stop(server);
}

/* --- Graceful shutdown --- */

static int
close_active_clients(csilk_server_t* server, struct io_uring* loop)
{
	int count = 0;
	for (int w = 0; w < server->worker_pool_count; w++) {
		worker_pool_t* wp = &server->worker_pools[w];
		csilk_client_t* client = wp->active_clients;
		while (client) {
			csilk_client_t* next = client->next;
			if (client->ctx.is_websocket) {
				csilk_ws_close(&client->ctx, 1001, "Server stopping");
			} else if (client->ctx.is_sse) {
				csilk_sse_send(&client->ctx, "close", "Server stopping");
				csilk_sse_close(&client->ctx);
			}
			close(client->handle.fd);
			client = next;
			count++;
		}
		wp->active_clients = NULL;
	}
	return count;
}

static void
on_stop_async(csilk_io_async_t* handle)
{
	csilk_server_t* server = (csilk_server_t*)handle->data;
	CSILK_LOG_I("Server: initiating graceful shutdown");

	_csilk_trigger_hooks(server, nullptr, CSILK_HOOK_SERVER_STOP);

	CSILK_LOG_D("Server: closing server socket listener");
	close(server->server_handle.fd);

	{
		int n = close_active_clients(server, server->loop);
		if (n > 0) {
			CSILK_LOG_I("Server: closed %d active client connection(s)", n);
		}
	}

	close(server->sig_handle.signal_fd);
	close(server->worker_pools[0].dispatch_async.event_fd);
	close(server->async_handle.event_fd);

	for (int i = 1; i < server->worker_pool_count; i++) {
		CSILK_LOG_D("Server: signaling worker thread %d to stop", i);
		uint64_t val = 1;
		if (write(server->worker_pools[i].stop_async.event_fd, &val, sizeof(val)) < 0) {
			// ignore
		}
	}

	if (server->mq) {
		CSILK_LOG_D("Server: freeing message queue");
		_csilk_mq_free(server->mq);
		server->mq = nullptr;
	}
}

/* --- Server creation --- */

#include "csilk/reflection/reflect.h"

csilk_server_t*
csilk_server_new(csilk_router_t* router)
{
	csilk_reflect_init();
	csilk_arena_init();
	csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
	if (!s) {
		return nullptr;
	}
	s->loop = malloc(sizeof(csilk_io_loop_t));
	if (!s->loop) {
		free(s);
		return nullptr;
	}
	int uring_flags = IORING_SETUP_SQPOLL;
	int ret = io_uring_queue_init(4096, s->loop, uring_flags);
	if (ret < 0) {
		/* SQPOLL may require CAP_SYS_NICE or /proc/sys/kernel/io_uring_sqpoll_cred_limit=0.
		 * Fall back to non-polling mode. */
		CSILK_LOG_W("io_uring SQPOLL not available (ret=%d), falling back to non-polling",
			    ret);
		ret = io_uring_queue_init(4096, s->loop, 0);
		if (ret < 0) {
			free(s->loop);
			free(s);
			return nullptr;
		}
	}
	CSILK_LOG_I("io_uring initialized (depth=4096, flags=0x%x)", uring_flags);

	s->router = router;
	llhttp_settings_init(&s->settings);
	s->settings.on_message_begin = on_message_begin;
	s->settings.on_url = on_url;
	s->settings.on_header_field = on_header_field;
	s->settings.on_header_value = on_header_value;
	s->settings.on_headers_complete = on_headers_complete;
	s->settings.on_body = on_body;
	s->settings.on_message_complete = on_message_complete;

	s->config.idle_timeout_ms = CSILK_DEFAULT_IDLE_TIMEOUT;
	s->config.max_body_size = CSILK_DEFAULT_MAX_BODY_SIZE;
	s->server_handle.fd = -1;
	s->async_handle.event_fd = -1;
	s->sig_handle.signal_fd = -1;
	s->config.max_header_size = CSILK_DEFAULT_MAX_HEADER_SIZE;
	s->config.listen_backlog = CSILK_DEFAULT_LISTEN_BACKLOG;

	// _csilk_mq_new uses loop implicitly internally if needed? Let's assume it handles io_uring loop.
	s->mq = _csilk_mq_new(s->loop);

	return s;
}

/* --- SPA fallback --- */

static void
spa_fallback_handler(csilk_ctx_t* c)
{
	const char* method = csilk_get_method(c);
	if (!method || strcmp(method, "GET") != 0) {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	if (!client || !client->server || !client->server->spa_doc_root) {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/index.html", client->server->spa_doc_root);

	FILE* f = fopen(path, "rb");
	if (!f) {
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	fseek(f, 0, SEEK_END);
	long fsize = ftell(f);
	if (fsize <= 0) {
		fclose(f);
		csilk_string(c, CSILK_STATUS_NOT_FOUND, "Not Found");
		return;
	}
	rewind(f);
	char* body = malloc((size_t)fsize + 1);
	if (!body) {
		fclose(f);
		csilk_string(c, CSILK_STATUS_INTERNAL_SERVER_ERROR, "");
		return;
	}
	size_t nread = fread(body, 1, (size_t)fsize, f);
	fclose(f);
	body[nread] = '\0';

	csilk_set_header(c, "Content-Type", "text/html");
	c->response.body = body;
	c->response.body_len = nread;
	c->response.body_is_managed = 1;
	csilk_status(c, CSILK_STATUS_OK);
}

/* --- Server configuration --- */

void
csilk_server_set_not_found_handler(csilk_server_t* server, csilk_handler_t handler)
{
	if (!server) {
		return;
	}
	server->not_found_handler = handler;
}

void
csilk_server_set_spa_fallback(csilk_server_t* server, const char* doc_root)
{
	if (!server || !doc_root) {
		return;
	}
	free(server->spa_doc_root);
	server->spa_doc_root = strdup(doc_root);
	if (server->spa_doc_root) {
		server->not_found_handler = spa_fallback_handler;
	}
}

int
csilk_server_use(csilk_server_t* server, csilk_handler_t handler)
{
	if (!server || !handler) {
		return -1;
	}
	if (server->middleware_count >= 32) {
		CSILK_LOG_E("Server: global middleware limit (32) reached. Middleware dropped.");
		return -1;
	}
	server->middlewares[server->middleware_count++] = handler;
	CSILK_LOG_D("Server: registered global middleware %p (count: %d)",
		    (void*)handler,
		    server->middleware_count);
	return 0;
}

/* --- Server free --- */

void
csilk_server_free(csilk_server_t* server)
{
	if (!server) {
		return;
	}

	if (server->worker_tids) {
		for (int i = 0; i < server->worker_count; i++) {
			pthread_join((pthread_t)server->worker_tids[i], NULL);
		}
		free(server->worker_tids);
		server->worker_tids = nullptr;
	}

	free(server->spa_doc_root);
	if (server->worker_pools) {
		for (int w = 0; w < server->worker_pool_count; w++) {
			worker_pool_t* wp = &server->worker_pools[w];
			if (wp->thread_pool) {
				uring_tp_destroy((uring_thread_pool_t*)wp->thread_pool);
				wp->thread_pool = NULL;
			}
			for (int i = 0; i < wp->client_pool_count; i++) {
				free(wp->client_pool[i]);
			}
			for (int i = 0; i < wp->arena_pool_count; i++) {
				csilk_arena_free(wp->arena_pool[i]);
			}
		}
		free(server->worker_pools);
	}

	cleanup_tls(server);

	if (server->mq) {
		_csilk_mq_free(server->mq);
	}

	for (int i = 0; i < CSILK_HOOK_COUNT; i++) {
		csilk_hook_node_t* curr = server->hooks[i];
		while (curr) {
			csilk_hook_node_t* next = curr->next;
			free(curr);
			curr = next;
		}
	}

	csilk_arena_flush_free_list();

	if (server->loop) {
		io_uring_queue_exit(server->loop);
		free(server->loop);
	}

	free(server);
}

/* --- Server stop --- */

void
csilk_server_stop(csilk_server_t* server)
{
	if (!server) {
		return;
	}
	uint64_t val = 1;
	if (write(server->async_handle.event_fd, &val, sizeof(val)) < 0) {
		// ignore error
	}
}

/* --- Server stats --- */

void
csilk_server_get_stats(csilk_server_t* server, int* active_conn, int* pooled_conn)
{
	if (!server) {
		return;
	}
	if (active_conn) {
		*active_conn = atomic_load(&server->active_connections);
	}
	if (pooled_conn) {
		int total = 0;
		for (int w = 0; w < server->worker_pool_count; w++) {
			total += server->worker_pools[w].client_pool_count;
		}
		*pooled_conn = total;
	}
}

/* --- Server config --- */

void
csilk_server_set_config(csilk_server_t* server, const csilk_server_config_t* config)
{
	if (!server || !config) {
		return;
	}

	csilk_server_config_t old = server->config;

	server->config = *config;

	if (server->config.idle_timeout_ms == 0) {
		server->config.idle_timeout_ms =
		    old.idle_timeout_ms ? old.idle_timeout_ms : CSILK_DEFAULT_IDLE_TIMEOUT;
	}
	if (server->config.max_body_size == 0) {
		server->config.max_body_size =
		    old.max_body_size ? old.max_body_size : CSILK_DEFAULT_MAX_BODY_SIZE;
	}
	if (server->config.max_header_size == 0) {
		server->config.max_header_size =
		    old.max_header_size ? old.max_header_size : CSILK_DEFAULT_MAX_HEADER_SIZE;
	}
	if (server->config.listen_backlog == 0) {
		server->config.listen_backlog =
		    old.listen_backlog ? old.listen_backlog : CSILK_DEFAULT_LISTEN_BACKLOG;
	}
}

/* --- Max connections --- */

int
csilk_server_set_max_connections(csilk_server_t* server, int max)
{
	if (!server) {
		return -1;
	}
	int prev = server->max_connections;
	server->max_connections = max;
	return prev;
}

/* --- Driver injection --- */

void
csilk_server_set_storage_driver(csilk_server_t* server, csilk_storage_driver_t* driver)
{
	if (server) {
		server->storage_driver = driver;
	}
}

void
csilk_server_set_crypto_driver(csilk_server_t* server, csilk_crypto_driver_t* driver)
{
	if (server) {
		server->crypto_driver = driver;
	}
}

void
csilk_server_set_cipher_driver(csilk_server_t* server, csilk_cipher_driver_t* driver)
{
	if (server) {
		server->cipher_driver = driver;
	}
}

/* --- Worker thread internals --- */

static int bind_and_listen(
    csilk_io_loop_t* loop, csilk_io_tcp_t* out_handle, int port, int backlog, bool reuseport);

typedef struct {
	worker_pool_t* wp;
	int port;
	csilk_barrier_t* barrier;
} worker_data_t;

typedef struct {
	csilk_io_loop_t* loop;
	csilk_io_tcp_t* listen_handle;
	csilk_server_t* server;
	int worker_index;
} worker_stop_data_t;

static void
on_worker_stop_async(csilk_io_async_t* handle)
{
	worker_stop_data_t* sd = (worker_stop_data_t*)handle->data;
	if (!sd) {
		return;
	}

	csilk_server_t* server = sd->server;
	csilk_io_loop_t* loop = sd->loop;

	close(sd->listen_handle->fd);

	close_active_clients(server, loop);

	int worker_idx = sd->worker_index;
	close(server->worker_pools[worker_idx].dispatch_async.event_fd);
	close(handle->event_fd);
}

static void
on_dispatch_async(csilk_io_async_t* handle)
{
	worker_pool_t* wp = (worker_pool_t*)handle->data;
	if (!wp) {
		return;
	}

	csilk_lfq_node_t* node = csilk_lfq_dequeue(&wp->dispatch_queue);
	while (node) {
		csilk_dispatch_task_t* task = (csilk_dispatch_task_t*)node;
		if (task->cb) {
			task->cb(task->arg);
		}
		free(task);
		node = csilk_lfq_dequeue(&wp->dispatch_queue);
	}
}

static void
_csilk_worker_init_dispatch(worker_pool_t* wp, csilk_io_loop_t* loop)
{
	csilk_lfq_init(&wp->dispatch_queue);
	wp->dispatch_async.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	wp->dispatch_async.data = wp;
}

void
csilk_dispatch(csilk_ctx_t* c, void (*cb)(void* arg), void* arg)
{
	if (!c || !c->_internal_client || !cb) {
		return;
	}
	csilk_client_t* client = (csilk_client_t*)c->_internal_client;
	if (!client->owner_pool) {
		return;
	}
	worker_pool_t* wp = client->owner_pool;

	csilk_dispatch_task_t* task = malloc(sizeof(csilk_dispatch_task_t));
	if (!task) {
		return;
	}
	task->cb = cb;
	task->arg = arg;
	csilk_lfq_enqueue(&wp->dispatch_queue, &task->lfq_node);

	uint64_t val = 1;
	if (write(wp->dispatch_async.event_fd, &val, sizeof(val)) < 0) {
		// ignore
	}
}

static void*
worker_thread(void* arg)
{
	worker_data_t* data = (worker_data_t*)arg;
	worker_pool_t* wp = data->wp;
	csilk_server_t* server = wp->server;
	int port = data->port;
	csilk_barrier_t* barrier = data->barrier;
	free(data);

	csilk_io_loop_t* loop_ptr = &wp->loop;
	wp->loop_ptr = loop_ptr;
	int uring_flags_worker = IORING_SETUP_SQPOLL;
	if (io_uring_queue_init(4096, loop_ptr, uring_flags_worker) < 0) {
		CSILK_LOG_W("Worker %d: SQPOLL unavailable, falling back to non-polling",
			    wp->worker_index);
		if (io_uring_queue_init(4096, loop_ptr, 0) < 0) {
			CSILK_LOG_E("Worker %d: io_uring_queue_init failed", wp->worker_index);
			if (barrier) {
				barrier_wait(barrier);
			}
			return NULL;
		}
	}

	wp->server_handle.data = wp;

	_csilk_worker_init_arena_pool(wp);
	_csilk_worker_init_dispatch(wp, loop_ptr);

	/* Initialise this worker's thread pool. */
	{
		int nprocs = (int)sysconf(_SC_NPROCESSORS_ONLN);
		int nworkers = server->worker_pool_count;
		int tp_nthreads = (nprocs > 0) ? nprocs / nworkers : 1;
		if (tp_nthreads < 1) tp_nthreads = 1;
		wp->thread_pool = uring_tp_init(tp_nthreads);
		if (wp->thread_pool) {
			uring_tp_set_current((uring_thread_pool_t*)wp->thread_pool);
		}
		CSILK_LOG_I("Worker %d: thread pool initialised (%d threads)",
			    wp->worker_index, tp_nthreads);
	}

	if (bind_and_listen(
		loop_ptr, &wp->server_handle, port, server->config.listen_backlog, true) < 0) {
		if (barrier) {
			barrier_wait(barrier);
		}
		io_uring_queue_exit(loop_ptr);
		return NULL;
	}

	worker_stop_data_t sd = {loop_ptr, &wp->server_handle, server, wp->worker_index};
	wp->stop_async.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	wp->stop_async.data = &sd;

	struct io_uring_sqe* stop_sqe = io_uring_get_sqe(loop_ptr);
	io_uring_prep_poll_add(stop_sqe, wp->stop_async.event_fd, POLLIN);
	io_uring_sqe_set_data64(stop_sqe,
				uring_encode_data(URING_OP_WAKEUP, NULL, &wp->stop_async));

	struct io_uring_sqe* disp_sqe = io_uring_get_sqe(loop_ptr);
	io_uring_prep_poll_add(disp_sqe, wp->dispatch_async.event_fd, POLLIN);
	io_uring_sqe_set_data64(disp_sqe,
				uring_encode_data(URING_OP_WAKEUP, NULL, &wp->dispatch_async));

	/* Poll this worker's thread-pool wakeup eventfd. */
	if (wp->thread_pool) {
		int tp_fd = uring_tp_wakeup_fd((uring_thread_pool_t*)wp->thread_pool);
		struct io_uring_sqe* tp_sqe = io_uring_get_sqe(loop_ptr);
		if (tp_sqe && tp_fd >= 0) {
			io_uring_prep_poll_add(tp_sqe, tp_fd, POLLIN);
			io_uring_sqe_set_data64(
			    tp_sqe,
			    uring_encode_data(URING_OP_WAKEUP, NULL, wp->thread_pool));
		}
	}

	struct io_uring_sqe* acc_sqe = io_uring_get_sqe(loop_ptr);
	io_uring_prep_accept(
	    acc_sqe, wp->server_handle.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	io_uring_sqe_set_data64(acc_sqe, uring_encode_data(URING_OP_ACCEPT, NULL, wp));

	io_uring_submit(loop_ptr);

	if (barrier) {
		barrier_wait(barrier);
	}

	struct io_uring_cqe* cqe;
	int running = 1;
	while (running) {
		int ret = io_uring_wait_cqe(loop_ptr, &cqe);
		if (ret < 0) {
			if (ret == -EINTR) {
				continue;
			}
			CSILK_LOG_E("io_uring_wait_cqe failed: %d", ret);
			break;
		}

		int res = cqe->res;
		int flags = cqe->flags;
		uring_op_type_t op;
		void* ptr;
		uint8_t gen = 0;
		uring_decode_data(io_uring_cqe_get_data64(cqe), &op, &ptr, &gen);
		if (ptr && op != URING_OP_ACCEPT && op != URING_OP_WAKEUP && op != URING_OP_CLOSE) {
			csilk_client_t* client = (csilk_client_t*)ptr;
			if (op == URING_OP_WRITE) {
				client = ((uring_write_req_t*)ptr)->client;
			} else if (op == URING_OP_UV_WRITE) {
				client = (csilk_client_t*)(((void**)ptr)[0]);
			}
			if (client->generation != gen) {
				CSILK_LOG_D("Worker: ignoring old CQE (op %d, res %d)", op, res);
				io_uring_cqe_seen(loop_ptr, cqe);
				continue;
			}
		}

		io_uring_cqe_seen(loop_ptr, cqe);

		CSILK_LOG_D("Worker %d: wait_cqe returned op %d, res %d, flags %d",
			    wp->worker_index,
			    op,
			    res,
			    flags);

		if (op == URING_OP_ACCEPT) {
			if (res >= 0) {
				on_new_connection((worker_pool_t*)ptr, res);
			} else if (res != -EAGAIN && res != -ECANCELED) {
				CSILK_LOG_E("Accept failed with %d", res);
			}
			acc_sqe = io_uring_get_sqe(loop_ptr);
			if (acc_sqe) {
				io_uring_prep_accept(acc_sqe,
						     wp->server_handle.fd,
						     NULL,
						     NULL,
						     SOCK_NONBLOCK | SOCK_CLOEXEC);
				io_uring_sqe_set_data64(
				    acc_sqe, uring_encode_data(URING_OP_ACCEPT, NULL, wp));
				int submit_ret = io_uring_submit(loop_ptr);
				if (submit_ret < 0) {
					CSILK_LOG_E("io_uring_submit failed: %d", submit_ret);
				}
			} else {
				CSILK_LOG_E("Failed to get SQE for accept!");
			}
		} else if (op == URING_OP_WAKEUP) {
			if (ptr == &wp->dispatch_async) {
				uint64_t val;
				if (read(wp->dispatch_async.event_fd, &val, sizeof(val)) > 0) {
					on_dispatch_async(&wp->dispatch_async);
				}
				struct io_uring_sqe* poll_sqe = io_uring_get_sqe(loop_ptr);
				if (poll_sqe) {
					io_uring_prep_poll_add(
					    poll_sqe, wp->dispatch_async.event_fd, POLLIN);
					io_uring_sqe_set_data64(
					    poll_sqe,
					    uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
					io_uring_submit(loop_ptr);
				}
			} else if (wp->thread_pool && ptr == wp->thread_pool) {
				uint64_t val;
				uring_thread_pool_t* tp = (uring_thread_pool_t*)wp->thread_pool;
				if (read(uring_tp_wakeup_fd(tp), &val, sizeof(val)) > 0) {
					uring_tp_drain(tp);
				}
				int tp_fd = uring_tp_wakeup_fd(tp);
				struct io_uring_sqe* poll_sqe = io_uring_get_sqe(loop_ptr);
				if (poll_sqe && tp_fd >= 0) {
					io_uring_prep_poll_add(poll_sqe, tp_fd, POLLIN);
					io_uring_sqe_set_data64(
					    poll_sqe,
					    uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
					io_uring_submit(loop_ptr);
				}
			} else if (ptr == &wp->stop_async) {
				uint64_t val;
				if (read(wp->stop_async.event_fd, &val, sizeof(val)) > 0) {
					on_worker_stop_async(&wp->stop_async);
					running = 0;
				}
			}
		} else if (op == URING_OP_READ) {
			on_read((csilk_client_t*)ptr, cqe->res);
		} else if (op == URING_OP_WRITE) {
			on_write_done(ptr, cqe->res);
		} else if (op == URING_OP_UV_WRITE) {
			csilk_uv_on_write_done(ptr, cqe->res);
		} else if (op == URING_OP_TMR_READ || op == URING_OP_TMR_WRITE ||
			   op == URING_OP_TMR_IDLE || op == URING_OP_TMR_REQ) {
			/* All differentiated timer opcodes currently share the same
			 * handler.  Future optimisations can schedule per-timer-type
			 * actions (e.g. avoid closing on read-timeout if data just
			 * arrived). */
			on_timeout((csilk_client_t*)ptr);
		} else if (op == URING_OP_CLOSE) {
			on_close_done((csilk_client_t*)ptr);
		}
	}

	io_uring_queue_exit(loop_ptr);
	return NULL;
}

/* --- Bind and listen --- */

static int
bind_and_listen(
    csilk_io_loop_t* loop, csilk_io_tcp_t* out_handle, int port, int backlog, bool reuseport)
{
	(void)loop;
#ifndef _WIN32
	int fd;
	if (reuseport) {
#ifdef __APPLE__
		fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd >= 0) {
			int flags = fcntl(fd, F_GETFL, 0);
			fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		}
#else
		fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
#endif
		if (fd < 0) {
			return -1;
		}
		int on = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
		setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on));
	} else {
		fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		if (fd < 0) {
			return -1;
		}
		int on = 1;
		setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
	}

	struct sockaddr_in addr;
	addr.sin_family = AF_INET;
	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
		close(fd);
		return -1;
	}
	if (listen(fd, backlog) < 0) {
		close(fd);
		return -1;
	}
	out_handle->fd = fd;
	out_handle->data = NULL;
	return fd;
#else
	return -1;
#endif
}

/* --- Server run --- */

static void
openapi_json_handler(csilk_ctx_t* c)
{
	csilk_server_t* server = csilk_ctx_get_server(c);
	if (server && server->router) {
		csilk_serve_openapi(c,
				    server->router,
				    "Csilk API",
				    "1.0.0",
				    "Auto-generated OpenAPI documentation.");
	} else {
		csilk_set_status(c, 500);
	}
}

int
csilk_server_run(csilk_server_t* server, int port)
{
	if (!server) {
		return -1;
	}

	if (server->config.enable_openapi && server->router) {
		static csilk_handler_t handlers[] = {openapi_json_handler, nullptr};
		csilk_router_add(server->router, "GET", "/openapi.json", handlers, 1);
		CSILK_LOG_I(
		    "Server: OpenAPI endpoint automatically registered at GET /openapi.json");
	}

	if (server->config.enable_tls) {
		init_tls(server);
		if (!server->ssl_ctx) {
			CSILK_LOG_E("Server: failed to initialize TLS context");
			return -1;
		}
	}

	int workers = server->config.worker_threads;
	if (workers <= 0) {
		workers = 1;
	}

	server->async_handle.event_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	server->async_handle.data = server;

	if (bind_and_listen(server->loop,
			    &server->server_handle,
			    port,
			    server->config.listen_backlog,
			    workers > 0) < 0) {
		CSILK_LOG_E("Server: failed to bind and listen on port %d", port);
		close(server->async_handle.event_fd);
		return -1;
	}

	server->worker_pool_count = workers;
	server->worker_pools = calloc((size_t)workers, sizeof(worker_pool_t));
	if (!server->worker_pools) {
		CSILK_LOG_E("Server: failed to allocate memory for worker pools");
		close(server->async_handle.event_fd);
		close(server->server_handle.fd);
		return -1;
	}
	server->worker_pools[0].server = server;
	server->worker_pools[0].worker_index = 0;
	server->worker_pools[0].loop_ptr = server->loop;
	server->server_handle.data = &server->worker_pools[0];

	_csilk_worker_init_arena_pool(&server->worker_pools[0]);
	_csilk_worker_init_dispatch(&server->worker_pools[0], server->loop);

	/* Initialise per-worker thread pool (shared pool threads = nprocs / workers). */
	{
		int nprocs = (int)sysconf(_SC_NPROCESSORS_ONLN);
		int tp_nthreads = (nprocs > 0) ? nprocs / workers : 1;
		if (tp_nthreads < 1) tp_nthreads = 1;
		server->worker_pools[0].thread_pool = uring_tp_init(tp_nthreads);
		if (server->worker_pools[0].thread_pool) {
			uring_tp_set_current(server->worker_pools[0].thread_pool);
		}
		CSILK_LOG_I("Server: thread pool for worker 0 initialised (%d threads)", tp_nthreads);
	}

	if (workers > 1) {
		CSILK_LOG_I("Server: spawning %d worker threads...", workers - 1);
		int nworkers = workers - 1;
		server->worker_tids = malloc((size_t)nworkers * sizeof(pthread_t));
		if (server->worker_tids) {
			server->worker_count = nworkers;

			csilk_barrier_t barrier;
			barrier_init(&barrier, workers);

			for (int i = 0; i < nworkers; i++) {
				int idx = i + 1;
				server->worker_pools[idx].server = server;
				server->worker_pools[idx].worker_index = idx;

				worker_data_t* data = malloc(sizeof(worker_data_t));
				if (!data) {
					CSILK_LOG_E("Server: failed to allocate memory for worker "
						    "thread data");
					continue;
				}
				data->wp = &server->worker_pools[idx];
				data->port = port;
				data->barrier = &barrier;
				pthread_create(
				    (pthread_t*)&server->worker_tids[i], NULL, worker_thread, data);
			}

			barrier_wait(&barrier);
			barrier_destroy(&barrier);
			CSILK_LOG_I("Server: all %d worker threads spawned successfully",
				    workers - 1);
		} else {
			CSILK_LOG_E("Server: failed to allocate memory for worker thread IDs");
			free(server->worker_tids);
			server->worker_tids = nullptr;
		}
	}

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigprocmask(SIG_BLOCK, &mask, NULL);
	server->sig_handle.signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	server->sig_handle.data = server;

	struct io_uring_sqe* stop_sqe = io_uring_get_sqe(server->loop);
	io_uring_prep_poll_add(stop_sqe, server->async_handle.event_fd, POLLIN);
	io_uring_sqe_set_data64(stop_sqe,
				uring_encode_data(URING_OP_WAKEUP, NULL, &server->async_handle));

	struct io_uring_sqe* sig_sqe = io_uring_get_sqe(server->loop);
	io_uring_prep_poll_add(sig_sqe, server->sig_handle.signal_fd, POLLIN);
	io_uring_sqe_set_data64(sig_sqe,
				uring_encode_data(URING_OP_WAKEUP, NULL, &server->sig_handle));

	struct io_uring_sqe* disp_sqe = io_uring_get_sqe(server->loop);
	io_uring_prep_poll_add(disp_sqe, server->worker_pools[0].dispatch_async.event_fd, POLLIN);
	io_uring_sqe_set_data64(
	    disp_sqe,
	    uring_encode_data(URING_OP_WAKEUP, NULL, &server->worker_pools[0].dispatch_async));

	/* Poll MQ async eventfd for message dispatch */
	if (server->mq && server->mq->async_handle.event_fd >= 0) {
		struct io_uring_sqe* mq_sqe = io_uring_get_sqe(server->loop);
		if (mq_sqe) {
			io_uring_prep_poll_add(mq_sqe, server->mq->async_handle.event_fd, POLLIN);
			io_uring_sqe_set_data64(
			    mq_sqe,
			    uring_encode_data(URING_OP_WAKEUP, NULL, &server->mq->async_handle));
		}
	}

	/* Poll thread-pool wakeup eventfd for async work completions. */
	if (server->worker_pools[0].thread_pool) {
		int tp_fd = uring_tp_wakeup_fd(server->worker_pools[0].thread_pool);
		struct io_uring_sqe* tp_sqe = io_uring_get_sqe(server->loop);
		if (tp_sqe && tp_fd >= 0) {
			io_uring_prep_poll_add(tp_sqe, tp_fd, POLLIN);
			io_uring_sqe_set_data64(
			    tp_sqe,
			    uring_encode_data(URING_OP_WAKEUP, NULL,
					      server->worker_pools[0].thread_pool));
		}
	}

	struct io_uring_sqe* acc_sqe = io_uring_get_sqe(server->loop);
	io_uring_prep_accept(
	    acc_sqe, server->server_handle.fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
	io_uring_sqe_set_data64(acc_sqe,
				uring_encode_data(URING_OP_ACCEPT, NULL, &server->worker_pools[0]));

	io_uring_submit(server->loop);

	CSILK_LOG_I("\n  Server started on port %d with %d worker(s)\n", port, workers);

	_csilk_trigger_hooks(server, nullptr, CSILK_HOOK_SERVER_START);

	struct io_uring_cqe* cqe;
	int running = 1;
	while (running) {
		int ret = io_uring_wait_cqe(server->loop, &cqe);
		if (ret < 0) {
			if (ret == -EINTR) {
				continue;
			}
			CSILK_LOG_E("io_uring_wait_cqe failed: %d", ret);
			break;
		}

		int res = cqe->res;
		int flags = cqe->flags;
		uring_op_type_t op;
		void* ptr;
		uint8_t gen = 0;
		uring_decode_data(io_uring_cqe_get_data64(cqe), &op, &ptr, &gen);
		if (ptr && op != URING_OP_ACCEPT && op != URING_OP_WAKEUP && op != URING_OP_CLOSE) {
			csilk_client_t* client = (csilk_client_t*)ptr;
			if (op == URING_OP_WRITE) {
				client = ((uring_write_req_t*)ptr)->client;
			} else if (op == URING_OP_UV_WRITE) {
				client = (csilk_client_t*)(((void**)ptr)[0]);
			}
			if (client->generation != gen) {
				CSILK_LOG_D("Main: ignoring old CQE (op %d, res %d)", op, res);
				io_uring_cqe_seen(server->loop, cqe);
				continue;
			}
		}

		io_uring_cqe_seen(server->loop, cqe);

		CSILK_LOG_D("Worker 0: wait_cqe returned op %d, res %d, flags %d", op, res, flags);

		if (op == URING_OP_ACCEPT) {
			if (res >= 0) {
				on_new_connection((worker_pool_t*)ptr, res);
			} else if (res != -EAGAIN && res != -ECANCELED) {
				CSILK_LOG_E("Accept failed with %d", res);
			}
			acc_sqe = io_uring_get_sqe(server->loop);
			if (acc_sqe) {
				io_uring_prep_accept(acc_sqe,
						     server->server_handle.fd,
						     NULL,
						     NULL,
						     SOCK_NONBLOCK | SOCK_CLOEXEC);
				io_uring_sqe_set_data64(
				    acc_sqe, uring_encode_data(URING_OP_ACCEPT, NULL, ptr));
				int submit_ret = io_uring_submit(server->loop);
				if (submit_ret < 0) {
					CSILK_LOG_E("io_uring_submit failed: %d", submit_ret);
				}
			} else {
				CSILK_LOG_E("Failed to get SQE for accept!");
			}
		} else if (op == URING_OP_WAKEUP) {
			if (ptr == &server->async_handle) {
				uint64_t val;
				if (read(server->async_handle.event_fd, &val, sizeof(val)) > 0) {
					on_stop_async(&server->async_handle);
					running = 0;
				}
			} else if (ptr == &server->sig_handle) {
				struct signalfd_siginfo fdsi;
				if (read(server->sig_handle.signal_fd, &fdsi, sizeof(fdsi)) > 0) {
					on_signal(server);
				}
				struct io_uring_sqe* poll_sqe = io_uring_get_sqe(server->loop);
				if (poll_sqe) {
					io_uring_prep_poll_add(
					    poll_sqe, server->sig_handle.signal_fd, POLLIN);
					io_uring_sqe_set_data64(
					    poll_sqe,
					    uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
					io_uring_submit(server->loop);
				}
			} else if (ptr == &server->worker_pools[0].dispatch_async) {
				uint64_t val;
				if (read(server->worker_pools[0].dispatch_async.event_fd,
					 &val,
					 sizeof(val)) > 0) {
					on_dispatch_async(&server->worker_pools[0].dispatch_async);
				}
				struct io_uring_sqe* poll_sqe = io_uring_get_sqe(server->loop);
				if (poll_sqe) {
					io_uring_prep_poll_add(
					    poll_sqe,
					    server->worker_pools[0].dispatch_async.event_fd,
					    POLLIN);
					io_uring_sqe_set_data64(
					    poll_sqe,
					    uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
					io_uring_submit(server->loop);
				}
			} else if (server->mq && ptr == &server->mq->async_handle) {
				uint64_t val;
				if (read(server->mq->async_handle.event_fd, &val, sizeof(val)) >
				    0) {
					if (server->mq->async_handle.cb) {
						server->mq->async_handle.cb(
						    &server->mq->async_handle);
					}
				}
				struct io_uring_sqe* poll_sqe = io_uring_get_sqe(server->loop);
				if (poll_sqe) {
					io_uring_prep_poll_add(
					    poll_sqe, server->mq->async_handle.event_fd, POLLIN);
					io_uring_sqe_set_data64(
					    poll_sqe,
					    uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
					io_uring_submit(server->loop);
				}
			} else if (ptr == server->worker_pools[0].thread_pool) {
				uint64_t val;
				uring_thread_pool_t* tp =
				    (uring_thread_pool_t*)server->worker_pools[0].thread_pool;
				if (read(uring_tp_wakeup_fd(tp), &val, sizeof(val)) > 0) {
					uring_tp_drain(tp);
				}
				int tp_fd = uring_tp_wakeup_fd(tp);
				struct io_uring_sqe* poll_sqe = io_uring_get_sqe(server->loop);
				if (poll_sqe && tp_fd >= 0) {
					io_uring_prep_poll_add(poll_sqe, tp_fd, POLLIN);
					io_uring_sqe_set_data64(
					    poll_sqe,
					    uring_encode_data(URING_OP_WAKEUP, NULL, ptr));
					io_uring_submit(server->loop);
				}
			}
		} else if (op == URING_OP_READ) {
			on_read((csilk_client_t*)ptr, cqe->res);
		} else if (op == URING_OP_WRITE) {
			on_write_done(ptr, cqe->res);
		} else if (op == URING_OP_UV_WRITE) {
			csilk_uv_on_write_done(ptr, cqe->res);
		} else if (op == URING_OP_TMR_READ || op == URING_OP_TMR_WRITE ||
			   op == URING_OP_TMR_IDLE || op == URING_OP_TMR_REQ) {
			on_timeout((csilk_client_t*)ptr);
		} else if (op == URING_OP_CLOSE) {
			on_close_done((csilk_client_t*)ptr);
		}
	}

	return 0;
}

/* --- Accessors --- */

csilk_mq_t*
csilk_server_get_mq(csilk_server_t* server)
{
	return server ? server->mq : nullptr;
}

csilk_router_t*
csilk_server_get_router(csilk_server_t* server)
{
	return server ? server->router : nullptr;
}

void
csilk_server_set_router(csilk_server_t* server, csilk_router_t* router)
{
	if (!server || !router) {
		return;
	}
	csilk_router_t* old_router = server->router;
	server->router = router;
	if (old_router) {
		csilk_router_free(old_router);
	}
}
