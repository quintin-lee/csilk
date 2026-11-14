/**
 * @file vector_hnsw.c
 * @brief In-memory multi-layer HNSW vector index implementation.
 *
 * Implements Hierarchical Navigable Small World (HNSW) approximate
 * nearest-neighbour indexing and search over float vectors. Vectors are
 * compared with SIMD distance kernels (see vector_simd.c). The index is
 * guarded by a mutex so it can be shared across threads.
 *
 * @copyright MIT License
 */

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "vector_internal.h"

enum { HNSW_MAX_LEVEL = 16 };

typedef struct {
    float    dist;
    uint32_t id;
} hnsw_cand_t;

typedef struct {
    hnsw_cand_t* data;
    size_t       size;
    size_t       cap;
    bool         is_max;
} hnsw_heap_t;

static inline size_t
hnsw_layer_cap(const csilk_hnsw_index_t* index, int level)
{
    return level == 0 ? (index->M * 2) : index->M;
}

static uint64_t
hnsw_rng_next(csilk_hnsw_index_t* index)
{
    uint64_t x = index->rng_state;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    index->rng_state = x;
    return x * 2685821657736338717ULL;
}

static int
hnsw_random_level(csilk_hnsw_index_t* index)
{
    /* Map upper 53 bits to (0, 1] for -ln(u) * mL, with mL = 1 / ln(M). */
    uint64_t r = hnsw_rng_next(index) >> 11;
    double   u = ((double)r + 1.0) / 9007199254740992.0;
    double   ml = 1.0 / log((double)index->M);
    int      lvl = (int)(-log(u) * ml);
    if (lvl < 0) {
        lvl = 0;
    } else if (lvl > HNSW_MAX_LEVEL) {
        lvl = HNSW_MAX_LEVEL;
    }
    return lvl;
}

static int
hnsw_heap_init(hnsw_heap_t* h, size_t initial_cap, bool is_max)
{
    h->cap = initial_cap > 0 ? initial_cap : 16;
    h->size = 0;
    h->is_max = is_max;
    h->data = malloc(h->cap * sizeof(hnsw_cand_t));
    return h->data ? 0 : -1;
}

static void
hnsw_heap_free(hnsw_heap_t* h)
{
    if (!h) {
        return;
    }
    free(h->data);
    h->data = NULL;
    h->size = 0;
    h->cap = 0;
}

static inline bool
hnsw_heap_better(bool is_max, hnsw_cand_t a, hnsw_cand_t b)
{
    return is_max ? (a.dist > b.dist) : (a.dist < b.dist);
}

static int
hnsw_heap_push(hnsw_heap_t* h, hnsw_cand_t val)
{
    if (h->size == h->cap) {
        size_t       new_cap = h->cap * 2;
        hnsw_cand_t* next = realloc(h->data, new_cap * sizeof(hnsw_cand_t));
        if (!next) {
            return -1;
        }
        h->data = next;
        h->cap = new_cap;
    }

    size_t i = h->size++;
    while (i > 0) {
        size_t p = (i - 1) / 2;
        if (!hnsw_heap_better(h->is_max, val, h->data[p])) {
            break;
        }
        h->data[i] = h->data[p];
        i = p;
    }
    h->data[i] = val;
    return 0;
}

static hnsw_cand_t
hnsw_heap_pop(hnsw_heap_t* h)
{
    hnsw_cand_t top = h->data[0];
    h->size--;
    if (h->size > 0) {
        hnsw_cand_t last = h->data[h->size];
        size_t      i = 0;
        while (true) {
            size_t left = 2 * i + 1;
            size_t right = left + 1;
            if (left >= h->size) {
                break;
            }
            size_t best = left;
            if (right < h->size && hnsw_heap_better(h->is_max, h->data[right], h->data[left])) {
                best = right;
            }
            if (!hnsw_heap_better(h->is_max, h->data[best], last)) {
                break;
            }
            h->data[i] = h->data[best];
            i = best;
        }
        h->data[i] = last;
    }
    return top;
}

