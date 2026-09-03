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
    if (!json_is_mutable(arr) || !arr->doc.mdoc) {
        return false;
    }
    if (json_is_mutable(item)) {
        if (item->doc.mdoc == arr->doc.mdoc) {
            return yyjson_mut_arr_add_val((yyjson_mut_val*)arr->u.mval,
                                          (yyjson_mut_val*)item->u.mval);
        }
        yyjson_mut_val* mval =
            yyjson_mut_val_mut_copy((yyjson_mut_doc*)arr->doc.mdoc, (yyjson_mut_val*)item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_arr_add_val((yyjson_mut_val*)arr->u.mval, mval)) {
            return false;
        }
        if (json_is_owner(item) && item->doc.mdoc) {
            json_va_release(item);
            yyjson_mut_doc_free((yyjson_mut_doc*)item->doc.mdoc);
        }
        bool is_heap = (item->flags & CSILK_JSON_F_HEAP) != 0;
        item->u.mval = mval;
        item->doc.mdoc = arr->doc.mdoc;
        item->flags &= ~(CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP);
        if (is_heap) {
            free(item);
        }
        return true;
    }
    yyjson_mut_val* mval =
        yyjson_val_mut_copy((yyjson_mut_doc*)arr->doc.mdoc, (yyjson_val*)item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_arr_add_val((yyjson_mut_val*)arr->u.mval, mval)) {
        return false;
    }
    if (json_is_owner(item) && item->doc.idoc) {
        json_va_release(item);
        yyjson_doc_free((yyjson_doc*)item->doc.idoc);
    }
    bool is_heap = (item->flags & CSILK_JSON_F_HEAP) != 0;
    item->u.mval = mval;
    item->doc.mdoc = arr->doc.mdoc;
    item->flags &= ~(CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP);
    item->flags |= CSILK_JSON_F_MUTABLE;
    if (is_heap) {
        free(item);
    }
    return true;
}
