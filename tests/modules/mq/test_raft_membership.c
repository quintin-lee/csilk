#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/messaging/raft.h"
#include "modules/mq/raft/raft_internal.h"

static void
test_membership_init_and_query(void)
{
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 3;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != nullptr);

    assert(csilk_raft_node_add_peer(node, "2", "127.0.0.1:8002") == 0);
    assert(csilk_raft_node_add_peer(node, "3", "127.0.0.1:8003") == 0);

    csilk_raft_membership_t memb;
    assert(csilk_raft_get_membership(node, &memb) == 0);
    assert(memb.state == CSILK_RAFT_CONF_STABLE);
    assert(memb.old_count == 3);
    assert(memb.new_count == 0);
    assert(memb.joint_index == 0);
    assert(memb.new_index == 0);

    /* Verify member node IDs */
    assert(strcmp(memb.old_nodes[0].id, "1") == 0);
    assert(strcmp(memb.old_nodes[1].id, "2") == 0);
    assert(strcmp(memb.old_nodes[2].id, "3") == 0);

    csilk_raft_node_free(node);
    printf("test_membership_init_and_query passed\n");
}

static void
test_membership_validation_and_rejections(void)
{
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != nullptr);

    /* Follower cannot initiate membership change */
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_FOLLOWER);
    assert(csilk_raft_add_node(node, "2", "127.0.0.1:9002") == -1);
    assert(csilk_raft_remove_node(node, "2") == -1);

    /* Start node as single leader */
    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_LEADER);

    /* Invalid arguments */
    assert(csilk_raft_add_node(node, NULL, "127.0.0.1:9002") == -1);
    assert(csilk_raft_add_node(node, "", "127.0.0.1:9002") == -1);
    assert(csilk_raft_add_node(node, "2", NULL) == -1);
    assert(csilk_raft_add_node(node, "1", "127.0.0.1:9001") == -1); /* Self */

    /* Cannot remove non-existent node */
    assert(csilk_raft_remove_node(node, "99") == -1);

    /* Cannot remove sole member */
    assert(csilk_raft_remove_node(node, "1") == -1);

    csilk_raft_node_free(node);
    printf("test_membership_validation_and_rejections passed\n");
}

static void
test_joint_consensus_dual_quorum_logic(void)
{
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 5;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != nullptr);

    /* Setup 4 peers: 2, 3, 4, 5 */
    assert(csilk_raft_node_add_peer(node, "2", "127.0.0.1:8002") == 0);
    assert(csilk_raft_node_add_peer(node, "3", "127.0.0.1:8003") == 0);
    assert(csilk_raft_node_add_peer(node, "4", "127.0.0.1:8004") == 0);
    assert(csilk_raft_node_add_peer(node, "5", "127.0.0.1:8005") == 0);

    csilk_mutex_lock(&node->mutex);
    /* Manually configure Joint state: C_old = {1, 2, 3}, C_new = {1, 2, 4, 5} */
    node->membership.state = CSILK_RAFT_CONF_JOINT;
    node->membership.old_count = 3;
    snprintf(node->membership.old_nodes[0].id, 64, "1");
    snprintf(node->membership.old_nodes[1].id, 64, "2");
    snprintf(node->membership.old_nodes[2].id, 64, "3");

    node->membership.new_count = 4;
    snprintf(node->membership.new_nodes[0].id, 64, "1");
    snprintf(node->membership.new_nodes[1].id, 64, "2");
    snprintf(node->membership.new_nodes[2].id, 64, "4");
    snprintf(node->membership.new_nodes[3].id, 64, "5");

    uint64_t test_idx = 10;

    /* Case 1: Only Node 1 (acks: 1 in old [need 2], 1 in new [need 3]) -> NO COMMIT */
    assert(_raft_membership_is_log_committed_locked(node, test_idx) == 0);

    /* Case 2: Node 1 and Node 3 ack -> old has 2/3 (majority ok), but new has only Node 1 (1/4) -> NO COMMIT */
    node->peers[1].match_index = 10; /* Peer "3" matches */
    assert(_raft_membership_is_log_committed_locked(node, test_idx) == 0);

    /* Case 3: Node 1, Node 4, Node 5 ack -> new has 3/4 (majority ok), but old has only Node 1 (1/3) -> NO COMMIT */
    node->peers[1].match_index = 0;  /* Reset peer "3" */
    node->peers[2].match_index = 10; /* Peer "4" matches */
    node->peers[3].match_index = 10; /* Peer "5" matches */
    assert(_raft_membership_is_log_committed_locked(node, test_idx) == 0);

    /* Case 4: Node 1, Node 2, Node 4 ack -> old has {1, 2} (2/3 OK!), new has {1, 2, 4} (3/4 OK!) -> COMMITTED! */
    node->peers[0].match_index = 10; /* Peer "2" matches */
    assert(_raft_membership_is_log_committed_locked(node, test_idx) == 1);

    csilk_mutex_unlock(&node->mutex);
    csilk_raft_node_free(node);
    printf("test_joint_consensus_dual_quorum_logic passed\n");
}

