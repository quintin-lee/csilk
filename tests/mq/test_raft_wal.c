#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/messaging/raft.h"
#include "csilk/messaging/raft_wal.h"

static void
rm_rf_dir(const char* dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", dir);
    (void)system(cmd);
}

static void
test_raft_wal_in_memory(void)
{
    csilk_raft_config_t cfg = {.node_id = 1, .cluster_size = 3};

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_FOLLOWER);
    assert(csilk_raft_get_term(node) == 1);
    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_LEADER);
    assert(csilk_raft_get_term(node) == 2);

    const char* data = "LOG_ENTRY_001";
    uint64_t    idx = csilk_raft_append_log(node, (const uint8_t*)data, strlen(data));
    assert(idx == 1);

    uint64_t committed = csilk_raft_quorum_ack(node, 2);
    assert(committed == 1);

    csilk_raft_node_free(node);
}

static void
test_raft_wal_persistence_and_recovery(void)
{
    char  dir_tmpl[] = "/tmp/csilk_raft_wal_XXXXXX";
    char* wal_dir = mkdtemp(dir_tmpl);
    assert(wal_dir != NULL);

    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    cfg.wal_dir = wal_dir;
    cfg.sync_policy = CSILK_RAFT_SYNC_ALWAYS;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);
    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_get_term(node) == 2);

    const char* e1 = "entry-alpha";
    const char* e2 = "entry-beta-1234";
    assert(csilk_raft_append_log(node, (const uint8_t*)e1, strlen(e1)) == 1);
    assert(csilk_raft_append_log(node, (const uint8_t*)e2, strlen(e2)) == 2);
    assert(csilk_raft_wal_sync_count(node) >= 2);
    csilk_raft_node_free(node);

    /* Corrupt tail with partial garbage bytes; recovery must truncate tail cleanly */
    char wal_file[512];
    snprintf(wal_file, sizeof(wal_file), "%s/raft.wal", wal_dir);
    int fd = open(wal_file, O_WRONLY | O_APPEND);
    assert(fd >= 0);
    const uint8_t garbage[7] = {0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03};
    assert(write(fd, garbage, sizeof(garbage)) == (ssize_t)sizeof(garbage));
    close(fd);

    /* Reopen and verify term & entries survived */
    csilk_raft_node_t* recovered = csilk_raft_node_new(&cfg);
    assert(recovered != NULL);
    assert(csilk_raft_get_term(recovered) == 2);
    assert(csilk_raft_get_last_log_index(recovered) == 2);
    assert(csilk_raft_get_last_log_term(recovered) == 2);

    uint64_t term = 0;
    uint8_t* buf = NULL;
    size_t   len = 0;
    assert(csilk_raft_get_log_entry(recovered, 1, &term, &buf, &len) == 0);
    assert(term == 2 && len == strlen(e1) && memcmp(buf, e1, len) == 0);
    free(buf);

    assert(csilk_raft_get_log_entry(recovered, 2, &term, &buf, &len) == 0);
    assert(term == 2 && len == strlen(e2) && memcmp(buf, e2, len) == 0);
    free(buf);

    csilk_raft_node_free(recovered);
    rm_rf_dir(wal_dir);
}

static void
test_raft_wal_sync_policies(void)
{
    char  dir_tmpl[] = "/tmp/csilk_raft_batch_XXXXXX";
    char* wal_dir = mkdtemp(dir_tmpl);
    assert(wal_dir != NULL);

    /* BATCH policy */
    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    cfg.wal_dir = wal_dir;
    cfg.sync_policy = CSILK_RAFT_SYNC_BATCH;
    cfg.sync_batch_size = 4;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);
    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_wal_sync_count(node) == 0);

    for (int i = 0; i < 3; i++) {
        assert(csilk_raft_append_log(node, (const uint8_t*)"abc", 3) > 0);
    }
    assert(csilk_raft_wal_sync_count(node) == 0);

    assert(csilk_raft_append_log(node, (const uint8_t*)"abc", 3) == 4);
    assert(csilk_raft_wal_sync_count(node) == 1);

    assert(csilk_raft_append_log(node, (const uint8_t*)"xyz", 3) == 5);
    assert(csilk_raft_wal_sync_count(node) == 1);
    assert(csilk_raft_wal_flush(node) == 0);
    assert(csilk_raft_wal_sync_count(node) == 2);
    csilk_raft_node_free(node);

    /* INTERVAL_MS policy */
    cfg.sync_policy = CSILK_RAFT_SYNC_INTERVAL_MS;
    cfg.sync_interval_ms = 5000;
    node = csilk_raft_node_new(&cfg);
    assert(node != NULL);
    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_append_log(node, (const uint8_t*)"intv", 4) == 6);
    assert(csilk_raft_wal_sync_count(node) == 0);
    assert(csilk_raft_node_tick(node, UINT64_MAX / 2ULL) == 0);
    assert(csilk_raft_wal_sync_count(node) == 1);

    csilk_raft_node_free(node);
    rm_rf_dir(wal_dir);
}

int
main(void)
{
    printf("Testing Distributed Raft Consensus WAL Engine...\n");
    test_raft_wal_in_memory();
    test_raft_wal_persistence_and_recovery();
    test_raft_wal_sync_policies();
    printf("test_raft_wal: PASS\n");
    return 0;
}
