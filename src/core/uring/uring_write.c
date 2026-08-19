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

    req->cb = (void*)cb;
    req->handle = handle;

    struct iovec* iov = NULL;
    if (nbufs == 1) {
        io_uring_prep_send(sqe, handle->fd, bufs[0].base, bufs[0].len, MSG_NOSIGNAL);
    } else {
        iov = malloc(sizeof(struct iovec) * nbufs);
        if (!iov) {
            return -1;
        }
        for (unsigned int i = 0; i < nbufs; ++i) {
            iov[i].iov_base = bufs[i].base;
            iov[i].iov_len = bufs[i].len;
        }
        io_uring_prep_writev(sqe, handle->fd, iov, nbufs, 0);
    }

    void** ctx = malloc(sizeof(void*) * 3);
    if (!ctx) {
        if (iov) {
            free(iov);
        }
        return -1;
    }
    ctx[0] = client;
    ctx[1] = req;
    ctx[2] = iov;
    io_uring_sqe_set_data64(sqe, uring_encode_data(URING_OP_UV_WRITE, client, ctx));
    if (client) {
        _csilk_ctx_async_ref_incr(&client->ctx);
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
