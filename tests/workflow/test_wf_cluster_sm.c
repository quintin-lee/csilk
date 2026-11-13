#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/app/workflow_dsl.h"
#include "csilk/messaging/raft.h"

static void
rm_rf_wf_dir(const char* dir)
{
    if (!dir || dir[0] == '\0') {
        return;
    }
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
    (void)system(cmd);
}

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

static void
test_wf_cluster_kill_restart_wal_and_snapshot_recovery(void)
{
    char wal_dir[] = "/tmp/csilk_wf_sm_XXXXXX";
    assert(mkdtemp(wal_dir) != NULL);

    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    cfg.wal_dir = wal_dir;
    cfg.sync_policy = CSILK_RAFT_SYNC_ALWAYS;

    csilk_wf_manager_t* mgr = csilk_wf_manager_new();
    csilk_raft_node_t*  node = csilk_raft_node_new(&cfg);
    assert(mgr != nullptr && node != nullptr);
    assert(csilk_wf_cluster_bind_raft(mgr, node) == 0);
    assert(csilk_raft_node_start(node) == 0);

    assert(csilk_wf_cluster_replicate_cmd_wait(node, 0x10, "wf_step_1", 1000) == 0);
    assert(csilk_wf_cluster_replicate_cmd_wait(node, 0x11, "wf_step_2", 1000) == 0);
    assert(csilk_wf_cluster_replicate_cmd_wait(node, 0x12, "wf_step_3", 1000) == 0);
    assert(csilk_wf_cluster_applied_count(mgr) == 3);

    /* Create snapshot capturing state at step 3, then append step 4 into WAL */
    const uint8_t snap_frame[] = {0x12, 'w', 'f', '_', 's', 't', 'e', 'p', '_', '3'};
    assert(csilk_raft_snapshot_create(node, snap_frame, sizeof(snap_frame)) == 0);
    assert(csilk_wf_cluster_replicate_cmd_wait(node, 0x13, "wf_step_4", 1000) == 0);
    assert(csilk_wf_cluster_applied_count(mgr) == 4);

    /* Simulate process kill */
    csilk_raft_node_free(node);
    csilk_wf_manager_free(mgr);

    /* Restart fresh workflow manager + raft node from persisted wal_dir */
    csilk_wf_manager_t* mgr2 = csilk_wf_manager_new();
    csilk_raft_node_t*  node2 = csilk_raft_node_new(&cfg);
    assert(mgr2 != nullptr && node2 != nullptr);

    /* Binding replays recovered snapshot immediately */
    assert(csilk_wf_cluster_bind_raft(mgr2, node2) == 0);
    uint8_t cmd_type = 0;
    char    payload[128];
    assert(csilk_wf_cluster_get_last_cmd(mgr2, &cmd_type, payload, sizeof(payload)) == 0);
    assert(cmd_type == 0x12 && strcmp(payload, "wf_step_3") == 0);

    /* Re-promote single node and commit tail WAL entry #4 + new entry #5 */
    assert(csilk_raft_node_start(node2) == 0);
    assert(csilk_wf_cluster_replicate_cmd_wait(node2, 0x14, "wf_step_5", 1000) == 0);
    assert(csilk_wf_cluster_get_last_cmd(mgr2, &cmd_type, payload, sizeof(payload)) == 0);
    assert(cmd_type == 0x14 && strcmp(payload, "wf_step_5") == 0);

    csilk_raft_node_free(node2);
    csilk_wf_manager_free(mgr2);
    rm_rf_wf_dir(wal_dir);
    printf("test_wf_cluster_kill_restart_wal_and_snapshot_recovery passed\n");
}