static int
hnsw_cand_cmp_asc(const void* pa, const void* pb)
{
    const hnsw_cand_t* a = (const hnsw_cand_t*)pa;
    const hnsw_cand_t* b = (const hnsw_cand_t*)pb;
    if (a->dist < b->dist) {
        return -1;
    }
    if (a->dist > b->dist) {
        return 1;
    }
    return (a->id > b->id) - (a->id < b->id);
}

static int
hnsw_search_layer(const csilk_hnsw_index_t* index,
                  const float*              query,
                  uint32_t                  entry_id,
                  size_t                    ef,
                  int                       layer,
                  hnsw_cand_t**             out_results,
                  size_t*                   out_count)
{
    size_t words = (index->node_count + 63) / 64;
    if (words == 0) {
        *out_results = NULL;
        *out_count = 0;
        return 0;
    }

    uint64_t* visited = calloc(words, sizeof(uint64_t));
    if (!visited) {
        return -1;
    }

    hnsw_heap_t candidates = {0};
    hnsw_heap_t results = {0};
    if (hnsw_heap_init(&candidates, ef * 2, false) != 0 ||
        hnsw_heap_init(&results, ef + 1, true) != 0) {
        free(visited);
        hnsw_heap_free(&candidates);
        hnsw_heap_free(&results);
        return -1;
    }

    const csilk_hnsw_node_t* entry = index->nodes[entry_id];
    float                    entry_dist = index->dist_fn(query, entry->vector, index->dim);
    hnsw_cand_t              start = {entry_dist, entry_id};

    visited[entry_id >> 6] |= (1ULL << (entry_id & 63));
    if (hnsw_heap_push(&candidates, start) != 0 || hnsw_heap_push(&results, start) != 0) {
        free(visited);
        hnsw_heap_free(&candidates);
        hnsw_heap_free(&results);
        return -1;
    }

    while (candidates.size > 0) {
        hnsw_cand_t curr = hnsw_heap_pop(&candidates);
        hnsw_cand_t worst = results.data[0];
        if (results.size >= ef && curr.dist > worst.dist) {
            break;
        }

        const csilk_hnsw_node_t* curr_node = index->nodes[curr.id];
        if (!curr_node || layer > curr_node->level) {
            continue;
        }

        uint16_t        nb_count = curr_node->neighbor_count[layer];
        const uint32_t* nbs = curr_node->neighbors[layer];
        for (uint16_t i = 0; i < nb_count; i++) {
            uint32_t nb_id = nbs[i];
            if (nb_id >= index->node_count) {
                continue;
            }
            uint64_t mask = 1ULL << (nb_id & 63);
            if ((visited[nb_id >> 6] & mask) != 0) {
                continue;
            }
            visited[nb_id >> 6] |= mask;

            const csilk_hnsw_node_t* nb_node = index->nodes[nb_id];
            if (!nb_node || !nb_node->vector) {
                continue;
            }
            float d = index->dist_fn(query, nb_node->vector, index->dim);

            if (results.size < ef || d < results.data[0].dist) {
                hnsw_cand_t item = {d, nb_id};
                if (hnsw_heap_push(&candidates, item) != 0 || hnsw_heap_push(&results, item) != 0) {
                    free(visited);
                    hnsw_heap_free(&candidates);
                    hnsw_heap_free(&results);
                    return -1;
                }
                if (results.size > ef) {
                    (void)hnsw_heap_pop(&results);
                }
            }
        }
    }

    free(visited);
    hnsw_heap_free(&candidates);

    qsort(results.data, results.size, sizeof(hnsw_cand_t), hnsw_cand_cmp_asc);
    *out_results = results.data;
    *out_count = results.size;
    return 0;
}

static uint32_t
hnsw_greedy_descend_one(const csilk_hnsw_index_t* index,
                        const float*              query,
                        uint32_t                  curr_id,
                        float*                    curr_dist,
                        int                       layer)
{
    bool changed = true;
    while (changed) {
        changed = false;
        const csilk_hnsw_node_t* curr = index->nodes[curr_id];
        if (!curr || layer > curr->level) {
            break;
        }
        uint16_t        nb_count = curr->neighbor_count[layer];
        const uint32_t* nbs = curr->neighbors[layer];
        for (uint16_t i = 0; i < nb_count; i++) {
            uint32_t nb_id = nbs[i];
            if (nb_id >= index->node_count) {
                continue;
            }
            const csilk_hnsw_node_t* nb = index->nodes[nb_id];
            if (!nb || !nb->vector) {
                continue;
            }
            float d = index->dist_fn(query, nb->vector, index->dim);
            if (d < *curr_dist) {
                *curr_dist = d;
                curr_id = nb_id;
                changed = true;
            }
        }
    }
    return curr_id;
}

