#pragma once
#ifndef CSILK_CORE_JSON_H
/**
 * @file csilk/core/json.h
 * @brief Opaque JSON value type backed by yyjson.
 *
 * Provides a type-safe, opaque wrapper around yyjson that replaces the
 * direct cJSON dependency in all public headers.  Consumers only see
 * csilk_json_t* — the internal yyjson types are never exposed.
 *
 * Key design decisions:
 *   - Opaque type: callers never see yyjson_val or yyjson_doc.
 *   - Per-value doc pooling: every csilk_json_t owns (and frees) its
 *     yyjson_doc so that child values remain valid until the parent is freed.
 *   - No shallow copies: csilk_json_copy copies the entire subtree into
 *     a fresh document.
 *   - Serialization defaults to compact (no pretty-print) to match
 *     cJSON_PrintUnformatted behaviour.
 *
 * Migration: replace every csilk_json_t* with csilk_json_t*, and map the
 * cJSON_* API onto the csilk_json_* API listed below.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque JSON value handle. */
typedef struct csilk_json_s csilk_json_t;

/* ====================================================================
 * Creation
 * ==================================================================== */

/** Create an empty JSON object.  Caller owns the result. */
csilk_json_t* csilk_json_object(void);

/** Create an empty JSON array.  Caller owns the result. */
csilk_json_t* csilk_json_array(void);

/** Create a JSON string (null-terminated).  Caller owns the result. */
csilk_json_t* csilk_json_string_new(const char* s);

/** Create a JSON number (double).  Caller owns the result. */
csilk_json_t* csilk_json_number(double n);

/** Create a JSON integer (int64).  Caller owns the result. */
csilk_json_t* csilk_json_int(int64_t n);

/** Create a JSON boolean.  Caller owns the result. */
csilk_json_t* csilk_json_bool(bool b);

/** Create a JSON null.  Caller owns the result. */
csilk_json_t* csilk_json_null(void);

/* ====================================================================
 * Add to object
 * ==================================================================== */

/**
 * Add a value under @p key to an object.
 * @return true on success, false if obj or item is null.
 * The object takes ownership of item.
 */
bool          csilk_json_add_object(csilk_json_t* obj, const char* key, csilk_json_t* item);
bool          csilk_json_add_array(csilk_json_t* obj, const char* key, csilk_json_t* item);
csilk_json_t* csilk_json_add_array_obj(csilk_json_t* obj, const char* key, csilk_json_t* item);
bool          csilk_json_add_string(csilk_json_t* obj, const char* key, const char* value);
bool          csilk_json_add_number(csilk_json_t* obj, const char* key, double value);
bool          csilk_json_add_int(csilk_json_t* obj, const char* key, int64_t value);
bool          csilk_json_add_bool(csilk_json_t* obj, const char* key, bool value);
bool          csilk_json_add_null(csilk_json_t* obj, const char* key);

/** Add a value to an object without a key (append as-is). */
bool csilk_json_add_item(csilk_json_t* obj, csilk_json_t* item);

/* ====================================================================
 * Add to array
 * ==================================================================== */

/** Append a value to an array.  The array takes ownership of item. */
bool csilk_json_array_append(csilk_json_t* arr, csilk_json_t* item);

/* ====================================================================
 * Get / inspect
 * ==================================================================== */

/** Get a child object by key.  Returns NULL if key not found or not an object. */
/** Generic get: returns the child value regardless of type. NULL if not found. */
csilk_json_t* csilk_json_get(const csilk_json_t* obj, const char* key);

csilk_json_t* csilk_json_get_object(const csilk_json_t* obj, const char* key);

/** Get a child array by key.  Returns NULL if key not found or not an array. */
csilk_json_t* csilk_json_get_array(const csilk_json_t* obj, const char* key);

/** Get a child string by key.  Returns NULL if key not found or not a string. */
const char* csilk_json_get_string(const csilk_json_t* obj, const char* key);

/** Get a child number by key as double.  Returns 0.0 if key not found or not a number. */
double csilk_json_get_number(const csilk_json_t* obj, const char* key);