static void
test_dynamic_cluster_add_and_remove(void)
{
    /* Spin up a 3-node cluster, elect a leader, dynamically add Node 4, then remove Node 4 */
    csilk_raft_node_t* nodes[4] = {NULL, NULL, NULL, NULL};
    uint16_t           ports[4] = {0, 0, 0, 0};

    for (uint32_t i = 0; i < 4; i++) {
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

    /* Connect initial 3 nodes */
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

    /* Start initial 3 nodes */
    for (uint32_t i = 0; i < 3; i++) {
        assert(csilk_raft_node_start(nodes[i]) == 0);
    }

    /* Start Node 4 listening so it can receive RPCs */
    assert(csilk_raft_node_start(nodes[3]) == 0);

    /* Wait for leader election */
    int leader_idx = -1;
    for (int attempt = 0; attempt < 200; attempt++) {
        for (int i = 0; i < 3; i++) {
            if (csilk_raft_get_role(nodes[i]) == CSILK_RAFT_ROLE_LEADER) {
                leader_idx = i;
                break;
            }
        }
        if (leader_idx >= 0) {
            break;
        }
        usleep(15000);
    }
    assert(leader_idx >= 0);
    csilk_raft_node_t* leader = nodes[leader_idx];

    /* Replicate initial entry */
    uint64_t      init_idx = 0;
    const uint8_t msg1[] = "initial_cluster_state";
    assert(csilk_raft_replicate_wait(leader, msg1, sizeof(msg1), 2000, &init_idx) == 0);
    assert(init_idx > 0);

    /* Dynamically add Node 4 via Joint Consensus */
    char node4_addr[64];
    snprintf(node4_addr, sizeof(node4_addr), "127.0.0.1:%u", ports[3]);
    int add_rc = csilk_raft_change_membership_wait(leader, "4", node4_addr, 1, 3000);
    assert(add_rc == 0);

    /* Verify membership state is back to STABLE with 4 members */
    csilk_raft_membership_t memb4;
    assert(csilk_raft_get_membership(leader, &memb4) == 0);
    assert(memb4.state == CSILK_RAFT_CONF_STABLE);
    assert(memb4.old_count == 4);

    int found4 = 0;
    for (size_t i = 0; i < memb4.old_count; i++) {
        if (strcmp(memb4.old_nodes[i].id, "4") == 0) {
            found4 = 1;
            break;
        }
    }
    assert(found4 == 1);

    /* Replicate log entry in 4-node cluster */
    uint64_t      four_idx = 0;
    const uint8_t msg2[] = "four_node_state";
    assert(csilk_raft_replicate_wait(leader, msg2, sizeof(msg2), 2000, &four_idx) == 0);
    assert(four_idx > init_idx);

    /* Dynamically remove Node 4 via Joint Consensus */
    int rm_rc = csilk_raft_change_membership_wait(leader, "4", NULL, 0, 3000);
    assert(rm_rc == 0);

    /* Verify membership state is back to STABLE with 3 members */
    csilk_raft_membership_t memb3;
    assert(csilk_raft_get_membership(leader, &memb3) == 0);
    assert(memb3.state == CSILK_RAFT_CONF_STABLE);
    assert(memb3.old_count == 3);

    for (size_t i = 0; i < memb3.old_count; i++) {
        assert(strcmp(memb3.old_nodes[i].id, "4") != 0);
    }

    /* Replicate log entry in 3-node cluster */
    uint64_t      post_idx = 0;
    const uint8_t msg3[] = "three_node_pruned_state";
    assert(csilk_raft_replicate_wait(leader, msg3, sizeof(msg3), 2000, &post_idx) == 0);
    assert(post_idx > four_idx);

    for (uint32_t i = 0; i < 4; i++) {
        csilk_raft_node_free(nodes[i]);
    }
    printf("test_dynamic_cluster_add_and_remove passed\n");
}

typedef struct {
    int  joined_count;
    char last_joined_id[64];
    char last_joined_addr[128];
    int  left_count;
    char last_left_id[64];
    int  stable_count;
} test_topology_ctx_t;

static void
_test_topology_cb(csilk_raft_node_t*          node,
                  csilk_raft_topology_event_t event,
                  const char*                 peer_id,
                  const char*                 peer_addr,
                  void*                       user_data)
{
    (void)node;
    test_topology_ctx_t* ctx = (test_topology_ctx_t*)user_data;
    if (event == CSILK_RAFT_TOPOLOGY_NODE_JOINED) {
        ctx->joined_count++;
        if (peer_id) {
            snprintf(ctx->last_joined_id, sizeof(ctx->last_joined_id), "%s", peer_id);
        }
        if (peer_addr) {
            snprintf(ctx->last_joined_addr, sizeof(ctx->last_joined_addr), "%s", peer_addr);
        }
    } else if (event == CSILK_RAFT_TOPOLOGY_NODE_LEFT) {
        ctx->left_count++;
        if (peer_id) {
            snprintf(ctx->last_left_id, sizeof(ctx->last_left_id), "%s", peer_id);
        }
    } else if (event == CSILK_RAFT_TOPOLOGY_CONF_STABLE) {
        ctx->stable_count++;
    }
}

static void
test_topology_listener_callbacks(void)
{
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 3;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != nullptr);

    test_topology_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    /* Register topology listener */
    assert(csilk_raft_set_topology_cb(node, _test_topology_cb, &ctx) == 0);

    /* Adding peers triggers NODE_JOINED */
    assert(csilk_raft_node_add_peer(node, "2", "127.0.0.1:8002") == 0);
    assert(ctx.joined_count == 1);
    assert(strcmp(ctx.last_joined_id, "2") == 0);
    assert(strcmp(ctx.last_joined_addr, "127.0.0.1:8002") == 0);

    assert(csilk_raft_node_add_peer(node, "3", "127.0.0.1:8003") == 0);
    assert(ctx.joined_count == 2);
    assert(strcmp(ctx.last_joined_id, "3") == 0);

    /* Unregister listener */
    assert(csilk_raft_set_topology_cb(node, nullptr, nullptr) == 0);
    assert(csilk_raft_node_add_peer(node, "4", "127.0.0.1:8004") == 0);
    assert(ctx.joined_count == 2);

    csilk_raft_node_free(node);
    printf("test_topology_listener_callbacks passed\n");
}

int
main(void)
{
    printf("Starting Raft Joint Consensus membership tests...\n");
    test_membership_init_and_query();
    test_membership_validation_and_rejections();
    test_joint_consensus_dual_quorum_logic();
    test_dynamic_cluster_add_and_remove();
    test_topology_listener_callbacks();
    printf("All Raft Joint Consensus membership tests passed successfully!\n");
    return 0;
}
