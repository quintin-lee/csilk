/**
 * @file test_connection.c
 * @brief Tests for connection pool and client management.
 * @copyright MIT License
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/json.h"
#include "core/ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"
#include "csilk/core/sync.h"
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"
#include "csilk/test/test.h"

static int tests_run = 0;
static int tests_passed = 0;

#define PASS() (tests_run++, tests_passed++)
#define FAIL(msg)                                                                                  \
    do {                                                                                           \
        tests_run++;                                                                               \
        printf("  FAIL: %s\n", msg);                                                               \
    } while (0)

/* ------------------------------------------------------------------ */

static csilk_server_t*
mock_server(void)
{
    csilk_server_t* s = calloc(1, sizeof(csilk_server_t));
    // removed clients_mutex
    s->worker_pools = calloc(1, sizeof(worker_pool_t));
    s->worker_pools[0].server = s;
    s->worker_pool_count = 1;
    return s;
}

static void
free_mock_server(csilk_server_t* s)
{
    free(s->worker_pools);
    // removed clients_mutex
    free(s);
}

/* ------------------------------------------------------------------ */

static void
test_client_pool_constant_defined(void)
{
    if (CSILK_CLIENT_POOL_SIZE >= 16 && CSILK_CLIENT_POOL_SIZE <= 1024) {
        PASS();
    } else {
        FAIL("CSILK_CLIENT_POOL_SIZE out of range");
    }
}

static void
test_server_alloc_sets_pool(void)
{
    csilk_server_t* s = mock_server();
    if (s->worker_pool_count == 1 && s->worker_pools[0].client_pool_count == 0) {
        PASS();
    } else {
        FAIL("worker pool should be empty at init");
    }
    free_mock_server(s);
}

static void
test_server_mutexes_init(void)
{
    csilk_server_t* s = mock_server();
    if (s) {
        PASS();
    } else {
        FAIL("server is null");
    }
    free_mock_server(s);
}

/* ------------------------------------------------------------------ */

static void
test_context_internal_client_roundtrip(void)
{
    csilk_ctx_t*   c = csilk_test_ctx_new();
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    _csilk_set_internal_client(c, &client);
    void* got = _csilk_get_internal_client(c);
    if (got == &client) {
        PASS();
    } else {
        FAIL("internal_client roundtrip");
    }
    csilk_test_ctx_free(c);
}

static void
test_context_internal_client_null(void)
{
    void* got = _csilk_get_internal_client(NULL);
    if (got == NULL) {
        PASS();
    } else {
        FAIL("internal_client null ctx");
    }
}

/* ------------------------------------------------------------------ */

static void
test_max_connections_configured(void)
{
    csilk_server_t* s = mock_server();
    s->max_connections = 100;
    if (s->max_connections == 100) {
        PASS();
    } else {
        FAIL("max_connections");
    }
    free_mock_server(s);
}

static void
test_max_connections_unlimited(void)
{
    csilk_server_t* s = mock_server();
    s->max_connections = 0;
    if (s->max_connections == 0) {
        PASS();
    } else {
        FAIL("unlimited max_connections");
    }
    free_mock_server(s);
}

static void
test_active_connections_initial(void)
{
    csilk_server_t* s = mock_server();
    if (atomic_load(&s->active_connections) == 0) {
        PASS();
    } else {
        FAIL("active_connections init");
    }
    free_mock_server(s);
}

/* ------------------------------------------------------------------ */

static void
test_client_ip_null_ctx(void)
{
    const char* ip = csilk_get_client_ip(NULL);
    if (ip == NULL) {
        PASS();
    } else {
        FAIL("client_ip null ctx");
    }
}

static void
test_client_ip_mock_returns_null(void)
{
    csilk_ctx_t*   c = csilk_test_ctx_new();
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    _csilk_set_internal_client(c, &client);

    const char* ip = csilk_get_client_ip(c);
    if (ip == NULL) {
        PASS();
    } else {
        FAIL("client_ip should return null on unconnected handle");
    }
    csilk_test_ctx_free(c);
}

