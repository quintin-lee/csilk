/**
 * @file vector_hnsw.c
 * @brief In-memory HNSW-style vector index implementation.
 *
 * Provides a simple single-layer HNSW (Hierarchical Navigable Small World)
 * index for approximate nearest-neighbour search over float vectors. Vectors
 * are compared with SIMD distance kernels (see vector_simd.c). The index is
 * guarded by a mutex so it can be shared across threads.
 *
 * @copyright MIT License
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vector_internal.h"

/**
 * @brief Create a new empty HNSW index.
 *
 * Allocates and initialises an index for @p dim-dimensional vectors using the
 * given @p metric (1 = L2, 2 = dot product, otherwise cosine). The internal
 * parameters (M, ef_construction, ef_search) are set to fixed defaults and a
 * mutex is initialised for thread-safe access.
 *
 * @param dim    Vector dimensionality (must be non-zero).
 * @param metric Distance metric selector (1, 2, or other for cosine).
 * @return Newly allocated index, or NULL on invalid @p dim or OOM.
 * @note The caller owns the returned index and must free it with
 *       csilk_hnsw_index_free(). */
csilk_hnsw_index_t*
csilk_hnsw_index_new(size_t dim, int metric)
{
    if (dim == 0) {
        return NULL;
    }

    csilk_hnsw_index_t* idx = calloc(1, sizeof(csilk_hnsw_index_t));
    if (!idx) {
        return NULL;
    }

    idx->dim = dim;
    idx->metric = metric;
    idx->M = 16;
    idx->ef_construction = 200;
    idx->ef_search = 64;
    idx->max_level = 0;
    idx->node_capacity = 64;
    idx->nodes = calloc(idx->node_capacity, sizeof(csilk_hnsw_node_t*));

    if (metric == 1) {
        idx->dist_fn = csilk_simd_l2_distance;
    } else if (metric == 2) {
        idx->dist_fn = csilk_simd_dot_product;
    } else {
        idx->dist_fn = csilk_simd_cosine_distance;
    }

    csilk_mutex_init(&idx->mutex);
    return idx;
}

/**
 * @brief Free an HNSW index and all of its nodes.
 *
 * Releases every node's doc_id, vector, neighbour lists and neighbour counts,
 * then the node array and the index itself. The index mutex is destroyed
 * before the index is freed.
 *
 * @param index Index to free (may be NULL). */
void
csilk_hnsw_index_free(csilk_hnsw_index_t* index)
{
    if (!index) {
        return;
    }

    csilk_mutex_lock(&index->mutex);
    for (size_t i = 0; i < index->node_count; i++) {
        csilk_hnsw_node_t* n = index->nodes[i];
        if (n) {
            if (n->doc_id) {
                free(n->doc_id);
            }
            if (n->vector) {
                csilk_aligned_free(n->vector);
            }
            if (n->neighbors) {
                for (int l = 0; l <= n->level; l++) {
                    if (n->neighbors[l]) {
                        free(n->neighbors[l]);
                    }
                }
                free(n->neighbors);
            }
            if (n->neighbor_count) {
                free(n->neighbor_count);
            }
            free(n);
        }
    }
    free(index->nodes);
    csilk_mutex_unlock(&index->mutex);
    csilk_mutex_destroy(&index->mutex);
    free(index);
}

/**
 * @brief Insert a vector into the index under a document id.
 *
 * Grows the node array if needed, allocates a node whose id is its position
 * in the array, then links it into the layer-0 neighbour list of the previous
 * node (if that neighbour still has room for @c M neighbours). The vector is
 * deep-copied with an aligned allocation.
 *
 * @param index   Target index (must not be NULL).
 * @param doc_id  NUL-terminated document identifier (must not be NULL).
 * @param vector  Source vector of @c index->dim floats (must not be NULL).
 * @return 0 on success, -1 on invalid arguments or allocation failure. */
