#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/http/h2.h"
#include "core/internal/srv_internal.h"

int
LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)
{
    if (size == 0 || size > 65536) {
        return 0;
    }

    csilk_client_t* client = calloc(1, sizeof(csilk_client_t));
    if (!client) {
        return 0;
    }
    client->protocol = CSILK_PROTO_HTTP2;
    client->state = CSILK_CONN_CLOSED;
    client->server = nullptr;

    if (csilk_h2_init_session(client) == 0) {
        size_t split = size / 2;
        if (split > 0) {
            if (csilk_h2_process_data(client, data, split) == 0) {
                csilk_h2_process_data(client, data + split, size - split);
            }
        } else {
            csilk_h2_process_data(client, data, size);
        }
    }

    if (client->h2_session) {
        nghttp2_session_del(client->h2_session);
        client->h2_session = nullptr;
    }
    csilk_h2_free_streams(client);
    free(client);
    return 0;
}