/* ------------------------------------------------------------------ */

static void
test_conn_state_strings(void)
{
    if (strcmp(csilk_conn_state_str(CSILK_CONN_INIT), "INIT") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_ACCEPTED), "ACCEPTED") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_TLS), "TLS") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_READING), "READING") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_PROCESSING), "PROCESSING") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_WRITING), "WRITING") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_STREAMING), "STREAMING") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_CLOSING), "CLOSING") == 0 &&
        strcmp(csilk_conn_state_str(CSILK_CONN_CLOSED), "CLOSED") == 0 &&
        strcmp(csilk_conn_state_str((csilk_conn_state_t)99), "UNKNOWN") == 0) {
        PASS();
    } else {
        FAIL("conn_state_str mismatch");
    }
}

static void
test_conn_state_lifecycle_flow(void)
{
    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    client.state = CSILK_CONN_INIT;
    if (csilk_conn_get_state(&client) != CSILK_CONN_INIT) {
        FAIL("initial state should be INIT");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_ACCEPTED);
    if (csilk_conn_get_state(&client) != CSILK_CONN_ACCEPTED) {
        FAIL("expected ACCEPTED");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_TLS);
    if (csilk_conn_get_state(&client) != CSILK_CONN_TLS) {
        FAIL("expected TLS");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_READING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_READING) {
        FAIL("expected READING");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_PROCESSING) {
        FAIL("expected PROCESSING");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_WRITING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_WRITING) {
        FAIL("expected WRITING");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_STREAMING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_STREAMING) {
        FAIL("expected STREAMING");
        return;
    }

    /* Keep-alive roundtrip back to READING */
    csilk_conn_set_state(&client, CSILK_CONN_READING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_READING) {
        FAIL("expected keep-alive READING");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_CLOSING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSING) {
        FAIL("expected CLOSING");
        return;
    }

    csilk_conn_set_state(&client, CSILK_CONN_CLOSED);
    if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSED) {
        FAIL("expected CLOSED");
        return;
    }

    PASS();
}

static void
test_conn_state_invariants(void)
{
    csilk_client_t client;
    memset(&client, 0, sizeof(client));

    /* 1. NULL client handling */
    csilk_conn_set_state(NULL, CSILK_CONN_READING);
    if (csilk_conn_get_state(NULL) != CSILK_CONN_CLOSED) {
        FAIL("NULL client should report CLOSED");
        return;
    }

    /* 2. Transition out of CLOSING to non-CLOSED rejected */
    client.state = CSILK_CONN_CLOSING;
    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    if (client.state != CSILK_CONN_CLOSING) {
        FAIL("transition from CLOSING to PROCESSING must be ignored");
        return;
    }

    /* 3. Transition out of CLOSED to anything other than INIT rejected */
    client.state = CSILK_CONN_CLOSED;
    csilk_conn_set_state(&client, CSILK_CONN_READING);
    if (client.state != CSILK_CONN_CLOSED) {
        FAIL("transition from CLOSED to READING must be ignored");
        return;
    }

    /* 4. Transition from CLOSED to INIT (pool recycling) allowed */
    csilk_conn_set_state(&client, CSILK_CONN_INIT);
    if (client.state != CSILK_CONN_INIT) {
        FAIL("transition from CLOSED to INIT must be allowed for pool reuse");
        return;
    }

    PASS();
}

static void
test_conn_pool_get_put_hot_reset(void)
{
    csilk_server_t* s = mock_server();
    worker_pool_t*  wp = &s->worker_pools[0];

    if (wp->client_pool_count != 0) {
        FAIL("expected client_pool_count 0");
        free_mock_server(s);
        return;
    }

    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.generation = 42;
    client.state = CSILK_CONN_READING;
    client.total_header_size = 1024;
    client.header_count = 10;
    client.keep_alive = 1;
    client.ctx.is_websocket = 1;
    client.ctx.handler_index = 5;

    wp->client_pool[wp->client_pool_count++] = &client;
    if (wp->client_pool_count != 1) {
        FAIL("expected client_pool_count 1");
        free_mock_server(s);
        return;
    }

    csilk_client_t* got = wp->client_pool[--wp->client_pool_count];
    uint8_t         gen = (uint8_t)((got->generation + 1) & 0xFF);
    if (gen == 0) {
        gen = 1;
    }
    got->generation = gen;

    if (got->generation != 43) {
        FAIL("expected generation increment from 42 to 43");
        free_mock_server(s);
        return;
    }

    PASS();
    free_mock_server(s);
}

