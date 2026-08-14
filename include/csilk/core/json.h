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

/** @brief Opaque JSON value handle. */
typedef struct csilk_json_s csilk_json_t;

/* ====================================================================
 * Creation
 * ==================================================================== */

/** @brief Create an empty JSON object.  Caller owns the result. */
csilk_json_t* csilk_json_object(void);

/** @brief Create an empty JSON array.  Caller owns the result. */
csilk_json_t* csilk_json_array(void);

/** @brief Create a JSON string (null-terminated).  Caller owns the result.
 *  @param[in] s Null-terminated input string.
 */
csilk_json_t* csilk_json_string_new(const char* s);

/** @brief Create a JSON number (double).  Caller owns the result.
 *  @param[in] n Numeric value.
 */
csilk_json_t* csilk_json_number(double n);

/** @brief Create a JSON integer (int64).  Caller owns the result.
 *  @param[in] n Integer value.
 */
csilk_json_t* csilk_json_int(int64_t n);

/** @brief Create a JSON boolean.  Caller owns the result.
 *  @param[in] b Boolean value.
 */
csilk_json_t* csilk_json_bool(bool b);

/** @brief Create a JSON null.  Caller owns the result. */
csilk_json_t* csilk_json_null(void);

/* ====================================================================
 * Add to object
 * ==================================================================== */

/**
 * @brief Add @p item under @p key to an object.
 * @param[in,out] obj Target object (takes ownership of @p item).
 * @param[in] key Object key (copied).
 * @param[in] item Value to add; becomes owned by @p obj.
 * @return true on success, false if obj or item is null.
 */
bool csilk_json_add_object(csilk_json_t* obj, const char* key, csilk_json_t* item);

/**
 * @brief Add @p item as an array under @p key to an object.
 * @param[in,out] obj Target object (takes ownership of @p item).
 * @param[in] key Object key (copied).
 * @param[in] item Value to add; becomes owned by @p obj.
 * @return true on success, false if obj or item is null.
 */
bool csilk_json_add_array(csilk_json_t* obj, const char* key, csilk_json_t* item);

/**
 * @brief Add @p item under @p key to an object and return it.
 * @param[in,out] obj Target object (takes ownership of @p item).
 * @param[in] key Object key (copied).
 * @param[in] item Value to add; becomes owned by @p obj.
 * @return The added @p item, or NULL on failure.
 */
csilk_json_t* csilk_json_add_array_obj(csilk_json_t* obj, const char* key, csilk_json_t* item);

/**
 * @brief Add a string value under @p key to an object.
 * @param[in,out] obj Target object.
 * @param[in] key Object key (copied).
 * @param[in] value String value (copied).
 * @return true on success, false on failure.
 */
bool csilk_json_add_string(csilk_json_t* obj, const char* key, const char* value);

/**
 * @brief Add a double number under @p key to an object.
 * @param[in,out] obj Target object.
 * @param[in] key Object key (copied).
 * @param[in] value Numeric value.
 * @return true on success, false on failure.
 */
bool csilk_json_add_number(csilk_json_t* obj, const char* key, double value);

/**
 * @brief Add an integer under @p key to an object.
 * @param[in,out] obj Target object.
 * @param[in] key Object key (copied).
 * @param[in] value Integer value.
 * @return true on success, false on failure.
 */
bool csilk_json_add_int(csilk_json_t* obj, const char* key, int64_t value);

/**
 * @brief Add a boolean under @p key to an object.
 * @param[in,out] obj Target object.
 * @param[in] key Object key (copied).
 * @param[in] value Boolean value.
 * @return true on success, false on failure.
 */
bool csilk_json_add_bool(csilk_json_t* obj, const char* key, bool value);

/**
 * @brief Add a null value under @p key to an object.
 * @param[in,out] obj Target object.
 * @param[in] key Object key (copied).
 * @return true on success, false on failure.
 */
bool csilk_json_add_null(csilk_json_t* obj, const char* key);

/** @brief Append @p item to an object without a key.
 *  @param[in,out] obj Target object (takes ownership of @p item).
 *  @param[in] item Value to add; becomes owned by @p obj.
 *  @return true on success, false on failure.
 */
bool csilk_json_add_item(csilk_json_t* obj, csilk_json_t* item);

/* ====================================================================
 * Add to array
 * ==================================================================== */

