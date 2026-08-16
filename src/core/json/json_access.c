/**
 * @file src/core/json/json_access.c
 * @brief Value accessors: get-by-key, type extractors, array indexing.
 */

#include "json_internal.h"

csilk_json_t*
csilk_json_get(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        yyjson_mut_val* v = yyjson_mut_obj_get(obj->u.mval, key);
        if (!v) {
            return NULL;
        }
        return json_view_mutable(obj->doc.mdoc, v);
    }
    yyjson_val* v = yyjson_obj_get(obj->u.ival, key);
    if (!v) {
        return NULL;
    }
    return json_view_immutable(obj->doc.idoc, v);
}

csilk_json_t*
csilk_json_get_object(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        yyjson_mut_val* v = yyjson_mut_obj_get(obj->u.mval, key);
        if (!v || !yyjson_mut_is_obj(v)) {
            return NULL;
        }
        return json_view_mutable(obj->doc.mdoc, v);
    }
    yyjson_val* v = yyjson_obj_get(obj->u.ival, key);
    if (!v || !yyjson_is_obj(v)) {
        return NULL;
    }
    return json_view_immutable(obj->doc.idoc, v);
}

csilk_json_t*
csilk_json_get_array(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        yyjson_mut_val* v = yyjson_mut_obj_get(obj->u.mval, key);
        if (!v || !yyjson_mut_is_arr(v)) {
            return NULL;
        }
        return json_view_mutable(obj->doc.mdoc, v);
    }
    yyjson_val* v = yyjson_obj_get(obj->u.ival, key);
    if (!v || !yyjson_is_arr(v)) {
        return NULL;
    }
    return json_view_immutable(obj->doc.idoc, v);
}

const char*
csilk_json_get_string(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        yyjson_mut_val* v = yyjson_mut_obj_get(obj->u.mval, key);
        if (!v || !yyjson_mut_is_str(v)) {
            return NULL;
        }
        return yyjson_mut_get_str(v);
    }
    yyjson_val* v = yyjson_obj_get(obj->u.ival, key);
    if (!v || !yyjson_is_str(v)) {
        return NULL;
    }
    return yyjson_get_str(v);
}

double
csilk_json_get_number(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return 0.0;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        yyjson_mut_val* v = yyjson_mut_obj_get(obj->u.mval, key);
        if (!v || !yyjson_mut_is_num(v)) {
            return 0.0;
        }
        return yyjson_mut_get_num(v);
    }
    yyjson_val* v = yyjson_obj_get(obj->u.ival, key);
    if (!v || !yyjson_is_num(v)) {
        return 0.0;
    }
    return yyjson_get_num(v);
}

int64_t
csilk_json_get_int(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return 0;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        yyjson_mut_val* v = yyjson_mut_obj_get(obj->u.mval, key);
        if (!v) {
            return 0;
        }
        if (yyjson_mut_is_int(v)) {
            return yyjson_mut_get_sint(v);
        }
        if (yyjson_mut_is_num(v)) {
            return (int64_t)yyjson_mut_get_num(v);
        }
        return 0;
    }
    yyjson_val* v = yyjson_obj_get(obj->u.ival, key);
    if (!v) {
        return 0;
    }
    if (yyjson_is_int(v)) {
        return yyjson_get_int(v);
    }
    if (yyjson_is_num(v)) {
        return (int64_t)yyjson_get_num(v);
    }
    return 0;
}

bool
csilk_json_get_bool(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind == CSILK_JSON_MUTABLE) {
        yyjson_mut_val* v = yyjson_mut_obj_get(obj->u.mval, key);
        if (!v || !yyjson_mut_is_bool(v)) {
            return false;
        }
        return yyjson_mut_get_bool(v);
    }
    yyjson_val* v = yyjson_obj_get(obj->u.ival, key);
    if (!v || !yyjson_is_bool(v)) {
        return false;
    }
    return yyjson_get_bool(v);
}

const char*
csilk_json_string_value(const csilk_json_t* v)
{
    if (!v) {
        return NULL;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_str(v->u.mval)) {
            return NULL;
        }
        return yyjson_mut_get_str(v->u.mval);
    }
    if (!yyjson_is_str(v->u.ival)) {
        return NULL;
    }
    return yyjson_get_str(v->u.ival);
}

double
csilk_json_number_value(const csilk_json_t* v)
{
    if (!v) {
        return 0.0;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_num(v->u.mval)) {
            return 0.0;
        }
        return yyjson_mut_get_num(v->u.mval);
    }
    if (!yyjson_is_num(v->u.ival)) {
        return 0.0;
    }
    return yyjson_get_num(v->u.ival);
}

int64_t
csilk_json_int_value(const csilk_json_t* v)
{
    if (!v) {
        return 0;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (yyjson_mut_is_int(v->u.mval)) {
            return yyjson_mut_get_sint(v->u.mval);
        }
        if (yyjson_mut_is_num(v->u.mval)) {
            return (int64_t)yyjson_mut_get_num(v->u.mval);
        }
        return 0;
    }
    if (yyjson_is_int(v->u.ival)) {
        return yyjson_get_int(v->u.ival);
    }
    if (yyjson_is_num(v->u.ival)) {
        return (int64_t)yyjson_get_num(v->u.ival);
    }
    return 0;
}

bool
csilk_json_bool_value(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_bool(v->u.mval)) {
            return false;
        }
        return yyjson_mut_get_bool(v->u.mval);
    }
    if (!yyjson_is_bool(v->u.ival)) {
        return false;
    }
    return yyjson_get_bool(v->u.ival);
}

csilk_json_t*
csilk_json_array_get(const csilk_json_t* arr, size_t index)
{
    if (!arr) {
        return NULL;
    }
    if (arr->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_arr(arr->u.mval)) {
            return NULL;
        }
        yyjson_mut_val* v = yyjson_mut_arr_get(arr->u.mval, index);
        if (!v) {
            return NULL;
        }
        return json_view_mutable(arr->doc.mdoc, v);
    }
    if (!yyjson_is_arr(arr->u.ival)) {
        return NULL;
    }
    yyjson_val* v = yyjson_arr_get(arr->u.ival, index);
    if (!v) {
        return NULL;
    }
    return json_view_immutable(arr->doc.idoc, v);
}

size_t
csilk_json_array_size(const csilk_json_t* arr)
{
    if (!arr) {
        return 0;
    }
    if (arr->kind == CSILK_JSON_MUTABLE) {
        if (!yyjson_mut_is_arr(arr->u.mval)) {
            return 0;
        }
        return yyjson_mut_arr_size(arr->u.mval);
    }
    if (!yyjson_is_arr(arr->u.ival)) {
        return 0;
    }
    return yyjson_arr_size(arr->u.ival);
}