/* HNSW Paper Algorithm 4: diversity-aware neighbor selection with backfill. */
static size_t
hnsw_select_neighbors_heuristic(const csilk_hnsw_index_t* index,
                                const hnsw_cand_t*        sorted_cands,
                                size_t                    cand_count,
                                size_t                    max_conn,
                                uint32_t*                 out_ids)
{
    size_t selected = 0;
    if (cand_count == 0 || max_conn == 0) {
        return 0;
    }

    bool* discarded = calloc(cand_count, sizeof(bool));
    if (!discarded) {
        size_t take = cand_count < max_conn ? cand_count : max_conn;
        for (size_t i = 0; i < take; i++) {
            out_ids[i] = sorted_cands[i].id;
        }
        return take;
    }

    for (size_t i = 0; i < cand_count && selected < max_conn; i++) {
        uint32_t                 cid = sorted_cands[i].id;
        float                    c_to_q = sorted_cands[i].dist;
        const csilk_hnsw_node_t* c_node = index->nodes[cid];
        bool                     good = true;

        for (size_t j = 0; j < selected; j++) {
            const csilk_hnsw_node_t* s_node = index->nodes[out_ids[j]];
            float c_to_s = index->dist_fn(c_node->vector, s_node->vector, index->dim);
            if (c_to_s < c_to_q) {
                good = false;
                break;
            }
        }

        if (good) {
            out_ids[selected++] = cid;
        } else {
            discarded[i] = true;
        }
    }

    /* Backfill from discarded closest candidates to preserve graph connectivity. */
    for (size_t i = 0; i < cand_count && selected < max_conn; i++) {
        if (discarded[i]) {
            out_ids[selected++] = sorted_cands[i].id;
        }
    }

    free(discarded);
    return selected;
}

static void
hnsw_add_bidirectional_link(const csilk_hnsw_index_t* index,
                            csilk_hnsw_node_t*        node,
                            uint32_t                  nb_id,
                            int                       layer)
{
    csilk_hnsw_node_t* nb = index->nodes[nb_id];
    if (!nb || layer > nb->level) {
        return;
    }

    size_t cap = hnsw_layer_cap(index, layer);

    /* Avoid duplicate links on node */
    bool dup_node = false;
    for (uint16_t i = 0; i < node->neighbor_count[layer]; i++) {
        if (node->neighbors[layer][i] == nb_id) {
            dup_node = true;
            break;
        }
    }
    if (!dup_node && node->neighbor_count[layer] < cap) {
        node->neighbors[layer][node->neighbor_count[layer]++] = nb_id;
    }

    /* Avoid duplicate links on nb */
    for (uint16_t i = 0; i < nb->neighbor_count[layer]; i++) {
        if (nb->neighbors[layer][i] == node->id) {
            return;
        }
    }

    if (nb->neighbor_count[layer] < cap) {
        nb->neighbors[layer][nb->neighbor_count[layer]++] = node->id;
        return;
    }

    /* Neighbor list is full: re-select best `cap` neighbors using heuristic. */
    size_t       total = (size_t)nb->neighbor_count[layer] + 1;
    hnsw_cand_t* cands = malloc(total * sizeof(hnsw_cand_t));
    if (!cands) {
        return;
    }
    for (uint16_t i = 0; i < nb->neighbor_count[layer]; i++) {
        uint32_t existing_id = nb->neighbors[layer][i];
        cands[i].id = existing_id;
        cands[i].dist = index->dist_fn(nb->vector, index->nodes[existing_id]->vector, index->dim);
    }
    cands[total - 1].id = node->id;
    cands[total - 1].dist = index->dist_fn(nb->vector, node->vector, index->dim);

    qsort(cands, total, sizeof(hnsw_cand_t), hnsw_cand_cmp_asc);
    size_t kept = hnsw_select_neighbors_heuristic(index, cands, total, cap, nb->neighbors[layer]);
    nb->neighbor_count[layer] = (uint16_t)kept;
    free(cands);
}

