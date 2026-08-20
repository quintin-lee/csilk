/**
 * @file src/core/json/json_type.c
 * @brief Type predicates: is_null, is_object, is_array, is_string, etc.
 */

#include "json_internal.h"

bool
csilk_json_is_null(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_null((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_null((yyjson_val*)v->u.ival);
}

bool
csilk_json_is_object(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_obj((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_obj((yyjson_val*)v->u.ival);
}

bool
csilk_json_is_array(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_arr((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_arr((yyjson_val*)v->u.ival);
}

bool
csilk_json_is_string(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_str((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_str((yyjson_val*)v->u.ival);
}

bool
csilk_json_is_number(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_num((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_num((yyjson_val*)v->u.ival);
}

bool
csilk_json_is_bool(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_bool((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_bool((yyjson_val*)v->u.ival);
}

bool
csilk_json_is_true(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_true((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_true((yyjson_val*)v->u.ival);
}

bool
csilk_json_is_false(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return false;
    }
    return json_is_mutable(v) ? yyjson_mut_is_false((yyjson_mut_val*)v->u.mval)
                              : yyjson_is_false((yyjson_val*)v->u.ival);
}
