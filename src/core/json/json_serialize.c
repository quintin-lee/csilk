/**
 * @file src/core/json/json_serialize.c
 * @brief JSON serialization functions.
 */

#include "json_internal.h"

char*
csilk_json_serialize(const csilk_json_t* v, size_t* len)
{
    if (!v || !v->u.raw) {
        return NULL;
    }
    if (json_is_mutable(v)) {
        return yyjson_mut_val_write((yyjson_mut_val*)v->u.mval, 0, len);
    }
    return yyjson_val_write((yyjson_val*)v->u.ival, 0, len);
}

char*
csilk_json_serialize_pretty(const csilk_json_t* v, size_t* len)
{
    if (!v || !v->u.raw) {
        return NULL;
    }
    if (json_is_mutable(v)) {
        return yyjson_mut_val_write((yyjson_mut_val*)v->u.mval, YYJSON_WRITE_PRETTY, len);
    }
    return yyjson_val_write((yyjson_val*)v->u.ival, YYJSON_WRITE_PRETTY, len);
}
