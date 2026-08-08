#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "csilk/messaging/raft_wal.h"

int
main(void)
{
    printf("Testing Distributed Raft Consensus WAL Engine...\n");

    csilk_raft_config_t cfg = {.node_id = 1, .cluster_size = 3};

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_FOLLOWER);
    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_LEADER);
    assert(csilk_raft_get_term(node) == 1);

    /* Test 1: Append log entry */
    const char* data = "LOG_ENTRY_001";
    uint64_t    idx = csilk_raft_append_log(node, (const uint8_t*)data, strlen(data));
    assert(idx == 1);

    /* Test 2: Quorum ack */
    uint64_t committed = csilk_raft_quorum_ack(node, 2);
    assert(committed == 1);

    csilk_raft_node_free(node);
    printf("test_raft_wal: PASS\n");
    return 0;
}