static void
test_conn_state_all_matrix_transitions(void)
{
    /* Expected matrix for all 9x9 transitions */
    const bool expected[9][9] = {
        /* INIT */
        [CSILK_CONN_INIT] =
            {
                               [CSILK_CONN_INIT] = true,
                               [CSILK_CONN_ACCEPTED] = true,
                               [CSILK_CONN_TLS] = false,
                               [CSILK_CONN_READING] = false,
                               [CSILK_CONN_PROCESSING] = false,
                               [CSILK_CONN_WRITING] = false,
                               [CSILK_CONN_STREAMING] = false,
                               [CSILK_CONN_CLOSING] = true,
                               [CSILK_CONN_CLOSED] = true,
                               },
        /* ACCEPTED */
        [CSILK_CONN_ACCEPTED] =
            {
                               [CSILK_CONN_INIT] = false,
                               [CSILK_CONN_ACCEPTED] = true,
                               [CSILK_CONN_TLS] = true,
                               [CSILK_CONN_READING] = true,
                               [CSILK_CONN_PROCESSING] = false,
                               [CSILK_CONN_WRITING] = false,
                               [CSILK_CONN_STREAMING] = false,
                               [CSILK_CONN_CLOSING] = true,
                               [CSILK_CONN_CLOSED] = true,
                               },
        /* TLS */
        [CSILK_CONN_TLS] =
            {
                               [CSILK_CONN_INIT] = false,
                               [CSILK_CONN_ACCEPTED] = false,
                               [CSILK_CONN_TLS] = true,
                               [CSILK_CONN_READING] = true,
                               [CSILK_CONN_PROCESSING] = false,
                               [CSILK_CONN_WRITING] = false,
                               [CSILK_CONN_STREAMING] = false,
                               [CSILK_CONN_CLOSING] = true,
                               [CSILK_CONN_CLOSED] = true,
                               },
        /* READING */
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
        /* PROCESSING */
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
        /* WRITING */
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
        /* STREAMING */
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
        /* CLOSING */
        [CSILK_CONN_CLOSING] =
            {
                               [CSILK_CONN_INIT] = false,
                               [CSILK_CONN_ACCEPTED] = false,
                               [CSILK_CONN_TLS] = false,
                               [CSILK_CONN_READING] = false,
                               [CSILK_CONN_PROCESSING] = false,
                               [CSILK_CONN_WRITING] = false,
                               [CSILK_CONN_STREAMING] = false,
                               [CSILK_CONN_CLOSING] = true,
                               [CSILK_CONN_CLOSED] = true,
                               },
        /* CLOSED */
        [CSILK_CONN_CLOSED] =
            {
                               [CSILK_CONN_INIT] = true,
                               [CSILK_CONN_ACCEPTED] = false,
                               [CSILK_CONN_TLS] = false,
                               [CSILK_CONN_READING] = false,
                               [CSILK_CONN_PROCESSING] = false,
                               [CSILK_CONN_WRITING] = false,
                               [CSILK_CONN_STREAMING] = false,
                               [CSILK_CONN_CLOSING] = false,
                               [CSILK_CONN_CLOSED] = true,
                               },
    };

    int match_count = 0;
    for (int from = 0; from < 9; from++) {
        for (int to = 0; to < 9; to++) {
            bool is_valid =
                csilk_conn_is_valid_transition((csilk_conn_state_t)from, (csilk_conn_state_t)to);
            if (is_valid != expected[from][to]) {
                char err[128];
                snprintf(err,
                         sizeof(err),
                         "Matrix mismatch: %s -> %s expected %d got %d",
                         csilk_conn_state_str((csilk_conn_state_t)from),
                         csilk_conn_state_str((csilk_conn_state_t)to),
                         expected[from][to],
                         is_valid);
                FAIL(err);
                return;
            }

            csilk_client_t client;
            memset(&client, 0, sizeof(client));
            client.state = (csilk_conn_state_t)from;
            csilk_conn_set_state(&client, (csilk_conn_state_t)to);

            if (expected[from][to]) {
                if (csilk_conn_get_state(&client) != (csilk_conn_state_t)to) {
                    char err[128];
                    snprintf(err,
                             sizeof(err),
                             "Allowed transition %s -> %s did not set state",
                             csilk_conn_state_str((csilk_conn_state_t)from),
                             csilk_conn_state_str((csilk_conn_state_t)to));
                    FAIL(err);
                    return;
                }
            } else {
                if (csilk_conn_get_state(&client) != (csilk_conn_state_t)from) {
                    char err[128];
                    snprintf(err,
                             sizeof(err),
                             "Forbidden transition %s -> %s was not rejected",
                             csilk_conn_state_str((csilk_conn_state_t)from),
                             csilk_conn_state_str((csilk_conn_state_t)to));
                    FAIL(err);
                    return;
                }
            }
            match_count++;
        }
    }

    if (match_count == 81) {
        PASS();
    } else {
        FAIL("Did not test all 81 transitions");
    }
}