static void
test_wf_cluster_three_node_failover_replication(void)
{
    char dirs[3][64];
    for (int i = 0; i < 3; i++) {
        snprintf(dirs[i], sizeof(dirs[i]), "/tmp/csilk_wf_ha_%d_XXXXXX", i + 1);
        assert(mkdtemp(dirs[i]) != NULL);
    }

    csilk_wf_manager_t* mgrs[3] = {NULL, NULL, NULL};
    csilk_raft_node_t*  nodes[3] = {NULL, NULL, NULL};
    uint16_t            ports[3] = {0, 0, 0};

    for (uint32_t i = 0; i < 3; i++) {
        csilk_raft_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.node_id = i + 1U;
        cfg.cluster_size = 3;
        cfg.wal_dir = dirs[i];
        cfg.heartbeat_interval_ms = 25;
        cfg.election_timeout_min_ms = 80;
        cfg.election_timeout_max_ms = 180;

        mgrs[i] = csilk_wf_manager_new();
        nodes[i] = csilk_raft_node_new(&cfg);
        assert(mgrs[i] != nullptr && nodes[i] != nullptr);
        assert(csilk_wf_cluster_bind_raft(mgrs[i], nodes[i]) == 0);
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

    int leader = -1;
    for (int attempt = 0; attempt < 250; attempt++) {
        int leaders = 0;
        int found = -1;
        for (int i = 0; i < 3; i++) {
            if (csilk_raft_get_role(nodes[i]) == CSILK_RAFT_ROLE_LEADER) {
                leaders++;
                found = i;
            }
        }
        if (leaders == 1) {
            leader = found;
            break;
        }
        usleep(15000);
    }
    assert(leader >= 0);

    assert(csilk_wf_cluster_replicate_cmd_wait(nodes[leader], 0x21, "wf_deploy_v1", 2500) == 0);

    for (int attempt = 0; attempt < 150; attempt++) {
        int all_applied = 1;
        for (int i = 0; i < 3; i++) {
            if (csilk_wf_cluster_applied_count(mgrs[i]) < 1) {
                all_applied = 0;
                break;
            }
        }
        if (all_applied) {
            break;
        }
        usleep(15000);
    }
    for (int i = 0; i < 3; i++) {
        uint8_t cmd_type = 0;
        char    payload[128];
        assert(csilk_wf_cluster_get_last_cmd(mgrs[i], &cmd_type, payload, sizeof(payload)) == 0);
        assert(cmd_type == 0x21 && strcmp(payload, "wf_deploy_v1") == 0);
    }

    /* Kill leader and wait for failover */
    csilk_raft_node_free(nodes[leader]);
    nodes[leader] = NULL;

    int new_leader = -1;
    for (int attempt = 0; attempt < 250; attempt++) {
        int leaders = 0;
        int found = -1;
        for (int i = 0; i < 3; i++) {
            if (nodes[i] && csilk_raft_get_role(nodes[i]) == CSILK_RAFT_ROLE_LEADER) {
                leaders++;
                found = i;
            }
        }
        if (leaders == 1) {
            new_leader = found;
            break;
        }
        usleep(15000);
    }
    assert(new_leader >= 0 && new_leader != leader);

    assert(csilk_wf_cluster_replicate_cmd_wait(nodes[new_leader], 0x22, "wf_deploy_v2", 2500) == 0);

    for (int attempt = 0; attempt < 150; attempt++) {
        int all_applied = 1;
        for (int i = 0; i < 3; i++) {
            if (nodes[i] && csilk_wf_cluster_applied_count(mgrs[i]) < 2) {
                all_applied = 0;
                break;
            }
        }
        if (all_applied) {
            break;
        }
        usleep(15000);
    }
    for (int i = 0; i < 3; i++) {
        if (!nodes[i]) {
            continue;
        }
        uint8_t cmd_type = 0;
        char    payload[128];
        assert(csilk_wf_cluster_get_last_cmd(mgrs[i], &cmd_type, payload, sizeof(payload)) == 0);
        assert(cmd_type == 0x22 && strcmp(payload, "wf_deploy_v2") == 0);
    }

    for (int i = 0; i < 3; i++) {
        csilk_raft_node_free(nodes[i]);
        csilk_wf_manager_free(mgrs[i]);
        rm_rf_wf_dir(dirs[i]);
    }
    printf("test_wf_cluster_three_node_failover_replication passed\n");
}

int
main(void)
{
    test_wf_cluster_state_machine();
    test_wf_cluster_kill_restart_wal_and_snapshot_recovery();
    test_wf_cluster_three_node_failover_replication();
    printf("All test_wf_cluster_sm tests passed successfully!\n");
    return 0;
}
