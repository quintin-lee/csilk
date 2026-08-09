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
        yyjson_val*     ival;   /**< Immutable yyjson value. */
        yyjson_mut_val* mval;   /**< Mutable yyjson value. */
    } u;
    union {
        yyjson_doc*     idoc;   /**< Owning or referenced immutable doc. */
        yyjson_mut_doc* mdoc;   /**< Owning or referenced mutable doc. */
    } doc;
    bool              is_owner; /**< True if handle owns the doc (frees doc on csilk_json_free). */
    csilk_json_kind_t kind;
};