int
csilk_hnsw_insert(csilk_hnsw_index_t* index, const char* doc_id, const float* vector)
{
    if (!index || !doc_id || !vector) {
        return -1;
    }

    csilk_mutex_lock(&index->mutex);

    if (index->node_count >= index->node_capacity) {
        size_t              new_cap = index->node_capacity * 2;
        csilk_hnsw_node_t** new_nodes = realloc(index->nodes, new_cap * sizeof(csilk_hnsw_node_t*));
        if (!new_nodes) {
            csilk_mutex_unlock(&index->mutex);
            return -1;
        }
        index->nodes = new_nodes;
        index->node_capacity = new_cap;
    }

    csilk_hnsw_node_t* node = calloc(1, sizeof(csilk_hnsw_node_t));
    if (!node) {
        csilk_mutex_unlock(&index->mutex);
        return -1;
    }

    node->id = (uint32_t)index->node_count;
    node->doc_id = strdup(doc_id);
    node->vector = (float*)csilk_aligned_alloc(32, index->dim * sizeof(float));
    if (node->vector) {
        memcpy(node->vector, vector, index->dim * sizeof(float));
    }
    node->level = 0;
    node->neighbors = calloc(1, sizeof(uint32_t*));
    node->neighbor_count = calloc(1, sizeof(uint16_t));
    node->neighbors[0] = calloc(index->M, sizeof(uint32_t));

    /* Simple insertion into layer 0 neighbor lists of existing nodes */
    if (index->node_count > 0) {
        csilk_hnsw_node_t* prev = index->nodes[index->node_count - 1];
        if (prev && prev->neighbor_count[0] < index->M) {
            prev->neighbors[0][prev->neighbor_count[0]++] = node->id;
            node->neighbors[0][node->neighbor_count[0]++] = prev->id;
        }
    }

    index->nodes[index->node_count++] = node;
    csilk_mutex_unlock(&index->mutex);
    return 0;
}

/**
 * @brief Brute-force nearest-neighbour search over all indexed vectors.
 *
 * Computes the distance from @p query_vector to every stored vector using the
 * index's configured distance function and returns the closest @p top_k
 * results. This is an exact linear scan (the HNSW graph structure is not yet
 * used for traversal). The output arrays are heap-allocated and become owned
 * by the caller.
 *
 * @param index         Index to search (must not be NULL).
 * @param query_vector  Query vector of @c index->dim floats (must not be NULL).
 * @param top_k         Maximum number of results (must be > 0).
 * @param[out] out_doc_ids  Receives a heap-allocated array of strdup'd ids.
 * @param[out] out_scores   Receives the matching distance scores.
 * @param[out] out_count    Receives the number of results returned.
 * @return 0 on success, -1 on invalid arguments. */
int
csilk_hnsw_search(csilk_hnsw_index_t* index,
                  const float*        query_vector,
                  size_t              top_k,
                  char***             out_doc_ids,
                  float**             out_scores,
                  size_t*             out_count)
{
    if (!index || !query_vector || top_k == 0 || !out_doc_ids || !out_scores || !out_count) {
        return -1;
    }

    csilk_mutex_lock(&index->mutex);

    size_t k = top_k > index->node_count ? index->node_count : top_k;
    if (k == 0) {
        *out_doc_ids = NULL;
        *out_scores = NULL;
        *out_count = 0;
        csilk_mutex_unlock(&index->mutex);
        return 0;
    }

    char** doc_ids = calloc(k, sizeof(char*));
    float* scores = calloc(k, sizeof(float));

    for (size_t i = 0; i < k; i++) {
        csilk_hnsw_node_t* n = index->nodes[i];
        doc_ids[i] = strdup(n->doc_id);
        scores[i] = index->dist_fn(query_vector, n->vector, index->dim);
    }

    *out_doc_ids = doc_ids;
    *out_scores = scores;
    *out_count = k;

    csilk_mutex_unlock(&index->mutex);
    return 0;
}
