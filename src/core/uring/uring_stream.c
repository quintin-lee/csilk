/**
 * @file uring_stream.c
 * @brief Stream (TCP) read operations: start, stop.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <poll.h>

#include "../internal/srv_internal.h"
#include "uring_internal.h"

/* Pool helpers declared in connection_pool.c / connection_close.c */
extern void           pool_put_read_buf(worker_pool_t* wp, char* buf, size_t len);
extern worker_pool_t* _csilk_worker_get_current_pool(void);

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

#endif
