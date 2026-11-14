#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "core/io/af_xdp_internal.h"

static void
test_umem_pool_allocation(void)
{
    size_t   sz = 1024 * 1024; /* 1MB */
    uint32_t frame_sz = 2048;

    csilk_xdp_umem_pool_t* pool = csilk_xdp_umem_pool_create(sz, frame_sz);
#if !CSILK_ENABLE_EXPERIMENTAL_STUBS
    assert(pool == nullptr);
    assert(errno == ENOTSUP);
    printf("test_umem_pool_allocation (fail-closed) passed\n");
    return;
#else
    assert(pool != nullptr);
    assert(pool->size == sz);
    assert(pool->frame_size == frame_sz);
    assert(pool->frame_count == (sz / frame_sz));
    assert(pool->buffer != nullptr);
    assert(((uintptr_t)pool->buffer % 4096) == 0);

    csilk_xdp_umem_pool_free(pool);
    printf("test_umem_pool_allocation passed\n");
#endif
}

int
main(void)
{
    test_umem_pool_allocation();
    printf("All test_af_xdp_zerocopy tests passed!\n");
    return 0;
}
