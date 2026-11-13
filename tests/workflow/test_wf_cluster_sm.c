#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/messaging/raft.h"

int csilk_wf_cluster_replicate_cmd(csilk_raft_node_t* node, uint8_t cmd_type, const char* payload);

static void
test_wf_cluster_state_machine(void)
{
    csilk_wf_manager_t* mgr = csilk_wf_manager_new();
    assert(mgr != nullptr);

    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != nullptr);

    /* Null safety checks */
    assert(csilk_wf_cluster_bind_raft(NULL, node) == -1);
    assert(csilk_wf_cluster_bind_raft(mgr, NULL) == -1);
    assert(csilk_wf_cluster_replicate_cmd(NULL, 0x11, "cmd") == -1);
    assert(csilk_wf_cluster_replicate_cmd(node, 0x11, NULL) == -1);

    int res = csilk_wf_cluster_bind_raft(mgr, node);
    assert(res == 0);

    /* Follower node cannot replicate commands (rejects non-leader writes) */
    assert(csilk_wf_cluster_replicate_cmd(node, 0x11, "{\"node\":\"ai_node_1\"}") == -1);

    /* Start node (single node with 0 peers becomes leader automatically) */
    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_LEADER);

    /* Leader mode replicates commands into Raft WAL */
    res = csilk_wf_cluster_replicate_cmd(node, 0x11, "{\"node\":\"ai_node_1\"}");
    assert(res == 0);

    csilk_raft_node_free(node);
    csilk_wf_manager_free(mgr);
    printf("test_wf_cluster_state_machine passed\n");
}

int
main(void)
{
    test_wf_cluster_state_machine();
    printf("All test_wf_cluster_sm tests passed successfully!\n");
    return 0;
}
