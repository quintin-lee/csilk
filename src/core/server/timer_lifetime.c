/**
 * @file timer_lifetime.c
 * @brief Centralized timer lifecycle helpers for client shutdown.
 */

#include "../internal/srv_impl.h"

#include "../internal/srv_internal.h"

void
_csilk_client_stop_timers(csilk_client_t* client)
{
    if (!client) {
        return;
    }

    csilk_io_timer_stop(&client->timer);
    csilk_io_timer_stop(&client->read_timer);
    csilk_io_timer_stop(&client->write_timer);
    csilk_io_timer_stop(&client->request_timer);

    csilk_io_handle_t* timers[] = {(csilk_io_handle_t*)&client->timer,
                                   (csilk_io_handle_t*)&client->read_timer,
                                   (csilk_io_handle_t*)&client->write_timer,
                                   (csilk_io_handle_t*)&client->request_timer};
    for (int i = 0; i < 4; i++) {
        if (!csilk_io_is_closing(timers[i])) {
            _csilk_client_pending_io_inc(client);
            timers[i]->data = client;
            timers[i]->close_cb = on_timer_close;
            csilk_io_close(timers[i], on_timer_close);
        }
    }
}
