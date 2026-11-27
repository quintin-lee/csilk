/**
 * @file src/core/json/json_doc.c
 * @brief JSON document lifecycle: parse, serialize, copy, free, and value factories.
 */

#include "json_internal.h"
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Parse
 * ==================================================================== */

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

/* ====================================================================
 * Serialize
 * ==================================================================== */

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

/* ====================================================================
 * Copy
 * ==================================================================== */

csilk_json_t*
csilk_json_copy(const csilk_json_t* v)
{
    if (!v || !v->u.raw) {
        return NULL;
    }
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = NULL;
    if (json_is_mutable(v)) {
        if (!v->u.mval) {
            yyjson_mut_doc_free(mdoc);
            return NULL;
        }
        mval = yyjson_mut_val_mut_copy(mdoc, (yyjson_mut_val*)v->u.mval);
    } else {
        if (!v->u.ival) {
            yyjson_mut_doc_free(mdoc);
            return NULL;
        }
        mval = yyjson_val_mut_copy(mdoc, (yyjson_val*)v->u.ival);
    }
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

/* ====================================================================
 * Free
 * ==================================================================== */

void
csilk_json_free(csilk_json_t* v)
{
    if (!v) {
        return;
    }
    if (v->flags & CSILK_JSON_F_OWNER) {
        if (v->flags & CSILK_JSON_F_MUTABLE) {
            if (v->doc.mdoc) {
                json_release_doc_views(v->doc.mdoc);
                yyjson_mut_doc_free((yyjson_mut_doc*)v->doc.mdoc);
            }
        } else {
            if (v->doc.idoc) {
                json_release_doc_views(v->doc.idoc);
                yyjson_doc_free((yyjson_doc*)v->doc.idoc);
            }
        }
        v->flags &= ~CSILK_JSON_F_OWNER;
    }
    if (v->flags & CSILK_JSON_F_HEAP) {
        free(v);
    }
}

/* ====================================================================
 * Value Factories
 * ==================================================================== */

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
