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

int
main(void)
{
    test_worker_pool_h3_fields_and_reset();
    printf("All HTTP/3 worker pool infrastructure tests passed!\n");
    return 0;
}
