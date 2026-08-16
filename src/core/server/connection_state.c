/**
 * @file connection_state.c
 * @brief Connection lifecycle state machine.
 */

#include "../internal/srv_internal.h"

const char*
csilk_conn_state_str(csilk_conn_state_t state)
{
    switch (state) {
    case CSILK_CONN_INIT:
        return "INIT";
    case CSILK_CONN_ACCEPTED:
        return "ACCEPTED";
    case CSILK_CONN_TLS:
        return "TLS";
    case CSILK_CONN_READING:
        return "READING";
    case CSILK_CONN_PROCESSING:
        return "PROCESSING";
    case CSILK_CONN_WRITING:
        return "WRITING";
    case CSILK_CONN_STREAMING:
        return "STREAMING";
    case CSILK_CONN_CLOSING:
        return "CLOSING";
    case CSILK_CONN_CLOSED:
        return "CLOSED";
    default:
        return "UNKNOWN";
    }
}

void
csilk_conn_set_state(csilk_client_t* client, csilk_conn_state_t new_state)
{
    if (!client) {
        return;
    }
    csilk_conn_state_t old_state = client->state;
    if (old_state == new_state) {
        return;
    }

    /* Invariant: once CLOSED, connection cannot transition except to INIT (pool reuse) */
    if (old_state == CSILK_CONN_CLOSED && new_state != CSILK_CONN_INIT) {
        CSILK_LOG_W("Conn %p: illegal state transition from CLOSED to %s",
                    (void*)client,
                    csilk_conn_state_str(new_state));
        return;
    }

    /* Invariant: once CLOSING, only allowed next state is CLOSED */
    if (old_state == CSILK_CONN_CLOSING && new_state != CSILK_CONN_CLOSED) {
        CSILK_LOG_D("Conn %p: ignored transition from CLOSING to %s",
                    (void*)client,
                    csilk_conn_state_str(new_state));
        return;
    }

    CSILK_LOG_T("Conn %p state: %s -> %s",
                (void*)client,
                csilk_conn_state_str(old_state),
                csilk_conn_state_str(new_state));
    client->state = new_state;
}

csilk_conn_state_t
csilk_conn_get_state(const csilk_client_t* client)
{
    return client ? client->state : CSILK_CONN_CLOSED;
}
