/**
 * @file src/core/json/json_internal.c
 * @brief Thread-local view ring and view creation helpers.
 */

#include "json_internal.h"

__thread csilk_json_t tls_view_ring[CSILK_JSON_VIEW_RING_SIZE];
__thread size_t       tls_view_ring_idx = 0;

csilk_json_t*
json_mut_new(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->is_owner = true;
    j->is_static = true;
    j->kind = CSILK_JSON_MUTABLE;
    return j;
}

csilk_json_t*
json_imut_new(yyjson_doc* doc, yyjson_val* val)
{
    if (!val) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.ival = val;
    j->doc.idoc = doc;
    j->is_owner = true;
    j->is_static = true;
    j->kind = CSILK_JSON_IMMUTABLE;
    return j;
}

csilk_json_t*
json_view_immutable(yyjson_doc* idoc, yyjson_val* val)
{
    if (!val) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.ival = val;
    j->doc.idoc = idoc;
    j->is_owner = false;
    j->is_static = true;
    j->kind = CSILK_JSON_IMMUTABLE;
    return j;
}

csilk_json_t*
json_view_mutable(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->is_owner = false;
    j->is_static = true;
    j->kind = CSILK_JSON_MUTABLE;
    return j;
}