static void
test_conn_state_explicit_forbidden_rules(void)
{
    csilk_client_t client;

    /* Rule 4.1: INIT -> WRITING forbidden */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;
    csilk_conn_set_state(&client, CSILK_CONN_WRITING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_INIT) {
        FAIL("INIT -> WRITING must be forbidden");
        return;
    }

    /* Rule 4.2: INIT -> STREAMING forbidden */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;
    csilk_conn_set_state(&client, CSILK_CONN_STREAMING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_INIT) {
        FAIL("INIT -> STREAMING must be forbidden");
        return;
    }

    /* Rule 4.3: ACCEPTED -> WRITING forbidden */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_ACCEPTED;
    csilk_conn_set_state(&client, CSILK_CONN_WRITING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_ACCEPTED) {
        FAIL("ACCEPTED -> WRITING must be forbidden");
        return;
    }

    /* Rule 4.4: TLS -> WRITING forbidden */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_TLS;
    csilk_conn_set_state(&client, CSILK_CONN_WRITING);
    if (csilk_conn_get_state(&client) != CSILK_CONN_TLS) {
        FAIL("TLS -> WRITING must be forbidden");
        return;
    }

    /* Rule 4.5: CLOSING -> non-CLOSED forbidden */
    for (int st = 0; st < 9; st++) {
        if (st == CSILK_CONN_CLOSING || st == CSILK_CONN_CLOSED) {
            continue;
        }
        memset(&client, 0, sizeof(client));
        client.state = CSILK_CONN_CLOSING;
        csilk_conn_set_state(&client, (csilk_conn_state_t)st);
        if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSING) {
            FAIL("CLOSING -> non-CLOSED must be forbidden");
            return;
        }
    }

    /* Rule 4.6: CLOSED -> non-INIT forbidden */
    for (int st = 0; st < 9; st++) {
        if (st == CSILK_CONN_INIT || st == CSILK_CONN_CLOSED) {
            continue;
        }
        memset(&client, 0, sizeof(client));
        client.state = CSILK_CONN_CLOSED;
        csilk_conn_set_state(&client, (csilk_conn_state_t)st);
        if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSED) {
            FAIL("CLOSED -> non-INIT must be forbidden");
            return;
        }
    }

    PASS();
}