/** Get a child integer by key (valueint equivalent). Returns 0 if not an integer. */
int64_t csilk_json_get_int(const csilk_json_t* obj, const char* key);

/** Get a child boolean by key.  Returns false if key not found or not a bool. */
bool csilk_json_get_bool(const csilk_json_t* obj, const char* key);

/** Get a child string directly from a string node. Returns NULL if not a string. */
const char* csilk_json_string_value(const csilk_json_t* v);

/** Get a child number directly from a number node as double. */
double csilk_json_number_value(const csilk_json_t* v);

/** Get a child integer directly from a number node (valueint equivalent). */
int64_t csilk_json_int_value(const csilk_json_t* v);

/** Get the N-th element of an array.  Returns NULL if out of bounds. */
csilk_json_t* csilk_json_array_get(const csilk_json_t* arr, size_t index);

/** Return the number of elements in an array. */
size_t csilk_json_array_size(const csilk_json_t* arr);

/* ====================================================================
 * Type predicates
 * ==================================================================== */

bool csilk_json_is_null(const csilk_json_t* v);
bool csilk_json_is_object(const csilk_json_t* v);
bool csilk_json_is_array(const csilk_json_t* v);
bool csilk_json_is_string(const csilk_json_t* v);
bool csilk_json_is_number(const csilk_json_t* v);
bool csilk_json_is_bool(const csilk_json_t* v);
bool csilk_json_is_true(const csilk_json_t* v);
bool csilk_json_is_false(const csilk_json_t* v);

/* ====================================================================
 * Parse
 * ==================================================================== */

/**
 * Parse a null-terminated JSON string.
 * @return New csilk_json_t on success, NULL on failure.
 * Caller must free with csilk_json_free().
 */
csilk_json_t* csilk_json_parse(const char* json_str);

/**
 * Parse a JSON string with explicit length (handles embedded NULs).
 */
csilk_json_t* csilk_json_parse_len(const char* json_str, size_t len);

/**
 * Parse with error output.
 * @param error  [out] Static pointer describing the error, or NULL.
 * @return New csilk_json_t on success, NULL on failure.
 */
csilk_json_t* csilk_json_parse_err(const char* json_str, const char** error);

/* ====================================================================
 * Serialize
 * ==================================================================== */

/**
 * Serialize to a compact JSON string (equivalent to cJSON_PrintUnformatted).
 * @param len  [out] Optional pointer to receive string length (excl. NUL).
 * @return Heap-allocated NUL-terminated string. Caller must free().
 */
char* csilk_json_serialize(const csilk_json_t* v, size_t* len);

/**
 * Serialize to a pretty-printed JSON string (equivalent to cJSON_Print).
 */
char* csilk_json_serialize_pretty(const csilk_json_t* v, size_t* len);

/* ====================================================================
 * Free
 * ==================================================================== */

/**
 * Free a csilk_json_t and all its children.
 * Equivalent to cJSON_Delete.
 */
void csilk_json_free(csilk_json_t* v);

/* ====================================================================
 * Copy
 * ==================================================================== */

/** Deep-copy a JSON value into a new csilk_json_t. Caller owns the result. */
csilk_json_t* csilk_json_copy(const csilk_json_t* v);

/* ====================================================================
 * Key iteration (for walking object keys)
 * ==================================================================== */

/**
 * Return the N-th key in an object.  Returns NULL if out of bounds.
 * The returned string pointer is valid until the json is freed.
 */
const char* csilk_json_object_key(const csilk_json_t* obj, size_t index);

/** Return the number of keys in an object. */
size_t csilk_json_object_size(const csilk_json_t* obj);

/** Get the value at the N-th key position. */
csilk_json_t* csilk_json_object_val(const csilk_json_t* obj, size_t index);

/* ====================================================================
 * Mutation (in-place update — requires copying for yyjson immutability)
 * ==================================================================== */

/**
 * Replace the string value of an existing string node in-place.
 * Because yyjson values are immutable, this creates a new string value
 * and replaces the key in the parent object.
 * @return true on success, false on failure.
 */
bool csilk_json_set_string(csilk_json_t* v, const char* new_value);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CORE_JSON_H */