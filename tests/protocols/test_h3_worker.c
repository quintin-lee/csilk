#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/protocols/h3.h"
#include "csilk/test/test.h"
#include "core/internal/srv_internal.h"
#include "core/internal/srv_impl.h"
#include "protocols/http3_internal.h"

static void
test_worker_pool_h3_fields_and_reset(void)
{
    printf("Testing worker_pool_t HTTP/3 fields and hot-state reset...\n");

    csilk_server_t* server = csilk_server_new(NULL);
    assert(server != NULL);

    /* Verify server H3 fields exist */
    server->h3_port = 4433;
    assert(server->h3_port == 4433);
    assert(server->h3_ssl_ctx == NULL);

    /* Setup a dummy worker pool */
    worker_pool_t wp;
    memset(&wp, 0, sizeof(wp));
    wp.server = server;
    atomic_init(&wp.client_pool_count, 0);
    atomic_init(&wp.active_connections, 0);

    assert(wp.h3_udp_active == false);
    assert(wp.h3_transport == NULL);

    /* Get client and attach dummy h3_conn */
    csilk_client_t* client = pool_get(&wp);
    assert(client != NULL);
    assert(client->generation >= 1);
    assert(csilk_conn_get_state(client) == CSILK_CONN_INIT);

    int dummy_h3 = 42;
    client->h3_conn = &dummy_h3;
    client->protocol = CSILK_PROTO_HTTP3;

    /* Return to pool; hot reset must clear h3_conn */
    pool_put(&wp, client);
    assert(client->h3_conn == NULL);
    assert(client->protocol == CSILK_PROTO_HTTP1);

    /* Re-acquire client from pool */
    csilk_client_t* recycled = pool_get(&wp);
    assert(recycled == client);
    assert(recycled->h3_conn == NULL);

    pool_put(&wp, recycled);
    csilk_server_free(server);

    printf("test_worker_pool_h3_fields_and_reset: PASS\n");
}

#ifdef CSILK_HAS_NGTCP2
static void
test_h3_conn_pool_lifecycle(void)
{
    printf("Testing HTTP/3 connection pool integration and generation tracking...\n");

    csilk_server_t* server = csilk_server_new(NULL);
    assert(server != NULL);

    worker_pool_t wp;
    memset(&wp, 0, sizeof(wp));
    wp.server = server;
    atomic_init(&wp.client_pool_count, 0);
    atomic_init(&wp.active_connections, 0);
    _csilk_worker_set_current_pool(&wp);

    csilk_h3_transport_t t;
    memset(&t, 0, sizeof(t));
    t.server = server;
    t.wp = &wp;

    ngtcp2_cid dcid;
    dcid.datalen = 8;
    memcpy(dcid.data, "12345678", 8);

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(12345);

    /* 1. Allocate a new HTTP/3 connection under worker pool */
    struct csilk_h3_conn_s* conn =
        _h3_new_conn(&t, &dcid, (struct sockaddr*)&remote, sizeof(remote));
    assert(conn != NULL);
    assert(conn->client != NULL);

    csilk_client_t* cl = (csilk_client_t*)conn->client;
    assert(cl->protocol == CSILK_PROTO_HTTP3);
    assert(cl->owner_pool == &wp);
    assert(cl->server == server);
    assert(cl->h3_conn == conn);
    assert(cl->generation >= 1);
    assert(csilk_conn_get_state(cl) == CSILK_CONN_ACCEPTED);
    assert(atomic_load(&wp.active_connections) == 1);
    assert(wp.active_clients == cl);

    /* 2. Free connection: must set CLOSING, unlink from active, and return to pool */
    _h3_del_conn(&t, conn);
    assert(atomic_load(&wp.active_connections) == 0);
    assert(wp.active_clients == NULL);
    assert(atomic_load(&wp.client_pool_count) == 1);
    assert(wp.client_pool[0] == cl);

    _csilk_worker_set_current_pool(NULL);
    csilk_server_free(server);

    printf("test_h3_conn_pool_lifecycle: PASS\n");
}

