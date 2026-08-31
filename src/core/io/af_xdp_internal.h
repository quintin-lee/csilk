/**
 * @file af_xdp_internal.h
 * @brief Internal XSK Ring buffers and DMA descriptor structures for AF_XDP.
 */

#ifndef CSILK_AF_XDP_INTERNAL_H
#define CSILK_AF_XDP_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/uring/io_perf.h"

/**
 * @brief UMEM memory pool handle for AF_XDP Zero-Copy.
 */
typedef struct {
    void*    buffer;      /**< Pointer to hugepage buffer block */
    size_t   size;        /**< Total allocated bytes */
    uint32_t frame_size;  /**< Frame size (2048 or 4096 bytes) */
    uint32_t frame_count; /**< Total frames */
    void*    umem_handle; /**< Native xsk_umem handle pointer */
} csilk_xdp_umem_pool_t;

/**
 * @brief Zero-copy String View / Memory Slice.
 */
typedef struct {
    csilk_str_view_t name;
    csilk_str_view_t value;
} csilk_header_slice_t;

csilk_xdp_umem_pool_t* csilk_xdp_umem_pool_create(size_t total_size, uint32_t frame_size);
void                   csilk_xdp_umem_pool_free(csilk_xdp_umem_pool_t* pool);

#endif /* CSILK_AF_XDP_INTERNAL_H */
