/**
 * @file uring_stream.c
 * @brief Stream (TCP) read operations: start, stop.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <poll.h>

#include "../internal/srv_internal.h"
#include "uring_internal.h"

/* Pool helpers declared in connection_pool.c */
extern void pool_put_read_buf(worker_pool_t* wp, char* buf, size_t len);

typedef struct {
    csilk_io_op_t      op;
    csilk_io_stream_t* stream;
    csilk_io_read_cb   read_cb;
} csilk_io_read_op_t;

static void
csilk_io_read_op_complete(csilk_io_op_t* op, int status)
{
    csilk_io_read_op_t* read_op = (csilk_io_read_op_t*)op;
    if (!read_op || !read_op->stream || !read_op->read_cb) {
        return;
    }

    csilk_io_stream_t* stream = read_op->stream;
    csilk_io_read_cb   cb = read_op->read_cb;

    if (status < 0) {
        stream->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        stream->reading = 0;
        if (stream->loop && stream->loop->active_handles > 0) {
            stream->loop->active_handles--;
        }
        if (stream->recv_buf.base) {
            pool_put_read_buf(NULL, stream->recv_buf.base, stream->recv_buf.len);
            stream->recv_buf.base = NULL;
            stream->recv_buf.len = 0;
        }
    }

    cb(stream, status, &stream->recv_buf);
}

static void
csilk_io_read_op_cancel(csilk_io_op_t* op)
{
    (void)op;
}

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
    stream->reading = 1;
    if (!(stream->flags & CSILK_IO_HANDLE_ACTIVE)) {
        stream->flags |= CSILK_IO_HANDLE_ACTIVE;
        csilk_io_loop_t* loop = stream->loop ? stream->loop : csilk_io_default_loop();
        stream->loop = loop;
        loop->active_handles++;
    }
    stream->generation++;

    csilk_io_loop_t* loop = stream->loop ? stream->loop : csilk_io_default_loop();

    /* Allocate buffer from pool for native io_uring recv SQE */
    if (stream->alloc_cb) {
        stream->alloc_cb((csilk_io_handle_t*)stream, 65536, &stream->recv_buf);
    }
    if (!stream->recv_buf.base || stream->recv_buf.len == 0) {
        return -1;
    }

    csilk_io_read_op_t* read_op = calloc(1, sizeof(*read_op));
    if (!read_op) {
        if (stream->recv_buf.base) {
            pool_put_read_buf(NULL, stream->recv_buf.base, stream->recv_buf.len);
            stream->recv_buf.base = NULL;
            stream->recv_buf.len = 0;
        }
        return -1;
    }

    csilk_io_op_init(&read_op->op, CSILK_IO_OP_READ, stream, stream->generation);
    read_op->op.complete = csilk_io_read_op_complete;
    read_op->op.cancel = csilk_io_read_op_cancel;
    read_op->stream = stream;
    read_op->read_cb = read_cb;

    if (csilk_io_op_submit(&read_op->op) != 0) {
        free(read_op);
        if (stream->recv_buf.base) {
            pool_put_read_buf(NULL, stream->recv_buf.base, stream->recv_buf.len);
            stream->recv_buf.base = NULL;
            stream->recv_buf.len = 0;
        }
        return -1;
    }

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        csilk_io_op_cancel(&read_op->op);
        csilk_io_op_retire(&read_op->op);
        free(read_op);
        if (stream->recv_buf.base) {
            pool_put_read_buf(NULL, stream->recv_buf.base, stream->recv_buf.len);
            stream->recv_buf.base = NULL;
            stream->recv_buf.len = 0;
        }
        return -1;
    }
    io_uring_prep_recv(sqe, stream->fd, stream->recv_buf.base, stream->recv_buf.len, 0);
    io_uring_sqe_set_data64(sqe,
                            uring_encode_handle_data(URING_OP_READ, (csilk_io_handle_t*)stream));
    io_uring_submit(&loop->ring);
    return 0;
}

int
csilk_io_read_stop(csilk_io_stream_t* stream)
{
    if (!stream) {
        return -1;
    }
    if (stream->flags & CSILK_IO_HANDLE_ACTIVE) {
        stream->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (stream->loop && stream->loop->active_handles > 0) {
            stream->loop->active_handles--;
        }
    }
    stream->reading = 0;
    stream->generation++;
    if (stream->recv_buf.base) {
        pool_put_read_buf(NULL, stream->recv_buf.base, stream->recv_buf.len);
        stream->recv_buf.base = NULL;
        stream->recv_buf.len = 0;
    }
    return 0;
}

#endif
