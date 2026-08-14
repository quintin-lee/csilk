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

/** @brief Wrap a mutable yyjson value as an owning csilk_json_t view.
 *
 * Allocates a slot from the thread-local view ring and populates it with the
 * given mutable document/value, marking the view as the owner of @p mdoc.
 *
 * @param mdoc Owning mutable document (may be NULL if mval is NULL).
 * @param mval Mutable yyjson value to wrap.
 * @return Thread-local view, or NULL if mval is NULL.
 * @note The returned view is stored in a thread-local ring buffer and is not
 *       safe to share between threads. */
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

/** @brief Wrap an immutable yyjson value as an owning csilk_json_t view.
 *
 * Allocates a slot from the thread-local view ring and populates it with the
 * given immutable document/value, marking the view as the owner of @p doc.
 *
 * @param doc  Owning immutable document (may be NULL if val is NULL).
 * @param val  Immutable yyjson value to wrap.
 * @return Thread-local view, or NULL if val is NULL.
 * @note The returned view is stored in a thread-local ring buffer and is not
 *       safe to share between threads. */
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

/** @brief Wrap an immutable yyjson value as a non-owning (read-only) view.
 *
 * Creates a view that borrows @p idoc/@p val without taking ownership, so
 * freeing the view will not free the underlying document.
 *
 * @param idoc Immutable document the value belongs to.
 * @param val  Immutable yyjson value to wrap.
 * @return Thread-local view, or NULL if val is NULL.
 * @note Non-owning views must not outlive the document they reference. */
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

/** @brief Wrap a mutable yyjson value as a non-owning view.
 *
 * Creates a view that borrows @p mdoc/@p mval without taking ownership, so
 * freeing the view will not free the underlying document.
 *
 * @param mdoc Mutable document the value belongs to.
 * @param mval Mutable yyjson value to wrap.
 * @return Thread-local view, or NULL if mval is NULL.
 * @note Non-owning views must not outlive the document they reference. */
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

/**
 * @brief Create a new empty mutable JSON object.
 * @return A new mutable csilk_json_t object, or NULL on allocation failure.
 */
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

/**
 * @brief Create a new empty mutable JSON array.
 * @return A new mutable csilk_json_t array, or NULL on allocation failure.
 */
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

/**
 * @brief Create a JSON string value from a NUL-terminated C string.
 * @param[in] s Source string (may be NULL, which yields a JSON null).
 * @return A new mutable csilk_json_t string, or NULL on allocation failure.
 */
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

/**
 * @brief Create a JSON number value from a double.
 * @param[in] n Floating-point value to store.
 * @return A new mutable csilk_json_t number, or NULL on allocation failure.
 */
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

/**
 * @brief Create a JSON integer value from a signed 64-bit integer.
 * @param[in] n Integer value to store.
 * @return A new mutable csilk_json_t integer, or NULL on allocation failure.
 */
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

/**
 * @brief Create a JSON boolean value.
 * @param[in] b Boolean value to store.
 * @return A new mutable csilk_json_t boolean, or NULL on allocation failure.
 */
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

/**
 * @brief Create a JSON null value.
 * @return A new mutable csilk_json_t null, or NULL on allocation failure.
 */
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

/** @brief Insert a key/value pair into a mutable JSON object.
 *
 * Internal helper backing the public csilk_json_add_* family. When @p item
 * lives in a different document it is deep-copied into @p obj's document; the
 * copied item's document is then freed if it was an owning view.
 *
 * @param[in] obj  Target mutable object (must be MUTABLE).
 * @param[in] key  NUL-terminated key.
 * @param[in] item Value to insert.
 * @return true on success, false on NULL args, wrong object kind, or copy
 *         failure.
 * @note For immutable @p item values, the value is converted to a mutable copy
 *       inside @p obj's document. */
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

/**
 * @brief Add an object value to a JSON object under the given key.
 * @param[in] obj  Target mutable object.
 * @param[in] key  NUL-terminated key.
 * @param[in] item Object value to add.
 * @return true on success, false on NULL args, non-object target, or failure.
 */
