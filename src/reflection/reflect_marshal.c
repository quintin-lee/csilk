/**
 * @file reflect_marshal.c
 * @brief JSON marshalling (struct -> JSON) implementation.
 *
 * Split from reflect.c.  Contains serialize_scalar, struct_to_cjson_internal,
 * csilk_json_marshal, and csilk_json_marshal_arena.
 *
 * @copyright MIT License
 */

#include "reflect_internal.h"
#include "csilk/core/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration — struct_to_cjson_internal is mutually recursive with
 * serialize_scalar (nested struct fields trigger recursion). */
static void struct_to_cjson_internal(cJSON*                    obj,
                                     const void*               struct_ptr,
                                     const csilk_field_desc_t* descs,
                                     size_t                    field_count);

/** @brief Internal: serialize a single struct field value to a cJSON node.
 *
 * Maps C primitive types and nested structs to cJSON values based on the
 * field descriptor. Supports int8/16/32/64, uint8/16/32/64, float, double,
 * bool, string (fixed-size buffer or pointer), and nested struct types.
 * For nested structs, recursively calls struct_to_cjson_internal().
 *
 * @param addr Memory address of the field within the source struct.
 * @param desc Field descriptor specifying type, offset, and metadata.
 * @return cJSON node (owned by caller), or cJSON_CreateNull() on failure.
 * @note The caller must free the returned cJSON node with cJSON_Delete(). */
static cJSON*
serialize_scalar(const void* addr, const csilk_field_desc_t* desc)
{
    /*
   * Dispatch on field type to produce the matching cJSON node:
   *
   * Integer/float/double → cJSON_CreateNumber(double).  Note: int64/uint64
   * are cast to double, which loses precision for values > 2^53.  This is
   * a known limitation shared by all C JSON libraries using IEEE 754 doubles.
   *
   * Bool → cJSON_CreateBool().
   *
   * String → two modes via desc->is_pointer:
   *   - Pointer string: field stores a char*; dereference to get the string.
   *   - Fixed buffer: field IS the char array; cast addr directly.
   *   nullptr or empty → cJSON_CreateNull().
   *
   * Nested struct → look up the type's reflection entry by nested_type_name,
   *   create a fresh cJSON object, and recurse via struct_to_cjson_internal().
   *   If desc->is_pointer (struct*), dereference the pointer first.  Returns
   *   Null if the nested type is not registered or the pointer is nullptr.
   */
    switch (desc->type) {
    case CSILK_TYPE_INT8:
        return cJSON_CreateNumber(*(const int8_t*)addr);
    case CSILK_TYPE_UINT8:
        return cJSON_CreateNumber(*(const uint8_t*)addr);
    case CSILK_TYPE_INT16:
        return cJSON_CreateNumber(*(const int16_t*)addr);
    case CSILK_TYPE_UINT16:
        return cJSON_CreateNumber(*(const uint16_t*)addr);
    case CSILK_TYPE_INT32:
        return cJSON_CreateNumber(*(const int32_t*)addr);
    case CSILK_TYPE_UINT32:
        return cJSON_CreateNumber(*(const uint32_t*)addr);
    case CSILK_TYPE_INT64:
        return cJSON_CreateNumber((double)*(const int64_t*)addr);
    case CSILK_TYPE_UINT64:
        return cJSON_CreateNumber((double)*(const uint64_t*)addr);
    case CSILK_TYPE_FLOAT:
        return cJSON_CreateNumber(*(const float*)addr);
    case CSILK_TYPE_DOUBLE:
        return cJSON_CreateNumber(*(const double*)addr);
    case CSILK_TYPE_BOOL:
        return cJSON_CreateBool(*(const bool*)addr);
    case CSILK_TYPE_STRING: {
        const char* str = desc->is_pointer ? *(const char**)addr : (const char*)addr;
        return str ? cJSON_CreateString(str) : cJSON_CreateNull();
    }
    case CSILK_TYPE_STRUCT: {
        const void* struct_addr = desc->is_pointer ? *(const void**)addr : addr;
        if (!struct_addr) {
            return cJSON_CreateNull();
        }

        const csilk_reflect_entry_t* entry = csilk_reflect_find(desc->nested_type_name);
        if (!entry) {
            return cJSON_CreateNull();
        }

        cJSON* sub_obj = cJSON_CreateObject();
        if (!sub_obj) {
            return nullptr;
        }
        struct_to_cjson_internal(sub_obj, struct_addr, entry->fields, entry->count);
        return sub_obj;
    }
    }
    return cJSON_CreateNull();
}

