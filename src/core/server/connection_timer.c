/**
 * @file connection_timer.c
 * @brief Timer callbacks for connection lifecycle.
 */

#include "../internal/srv_internal.h"

/** @brief Close callback for timer handles — decrements close_pending
 *  and triggers client_destroy when all timers are closed.
 *
 *  Each of the four timers (idle, read, write, request) calls this once on
 *  close.  Client destruction is deferred until all four have acknowledged
 *  AND async_ref is zero.
 *
 *  @param handle The timer handle being closed (data points to csilk_client_t).
 */
void
on_timer_close(csilk_io_handle_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!client) {
        return;
    }
    client->close_pending--;
    if (client->close_pending > 0) {
        return;
    }
    if (client->async_ref > 0) {
        return;
    }
    /* Forward declaration - defined in connection_close.c */
    extern void client_destroy(csilk_client_t * client);
    client_destroy(client);
}

/** @brief Timer callback: fired when no I/O activity occurs within the
 *  idle timer window (keep-alive timeout).
 *
 *  Closes the connection gracefully, which triggers the on_close chain.
 *  Skips close if already closing to avoid double-close.
 *
 *  @param handle The idle timer handle (data points to csilk_client_t).
 */
void
on_idle_timeout(csilk_io_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        CSILK_LOG_D("Connection: closing connection due to idle timeout");
        extern void on_close(csilk_io_handle_t * handle);
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/** @brief Timer callback: fired when no request data has been received
 * within read_timeout_ms.
 *
 * Closes the connection immediately.
 *
 * @param handle The timer handle (castable to client via handle->data).
 */
void
on_read_timeout(csilk_io_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        CSILK_LOG_D("Connection: closing connection due to read timeout");
        extern void on_close(csilk_io_handle_t * handle);
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}

/** @brief Timer callback: fired when the response write has not
 * completed within write_timeout_ms.
 *
 * Closes the connection immediately.
 *
 * @param handle The timer handle (castable to client via handle->data).
 */
void
on_write_timeout(csilk_io_timer_t* handle)
{
    csilk_client_t* client = (csilk_client_t*)handle->data;
    if (!csilk_io_is_closing((csilk_io_handle_t*)&client->handle)) {
        extern void on_close(csilk_io_handle_t * handle);
        csilk_io_close((csilk_io_handle_t*)&client->handle, on_close);
    }
}
