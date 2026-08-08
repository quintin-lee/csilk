/**
 * @file reflect_unmarshal.c
 * @brief JSON unmarshalling (JSON -> struct) implementation.
 *
 * Split from reflect.c.  Contains deserialize_scalar, cjson_to_struct_internal,
 * and csilk_json_unmarshal.
 *
 * @copyright MIT License
 */

#include "reflect_internal.h"
#include "csilk/core/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration — cjson_to_struct_internal is mutually recursive with
 * deserialize_scalar (nested struct fields trigger recursion). */
static void cjson_to_struct_internal(const cJSON*              obj,
                                     void*                     struct_ptr,
                                     const csilk_field_desc_t* descs,
                                     size_t                    field_count);

/** @brief Internal: deserialize a cJSON value into a single struct field.
 *
 * Maps cJSON types back to C primitives based on the field descriptor.
 * For CSILK_TYPE_STRING, handles both fixed-size buffers (strncpy) and
 * pointer fields (malloc + copy). For CSILK_TYPE_STRUCT, recursively
 * calls cjson_to_struct_internal(). Null JSON values or missing items
 * cause the field to be skipped (left at its current value).
 *
 * @param item Source cJSON node (may be nullptr or Null).
 * @param addr Memory address of the target field.
 * @param desc Field descriptor with type, size, and pointer flag.
 * @note For pointer string fields, any existing allocation is freed before
 *       the new value is assigned. */
static void
deserialize_scalar(const cJSON* item, void* addr, const csilk_field_desc_t* desc)
{
    /*
   * Skip null/missing JSON values — the field retains whatever it currently
   * holds (typically zero-initialized from calloc).
   *
   * Type coercion from cJSON to C primitives:
   *   - Integer types (int8-uint32): read from item->valueint.
   *   - Int64/uint64/float/double: read from item->valuedouble to capture
   *     full numeric range (accepts potential precision loss for i64).
   *   - Bool: cJSON_IsTrue() handles both True and False.
   *   - String, pointer mode (desc->is_pointer): free existing allocation
   *     before malloc + memcpy of new value (avoids leaks on re-parse).
   *   - String, buffer mode: snprintf with desc->size limit + automatic
   *     null-termination (prevents buffer overrun).
   *   - Nested struct: if pointer field and nil, auto-allocate with
   *     calloc(1, desc->size), then recurse via cjson_to_struct_internal().
   */
    if (!item || cJSON_IsNull(item)) {
        return;
    }

    switch (desc->type) {
    case CSILK_TYPE_INT8:
        *(int8_t*)addr = (int8_t)item->valueint;
        break;
    case CSILK_TYPE_UINT8:
        *(uint8_t*)addr = (uint8_t)item->valueint;
        break;
    case CSILK_TYPE_INT16:
        *(int16_t*)addr = (int16_t)item->valueint;
        break;
    case CSILK_TYPE_UINT16:
        *(uint16_t*)addr = (uint16_t)item->valueint;
        break;
    case CSILK_TYPE_INT32:
        *(int32_t*)addr = (int32_t)item->valueint;
        break;
    case CSILK_TYPE_UINT32:
        *(uint32_t*)addr = (uint32_t)item->valueint;
        break;
    case CSILK_TYPE_INT64:
        *(int64_t*)addr = (int64_t)item->valuedouble;
        break;
    case CSILK_TYPE_UINT64:
        *(uint64_t*)addr = (uint64_t)item->valuedouble;
        break;
    case CSILK_TYPE_FLOAT:
        *(float*)addr = (float)item->valuedouble;
        break;
    case CSILK_TYPE_DOUBLE:
        *(double*)addr = item->valuedouble;
        break;
    case CSILK_TYPE_BOOL:
        *(bool*)addr = cJSON_IsTrue(item);
        break;
    case CSILK_TYPE_STRING:
        if (cJSON_IsString(item) && item->valuestring) {
            if (desc->is_pointer) {
                char** ptr = (char**)addr;
                if (*ptr) {
                    free(*ptr);
                }
                size_t len = strlen(item->valuestring) + 1;
                *ptr = (char*)malloc(len);
                if (*ptr) {
                    memcpy(*ptr, item->valuestring, len);
                }
            } else {
                snprintf((char*)addr, desc->size, "%s", item->valuestring);
            }
        }
        break;
    case CSILK_TYPE_STRUCT:
        if (cJSON_IsObject(item)) {
            void* struct_addr = addr;
            if (desc->is_pointer) {
                void** ptr = (void**)addr;
                if (!*ptr) {
                    *ptr = calloc(1, desc->size);
                }
                struct_addr = *ptr;
            }
            if (struct_addr) {
                const csilk_reflect_entry_t* entry = csilk_reflect_find(desc->nested_type_name);
                if (entry) {
                    cjson_to_struct_internal(item, struct_addr, entry->fields, entry->count);
                }
            }
        }
        break;
    }
}

