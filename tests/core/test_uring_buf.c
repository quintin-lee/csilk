#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/core/uring_buf.h"

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

int
main()
{
    test_uring_buf_ring_basic();
    printf("All io_uring Registered Buffer tests passed successfully!\n");
    return 0;
}
