/**
 * @file src/core/json/json_parse.c
 * @brief JSON parsing functions.
 */

#include "json_internal.h"

#include <string.h>

csilk_json_t*
csilk_json_parse(const char* json_str)
{
    if (!json_str) {
        return NULL;
    }
    return csilk_json_parse_len(json_str, strlen(json_str));
}

csilk_json_t*
csilk_json_parse_len(const char* json_str, size_t len)
{
    if (!json_str) {
        return NULL;
    }
    yyjson_doc* doc = yyjson_read(json_str, len, 0);
    if (!doc) {
        return NULL;
    }
    return json_imut_new(doc, yyjson_doc_get_root(doc));
}

csilk_json_t*
csilk_json_parse_err(const char* json_str, const char** error)
{
    if (error) {
        *error = NULL;
    }
    if (!json_str) {
        if (error) {
            *error = "Null input";
        }
        return NULL;
    }
    size_t          len = strlen(json_str);
    yyjson_read_err err;
    yyjson_doc*     doc =
        yyjson_read_opts((char*)(void*)(size_t)(const void*)json_str, len, 0, NULL, &err);
    if (!doc) {
        if (error) {
            *error = err.msg ? err.msg : "Invalid JSON";
        }
        return NULL;
    }
    return json_imut_new(doc, yyjson_doc_get_root(doc));
}
