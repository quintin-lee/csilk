#pragma once
/**
 * @file src/core/json/json_internal.h
 * @brief Internal yyjson-backed implementation of csilk_json_t.
 *
 * This header is NOT part of the public API and must never be included
 * by consumers. It lives in src/ so it remains hidden.
 */

#include <stdlib.h>
#include "csilk/core/json/json.h"
#include <yyjson.h>

enum {
    CSILK_JSON_F_OWNER = 1U << 0,
    CSILK_JSON_F_MUTABLE = 1U << 1,
    CSILK_JSON_F_HEAP = 1U << 2,
};

static inline csilk_json_t*
csilk_json_invalid(void)
{
    return NULL;
}

struct csilk_json_s {
    union {
        void* raw;
        void* ival;
        void* mval;
    } u;
    union {
        void* raw;
        void* idoc;
        void* mdoc;
    } doc;
    uint32_t flags;
    uint32_t _pad;
};

static inline yyjson_val*
json_get_ival(const csilk_json_t* j)
{
    return (yyjson_val*)j->u.ival;
}

static inline yyjson_mut_val*
json_get_mval(const csilk_json_t* j)
{
    return (yyjson_mut_val*)j->u.mval;
}

static inline yyjson_doc*
json_get_idoc(const csilk_json_t* j)
{
    return (yyjson_doc*)j->doc.idoc;
}

static inline yyjson_mut_doc*
json_get_mdoc(const csilk_json_t* j)
{
    return (yyjson_mut_doc*)j->doc.mdoc;
}

static inline bool
json_is_mutable(const csilk_json_t* j)
{
    return (j->flags & CSILK_JSON_F_MUTABLE) != 0;
}

static inline bool
json_is_owner(const csilk_json_t* j)
{
    return (j->flags & CSILK_JSON_F_OWNER) != 0;
}

/* ====================================================================
 * Handle creation helpers
 * ==================================================================== */

/** @brief Wrap a mutable yyjson value as an owning root handle. */
csilk_json_t* json_mut_new(yyjson_mut_doc* mdoc, yyjson_mut_val* mval);

/** @brief Wrap an immutable yyjson value as an owning root handle. */
csilk_json_t* json_imut_new(yyjson_doc* doc, yyjson_val* val);

/** @brief Wrap an immutable yyjson value as a non-owning view pointer. */
csilk_json_t* json_view_immutable(yyjson_doc* idoc, yyjson_val* val);

/** @brief Wrap a mutable yyjson value as a non-owning view pointer. */
csilk_json_t* json_view_mutable(yyjson_mut_doc* mdoc, yyjson_mut_val* mval);

/** @brief Shared helper: add item to object, handling cross-doc copy. */
bool json_add_to_obj(csilk_json_t* obj, const char* key, csilk_json_t* item);
