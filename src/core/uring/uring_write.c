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
    csilk_client_t*   client;
    csilk_io_write_t* req;
    struct iovec*     iov;
} uring_write_ctx_t;

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

#endif