/** @brief Append @p item to an array (array takes ownership of item).
 *  @param[in,out] arr Target array (takes ownership of @p item).
 *  @param[in] item Value to append; becomes owned by @p arr.
 *  @return true on success, false on failure.
 */
bool csilk_json_array_append(csilk_json_t* arr, csilk_json_t* item);

/* ====================================================================
 * Get / inspect
 * ==================================================================== */

/** @brief Get a child value by key, regardless of type.
 *  @param[in] obj Object to search.
 *  @param[in] key Key to look up.
 *  @return Child value, or NULL if not found.
 */
csilk_json_t* csilk_json_get(const csilk_json_t* obj, const char* key);

/** @brief Get a child object by key.
 *  @param[in] obj Object to search.
 *  @param[in] key Key to look up.
 *  @return Child object, or NULL if not found or not an object.
 */
csilk_json_t* csilk_json_get_object(const csilk_json_t* obj, const char* key);

/** @brief Get a child array by key.
 *  @param[in] obj Object to search.
 *  @param[in] key Key to look up.
 *  @return Child array, or NULL if not found or not an array.
 */
csilk_json_t* csilk_json_get_array(const csilk_json_t* obj, const char* key);

/** @brief Get a child string by key.
 *  @param[in] obj Object to search.
 *  @param[in] key Key to look up.
 *  @return Child string, or NULL if not found or not a string.
 */
const char* csilk_json_get_string(const csilk_json_t* obj, const char* key);

/** @brief Get a child number by key as double.
 *  @param[in] obj Object to search.
 *  @param[in] key Key to look up.
 *  @return Numeric value, or 0.0 if not found or not a number.
 */
double csilk_json_get_number(const csilk_json_t* obj, const char* key);

/** @brief Get a child integer by key.
 *  @param[in] obj Object to search.
 *  @param[in] key Key to look up.
 *  @return Integer value, or 0 if not an integer.
 */
int64_t csilk_json_get_int(const csilk_json_t* obj, const char* key);

/** @brief Get a child boolean by key.
 *  @param[in] obj Object to search.
 *  @param[in] key Key to look up.
 *  @return Boolean value, or false if not found or not a bool.
 */
bool csilk_json_get_bool(const csilk_json_t* obj, const char* key);

/** @brief Get the string value directly from a string node.
 *  @param[in] v String node.
 *  @return String contents, or NULL if not a string.
 */
const char* csilk_json_string_value(const csilk_json_t* v);

/** @brief Get the number value directly from a number node as double.
 *  @param[in] v Number node.
 *  @return Numeric value, or 0.0 if not a number.
 */
double csilk_json_number_value(const csilk_json_t* v);

/** @brief Get the integer value directly from a number node.
 *  @param[in] v Number node.
 *  @return Integer value, or 0 if not a number.
 */
int64_t csilk_json_int_value(const csilk_json_t* v);

/** @brief Get the boolean value directly from a bool node.
 *  @param[in] v Bool node.
 *  @return Boolean value.
 */
bool csilk_json_bool_value(const csilk_json_t* v);

/** @brief Get the N-th element of an array.
 *  @param[in] arr Array to index.
 *  @param[in] index Zero-based element index.
 *  @return Element at @p index, or NULL if out of bounds.
 */
csilk_json_t* csilk_json_array_get(const csilk_json_t* arr, size_t index);

/** @brief Return the number of elements in an array.
 *  @param[in] arr Array to measure.
 *  @return Element count.
 */
size_t csilk_json_array_size(const csilk_json_t* arr);

/* ====================================================================
 * Type predicates
 * ==================================================================== */

/** @brief Test whether a value is JSON null. @param[in] v Value. @return true if null. */
bool csilk_json_is_null(const csilk_json_t* v);
/** @brief Test whether a value is a JSON object. @param[in] v Value. @return true if object. */
bool csilk_json_is_object(const csilk_json_t* v);
/** @brief Test whether a value is a JSON array. @param[in] v Value. @return true if array. */
bool csilk_json_is_array(const csilk_json_t* v);
/** @brief Test whether a value is a JSON string. @param[in] v Value. @return true if string. */
bool csilk_json_is_string(const csilk_json_t* v);
/** @brief Test whether a value is a JSON number. @param[in] v Value. @return true if number. */
bool csilk_json_is_number(const csilk_json_t* v);
/** @brief Test whether a value is a JSON boolean. @param[in] v Value. @return true if bool. */
bool csilk_json_is_bool(const csilk_json_t* v);
/** @brief Test whether a value is JSON true. @param[in] v Value. @return true if true. */
bool csilk_json_is_true(const csilk_json_t* v);
/** @brief Test whether a value is JSON false. @param[in] v Value. @return true if false. */
bool csilk_json_is_false(const csilk_json_t* v);

