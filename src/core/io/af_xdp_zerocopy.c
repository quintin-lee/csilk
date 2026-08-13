#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "af_xdp_internal.h"

csilk_xdp_umem_pool_t*
csilk_xdp_umem_pool_create(size_t total_size, uint32_t frame_size)
{
    if (total_size == 0 || frame_size == 0) {
        return NULL;
    }

    csilk_xdp_umem_pool_t* pool = (csilk_xdp_umem_pool_t*)calloc(1, sizeof(csilk_xdp_umem_pool_t));
    if (!pool) {
        return NULL;
    }

    pool->size = total_size;
    pool->frame_size = frame_size;
    pool->frame_count = (uint32_t)(total_size / frame_size);

    /* Allocate aligned UMEM page block */
    int res = posix_memalign(&pool->buffer, 4096, total_size);
    if (res != 0 || !pool->buffer) {
        free(pool);
        return NULL;
    }
    memset(pool->buffer, 0, total_size);

    return pool;
}

void
csilk_xdp_umem_pool_free(csilk_xdp_umem_pool_t* pool)
{
    if (!pool) {
        return;
    }

    if (pool->buffer) {
        free(pool->buffer);
    }
    free(pool);
}
