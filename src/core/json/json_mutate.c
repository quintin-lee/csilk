/**
 * @file src/core/json/json_mutate.c
 * @brief Mutation helpers: set_string.
 */

#include "json_internal.h"

bool
csilk_json_set_string(csilk_json_t* v, const char* new_value)
{
    if (!v || !new_value) {
        return false;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (!v->doc.mdoc || !yyjson_mut_is_str(v->u.mval)) {
            return false;
        }
        return yyjson_mut_set_str(v->u.mval, new_value);
    }
    if (!v->doc.idoc || !yyjson_is_str(v->u.ival)) {
        return false;
    }

    /* Convert immutable doc to mutable, replace root string, then swap. */
    yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(v->doc.idoc, NULL);
    if (!mdoc) {
        return false;
    }

    yyjson_mut_val* mroot = yyjson_mut_doc_get_root(mdoc);
    if (!mroot) {
        yyjson_mut_doc_free(mdoc);
        return false;
    }

    yyjson_mut_val* mnew_str = yyjson_mut_strcpy(mdoc, new_value);
    if (!mnew_str) {
        yyjson_mut_doc_free(mdoc);
        return false;
    }

    if (v->is_owner && v->doc.idoc) {
        yyjson_doc_free(v->doc.idoc);
    }
    v->u.mval = mnew_str;
    v->doc.mdoc = mdoc;
    v->is_owner = true;
    v->kind = CSILK_JSON_MUTABLE;
    return true;
}