static void
hnsw_unlink_node(const csilk_hnsw_index_t* index,
                 const csilk_hnsw_node_t*  node,
                 int                       min_layer,
                 int                       max_layer)
{
    for (int l = min_layer; l <= max_layer; l++) {
        for (uint16_t i = 0; i < node->neighbor_count[l]; i++) {
            uint32_t nb_id = node->neighbors[l][i];
            if (nb_id >= index->node_count) {
                continue;
            }
            csilk_hnsw_node_t* nb = index->nodes[nb_id];
            if (!nb || l > nb->level) {
                continue;
            }
            for (uint16_t j = 0; j < nb->neighbor_count[l]; j++) {
                if (nb->neighbors[l][j] == node->id) {
                    nb->neighbors[l][j] = nb->neighbors[l][--nb->neighbor_count[l]];
                    break;
                }
            }
        }
    }
}

static void
hnsw_free_node(csilk_hnsw_node_t* n)
{
    if (!n) {
        return;
    }
    free(n->doc_id);
    if (n->vector) {
        csilk_aligned_free(n->vector);
    }
    if (n->neighbors) {
        for (int l = 0; l <= n->level; l++) {
            free(n->neighbors[l]);
        }
        free(n->neighbors);
    }
    free(n->neighbor_count);
    free(n);
}

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
    idx->enter_node_id = 0;
    idx->rng_state = 0x9e3779b97f4a7c15ULL;
    idx->node_capacity = 64;
    idx->nodes = calloc(idx->node_capacity, sizeof(csilk_hnsw_node_t*));
    if (!idx->nodes) {
        free(idx);
        return NULL;
    }

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
        hnsw_free_node(index->nodes[i]);
    }
    free(index->nodes);
    csilk_mutex_unlock(&index->mutex);
    csilk_mutex_destroy(&index->mutex);
    free(index);
}

/**
 * @brief Insert a vector into the multi-layer HNSW index under a document id.
 *
 * Assigns an exponentially decaying random level, navigates greedily from the
 * entry point down to the node's level, then runs beam search (`ef_construction`)
 * and diversity-aware bidirectional linking at every layer down to layer 0.
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

    int                level = (index->node_count == 0) ? 0 : hnsw_random_level(index);
    csilk_hnsw_node_t* node = calloc(1, sizeof(csilk_hnsw_node_t));
    if (!node) {
        csilk_mutex_unlock(&index->mutex);
        return -1;
    }

    node->id = (uint32_t)index->node_count;
    node->level = level;
    node->doc_id = strdup(doc_id);
    node->vector = (float*)csilk_aligned_alloc(32, index->dim * sizeof(float));
    node->neighbors = calloc((size_t)(level + 1), sizeof(uint32_t*));
    node->neighbor_count = calloc((size_t)(level + 1), sizeof(uint16_t));
    if (!node->doc_id || !node->vector || !node->neighbors || !node->neighbor_count) {
        hnsw_free_node(node);
        csilk_mutex_unlock(&index->mutex);
        return -1;
    }
    memcpy(node->vector, vector, index->dim * sizeof(float));

    for (int l = 0; l <= level; l++) {
        size_t cap = hnsw_layer_cap(index, l);
        node->neighbors[l] = calloc(cap, sizeof(uint32_t));
        if (!node->neighbors[l]) {
            hnsw_free_node(node);
            csilk_mutex_unlock(&index->mutex);
            return -1;
        }
    }

    index->nodes[index->node_count] = node;

    if (index->node_count == 0) {
        index->enter_node_id = node->id;
        index->max_level = level;
        index->node_count++;
        csilk_mutex_unlock(&index->mutex);
        return 0;
    }

    uint32_t curr_id = index->enter_node_id;
    float    curr_dist = index->dist_fn(node->vector, index->nodes[curr_id]->vector, index->dim);

    /* Phase 1: greedy descent from top layer down to level + 1 */
    for (int l = index->max_level; l > level; l--) {
        curr_id = hnsw_greedy_descend_one(index, node->vector, curr_id, &curr_dist, l);
    }

    /* Phase 2: beam search & bidirectional linking from min(level, max_level) down to 0 */
    int       top_insert_layer = level < index->max_level ? level : index->max_level;
    uint32_t* selected_ids = malloc(index->M * sizeof(uint32_t));
    if (!selected_ids) {
        hnsw_free_node(node);
        index->nodes[index->node_count] = NULL;
        csilk_mutex_unlock(&index->mutex);
        return -1;
    }

    /* Temporarily exclude the new node from graph traversal during linking by
     * keeping node_count unincremented until linking finishes (node->id == node_count,
     * and search_layer checks nb_id < index->node_count). */
    for (int l = top_insert_layer; l >= 0; l--) {
        hnsw_cand_t* cands = NULL;
        size_t       cand_count = 0;
        if (hnsw_search_layer(
                index, node->vector, curr_id, index->ef_construction, l, &cands, &cand_count) !=
            0) {
            hnsw_unlink_node(index, node, l + 1, top_insert_layer);
            free(selected_ids);
            hnsw_free_node(node);
            index->nodes[index->node_count] = NULL;
            csilk_mutex_unlock(&index->mutex);
            return -1;
        }

        size_t n_sel =
            hnsw_select_neighbors_heuristic(index, cands, cand_count, index->M, selected_ids);
        for (size_t i = 0; i < n_sel; i++) {
            hnsw_add_bidirectional_link(index, node, selected_ids[i], l);
        }

        if (cand_count > 0) {
            curr_id = cands[0].id;
        }
        free(cands);
    }

    free(selected_ids);
    index->node_count++;

    if (level > index->max_level) {
        index->max_level = level;
        index->enter_node_id = node->id;
    }

    csilk_mutex_unlock(&index->mutex);
    return 0;
}

