/**
 * @file src/core/json/json.c
 * @brief yyjson-backed implementation of the opaque csilk_json_t API.
 */

#include "json_internal.h"

#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Internal helpers
 * ==================================================================== */

#define CSILK_JSON_VIEW_RING_SIZE 65536

static __thread csilk_json_t tls_view_ring[CSILK_JSON_VIEW_RING_SIZE];
static __thread size_t       tls_view_ring_idx = 0;

static csilk_json_t*
json_mut_new(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->is_owner = true;
    j->is_static = true;
    j->kind = CSILK_JSON_MUTABLE;
    return j;
}

static csilk_json_t*
json_imut_new(yyjson_doc* doc, yyjson_val* val)
{
    if (!val) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.ival = val;
    j->doc.idoc = doc;
    j->is_owner = true;
    j->is_static = true;
    j->kind = CSILK_JSON_IMMUTABLE;
    return j;
}

static csilk_json_t*
json_view_immutable(yyjson_doc* idoc, yyjson_val* val)
{
    if (!val) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.ival = val;
    j->doc.idoc = idoc;
    j->is_owner = false;
    j->is_static = true;
    j->kind = CSILK_JSON_IMMUTABLE;
    return j;
}

static csilk_json_t*
json_view_mutable(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    size_t        idx = (tls_view_ring_idx++) % CSILK_JSON_VIEW_RING_SIZE;
    csilk_json_t* j = &tls_view_ring[idx];
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->is_owner = false;
    j->is_static = true;
    j->kind = CSILK_JSON_MUTABLE;
    return j;
}

/* ====================================================================
 * Creation
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

/* ====================================================================
 * Add to object
 * ==================================================================== */

static bool
json_add_to_obj(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    if (!obj || !key || !item) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }

    if (item->kind == CSILK_JSON_MUTABLE) {
        if (item->doc.mdoc == obj->doc.mdoc) {
            return yyjson_mut_obj_add(
                obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), item->u.mval);
        }
        yyjson_mut_val* mval = yyjson_mut_val_mut_copy(obj->doc.mdoc, item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_obj_add(obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), mval)) {
            return false;
        }
        if (item->is_owner && item->doc.mdoc) {
            yyjson_mut_doc_free(item->doc.mdoc);
        }
        item->u.mval = mval;
        item->doc.mdoc = obj->doc.mdoc;
        item->is_owner = false;
        return true;
    }

    /* item is immutable — deep-copy into mutable doc */
    yyjson_mut_val* mval = yyjson_val_mut_copy(obj->doc.mdoc, item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_obj_add(obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), mval)) {
        return false;
    }
    if (item->is_owner && item->doc.idoc) {
        yyjson_doc_free(item->doc.idoc);
    }
    item->u.mval = mval;
    item->doc.mdoc = obj->doc.mdoc;
    item->is_owner = false;
    item->kind = CSILK_JSON_MUTABLE;
    return true;
}

bool
csilk_json_add_object(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

bool
csilk_json_add_array(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

csilk_json_t*
csilk_json_add_array_obj(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    if (!json_add_to_obj(obj, key, item)) {
        return NULL;
    }
    return item;
}

bool
csilk_json_add_string(csilk_json_t* obj, const char* key, const char* value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    if (!value) {
        return csilk_json_add_null(obj, key);
    }
    return yyjson_mut_obj_add_strcpy(obj->doc.mdoc, obj->u.mval, key, value);
}

bool
csilk_json_add_number(csilk_json_t* obj, const char* key, double value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(obj->u.mval,
                              yyjson_mut_strcpy(obj->doc.mdoc, key),
                              yyjson_mut_double(obj->doc.mdoc, value));
}

bool
csilk_json_add_int(csilk_json_t* obj, const char* key, int64_t value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(
        obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), yyjson_mut_sint(obj->doc.mdoc, value));
}

bool
csilk_json_add_bool(csilk_json_t* obj, const char* key, bool value)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(
        obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), yyjson_mut_bool(obj->doc.mdoc, value));
}

bool
csilk_json_add_null(csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add(
        obj->u.mval, yyjson_mut_strcpy(obj->doc.mdoc, key), yyjson_mut_null(obj->doc.mdoc));
}

bool
csilk_json_add_item(csilk_json_t* obj, csilk_json_t* item)
{
    if (!obj || !item) {
        return false;
    }
    if (obj->kind != CSILK_JSON_MUTABLE || !obj->doc.mdoc) {
        return false;
    }
    if (item->kind == CSILK_JSON_MUTABLE) {
        if (item->doc.mdoc == obj->doc.mdoc) {
            return yyjson_mut_arr_add_val(obj->u.mval, item->u.mval);
        }
        yyjson_mut_val* mval = yyjson_mut_val_mut_copy(obj->doc.mdoc, item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_arr_add_val(obj->u.mval, mval)) {
            return false;
        }
        if (item->is_owner && item->doc.mdoc) {
            yyjson_mut_doc_free(item->doc.mdoc);
        }
        item->u.mval = mval;
        item->doc.mdoc = obj->doc.mdoc;
        item->is_owner = false;
        return true;
    }
    yyjson_mut_val* mval = yyjson_val_mut_copy(obj->doc.mdoc, item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_arr_add_val(obj->u.mval, mval)) {
        return false;
    }
    if (item->is_owner && item->doc.idoc) {
        yyjson_doc_free(item->doc.idoc);
    }
    item->u.mval = mval;
    item->doc.mdoc = obj->doc.mdoc;
    item->is_owner = false;
    item->kind = CSILK_JSON_MUTABLE;
    return true;
}

