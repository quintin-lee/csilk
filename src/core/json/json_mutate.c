/**
 * @file src/core/json/json_mutate.c
 * @brief Mutation helpers: set_string.
 */

#include "json_internal.h"

bool
csilk_json_set_string(csilk_json_t* v, const char* new_value)
{
    if (!v || !new_value || !v->u.raw) {
        return false;
    }
    if (json_is_mutable(v)) {
        if (!v->doc.mdoc || !yyjson_mut_is_str((yyjson_mut_val*)v->u.mval)) {
            return false;
        }
        return yyjson_mut_set_str((yyjson_mut_val*)v->u.mval, new_value);
    }
    if (!v->doc.idoc || !yyjson_is_str((yyjson_val*)v->u.ival)) {
        return false;
    }

    /* Convert immutable doc to mutable, replace root string, then swap. */
    yyjson_mut_doc* mdoc = yyjson_doc_mut_copy((yyjson_doc*)v->doc.idoc, NULL);
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

    if (json_is_owner(v) && v->doc.idoc) {
        yyjson_doc_free((yyjson_doc*)v->doc.idoc);
    }
    v->u.mval = mnew_str;
    v->doc.mdoc = mdoc;
    v->flags |= (CSILK_JSON_F_OWNER | CSILK_JSON_F_MUTABLE);
    return true;
}
