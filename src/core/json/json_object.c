/**
 * @file src/core/json/json_object.c
 * @brief Object mutation: csilk_json_add_* functions.
 */

#include "json_internal.h"

bool
json_add_to_obj(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    if (!obj || !key || !item) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }

    if (json_is_mutable(item)) {
        if (item->doc.mdoc == obj->doc.mdoc) {
            return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                                      yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                                      (yyjson_mut_val*)item->u.mval);
        }
        yyjson_mut_val* mval =
            yyjson_mut_val_mut_copy((yyjson_mut_doc*)obj->doc.mdoc, (yyjson_mut_val*)item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                                yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                                mval)) {
            return false;
        }
        if (json_is_owner(item) && item->doc.mdoc) {
            yyjson_mut_doc_free((yyjson_mut_doc*)item->doc.mdoc);
        }
        bool is_heap = (item->flags & CSILK_JSON_F_HEAP) != 0;
        item->u.mval = mval;
        item->doc.mdoc = obj->doc.mdoc;
        item->flags &= ~(CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP);
        if (is_heap) {
            free(item);
        }
        return true;
    }

    /* item is immutable — deep-copy into mutable doc */
    yyjson_mut_val* mval =
        yyjson_val_mut_copy((yyjson_mut_doc*)obj->doc.mdoc, (yyjson_val*)item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                            yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                            mval)) {
        return false;
    }
    if (json_is_owner(item) && item->doc.idoc) {
        yyjson_doc_free((yyjson_doc*)item->doc.idoc);
    }
    bool is_heap = (item->flags & CSILK_JSON_F_HEAP) != 0;
    item->u.mval = mval;
    item->doc.mdoc = obj->doc.mdoc;
    item->flags &= ~(CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP);
    item->flags |= CSILK_JSON_F_MUTABLE;
    if (is_heap) {
        free(item);
    }
    return true;
}

bool
csilk_json_add_object(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

bool
csilk_json_add_array(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

csilk_json_t*
csilk_json_add_array_obj(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    if (!json_add_to_obj(obj, key, item)) {
        return NULL;
    }
    return csilk_json_get(obj, key);
}

bool
csilk_json_add_string(csilk_json_t* obj, const char* key, const char* value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc || !obj->u.mval) {
        return false;
    }
    if (!value) {
        return csilk_json_add_null(obj, key);
    }
    yyjson_mut_val* key_val = yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key);
    yyjson_mut_val* value_val = yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, value);
    if (!key_val || !value_val) {
        return false;
    }
    if (!yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval, key_val, value_val)) {
        return false;
    }
    return true;
}

bool
csilk_json_add_number(csilk_json_t* obj, const char* key, double value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_double((yyjson_mut_doc*)obj->doc.mdoc, value));
}

bool
csilk_json_add_int(csilk_json_t* obj, const char* key, int64_t value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_sint((yyjson_mut_doc*)obj->doc.mdoc, value));
}

bool
csilk_json_add_bool(csilk_json_t* obj, const char* key, bool value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_bool((yyjson_mut_doc*)obj->doc.mdoc, value));
}

bool
csilk_json_add_null(csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_null((yyjson_mut_doc*)obj->doc.mdoc));
}

bool
csilk_json_add_item(csilk_json_t* obj, csilk_json_t* item)
{
    if (!obj || !item) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    if (json_is_mutable(item)) {
        if (item->doc.mdoc == obj->doc.mdoc) {
            return yyjson_mut_arr_add_val((yyjson_mut_val*)obj->u.mval,
                                          (yyjson_mut_val*)item->u.mval);
        }
        yyjson_mut_val* mval =
            yyjson_mut_val_mut_copy((yyjson_mut_doc*)obj->doc.mdoc, (yyjson_mut_val*)item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_arr_add_val((yyjson_mut_val*)obj->u.mval, mval)) {
            return false;
        }
        if (json_is_owner(item) && item->doc.mdoc) {
            yyjson_mut_doc_free((yyjson_mut_doc*)item->doc.mdoc);
        }
        bool is_heap = (item->flags & CSILK_JSON_F_HEAP) != 0;
        item->u.mval = mval;
        item->doc.mdoc = obj->doc.mdoc;
        item->flags &= ~(CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP);
        if (is_heap) {
            free(item);
        }
        return true;
    }
    yyjson_mut_val* mval =
        yyjson_val_mut_copy((yyjson_mut_doc*)obj->doc.mdoc, (yyjson_val*)item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_arr_add_val((yyjson_mut_val*)obj->u.mval, mval)) {
        return false;
    }
    if (json_is_owner(item) && item->doc.idoc) {
        yyjson_doc_free((yyjson_doc*)item->doc.idoc);
    }
    bool is_heap = (item->flags & CSILK_JSON_F_HEAP) != 0;
    item->u.mval = mval;
    item->doc.mdoc = obj->doc.mdoc;
    item->flags &= ~(CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP);
    item->flags |= CSILK_JSON_F_MUTABLE;
    if (is_heap) {
        free(item);
    }
    return true;
}
