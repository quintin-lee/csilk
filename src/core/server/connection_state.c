/**
 * @file connection_state.c
 * @brief Connection lifecycle state machine.
 */

#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"

enum { CSILK_CONN_STATE_COUNT = 9 };

/**
 * @brief State transition validity matrix.
 *
 * Rows represent the current (FROM) state, columns represent the target (TO) state.
 * Transitions marked true (1) are permitted by the lifecycle state machine;
 * transitions marked false (0) are strictly rejected.
 *
 * Invariant rules enforced:
 *   1. INIT can only transition to ACCEPTED, CLOSING, or CLOSED.
 *      (INIT -> WRITING and INIT -> STREAMING are forbidden).
 *   2. ACCEPTED can transition to TLS, READING, CLOSING, or CLOSED.
 *      (ACCEPTED -> WRITING is forbidden).
 *   3. TLS can transition to READING, CLOSING, or CLOSED.
 *      (TLS -> WRITING is forbidden).
 *   4. READING can transition to PROCESSING, STREAMING, CLOSING, or CLOSED.
 *   5. PROCESSING can transition to WRITING, STREAMING, CLOSING, or CLOSED.
 *   6. WRITING can transition to READING (keep-alive), STREAMING, CLOSING, or CLOSED.
 *   7. STREAMING can transition to READING (keep-alive), WRITING, CLOSING, or CLOSED.
 *   8. CLOSING can ONLY transition to CLOSED (or self-noop).
 *   9. CLOSED can ONLY transition to INIT (pool recycling) (or self-noop).
 *  10. Self-transitions (FROM == TO) are always permitted as no-ops.
 */
