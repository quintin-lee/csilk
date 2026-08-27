/**
 * @file mvcc_cache.c
 * @brief Epoch-based RCU / MVCC Lock-Free In-Memory Cache Implementation.
 * @copyright MIT License
 */

#include "csilk/core/mvcc_cache.h"

#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CSILK_MVCC_MAX_READERS 256
#define CSILK_MVCC_RETIRE_THRESHOLD 32

/**
 * @brief Internal node in the MVCC hash table bucket.
 */
typedef struct csilk_mvcc_node_s {
    char*                              key;
    void*                              val;
    size_t                             val_len;
    uint64_t                           version;       /**< Creation epoch */
    uint64_t                           retired_epoch; /**< Retirement epoch */
    _Atomic(struct csilk_mvcc_node_s*) next; /**< Next in bucket chain (immutable for readers) */
    _Atomic(struct csilk_mvcc_node_s*) retired_next; /**< Next in retired chain */
} csilk_mvcc_node_t;

/**
 * @brief Per-reader epoch tracking slot aligned to 64-byte cache line.
 */
typedef struct csilk_mvcc_reader_slot_s {
    _Atomic(uint64_t)  active_epoch;  /**< 0 = inactive, >0 = active epoch */
    _Atomic(uintptr_t) owner_tid;     /**< Owner pthread ID or token */
    _Atomic(uint32_t)  nesting_depth; /**< Reader nesting depth on same thread */
    char               _pad[44];      /**< Pad to 64-byte cache line (64 - 8 - 8 - 4) */
} csilk_mvcc_reader_slot_t;

/**
 * @brief MVCC Cache Structure with Epoch-Based Reclamation.
 */
struct csilk_mvcc_cache_s {
    size_t                      capacity;      /**< Number of hash buckets */
    _Atomic(uint64_t)           global_epoch;  /**< Monotonically increasing epoch */
    _Atomic(uint32_t)           retired_count; /**< Approximate count of retired nodes */
    _Atomic(uint32_t)           reclaim_lock;  /**< Lock-free reclamation mutual exclusion */
    _Atomic(uint32_t)           closing;       /**< 1 when shutting down */
    _Atomic(csilk_mvcc_node_t*) retired_head;  /**< Lock-free retired nodes singly linked list */
    csilk_mvcc_reader_slot_t    reader_slots[CSILK_MVCC_MAX_READERS]; /**< Reader epoch slots */
    _Atomic(csilk_mvcc_node_t*) buckets[];                            /**< Hash bucket heads */
};

/** @brief FNV-1a 64-bit hash of a NUL-terminated key. */
static inline uint64_t
hash_key(const char* key)
{
    uint64_t h = 14695981039346656037ULL;
    for (const char* p = key; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;
    }
    return h;
}

static _Thread_local csilk_mvcc_reader_slot_t* tls_mvcc_slot = NULL;
static _Thread_local csilk_mvcc_cache_t*       tls_mvcc_cache = NULL;

/**
 * @brief Acquire or locate a reader epoch slot for the current thread.
 */
static csilk_mvcc_reader_slot_t*
acquire_reader_slot(csilk_mvcc_cache_t* cache)
{
    if (__builtin_expect(tls_mvcc_slot != NULL && tls_mvcc_cache == cache, 1)) {
        return tls_mvcc_slot;
    }

    uintptr_t my_tid = (uintptr_t)pthread_self();
    if (my_tid == 0) {
        my_tid = 1;
    }

    size_t start = (size_t)(my_tid % CSILK_MVCC_MAX_READERS);

    /* 1. Fast path: check if this thread already owns a slot */
    for (size_t i = 0; i < CSILK_MVCC_MAX_READERS; i++) {
        size_t idx = (start + i) % CSILK_MVCC_MAX_READERS;
        if (atomic_load_explicit(&cache->reader_slots[idx].owner_tid, memory_order_relaxed) ==
            my_tid) {
            tls_mvcc_slot = &cache->reader_slots[idx];
            tls_mvcc_cache = cache;
            return tls_mvcc_slot;
        }
    }

    /* 2. Slow path: claim an unused slot */
    for (size_t i = 0; i < CSILK_MVCC_MAX_READERS; i++) {
        size_t    idx = (start + i) % CSILK_MVCC_MAX_READERS;
        uintptr_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&cache->reader_slots[idx].owner_tid,
                                                    &expected,
                                                    my_tid,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            tls_mvcc_slot = &cache->reader_slots[idx];
            tls_mvcc_cache = cache;
            return tls_mvcc_slot;
        }
    }

    /* Fallback: deterministic slot hashing */
    tls_mvcc_slot = &cache->reader_slots[start];
    tls_mvcc_cache = cache;
    return tls_mvcc_slot;
}