static void
test_h3_multi_worker_udp_reuseport(void)
{
    printf("Testing HTTP/3 multi-worker SO_REUSEPORT binding & teardown...\n");

    csilk_server_t* server = csilk_server_new(NULL);
    assert(server != NULL);
    server->h3_port = 24433;

    csilk_io_loop_t loop0;
    csilk_io_loop_init(&loop0);
    csilk_io_loop_t loop1;
    csilk_io_loop_init(&loop1);

    worker_pool_t wp0;
    memset(&wp0, 0, sizeof(wp0));
    wp0.server = server;
    wp0.worker_index = 0;
    wp0.loop_ptr = &loop0;

    worker_pool_t wp1;
    memset(&wp1, 0, sizeof(wp1));
    wp1.server = server;
    wp1.worker_index = 1;
    wp1.loop_ptr = &loop1;

    /* Bind worker 0 */
    int r0 = _csilk_worker_init_h3(server, &wp0, &loop0);
    assert(r0 == 0);
    assert(wp0.h3_udp_active == true);
    assert(wp0.h3_transport != NULL);

    /* Bind worker 1 to the same port using SO_REUSEPORT */
    int r1 = _csilk_worker_init_h3(server, &wp1, &loop1);
    assert(r1 == 0);
    assert(wp1.h3_udp_active == true);
    assert(wp1.h3_transport != NULL);

    /* Stop both workers */
    _csilk_worker_stop_h3(&wp0);
    assert(wp0.h3_udp_active == false);
    assert(wp0.h3_transport == NULL);

    _csilk_worker_stop_h3(&wp1);
    assert(wp1.h3_udp_active == false);
    assert(wp1.h3_transport == NULL);

    for (int i = 0; i < 10 && csilk_io_loop_alive(&loop0); i++) {
        csilk_io_run(&loop0, CSILK_IO_RUN_NOWAIT);
    }
    csilk_io_loop_close(&loop0);
    for (int i = 0; i < 10 && csilk_io_loop_alive(&loop1); i++) {
        csilk_io_run(&loop1, CSILK_IO_RUN_NOWAIT);
    }
    csilk_io_loop_close(&loop1);

    csilk_server_free(server);
    printf("test_h3_multi_worker_udp_reuseport: PASS\n");
}

static void
test_h3_shutdown_drain_active_clients(void)
{
    printf("Testing HTTP/3 graceful shutdown active client draining...\n");

    csilk_server_t* server = csilk_server_new(NULL);
    assert(server != NULL);
    server->h3_port = 24434;

    csilk_io_loop_t loop;
    csilk_io_loop_init(&loop);

    worker_pool_t wp;
    memset(&wp, 0, sizeof(wp));
    wp.server = server;
    wp.worker_index = 0;
    wp.loop_ptr = &loop;
    _csilk_worker_set_current_pool(&wp);

    int r = _csilk_worker_init_h3(server, &wp, &loop);
    assert(r == 0);

    ngtcp2_cid dcid;
    dcid.datalen = 8;
    memcpy(dcid.data, "87654321", 8);

    struct sockaddr_in remote;
    memset(&remote, 0, sizeof(remote));
    remote.sin_family = AF_INET;
    remote.sin_port = htons(54321);

    /* Allocate an H3 connection */
    struct csilk_h3_conn_s* conn = _h3_new_conn(
        (csilk_h3_transport_t*)wp.h3_transport, &dcid, (struct sockaddr*)&remote, sizeof(remote));
    assert(conn != NULL);
    assert(wp.active_clients != NULL);
    csilk_client_t* cl = wp.active_clients;
    assert(cl->protocol == CSILK_PROTO_HTTP3);

    /* Simulate shutdown closing active clients via csilk_h3_conn_close */
    csilk_h3_conn_close(cl);
    assert(wp.active_clients == NULL);
    assert(atomic_load(&wp.client_pool_count) == 1);

    _csilk_worker_stop_h3(&wp);
    for (int i = 0; i < 10 && csilk_io_loop_alive(&loop); i++) {
        csilk_io_run(&loop, CSILK_IO_RUN_NOWAIT);
    }
    csilk_io_loop_close(&loop);

    _csilk_worker_set_current_pool(NULL);
    csilk_server_free(server);

    printf("test_h3_shutdown_drain_active_clients: PASS\n");
}
#endif

int
main(void)
{
    test_worker_pool_h3_fields_and_reset();
#ifdef CSILK_HAS_NGTCP2
    test_h3_conn_pool_lifecycle();
    test_h3_multi_worker_udp_reuseport();
    test_h3_shutdown_drain_active_clients();
#endif
    printf("All HTTP/3 worker pool infrastructure tests passed!\n");
    return 0;
}
