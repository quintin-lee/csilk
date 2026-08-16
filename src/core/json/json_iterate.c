/**
 * @file src/core/json/json_iterate.c
 * @brief Object key/value iteration helpers.
 */

#include "json_internal.h"

const char*
csilk_json_object_key(const csilk_json_t* obj, size_t index)
{
    if (!obj) {
        return NULL;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_obj(obj->u.mval)) {
            return NULL;
        }
        yyjson_mut_obj_iter it;
        yyjson_mut_obj_iter_init(obj->u.mval, &it);
        yyjson_mut_val* key_val;
        size_t          i = 0;
        while ((key_val = yyjson_mut_obj_iter_next(&it))) {
            if (i == index) {
                return yyjson_mut_get_str(key_val);
            }
            i++;
        }
        return NULL;
    }
    if (!yyjson_is_obj(obj->u.ival)) {
        return NULL;
    }
    yyjson_obj_iter it;
    yyjson_obj_iter_init(obj->u.ival, &it);
    yyjson_val* key_val;
    size_t      i = 0;
    while ((key_val = yyjson_obj_iter_next(&it))) {
        if (i == index) {
            return yyjson_get_str(key_val);
        }
        i++;
    }
    return NULL;
}

size_t
csilk_json_object_size(const csilk_json_t* obj)
{
    if (!obj) {
        return 0;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_obj(obj->u.mval)) {
            return 0;
        }
        return yyjson_mut_obj_size(obj->u.mval);
    }
    if (!yyjson_is_obj(obj->u.ival)) {
        return 0;
    }
    return yyjson_obj_size(obj->u.ival);
}

csilk_json_t*
csilk_json_object_val(const csilk_json_t* obj, size_t index)
{
    if (!obj) {
        return NULL;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_obj(obj->u.mval)) {
            return NULL;
        }
        yyjson_mut_obj_iter it;
        yyjson_mut_obj_iter_init(obj->u.mval, &it);
        yyjson_mut_val* key_val;
        size_t          i = 0;
        while ((key_val = yyjson_mut_obj_iter_next(&it))) {
            if (i == index) {
                return json_view_mutable(obj->doc.mdoc, yyjson_mut_obj_iter_get_val(key_val));
            }
            i++;
        }
        return NULL;
    }
    if (!yyjson_is_obj(obj->u.ival)) {
        return NULL;
    }
    yyjson_obj_iter it;
    yyjson_obj_iter_init(obj->u.ival, &it);
    yyjson_val* key_val;
    size_t      i = 0;
    while ((key_val = yyjson_obj_iter_next(&it))) {
        if (i == index) {
            return json_view_immutable(obj->doc.idoc, yyjson_obj_iter_get_val(key_val));
        }
        i++;
    }
    return NULL;
}
