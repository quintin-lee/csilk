/**
 * @file header_map.c
 * @brief High-performance HTTP header hash-map implementation.
 *
 * Implements single-pass case-folding hash generation, stores hash & key length
 * in each node, and accelerates lookups using:
 *   1. Hash equality check (uint32_t)
 *   2. Key length equality check (size_t)
 *   3. Hardware-accelerated memory comparison (memcmp)
 *
 * All allocations come from the request arena for zero-fragmentation cleanup.
 *
 * Thread safety: Per-request context data, called only from the owning worker thread.
 *
 * @copyright MIT License
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "header_map.h"

/**
 * @brief Single-pass case-folding hash for NUL-terminated keys.
 *
 * Computes the 32-bit djb2 hash with case-insensitive ASCII folding.
 *
 * @param[in]  key       NUL-terminated input key.
 * @param[out] out_len   Receives the exact string length of @p key.
 * @return 32-bit hash value.
 */
static inline uint32_t
header_hash(const char* key, size_t* out_len)
{
    uint32_t hash = 5381;
    size_t   len = 0;

    while (key[len]) {
        unsigned char c = (unsigned char)key[len];
        if (c >= 'A' && c <= 'Z') {
            c |= 0x20;
        }
        hash = ((hash << 5) + hash) + c;
        len++;
    }

    if (out_len) {
        *out_len = len;
    }
    return hash;
}

/**
 * @brief Single-pass case-folding hash for string view slices.
 *
 * @param[in]  data      Input key data slice (may not be NUL-terminated).
 * @param[in]  len       Length of the slice in bytes.
 * @return 32-bit hash value.
 */
static inline uint32_t
header_hash_view(const char* data, size_t len)
{
    uint32_t hash = 5381;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)data[i];
        if (c >= 'A' && c <= 'Z') {
            c |= 0x20;
        }
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/**
 * @brief Hash a header key string into a bucket index using case-folded djb2.
 *
 * @param key Header key string (null-terminated).
 * @return Bucket index in the range [0, CSILK_HEADER_BUCKETS - 1].
 */
uint32_t
hash_key(const char* key)
{
    if (!key) {
        return 0;
    }
    size_t   len = 0;
    uint32_t hash = header_hash(key, &len);
    return hash & (CSILK_HEADER_BUCKETS - 1);
}

/**
 * @brief Look up a header value by key in the hash map (case-insensitive).
 *
 * Performs ultra-fast multi-stage filtering:
 *   1. 32-bit hash equality (uint32_t comparison)
 *   2. Key length equality (size_t comparison)
 *   3. memcmp fast-path / strncasecmp fallback
 *
 * @param map Header hash map (must not be NULL).
 * @param key Header key to find (case-insensitive).
 * @return Pointer to the value string, or NULL if not found.
 */
const char*
map_get(csilk_header_map_t* map, const char* key)
{
    if (!map || !key) {
        return NULL;
    }

    size_t          key_len = 0;
    uint32_t        hash = header_hash(key, &key_len);
    uint32_t        bucket = hash & (CSILK_HEADER_BUCKETS - 1);
    csilk_header_t* h = map->buckets[bucket];

    while (h) {
        if (h->hash == hash && h->key_len == key_len) {
            if (memcmp(h->key, key, key_len) == 0 || strncasecmp(h->key, key, key_len) == 0) {
                return h->value;
            }
        }
        h = h->next;
    }
    return NULL;
}

/**
 * @brief Look up a header value returning a zero-copy slice view (case-insensitive).
 *
 * @param map Header hash map (may be NULL).
 * @param key Header key to find (case-insensitive).
 * @return A csilk_view_t slice of the header value.
 */
csilk_view_t
map_get_view(csilk_header_map_t* map, const char* key)
{
    if (!map || !key) {
        return csilk_view(NULL, 0);
    }

    size_t          key_len = 0;
    uint32_t        hash = header_hash(key, &key_len);
    uint32_t        bucket = hash & (CSILK_HEADER_BUCKETS - 1);
    csilk_header_t* h = map->buckets[bucket];

    while (h) {
        if (h->hash == hash && h->key_len == key_len) {
            if (memcmp(h->key, key, key_len) == 0 || strncasecmp(h->key, key, key_len) == 0) {
                return csilk_view(h->value,
                                  h->value_len ? h->value_len : (h->value ? strlen(h->value) : 0));
            }
        }
        h = h->next;
    }
    return csilk_view(NULL, 0);
}

