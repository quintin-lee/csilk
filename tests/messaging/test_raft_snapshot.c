/**
 * @file test_raft_snapshot.c
 * @brief Unit tests for Raft snapshot create and restore operations.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/messaging/raft.h"
#include "messaging/raft_internal.h"

static void
test_snapshot_create_null_node(void)
{
    printf("Testing csilk_raft_snapshot_create with NULL node...\n");
    int rc = csilk_raft_snapshot_create(NULL, (const uint8_t*)"data", 4);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_snapshot_create_null_data(void)
{
    printf("Testing csilk_raft_snapshot_create with NULL data...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    int rc = csilk_raft_snapshot_create(node, NULL, 4);
    assert(rc == -1);

    csilk_raft_node_free(node);
    printf("  passed\n");
}

static void
test_snapshot_create_empty_data(void)
{
    printf("Testing csilk_raft_snapshot_create with empty data...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    int rc = csilk_raft_snapshot_create(node, (const uint8_t*)"", 0);
    assert(rc == -1);

    csilk_raft_node_free(node);
    printf("  passed\n");
}

static void
test_snapshot_create_success(void)
{
    printf("Testing csilk_raft_snapshot_create success path...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    uint8_t state[] = {0xDE, 0xAD, 0xBE, 0xEF};
    int     rc = csilk_raft_snapshot_create(node, state, sizeof(state));
    assert(rc == 0);

    csilk_raft_node_free(node);
    printf("  passed\n");
}

static void
test_snapshot_restore_null_node(void)
{
    printf("Testing csilk_raft_snapshot_restore with NULL node...\n");
    int rc = csilk_raft_snapshot_restore(NULL, (const uint8_t*)"data", 4);
    assert(rc == -1);
    printf("  passed\n");
}

static void
test_snapshot_restore_null_data(void)
{
    printf("Testing csilk_raft_snapshot_restore with NULL data...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    int rc = csilk_raft_snapshot_restore(node, NULL, 4);
    assert(rc == -1);

    csilk_raft_node_free(node);
    printf("  passed\n");
}

static void
test_snapshot_restore_empty_data(void)
{
    printf("Testing csilk_raft_snapshot_restore with empty data...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    int rc = csilk_raft_snapshot_restore(node, (const uint8_t*)"", 0);
    assert(rc == -1);

    csilk_raft_node_free(node);
    printf("  passed\n");
}

static void
test_snapshot_restore_success(void)
{
    printf("Testing csilk_raft_snapshot_restore success path...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    uint8_t state[] = {0xCA, 0xFE, 0xBA, 0xBE};
    int     rc = csilk_raft_snapshot_restore(node, state, sizeof(state));
    assert(rc == 0);

    csilk_raft_node_free(node);
    printf("  passed\n");
}

static void
test_snapshot_roundtrip(void)
{
    printf("Testing snapshot create -> restore roundtrip...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    /* Create snapshot */
    uint8_t state[] = {0x01, 0x02, 0x03, 0x04};
    int     rc = csilk_raft_snapshot_create(node, state, sizeof(state));
    assert(rc == 0);

    /* Restore from snapshot */
    rc = csilk_raft_snapshot_restore(node, state, sizeof(state));
    assert(rc == 0);

    csilk_raft_node_free(node);
    printf("  passed\n");
}

static void
rm_rf_snap_dir(const char* dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
    (void)system(cmd);
}

typedef struct {
    int      called;
    uint64_t last_idx;
    uint64_t last_term;
    size_t   len;
} snap_cb_state_t;

static void
on_snap_applied(
    uint64_t last_index, uint64_t last_term, const uint8_t* data, size_t len, void* user_data)
{
    (void)data;
    snap_cb_state_t* st = (snap_cb_state_t*)user_data;
    st->called++;
    st->last_idx = last_index;
    st->last_term = last_term;
    st->len = len;
}

static void
test_snapshot_disk_compaction_and_recovery(void)
{
    printf("Testing snapshot disk persistence, WAL compaction, and recovery...\n");
    char  dir_tmpl[] = "/tmp/csilk_raft_snap_XXXXXX";
    char* wal_dir = mkdtemp(dir_tmpl);
    assert(wal_dir != NULL);

    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    cfg.wal_dir = wal_dir;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);
    assert(csilk_raft_node_start(node) == 0);

    for (int i = 1; i <= 5; i++) {
        char payload[32];
        snprintf(payload, sizeof(payload), "cmd-%d", i);
        uint64_t idx = csilk_raft_append_log(node, (const uint8_t*)payload, strlen(payload));
        assert(idx == (uint64_t)i);
    }
    assert(csilk_raft_quorum_ack(node, 1) == 5);

    const char snap_state[] = "SNAP_STATE_AT_5";
    assert(csilk_raft_snapshot_create(node, (const uint8_t*)snap_state, sizeof(snap_state)) == 0);

    /* Entries <= 5 must be compacted away */
    for (uint64_t i = 1; i <= 5; i++) {
        assert(csilk_raft_get_log_entry(node, i, NULL, NULL, NULL) == -1);
    }

    /* Append trailing entries after snapshot */
    assert(csilk_raft_append_log(node, (const uint8_t*)"cmd-6", 5) == 6);
    assert(csilk_raft_append_log(node, (const uint8_t*)"cmd-7", 5) == 7);
    csilk_raft_node_free(node);

    /* Reopen and verify snapshot + tail WAL recovery */
    csilk_raft_node_t* recovered = csilk_raft_node_new(&cfg);
    assert(recovered != NULL);

    uint64_t snap_idx = 0;
    uint64_t snap_term = 0;
    uint8_t* snap_data = NULL;
    size_t   snap_len = 0;
    assert(csilk_raft_snapshot_load(recovered, &snap_idx, &snap_term, &snap_data, &snap_len) == 0);
    assert(snap_idx == 5 && snap_term == 2 && snap_len == sizeof(snap_state));
    assert(memcmp(snap_data, snap_state, sizeof(snap_state)) == 0);
    free(snap_data);

    assert(csilk_raft_get_last_log_index(recovered) == 7);
    assert(csilk_raft_get_log_entry(recovered, 5, NULL, NULL, NULL) == -1);
    uint8_t* ent6 = NULL;
    size_t   ent6_len = 0;
    assert(csilk_raft_get_log_entry(recovered, 6, NULL, &ent6, &ent6_len) == 0);
    assert(ent6_len == 5 && memcmp(ent6, "cmd-6", 5) == 0);
    free(ent6);

    csilk_raft_node_free(recovered);
    rm_rf_snap_dir(wal_dir);
    printf("  passed\n");
}