/* ====================================================================
 * Add to array
 * ==================================================================== */

bool
csilk_json_array_append(csilk_json_t* arr, csilk_json_t* item)
{
    if (!arr || !item) {
        return false;
    }
    if (arr->kind != CSILK_JSON_MUTABLE || !arr->doc.mdoc) {
        return false;
    }
    if (item->kind == CSILK_JSON_MUTABLE) {
        if (item->doc.mdoc == arr->doc.mdoc) {
            return yyjson_mut_arr_add_val(arr->u.mval, item->u.mval);
        }
        yyjson_mut_val* mval = yyjson_mut_val_mut_copy(arr->doc.mdoc, item->u.mval);
        if (!mval) {
            return false;
        }
        if (!yyjson_mut_arr_add_val(arr->u.mval, mval)) {
            return false;
        }
        if (item->is_owner && item->doc.mdoc) {
            yyjson_mut_doc_free(item->doc.mdoc);
        }
        item->u.mval = mval;
        item->doc.mdoc = arr->doc.mdoc;
        item->is_owner = false;
        return true;
    }
    yyjson_mut_val* mval = yyjson_val_mut_copy(arr->doc.mdoc, item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_arr_add_val(arr->u.mval, mval)) {
        return false;
    }
    if (item->is_owner && item->doc.idoc) {
        yyjson_doc_free(item->doc.idoc);
    }
    item->u.mval = mval;
    item->doc.mdoc = arr->doc.mdoc;
    item->is_owner = false;
    item->kind = CSILK_JSON_MUTABLE;
    return true;
}

/* ====================================================================
 * Get / inspect
 * ==================================================================== */

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

/* ====================================================================
 * Type predicates
 * ==================================================================== */

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

/* ====================================================================
 * Free
 * ==================================================================== */

void
csilk_json_free(csilk_json_t* v)
{
    if (!v) {
        return;
    }
    if (v->is_owner) {
        if (v->kind == CSILK_JSON_MUTABLE) {
            if (v->doc.mdoc) {
                yyjson_mut_doc_free(v->doc.mdoc);
            }
        } else {
            if (v->doc.idoc) {
                yyjson_doc_free(v->doc.idoc);
            }
        }
    }
    if (!v->is_static) {
        free(v);
    }
}

/* ====================================================================
 * Copy
 * ==================================================================== */

csilk_json_t*
csilk_json_copy(const csilk_json_t* v)
{
    if (!v) {
        return NULL;
    }
    yyjson_mut_doc* mdoc = yyjson_mut_doc_new(NULL);
    if (!mdoc) {
        return NULL;
    }
    yyjson_mut_val* mval = NULL;
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (!v->u.mval) {
            yyjson_mut_doc_free(mdoc);
            return NULL;
        }
        mval = yyjson_mut_val_mut_copy(mdoc, v->u.mval);
    } else {
        if (!v->u.ival) {
            yyjson_mut_doc_free(mdoc);
            return NULL;
        }
        mval = yyjson_val_mut_copy(mdoc, v->u.ival);
    }
    if (!mval) {
        yyjson_mut_doc_free(mdoc);
        return NULL;
    }
    return json_mut_new(mdoc, mval);
}

/* ====================================================================
 * Key iteration
 * ==================================================================== */

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

/* ====================================================================
 * Mutation
 * ==================================================================== */

bool
csilk_json_set_string(csilk_json_t* v, const char* new_value)
{
    if (!v || !new_value) {
        return false;
    }
    if (v->kind == CSILK_JSON_MUTABLE) {
        if (!v->doc.mdoc || !yyjson_mut_is_str(v->u.mval)) {
            return false;
        }
        return yyjson_mut_set_str(v->u.mval, new_value);
    }
    if (!v->doc.idoc || !yyjson_is_str(v->u.ival)) {
        return false;
    }

    /* Convert immutable doc to mutable, replace root string, then swap. */
    yyjson_mut_doc* mdoc = yyjson_doc_mut_copy(v->doc.idoc, NULL);
    if (!mdoc) {
        return false;
    }

    yyjson_mut_val* mroot = yyjson_mut_doc_get_root(mdoc);
    if (!mroot) {
        yyjson_mut_doc_free(mdoc);
        return false;
    }

    yyjson_mut_val* mnew_str = yyjson_mut_strcpy(mdoc, new_value);
    if (!mnew_str) {
        yyjson_mut_doc_free(mdoc);
        return false;
    }

    if (v->is_owner && v->doc.idoc) {
        yyjson_doc_free(v->doc.idoc);
    }
    v->u.mval = mnew_str;
    v->doc.mdoc = mdoc;
    v->is_owner = true;
    v->kind = CSILK_JSON_MUTABLE;
    return true;
}