static const bool allowed_transitions[CSILK_CONN_STATE_COUNT][CSILK_CONN_STATE_COUNT] = {
    /* [FROM] = { [TO] ... } */
    [CSILK_CONN_INIT] =
        {
                           [CSILK_CONN_INIT] = true,
                           [CSILK_CONN_ACCEPTED] = true,
                           [CSILK_CONN_TLS] = false,
                           [CSILK_CONN_READING] = false,
                           [CSILK_CONN_PROCESSING] = false,
                           [CSILK_CONN_WRITING] = false,   /* Forbidden */
            [CSILK_CONN_STREAMING] = false, /* Forbidden */
            [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,
                           },
    [CSILK_CONN_ACCEPTED] =
        {
                           [CSILK_CONN_INIT] = false,
                           [CSILK_CONN_ACCEPTED] = true,
                           [CSILK_CONN_TLS] = true,
                           [CSILK_CONN_READING] = true,
                           [CSILK_CONN_PROCESSING] = false,
                           [CSILK_CONN_WRITING] = false,   /* Forbidden */
            [CSILK_CONN_STREAMING] = false, /* Forbidden */
            [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,
                           },
    [CSILK_CONN_TLS] =
        {
                           [CSILK_CONN_INIT] = false,
                           [CSILK_CONN_ACCEPTED] = false,
                           [CSILK_CONN_TLS] = true,
                           [CSILK_CONN_READING] = true,
                           [CSILK_CONN_PROCESSING] = false,
                           [CSILK_CONN_WRITING] = false,   /* Forbidden */
            [CSILK_CONN_STREAMING] = false, /* Forbidden */
            [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,
                           },
    [CSILK_CONN_READING] =
        {
                           [CSILK_CONN_INIT] = false,
                           [CSILK_CONN_ACCEPTED] = false,
                           [CSILK_CONN_TLS] = false,
                           [CSILK_CONN_READING] = true,
                           [CSILK_CONN_PROCESSING] = true,
                           [CSILK_CONN_WRITING] = false,
                           [CSILK_CONN_STREAMING] = true,
                           [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,
                           },
    [CSILK_CONN_PROCESSING] =
        {
                           [CSILK_CONN_INIT] = false,
                           [CSILK_CONN_ACCEPTED] = false,
                           [CSILK_CONN_TLS] = false,
                           [CSILK_CONN_READING] = false,
                           [CSILK_CONN_PROCESSING] = true,
                           [CSILK_CONN_WRITING] = true,
                           [CSILK_CONN_STREAMING] = true,
                           [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,
                           },
    [CSILK_CONN_WRITING] =
        {
                           [CSILK_CONN_INIT] = false,
                           [CSILK_CONN_ACCEPTED] = false,
                           [CSILK_CONN_TLS] = false,
                           [CSILK_CONN_READING] = true,
                           [CSILK_CONN_PROCESSING] = false,
                           [CSILK_CONN_WRITING] = true,
                           [CSILK_CONN_STREAMING] = true,
                           [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,
                           },
    [CSILK_CONN_STREAMING] =
        {
                           [CSILK_CONN_INIT] = false,
                           [CSILK_CONN_ACCEPTED] = false,
                           [CSILK_CONN_TLS] = false,
                           [CSILK_CONN_READING] = true,
                           [CSILK_CONN_PROCESSING] = false,
                           [CSILK_CONN_WRITING] = true,
                           [CSILK_CONN_STREAMING] = true,
                           [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,
                           },
    [CSILK_CONN_CLOSING] =
        {
                           [CSILK_CONN_INIT] = false,       /* Forbidden */
            [CSILK_CONN_ACCEPTED] = false,   /* Forbidden */
            [CSILK_CONN_TLS] = false,        /* Forbidden */
            [CSILK_CONN_READING] = false,    /* Forbidden */
            [CSILK_CONN_PROCESSING] = false, /* Forbidden */
            [CSILK_CONN_WRITING] = false,    /* Forbidden */
            [CSILK_CONN_STREAMING] = false,  /* Forbidden */
            [CSILK_CONN_CLOSING] = true,
                           [CSILK_CONN_CLOSED] = true,      /* ONLY ALLOWED */
        },
    [CSILK_CONN_CLOSED] =
        {
                           [CSILK_CONN_INIT] = true,        /* ONLY ALLOWED (pool recycling) */
            [CSILK_CONN_ACCEPTED] = false,   /* Forbidden */
            [CSILK_CONN_TLS] = false,        /* Forbidden */
            [CSILK_CONN_READING] = false,    /* Forbidden */
            [CSILK_CONN_PROCESSING] = false, /* Forbidden */
            [CSILK_CONN_WRITING] = false,    /* Forbidden */
            [CSILK_CONN_STREAMING] = false,  /* Forbidden */
            [CSILK_CONN_CLOSING] = false,    /* Forbidden */
            [CSILK_CONN_CLOSED] = true,
                           },
};

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

bool
csilk_conn_is_valid_transition(csilk_conn_state_t from, csilk_conn_state_t to)
{
    if ((int)from < 0 || (int)from >= CSILK_CONN_STATE_COUNT || (int)to < 0 ||
        (int)to >= CSILK_CONN_STATE_COUNT) {
        return false;
    }
    return allowed_transitions[from][to];
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

    if (!csilk_conn_is_valid_transition(old_state, new_state)) {
#if !defined(NDEBUG) || defined(DEBUG)
        CSILK_LOG_E("Conn %p (gen: %u): illegal state transition from %s (%d) to %s (%d)",
                    (void*)client,
                    client->generation,
                    csilk_conn_state_str(old_state),
                    (int)old_state,
                    csilk_conn_state_str(new_state),
                    (int)new_state);
#endif
        return;
    }

#if !defined(NDEBUG) || defined(DEBUG)
    CSILK_LOG_T("Conn %p state: %s -> %s",
                (void*)client,
                csilk_conn_state_str(old_state),
                csilk_conn_state_str(new_state));
#endif
    client->state = new_state;
}

csilk_conn_state_t
csilk_conn_get_state(const csilk_client_t* client)
{
    return client ? client->state : CSILK_CONN_CLOSED;
}
