/**
 * @file src/core/json/json_factory.c
 * @brief JSON value factory functions (object, array, string, number, etc.).
 */

#include "json_internal.h"

csilk_json_t*
csilk_json_object(void)
{
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = yyjson_mut_obj(mdoc);
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

csilk_json_t*
csilk_json_array(void)
{
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = yyjson_mut_arr(mdoc);
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

csilk_json_t*
csilk_json_string_new(const char* s)
{
    if (!s) {
        return csilk_json_null();
    }
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = yyjson_mut_strcpy(mdoc, s);
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

csilk_json_t*
csilk_json_number(double n)
{
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = yyjson_mut_double(mdoc, n);
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

csilk_json_t*
csilk_json_int(int64_t n)
{
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = yyjson_mut_sint(mdoc, n);
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

csilk_json_t*
csilk_json_bool(bool b)
{
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = yyjson_mut_bool(mdoc, b);
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

csilk_json_t*
csilk_json_null(void)
{
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = yyjson_mut_null(mdoc);
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}
