#include <csilk/core/sys_io.h>

#ifdef CSILK_USE_URING

#include <csilk/csilk.h>
#include <csilk/server.h>
#include <csilk/core/internal.h>
#include "../srv_internal.h"
#include "uring_internal.h"

void csilk_client_close(csilk_client_t* client);

int
csilk_io_is_closing(const csilk_io_handle_t* handle)
{
	(void)handle;
	/* io_uring backend has no libuv closing state machine.
	 * Returns 0 (not closing) for all handles. */
	return 0;
}

int
csilk_io_fileno(const csilk_io_handle_t* handle, csilk_io_os_fd_t* fd)
{
	if (!handle || !fd) {
		return -1;
	}
	csilk_io_stream_t* stream = (csilk_io_stream_t*)handle;
	*fd = stream->fd;
	return 0;
}

int
csilk_io_write(csilk_io_write_t* req,
	       csilk_io_stream_t* handle,
	       const csilk_io_buf_t bufs[],
	       unsigned int nbufs,
	       csilk_io_write_cb cb)
{
	if (!handle || !handle->data) {
		return -1;
	}
	csilk_client_t* client = (csilk_client_t*)handle->data;
	if (client->ctx.conn_closed) {
		return -1;
	}

	struct io_uring* ring = client->owner_pool->loop_ptr;
	struct io_uring_sqe* sqe = io_uring_get_sqe(ring);
	if (!sqe) {
		return -1;
	}

	req->cb = (void*)cb;
	req->handle = handle;

	struct iovec* iov = malloc(sizeof(struct iovec) * nbufs);
	for (unsigned int i = 0; i < nbufs; ++i) {
		iov[i].iov_base = bufs[i].base;
		iov[i].iov_len = bufs[i].len;
	}

	io_uring_prep_writev(sqe, handle->fd, iov, nbufs, 0);

	void** ctx = malloc(sizeof(void*) * 3);
	ctx[0] = client;
	ctx[1] = req;
	ctx[2] = iov;
	io_uring_sqe_set_data(sqe, (void*)uring_encode_data(URING_OP_UV_WRITE, client, ctx));
	atomic_fetch_add(&client->async_ref, 1);
	io_uring_submit(ring);
	return 0;
}

void
csilk_io_close(csilk_io_handle_t* handle, csilk_io_close_cb cb)
{
	if (!handle) {
		return;
	}
	/* In the uring backend, only TCP handles (csilk_io_tcp_t) wrap clients.
	 * Distinguish by checking if data points to a valid client structure
	 * (has ctx.conn_closed at a known offset). Async/timer handles have
	 * different layouts and must not be cast to stream. */
	if (handle->data && ((uintptr_t)handle->data > 0x1000)) {
		csilk_io_stream_t* stream = (csilk_io_stream_t*)handle;
		csilk_client_t* client = (csilk_client_t*)stream->data;
		/* Verify this looks like a real client (not a random pointer) */
		if (client && client->server && client->handle.fd >= 0) {
			csilk_client_close(client);
		}
	}
	/* For non-stream handles (async, timer, etc.) the callback is a no-op
	 * since there's no libuv close semantics in the io_uring backend. */
	if (cb && handle->data && ((uintptr_t)handle->data > 0x1000)) {
		cb(handle);
	}
}

void
on_close(csilk_io_handle_t* handle)
{
}
void
on_idle_timeout(csilk_io_timer_t* handle)
{
}
void
on_read_timeout(csilk_io_timer_t* handle)
{
}
void
on_write_timeout(csilk_io_timer_t* handle)
{
}

int
csilk_io_timer_start(csilk_io_timer_t* handle,
		     csilk_io_timer_cb cb,
		     uint64_t timeout,
		     uint64_t repeat)
{
	return -1;
}
int
csilk_io_timer_stop(csilk_io_timer_t* handle)
{
	return -1;
}
int
csilk_io_timer_init(csilk_io_loop_t* loop, csilk_io_timer_t* handle)
{
	return -1;
}
int
csilk_io_run(csilk_io_loop_t* loop, csilk_io_run_mode mode)
{
	return -1;
}

#endif

void
csilk_uv_on_write_done(void* arg, ssize_t res)
{
	void** ctx = (void**)arg;
	if (!ctx) {
		return;
	}
	csilk_client_t* client = (csilk_client_t*)ctx[0];
	csilk_io_write_t* req = (csilk_io_write_t*)ctx[1];
	struct iovec* iov = (struct iovec*)ctx[2];

	if (iov) {
		free(iov);
	}
	if (req && req->cb) {
		csilk_io_write_cb cb = (csilk_io_write_cb)req->cb;
		cb(req, (int)res);
	}
	free(ctx);

	atomic_fetch_sub(&client->async_ref, 1);
	if (client->close_pending && atomic_load(&client->async_ref) == 0) {
		on_close_done(client);
	}
}
