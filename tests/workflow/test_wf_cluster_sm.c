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

    int res = csilk_wf_cluster_bind_raft(mgr, node);
    assert(res == 0);

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
