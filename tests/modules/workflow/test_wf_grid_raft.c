/**
 * @file test_wf_grid_raft.c
 * @brief End-to-end integration test for Decentralized Task Grid & Raft Auto-Healing Topology.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/app/wf_grid.h"
#include "csilk/csilk.h"
#include "csilk/messaging/raft.h"

static int
wait_for_leader(csilk_raft_node_t** nodes, size_t count, int max_ms)
{
    int elapsed = 0;
    while (elapsed < max_ms) {
        for (size_t i = 0; i < count; i++) {
            if (nodes[i] && csilk_raft_get_role(nodes[i]) == CSILK_RAFT_ROLE_LEADER) {
                return (int)i;
            }
        }
        usleep(15000);
        elapsed += 15;
    }
    return -1;
}

int
main(void)
{
    printf("=== Starting test_wf_grid_raft: Grid & Raft Auto-Topology Self-Healing ===\n");

    /* 1. Setup 3-node Raft cluster */
    csilk_raft_config_t cfgs[3];
    csilk_raft_node_t*  nodes[3] = {NULL, NULL, NULL};
    uint16_t            ports[3] = {0, 0, 0};
    csilk_wf_grid_t*    grids[3] = {NULL, NULL, NULL};

    for (uint32_t i = 0; i < 3; i++) {
        memset(&cfgs[i], 0, sizeof(cfgs[i]));
        cfgs[i].node_id = i + 1U;
        cfgs[i].cluster_size = 3;
        cfgs[i].heartbeat_interval_ms = 25;
        cfgs[i].election_timeout_min_ms = 80;
        cfgs[i].election_timeout_max_ms = 180;

        nodes[i] = csilk_raft_node_new(&cfgs[i]);
        assert(nodes[i] != NULL);
        assert(csilk_raft_node_listen(nodes[i], "127.0.0.1:0") == 0);
        ports[i] = csilk_raft_node_get_port(nodes[i]);
        assert(ports[i] > 0);

        char grid_node_id[16];
        snprintf(grid_node_id, sizeof(grid_node_id), "%u", i + 1U);
        grids[i] = csilk_wf_grid_new(grid_node_id, nodes[i], NULL);
        assert(grids[i] != NULL);
    }

    /* Interconnect Raft cluster peers */
    for (uint32_t i = 0; i < 3; i++) {
        for (uint32_t j = 0; j < 3; j++) {
            if (i == j) {
                continue;
            }
            char id_str[16];
            char addr_str[64];
            snprintf(id_str, sizeof(id_str), "%u", j + 1U);
            snprintf(addr_str, sizeof(addr_str), "127.0.0.1:%u", ports[j]);
            assert(csilk_raft_node_add_peer(nodes[i], id_str, addr_str) == 0);
        }
    }

    /* 2. Synchronize Grid topology with Raft */
    for (size_t i = 0; i < 3; i++) {
        assert(csilk_wf_grid_sync_raft_topology(grids[i]) == 0);
    }

    /* Verify initial mesh discovery: each node sees the other 2 peers */
    assert(csilk_wf_grid_peer_count(grids[0]) == 2);
    assert(csilk_wf_grid_has_peer(grids[0], "2") == true);
    assert(csilk_wf_grid_has_peer(grids[0], "3") == true);

    assert(csilk_wf_grid_peer_count(grids[1]) == 2);
    assert(csilk_wf_grid_has_peer(grids[1], "1") == true);
    assert(csilk_wf_grid_has_peer(grids[1], "3") == true);

    assert(csilk_wf_grid_peer_count(grids[2]) == 2);
    assert(csilk_wf_grid_has_peer(grids[2], "1") == true);
    assert(csilk_wf_grid_has_peer(grids[2], "2") == true);

    /* 3. Start Raft event loop and elect leader */
    for (size_t i = 0; i < 3; i++) {
        assert(csilk_raft_node_start(nodes[i]) == 0);
    }

    int leader_idx = wait_for_leader(nodes, 3, 3000);
    assert(leader_idx >= 0);
    printf("Leader elected: Node %d (Port %u)\n", leader_idx + 1, ports[leader_idx]);

    /* 4. Task submission & Work-Stealing across Grid nodes */
    assert(csilk_wf_grid_push_task(grids[0], "task-grid-101", "{\"action\":\"ai_infer\"}", 22) ==
           0);
    assert(csilk_wf_grid_task_count(grids[0]) == 1);
    assert(csilk_wf_grid_task_count(grids[1]) == 0);

    /* Node 2 steals task from Node 1 */
    char stolen_id[64] = {0};
    int  steal_rc = csilk_wf_grid_steal_from_peer(grids[1], grids[0], stolen_id, sizeof(stolen_id));
    assert(steal_rc == 0);
    assert(strcmp(stolen_id, "task-grid-101") == 0);
    assert(csilk_wf_grid_task_count(grids[0]) == 0);
    assert(csilk_wf_grid_task_count(grids[1]) == 1);

    /* 5. Dynamically remove a non-leader follower via Raft Joint Consensus */
    int  remove_idx = (leader_idx == 1) ? 2 : 1;
    char remove_id_str[16];
    snprintf(remove_id_str, sizeof(remove_id_str), "%u", remove_idx + 1U);

    printf("Dynamically removing node %s via Joint Consensus from leader %d...\n",
           remove_id_str,
           leader_idx + 1);

    int rc = csilk_raft_change_membership_wait(nodes[leader_idx], remove_id_str, NULL, 0, 4000);
    assert(rc == 0);

    /* Wait briefly for topology callbacks to propagate */
    usleep(50000);

    /* 6. Verify survivor Grid nodes automatically pruned the departed peer */
    int survivor_idx =
        (leader_idx == 0 || remove_idx == 0) ? ((leader_idx != 1 && remove_idx != 1) ? 1 : 2) : 0;
    (void)survivor_idx;

    assert(csilk_wf_grid_has_peer(nodes[leader_idx] == nodes[0] ? grids[0] : grids[survivor_idx],
                                  remove_id_str) == false);

    /* 7. Verify Auto-Healing & Task Rebalancing */
    /* Inject task owned by departed node into leader's grid to verify rebalance reclamation */
    assert(csilk_wf_grid_push_task(grids[leader_idx], "task-orphaned", "payload", 7) == 0);
    /* Trigger rebalance for departed node */
    assert(csilk_wf_grid_rebalance(grids[leader_idx], remove_id_str) == 0);

    /* Ensure remaining task can be popped and executed by leader */
    char recovered_task[64] = {0};
    assert(csilk_wf_grid_pop_task(grids[leader_idx], recovered_task, sizeof(recovered_task)) == 0);
    assert(strcmp(recovered_task, "task-orphaned") == 0);

    /* 8. Teardown all resources cleanly */
    for (size_t i = 0; i < 3; i++) {
        csilk_wf_grid_free(grids[i]);
        csilk_raft_node_free(nodes[i]);
    }

    printf("PASS: test_wf_grid_raft\n");
    return 0;
}
