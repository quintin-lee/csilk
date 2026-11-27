/**
 * @file uring_stream.c
 * @brief Stream (TCP) read, write, and vectorized response write operations.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <liburing.h>
#include <poll.h>
#include <sys/uio.h>

#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "uring_internal.h"

/* Pool helpers declared in connection_pool.c / connection_close.c */
extern void           pool_put_read_buf(worker_pool_t* wp, char* buf, size_t len);
extern worker_pool_t* _csilk_worker_get_current_pool(void);

typedef struct {
    csilk_client_t*   client;
    csilk_io_write_t* req;
    struct iovec*     iov;
} uring_write_ctx_t;

int
csilk_io_read_start(csilk_io_stream_t* stream, csilk_io_alloc_cb alloc_cb, csilk_io_read_cb read_cb)
{
    if (!stream || stream->fd < 0) {
        return -1;
    }
    stream->alloc_cb = alloc_cb;
    stream->read_cb = read_cb;
    if (stream->reading && (stream->flags & CSILK_IO_HANDLE_ACTIVE)) {
        return 0;
    }

    csilk_io_loop_t* loop = stream->loop ? stream->loop : csilk_io_default_loop();
    if (!loop) {
        return -1;
    }
    stream->loop = loop;

    /* Allocate buffer from pool for native io_uring recv SQE if not already retained */
    if (!stream->recv_buf.base || stream->recv_buf.len == 0) {
        if (stream->alloc_cb) {
            stream->alloc_cb((csilk_io_handle_t*)stream, 65536, &stream->recv_buf);
        }
    }
    if (!stream->recv_buf.base || stream->recv_buf.len == 0) {
        return -1;
    }

    uint64_t next_gen = stream->generation + 1;
    stream->generation = next_gen;

    uint64_t sqe_data = uring_encode_handle_data(URING_OP_READ, (csilk_io_handle_t*)stream);
    if (sqe_data == 0) {
        pool_put_read_buf(
            _csilk_worker_get_current_pool(), stream->recv_buf.base, stream->recv_buf.len);
        stream->recv_buf.base = NULL;
        stream->recv_buf.len = 0;
        return -1;
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        uring_op_free(loop, (uring_op_context_t*)(uintptr_t)sqe_data);
        pool_put_read_buf(
            _csilk_worker_get_current_pool(), stream->recv_buf.base, stream->recv_buf.len);
        stream->recv_buf.base = NULL;
        stream->recv_buf.len = 0;
        return -1;
    }

    stream->reading = 1;
    if (!(stream->flags & CSILK_IO_HANDLE_ACTIVE)) {
        stream->flags |= CSILK_IO_HANDLE_ACTIVE;
        loop->active_handles++;
    }

    io_uring_prep_recv(sqe, stream->fd, stream->recv_buf.base, stream->recv_buf.len, 0);
    io_uring_sqe_set_data64(sqe, sqe_data);
    io_uring_submit(&loop->ring);
    return 0;
}

int
csilk_io_read_stop(csilk_io_stream_t* stream)
{
    if (!stream) {
        return -1;
    }
    int was_reading = stream->reading;
    if (stream->flags & CSILK_IO_HANDLE_ACTIVE) {
        stream->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (stream->loop && stream->loop->active_handles > 0) {
            stream->loop->active_handles--;
        }
    }
    stream->reading = 0;
    stream->generation++;
    /* If an in-flight IORING_OP_RECV SQE is still pending on an open fd, issue
     * a cancel request and defer freeing recv_buf until the CQE is reaped or
     * the fd is closed in csilk_io_close(). */
    if (was_reading && stream->fd >= 0 && stream->loop) {
        struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&stream->loop->ring);
        if (sqe) {
            io_uring_prep_cancel_fd(sqe, stream->fd, 0);
            io_uring_sqe_set_data64(sqe, 0);
            io_uring_submit(&stream->loop->ring);
        }
    } else if (stream->recv_buf.base) {
        pool_put_read_buf(
            _csilk_worker_get_current_pool(), stream->recv_buf.base, stream->recv_buf.len);
        stream->recv_buf.base = NULL;
        stream->recv_buf.len = 0;
    }
    return 0;
}

/* ====================================================================
 * Stream write operations
 * ==================================================================== */