/**
 * @brief Set a header value from zero-copy string views, replacing existing entry.
 *
 * @param c     Request context for arena allocation.
 * @param map   Header hash map.
 * @param key   Header key string view.
 * @param value Header value string view.
 */
void
map_set_view(csilk_ctx_t*            c,
             csilk_header_map_t*     map,
             const csilk_str_view_t* key,
             const csilk_str_view_t* value)
{
    if (!c || !c->arena || !map || !key || !key->data || !value || !value->data) {
        return;
    }
    map->used = 1;

    size_t          key_len = key->len;
    uint32_t        hash = header_hash_view(key->data, key_len);
    uint32_t        bucket = hash & (CSILK_HEADER_BUCKETS - 1);
    csilk_header_t* h = map->buckets[bucket];

    while (h) {
        if (h->hash == hash && h->key_len == key_len) {
            if (memcmp(h->key, key->data, key_len) == 0 ||
                strncasecmp(h->key, key->data, key_len) == 0) {
                h->value = csilk_arena_strndup(c->arena, value->data, value->len);
                h->value_len = value->len;
                return;
            }
        }
        h = h->next;
    }

    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (!new_h) {
        return;
    }

    new_h->key = csilk_arena_strndup(c->arena, key->data, key_len);
    new_h->key_len = key_len;
    new_h->value = csilk_arena_strndup(c->arena, value->data, value->len);
    new_h->value_len = value->len;
    new_h->hash = hash;
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;
}

/**
 * @brief Set a header value, overwriting any existing entry with the same key.
 *
 * @param c     Request context for arena allocation.
 * @param map   Header hash map.
 * @param key   Header key string (null-terminated).
 * @param value Header value string (null-terminated).
 */
void
map_set(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
    if (!c || !c->arena || !map || !key || !value) {
        return;
    }
    map->used = 1;

    size_t          key_len = 0;
    uint32_t        hash = header_hash(key, &key_len);
    uint32_t        bucket = hash & (CSILK_HEADER_BUCKETS - 1);
    csilk_header_t* h = map->buckets[bucket];

    while (h) {
        if (h->hash == hash && h->key_len == key_len) {
            if (memcmp(h->key, key, key_len) == 0 || strncasecmp(h->key, key, key_len) == 0) {
                h->value = csilk_arena_strdup(c->arena, value);
                h->value_len = h->value ? strlen(h->value) : 0;
                return;
            }
        }
        h = h->next;
    }

    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (!new_h) {
        return;
    }

    new_h->key = csilk_arena_strdup(c->arena, key);
    new_h->key_len = key_len;
    new_h->value = csilk_arena_strdup(c->arena, value);
    new_h->value_len = new_h->value ? strlen(new_h->value) : 0;
    new_h->hash = hash;
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;
}

/**
 * @brief Add a header value to the hash map, allowing duplicate keys.
 *
 * Always creates a new header node without overwriting existing entries.
 *
 * @param c     Request context for arena allocation.
 * @param map   Header hash map.
 * @param key   Header key string.
 * @param value Header value string.
 */
void
map_add(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
    if (!c || !c->arena || !map || !key || !value) {
        return;
    }
    map->used = 1;

    size_t          key_len = 0;
    uint32_t        hash = header_hash(key, &key_len);
    uint32_t        bucket = hash & (CSILK_HEADER_BUCKETS - 1);
    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (!new_h) {
        return;
    }

    new_h->key = csilk_arena_strdup(c->arena, key);
    new_h->key_len = key_len;
    new_h->value = csilk_arena_strdup(c->arena, value);
    new_h->value_len = new_h->value ? strlen(new_h->value) : 0;
    new_h->hash = hash;
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;
}
