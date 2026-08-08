#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/messaging/raft.h"

static void
test_raft_cluster_failover_scenario(void)
{
    csilk_raft_config_t cfg1, cfg2, cfg3;
    memset(&cfg1, 0, sizeof(cfg1));
    memset(&cfg2, 0, sizeof(cfg2));
    memset(&cfg3, 0, sizeof(cfg3));

    cfg1.node_id = 1;
    cfg1.cluster_size = 3;
    cfg2.node_id = 2;
    cfg2.cluster_size = 3;
    cfg3.node_id = 3;
    cfg3.cluster_size = 3;

    csilk_raft_node_t* n1 = csilk_raft_node_new(&cfg1);
    csilk_raft_node_t* n2 = csilk_raft_node_new(&cfg2);
    csilk_raft_node_t* n3 = csilk_raft_node_new(&cfg3);

    assert(n1 && n2 && n3);

    csilk_raft_node_add_peer(n1, "2", "127.0.0.1:9002");
    csilk_raft_node_add_peer(n1, "3", "127.0.0.1:9003");

    csilk_raft_node_start(n1);
    csilk_raft_node_start(n2);
    csilk_raft_node_start(n3);

    /* Simulate killing active leader n1 */
    csilk_raft_node_free(n1);

    /* Promote n2 to leader as candidate quorum winner */
    csilk_raft_node_start(n2);
    assert(csilk_raft_get_role(n2) == CSILK_RAFT_ROLE_LEADER);

    csilk_raft_node_free(n2);
    csilk_raft_node_free(n3);
    printf("test_raft_cluster_failover_scenario passed\n");
}

int
main(void)
{
    test_raft_cluster_failover_scenario();
    printf("All test_raft_failover tests passed successfully!\n");
    return 0;
}
