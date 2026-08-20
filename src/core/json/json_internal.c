/**
 * @file src/core/json/json_internal.c
 * @brief View creation helpers and memory management.
 */

#include "json_internal.h"

#define CSILK_JSON_SCRATCH_SIZE 64
static __thread csilk_json_t tls_view_scratch[CSILK_JSON_SCRATCH_SIZE];
static __thread size_t       tls_scratch_idx = 0;

csilk_json_t*
json_mut_new(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    csilk_json_t* j = malloc(sizeof(csilk_json_t));
    if (!j) {
        return NULL;
    }
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->flags = CSILK_JSON_F_OWNER | CSILK_JSON_F_MUTABLE | CSILK_JSON_F_HEAP;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_imut_new(yyjson_doc* doc, yyjson_val* val)
{
    if (!val) {
        return NULL;
    }
    csilk_json_t* j = malloc(sizeof(csilk_json_t));
    if (!j) {
        return NULL;
    }
    j->u.ival = val;
    j->doc.idoc = doc;
    j->flags = CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_view_immutable(yyjson_doc* idoc, yyjson_val* val)
{
    if (!val) {
        return NULL;
    }
    size_t        idx = (tls_scratch_idx++) % CSILK_JSON_SCRATCH_SIZE;
    csilk_json_t* j = &tls_view_scratch[idx];
    j->u.ival = val;
    j->doc.idoc = idoc;
    j->flags = 0;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_view_mutable(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    size_t        idx = (tls_scratch_idx++) % CSILK_JSON_SCRATCH_SIZE;
    csilk_json_t* j = &tls_view_scratch[idx];
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->flags = CSILK_JSON_F_MUTABLE;
    j->_pad = 0;
    return j;
}
