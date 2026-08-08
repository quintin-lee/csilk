#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/messaging/raft.h"

static void
test_raft_consensus_single_node(void)
{
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != nullptr);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_FOLLOWER);

    int res = csilk_raft_node_start(node);
    assert(res == 0);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_LEADER);

    csilk_raft_node_free(node);
    printf("test_raft_consensus_single_node passed\n");
}

int
main(void)
{
    test_raft_consensus_single_node();
    printf("All test_raft_consensus tests passed successfully!\n");
    return 0;
}
