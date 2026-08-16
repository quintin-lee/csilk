/**
 * @file src/core/json/json_copy.c
 * @brief Deep-copy a csilk_json_t value.
 */

#include "json_internal.h"

csilk_json_t*
csilk_json_copy(const csilk_json_t* v)
{
    if (!v) {
        return NULL;
    }
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = NULL;
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (!v->u.mval) {
            yyjson_mut_doc_free(mdoc);
            return NULL;
        }
        mval = yyjson_mut_val_mut_copy(mdoc, v->u.mval);
    } else {
        if (!v->u.ival) {
            yyjson_mut_doc_free(mdoc);
            return NULL;
        }
        mval = yyjson_val_mut_copy(mdoc, v->u.ival);
    }
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}
