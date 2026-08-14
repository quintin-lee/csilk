/**
 * @file vector_internal.h
 * @brief Internal header for SIMD vector distance kernels and HNSW graph structures.
 * @copyright MIT License
 */

#ifndef CSILK_VECTOR_INTERNAL_H
#define CSILK_VECTOR_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/sync.h"

typedef float (*csilk_simd_dist_fn)(const float* a, const float* b, size_t dim);

/** @brief Allocate an aligned buffer (see csilk_aligned_alloc()). */
void* csilk_aligned_alloc(size_t alignment, size_t size);
/** @brief Free an aligned buffer (see csilk_aligned_free()). */
void csilk_aligned_free(void* ptr);

/** @brief Cosine distance kernel (1 - cos similarity). */
float csilk_simd_cosine_distance(const float* a, const float* b, size_t dim);
/** @brief Squared L2 distance kernel. */
float csilk_simd_l2_distance(const float* a, const float* b, size_t dim);
/** @brief Negative dot-product kernel. */
float csilk_simd_dot_product(const float* a, const float* b, size_t dim);

typedef struct csilk_hnsw_node_s {
    uint32_t   id;
    char*      doc_id;
    float*     vector;
    int        level;
    uint32_t** neighbors;
    uint16_t*  neighbor_count;
} csilk_hnsw_node_t;

typedef struct {
    csilk_hnsw_node_t** nodes;
    size_t              node_count;
    size_t              node_capacity;
    uint32_t            enter_node_id;
    int                 max_level;
    size_t              dim;
    int                 metric;
    size_t              M;
    size_t              ef_construction;
    size_t              ef_search;
    csilk_simd_dist_fn  dist_fn;
    csilk_mutex_t       mutex;
} csilk_hnsw_index_t;

/** @brief Create a new empty HNSW index. */
csilk_hnsw_index_t* csilk_hnsw_index_new(size_t dim, int metric);
/** @brief Free an HNSW index and all its nodes. */
void csilk_hnsw_index_free(csilk_hnsw_index_t* index);
/** @brief Insert a vector into the index under a document id. */
int csilk_hnsw_insert(csilk_hnsw_index_t* index, const char* doc_id, const float* vector);
/** @brief Exact nearest-neighbour search over all indexed vectors. */
int csilk_hnsw_search(csilk_hnsw_index_t* index,
                      const float*        query_vector,
                      size_t              top_k,
                      char***             out_doc_ids,
                      float**             out_scores,
                      size_t*             out_count);

#endif /* CSILK_VECTOR_INTERNAL_H */
