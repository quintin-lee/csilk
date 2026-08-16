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
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }

    if (item->kind == CSILK_JSON_MUTABLE) {
        if (item->doc.mdoc == obj->doc.mdoc) {
            return yyjson_mut_obj_add(
                obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), item->u.mval);
        }
        yyjson_mut_val* mval = yyjson_mut_val_mut_copy(obj->doc.mdoc, item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_obj_add(obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), mval)) {
            return false;
        }
        if (item->is_owner && item->doc.mdoc) {
            yyjson_mut_doc_free(item->doc.mdoc);
        }
        item->u.mval = mval;
        item->doc.mdoc = obj->doc.mdoc;
        item->is_owner = false;
        return true;
    }

    /* item is immutable — deep-copy into mutable doc */
    yyjson_mut_val* mval = yyjson_val_mut_copy(obj->doc.mdoc, item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_obj_add(obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), mval)) {
        return false;
    }
    if (item->is_owner && item->doc.idoc) {
        yyjson_doc_free(item->doc.idoc);
    }
    item->u.mval = mval;
    item->doc.mdoc = obj->doc.mdoc;
    item->is_owner = false;
    item->kind = CSILK_JSON_MUTABLE;
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
    return item;
}

bool
csilk_json_add_string(csilk_json_t* obj, const char* key, const char* value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    if (!value) {
        return csilk_json_add_null(obj, key);
    }
    return yyjson_mut_obj_add_strcpy(obj->doc.mdoc, obj->u.mval, key, value);
}

bool
csilk_json_add_number(csilk_json_t* obj, const char* key, double value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(obj->u.mval,
                              yyjson_mut_strcpy(obj->doc.mdoc, key),
                              yyjson_mut_double(obj->doc.mdoc, value));
}

bool
csilk_json_add_int(csilk_json_t* obj, const char* key, int64_t value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(
        obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), yyjson_mut_sint(obj->doc.mdoc, value));
}

bool
csilk_json_add_bool(csilk_json_t* obj, const char* key, bool value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(
        obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), yyjson_mut_bool(obj->doc.mdoc, value));
}

bool
csilk_json_add_null(csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(
        obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), yyjson_mut_null(obj->doc.mdoc));
}

bool
csilk_json_add_item(csilk_json_t* obj, csilk_json_t* item)
{
    if (!obj || !item) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    if (item->kind == CSILK_JSON_MUTABLE) {
        if (item->doc.mdoc == obj->doc.mdoc) {
            return yyjson_mut_arr_add_val(obj->u.mval, item->u.mval);
        }
        yyjson_mut_val* mval = yyjson_mut_val_mut_copy(obj->doc.mdoc, item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_arr_add_val(obj->u.mval, mval)) {
            return false;
        }
        if (item->is_owner && item->doc.mdoc) {
            yyjson_mut_doc_free(item->doc.mdoc);
        }
        item->u.mval = mval;
        item->doc.mdoc = obj->doc.mdoc;
        item->is_owner = false;
        return true;
    }
    yyjson_mut_val* mval = yyjson_val_mut_copy(obj->doc.mdoc, item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_arr_add_val(obj->u.mval, mval)) {
        return false;
    }
    if (item->is_owner && item->doc.idoc) {
        yyjson_doc_free(item->doc.idoc);
    }
    item->u.mval = mval;
    item->doc.mdoc = obj->doc.mdoc;
    item->is_owner = false;
    item->kind = CSILK_JSON_MUTABLE;
    return true;
}