/**
 * @brief Approximate nearest-neighbour search using the multi-layer HNSW graph.
 *
 * Greedily traverses upper layers from the entry point down to layer 1, then
 * performs an @c ef_search beam search at layer 0 to retrieve up to @p top_k
 * nearest neighbours sorted in ascending order of distance.
 *
 * @param index         Index to search (must not be NULL).
 * @param query_vector  Query vector of @c index->dim floats (must not be NULL).
 * @param top_k         Maximum number of results (must be > 0).
 * @param[out] out_doc_ids  Receives a heap-allocated array of strdup'd ids.
 * @param[out] out_scores   Receives the matching distance scores.
 * @param[out] out_count    Receives the number of results returned.
 * @return 0 on success, -1 on invalid arguments or allocation failure. */
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

    uint32_t curr_id = index->enter_node_id;
    float    curr_dist = index->dist_fn(query_vector, index->nodes[curr_id]->vector, index->dim);

    for (int l = index->max_level; l >= 1; l--) {
        curr_id = hnsw_greedy_descend_one(index, query_vector, curr_id, &curr_dist, l);
    }

    size_t       ef = index->ef_search > k ? index->ef_search : k;
    hnsw_cand_t* results = NULL;
    size_t       res_count = 0;
    if (hnsw_search_layer(index, query_vector, curr_id, ef, 0, &results, &res_count) != 0) {
        csilk_mutex_unlock(&index->mutex);
        return -1;
    }

    size_t out_n = res_count < k ? res_count : k;
    if (out_n == 0) {
        free(results);
        *out_doc_ids = NULL;
        *out_scores = NULL;
        *out_count = 0;
        csilk_mutex_unlock(&index->mutex);
        return 0;
    }
    char** doc_ids = calloc(out_n, sizeof(char*));
    float* scores = calloc(out_n, sizeof(float));
    if (!doc_ids || !scores) {
        free(doc_ids);
        free(scores);
        free(results);
        csilk_mutex_unlock(&index->mutex);
        return -1;
    }

    for (size_t i = 0; i < out_n; i++) {
        doc_ids[i] = strdup(index->nodes[results[i].id]->doc_id);
        if (!doc_ids[i]) {
            for (size_t j = 0; j < i; j++) {
                free(doc_ids[j]);
            }
            free(doc_ids);
            free(scores);
            free(results);
            csilk_mutex_unlock(&index->mutex);
            return -1;
        }
        scores[i] = results[i].dist;
    }

    free(results);
    *out_doc_ids = doc_ids;
    *out_scores = scores;
    *out_count = out_n;

    csilk_mutex_unlock(&index->mutex);
    return 0;
}

