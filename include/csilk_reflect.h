/**
 * @file csilk_reflect.h
 * @brief Csilk reflection and JSON binding engine.
 * @copyright MIT License
 */

#ifndef CSILK_REFLECT_H
#define CSILK_REFLECT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/** @brief Supported field types for reflection. */
typedef enum {
  CSILK_TYPE_INT8,   /**< 8-bit signed integer */
  CSILK_TYPE_UINT8,  /**< 8-bit unsigned integer */
  CSILK_TYPE_INT16,  /**< 16-bit signed integer */
  CSILK_TYPE_UINT16, /**< 16-bit unsigned integer */
  CSILK_TYPE_INT32,  /**< 32-bit signed integer */
  CSILK_TYPE_UINT32, /**< 32-bit unsigned integer */
  CSILK_TYPE_INT64,  /**< 64-bit signed integer */
  CSILK_TYPE_UINT64, /**< 64-bit unsigned integer */
  CSILK_TYPE_FLOAT,  /**< Single-precision floating point */
  CSILK_TYPE_DOUBLE, /**< Double-precision floating point */
  CSILK_TYPE_BOOL,   /**< Boolean value */
  CSILK_TYPE_STRING, /**< Supports char[] or char* */
  CSILK_TYPE_STRUCT  /**< Supports nested structs or pointers to structs */
} csilk_field_type_t;

/** @brief Forward declaration for the field descriptor struct. */
typedef struct csilk_field_desc_s csilk_field_desc_t;
/** @brief Descriptor for a single struct field in the reflection system. */
struct csilk_field_desc_s {
  const char* json_key;    /**< JSON key name. */
  csilk_field_type_t type; /**< Data type. */
  size_t offset;           /**< Offset from struct start. */
  size_t size;             /**< Size of one element. */
  size_t array_length;     /**< 0 for scalar, >0 for array. */
  bool is_pointer;         /**< True if char* or Struct*. */
  const char*
      nested_type_name; /**< Type name for nested structs (lazy lookup). */
};

/** @brief Type registration structure. */
typedef struct {
  const char* name;                 /**< Type name string. */
  const csilk_field_desc_t* fields; /**< Array of field descriptors. */
  size_t count;                     /**< Number of fields in the array. */
} csilk_reflect_entry_t;

/** @brief Initialize the reflection system (mutexes, etc). Safe to call
 * multiple times. */
void csilk_reflect_init(void);

/** @brief Register a type manually. */
void csilk_reflect_register(const char* name, const csilk_field_desc_t* fields,
                            size_t count);

/** @brief Find a registered type by name. */
const csilk_reflect_entry_t* csilk_reflect_find(const char* name);

/** @brief High-level: Struct to JSON string. */
char* csilk_json_marshal(const char* type_name, const void* ptr);

/** @brief High-level: JSON string to Struct. */
int csilk_json_unmarshal(const char* type_name, const char* json_str,
                         void* ptr);

/* --- Automatic Type Dispatch (C11 _Generic) --- */

/** @brief User-extensible type map. Define this before including
 * csilk_reflect.h to add types. */
#ifndef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP
#endif

/** @brief Map a C type to its reflected string name using _Generic.
 *  Extensible via CSILK_USER_TYPE_MAP.
 *  @param x Expression whose type determines the returned string. */
#define csilk_type_name(x)                       \
  _Generic((x),                                  \
      char*: "string",                           \
      const char*: "string" CSILK_USER_TYPE_MAP, \
      default: "unknown")

/** @brief Marshal (serialize) a reflected struct to a JSON string.
 *  Automatically deduces the type name via csilk_type_name.
 *  @param ptr Pointer to the struct instance.
 *  @return A JSON string allocated with malloc, or NULL on error. Caller must
 * free. */
#define csilk_marshal(ptr) csilk_json_marshal(csilk_type_name(*(ptr)), ptr)
/** @brief Unmarshal (deserialize) a JSON string into a reflected struct.
 *  Automatically deduces the type name via csilk_type_name.
 *  @param json The JSON string to parse.
 *  @param ptr Pointer to the struct instance.
 *  @return 0 on success, -1 on error. */
#define csilk_unmarshal(json, ptr) \
  csilk_json_unmarshal(csilk_type_name(*(ptr)), json, ptr)

/* --- Macro Magic for Automatic Registration --- */

/** @brief Internal helper macro to expand a field descriptor entry.
 *  Used by CSILK_REGISTER_REFLECT to produce initializer elements.
 *  @param struct_type The struct type name.
 *  @param field The field name.
 *  @param type_enum The csilk_field_type_t enum value.
 *  @param size Size of one element.
 *  @param arr_len Array length (0 for scalar).
 *  @param is_ptr Whether the field is a pointer.
 *  @param nested_name Nested type name for CSILK_TYPE_STRUCT fields. */
#define CSILK_META_EXPAND(struct_type, field, type_enum, size, arr_len, \
                          is_ptr, nested_name)                          \
  {#field, type_enum,  offsetof(struct_type, field), size, arr_len,     \
   is_ptr, nested_name},

/** @brief Automatically register a reflectable struct at startup. */
#define CSILK_REGISTER_REFLECT(struct_type, map_macro)                    \
  static csilk_field_desc_t struct_type##_meta[] = {                      \
      map_macro(CSILK_META_EXPAND){NULL, 0, 0, 0, 0, false, NULL}};       \
  static void __attribute__((constructor)) auto_reg_##struct_type(void) { \
    size_t count =                                                        \
        (sizeof(struct_type##_meta) / sizeof(csilk_field_desc_t)) - 1;    \
    csilk_reflect_register(#struct_type, struct_type##_meta, count);      \
  }

#endif