/**
 * @brief Enter the reader epoch section.
 */
static inline void
reader_enter(csilk_mvcc_cache_t* cache, csilk_mvcc_reader_slot_t* slot)
{
    uint32_t depth = atomic_fetch_add_explicit(&slot->nesting_depth, 1, memory_order_relaxed);
    if (depth == 0) {
        uint64_t e = atomic_load_explicit(&cache->global_epoch, memory_order_acquire);
        atomic_store_explicit(&slot->active_epoch, e, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);
    }
}

/**
 * @brief Exit the reader epoch section.
 */
static inline void
reader_exit(csilk_mvcc_reader_slot_t* slot)
{
    uint32_t depth = atomic_fetch_sub_explicit(&slot->nesting_depth, 1, memory_order_relaxed);
    if (depth <= 1) {
        atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
    }
}

/**
 * @brief Attempt to advance the global epoch and reclaim safe retired nodes.
 */
static void
try_reclaim(csilk_mvcc_cache_t* cache)
{
    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &cache->reclaim_lock, &expected, 1, memory_order_acquire, memory_order_relaxed)) {
        return; /* Another thread is currently performing reclamation */
    }

    uint64_t current_epoch = atomic_load_explicit(&cache->global_epoch, memory_order_acquire);
    uint64_t min_active_epoch = UINT64_MAX;
    bool     has_active_readers = false;

    for (size_t i = 0; i < CSILK_MVCC_MAX_READERS; i++) {
        uint64_t r_epoch =
            atomic_load_explicit(&cache->reader_slots[i].active_epoch, memory_order_acquire);
        if (r_epoch != 0) {
            has_active_readers = true;
            if (r_epoch < min_active_epoch) {
                min_active_epoch = r_epoch;
            }
        }
    }

    if (!has_active_readers) {
        /* No active readers: all retired nodes up to current_epoch can be reclaimed safely */
        min_active_epoch = current_epoch + 2;
    } else {
        /* Advance global epoch if all readers are caught up */
        if (min_active_epoch >= current_epoch) {
            atomic_fetch_add_explicit(&cache->global_epoch, 1, memory_order_acq_rel);
        }
    }

    /* Detach the entire retired list for processing */
    csilk_mvcc_node_t* list =
        atomic_exchange_explicit(&cache->retired_head, NULL, memory_order_acq_rel);
    csilk_mvcc_node_t* retain_head = NULL;
    uint32_t           retained_count = 0;

    while (list) {
        csilk_mvcc_node_t* next = atomic_load_explicit(&list->retired_next, memory_order_relaxed);
        if (list->retired_epoch + 1 < min_active_epoch) {
            /* Safe to free: no active reader can ever reach this node */
            free(list->key);
            free(list->val);
            free(list);
        } else {
            /* Keep in retired list (still within a reader's grace period) */
            atomic_store_explicit(&list->retired_next, retain_head, memory_order_relaxed);
            retain_head = list;
            retained_count++;
        }
        list = next;
    }

    if (retain_head) {
        /* Re-prepend retained nodes back to retired_head */
        csilk_mvcc_node_t* tail = retain_head;
        while (atomic_load_explicit(&tail->retired_next, memory_order_relaxed) != NULL) {
            tail = atomic_load_explicit(&tail->retired_next, memory_order_relaxed);
        }

        csilk_mvcc_node_t* cur_retired =
            atomic_load_explicit(&cache->retired_head, memory_order_relaxed);
        do {
            atomic_store_explicit(&tail->retired_next, cur_retired, memory_order_relaxed);
        } while (!atomic_compare_exchange_weak_explicit(&cache->retired_head,
                                                        &cur_retired,
                                                        retain_head,
                                                        memory_order_release,
                                                        memory_order_relaxed));
    }

    atomic_store_explicit(&cache->retired_count, retained_count, memory_order_relaxed);
    atomic_store_explicit(&cache->reclaim_lock, 0, memory_order_release);
}