static void
test_snapshot_install_chunked_rpc(void)
{
    printf("Testing chunked InstallSnapshot RPC reassembly...\n");
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 2;
    cfg.cluster_size = 3;

    csilk_raft_node_t* follower = csilk_raft_node_new(&cfg);
    assert(follower != NULL);

    snap_cb_state_t cb_st;
    memset(&cb_st, 0, sizeof(cb_st));
    assert(csilk_raft_set_apply_cb(follower, NULL, on_snap_applied, &cb_st) == 0);

    size_t   big_len = 20000;
    uint8_t* big_snap = malloc(big_len);
    assert(big_snap != NULL);
    for (size_t i = 0; i < big_len; i++) {
        big_snap[i] = (uint8_t)((i * 31U) & 0xFFU);
    }

    uint8_t* pkt = malloc(CSILK_RAFT_RPC_MAX_PACKET);
    uint8_t* reply = malloc(4096);
    assert(pkt && reply);

    size_t offset = 0;
    while (offset < big_len) {
        size_t chunk = big_len - offset;
        if (chunk > CSILK_RAFT_SNAPSHOT_CHUNK_SIZE) {
            chunk = CSILK_RAFT_SNAPSHOT_CHUNK_SIZE;
        }

        csilk_raft_is_req_t req;
        memset(&req, 0, sizeof(req));
        req.term = 4;
        req.leader_id = 1;
        req.last_included_index = 42;
        req.last_included_term = 3;
        req.total_len = big_len;
        req.offset = offset;
        req.chunk_len = (uint32_t)chunk;
        req.done = (offset + chunk >= big_len) ? 1U : 0U;

        int plen = _raft_rpc_encode_is_req(&req,
                                           big_snap + offset,
                                           pkt + sizeof(csilk_raft_hdr_t),
                                           CSILK_RAFT_RPC_MAX_PACKET - sizeof(csilk_raft_hdr_t));
        assert(plen > 0);
        assert(csilk_raft_rpc_encode_hdr(CSILK_RAFT_MSG_INSTALL_SNAPSHOT_REQ,
                                         4,
                                         (uint32_t)plen,
                                         pkt,
                                         CSILK_RAFT_RPC_MAX_PACKET) > 0);

        size_t rlen = 0;
        assert(
            _raft_handle_rpc_packet(
                follower, pkt, sizeof(csilk_raft_hdr_t) + (size_t)plen, NULL, reply, 4096, &rlen) ==
            0);
        assert(rlen > sizeof(csilk_raft_hdr_t));
        csilk_raft_is_resp_t resp;
        assert(_raft_rpc_decode_is_resp(
                   reply + sizeof(csilk_raft_hdr_t), rlen - sizeof(csilk_raft_hdr_t), &resp) == 0);
        assert(resp.success == 1);
        offset += chunk;
    }

    assert(cb_st.called == 1);
    assert(cb_st.last_idx == 42 && cb_st.last_term == 3 && cb_st.len == big_len);
    assert(csilk_raft_get_commit_index(follower) == 42);

    uint8_t* loaded = NULL;
    size_t   loaded_len = 0;
    assert(csilk_raft_snapshot_load(follower, NULL, NULL, &loaded, &loaded_len) == 0);
    assert(loaded_len == big_len && memcmp(loaded, big_snap, big_len) == 0);
    free(loaded);

    free(reply);
    free(pkt);
    free(big_snap);
    csilk_raft_node_free(follower);
    printf("  passed\n");
}

int
main(void)
{
    test_snapshot_create_null_node();
    test_snapshot_create_null_data();
    test_snapshot_create_empty_data();
    test_snapshot_create_success();
    test_snapshot_restore_null_node();
    test_snapshot_restore_null_data();
    test_snapshot_restore_empty_data();
    test_snapshot_restore_success();
    test_snapshot_roundtrip();
    test_snapshot_disk_compaction_and_recovery();
    test_snapshot_install_chunked_rpc();

    printf("All test_raft_snapshot tests passed successfully!\n");
    return 0;
}
