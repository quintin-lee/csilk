/**
 * @file uring_write.c
 * @brief TCP write operations and completion callback.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <sys/uio.h>

#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "uring_internal.h"

typedef struct {
    csilk_io_op_t    op;
    csilk_io_write_t req;
    struct iovec*    iov;
} csilk_io_write_op_t;

static void
csilk_io_write_op_complete(csilk_io_op_t* op, int status)
{
    if (!op || !op->user_data) {
        return;
    }
    csilk_io_write_op_t* write_op = (csilk_io_write_op_t*)op->user_data;
    csilk_io_write_cb    cb = (csilk_io_write_cb)write_op->req.cb;
    if (cb) {
        cb(&write_op->req, status);
    }
    free(write_op);
}

static void
csilk_io_write_op_cancel(csilk_io_op_t* op)
{
    if (!op || !op->user_data) {
        return;
    }
    csilk_io_write_op_t* write_op = (csilk_io_write_op_t*)op->user_data;
    if (write_op->iov) {
        free(write_op->iov);
        write_op->iov = NULL;
    }
}

static csilk_io_write_op_t*
csilk_io_write_op_alloc(csilk_client_t* client)
{
    csilk_io_write_op_t* write_op = malloc(sizeof(csilk_io_write_op_t));
    if (!write_op) {
        return NULL;
    }
    uint64_t generation = client ? client->generation : 0;
    csilk_io_op_init(&write_op->op, CSILK_IO_OP_WRITE, client, generation);
    write_op->op.complete = csilk_io_write_op_complete;
    write_op->op.cancel = csilk_io_write_op_cancel;
    write_op->op.user_data = write_op;
    write_op->iov = NULL;
    return write_op;
}

int
csilk_io_write(csilk_io_write_t*    req,
               csilk_io_stream_t*   handle,
               const csilk_io_buf_t bufs[],
               unsigned int         nbufs,
               csilk_io_write_cb    cb)
{
    if (!handle || handle->fd < 0) {
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

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        return -1;
    }

    csilk_io_write_op_t* write_op = csilk_io_write_op_alloc(client);
    if (!write_op) {
        return -1;
    }

    write_op->req.cb = (void*)cb;
    write_op->req.handle = handle;

    struct iovec* iov = NULL;
    if (nbufs == 1) {
        io_uring_prep_send(sqe, handle->fd, bufs[0].base, bufs[0].len, MSG_NOSIGNAL);
    } else {
        iov = malloc(sizeof(struct iovec) * nbufs);
        if (!iov) {
            free(write_op);
            return -1;
        }
        for (unsigned int i = 0; i < nbufs; ++i) {
            iov[i].iov_base = bufs[i].base;
            iov[i].iov_len = bufs[i].len;
        }
        io_uring_prep_writev(sqe, handle->fd, iov, nbufs, 0);
    }
    write_op->iov = iov;

    void** ctx = malloc(sizeof(void*) * 3);
    if (!ctx) {
        if (iov) {
            free(iov);
        }
        free(write_op);
        return -1;
    }
    ctx[0] = client;
    ctx[1] = &write_op->req;
    ctx[2] = iov;
    io_uring_sqe_set_data64(sqe, uring_encode_data(URING_OP_UV_WRITE, client, ctx));
    if (client) {
        _csilk_ctx_async_ref_incr(&client->ctx);
    }
    if (csilk_io_op_submit(&write_op->op) != 0) {
        if (client) {
            _csilk_ctx_async_ref_decr(&client->ctx);
        }
        free(ctx);
        free(write_op);
        return -1;
    }
    io_uring_submit(&loop->ring);
    return 0;
}

void
csilk_uv_on_write_done(void* arg, ssize_t res, uint64_t gen)
{

    void** ctx = (void**)arg;
    if (!ctx) {
        return;
    }
    csilk_client_t*   client = (csilk_client_t*)ctx[0];
    csilk_io_write_t* req = (csilk_io_write_t*)ctx[1];
    struct iovec*     iov = (struct iovec*)ctx[2];

    /* Stale completion: the client struct was recycled (its generation was
     * bumped in pool_get) while this write was still in flight. The write
     * belongs to the PREVIOUS incarnation — free the request memory and
     * return. Running the callback or decrementing the new connection's
     * async_ref would corrupt the live connection (e.g. close its fd before
     * its request is ever read). */
    if (client && gen != client->generation) {
        if (req) {
            if (req->data) {
                free(req->data);
            }
            free(req);
        }
        if (iov) {
            free(iov);
        }
        free(ctx);
        return;
    }

    if (iov) {
        free(iov);
    }
    if (req && req->cb) {
        csilk_io_write_cb cb = (csilk_io_write_cb)req->cb;
        cb(req, (int)res);
    }
    free(ctx);

    if (client) {
        _csilk_ctx_async_ref_decr(&client->ctx);
    }
}

#endif
