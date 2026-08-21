/**
 * @file uring_close.c
 * @brief Generic handle close operation.
 */

#include "csilk/core/sys_io.h"

#ifdef CSILK_USE_URING

#include "uring_internal.h"

void
csilk_io_close(csilk_io_handle_t* handle, csilk_io_close_cb cb)
{
    if (!handle) {
        return;
    }
    if (handle->flags & CSILK_IO_HANDLE_CLOSING) {
        return;
    }
    handle->flags |= CSILK_IO_HANDLE_CLOSING;
    if (handle->flags & CSILK_IO_HANDLE_ACTIVE) {
        handle->flags &= ~CSILK_IO_HANDLE_ACTIVE;
        if (handle->loop && handle->loop->active_handles > 0) {
            handle->loop->active_handles--;
        }
    }
    handle->generation++;

    if (handle->type == CSILK_IO_HANDLE_TIMER) {
        csilk_io_timer_stop((csilk_io_timer_t*)handle);
    } else if (handle->type == CSILK_IO_HANDLE_SIGNAL) {
        csilk_io_signal_stop((csilk_io_signal_t*)handle);
    } else if (handle->type == CSILK_IO_HANDLE_ASYNC) {
        csilk_io_async_t* async = (csilk_io_async_t*)handle;
        int event_fd_val = atomic_load_explicit(&async->event_fd, memory_order_relaxed);
        if (event_fd_val >= 0) {
            close(event_fd_val);
            atomic_store_explicit(&async->event_fd, -1, memory_order_release);
            async->fd = -1;
        }
    } else if (handle->type == CSILK_IO_HANDLE_TCP) {
        csilk_io_tcp_t* tcp = (csilk_io_tcp_t*)handle;
        if (tcp->recv_buf.base) {
            extern void pool_put_read_buf(worker_pool_t * wp, char* buf, size_t len);
            pool_put_read_buf(NULL, tcp->recv_buf.base, tcp->recv_buf.len);
            tcp->recv_buf.base = NULL;
            tcp->recv_buf.len = 0;
        }
        if (handle->fd >= 0) {
            csilk_client_t* client = (csilk_client_t*)handle->data;
            if (!client ||
                (atomic_load(&client->ref_count) <= 0 && atomic_load(&client->pending_io) <= 0)) {
                close(handle->fd);
                handle->fd = -1;
            }
        }
    } else if (handle->fd >= 0) {
        csilk_client_t* client = (csilk_client_t*)handle->data;
        if (!client ||
            (atomic_load(&client->ref_count) <= 0 && atomic_load(&client->pending_io) <= 0)) {
            close(handle->fd);
            handle->fd = -1;
        }
    }

    if (cb) {
        cb(handle);
    }
}

#endif