bool
csilk_json_add_object(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

/**
 * @brief Add an array value to a JSON object under the given key.
 * @param[in] obj  Target mutable object.
 * @param[in] key  NUL-terminated key.
 * @param[in] item Array value to add.
 * @return true on success, false on NULL args, non-object target, or failure.
 */
bool
csilk_json_add_array(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

/**
 * @brief Add an array value under a key and return the added item.
 *
 * Behaves like csilk_json_add_array but returns @p item (or NULL on failure)
 * so callers can keep mutating the inserted value.
 *
 * @param[in] obj  Target mutable object.
 * @param[in] key  NUL-terminated key.
 * @param[in] item Array value to add.
 * @return @p item on success, or NULL on failure.
 */
csilk_json_t*
csilk_json_add_array_obj(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    if (!json_add_to_obj(obj, key, item)) {
        return NULL;
    }
    return item;
}

/**
 * @brief Add a string value to a JSON object under the given key.
 * @param[in] obj   Target mutable object.
 * @param[in] key   NUL-terminated key.
 * @param[in] value String value (may be NULL, which stores JSON null).
 * @return true on success, false on NULL args, non-object target, or failure.
 */
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

/**
 * @brief Add a double number value to a JSON object under the given key.
 * @param[in] obj   Target mutable object.
 * @param[in] key   NUL-terminated key.
 * @param[in] value Numeric value to store.
 * @return true on success, false on NULL args, non-object target, or failure.
 */
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

/**
 * @brief Add a signed 64-bit integer value to a JSON object under a key.
 * @param[in] obj   Target mutable object.
 * @param[in] key   NUL-terminated key.
 * @param[in] value Integer value to store.
 * @return true on success, false on NULL args, non-object target, or failure.
 */
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

/**
 * @brief Add a boolean value to a JSON object under the given key.
 * @param[in] obj   Target mutable object.
 * @param[in] key   NUL-terminated key.
 * @param[in] value Boolean value to store.
 * @return true on success, false on NULL args, non-object target, or failure.
 */
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

/**
 * @brief Add a JSON null value to an object under the given key.
 * @param[in] obj Target mutable object.
 * @param[in] key NUL-terminated key.
 * @return true on success, false on NULL args, non-object target, or failure.
 */
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

/**
 * @brief Append an item to a JSON array (root must be an array).
 * @param[in] obj  Target mutable array.
 * @param[in] item Value to append.
 * @return true on success, false on NULL args, non-array target, or failure.
 */
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

/**
 * @brief Append an item to the end of a mutable JSON array.
 *
 * Equivalent to csilk_json_add_item but documented separately for callers that
 * append to a value already known to be an array.
 *
 * @param[in] arr  Target mutable array.
 * @param[in] item Value to append.
 * @return true on success, false on NULL args, non-array target, or failure.
 */
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

/**
 * @brief Look up a value by key, returning a non-owning view of any type.
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] key NUL-terminated key.
 * @return Non-owning view of the value, or NULL if absent/invalid.
 * @note The returned view borrows the underlying document; do not free it.
 */
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

/**
 * @brief Look up an object-typed value by key.
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] key NUL-terminated key.
 * @return Non-owning view of the object, or NULL if absent or not an object.
 */
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

/**
 * @brief Look up an array-typed value by key.
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] key NUL-terminated key.
 * @return Non-owning view of the array, or NULL if absent or not an array.
 */
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

/**
 * @brief Look up a string value by key and return its C string.
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] key NUL-terminated key.
 * @return NUL-terminated string, or NULL if absent or not a string.
 * @note The returned pointer aliases document memory and must not be freed.
 */
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

/**
 * @brief Look up a numeric value by key as a double.
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] key NUL-terminated key.
 * @return The numeric value, or 0.0 if absent or not a number.
 */
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

/**
 * @brief Look up an integer value by key as a signed 64-bit integer.
 *
 * Accepts both integer and floating-point JSON numbers (the latter are
 * truncated). Returns 0 on absence or non-numeric types.
 *
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] key NUL-terminated key.
 * @return The integer value, or 0 if absent or not numeric.
 */
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

/**
 * @brief Look up a boolean value by key.
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] key NUL-terminated key.
 * @return The boolean value, or false if absent or not a boolean.
 */
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

/**
 * @brief Return the C string of a JSON string value.
 * @param[in] v JSON value (mutable or immutable).
 * @return NUL-terminated string, or NULL if v is NULL or not a string.
 * @note The returned pointer aliases document memory and must not be freed.
 */
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

/**
 * @brief Return the double value of a JSON number value.
 * @param[in] v JSON value (mutable or immutable).
 * @return The numeric value, or 0.0 if v is NULL or not a number.
 */
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

/**
 * @brief Return the signed 64-bit integer of a JSON value.
 * @param[in] v JSON value (mutable or immutable).
 * @return The integer value, or 0 if v is NULL or not numeric.
 */
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

/**
 * @brief Return the boolean of a JSON value.
 * @param[in] v JSON value (mutable or immutable).
 * @return The boolean value, or false if v is NULL or not a boolean.
 */
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

/**
 * @brief Return the array element at the given index.
 * @param[in] arr JSON array (mutable or immutable).
 * @param[in] index Zero-based element index.
 * @return Non-owning element view, or NULL if out of range or not array.
 * @note The returned view borrows the underlying document; do not free it.
 */
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

/**
 * @brief Return the number of elements in a JSON array.
 * @param[in] arr JSON array (mutable or immutable).
 * @return Element count, or 0 if arr is NULL or not an array.
 */
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

/**
 * @brief Test whether a JSON value is null.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is a JSON null, false otherwise.
 */
bool
csilk_json_is_null(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_null(v->u.mval)
                                         : yyjson_is_null(v->u.ival);
}

/**
 * @brief Test whether a JSON value is an object.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is a JSON object, false otherwise.
 */
bool
csilk_json_is_object(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_obj(v->u.mval) : yyjson_is_obj(v->u.ival);
}

/**
 * @brief Test whether a JSON value is an array.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is a JSON array, false otherwise.
 */
bool
csilk_json_is_array(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_arr(v->u.mval) : yyjson_is_arr(v->u.ival);
}

/**
 * @brief Test whether a JSON value is a string.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is a JSON string, false otherwise.
 */
bool
csilk_json_is_string(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_str(v->u.mval) : yyjson_is_str(v->u.ival);
}

/**
 * @brief Test whether a JSON value is a number.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is a JSON number, false otherwise.
 */
bool
csilk_json_is_number(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_num(v->u.mval) : yyjson_is_num(v->u.ival);
}

/**
 * @brief Test whether a JSON value is a boolean.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is a JSON boolean, false otherwise.
 */
bool
csilk_json_is_bool(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_bool(v->u.mval)
                                         : yyjson_is_bool(v->u.ival);
}

/**
 * @brief Test whether a JSON value is the boolean true.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is JSON true, false otherwise.
 */
bool
csilk_json_is_true(const csilk_json_t* v)
{
    if (!v) {
        return false;
    }
    return v->kind == CSILK_JSON_MUTABLE ? yyjson_mut_is_true(v->u.mval)
                                         : yyjson_is_true(v->u.ival);
}

/**
 * @brief Test whether a JSON value is the boolean false.
 * @param[in] v JSON value (may be NULL).
 * @return true if v is JSON false, false otherwise.
 */
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

/**
 * @brief Parse a NUL-terminated JSON string into a csilk_json_t.
 * @param[in] json_str NUL-terminated JSON text (may be NULL).
 * @return An owning immutable csilk_json_t, or NULL on NULL input/parse error.
 */
csilk_json_t*
csilk_json_parse(const char* json_str)
{
    if (!json_str) {
        return NULL;
    }
    return csilk_json_parse_len(json_str, strlen(json_str));
}

/**
 * @brief Parse a JSON string of explicit length into a csilk_json_t.
 * @param[in] json_str JSON text (may be NULL).
 * @param[in] len     Length of json_str in bytes.
 * @return An owning immutable csilk_json_t, or NULL on NULL input/parse error.
 */
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

/**
 * @brief Parse JSON, reporting the error message on failure.
 *
 * Like csilk_json_parse but writes a human-readable error string to @p error
 * when parsing fails (or when @p json_str is NULL).
 *
 * @param[in]  json_str JSON text.
 * @param[out] error    Receives an error message pointer (may be NULL).
 * @return An owning immutable csilk_json_t, or NULL on error.
 */
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

/**
 * @brief Serialize a JSON value to a compact JSON string.
 * @param[in]  v   JSON value to serialize.
 * @param[out] len Receives the length of the returned string (may be NULL).
 * @return Newly malloc'd JSON string (caller frees), or NULL on error/NULL v.
 */
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

/**
 * @brief Serialize a JSON value to a pretty-printed JSON string.
 * @param[in]  v   JSON value to serialize.
 * @param[out] len Receives the length of the returned string (may be NULL).
 * @return Newly malloc'd JSON string (caller frees), or NULL on error/NULL v.
 */
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

/**
 * @brief Free a csilk_json_t value.
 *
 * Frees the underlying document only when the view is the owner; non-owning
 * views leave the document intact. Thread-local static views (allocated from
 * the ring buffer) are never individually freed.
 *
 * @param[in] v JSON value to free (no-op if NULL).
 */
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

/**
 * @brief Deep-copy a JSON value into a new owning csilk_json_t.
 * @param[in] v JSON value to copy (may be NULL).
 * @return New owning csilk_json_t copy, or NULL on NULL input/alloc error.
 */
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

/**
 * @brief Return the object key at the given index, or NULL if not an object.
 *
 * Iterates the object's key/value pairs and returns the key string of the
 * element at @p index.
 *
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] index Zero-based key index.
 * @return NUL-terminated key string, or NULL if out of range / not an object.
 * @note The returned pointer aliases document memory and must not be freed.
 */
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

/**
 * @brief Return the number of key/value pairs in a JSON object.
 * @param[in] obj JSON object (mutable or immutable).
 * @return Number of keys, or 0 if obj is NULL or not an object.
 */
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

/**
 * @brief Return the object value at the given index.
 * @param[in] obj JSON object (mutable or immutable).
 * @param[in] index Zero-based key index.
 * @return Non-owning value view, or NULL if out of range or not an object.
 * @note The returned view borrows the underlying document; do not free it.
 */
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

/**
 * @brief Replace the contents of a JSON string value.
 *
 * For a mutable string value, mutates it in place. For an immutable value, the
 * underlying document is converted to a mutable copy and the root is replaced,
 * taking ownership of the new document.
 *
 * @param[in] v         JSON value to mutate (must be a string).
 * @param[in] new_value New string contents.
 * @return true on success, false on NULL args / non-string value / alloc error.
 */
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
