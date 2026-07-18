/**
 * @file mvcc_cache.c
 * @brief Epoch-based RCU / MVCC Lock-Free In-Memory Cache Implementation.
 * @copyright MIT License
 */

#include "csilk/core/mvcc_cache.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct csilk_mvcc_node_s {
    char*                              key;
    void*                              val;
    size_t                             val_len;
    uint64_t                           version;
    _Atomic(struct csilk_mvcc_node_s*) next;
} csilk_mvcc_node_t;

struct csilk_mvcc_cache_s {
    size_t                      capacity;
    atomic_uint_fast64_t        global_epoch;
    _Atomic(csilk_mvcc_node_t*) buckets[];
};

static uint64_t
hash_key(const char* key)
{
    uint64_t h = 14695981039346656037ULL;
    for (const char* p = key; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

csilk_mvcc_cache_t*
csilk_mvcc_cache_new(size_t capacity)
{
    if (capacity == 0) {
        capacity = 1024;
    }
    csilk_mvcc_cache_t* cache =
        calloc(1, sizeof(csilk_mvcc_cache_t) + sizeof(csilk_mvcc_node_t*) * capacity);
    if (!cache) {
        return NULL;
    }
    cache->capacity = capacity;
    atomic_store(&cache->global_epoch, 1);
    return cache;
}

void
csilk_mvcc_cache_free(csilk_mvcc_cache_t* cache)
{
    if (!cache) {
        return;
    }
    for (size_t i = 0; i < cache->capacity; i++) {
        csilk_mvcc_node_t* curr = atomic_load(&cache->buckets[i]);
        while (curr) {
            csilk_mvcc_node_t* next = atomic_load(&curr->next);
            free(curr->key);
            free(curr->val);
            free(curr);
            curr = next;
        }
    }
    free(cache);
}

int
csilk_mvcc_cache_set(csilk_mvcc_cache_t* cache, const char* key, const void* val, size_t val_len)
{
    if (!cache || !key || !val) {
        return -1;
    }

    uint64_t idx = hash_key(key) % cache->capacity;
    uint64_t epoch = atomic_fetch_add(&cache->global_epoch, 1);

    csilk_mvcc_node_t* new_node = malloc(sizeof(csilk_mvcc_node_t));
    if (!new_node) {
        return -1;
    }
    new_node->key = strdup(key);
    new_node->val = malloc(val_len);
    if (!new_node->key || !new_node->val) {
        free(new_node->key);
        free(new_node->val);
        free(new_node);
        return -1;
    }
    memcpy(new_node->val, val, val_len);
    new_node->val_len = val_len;
    new_node->version = epoch;

    /* Atomically prepend new version using CAS RCU pointer swap */
    csilk_mvcc_node_t* old_head = atomic_load(&cache->buckets[idx]);
    do {
        atomic_store(&new_node->next, old_head);
    } while (!atomic_compare_exchange_weak(&cache->buckets[idx], &old_head, new_node));

    return 0;
}

const void*
csilk_mvcc_cache_get(csilk_mvcc_cache_t* cache, const char* key, size_t* val_len)
{
    if (!cache || !key) {
        return NULL;
    }

    uint64_t           idx = hash_key(key) % cache->capacity;
    csilk_mvcc_node_t* curr = atomic_load_explicit(&cache->buckets[idx], memory_order_acquire);

    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            if (val_len) {
                *val_len = curr->val_len;
            }
            return curr->val;
        }
        curr = atomic_load_explicit(&curr->next, memory_order_acquire);
    }

    return NULL;
}
