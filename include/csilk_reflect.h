/**
 * @file csilk_reflect.h
 * @brief Csilk reflection and JSON binding engine.
 * MIT License
 */

#ifndef CSILK_REFLECT_H
#define CSILK_REFLECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "cJSON.h"

/** @brief Supported field types for reflection. */
typedef enum {
    CSILK_TYPE_INT8,   CSILK_TYPE_UINT8,
    CSILK_TYPE_INT16,  CSILK_TYPE_UINT16,
    CSILK_TYPE_INT32,  CSILK_TYPE_UINT32,
    CSILK_TYPE_INT64,  CSILK_TYPE_UINT64,
    CSILK_TYPE_FLOAT,  CSILK_TYPE_DOUBLE,
    CSILK_TYPE_BOOL,
    CSILK_TYPE_STRING, /**< Supports char[] or char* */
    CSILK_TYPE_STRUCT  /**< Supports nested structs or pointers to structs */
} csilk_field_type_t;

typedef struct csilk_field_desc_s csilk_field_desc_t;
struct csilk_field_desc_s {
    const char *json_key;               /**< JSON key name. */
    csilk_field_type_t type;            /**< Data type. */
    size_t offset;                      /**< Offset from struct start. */
    size_t size;                        /**< Size of one element. */
    size_t array_length;                /**< 0 for scalar, >0 for array. */
    bool is_pointer;                    /**< True if char* or Struct*. */
    const char *nested_type_name;       /**< Type name for nested structs (lazy lookup). */
};

/** @brief Type registration structure. */
typedef struct {
    const char *name;
    const csilk_field_desc_t *fields;
    size_t count;
} csilk_reflect_entry_t;

/** @brief Register a type manually. */
void csilk_reflect_register(const char *name, const csilk_field_desc_t *fields, size_t count);

/** @brief Find a registered type by name. */
const csilk_reflect_entry_t* csilk_reflect_find(const char *name);

/** @brief High-level: Struct to JSON string. */
char* csilk_json_marshal(const char *type_name, const void *ptr);

/** @brief High-level: JSON string to Struct. */
int csilk_json_unmarshal(const char *type_name, const char *json_str, void *ptr);

/* --- Automatic Type Dispatch (C11 _Generic) --- */

/** @brief User-extensible type map. Define this before including csilk_reflect.h to add types. */
#ifndef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP
#endif

#define csilk_type_name(x) _Generic((x), \
    char*: "string", \
    const char*: "string" \
    CSILK_USER_TYPE_MAP \
    , default: "unknown" \
)

#define csilk_marshal(ptr) csilk_json_marshal(csilk_type_name(*(ptr)), ptr)
#define csilk_unmarshal(json, ptr) csilk_json_unmarshal(csilk_type_name(*(ptr)), json, ptr)

/* --- Macro Magic for Automatic Registration --- */

#define CSILK_META_EXPAND(struct_type, field, type_enum, size, arr_len, is_ptr, nested_name) \
    { #field, type_enum, offsetof(struct_type, field), size, arr_len, is_ptr, nested_name },

/** @brief Automatically register a reflectable struct at startup. */
#define CSILK_REGISTER_REFLECT(struct_type, map_macro) \
    static csilk_field_desc_t struct_type##_meta[] = { \
        map_macro(CSILK_META_EXPAND) \
        {NULL, 0, 0, 0, 0, false, NULL} \
    }; \
    static void __attribute__((constructor)) auto_reg_##struct_type(void) { \
        size_t count = (sizeof(struct_type##_meta) / sizeof(csilk_field_desc_t)) - 1; \
        csilk_reflect_register(#struct_type, struct_type##_meta, count); \
    }

#endif
