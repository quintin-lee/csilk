/**
 * @file header_map.c
 * @brief High-performance HTTP header hash-map implementation with Name Interning.
 *
 * Implements:
 *   1. Static header tokenization / name interning for common HTTP headers into integer IDs
 *   2. O(1) direct slot array indexing for known headers (map->known[id])
 *   3. Single-pass case-folding hash generation for custom / unknown headers
 *   4. Fast 3-level fallback filtering (Hash -> Key Length -> Memcmp / Strncasecmp)
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
#include <strings.h>

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "header_map.h"

/* ---------------------------------------------------------------------------
 * Header Name Interning (Tokenization)
 * -------------------------------------------------------------------------*/

static const char* const k_header_canonical_names[] = {
    [CSILK_HDR_UNKNOWN] = "Unknown",
    [CSILK_HDR_HOST] = "Host",
    [CSILK_HDR_CONTENT_TYPE] = "Content-Type",
    [CSILK_HDR_CONTENT_LENGTH] = "Content-Length",
    [CSILK_HDR_AUTHORIZATION] = "Authorization",
    [CSILK_HDR_COOKIE] = "Cookie",
    [CSILK_HDR_SET_COOKIE] = "Set-Cookie",
    [CSILK_HDR_ACCEPT] = "Accept",
    [CSILK_HDR_ACCEPT_ENCODING] = "Accept-Encoding",
    [CSILK_HDR_ACCEPT_LANGUAGE] = "Accept-Language",
    [CSILK_HDR_USER_AGENT] = "User-Agent",
    [CSILK_HDR_CONNECTION] = "Connection",
    [CSILK_HDR_UPGRADE] = "Upgrade",
    [CSILK_HDR_CACHE_CONTROL] = "Cache-Control",
    [CSILK_HDR_ORIGIN] = "Origin",
    [CSILK_HDR_REFERER] = "Referer",
    [CSILK_HDR_SEC_WEBSOCKET_KEY] = "Sec-WebSocket-Key",
    [CSILK_HDR_SEC_WEBSOCKET_VERSION] = "Sec-WebSocket-Version",
    [CSILK_HDR_SEC_WEBSOCKET_EXTENSIONS] = "Sec-WebSocket-Extensions",
    [CSILK_HDR_SEC_WEBSOCKET_PROTOCOL] = "Sec-WebSocket-Protocol",
    [CSILK_HDR_TRANSFER_ENCODING] = "Transfer-Encoding",
    [CSILK_HDR_LOCATION] = "Location",
    [CSILK_HDR_IF_MODIFIED_SINCE] = "If-Modified-Since",
    [CSILK_HDR_IF_NONE_MATCH] = "If-None-Match",
    [CSILK_HDR_ETAG] = "ETag",
    [CSILK_HDR_SERVER] = "Server",
    [CSILK_HDR_DATE] = "Date",
    [CSILK_HDR_VARY] = "Vary",
    [CSILK_HDR_X_REQUEST_ID] = "X-Request-ID",
    [CSILK_HDR_X_FORWARDED_FOR] = "X-Forwarded-For",
    [CSILK_HDR_X_REAL_IP] = "X-Real-IP",
    [CSILK_HDR_CONTENT_ENCODING] = "Content-Encoding",
};

/**
 * @brief Get the canonical name string for an interned header ID.
 */
const char*
csilk_header_id_name(csilk_header_id_t id)
{
    if ((size_t)id < sizeof(k_header_canonical_names) / sizeof(k_header_canonical_names[0])) {
        return k_header_canonical_names[id];
    }
    return "Unknown";
}

/**
 * @brief Convert a header name string into an interned integer ID (case-insensitive).
 *
 * Uses branchless length-first jump tables and character matching to classify
 * headers in 2 to 5 CPU instructions without memory allocation or string hashing.
 *
 * @param name Header name string.
 * @param len  Length of @p name in bytes.
 * @return Matching csilk_header_id_t or CSILK_HDR_UNKNOWN.
 */
