/**
 * @file src/core/json/json_access.c
 * @brief Value accessors: get-by-key, type extractors, array indexing.
 */

#include "json_internal.h"

/* ====================================================================
 * Value Object Accessors (Zero heap / Zero TLS, Register Return)
 * ==================================================================== */

csilk_json_t*
csilk_json_get_v(const csilk_json_t* obj, const char* key)
{
    return csilk_json_get(obj, key);
}

csilk_json_t*
csilk_json_get_object_v(const csilk_json_t* obj, const char* key)
{
    return csilk_json_get_object(obj, key);
}

csilk_json_t*
csilk_json_get_array_v(const csilk_json_t* obj, const char* key)
{
    return csilk_json_get_array(obj, key);
}

csilk_json_t*
csilk_json_array_get_v(const csilk_json_t* arr, size_t index)
{
    if (!arr || !arr->u.raw) {
        return NULL;
    }
    if (json_is_mutable(arr)) {
        if (!yyjson_mut_is_arr((yyjson_mut_val*)arr->u.mval)) {
            return NULL;
        }
        yyjson_mut_val* v = yyjson_mut_arr_get((yyjson_mut_val*)arr->u.mval, index);
        if (!v) {
            return NULL;
        }
        return json_view_mutable((yyjson_mut_doc*)arr->doc.mdoc, v);
    }
    if (!yyjson_is_arr((yyjson_val*)arr->u.ival)) {
        return csilk_json_invalid();
    }
    yyjson_val* v = yyjson_arr_get((yyjson_val*)arr->u.ival, index);
    if (!v) {
        return csilk_json_invalid();
    }
    return json_view_immutable((yyjson_doc*)arr->doc.idoc, v);
}

const char*
csilk_json_get_string_v(const csilk_json_t* obj, const char* key)
{
    csilk_json_t* v = csilk_json_get_v(obj, key);
    return csilk_json_string_value(v);
}

double
csilk_json_get_number_v(const csilk_json_t* obj, const char* key)
{
    csilk_json_t* v = csilk_json_get_v(obj, key);
    return csilk_json_number_value(v);
}

int64_t
csilk_json_get_int_v(const csilk_json_t* obj, const char* key)
{
    csilk_json_t* v = csilk_json_get_v(obj, key);
    return csilk_json_int_value(v);
}

bool
csilk_json_get_bool_v(const csilk_json_t* obj, const char* key)
{
    csilk_json_t* v = csilk_json_get_v(obj, key);
    return csilk_json_bool_value(v);
}

/* ====================================================================
 * Pointer Accessors
 * ==================================================================== */

csilk_json_t*
csilk_json_get(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (json_is_mutable(obj)) {
        yyjson_mut_val* v = yyjson_mut_obj_get((yyjson_mut_val*)obj->u.mval, key);
        if (!v) {
            return NULL;
        }
        return json_view_mutable((yyjson_mut_doc*)obj->doc.mdoc, v);
    }
    yyjson_val* v = yyjson_obj_get((yyjson_val*)obj->u.ival, key);
    if (!v) {
        return NULL;
    }
    return json_view_immutable((yyjson_doc*)obj->doc.idoc, v);
}

csilk_json_t*
csilk_json_get_object(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (json_is_mutable(obj)) {
        yyjson_mut_val* v = yyjson_mut_obj_get((yyjson_mut_val*)obj->u.mval, key);
        if (!v || !yyjson_mut_is_obj(v)) {
            return NULL;
        }
        return json_view_mutable((yyjson_mut_doc*)obj->doc.mdoc, v);
    }
    yyjson_val* v = yyjson_obj_get((yyjson_val*)obj->u.ival, key);
    if (!v || !yyjson_is_obj(v)) {
        return NULL;
    }
    return json_view_immutable((yyjson_doc*)obj->doc.idoc, v);
}

csilk_json_t*
csilk_json_get_array(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (json_is_mutable(obj)) {
        yyjson_mut_val* v = yyjson_mut_obj_get((yyjson_mut_val*)obj->u.mval, key);
        if (!v || !yyjson_mut_is_arr(v)) {
            return NULL;
        }
        return json_view_mutable((yyjson_mut_doc*)obj->doc.mdoc, v);
    }
    yyjson_val* v = yyjson_obj_get((yyjson_val*)obj->u.ival, key);
    if (!v || !yyjson_is_arr(v)) {
        return NULL;
    }
    return json_view_immutable((yyjson_doc*)obj->doc.idoc, v);
}

