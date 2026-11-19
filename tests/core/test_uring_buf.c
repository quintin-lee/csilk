#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/uring/uring_buf.h"

static void
test_uring_buf_ring_basic()
{
    printf("Testing io_uring Registered Buffer Ring creation & 4096-byte alignment...\n");

    size_t num_bufs = 8;
    size_t buf_size = 8192;

    csilk_uring_buf_ring_t* ring = csilk_uring_buf_ring_create(num_bufs, buf_size);
    assert(ring != NULL);
    assert(csilk_uring_buf_ring_get_num_bufs(ring) == num_bufs);
    assert(csilk_uring_buf_ring_get_buf_size(ring) == buf_size);

    for (size_t i = 0; i < num_bufs; i++) {
        void* ptr = csilk_uring_buf_ring_get(ring, i);
        assert(ptr != NULL);

        // Verify 4096-byte page boundary alignment
        uintptr_t addr = (uintptr_t)ptr;
        assert((addr % 4096) == 0);

        // Verify write and read operations
        memset(ptr, 0xAB, 128);
        uint8_t* byte_ptr = (uint8_t*)ptr;
        assert(byte_ptr[0] == 0xAB);
        assert(byte_ptr[127] == 0xAB);
    }

    assert(csilk_uring_buf_ring_get(ring, num_bufs) == NULL); // Out of bounds check

    csilk_uring_buf_ring_free(ring);
    printf("test_uring_buf_ring_basic: PASS\n");
}

static void
test_uring_buf_ring_registration()
{
    printf("Testing io_uring Registered Buffer Ring registration API...\n");

    csilk_uring_buf_ring_t* ring = csilk_uring_buf_ring_create(4, 4096);
    assert(ring != NULL);
    assert(!csilk_uring_buf_ring_is_registered(ring));

    /* NULL checks */
    assert(csilk_uring_buf_ring_register(NULL, (void*)0x1) == -EINVAL);
    assert(csilk_uring_buf_ring_register(ring, NULL) == -EINVAL);
    assert(csilk_uring_buf_ring_unregister(NULL, (void*)0x1) == -EINVAL);
    assert(csilk_uring_buf_ring_unregister(ring, NULL) == -EINVAL);

    /* When not registered, unregister returns 0 (noop) */
    assert(csilk_uring_buf_ring_unregister(ring, (void*)0x1) == 0);

#ifndef CSILK_USE_URING
    /* Under non-uring builds, registration returns -ENOSYS */
    assert(csilk_uring_buf_ring_register(ring, (void*)0x1) == -ENOSYS);
#endif

    csilk_uring_buf_ring_free(ring);
    printf("test_uring_buf_ring_registration: PASS\n");
}

static void
test_uring_direct_files()
{
    printf("Testing io_uring Direct Files (Registered FDs) table...\n");

    /* Invalid creation */
    assert(csilk_uring_direct_files_create(0) == NULL);

    csilk_uring_direct_files_t* df = csilk_uring_direct_files_create(8);
    assert(df != NULL);
    assert(csilk_uring_direct_files_get_capacity(df) == 8);
    assert(csilk_uring_direct_files_get_count(df) == 0);
    assert(!csilk_uring_direct_files_is_registered(df));

    /* Slot allocation */
    int slot0 = csilk_uring_direct_files_alloc_slot(df, 100, NULL);
    int slot1 = csilk_uring_direct_files_alloc_slot(df, 101, NULL);
    int slot2 = csilk_uring_direct_files_alloc_slot(df, 102, NULL);

    assert(slot0 == 0);
    assert(slot1 == 1);
    assert(slot2 == 2);
    assert(csilk_uring_direct_files_get_count(df) == 3);

    assert(csilk_uring_direct_files_get_fd(df, 0) == 100);
    assert(csilk_uring_direct_files_get_fd(df, 1) == 101);
    assert(csilk_uring_direct_files_get_fd(df, 2) == 102);
    assert(csilk_uring_direct_files_get_fd(df, 3) == -1);
    assert(csilk_uring_direct_files_get_fd(df, 99) == -1);

    /* Free slot 1 and verify slot reuse */
    assert(csilk_uring_direct_files_free_slot(df, 1, NULL) == 0);
    assert(csilk_uring_direct_files_get_count(df) == 2);
    assert(csilk_uring_direct_files_get_fd(df, 1) == -1);

    int reused = csilk_uring_direct_files_alloc_slot(df, 200, NULL);
    assert(reused == 1);
    assert(csilk_uring_direct_files_get_fd(df, 1) == 200);
    assert(csilk_uring_direct_files_get_count(df) == 3);

    /* Error handling */
    assert(csilk_uring_direct_files_alloc_slot(NULL, 10, NULL) == -1);
    assert(csilk_uring_direct_files_alloc_slot(df, -1, NULL) == -1);
    assert(csilk_uring_direct_files_free_slot(NULL, 0, NULL) == -1);
    assert(csilk_uring_direct_files_free_slot(df, -1, NULL) == -1);
    assert(csilk_uring_direct_files_free_slot(df, 100, NULL) == -1);

    /* NULL / bounds checks for registration */
    assert(csilk_uring_direct_files_register(NULL, (void*)0x1) == -EINVAL);
    assert(csilk_uring_direct_files_register(df, NULL) == -EINVAL);
    assert(csilk_uring_direct_files_unregister(NULL, (void*)0x1) == -EINVAL);
    assert(csilk_uring_direct_files_unregister(df, NULL) == -EINVAL);

    csilk_uring_direct_files_free(df);
    printf("test_uring_direct_files: PASS\n");
}

int
main()
{
    test_uring_buf_ring_basic();
    test_uring_buf_ring_registration();
    test_uring_direct_files();
    printf("All io_uring Registered Buffer & Direct Files tests passed successfully!\n");
    return 0;
}