#define CSILK_HNSW_MAGIC 0x57534E48U /* "HNSW" */
#define CSILK_HNSW_VERSION 1U

int
csilk_hnsw_save(const csilk_hnsw_index_t* index, const char* filepath)
{
    if (!index || !filepath) {
        return -1;
    }
    FILE* fp = fopen(filepath, "wb");
    if (!fp) {
        return -1;
    }

    uint32_t magic = CSILK_HNSW_MAGIC;
    uint32_t version = CSILK_HNSW_VERSION;
    uint32_t dim = (uint32_t)index->dim;
    int32_t  metric = (int32_t)index->metric;
    uint32_t m = (uint32_t)index->M;
    uint32_t ef_c = (uint32_t)index->ef_construction;
    uint32_t ef_s = (uint32_t)index->ef_search;
    uint32_t enter_id = index->enter_node_id;
    int32_t  max_level = index->max_level;
    uint32_t node_count = (uint32_t)index->node_count;

    if (fwrite(&magic, sizeof(magic), 1, fp) != 1 ||
        fwrite(&version, sizeof(version), 1, fp) != 1 || fwrite(&dim, sizeof(dim), 1, fp) != 1 ||
        fwrite(&metric, sizeof(metric), 1, fp) != 1 || fwrite(&m, sizeof(m), 1, fp) != 1 ||
        fwrite(&ef_c, sizeof(ef_c), 1, fp) != 1 || fwrite(&ef_s, sizeof(ef_s), 1, fp) != 1 ||
        fwrite(&enter_id, sizeof(enter_id), 1, fp) != 1 ||
        fwrite(&max_level, sizeof(max_level), 1, fp) != 1 ||
        fwrite(&node_count, sizeof(node_count), 1, fp) != 1) {
        fclose(fp);
        return -1;
    }

    for (uint32_t i = 0; i < node_count; i++) {
        const csilk_hnsw_node_t* node = index->nodes[i];
        uint32_t                 id = node->id;
        int32_t                  level = node->level;
        uint32_t                 doc_len = (uint32_t)strlen(node->doc_id);

        if (fwrite(&id, sizeof(id), 1, fp) != 1 || fwrite(&level, sizeof(level), 1, fp) != 1 ||
            fwrite(&doc_len, sizeof(doc_len), 1, fp) != 1 ||
            fwrite(node->doc_id, 1, doc_len, fp) != doc_len ||
            fwrite(node->vector, sizeof(float), dim, fp) != dim) {
            fclose(fp);
            return -1;
        }

        for (int l = 0; l <= level; l++) {
            uint16_t deg = node->neighbor_count[l];
            if (fwrite(&deg, sizeof(deg), 1, fp) != 1) {
                fclose(fp);
                return -1;
            }
            if (deg > 0) {
                if (fwrite(node->neighbors[l], sizeof(uint32_t), deg, fp) != deg) {
                    fclose(fp);
                    return -1;
                }
            }
        }
    }

    fclose(fp);
    return 0;
}