const char*
csilk_json_get_string(const csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return NULL;
    }
    if (json_is_mutable(obj)) {
        yyjson_mut_val* v = yyjson_mut_obj_get((yyjson_mut_val*)obj->u.mval, key);
        if (!v || !yyjson_mut_is_str(v)) {
            return NULL;
        }
        return yyjson_mut_get_str(v);
    }
    yyjson_val* v = yyjson_obj_get((yyjson_val*)obj->u.ival, key);
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
    if (json_is_mutable(obj)) {
        yyjson_mut_val* v = yyjson_mut_obj_get((yyjson_mut_val*)obj->u.mval, key);
        if (!v || !yyjson_mut_is_num(v)) {
            return 0.0;
        }
        return yyjson_mut_get_num(v);
    }
    yyjson_val* v = yyjson_obj_get((yyjson_val*)obj->u.ival, key);
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
    if (json_is_mutable(obj)) {
        yyjson_mut_val* v = yyjson_mut_obj_get((yyjson_mut_val*)obj->u.mval, key);
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
    yyjson_val* v = yyjson_obj_get((yyjson_val*)obj->u.ival, key);
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
    if (json_is_mutable(obj)) {
        yyjson_mut_val* v = yyjson_mut_obj_get((yyjson_mut_val*)obj->u.mval, key);
        if (!v || !yyjson_mut_is_bool(v)) {
            return false;
        }
        return yyjson_mut_get_bool(v);
    }
    yyjson_val* v = yyjson_obj_get((yyjson_val*)obj->u.ival, key);
    if (!v || !yyjson_is_bool(v)) {
        return false;
    }
    return yyjson_get_bool(v);
}

const char*
csilk_json_string_value(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return NULL;
    }
    if (json_is_mutable(v)) {
        if (!yyjson_mut_is_str((yyjson_mut_val*)v->u.mval)) {
            return NULL;
        }
        return yyjson_mut_get_str((yyjson_mut_val*)v->u.mval);
    }
    if (!yyjson_is_str((yyjson_val*)v->u.ival)) {
        return NULL;
    }
    return yyjson_get_str((yyjson_val*)v->u.ival);
}

double
csilk_json_number_value(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return 0.0;
    }
    if (json_is_mutable(v)) {
        if (!yyjson_mut_is_num((yyjson_mut_val*)v->u.mval)) {
            return 0.0;
        }
        return yyjson_mut_get_num((yyjson_mut_val*)v->u.mval);
    }
    if (!yyjson_is_num((yyjson_val*)v->u.ival)) {
        return 0.0;
    }
    return yyjson_get_num((yyjson_val*)v->u.ival);
}

int64_t
csilk_json_int_value(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return 0;
    }
    if (json_is_mutable(v)) {
        if (yyjson_mut_is_int((yyjson_mut_val*)v->u.mval)) {
            return yyjson_mut_get_sint((yyjson_mut_val*)v->u.mval);
        }
        if (yyjson_mut_is_num((yyjson_mut_val*)v->u.mval)) {
            return (int64_t)yyjson_mut_get_num((yyjson_mut_val*)v->u.mval);
        }
        return 0;
    }
    if (yyjson_is_int((yyjson_val*)v->u.ival)) {
        return yyjson_get_int((yyjson_val*)v->u.ival);
    }
    if (yyjson_is_num((yyjson_val*)v->u.ival)) {
        return (int64_t)yyjson_get_num((yyjson_val*)v->u.ival);
    }
    return 0;
}

bool
csilk_json_bool_value(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    if (json_is_mutable(v)) {
        if (!yyjson_mut_is_bool((yyjson_mut_val*)v->u.mval)) {
            return false;
        }
        return yyjson_mut_get_bool((yyjson_mut_val*)v->u.mval);
    }
    if (!yyjson_is_bool((yyjson_val*)v->u.ival)) {
        return false;
    }
    return yyjson_get_bool((yyjson_val*)v->u.ival);
}

csilk_json_t*
csilk_json_array_get(const csilk_json_t* arr, size_t index)
{
    if (!arr) {
        return NULL;
    }
    if (json_is_mutable(arr)) {
        if (!yyjson_mut_is_arr((yyjson_mut_val*)arr->u.mval)) {
            return NULL;
        }
        yyjson_mut_val* v = yyjson_mut_arr_get((yyjson_mut_val*)arr->u.mval, index);
        if (!v) {
            return NULL;
        }
        return json_view_mutable((yyjson_mut_doc*)arr->doc.mdoc, v);
    }
    if (!yyjson_is_arr((yyjson_val*)arr->u.ival)) {
        return NULL;
    }
    yyjson_val* v = yyjson_arr_get((yyjson_val*)arr->u.ival, index);
    if (!v) {
        return NULL;
    }
    return json_view_immutable((yyjson_doc*)arr->doc.idoc, v);
}

size_t
csilk_json_array_size(const csilk_json_t* arr)
{
    if (!arr || !arr->u.raw) {
        return 0;
    }
    if (json_is_mutable(arr)) {
        if (!yyjson_mut_is_arr((yyjson_mut_val*)arr->u.mval)) {
            return 0;
        }
        return yyjson_mut_arr_size((yyjson_mut_val*)arr->u.mval);
    }
    if (!yyjson_is_arr((yyjson_val*)arr->u.ival)) {
        return 0;
    }
    return yyjson_arr_size((yyjson_val*)arr->u.ival);
}