csilk_header_id_t
csilk_header_id_from_name(const char* name, size_t len)
{
    if (!name || len == 0) {
        return CSILK_HDR_UNKNOWN;
    }

    switch (len) {
    case 4: {
        uint32_t v;
        memcpy(&v, name, 4);
        v |= 0x20202020U;
        if (v == 0x74736f68U) {
            return CSILK_HDR_HOST;
        }
        if (v == 0x67617465U) {
            return CSILK_HDR_ETAG;
        }
        if (v == 0x65746164U) {
            return CSILK_HDR_DATE;
        }
        if (v == 0x79726176U) {
            return CSILK_HDR_VARY;
        }
        break;
    }
    case 6: {
        uint32_t v4;
        uint16_t v2;
        memcpy(&v4, name, 4);
        memcpy(&v2, name + 4, 2);
        v4 |= 0x20202020U;
        v2 |= 0x2020U;
        if (v4 == 0x65636361U && v2 == 0x7470U) {
            return CSILK_HDR_ACCEPT;
        }
        if (v4 == 0x6b6f6f63U && v2 == 0x6569U) {
            return CSILK_HDR_COOKIE;
        }
        if (v4 == 0x6769726fU && v2 == 0x6e69U) {
            return CSILK_HDR_ORIGIN;
        }
        if (v4 == 0x76726573U && v2 == 0x7265U) {
            return CSILK_HDR_SERVER;
        }
        break;
    }
    case 7: {
        char c0 = (char)(name[0] | 0x20);
        if (c0 == 'u' && strncasecmp(name + 1, "pgrade", 6) == 0) {
            return CSILK_HDR_UPGRADE;
        }
        if (c0 == 'r' && strncasecmp(name + 1, "eferer", 6) == 0) {
            return CSILK_HDR_REFERER;
        }
        break;
    }
    case 8: {
        uint64_t v8;
        memcpy(&v8, name, 8);
        v8 |= 0x2020202020202020ULL;
        if (v8 == 0x6e6f697461636f6cULL) {
            return CSILK_HDR_LOCATION;
        }
        break;
    }
    case 9: {
        if (strncasecmp(name, "x-real-ip", 9) == 0) {
            return CSILK_HDR_X_REAL_IP;
        }
        break;
    }
    case 10: {
        uint64_t v8;
        uint16_t v2;
        memcpy(&v8, name, 8);
        memcpy(&v2, name + 8, 2);
        v8 |= 0x2020202020202020ULL;
        v2 |= 0x2020U;
        if (v8 == 0x697463656e6e6f63ULL && v2 == 0x6e6fU) {
            return CSILK_HDR_CONNECTION;
        }
        if (v8 == 0x6567612d72657375ULL && v2 == 0x746eU) {
            return CSILK_HDR_USER_AGENT;
        }
        if (v8 == 0x6b6f6f632d746573ULL && v2 == 0x6569U) {
            return CSILK_HDR_SET_COOKIE;
        }
        break;
    }
    case 12: {
        uint64_t v8;
        uint32_t v4;
        memcpy(&v8, name, 8);
        memcpy(&v4, name + 8, 4);
        v8 |= 0x2020202020202020ULL;
        v4 |= 0x20202020U;
        if (v8 == 0x2d746e65746e6f63ULL && v4 == 0x65707974U) {
            return CSILK_HDR_CONTENT_TYPE;
        }
        if (v8 == 0x7365757165722d78ULL && v4 == 0x64692d74U) {
            return CSILK_HDR_X_REQUEST_ID;
        }
        break;
    }
    case 13: {
        char c0 = (char)(name[0] | 0x20);
        if (c0 == 'a' && strncasecmp(name + 1, "uthorization", 12) == 0) {
            return CSILK_HDR_AUTHORIZATION;
        }
        if (c0 == 'c' && strncasecmp(name + 1, "ache-control", 12) == 0) {
            return CSILK_HDR_CACHE_CONTROL;
        }
        if (c0 == 'i' && strncasecmp(name + 1, "f-none-match", 12) == 0) {
            return CSILK_HDR_IF_NONE_MATCH;
        }
        break;
    }
    case 14: {
        uint64_t v8;
        uint32_t v4;
        uint16_t v2;
        memcpy(&v8, name, 8);
        memcpy(&v4, name + 8, 4);
        memcpy(&v2, name + 12, 2);
        v8 |= 0x2020202020202020ULL;
        v4 |= 0x20202020U;
        v2 |= 0x2020U;
        if (v8 == 0x2d746e65746e6f63ULL && v4 == 0x676e656cU && v2 == 0x6874U) {
            return CSILK_HDR_CONTENT_LENGTH;
        }
        break;
    }
    case 15: {
        char c1 = (char)(name[1] | 0x20);
        if (c1 == 'c') {
            char c7 = (char)(name[7] | 0x20);
            if (c7 == 'e' && strncasecmp(name, "accept-encoding", 15) == 0) {
                return CSILK_HDR_ACCEPT_ENCODING;
            }
            if (c7 == 'l' && strncasecmp(name, "accept-language", 15) == 0) {
                return CSILK_HDR_ACCEPT_LANGUAGE;
            }
        } else if (c1 == '-') {
            if (strncasecmp(name, "x-forwarded-for", 15) == 0) {
                return CSILK_HDR_X_FORWARDED_FOR;
            }
        }
        break;
    }
    case 16: {
        if (strncasecmp(name, "content-encoding", 16) == 0) {
            return CSILK_HDR_CONTENT_ENCODING;
        }
        break;
    }
    case 17: {
        char c0 = (char)(name[0] | 0x20);
        if (c0 == 'i' && strncasecmp(name + 1, "f-modified-since", 16) == 0) {
            return CSILK_HDR_IF_MODIFIED_SINCE;
        }
        if (c0 == 's' && strncasecmp(name + 1, "ec-websocket-key", 16) == 0) {
            return CSILK_HDR_SEC_WEBSOCKET_KEY;
        }
        if (c0 == 't' && strncasecmp(name + 1, "ransfer-encoding", 16) == 0) {
            return CSILK_HDR_TRANSFER_ENCODING;
        }
        break;
    }
    case 21: {
        if (strncasecmp(name, "sec-websocket-version", 21) == 0) {
            return CSILK_HDR_SEC_WEBSOCKET_VERSION;
        }
        break;
    }
    case 22: {
        if (strncasecmp(name, "sec-websocket-protocol", 22) == 0) {
            return CSILK_HDR_SEC_WEBSOCKET_PROTOCOL;
        }
        break;
    }
    case 24: {
        if (strncasecmp(name, "sec-websocket-extensions", 24) == 0) {
            return CSILK_HDR_SEC_WEBSOCKET_EXTENSIONS;
        }
        break;
    }
    default:
        break;
    }
    return CSILK_HDR_UNKNOWN;
}

