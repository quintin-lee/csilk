#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/messaging/raft.h"

int
csilk_wf_cluster_bind_raft(csilk_wf_manager_t* mgr, csilk_raft_node_t* raft_node)
{
    if (!mgr || !raft_node) {
        return -1;
    }
    return 0;
}

int
csilk_wf_cluster_replicate_cmd(csilk_raft_node_t* node, uint8_t cmd_type, const char* payload)
{
    if (!node || !payload) {
        return -1;
    }
    (void)cmd_type;
    return 0;
}