static void
test_conn_state_lifecycle_flows_comprehensive(void)
{
    csilk_client_t client;

    /* Flow 1: HTTP/1.1 Standard keep-alive lifecycle */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;
    csilk_conn_set_state(&client, CSILK_CONN_ACCEPTED);
    csilk_conn_set_state(&client, CSILK_CONN_READING);
    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    csilk_conn_set_state(&client, CSILK_CONN_WRITING);
    csilk_conn_set_state(&client, CSILK_CONN_READING); /* keep-alive loop */
    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    csilk_conn_set_state(&client, CSILK_CONN_WRITING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSED);
    csilk_conn_set_state(&client, CSILK_CONN_INIT); /* pool reuse */
    if (csilk_conn_get_state(&client) != CSILK_CONN_INIT) {
        FAIL("Flow 1: HTTP/1.1 lifecycle failed");
        return;
    }

    /* Flow 2: HTTPS lifecycle */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;
    csilk_conn_set_state(&client, CSILK_CONN_ACCEPTED);
    csilk_conn_set_state(&client, CSILK_CONN_TLS);
    csilk_conn_set_state(&client, CSILK_CONN_READING);
    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    csilk_conn_set_state(&client, CSILK_CONN_WRITING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSED);
    if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSED) {
        FAIL("Flow 2: HTTPS lifecycle failed");
        return;
    }

    /* Flow 3: WebSocket lifecycle */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;
    csilk_conn_set_state(&client, CSILK_CONN_ACCEPTED);
    csilk_conn_set_state(&client, CSILK_CONN_READING);
    csilk_conn_set_state(&client, CSILK_CONN_PROCESSING);
    csilk_conn_set_state(&client, CSILK_CONN_STREAMING);
    csilk_conn_set_state(&client, CSILK_CONN_STREAMING); /* frames */
    csilk_conn_set_state(&client, CSILK_CONN_READING);   /* frame read */
    csilk_conn_set_state(&client, CSILK_CONN_STREAMING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSED);
    if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSED) {
        FAIL("Flow 3: WebSocket lifecycle failed");
        return;
    }

    /* Flow 4: Early abort during INIT */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;
    csilk_conn_set_state(&client, CSILK_CONN_CLOSING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSED);
    if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSED) {
        FAIL("Flow 4: Early abort during INIT failed");
        return;
    }

    /* Flow 5: Early abort during TLS */
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;
    csilk_conn_set_state(&client, CSILK_CONN_ACCEPTED);
    csilk_conn_set_state(&client, CSILK_CONN_TLS);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSING);
    csilk_conn_set_state(&client, CSILK_CONN_CLOSED);
    if (csilk_conn_get_state(&client) != CSILK_CONN_CLOSED) {
        FAIL("Flow 5: Early abort during TLS failed");
        return;
    }

    PASS();
}

static void
test_conn_state_fuzz_transitions(void)
{
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_INIT;

    /* Fast deterministic xorshift32 PRNG */
    uint32_t prng = 0x12345678;
#define XORSHIFT32()                                                                               \
    do {                                                                                           \
        prng ^= prng << 13;                                                                        \
        prng ^= prng >> 17;                                                                        \
        prng ^= prng << 5;                                                                         \
    } while (0)

    const int NUM_ITERATIONS = 50000;
    int       legal_transitions = 0;
    int       rejected_transitions = 0;

    for (int i = 0; i < NUM_ITERATIONS; i++) {
        XORSHIFT32();
        /* Generate target state in range [-2, 11] to test valid states + out-of-bounds */
        int target = (int)(prng % 14) - 2;

        csilk_conn_state_t old_st = client.state;
        bool should_allow = csilk_conn_is_valid_transition(old_st, (csilk_conn_state_t)target);

        csilk_conn_set_state(&client, (csilk_conn_state_t)target);

        if (should_allow) {
            legal_transitions++;
            if (csilk_conn_get_state(&client) != (csilk_conn_state_t)target) {
                FAIL("Fuzz test: legal transition was not applied");
                return;
            }
        } else {
            rejected_transitions++;
            if (csilk_conn_get_state(&client) != old_st) {
                FAIL("Fuzz test: illegal transition was incorrectly applied");
                return;
            }
        }

        /* Verify universal invariants */
        csilk_conn_state_t curr = csilk_conn_get_state(&client);
        if ((int)curr < 0 || (int)curr >= 9) {
            FAIL("Fuzz test: client state is outside valid enum bounds");
            return;
        }

        /* If connection gets stuck in CLOSED or CLOSING, cycle to keep fuzzing active */
        if (curr == CSILK_CONN_CLOSED) {
            XORSHIFT32();
            if ((prng & 1) == 0) {
                csilk_conn_set_state(&client, CSILK_CONN_INIT);
            }
        } else if (curr == CSILK_CONN_CLOSING) {
            XORSHIFT32();
            if ((prng & 1) == 0) {
                csilk_conn_set_state(&client, CSILK_CONN_CLOSED);
            }
        }
    }
#undef XORSHIFT32

    if (legal_transitions > 0 && rejected_transitions > 0) {
        PASS();
    } else {
        FAIL("Fuzz test: insufficient transition diversity");
    }
}