/** @brief Internal: walk a cJSON object and populate a struct's fields.
 *
 * For each field descriptor, looks up the matching JSON key in the cJSON
 * object (case-sensitive using cJSON_GetObjectItemCaseSensitive).
 * Array fields limit iteration to min(json_array_size, array_length).
 * Non-matching keys are silently ignored.
 *
 * @param obj         Source cJSON object.
 * @param struct_ptr  Pointer to the target struct.
 * @param descs       Array of field descriptors.
 * @param field_count Number of field descriptors. */
static void
cjson_to_struct_internal(const cJSON*              obj,
                         void*                     struct_ptr,
                         const csilk_field_desc_t* descs,
                         size_t                    field_count)
{
    /*
   * Walk all field descriptors and match each against a JSON key in the
   * parsed object.  cJSON_GetObjectItemCaseSensitive() does a string-key
   * lookup (O(n) in the number of keys for each call).  Keys not present
   * in the JSON are silently skipped — the struct field retains its
   * current value (safe for partial updates).
   *
   * Array fields: iterate up to min(json_array_length, array_length)
   * elements to stay within the C buffer's bounds.  Each element is
   * deserialized at offset field_addr + j * desc->size, matching the
   * contiguous array layout in memory.
   */
    for (size_t i = 0; i < field_count; i++) {
        cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, descs[i].json_key);
        if (!item) {
            continue;
        }

        char* field_addr = (char*)struct_ptr + descs[i].offset;

        if (descs[i].array_length > 0) {
            if (!cJSON_IsArray(item)) {
                continue;
            }
            size_t arr_size = cJSON_GetArraySize(item);
            size_t limit = (arr_size < descs[i].array_length) ? arr_size : descs[i].array_length;

            for (size_t j = 0; j < limit; j++) {
                char* item_addr = field_addr + (j * descs[i].size);
                deserialize_scalar(cJSON_GetArrayItem(item, j), item_addr, &descs[i]);
            }
        } else {
            deserialize_scalar(item, field_addr, &descs[i]);
        }
    }
}

/** @brief Deserialize a JSON string into a registered struct or basic type
 * instance. */
int
csilk_json_unmarshal(const char* type_name, const char* json_str, void* ptr)
{
    if (!type_name || !json_str || !ptr) {
        return 0;
    }

    /*
   * Fast path for scalar types: matches the fast path in marshal — parse
   * the JSON string directly to a single cJSON node and deserialize it
   * into the output pointer without a registry lookup.  Returns 1 on
   * success (even if the JSON value was null, which is silently skipped).
   */
    csilk_field_desc_t basic_desc;
    if (get_basic_type(type_name, &basic_desc)) {
        cJSON* root = cJSON_Parse(json_str);
        if (!root) {
            return 0;
        }
        deserialize_scalar(root, ptr, &basic_desc);
        cJSON_Delete(root);
        return 1;
    }

    const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
    if (!entry) {
        return 0;
    }

    cJSON* root = cJSON_Parse(json_str);
    if (!root) {
        return 0;
    }

    cjson_to_struct_internal(root, ptr, entry->fields, entry->count);
    cJSON_Delete(root);
    return 1;
}