csilk_hnsw_index_t*
csilk_hnsw_load(const char* filepath)
{
    if (!filepath) {
        return NULL;
    }
    FILE* fp = fopen(filepath, "rb");
    if (!fp) {
        return NULL;
    }

    uint32_t magic = 0, version = 0, dim = 0, m = 0, ef_c = 0, ef_s = 0;
    uint32_t enter_id = 0, node_count = 0;
    int32_t  metric = 0, max_level = 0;

    if (fread(&magic, sizeof(magic), 1, fp) != 1 || magic != CSILK_HNSW_MAGIC ||
        fread(&version, sizeof(version), 1, fp) != 1 || version != CSILK_HNSW_VERSION ||
        fread(&dim, sizeof(dim), 1, fp) != 1 || fread(&metric, sizeof(metric), 1, fp) != 1 ||
        fread(&m, sizeof(m), 1, fp) != 1 || fread(&ef_c, sizeof(ef_c), 1, fp) != 1 ||
        fread(&ef_s, sizeof(ef_s), 1, fp) != 1 || fread(&enter_id, sizeof(enter_id), 1, fp) != 1 ||
        fread(&max_level, sizeof(max_level), 1, fp) != 1 ||
        fread(&node_count, sizeof(node_count), 1, fp) != 1) {
        fclose(fp);
        return NULL;
    }

    csilk_hnsw_index_t* index = csilk_hnsw_index_new((size_t)dim, (int)metric);
    if (!index) {
        fclose(fp);
        return NULL;
    }

    index->M = m;
    index->ef_construction = ef_c;
    index->ef_search = ef_s;
    index->enter_node_id = enter_id;
    index->max_level = max_level;

    if (node_count > index->node_capacity) {
        size_t              new_cap = node_count + 64;
        csilk_hnsw_node_t** new_nodes = realloc(index->nodes, new_cap * sizeof(csilk_hnsw_node_t*));
        if (!new_nodes) {
            csilk_hnsw_index_free(index);
            fclose(fp);
            return NULL;
        }
        index->nodes = new_nodes;
        index->node_capacity = new_cap;
    }

    for (uint32_t i = 0; i < node_count; i++) {
        uint32_t id = 0, doc_len = 0;
        int32_t  level = 0;
        if (fread(&id, sizeof(id), 1, fp) != 1 || fread(&level, sizeof(level), 1, fp) != 1 ||
            fread(&doc_len, sizeof(doc_len), 1, fp) != 1) {
            csilk_hnsw_index_free(index);
            fclose(fp);
            return NULL;
        }

        csilk_hnsw_node_t* node = calloc(1, sizeof(csilk_hnsw_node_t));
        if (!node) {
            csilk_hnsw_index_free(index);
            fclose(fp);
            return NULL;
        }

        node->id = id;
        node->level = level;
        node->doc_id = malloc((size_t)doc_len + 1);
        if (!node->doc_id || fread(node->doc_id, 1, doc_len, fp) != doc_len) {
            free(node->doc_id);
            free(node);
            csilk_hnsw_index_free(index);
            fclose(fp);
            return NULL;
        }
        node->doc_id[doc_len] = '\0';

        node->vector = csilk_aligned_alloc(32, (size_t)dim * sizeof(float));
        if (!node->vector || fread(node->vector, sizeof(float), dim, fp) != dim) {
            csilk_aligned_free(node->vector);
            free(node->doc_id);
            free(node);
            csilk_hnsw_index_free(index);
            fclose(fp);
            return NULL;
        }

        node->neighbors = calloc((size_t)level + 1, sizeof(uint32_t*));
        node->neighbor_count = calloc((size_t)level + 1, sizeof(uint16_t));
        if (!node->neighbors || !node->neighbor_count) {
            hnsw_free_node(node);
            csilk_hnsw_index_free(index);
            fclose(fp);
            return NULL;
        }

        for (int l = 0; l <= level; l++) {
            uint16_t deg = 0;
            if (fread(&deg, sizeof(deg), 1, fp) != 1) {
                hnsw_free_node(node);
                csilk_hnsw_index_free(index);
                fclose(fp);
                return NULL;
            }
            node->neighbor_count[l] = deg;
            size_t cap = hnsw_layer_cap(index, l);
            node->neighbors[l] = malloc(cap * sizeof(uint32_t));
            if (!node->neighbors[l]) {
                hnsw_free_node(node);
                csilk_hnsw_index_free(index);
                fclose(fp);
                return NULL;
            }
            if (deg > 0) {
                if (fread(node->neighbors[l], sizeof(uint32_t), deg, fp) != deg) {
                    hnsw_free_node(node);
                    csilk_hnsw_index_free(index);
                    fclose(fp);
                    return NULL;
                }
            }
        }

        index->nodes[index->node_count++] = node;
    }

    fclose(fp);
    return index;
}