int
csilk_io_write(csilk_io_write_t*    req,
               csilk_io_stream_t*   handle,
               const csilk_io_buf_t bufs[],
               unsigned int         nbufs,
               csilk_io_write_cb    cb)
{
    if (!req || !handle || handle->fd < 0 || !bufs || nbufs == 0) {
        return -1;
    }
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (client && client->ctx.conn_closed) {
        return -1;
    }

    csilk_io_loop_t* loop = handle->loop;
    if (!loop && client && client->owner_pool) {
        loop = client->owner_pool->loop_ptr;
    }
    if (!loop) {
        loop = csilk_io_default_loop();
    }
    if (!loop) {
        return -1;
    }

    struct iovec* iov = NULL;
    if (nbufs > 1) {
        iov = malloc(sizeof(struct iovec) * nbufs);
        if (!iov) {
            return -1;
        }
        for (unsigned int i = 0; i < nbufs; ++i) {
            iov[i].iov_base = bufs[i].base;
            iov[i].iov_len = bufs[i].len;
        }
    }

    uring_write_ctx_t* wctx = malloc(sizeof(uring_write_ctx_t));
    if (!wctx) {
        free(iov);
        return -1;
    }
    wctx->client = client;
    wctx->req = req;
    wctx->iov = iov;

    uint64_t sqe_data = uring_encode_data(URING_OP_UV_WRITE, client, wctx);
    if (!sqe_data) {
        free(wctx);
        free(iov);
        return -1;
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        uring_op_free(loop, (uring_op_context_t*)(uintptr_t)sqe_data);
        free(wctx);
        free(iov);
        return -1;
    }

    req->cb = (void*)cb;
    req->handle = handle;

    if (nbufs == 1) {
        io_uring_prep_send(sqe, handle->fd, bufs[0].base, bufs[0].len, MSG_NOSIGNAL);
    } else {
        io_uring_prep_writev(sqe, handle->fd, iov, nbufs, 0);
    }
    io_uring_sqe_set_data64(sqe, sqe_data);

    if (client) {
        _csilk_ctx_async_ref_incr(&client->ctx);
    }
    io_uring_submit(&loop->ring);
    return 0;
}

void
csilk_uv_on_write_done(void* arg, ssize_t res, uint64_t gen)
{
    uring_write_ctx_t* wctx = (uring_write_ctx_t*)arg;
    if (!wctx) {
        return;
    }
    csilk_client_t*   client = wctx->client;
    csilk_io_write_t* req = wctx->req;
    struct iovec*     iov = wctx->iov;
    free(iov);
    free(wctx);

    /* Stale completion: the client struct was recycled (its generation was
     * bumped in pool_get) while this write was still in flight. The write
     * belongs to the PREVIOUS incarnation — free heap-allocated request
     * memory (skipping inline primary_write_req) and return without touching
     * the live connection. */
    if (client && gen != client->generation) {
        if (req && req != &client->primary_write_req) {
            if (req->data) {
                free(req->data);
            }
            free(req);
        }
        return;
    }

    if (req && req->cb) {
        csilk_io_write_cb cb = (csilk_io_write_cb)req->cb;
        cb(req, (int)res);
    }

    if (client) {
        _csilk_ctx_async_ref_decr(&client->ctx);
    }
}

/* ====================================================================
 * Vectorized response write
 * ==================================================================== */

/**
 * @brief Prepare a vectorized response write against an io_uring SQE.
 * @param[in] sqe_ptr     Pointer to the io_uring_sqe to populate.
 * @param[in] fd          File descriptor (socket/pipe) to write to; must be >= 0.
 * @param[out] iov_buf    Caller-owned buffer of at least 3 `struct iovec` entries
 *                        that must remain valid until the SQE is submitted/completed.
 * @param[in] status_line NUL-terminated status line (defaults to
 *                        "HTTP/1.1 200 OK\r\n" if NULL).
 * @param[in] headers     NUL-terminated header block (defaults to "" if NULL).
 * @param[in] body        Body bytes (defaults to "" if NULL).
 * @param[in] body_len    Number of bytes in body.
 * @return 0 on success, -1 on invalid arguments.
 */
int
csilk_uring_prep_writev_response(void*       sqe_ptr,
                                 int         fd,
                                 void*       iov_buf,
                                 const char* status_line,
                                 const char* headers,
                                 const char* body,
                                 size_t      body_len)
{
    if (!sqe_ptr || fd < 0 || !iov_buf) {
        return -1;
    }

    struct io_uring_sqe* sqe = (struct io_uring_sqe*)sqe_ptr;
    struct iovec*        iov = (struct iovec*)iov_buf;

    iov[0].iov_base = (void*)(status_line ? status_line : "HTTP/1.1 200 OK\r\n");
    iov[0].iov_len = strlen((const char*)iov[0].iov_base);

    iov[1].iov_base = (void*)(headers ? headers : "");
    iov[1].iov_len = strlen((const char*)iov[1].iov_base);

    iov[2].iov_base = (void*)(body ? body : "");
    iov[2].iov_len = body ? body_len : 0;

    io_uring_prep_writev2(sqe, fd, iov, 3, 0, 0);
    return 0;
}

#else

/**
 * @brief Stub for csilk_uring_prep_writev_response when io_uring is disabled.
 * @param[in] sqe_ptr     Unused.
 * @param[in] fd          Unused.
 * @param[in] iov_buf     Unused.
 * @param[in] status_line Unused.
 * @param[in] headers     Unused.
 * @param[in] body        Unused.
 * @param[in] body_len    Unused.
 * @return Always -1.
 */
int
csilk_uring_prep_writev_response(void*       sqe_ptr,
                                 int         fd,
                                 void*       iov_buf,
                                 const char* status_line,
                                 const char* headers,
                                 const char* body,
                                 size_t      body_len)
{
    (void)sqe_ptr;
    (void)fd;
    (void)iov_buf;
    (void)status_line;
    (void)headers;
    (void)body;
    (void)body_len;
    return -1;
}

#endif
