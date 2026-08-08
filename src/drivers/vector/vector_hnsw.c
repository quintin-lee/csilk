#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vector_internal.h"

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