/**
 * @brief Enqueue an unlinked node to the retired list.
 */
static void
retire_node(csilk_mvcc_cache_t* cache, csilk_mvcc_node_t* node, uint64_t retired_epoch)
{
    if (!node) {
        return;
    }
    node->retired_epoch = retired_epoch;
    csilk_mvcc_node_t* old_head = atomic_load_explicit(&cache->retired_head, memory_order_relaxed);
    do {
        atomic_store_explicit(&node->retired_next, old_head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(
        &cache->retired_head, &old_head, node, memory_order_release, memory_order_relaxed));

    uint32_t count = atomic_fetch_add_explicit(&cache->retired_count, 1, memory_order_relaxed);
    if (count >= CSILK_MVCC_RETIRE_THRESHOLD) {
        try_reclaim(cache);
    }
}

/**
 * @brief Helper to allocate a new node.
 */
static csilk_mvcc_node_t*
create_node(const char* key, const void* val, size_t val_len, uint64_t version)
{
    csilk_mvcc_node_t* node = malloc(sizeof(csilk_mvcc_node_t));
    if (!node) {
        return NULL;
    }
    node->key = strdup(key);
    node->val = malloc(val_len);
    if (!node->key || (!node->val && val_len > 0)) {
        free(node->key);
        free(node->val);
        free(node);
        return NULL;
    }
    if (val && val_len > 0) {
        memcpy(node->val, val, val_len);
    }
    node->val_len = val_len;
    node->version = version;
    node->retired_epoch = 0;
    atomic_init(&node->next, NULL);
    atomic_init(&node->retired_next, NULL);
    return node;
}

/**
 * @brief Create an MVCC (epoch/RCU) lock-free in-memory cache.
 */
csilk_mvcc_cache_t*
csilk_mvcc_cache_new(size_t capacity)
{
    if (capacity == 0) {
        capacity = 1024;
    }
    csilk_mvcc_cache_t* cache =
        calloc(1, sizeof(csilk_mvcc_cache_t) + sizeof(_Atomic(csilk_mvcc_node_t*)) * capacity);
    if (!cache) {
        return NULL;
    }
    cache->capacity = capacity;
    atomic_init(&cache->global_epoch, 1);
    atomic_init(&cache->retired_count, 0);
    atomic_init(&cache->reclaim_lock, 0);
    atomic_init(&cache->closing, 0);
    atomic_init(&cache->retired_head, NULL);

    for (size_t i = 0; i < CSILK_MVCC_MAX_READERS; i++) {
        atomic_init(&cache->reader_slots[i].active_epoch, 0);
        atomic_init(&cache->reader_slots[i].owner_tid, 0);
        atomic_init(&cache->reader_slots[i].nesting_depth, 0);
    }

    for (size_t i = 0; i < capacity; i++) {
        atomic_init(&cache->buckets[i], NULL);
    }

    return cache;
}

/**
 * @brief Destroy an MVCC cache, waiting for grace period and freeing all nodes.
 */
void
csilk_mvcc_cache_free(csilk_mvcc_cache_t* cache)
{
    if (!cache) {
        return;
    }

    atomic_store_explicit(&cache->closing, 1, memory_order_release);

    /* Wait for grace period: ensure all active readers exit */
    for (size_t i = 0; i < CSILK_MVCC_MAX_READERS; i++) {
        while (atomic_load_explicit(&cache->reader_slots[i].active_epoch, memory_order_acquire) !=
               0) {
            sched_yield();
        }
    }

    /* Wait to acquire reclaim lock and perform final reclamation */
    uint32_t expected = 0;
    while (!atomic_compare_exchange_weak_explicit(
        &cache->reclaim_lock, &expected, 1, memory_order_acquire, memory_order_relaxed)) {
        expected = 0;
        sched_yield();
    }

    /* Free all retired nodes */
    csilk_mvcc_node_t* ret =
        atomic_exchange_explicit(&cache->retired_head, NULL, memory_order_acq_rel);
    while (ret) {
        csilk_mvcc_node_t* next = atomic_load_explicit(&ret->retired_next, memory_order_relaxed);
        free(ret->key);
        free(ret->val);
        free(ret);
        ret = next;
    }

    /* Free all live bucket nodes */
    for (size_t i = 0; i < cache->capacity; i++) {
        csilk_mvcc_node_t* curr = atomic_load_explicit(&cache->buckets[i], memory_order_relaxed);
        while (curr) {
            csilk_mvcc_node_t* next = atomic_load_explicit(&curr->next, memory_order_relaxed);
            free(curr->key);
            free(curr->val);
            free(curr);
            curr = next;
        }
    }

    free(cache);
}

/**
 * @brief Insert or update a key in the MVCC cache with RCU atomic replacement.
 */
int
csilk_mvcc_cache_set(csilk_mvcc_cache_t* cache, const char* key, const void* val, size_t val_len)
{
    if (!cache || !key || !val) {
        return -1;
    }

    uint64_t idx = hash_key(key) % cache->capacity;
    uint64_t epoch = atomic_load_explicit(&cache->global_epoch, memory_order_acquire);

    csilk_mvcc_node_t* new_node = create_node(key, val, val_len, epoch);
    if (!new_node) {
        return -1;
    }

    csilk_mvcc_reader_slot_t* slot = acquire_reader_slot(cache);

    while (1) {
        reader_enter(cache, slot);
        csilk_mvcc_node_t* old_head =
            atomic_load_explicit(&cache->buckets[idx], memory_order_acquire);

        /* Case 1: Empty bucket */
        if (!old_head) {
            atomic_store_explicit(&new_node->next, NULL, memory_order_relaxed);
            if (atomic_compare_exchange_weak_explicit(&cache->buckets[idx],
                                                      &old_head,
                                                      new_node,
                                                      memory_order_release,
                                                      memory_order_acquire)) {
                reader_exit(slot);
                return 0;
            }
            reader_exit(slot);
            continue;
        }

        /* Case 2: Matching key is at the head of bucket */
        if (strcmp(old_head->key, key) == 0) {
            csilk_mvcc_node_t* old_next =
                atomic_load_explicit(&old_head->next, memory_order_relaxed);
            atomic_store_explicit(&new_node->next, old_next, memory_order_relaxed);
            if (atomic_compare_exchange_weak_explicit(&cache->buckets[idx],
                                                      &old_head,
                                                      new_node,
                                                      memory_order_release,
                                                      memory_order_acquire)) {
                reader_exit(slot);
                retire_node(cache, old_head, epoch);
                return 0;
            }
            reader_exit(slot);
            continue;
        }

        /* Case 3: Key might be deeper in the bucket chain or not present */
        csilk_mvcc_node_t* found_node = NULL;
        csilk_mvcc_node_t* curr = atomic_load_explicit(&old_head->next, memory_order_acquire);
        while (curr) {
            if (strcmp(curr->key, key) == 0) {
                found_node = curr;
                break;
            }
            curr = atomic_load_explicit(&curr->next, memory_order_acquire);
        }

        if (!found_node) {
            /* Key does not exist: prepend new_node */
            atomic_store_explicit(&new_node->next, old_head, memory_order_relaxed);
            if (atomic_compare_exchange_weak_explicit(&cache->buckets[idx],
                                                      &old_head,
                                                      new_node,
                                                      memory_order_release,
                                                      memory_order_acquire)) {
                reader_exit(slot);
                return 0;
            }
            reader_exit(slot);
        } else {
            /* Key exists deeper in chain: build a new chain excluding found_node, prepend new_node */
            /* Build spine: new_node -> copied preceding nodes -> rest of chain */
            csilk_mvcc_node_t* new_chain_head = new_node;
            csilk_mvcc_node_t* prev_alloc = new_node;
            csilk_mvcc_node_t* iter = old_head;
            bool               alloc_failed = false;

            /* Copy all nodes in old chain except found_node */
            while (iter) {
                if (iter != found_node) {
                    csilk_mvcc_node_t* clone =
                        create_node(iter->key, iter->val, iter->val_len, iter->version);
                    if (!clone) {
                        alloc_failed = true;
                        break;
                    }
                    atomic_store_explicit(&prev_alloc->next, clone, memory_order_relaxed);
                    prev_alloc = clone;
                }
                iter = atomic_load_explicit(&iter->next, memory_order_acquire);
            }

            if (alloc_failed) {
                /* Free newly built spine (excluding new_node which is kept for retry) */
                csilk_mvcc_node_t* cleanup =
                    atomic_load_explicit(&new_node->next, memory_order_relaxed);
                while (cleanup) {
                    csilk_mvcc_node_t* nxt =
                        atomic_load_explicit(&cleanup->next, memory_order_relaxed);
                    free(cleanup->key);
                    free(cleanup->val);
                    free(cleanup);
                    cleanup = nxt;
                }
                atomic_store_explicit(&new_node->next, NULL, memory_order_relaxed);
                free(new_node->key);
                free(new_node->val);
                free(new_node);
                reader_exit(slot);
                return -1;
            }

            atomic_store_explicit(&prev_alloc->next, NULL, memory_order_relaxed);

            if (atomic_compare_exchange_weak_explicit(&cache->buckets[idx],
                                                      &old_head,
                                                      new_chain_head,
                                                      memory_order_release,
                                                      memory_order_acquire)) {
                reader_exit(slot);
                /* Retire all replaced old nodes from old_head */
                csilk_mvcc_node_t* to_retire = old_head;
                while (to_retire) {
                    csilk_mvcc_node_t* nxt =
                        atomic_load_explicit(&to_retire->next, memory_order_relaxed);
                    retire_node(cache, to_retire, epoch);
                    to_retire = nxt;
                }
                return 0;
            }

            reader_exit(slot);

            /* CAS failed: free cloned spine (excluding new_node) and retry */
            csilk_mvcc_node_t* cleanup =
                atomic_load_explicit(&new_node->next, memory_order_relaxed);
            while (cleanup) {
                csilk_mvcc_node_t* nxt = atomic_load_explicit(&cleanup->next, memory_order_relaxed);
                free(cleanup->key);
                free(cleanup->val);
                free(cleanup);
                cleanup = nxt;
            }
            atomic_store_explicit(&new_node->next, NULL, memory_order_relaxed);
        }
    }
}

/**
 * @brief Acquire an epoch-protected read view for a key.
 */
int
csilk_mvcc_cache_get_view(csilk_mvcc_cache_t* cache, const char* key, csilk_mvcc_view_t* view)
{
    if (!cache || !key || !view) {
        return -1;
    }

    csilk_mvcc_reader_slot_t* slot = acquire_reader_slot(cache);
    reader_enter(cache, slot);

    uint64_t           idx = hash_key(key) % cache->capacity;
    csilk_mvcc_node_t* curr = atomic_load_explicit(&cache->buckets[idx], memory_order_acquire);

    while (curr) {
        if (strcmp(curr->key, key) == 0) {
            view->data = curr->val;
            view->len = curr->val_len;
            view->version = curr->version;
            view->_slot = slot;
            return 0;
        }
        curr = atomic_load_explicit(&curr->next, memory_order_acquire);
    }

    reader_exit(slot);
    memset(view, 0, sizeof(*view));
    return -1;
}

/**
 * @brief Release an epoch-protected read view.
 */
void
csilk_mvcc_cache_release_view(csilk_mvcc_cache_t* cache, csilk_mvcc_view_t* view)
{
    if (!cache || !view || !view->_slot) {
        return;
    }
    csilk_mvcc_reader_slot_t* slot = (csilk_mvcc_reader_slot_t*)view->_slot;
    reader_exit(slot);
    view->_slot = NULL;
    view->data = NULL;
    view->len = 0;
}

/**
 * @brief Safely copy out a value under epoch protection.
 */
int
csilk_mvcc_cache_get_copy(
    csilk_mvcc_cache_t* cache, const char* key, void* out_buf, size_t out_buf_len, size_t* val_len)
{
    csilk_mvcc_view_t view;
    if (csilk_mvcc_cache_get_view(cache, key, &view) != 0) {
        return -1;
    }

    if (val_len) {
        *val_len = view.len;
    }
    if (out_buf && out_buf_len > 0) {
        size_t copy_len = view.len < out_buf_len ? view.len : out_buf_len;
        memcpy(out_buf, view.data, copy_len);
    }

    csilk_mvcc_cache_release_view(cache, &view);
    return 0;
}

/**
 * @brief Delete/remove a key from the cache.
 */
int
csilk_mvcc_cache_delete(csilk_mvcc_cache_t* cache, const char* key)
{
    if (!cache || !key) {
        return -1;
    }

    uint64_t idx = hash_key(key) % cache->capacity;
    uint64_t epoch = atomic_load_explicit(&cache->global_epoch, memory_order_acquire);
    csilk_mvcc_reader_slot_t* slot = acquire_reader_slot(cache);

    while (1) {
        reader_enter(cache, slot);
        csilk_mvcc_node_t* old_head =
            atomic_load_explicit(&cache->buckets[idx], memory_order_acquire);
        if (!old_head) {
            reader_exit(slot);
            return -1; /* Not found */
        }

        if (strcmp(old_head->key, key) == 0) {
            csilk_mvcc_node_t* next = atomic_load_explicit(&old_head->next, memory_order_relaxed);
            if (atomic_compare_exchange_weak_explicit(&cache->buckets[idx],
                                                      &old_head,
                                                      next,
                                                      memory_order_release,
                                                      memory_order_acquire)) {
                reader_exit(slot);
                retire_node(cache, old_head, epoch);
                return 0;
            }
            reader_exit(slot);
            continue;
        }

        /* Check if key is deeper in the chain */
        csilk_mvcc_node_t* found_node = NULL;
        csilk_mvcc_node_t* curr = atomic_load_explicit(&old_head->next, memory_order_acquire);
        while (curr) {
            if (strcmp(curr->key, key) == 0) {
                found_node = curr;
                break;
            }
            curr = atomic_load_explicit(&curr->next, memory_order_acquire);
        }

        if (!found_node) {
            reader_exit(slot);
            return -1; /* Not found */
        }

        /* Rebuild chain without found_node */
        csilk_mvcc_node_t* new_chain_head = NULL;
        csilk_mvcc_node_t* prev_alloc = NULL;
        csilk_mvcc_node_t* iter = old_head;
        bool               alloc_failed = false;

        while (iter) {
            if (iter != found_node) {
                csilk_mvcc_node_t* clone =
                    create_node(iter->key, iter->val, iter->val_len, iter->version);
                if (!clone) {
                    alloc_failed = true;
                    break;
                }
                if (!new_chain_head) {
                    new_chain_head = clone;
                } else {
                    atomic_store_explicit(&prev_alloc->next, clone, memory_order_relaxed);
                }
                prev_alloc = clone;
            }
            iter = atomic_load_explicit(&iter->next, memory_order_acquire);
        }

        if (alloc_failed) {
            csilk_mvcc_node_t* cleanup = new_chain_head;
            while (cleanup) {
                csilk_mvcc_node_t* nxt = atomic_load_explicit(&cleanup->next, memory_order_relaxed);
                free(cleanup->key);
                free(cleanup->val);
                free(cleanup);
                cleanup = nxt;
            }
            reader_exit(slot);
            return -1;
        }

        if (prev_alloc) {
            atomic_store_explicit(&prev_alloc->next, NULL, memory_order_relaxed);
        }

        if (atomic_compare_exchange_weak_explicit(&cache->buckets[idx],
                                                  &old_head,
                                                  new_chain_head,
                                                  memory_order_release,
                                                  memory_order_acquire)) {
            reader_exit(slot);
            csilk_mvcc_node_t* to_retire = old_head;
            while (to_retire) {
                csilk_mvcc_node_t* nxt =
                    atomic_load_explicit(&to_retire->next, memory_order_relaxed);
                retire_node(cache, to_retire, epoch);
                to_retire = nxt;
            }
            return 0;
        }

        reader_exit(slot);

        /* CAS failed, cleanup and retry */
        csilk_mvcc_node_t* cleanup = new_chain_head;
        while (cleanup) {
            csilk_mvcc_node_t* nxt = atomic_load_explicit(&cleanup->next, memory_order_relaxed);
            free(cleanup->key);
            free(cleanup->val);
            free(cleanup);
            cleanup = nxt;
        }
    }
}

/**
 * @brief Retrieve a value atomically without acquiring any locks.
 */
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