/* ---------------------------------------------------------------------------
 * Single-pass Case-folding Hash
 * -------------------------------------------------------------------------*/

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

/* ---------------------------------------------------------------------------
 * ID-Based Lookups (Direct Slot Access)
 * -------------------------------------------------------------------------*/

const char*
map_get_id(csilk_header_map_t* map, csilk_header_id_t id)
{
    if (!map || id <= CSILK_HDR_UNKNOWN || (size_t)id >= CSILK_HDR_MAX_KNOWN) {
        return NULL;
    }
    csilk_header_t* h = map->known[id];
    return h ? h->value : NULL;
}

csilk_view_t
map_get_id_view(csilk_header_map_t* map, csilk_header_id_t id)
{
    if (!map || id <= CSILK_HDR_UNKNOWN || (size_t)id >= CSILK_HDR_MAX_KNOWN) {
        return csilk_view(NULL, 0);
    }
    csilk_header_t* h = map->known[id];
    if (h) {
        return csilk_view(h->value,
                          h->value_len ? h->value_len : (h->value ? strlen(h->value) : 0));
    }
    return csilk_view(NULL, 0);
}

/* ---------------------------------------------------------------------------
 * Name-Based Lookups (ID Fast Path + Hash Fallback)
 * -------------------------------------------------------------------------*/

