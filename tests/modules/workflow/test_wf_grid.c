/**
 * @file test_wf_grid.c
 * @brief Unit test for Decentralized Multi-Agent Task Grid and Work-Stealing.
 */

#include "csilk/csilk.h"
#include "csilk/app/wf_grid.h"
#include "csilk/messaging/raft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int
main(void)
{
    printf("=== Test: Decentralized Multi-Agent Work-Stealing Grid ===\n");

    /* Create two cluster grid nodes */
    csilk_wf_grid_t* grid_a = csilk_wf_grid_new("node-alpha", NULL, NULL);
    csilk_wf_grid_t* grid_b = csilk_wf_grid_new("node-beta", NULL, NULL);
    assert(grid_a != NULL);
    assert(grid_b != NULL);

    /* Push tasks to node A */
    int rc1 = csilk_wf_grid_push_task(grid_a, "task-001", "{\"step\":\"crawl\"}", 16);
    int rc2 = csilk_wf_grid_push_task(grid_a, "task-002", "{\"step\":\"index\"}", 16);
    assert(rc1 == 0);
    assert(rc2 == 0);
    assert(csilk_wf_grid_task_count(grid_a) == 2);
    assert(csilk_wf_grid_task_count(grid_b) == 0);

    /* Node B steals from Node A (stolen from Top of deque) */
    char stolen_id[64] = {0};
    int  steal_rc = csilk_wf_grid_steal_from_peer(grid_b, grid_a, stolen_id, sizeof(stolen_id));
    assert(steal_rc == 0);
    assert(strcmp(stolen_id, "task-001") == 0);
    assert(csilk_wf_grid_task_count(grid_a) == 1);
    assert(csilk_wf_grid_task_count(grid_b) == 1);

    /* Local pop on Node A (pops from Bottom of deque: task-002) */
    char popped_id[64] = {0};
    int  pop_rc = csilk_wf_grid_pop_task(grid_a, popped_id, sizeof(popped_id));
    assert(pop_rc == 0);
    assert(strcmp(popped_id, "task-002") == 0);
    assert(csilk_wf_grid_task_count(grid_a) == 0);

    /* Steal from empty node fails cleanly */
    int empty_rc = csilk_wf_grid_steal_from_peer(grid_b, grid_a, stolen_id, sizeof(stolen_id));
    assert(empty_rc == -1);

    /* Test Rebalance: Node B holds a task, simulate Node B departure */
    assert(csilk_wf_grid_task_count(grid_b) == 1);
    int rebalance_rc = csilk_wf_grid_rebalance(grid_a, "node-beta");
    assert(rebalance_rc == 0);

    csilk_wf_grid_free(grid_a);
    csilk_wf_grid_free(grid_b);

    /* Test Grid ↔ Raft Topology Auto-Discovery */
    printf("=== Test: Grid ↔ Raft Topology Auto-Discovery ===\n");
    csilk_raft_config_t rcfg;
    memset(&rcfg, 0, sizeof(rcfg));
    rcfg.node_id = 1;
    rcfg.cluster_size = 3;

    csilk_raft_node_t* raft_node = csilk_raft_node_new(&rcfg);
    assert(raft_node != NULL);
    assert(csilk_raft_node_add_peer(raft_node, "node-peer-1", "127.0.0.1:9001") == 0);

    csilk_wf_grid_t* grid_raft = csilk_wf_grid_new("node-local", raft_node, NULL);
    assert(grid_raft != NULL);
    assert(csilk_wf_grid_peer_count(grid_raft) == 0);

    /* Sync initial topology */
    assert(csilk_wf_grid_sync_raft_topology(grid_raft) == 0);
    assert(csilk_wf_grid_has_peer(grid_raft, "node-peer-1") == true);
    assert(csilk_wf_grid_peer_count(grid_raft) == 1);

    /* Dynamically add new peer to Raft, verify Grid discovers it automatically */
    assert(csilk_raft_node_add_peer(raft_node, "node-peer-2", "127.0.0.1:9002") == 0);
    assert(csilk_wf_grid_has_peer(grid_raft, "node-peer-2") == true);
    assert(csilk_wf_grid_peer_count(grid_raft) == 2);

    csilk_wf_grid_free(grid_raft);
    csilk_raft_node_free(raft_node);

    printf("PASS: test_wf_grid\n");
    return 0;
}
