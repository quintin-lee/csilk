/**
 * @file header_map.c
 * @brief Internal header hash-map implementation (djb2, case-insensitive).
 *
 * Provides the chained hash map used by both request and response headers.
 * All allocations come from the request arena for zero-fragmentation cleanup.
 *
 * Thread safety: these functions access per-request context data and are
 * intended to be called only from the event-loop thread that owns
 * the connection.
 *
 * @copyright MIT License
 */

#include <ctype.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Hash a header key string into a bucket index using djb2.
 *
 * Applies the djb2 hash algorithm with case-insensitive character folding
 * (via tolower()) so that "Content-Type" and "content-type" map to the same
 * bucket. This ensures consistent lookups regardless of header casing.
 *
 * @param key Header key string (null-terminated).
 * @return Bucket index in the range [0, CSILK_HEADER_BUCKETS - 1].
 * @note The caller must ensure @p key is non-nullptr. */
uint32_t
hash_key(const char* key)
{
    uint32_t hash = 5381;
    int      c;
    while ((c = (unsigned char)*key++)) {
        hash = ((hash << 5) + hash) + tolower(c);
    }
    return hash % CSILK_HEADER_BUCKETS;
}

/** @brief Look up a header value by key in the hash map (case-insensitive).
 *
 * Iterates the linked list at the hashed bucket and compares keys using
 * strcasecmp. Returns the first matching value.
 *
 * @param map Header hash map (must not be nullptr).
 * @param key Header key to find (case-insensitive).
 * @return Pointer to the value string, or nullptr if the key is not found.
 * @note The returned string shares the lifetime of the map's arena. */
const char*
map_get(csilk_header_map_t* map, const char* key)
{
    uint32_t        bucket = hash_key(key);
    csilk_header_t* h = map->buckets[bucket];
    while (h) {
        if (strcasecmp(h->key, key) == 0) {
            return h->value;
        }
        h = h->next;
    }
    return nullptr;
}
/** @brief Hash a key from a string view into a bucket index.
 *
 * Applies the djb2 hash algorithm with case-insensitive character folding.
 *
 * @param key Header key string view.
 * @return Bucket index in the range [0, CSILK_HEADER_BUCKETS - 1].
 * @note The caller must ensure @p key is non-nullptr and has valid data. */
static uint32_t
hash_key_view(const csilk_str_view_t* key)
{
    uint32_t hash = 5381;
    for (size_t i = 0; i < key->len; i++) {
        hash = ((hash << 5) + hash) + tolower((unsigned char)key->data[i]);
    }
    return hash % CSILK_HEADER_BUCKETS;
}

/** @brief Set a header value in the hash map from zero-copy string views.
 *
 * Copies the key and value from string views into arena memory, then
 * inserts into the hash map. This is the zero-copy path: the original
 * data remains in the receive buffer and is not touched.
 *
 * @param c     Request context used for arena-based memory allocation.
 * @param map   Header hash map (request or response headers).
 * @param key   Header key (zero-copy reference to recv buffer).
 * @param value Header value (zero-copy reference to recv buffer).
 * @note The key and value are copied into arena memory. If the arena is
 *       nullptr this function silently does nothing. */
void
map_set_view(csilk_ctx_t*            c,
             csilk_header_map_t*     map,
             const csilk_str_view_t* key,
             const csilk_str_view_t* value)
{
    if (!c->arena || !key || !key->data || !value || !value->data) {
        return;
    }
    uint32_t        bucket = hash_key_view(key);
    csilk_header_t* h = map->buckets[bucket];
    while (h) {
        if (strncasecmp(h->key, key->data, key->len) == 0 && h->key_len == key->len) {
            h->value = csilk_arena_strndup(c->arena, value->data, value->len);
            h->value_len = value->len;
            return;
        }
        h = h->next;
    }

    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (new_h) {
        new_h->key = csilk_arena_strndup(c->arena, key->data, key->len);
        new_h->key_len = key->len;
        new_h->value = csilk_arena_strndup(c->arena, value->data, value->len);
        new_h->value_len = value->len;
        new_h->next = map->buckets[bucket];
        map->buckets[bucket] = new_h;
    }
}

/** @brief Set a header value in the hash map, overwriting any existing entry.
 *
 * Hashes the key, searches the bucket for an existing entry, and replaces
 * its value. If no entry is found, a new header node is allocated via the
 * context's arena allocator and prepended to the bucket's linked list.
 *
 * @param c     Request context used for arena-based memory allocation.
 * @param map   Header hash map (request or response headers).
 * @param key   Header key (case-insensitive via strcasecmp on lookup).
 * @param value Header value string.
 * @note The key and value are duplicated into arena memory. If the arena is
 *       nullptr this function silently does nothing.
 */
void
map_set(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
    if (!c->arena || !key || !value) {
        return;
    }
    uint32_t        bucket = hash_key(key);
    csilk_header_t* h = map->buckets[bucket];
    while (h) {
        if (strcasecmp(h->key, key) == 0) {
            h->value = csilk_arena_strdup(c->arena, value);
            h->value_len = h->value ? strlen(h->value) : 0;
            return;
        }
        h = h->next;
    }

    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (new_h) {
        new_h->key = csilk_arena_strdup(c->arena, key);
        new_h->key_len = new_h->key ? strlen(new_h->key) : 0;
        new_h->value = csilk_arena_strdup(c->arena, value);
        new_h->value_len = new_h->value ? strlen(new_h->value) : 0;
        new_h->next = map->buckets[bucket];
        map->buckets[bucket] = new_h;
    }
}

/** @brief Add a header value to the hash map, allowing duplicate keys.
 *
 * Unlike map_set(), this function does NOT search for an existing entry with
 * the same key. It always prepends a new header node, so multiple calls with
 * the same key produce multiple entries (e.g., multiple Set-Cookie headers).
 *
 * @param c     Request context for arena-based memory allocation.
 * @param map   Header hash map.
 * @param key   Header key.
 * @param value Header value.
 * @note Both key and value are duplicated into arena memory. Silently does
 *       nothing if the arena is nullptr. */
void
map_add(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
    if (!c->arena) {
        return;
    }
    uint32_t        bucket = hash_key(key);
    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (new_h) {
        new_h->key = csilk_arena_strdup(c->arena, key);
        new_h->key_len = strlen(new_h->key);
        new_h->value = csilk_arena_strdup(c->arena, value);
        new_h->value_len = strlen(new_h->value);
        new_h->next = map->buckets[bucket];
        map->buckets[bucket] = new_h;
    }
}
