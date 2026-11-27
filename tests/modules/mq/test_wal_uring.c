#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "csilk/messaging/raft.h"
#include "csilk/messaging/wal_uring.h"
#include "modules/mq/raft/raft_internal.h"

static void
test_wal_uring_detection_and_creation(void)
{
    int supported = csilk_wal_is_uring_supported();
    printf("csilk_wal_is_uring_supported: %d\n", supported);

    csilk_wal_ring_t* ring = csilk_wal_ring_create(64);
    assert(ring != NULL);

    int real = csilk_wal_ring_is_real_uring(ring);
    printf("csilk_wal_ring_is_real_uring: %d\n", real);
    if (supported) {
        assert(real == 1);
    } else {
        assert(real == 0);
    }

    assert(csilk_wal_ring_get_completed(ring) == 0);
    csilk_wal_ring_destroy(ring);
    printf("test_wal_uring_detection_and_creation passed\n");
}

static void
test_wal_uring_batch_writev_and_fsync(void)
{
    char tmppath[] = "/tmp/csilk_wal_batch_test_XXXXXX";
    int  fd = mkstemp(tmppath);
    assert(fd >= 0);

    csilk_wal_ring_t* ring = csilk_wal_ring_create(64);
    assert(ring != NULL);

    /* Submit 3 vectorized write requests */
    char         chunk1_hdr[] = "HDR1:";
    char         chunk1_body[] = "data_payload_1\n";
    struct iovec iov1[2];
    iov1[0].iov_base = chunk1_hdr;
    iov1[0].iov_len = strlen(chunk1_hdr);
    iov1[1].iov_base = chunk1_body;
    iov1[1].iov_len = strlen(chunk1_body);
    size_t len1 = iov1[0].iov_len + iov1[1].iov_len;
    assert(csilk_wal_ring_submit_writev(ring, fd, iov1, 2, 0) == 0);

    char         chunk2_hdr[] = "HDR2:";
    char         chunk2_body[] = "second_payload_2\n";
    struct iovec iov2[2];
    iov2[0].iov_base = chunk2_hdr;
    iov2[0].iov_len = strlen(chunk2_hdr);
    iov2[1].iov_base = chunk2_body;
    iov2[1].iov_len = strlen(chunk2_body);
    assert(csilk_wal_ring_submit_writev(ring, fd, iov2, 2, (off_t)len1) == 0);

    /* Submit fsync */
    assert(csilk_wal_ring_submit_fsync(ring, fd, 1) == 0);

    /* Flush submissions */
    assert(csilk_wal_ring_flush(ring, 1) >= 0);

    /* Verify file contents */
    lseek(fd, 0, SEEK_SET);
    char    buf[256];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    assert(n > 0);
    buf[n] = '\0';

    assert(strstr(buf, "HDR1:data_payload_1\n") != NULL);
    assert(strstr(buf, "HDR2:second_payload_2\n") != NULL);

    csilk_wal_ring_destroy(ring);
    close(fd);
    unlink(tmppath);
    printf("test_wal_uring_batch_writev_and_fsync passed\n");
}

static void
test_wal_uring_linked_writev_fsync(void)
{
    char tmppath[] = "/tmp/csilk_wal_linked_test_XXXXXX";
    int  fd = mkstemp(tmppath);
    assert(fd >= 0);

    csilk_wal_ring_t* ring = csilk_wal_ring_create(64);
    assert(ring != NULL);

    char         hdr[] = "LINKED_HDR:";
    char         data[] = "atomic_durable_transaction\n";
    struct iovec iov[2];
    iov[0].iov_base = hdr;
    iov[0].iov_len = strlen(hdr);
    iov[1].iov_base = data;
    iov[1].iov_len = strlen(data);

    assert(csilk_wal_ring_submit_writev_fsync(ring, fd, iov, 2, -1, 1) == 0);
    assert(csilk_wal_ring_flush(ring, 1) >= 0);

    lseek(fd, 0, SEEK_SET);
    char    buf[128];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    assert(n > 0);
    buf[n] = '\0';
    assert(strcmp(buf, "LINKED_HDR:atomic_durable_transaction\n") == 0);

    csilk_wal_ring_destroy(ring);
    close(fd);
    unlink(tmppath);
    printf("test_wal_uring_linked_writev_fsync passed\n");
}

static void
test_raft_wal_batch_append_and_recovery(void)
{
    char tmppath[] = "/tmp/csilk_raft_batch_dir_XXXXXX";
    assert(mkdtemp(tmppath) != NULL);

    csilk_raft_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.node_id = 1;
    cfg.cluster_size = 1;
    cfg.wal_dir = tmppath;

    csilk_raft_node_t* node = csilk_raft_node_new(&cfg);
    assert(node != NULL);

    /* Verify io_uring active query */
    int active = csilk_raft_wal_is_uring_active(node);
    printf("csilk_raft_wal_is_uring_active: %d\n", active);

    assert(csilk_raft_node_start(node) == 0);
    assert(csilk_raft_get_role(node) == CSILK_RAFT_ROLE_LEADER);

    /* Prepare a batch of 25 log entries */
    const size_t BATCH_SIZE = 25;
    uint8_t*     data_ptrs[25];
    size_t       lengths[25];
    char         entry_bufs[25][64];

    for (size_t i = 0; i < BATCH_SIZE; i++) {
        snprintf(entry_bufs[i], sizeof(entry_bufs[i]), "batch_record_entry_#%zu", i + 1);
        data_ptrs[i] = (uint8_t*)entry_bufs[i];
        lengths[i] = strlen(entry_bufs[i]);
    }

    uint64_t first_idx = 0;
    size_t   appended = csilk_raft_append_log_batch(
        node, (const uint8_t* const*)data_ptrs, lengths, BATCH_SIZE, &first_idx);
    assert(appended == BATCH_SIZE);
    assert(first_idx == 1);
    assert(csilk_raft_get_last_log_index(node) == BATCH_SIZE);

    assert(csilk_raft_wal_flush(node) == 0);
    csilk_raft_node_free(node);

    /* Reopen node and verify recovery of all 25 batch entries */
    csilk_raft_node_t* recovered = csilk_raft_node_new(&cfg);
    assert(recovered != NULL);
    assert(csilk_raft_get_last_log_index(recovered) == BATCH_SIZE);

    for (size_t i = 0; i < BATCH_SIZE; i++) {
        uint64_t term = 0;
        uint8_t* out_data = NULL;
        size_t   out_len = 0;
        assert(csilk_raft_get_log_entry(recovered, i + 1, &term, &out_data, &out_len) == 0);
        assert(out_data != NULL);
        assert(out_len == lengths[i]);
        assert(memcmp(out_data, entry_bufs[i], out_len) == 0);
        free(out_data);
    }

    csilk_raft_node_free(recovered);

    /* Cleanup directory */
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", tmppath);
    (void)system(cmd);

    printf("test_raft_wal_batch_append_and_recovery passed\n");
}

int
main(void)
{
    printf("Starting io_uring WAL batch writing and fsync tests...\n");
    test_wal_uring_detection_and_creation();
    test_wal_uring_batch_writev_and_fsync();
    test_wal_uring_linked_writev_fsync();
    test_raft_wal_batch_append_and_recovery();
    printf("All io_uring WAL batch writing and fsync tests passed successfully!\n");
    return 0;
}
