/**
 * @file src/core/json/json_serialize.c
 * @brief JSON serialization functions.
 */

#include "json_internal.h"

char*
csilk_json_serialize(const csilk_json_t* v, size_t* len)
{
    if (!v) {
        return NULL;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        return yyjson_mut_val_write(v->u.mval, 0, len);
    }
    return yyjson_val_write(v->u.ival, 0, len);
}

char*
csilk_json_serialize_pretty(const csilk_json_t* v, size_t* len)
{
    if (!v) {
        return NULL;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        return yyjson_mut_val_write(v->u.mval, YYJSON_WRITE_PRETTY, len);
    }
    return yyjson_val_write(v->u.ival, YYJSON_WRITE_PRETTY, len);
}
