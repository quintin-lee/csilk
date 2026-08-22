/**
 * @file test_raft_snapshot.c
 * @brief Unit tests for Raft snapshot create and restore operations.
 * @copyright MIT License
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

    printf("All test_raft_snapshot tests passed successfully!\n");
    return 0;
}
