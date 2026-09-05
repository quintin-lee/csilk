/**
 * @file uring_write.c
 * @brief TCP write operations and completion callback.
 *
 * Ownership contract (mirrors libuv semantics):
 *  - The CALLER owns and allocates the csilk_io_write_t request. Primary
 *    requests live inside csilk_client_t; overflow requests are heap
 *    allocated by the caller. The transport NEVER frees the req itself —
 *    the caller's write callback does.
 *  - The transport owns the temporary iovec array (multi-buffer writes)
 *    and the 3-slot dispatch context, freeing both after the completion
 *    CQE is processed (or on the stale/generation-mismatch path).
 *  - The callback always receives the CALLER's original req pointer, so
 *    the libuv-style cleanup in on_write() (free req->data, free req if
 *    non-primary) works unchanged across both backends.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include <sys/uio.h>

#include "../ctx/ctx_internal.h"
#include "../internal/srv_internal.h"
#include "uring_internal.h"

/**
 * @brief Release a write request owned by a previous client incarnation.
 *
 * On a stale completion the caller's callback cannot run: the client struct
 * it would dereference has been recycled. The transport therefore performs
 * the minimal cleanup itself. A request may be pool-embedded (the client's
 * primary_write_req), in which case freeing it would free an interior pool
 * pointer — those are left alone; their in-flight flag died with the old
 * incarnation and the new one resets it in pool_get.
 */
static void
uring_release_stale_req(csilk_client_t* client, csilk_io_write_t* req)
{
    if (!req) {
        return;
    }
    if (req->data) {
        free(req->data);
        req->data = NULL;
    }
    if (client && req == &client->primary_write_req) {
        /* Interior pointer into the client pool — never free(). */
        return;
    }
    free(req);
}

/**
 * @brief Write completion dispatched from the CQE loop.
 *
 * @param arg   3-slot void* context: [0]=client, [1]=caller's req, [2]=iov.
 * @param res   Bytes written (>=0) or negative errno.
 * @param gen   Client generation captured at submit time.
 */
void
csilk_uv_on_write_done(void* arg, ssize_t res, uint64_t gen)
{
    (void)res;
    void** ctx = (void**)arg;
    if (!ctx) {
        return;
    }
    csilk_client_t*   client = (csilk_client_t*)ctx[0];
    csilk_io_write_t* req = (csilk_io_write_t*)ctx[1];
    struct iovec*     iov = (struct iovec*)ctx[2];

    /* Stale completion: the client struct was recycled (its generation was
     * bumped in pool_get) while this write was still in flight. The write
     * belongs to the PREVIOUS incarnation — do not run the callback against
     * the recycled client. */
    if (client && gen != client->generation) {
        uring_release_stale_req(client, req);
        if (iov) {
            free(iov);
        }
        free(ctx);
        return;
    }

    if (iov) {
        free(iov);
    }
    free(ctx);

    if (req && req->cb) {
        csilk_io_write_cb cb = (csilk_io_write_cb)req->cb;
        cb(req, (int)res);
    }

    if (client) {
        _csilk_ctx_async_ref_decr(&client->ctx);
    }
}

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

    /* libuv contract: uv_write() stamps the caller's req with the callback
     * and handle. All csilk write callbacks rely on req->handle to recover
     * the client, so populate the CALLER's req (never a wrapper copy). */
    req->cb = (void*)cb;
    req->handle = handle;

    struct io_uring_sqe* sqe = uring_get_sqe_or_submit(&loop->ring);
    if (!sqe) {
        return -1;
    }

    void** ctx = malloc(sizeof(void*) * 3);
    if (!ctx) {
        return -1;
    }
    ctx[0] = client;
    ctx[1] = req;
    ctx[2] = NULL;

    struct iovec* iov = NULL;
    if (nbufs == 1) {
        io_uring_prep_send(sqe, handle->fd, bufs[0].base, bufs[0].len, MSG_NOSIGNAL);
    } else {
        iov = malloc(sizeof(struct iovec) * nbufs);
        if (!iov) {
            free(ctx);
            return -1;
        }
        for (unsigned int i = 0; i < nbufs; ++i) {
            iov[i].iov_base = bufs[i].base;
            iov[i].iov_len = bufs[i].len;
        }
        io_uring_prep_writev(sqe, handle->fd, iov, nbufs, 0);
    }
    ctx[2] = iov;

    io_uring_sqe_set_data64(sqe, uring_encode_data(URING_OP_UV_WRITE, client, ctx));
    if (client) {
        _csilk_ctx_async_ref_incr(&client->ctx);
    }
    io_uring_submit(&loop->ring);
    return 0;
}

#endif