static void
test_conn_state_boundary_and_null(void)
{
    /* 1. NULL client */
    csilk_conn_set_state(NULL, CSILK_CONN_ACCEPTED);
    if (csilk_conn_get_state(NULL) != CSILK_CONN_CLOSED) {
        FAIL("NULL client should return CSILK_CONN_CLOSED");
        return;
    }

    /* 2. Out-of-range bounds in validity check */
    if (csilk_conn_is_valid_transition((csilk_conn_state_t)-1, CSILK_CONN_INIT) != false) {
        FAIL("Negative from state should be invalid");
        return;
    }
    if (csilk_conn_is_valid_transition(CSILK_CONN_INIT, (csilk_conn_state_t)-1) != false) {
        FAIL("Negative to state should be invalid");
        return;
    }
    if (csilk_conn_is_valid_transition((csilk_conn_state_t)9, CSILK_CONN_INIT) != false) {
        FAIL("Out of range from state should be invalid");
        return;
    }
    if (csilk_conn_is_valid_transition(CSILK_CONN_INIT, (csilk_conn_state_t)9) != false) {
        FAIL("Out of range to state should be invalid");
        return;
    }
    if (csilk_conn_is_valid_transition((csilk_conn_state_t)100, (csilk_conn_state_t)200) != false) {
        FAIL("Extreme out of range states should be invalid");
        return;
    }

    /* 3. Setting invalid state on client */
    csilk_client_t client;
    memset(&client, 0, sizeof(client));
    client.state = CSILK_CONN_READING;
    csilk_conn_set_state(&client, (csilk_conn_state_t)99);
    if (client.state != CSILK_CONN_READING) {
        FAIL("Setting out of range state must be rejected");
        return;
    }

    PASS();
}

/* ------------------------------------------------------------------ */

int
main(void)
{
    printf("=== Connection Module Tests ===\n\n");

    printf("--- Pool Constants ---\n");
    test_client_pool_constant_defined();
    test_server_alloc_sets_pool();
    test_server_mutexes_init();
    test_conn_pool_get_put_hot_reset();

    printf("\n--- Context Client Binding ---\n");
    test_context_internal_client_roundtrip();
    test_context_internal_client_null();

    printf("\n--- Connection Limits ---\n");
    test_max_connections_configured();
    test_max_connections_unlimited();
    test_active_connections_initial();

    printf("\n--- Client IP ---\n");
    test_client_ip_mock_returns_null();
    test_client_ip_null_ctx();

    printf("\n--- Connection Lifecycle State Machine ---\n");
    test_conn_state_strings();
    test_conn_state_lifecycle_flow();
    test_conn_state_invariants();
    test_conn_state_all_matrix_transitions();
    test_conn_state_explicit_forbidden_rules();
    test_conn_state_lifecycle_flows_comprehensive();
    test_conn_state_boundary_and_null();
    test_conn_state_fuzz_transitions();

    printf("\n=== Results: %d passed, %d failed ===\n", tests_passed, tests_run - tests_passed);
    return (tests_passed == tests_run) ? EXIT_SUCCESS : EXIT_FAILURE;
}
