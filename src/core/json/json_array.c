/**
 * @file src/core/json/json_array.c
 * @brief Array operations: append, size.
 */

#include "json_internal.h"

bool
csilk_json_array_append(csilk_json_t* arr, csilk_json_t* item)
{
    if (!arr || !item) {
        return false;
    }
    if (arr->kind != CSILK_JSON_MUTABLE || !arr->doc.mdoc) {
        return false;
    }
    if (item->kind == CSILK_JSON_MUTABLE) {
        if (item->doc.mdoc == arr->doc.mdoc) {
            return yyjson_mut_arr_add_val(arr->u.mval, item->u.mval);
        }
        yyjson_mut_val* mval = yyjson_mut_val_mut_copy(arr->doc.mdoc, item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_arr_add_val(arr->u.mval, mval)) {
            return false;
        }
        if (item->is_owner && item->doc.mdoc) {
            yyjson_mut_doc_free(item->doc.mdoc);
        }
        item->u.mval = mval;
        item->doc.mdoc = arr->doc.mdoc;
        item->is_owner = false;
        return true;
    }
    yyjson_mut_val* mval = yyjson_val_mut_copy(arr->doc.mdoc, item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_arr_add_val(arr->u.mval, mval)) {
        return false;
    }
    if (item->is_owner && item->doc.idoc) {
        yyjson_doc_free(item->doc.idoc);
    }
    item->u.mval = mval;
    item->doc.mdoc = arr->doc.mdoc;
    item->is_owner = false;
    item->kind = CSILK_JSON_MUTABLE;
    return true;
}