const char*
map_get(csilk_header_map_t* map, const char* key)
{
    if (!map || !key) {
        return NULL;
    }

    size_t key_len = 0;
    while (key[key_len]) {
        key_len++;
    }

    /* 1. O(1) Fast path for known interned headers */
    csilk_header_id_t id = csilk_header_id_from_name(key, key_len);
    if (id != CSILK_HDR_UNKNOWN) {
        csilk_header_t* h = map->known[id];
        if (h) {
            return h->value;
        }
    }

    /* 2. Fallback: 3-level hash bucket search for custom headers */
    uint32_t        hash = header_hash(key, NULL);
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

csilk_view_t
map_get_view(csilk_header_map_t* map, const char* key)
{
    if (__builtin_expect(!map || !map->used || !key || !key[0], 0)) {
        return csilk_view(NULL, 0);
    }

    size_t key_len = 0;
    while (key[key_len]) {
        key_len++;
    }

    /* 1. O(1) Fast path for known interned headers */
    csilk_header_id_t id = csilk_header_id_from_name(key, key_len);
    if (id != CSILK_HDR_UNKNOWN) {
        csilk_header_t* h = map->known[id];
        if (h) {
            return csilk_view(h->value,
                              h->value_len ? h->value_len : (h->value ? strlen(h->value) : 0));
        }
    }

    /* 2. Fallback: 3-level hash bucket search for custom headers */
    uint32_t        hash = header_hash(key, NULL);
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

/* ---------------------------------------------------------------------------
 * Header Insert / Update
 * -------------------------------------------------------------------------*/

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

    size_t            key_len = key->len;
    csilk_header_id_t id = csilk_header_id_from_name(key->data, key_len);
    uint32_t          hash = header_hash_view(key->data, key_len);
    uint32_t          bucket = hash & (CSILK_HEADER_BUCKETS - 1);

    /* Check if existing slot exists for known ID */
    if (id != CSILK_HDR_UNKNOWN && map->known[id]) {
        csilk_header_t* h = map->known[id];
        h->value = csilk_arena_strndup(c->arena, value->data, value->len);
        h->value_len = value->len;
        return;
    }

    csilk_header_t* h = map->buckets[bucket];
    while (h) {
        if (h->hash == hash && h->key_len == key_len) {
            if (memcmp(h->key, key->data, key_len) == 0 ||
                strncasecmp(h->key, key->data, key_len) == 0) {
                h->value = csilk_arena_strndup(c->arena, value->data, value->len);
                h->value_len = value->len;
                if (id != CSILK_HDR_UNKNOWN) {
                    map->known[id] = h;
                }
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
    new_h->id = id;
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;

    if (id != CSILK_HDR_UNKNOWN) {
        map->known[id] = new_h;
    }
}

/** @brief Strip CR and LF control characters from header strings to prevent CRLF injection (CWE-113). */
static char*
_csilk_sanitize_header_str(csilk_arena_t* arena, const char* str, size_t len, size_t* out_len)
{
    if (!arena || !str) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
    char* clean = csilk_arena_alloc(arena, len + 1);
    if (!clean) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        if (str[i] != '\r' && str[i] != '\n') {
            clean[j++] = str[i];
        }
    }
    clean[j] = '\0';
    if (out_len) {
        *out_len = j;
    }
    return clean;
}

void
map_set(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
    if (!c || !c->arena || !map || !key || !value) {
        return;
    }
    map->used = 1;

    size_t clean_key_len = 0;
    char*  clean_key = _csilk_sanitize_header_str(c->arena, key, strlen(key), &clean_key_len);
    if (!clean_key || clean_key_len == 0) {
        return;
    }

    size_t clean_val_len = 0;
    char*  clean_val = _csilk_sanitize_header_str(c->arena, value, strlen(value), &clean_val_len);
    if (!clean_val) {
        return;
    }

    uint32_t          hash = header_hash_view(clean_key, clean_key_len);
    csilk_header_id_t id = csilk_header_id_from_name(clean_key, clean_key_len);
    uint32_t          bucket = hash & (CSILK_HEADER_BUCKETS - 1);

    /* Check if existing slot exists for known ID */
    if (id != CSILK_HDR_UNKNOWN && map->known[id]) {
        csilk_header_t* h = map->known[id];
        h->value = clean_val;
        h->value_len = clean_val_len;
        return;
    }

    csilk_header_t* h = map->buckets[bucket];
    while (h) {
        if (h->hash == hash && h->key_len == clean_key_len) {
            if (memcmp(h->key, clean_key, clean_key_len) == 0 ||
                strncasecmp(h->key, clean_key, clean_key_len) == 0) {
                h->value = clean_val;
                h->value_len = clean_val_len;
                if (id != CSILK_HDR_UNKNOWN) {
                    map->known[id] = h;
                }
                return;
            }
        }
        h = h->next;
    }

    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (!new_h) {
        return;
    }

    new_h->key = clean_key;
    new_h->key_len = clean_key_len;
    new_h->value = clean_val;
    new_h->value_len = clean_val_len;
    new_h->hash = hash;
    new_h->id = id;
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;

    if (id != CSILK_HDR_UNKNOWN) {
        map->known[id] = new_h;
    }
}

void
map_add(csilk_ctx_t* c, csilk_header_map_t* map, const char* key, const char* value)
{
    if (!c || !c->arena || !map || !key || !value) {
        return;
    }
    map->used = 1;

    size_t clean_key_len = 0;
    char*  clean_key = _csilk_sanitize_header_str(c->arena, key, strlen(key), &clean_key_len);
    if (!clean_key || clean_key_len == 0) {
        return;
    }

    size_t clean_val_len = 0;
    char*  clean_val = _csilk_sanitize_header_str(c->arena, value, strlen(value), &clean_val_len);
    if (!clean_val) {
        return;
    }

    uint32_t          hash = header_hash_view(clean_key, clean_key_len);
    csilk_header_id_t id = csilk_header_id_from_name(clean_key, clean_key_len);
    uint32_t          bucket = hash & (CSILK_HEADER_BUCKETS - 1);

    csilk_header_t* new_h = csilk_arena_alloc(c->arena, sizeof(csilk_header_t));
    if (!new_h) {
        return;
    }

    new_h->key = clean_key;
    new_h->key_len = clean_key_len;
    new_h->value = clean_val;
    new_h->value_len = clean_val_len;
    new_h->hash = hash;
    new_h->id = id;
    new_h->next = map->buckets[bucket];
    map->buckets[bucket] = new_h;

    if (id != CSILK_HDR_UNKNOWN) {
        map->known[id] = new_h;
    }
}