/* ====================================================================
 * Parse
 * ==================================================================== */

/**
 * @brief Parse a null-terminated JSON string.
 * @param[in] json_str Null-terminated JSON text.
 * @return New csilk_json_t on success, NULL on failure.
 * Caller must free with csilk_json_free().
 */
csilk_json_t* csilk_json_parse(const char* json_str);

/**
 * @brief Parse a JSON string with explicit length (handles embedded NULs).
 * @param[in] json_str JSON text (may contain embedded NULs).
 * @param[in] len Length of @p json_str in bytes.
 * @return New csilk_json_t on success, NULL on failure.
 */
csilk_json_t* csilk_json_parse_len(const char* json_str, size_t len);

/**
 * @brief Parse with error output.
 * @param[in] json_str Null-terminated JSON text.
 * @param[out] error Static pointer describing the error, or NULL on success.
 * @return New csilk_json_t on success, NULL on failure.
 */
csilk_json_t* csilk_json_parse_err(const char* json_str, const char** error);

/* ====================================================================
 * Serialize
 * ==================================================================== */

/**
 * @brief Serialize to a compact JSON string (equivalent to cJSON_PrintUnformatted).
 * @param[in] v Value to serialize.
 * @param[out] len Optional pointer to receive string length (excl. NUL).
 * @return Heap-allocated NUL-terminated string. Caller must free().
 */
char* csilk_json_serialize(const csilk_json_t* v, size_t* len);

/**
 * @brief Serialize to a pretty-printed JSON string (equivalent to cJSON_Print).
 * @param[in] v Value to serialize.
 * @param[out] len Optional pointer to receive string length (excl. NUL).
 * @return Heap-allocated NUL-terminated string. Caller must free().
 */
char* csilk_json_serialize_pretty(const csilk_json_t* v, size_t* len);

/* ====================================================================
 * Free
 * ==================================================================== */

/**
 * @brief Free a csilk_json_t and all its children (equivalent to cJSON_Delete).
 * @param[in,out] v Value to free; may be NULL.
 */
void csilk_json_free(csilk_json_t* v);

/* ====================================================================
 * Copy
 * ==================================================================== */

/** @brief Deep-copy a JSON value into a new csilk_json_t. Caller owns the result.
 *  @param[in] v Value to copy.
 *  @return New independent copy, or NULL on failure.
 */
csilk_json_t* csilk_json_copy(const csilk_json_t* v);

/* ====================================================================
 * Key iteration (for walking object keys)
 * ==================================================================== */

/**
 * @brief Return the N-th key in an object.
 * @param[in] obj Object to inspect.
 * @param[in] index Zero-based key index.
 * @return Key string, or NULL if out of bounds. Valid until the json is freed.
 */
const char* csilk_json_object_key(const csilk_json_t* obj, size_t index);

/** @brief Return the number of keys in an object.
 *  @param[in] obj Object to inspect.
 *  @return Key count.
 */
size_t csilk_json_object_size(const csilk_json_t* obj);

/** @brief Get the value at the N-th key position.
 *  @param[in] obj Object to inspect.
 *  @param[in] index Zero-based key index.
 *  @return Value at @p index, or NULL if out of bounds.
 */
csilk_json_t* csilk_json_object_val(const csilk_json_t* obj, size_t index);

/* ====================================================================
 * Mutation (in-place update — requires copying for yyjson immutability)
 * ==================================================================== */

/**
 * @brief Replace the string value of an existing string node.
 * Because yyjson values are immutable, this creates a new string value
 * and replaces the key in the parent object.
 * @param[in,out] v String node whose value is replaced.
 * @param[in] new_value New string contents (copied).
 * @return true on success, false on failure.
 */
bool csilk_json_set_string(csilk_json_t* v, const char* new_value);

#ifdef __cplusplus
}
#endif

#endif /* CSILK_CORE_JSON_H */
