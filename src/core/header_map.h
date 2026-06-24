/**
 * @file header_map.h
 * @brief Internal header hash-map implementation for csilk.
 *
 * Provides the djb2-based, case-insensitive hash map used for both
 * request and response headers.  These functions are declared
 * CSILK_INTERNAL and shared between context.c and response.c.
 *
 * @copyright MIT License
 */

#ifndef CSILK_HEADER_MAP_H
#define CSILK_HEADER_MAP_H

#include "csilk/csilk.h"

/**
 * @brief Hash a header key string into a bucket index using djb2.
 *
 * Applies the djb2 hash algorithm with case-insensitive character folding
 * (via tolower()) so that "Content-Type" and "content-type" map to the same
 * bucket. This ensures consistent lookups regardless of header casing.
 *
 * @param key Header key string (null-terminated).
 * @return Bucket index in the range [0, CSILK_HEADER_BUCKETS - 1].
 * @note The caller must ensure @p key is non-nullptr.
 */
CSILK_INTERNAL uint32_t hash_key(const char* key);

/**
 * @brief Look up a header value by key in the hash map (case-insensitive).
 *
 * Iterates the linked list at the hashed bucket and compares keys using
 * strcasecmp. Returns the first matching value.
 *
 * @param map Header hash map (must not be nullptr).
 * @param key Header key to find (case-insensitive).
 * @return Pointer to the value string, or nullptr if the key is not found.
 * @note The returned string shares the lifetime of the map's arena.
 */
CSILK_INTERNAL const char* map_get(csilk_header_map_t* map, const char* key);

/**
 * @brief Set a header value in the hash map, overwriting any existing entry.
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
CSILK_INTERNAL void map_set_view(csilk_ctx_t* c,
				 csilk_header_map_t* map,
				 const csilk_str_view_t* key,
				 const csilk_str_view_t* value);

CSILK_INTERNAL void
map_set(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value);

/**
 * @brief Add a header value to the hash map, allowing duplicate keys.
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
 *       nothing if the arena is nullptr.
 */
CSILK_INTERNAL void
map_add(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value);

#endif /* CSILK_HEADER_MAP_H */