/** @brief Internal: walk all fields of a struct and build a cJSON object.
 *
 * Iterates over the field descriptors, computes each field's address by
 * adding the offset to the struct pointer, and serializes each field to
 * a cJSON node added to the object. Array fields are serialized as cJSON
 * arrays (one element per array slot). Non-array fields use the field's
 * json_key as the object key.
 *
 * @param obj         Target cJSON object to populate.
 * @param struct_ptr  Pointer to the source struct (must not be nullptr).
 * @param descs       Array of field descriptors.
 * @param field_count Number of field descriptors. */
static void
struct_to_cjson_internal(cJSON*                    obj,
                         const void*               struct_ptr,
                         const csilk_field_desc_t* descs,
                         size_t                    field_count)
{
    /*
   * Walk every field descriptor for this struct type.  For each field:
   *
   * 1. Compute the field's memory address as: struct_base + compile-time
   *    offset.  The offset is stored in the field descriptor by the
   *    CSILK_REGISTER_REFLECT macro using offsetof().  This is purely
   *    arithmetic — no hash lookups or name resolution at runtime.
   *
   * 2. Array fields (array_length > 0): elements are laid out contiguously
   *    in memory with stride = desc->size (sizeof the element type, including
   *    padding).  Each element is serialized via serialize_scalar() and
   *    appended to a cJSON array.
   *
   * 3. Non-array fields: serialize directly and add to the cJSON object
   *    using the field's json_key as the property name.
   */
    for (size_t i = 0; i < field_count; i++) {
        const char* field_addr = (const char*)struct_ptr + descs[i].offset;

        if (descs[i].array_length > 0) {
            cJSON* arr = cJSON_CreateArray();
            if (!arr) {
                continue;
            }
            for (size_t j = 0; j < descs[i].array_length; j++) {
                const char* item_addr = field_addr + (j * descs[i].size);
                cJSON_AddItemToArray(arr, serialize_scalar(item_addr, &descs[i]));
            }
            cJSON_AddItemToObject(obj, descs[i].json_key, arr);
        } else {
            cJSON_AddItemToObject(obj, descs[i].json_key, serialize_scalar(field_addr, &descs[i]));
        }
    }
}

/** @brief Serialize a registered struct or basic type to a compact JSON string.
 */
char*
csilk_json_marshal(const char* type_name, const void* ptr)
{
    if (!type_name || !ptr) {
        return nullptr;
    }

    /*
   * Fast path for scalar types: if type_name matches a built-in primitive
   * (int8, uint8, ..., string, bool), serialize it directly without a
   * registry lookup.  This avoids the overhead of registering a reflection
   * entry for single-value responses.  Returns a compact (unformatted) JSON
   * string.
   */
    csilk_field_desc_t basic_desc;
    if (get_basic_type(type_name, &basic_desc)) {
        cJSON* node = serialize_scalar(ptr, &basic_desc);
        if (!node) {
            return nullptr;
        }
        char* out = cJSON_PrintUnformatted(node);
        cJSON_Delete(node);
        return out;
    }

    const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
    if (!entry) {
        return nullptr;
    }

    cJSON* root = cJSON_CreateObject();
    if (!root) {
        return nullptr;
    }

    struct_to_cjson_internal(root, ptr, entry->fields, entry->count);
    char* out = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return out;
}

char*
csilk_json_marshal_arena(csilk_arena_t* arena,
                         const char*    type_name,
                         const void*    ptr,
                         size_t*        out_len)
{
    char* str = csilk_json_marshal(type_name, ptr);
    if (!str) {
        return nullptr;
    }
    size_t len = strlen(str);
    char*  arena_str = nullptr;
    if (arena) {
        arena_str = csilk_arena_strndup(arena, str, len);
    } else {
        arena_str = strdup(str);
    }
    free(str);
    if (out_len) {
        *out_len = len;
    }
    return arena_str;
}
