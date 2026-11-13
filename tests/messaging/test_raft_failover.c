#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/messaging/raft.h"

static void
rm_rf_failover_dir(const char* dir)
{
    if (!dir || dir[0] == '\0') {
        return;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
    (void)system(cmd);
}

static int
wait_for_single_leader(csilk_raft_node_t* nodes[], int count, int skip_idx)
{
    for (int attempt = 0; attempt < 300; attempt++) {
        int leaders = 0;
        int found = -1;
        for (int i = 0; i < count; i++) {
            if (i == skip_idx || !nodes[i]) {
                continue;
            }
            if (csilk_raft_get_role(nodes[i]) == CSILK_RAFT_ROLE_LEADER) {
                leaders++;
                found = i;
            }
        }
        if (leaders == 1) {
            return found;
        }
        usleep(15000);
    }
    return -1;
}

static void
test_raft_cluster_failover_scenario(void)
{
    char dirs[3][64];
    for (int i = 0; i < 3; i++) {
        snprintf(dirs[i], sizeof(dirs[i]), "/tmp/csilk_raft_fo_%d_XXXXXX", i + 1);
        assert(mkdtemp(dirs[i]) != NULL);
    }

    csilk_raft_config_t cfgs[3];
    csilk_raft_node_t*  nodes[3] = {NULL, NULL, NULL};
    uint16_t            ports[3] = {0, 0, 0};

    for (uint32_t i = 0; i < 3; i++) {
        memset(&cfgs[i], 0, sizeof(cfgs[i]));
        cfgs[i].node_id = i + 1U;
        cfgs[i].cluster_size = 3;
        cfgs[i].wal_dir = dirs[i];
        cfgs[i].sync_policy = CSILK_RAFT_SYNC_ALWAYS;
        cfgs[i].heartbeat_interval_ms = 25;
        cfgs[i].election_timeout_min_ms = 80;
        cfgs[i].election_timeout_max_ms = 180;

        nodes[i] = csilk_raft_node_new(&cfgs[i]);
        assert(nodes[i] != NULL);
        assert(csilk_raft_node_listen(nodes[i], "127.0.0.1:0") == 0);
        ports[i] = csilk_raft_node_get_port(nodes[i]);
        assert(ports[i] > 0);
    }

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

    for (int i = 0; i < 3; i++) {
        assert(csilk_raft_node_start(nodes[i]) == 0);
    }

    int leader_idx = wait_for_single_leader(nodes, 3, -1);
    assert(leader_idx >= 0);
    uint64_t initial_term = csilk_raft_get_term(nodes[leader_idx]);

    /* Replicate 3 entries under initial leader */
    for (int k = 1; k <= 3; k++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "fo-cmd-%d", k);
        uint64_t idx = 0;
        assert(csilk_raft_replicate_wait(
                   nodes[leader_idx], (const uint8_t*)payload, strlen(payload), 2500, &idx) == 0);
        assert(idx == (uint64_t)k);
    }

    /* Simulate crashing the active leader */
    csilk_raft_node_free(nodes[leader_idx]);
    nodes[leader_idx] = NULL;

    /* Remaining 2 nodes must elect a new leader at a strictly higher term */
    int new_leader_idx = wait_for_single_leader(nodes, 3, leader_idx);
    assert(new_leader_idx >= 0);
    assert(new_leader_idx != leader_idx);
    assert(csilk_raft_get_term(nodes[new_leader_idx]) > initial_term);

    /* Replicate 3 more entries under new leader while 3rd node is down */
    for (int k = 4; k <= 6; k++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "fo-cmd-%d", k);
        uint64_t idx = 0;
        assert(csilk_raft_replicate_wait(
                   nodes[new_leader_idx], (const uint8_t*)payload, strlen(payload), 2500, &idx) ==
               0);
        assert(idx == (uint64_t)k);
    }

    /* Restart the crashed node from its persisted wal_dir and rejoin cluster */
    nodes[leader_idx] = csilk_raft_node_new(&cfgs[leader_idx]);
    assert(nodes[leader_idx] != NULL);
    char bind_same[64];
    snprintf(bind_same, sizeof(bind_same), "127.0.0.1:%u", ports[leader_idx]);
    assert(csilk_raft_node_listen(nodes[leader_idx], bind_same) == 0);
    for (uint32_t j = 0; j < 3; j++) {
        if ((int)j == leader_idx) {
            continue;
        }
        char id_str[16];
        char addr_str[64];
        snprintf(id_str, sizeof(id_str), "%u", j + 1U);
        snprintf(addr_str, sizeof(addr_str), "127.0.0.1:%u", ports[j]);
        assert(csilk_raft_node_add_peer(nodes[leader_idx], id_str, addr_str) == 0);
    }
    assert(csilk_raft_node_start(nodes[leader_idx]) == 0);

    /* Wait for restarted node to catch up all 6 committed entries */
    for (int attempt = 0; attempt < 200; attempt++) {
        if (csilk_raft_get_commit_index(nodes[leader_idx]) == 6) {
            break;
        }
        usleep(15000);
    }
    assert(csilk_raft_get_commit_index(nodes[leader_idx]) == 6);

    for (uint64_t idx = 1; idx <= 6; idx++) {
        char expected[32];
        snprintf(expected, sizeof(expected), "fo-cmd-%llu", (unsigned long long)idx);
        uint8_t* data = NULL;
        size_t   len = 0;
        assert(csilk_raft_get_log_entry(nodes[leader_idx], idx, NULL, &data, &len) == 0);
        assert(len == strlen(expected) && memcmp(data, expected, len) == 0);
        free(data);
    }

    for (int i = 0; i < 3; i++) {
        csilk_raft_node_free(nodes[i]);
        rm_rf_failover_dir(dirs[i]);
    }
    printf("test_raft_cluster_failover_scenario passed\n");
}

int
main(void)
{
    test_raft_cluster_failover_scenario();
    printf("All test_raft_failover tests passed successfully!\n");
    return 0;
}
