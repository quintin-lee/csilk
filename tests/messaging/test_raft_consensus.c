#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/messaging/raft.h"
#include "messaging/raft_internal.h"

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

static void
test_raft_randomized_election_timeout(void)
{
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cluster_size = 3;
    cfg.election_timeout_min_ms = 120;
    cfg.election_timeout_max_ms = 280;

    uint64_t timeouts[5];
    int      has_diff = 0;
    for (uint32_t i = 0; i < 5; i++) {
        cfg.node_id = i + 1U;
        csilk_raft_node_t* n = csilk_raft_node_new(&cfg);
        assert(n != nullptr);
        timeouts[i] = n->election_timeout_ms;
        assert(timeouts[i] >= 120 && timeouts[i] <= 280);
        if (i > 0 && timeouts[i] != timeouts[0]) {
            has_diff = 1;
        }
        csilk_raft_node_free(n);
    }
    assert(has_diff == 1);
    printf("test_raft_randomized_election_timeout passed\n");
}

static void
test_raft_three_node_election_and_replication(void)
{
    csilk_raft_node_t* nodes[3] = {NULL, NULL, NULL};
    uint16_t           ports[3] = {0, 0, 0};

    for (uint32_t i = 0; i < 3; i++) {
        csilk_raft_config_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        cfg.node_id = i + 1U;
        cfg.cluster_size = 3;
        cfg.heartbeat_interval_ms = 25;
        cfg.election_timeout_min_ms = 80;
        cfg.election_timeout_max_ms = 180;
        nodes[i] = csilk_raft_node_new(&cfg);
        assert(nodes[i] != nullptr);
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

    for (uint32_t i = 0; i < 3; i++) {
        assert(csilk_raft_node_start(nodes[i]) == 0);
    }

    int leader_idx = -1;
    for (int attempt = 0; attempt < 200; attempt++) {
        int leaders = 0;
        int found = -1;
        for (int i = 0; i < 3; i++) {
            if (csilk_raft_get_role(nodes[i]) == CSILK_RAFT_ROLE_LEADER) {
                leaders++;
                found = i;
            }
        }
        if (leaders == 1) {
            leader_idx = found;
            break;
        }
        usleep(15000);
    }
    assert(leader_idx >= 0);

    for (int k = 1; k <= 5; k++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "msg-%d", k);
        uint64_t idx = 0;
        assert(csilk_raft_replicate_wait(
                   nodes[leader_idx], (const uint8_t*)payload, strlen(payload), 2000, &idx) == 0);
        assert(idx == (uint64_t)k);
    }

    /* Wait briefly for followers to apply committed entries via heartbeat */
    for (int attempt = 0; attempt < 100; attempt++) {
        int synced = 0;
        for (int i = 0; i < 3; i++) {
            if (csilk_raft_get_commit_index(nodes[i]) == 5) {
                synced++;
            }
        }
        if (synced == 3) {
            break;
        }
        usleep(15000);
    }

    for (int i = 0; i < 3; i++) {
        assert(csilk_raft_get_commit_index(nodes[i]) == 5);
        uint8_t* data = NULL;
        size_t   len = 0;
        assert(csilk_raft_get_log_entry(nodes[i], 5, NULL, &data, &len) == 0);
        assert(len == 5 && memcmp(data, "msg-5", 5) == 0);
        free(data);
    }

    for (int i = 0; i < 3; i++) {
        csilk_raft_node_free(nodes[i]);
    }
    printf("test_raft_three_node_election_and_replication passed\n");
}

/* Node stop is idempotent; tick drives election/heartbeat timers; apply-cb
 * + committed replay exercise the snapshot-apply and per-entry callbacks.
 * All pure state-machine logic, no live transport required. */
static int g_apply_calls = 0;

static void
on_raft_entry(uint64_t index, uint64_t term, const uint8_t* data, size_t len, void* user_data)
{
    (void)index;
    (void)term;
    (void)data;
    (void)len;
    *(int*)user_data = *(int*)user_data + 1;
}

static void
test_raft_node_lifecycle_and_callbacks(void)
{
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != nullptr);
    assert(csilk_raft_node_start(node) == 0);

    /* set_apply_cb with a per-entry callback + user_data counter. */
    int counter = 0;
    assert(csilk_raft_set_apply_cb(node, on_raft_entry, NULL, &counter) == 0);

    /* replicate_wait on a single-node cluster commits immediately. */
    uint64_t out_index = 0;
    assert(csilk_raft_replicate_wait(node, (const uint8_t*)"commit-me", 9, 1000, &out_index) == 0);
    assert(out_index >= 1);

    /* Explicit committed replay re-applies any not-yet-applied entries. */
    assert(csilk_raft_replay_committed(node) == 0);

    /* node_tick advances the leader broadcast timer (and interval sync). */
    assert(csilk_raft_node_tick(node, 0) == 0);

    /* node_stop is safe to call on an already-stopped node. */
    assert(csilk_raft_node_stop(node) == 0);
    assert(csilk_raft_node_stop(node) == 0);

    csilk_raft_node_free(node);

    /* NULL-node guards across the lifecycle entry points. */
    assert(csilk_raft_node_stop(nullptr) == -1);
    assert(csilk_raft_node_tick(nullptr, 0) == -1);
    assert(csilk_raft_replay_committed(nullptr) == -1);
    assert(csilk_raft_set_apply_cb(nullptr, on_raft_entry, NULL, nullptr) == -1);
    uint8_t* d = NULL;
    size_t   l = 0;
    assert(csilk_raft_get_log_entry(nullptr, 1, NULL, &d, &l) != 0);
    printf("test_raft_node_lifecycle_and_callbacks passed\n");
}

int
main(void)
{
    test_raft_consensus_single_node();
    test_raft_randomized_election_timeout();
    test_raft_three_node_election_and_replication();
    test_raft_node_lifecycle_and_callbacks();
    printf("All test_raft_consensus tests passed successfully!\n");
    return 0;
}
