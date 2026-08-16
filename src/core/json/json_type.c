/**
 * @file src/core/json/json_type.c
 * @brief Type predicates: is_null, is_object, is_array, is_string, etc.
 */

#include "json_internal.h"

bool
csilk_json_is_null(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_null(v->u.mval)
                                         : yyjson_is_null(v->u.ival);
}

bool
csilk_json_is_object(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_obj(v->u.mval) : yyjson_is_obj(v->u.ival);
}

bool
csilk_json_is_array(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_arr(v->u.mval) : yyjson_is_arr(v->u.ival);
}

bool
csilk_json_is_string(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_str(v->u.mval) : yyjson_is_str(v->u.ival);
}

bool
csilk_json_is_number(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_num(v->u.mval) : yyjson_is_num(v->u.ival);
}

bool
csilk_json_is_bool(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_bool(v->u.mval)
                                         : yyjson_is_bool(v->u.ival);
}

bool
csilk_json_is_true(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_true(v->u.mval)
                                         : yyjson_is_true(v->u.ival);
}

bool
csilk_json_is_false(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_false(v->u.mval)
                                         : yyjson_is_false(v->u.ival);
}
