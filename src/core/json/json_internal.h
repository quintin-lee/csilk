#pragma once
/**
 * @file src/core/json/json_internal.h
 * @brief Internal yyjson-backed implementation of csilk_json_t.
 *
 * This header is NOT part of the public API and must never be included
 * by consumers.  It lives in src/ so it remains hidden.
 */

#include "csilk/core/json.h"

#include <yyjson.h>

/**
 * Opaque JSON value handle backed by yyjson.
 *
 * We track both immutable (parsed) and mutable (built) forms because:
 *   - Parsed JSON comes from yyjson as immutable yyjson_val*.
 *   - Programmatically built JSON uses yyjson_mut_val* for efficiency.
 *   - Children of immutable parents are "views" (no own doc).
 *   - Children of mutable parents are also "views".
 */
typedef enum {
    CSILK_JSON_IMMUTABLE,
    CSILK_JSON_MUTABLE,
} csilk_json_kind_t;

struct csilk_json_s {
    union {
        yyjson_val*     ival; /**< Immutable yyjson value. */
        yyjson_mut_val* mval; /**< Mutable yyjson value. */
    } u;
    union {
        yyjson_doc*     idoc; /**< Owning or referenced immutable doc. */
        yyjson_mut_doc* mdoc; /**< Owning or referenced mutable doc. */
    } doc;
    bool is_owner;            /**< True if handle owns the doc (frees doc on csilk_json_free). */
    bool is_static;           /**< True if handle is in tls_view_ring (never free'd by free()). */
    csilk_json_kind_t kind;
};

/* ====================================================================
 * Thread-local view ring
 * ==================================================================== */

#define CSILK_JSON_VIEW_RING_SIZE 65536

extern __thread csilk_json_t tls_view_ring[CSILK_JSON_VIEW_RING_SIZE];
extern __thread size_t       tls_view_ring_idx;

/* ====================================================================
 * View creation helpers (internal linkage across modules)
 * ==================================================================== */

/** @brief Wrap a mutable yyjson value as an owning view. */
csilk_json_t* json_mut_new(yyjson_mut_doc* mdoc, yyjson_mut_val* mval);

/** @brief Wrap an immutable yyjson value as an owning view. */
csilk_json_t* json_imut_new(yyjson_doc* doc, yyjson_val* val);

/** @brief Wrap an immutable yyjson value as a non-owning view. */
csilk_json_t* json_view_immutable(yyjson_doc* idoc, yyjson_val* val);

/** @brief Wrap a mutable yyjson value as a non-owning view. */
csilk_json_t* json_view_mutable(yyjson_mut_doc* mdoc, yyjson_mut_val* mval);

/** @brief Shared helper: add item to object, handling cross-doc copy. */
bool json_add_to_obj(csilk_json_t* obj, const char* key, csilk_json_t* item);
