#pragma once
/**
 * @file mvcc_cache.h
 * @brief Epoch-based RCU / MVCC Lock-Free In-Memory Cache with Safe Epoch Reclamation.
 *
 * Provides a high-performance, concurrent, lock-free key-value cache
 * utilizing Epoch-Based Reclamation (EBR) for safe memory management.
 *
 * @version 0.5.2
 * @copyright MIT License
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "csilk/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_mvcc_cache_s csilk_mvcc_cache_t;

/**
 * @brief Epoch-protected read view.
 *
 * Guarantees that the underlying value memory remains valid and will not
 * be reclaimed for the entire duration between get_view and release_view.
 */
typedef struct csilk_mvcc_view_s {
    const void* data;    /**< Read-only pointer to the value bytes. */
    size_t      len;     /**< Value length in bytes. */
    uint64_t    version; /**< MVCC version / creation epoch. */
    void*       _slot;   /**< Internal epoch slot tracking (opaque). */
} csilk_mvcc_view_t;

/**
 * @brief Create a new Epoch-based MVCC lock-free cache instance.
 * @param capacity Maximum number of hash buckets (defaults to 1024 if 0).
 * @return New cache instance, or NULL on allocation failure.
 */
csilk_mvcc_cache_t* csilk_mvcc_cache_new(size_t capacity);

/**
 * @brief Destroy an MVCC cache instance and free all versions.
 *
 * Waits for all active reader grace periods to elapse before freeing memory.
 * @param cache Cache instance to destroy.
 */
void csilk_mvcc_cache_free(csilk_mvcc_cache_t* cache);

/**
 * @brief Store a key-value entry using MVCC RCU pointer swap.
 *
 * Creates a new version of the key-value pair and publishes it atomically.
 * Any previous version is unlinked and queued for epoch-based reclamation.
 *
 * @param cache   Cache instance.
 * @param key     Null-terminated key string.
 * @param val     Value byte buffer.
 * @param val_len Value length in bytes.
 * @return 0 on success, -1 on failure.
 */
int
csilk_mvcc_cache_set(csilk_mvcc_cache_t* cache, const char* key, const void* val, size_t val_len);

/**
 * @brief Acquire an epoch-protected read view for a key.
 *
 * Enters an active reader epoch and locates the key's most recent version.
 * Callers MUST call csilk_mvcc_cache_release_view() when finished.
 *
 * @param cache    Cache instance.
 * @param key      Null-terminated key string.
 * @param[out] view Populated with pointer, length, and version.
 * @return 0 on success (found), -1 if not found or NULL args.
 */
int csilk_mvcc_cache_get_view(csilk_mvcc_cache_t* cache, const char* key, csilk_mvcc_view_t* view);

/**
 * @brief Release an epoch-protected read view.
 *
 * Exits the reader epoch, allowing reclaimed nodes to be safely freed.
 * The pointer in @p view must NOT be accessed after this call.
 *
 * @param cache Cache instance.
 * @param view  View obtained from csilk_mvcc_cache_get_view.
 */
void csilk_mvcc_cache_release_view(csilk_mvcc_cache_t* cache, csilk_mvcc_view_t* view);

/**
 * @brief Safely copy out a value under epoch protection.
 *
 * Enters epoch, copies up to @p out_buf_len bytes into @p out_buf, and exits epoch.
 *
 * @param cache       Cache instance.
 * @param key         Null-terminated key string.
 * @param[out] out_buf Destination buffer (can be NULL if only querying length).
 * @param out_buf_len Capacity of destination buffer.
 * @param[out] val_len Receives total value length (can be NULL).
 * @return 0 on success (found), -1 if not found or NULL args.
 */
int csilk_mvcc_cache_get_copy(
    csilk_mvcc_cache_t* cache, const char* key, void* out_buf, size_t out_buf_len, size_t* val_len);

/**
 * @brief Delete/remove a key from the cache.
 *
 * Unlinks the node matching @p key and queues it for epoch reclamation.
 *
 * @param cache Cache instance.
 * @param key   Null-terminated key string.
 * @return 0 on success (deleted), -1 if not found or error.
 */
int csilk_mvcc_cache_delete(csilk_mvcc_cache_t* cache, const char* key);

/**
 * @brief Retrieve a value atomically without acquiring any locks.
 * @deprecated Use csilk_mvcc_cache_get_view() or csilk_mvcc_cache_get_copy() for safe lifecycle management.
 * @param cache Cache instance.
 * @param key Null-terminated key string.
 * @param[out] val_len Receives value length in bytes (can be NULL).
 * @return Read-only pointer to the value bytes, or NULL if not found.
 */
const void* csilk_mvcc_cache_get(csilk_mvcc_cache_t* cache, const char* key, size_t* val_len);

#ifdef __cplusplus
}
#endif
