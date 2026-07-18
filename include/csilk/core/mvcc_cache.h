#pragma once
/**
 * @file mvcc_cache.h
 * @brief Epoch-based RCU / MVCC Lock-Free In-Memory Cache.
 *
 * @version 0.4.0
 * @copyright MIT License
 */

#include <stddef.h>
#include <stdint.h>
#include "csilk/core/types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct csilk_mvcc_cache_s csilk_mvcc_cache_t;

/**
 * @brief Create a new Epoch-based MVCC lock-free cache instance.
 * @param capacity Maximum number of entries (hash table size).
 * @return New cache instance, or nullptr on allocation failure.
 */
csilk_mvcc_cache_t* csilk_mvcc_cache_new(size_t capacity);

/**
 * @brief Destroy an MVCC cache instance and free all versions.
 * @param cache Cache instance to destroy.
 */
void csilk_mvcc_cache_free(csilk_mvcc_cache_t* cache);

/**
 * @brief Store a key-value entry using MVCC RCU pointer swap.
 * @param cache Cache instance.
 * @param key Null-terminated key string.
 * @param val Value byte buffer.
 * @param val_len Value length in bytes.
 * @return 0 on success, -1 on failure.
 */
int
csilk_mvcc_cache_set(csilk_mvcc_cache_t* cache, const char* key, const void* val, size_t val_len);

/**
 * @brief Retrieve a value atomically without acquiring any locks.
 * @param cache Cache instance.
 * @param key Null-terminated key string.
 * @param[out] val_len Receives value length in bytes (can be nullptr).
 * @return Read-only pointer to the value bytes, or nullptr if not found.
 */
const void* csilk_mvcc_cache_get(csilk_mvcc_cache_t* cache, const char* key, size_t* val_len);

#ifdef __cplusplus
}
#endif
