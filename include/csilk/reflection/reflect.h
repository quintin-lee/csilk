/**
 * @file csilk_reflect.h
 * @brief Csilk compile-time reflection and JSON binding engine.
 *
 * Provides macros and functions to register C struct layouts at build time
 * and to marshal/unmarshal them to/from JSON automatically.  Uses C11
 * _Generic for type dispatch and GCC constructor attributes for
 * self-registration.
 *
 * @copyright MIT License
 */

#ifndef CSILK_REFLECT_H
#define CSILK_REFLECT_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

/**
 * @brief Supported C data types for struct field reflection.
 *
 * Each enumerator corresponds to a C type that the reflection system can
 * read/write when marshalling to or from JSON.
 */
typedef enum {
    CSILK_TYPE_INT8,   /**< 8-bit signed integer (int8_t / char). */
    CSILK_TYPE_UINT8,  /**< 8-bit unsigned integer (uint8_t / unsigned char). */
    CSILK_TYPE_INT16,  /**< 16-bit signed integer (int16_t / short). */
    CSILK_TYPE_UINT16, /**< 16-bit unsigned integer (uint16_t / unsigned short).
                      */
    CSILK_TYPE_INT32,  /**< 32-bit signed integer (int32_t / int). */
    CSILK_TYPE_UINT32, /**< 32-bit unsigned integer (uint32_t / unsigned int). */
    CSILK_TYPE_INT64,  /**< 64-bit signed integer (int64_t / long long). */
    CSILK_TYPE_UINT64, /**< 64-bit unsigned integer (uint64_t / unsigned long
                        long). */
    CSILK_TYPE_FLOAT,  /**< Single-precision IEEE 754 float. */
    CSILK_TYPE_DOUBLE, /**< Double-precision IEEE 754 double. */
    CSILK_TYPE_BOOL,   /**< Boolean (C99 _Bool / bool). */
    CSILK_TYPE_STRING, /**< String: supports both char[] fixed buffers and char*
                        pointers. */
    CSILK_TYPE_STRUCT  /**< Nested struct (by value or pointer).  @p
                        nested_type_name identifies the type. */
} csilk_field_type_t;

/** @brief Maximum number of struct types that can be registered in the
 * reflection registry. */
enum { MAX_REG_STRUCTS = 256 };

/** @brief Forward declaration for the field descriptor struct. */
typedef struct csilk_field_desc_s csilk_field_desc_t;

/**
 * @brief Descriptor for a single field in a reflected struct.
 *
 * Each field in a registered struct produces one of these descriptors,
 * typically via the CSILK_META_EXPAND macro.  The array of descriptors
 * is nullptr-terminated (sentinel entry with all-zero fields).
 *
 * During marshalling, the engine walks the field descriptor array, reads
 * @p offset bytes from the struct base, and converts the value according
 * to @p type.  During unmarshalling, JSON values are type-checked and
 * written to the same offset.  Nested structs (CSILK_TYPE_STRUCT) are
 * resolved lazily by name at marshal/unmarshal time, allowing forward
 * references.
 */
struct csilk_field_desc_s {
    const char*        json_key;     /**< JSON key name for this field (used during
                           marshal/unmarshal; e.g., "user_name"). */
    csilk_field_type_t type;         /**< Data type enumerator (see csilk_field_type_t). */
    size_t             offset;       /**< Byte offset of this field from the struct base address
                    (computed via offsetof). */
    size_t             size;         /**< Size in bytes of one element (sizeof(field_type)).  For
                    arrays this is the element size, not the total. */
    size_t             array_length; /**< Number of elements for fixed-size C arrays (0 =
                          scalar fields or pointer fields). */
    bool               is_pointer;   /**< True if the field is a pointer type (char* or struct
                          pointer).  Affects how the field is read/written. */
    const char*        nested_type_name; /**< For CSILK_TYPE_STRUCT fields, the registered type
                           name of the nested struct (resolved lazily at
                           marshalling time to support forward declarations).
                           nullptr for non-struct fields. */
};

/**
 * @brief Registration entry for a reflected type.
 *
 * Stored in an internal hash map.  Populated via csilk_reflect_register
 * or automatically via the CSILK_REGISTER_REFLECT macro.
 */
typedef struct {
    const char*               name;   /**< Unique type name string (e.g., "User", "Config"). */
    const csilk_field_desc_t* fields; /**< nullptr-terminated array of field descriptors. */
    size_t                    count; /**< Number of valid field descriptors (excluding sentinel). */
} csilk_reflect_entry_t;

/**
 * @brief Initialise the reflection subsystem.
 *
 * Sets up internal data structures (mutexes for thread-safe registration).
 * Safe to call multiple times — subsequent calls are no-ops.
 */
void csilk_reflect_init(void);

/**
 * @brief Manually register a struct type for reflection.
 *
 * Types registered with this function are immediately available for
 * marshal/unmarshal operations.  The @p name must be unique.
 *
 * @param name    Type name string (must remain valid for the lifetime of
 *                the program — typically a string literal).
 * @param fields  nullptr-terminated array of csilk_field_desc_t.
 * @param count   Number of valid entries in @p fields (excluding the
 *                nullptr-sentinel terminator).
 */
void csilk_reflect_register(const char* name, const csilk_field_desc_t* fields, size_t count);

/**
 * @brief Look up a registered type by name.
 *
 * @param name  Type name string.
 * @return Pointer to the csilk_reflect_entry_t, or nullptr if @p name has not
 *         been registered.
 */
const csilk_reflect_entry_t* csilk_reflect_find(const char* name);

/**
 * @brief Callback type for csilk_reflect_foreach.
 *
 * Invoked once per registered type.
 *
 * @param name      The registered type name.
 * @param entry     The type descriptor entry.
 * @param user_data Opaque pointer forwarded from csilk_reflect_foreach.
 */
typedef void (*csilk_reflect_foreach_cb)(const char*                  name,
                                         const csilk_reflect_entry_t* entry,
                                         void*                        user_data);

/**
 * @brief Iterate over all registered reflection types.
 *
 * Calls @p cb for each type in the registration table.  Safe to call at
 * any point — types registered via CSILK_REGISTER_REFLECT are available
 * via GCC constructor functions that run before main().
 *
 * @param cb        Callback invoked for each registered type (must not be
 * nullptr).
 * @param user_data Opaque pointer forwarded to every @p cb invocation.
 */
void csilk_reflect_foreach(csilk_reflect_foreach_cb cb, void* user_data);

/**
 * @brief Serialise a reflected struct to a JSON string.
 *
 * Walks the field descriptors for @p type_name and produces a compact
 * JSON string (no extra whitespace).  String fields are properly escaped.
 *
 * @param type_name  Registered type name of the struct.
 * @param ptr        Pointer to the struct instance to serialise.
 * @return A heap-allocated NUL-terminated JSON string (caller must free
 *         with free()), or nullptr on error.
 */
char* csilk_json_marshal(const char* type_name, const void* ptr);

/**
 * @brief Deserialise a JSON string into a reflected struct.
 *
 * Parses the JSON and populates the struct fields according to the
 * registered field descriptors.  Numeric type checking and range clamping
 * are performed where possible.
 *
 * @param type_name  Registered type name of the target struct.
 * @param  json_str   NUL-terminated JSON string to parse.
 * @param[out] ptr   Pointer to the struct instance to populate (must
 *                   already be allocated and zero-initialised).
 * @return 1 on success, 0 on parse error or type mismatch.
 */
int csilk_json_unmarshal(const char* type_name, const char* json_str, void* ptr);

/**
 * @brief Recursively free heap-allocated memory of a reflected struct's fields.
 *
 * Traverses the fields of the struct specified by @p type_name at address @p ptr
 * according to its registered reflection metadata. Dynamically allocated memory
 * (e.g. pointer fields containing strings or nested structs) is recursively freed.
 * Note that the top-level pointer @p ptr itself is NOT freed; the caller must
 * free it if it was dynamically allocated.
 *
 * @param type_name  Registered type name of the struct.
 * @param ptr        Pointer to the struct instance to free.
 */
void csilk_struct_free_reflect(const char* type_name, void* ptr);

/* --- Automatic Type Dispatch (C11 _Generic) --- */

/**
 * @brief User-extensible type-name mapping.
 *
 * Define CSILK_USER_TYPE_MAP before including csilk_reflect.h to add custom
 * type-to-string mappings via _Generic.  The default map handles char* and
 * const char* as "string".
 *
 * @code
 *   #define CSILK_USER_TYPE_MAP \
 *       int: "int32",           \
 *       double: "double"
 * @endcode
 */
#ifndef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP
#endif

/**
 * @brief Map a C expression's type to its reflected string name.
 *
 * Uses C11 _Generic dispatch.  Extend with CSILK_USER_TYPE_MAP for
 * user-defined types.
 *
 * @param x  Expression whose static type determines the returned name.
 * @return A string literal naming the type (e.g., "string", "int32").
 */
#define csilk_type_name(x)                                                                         \
    _Generic((x),                                                                                  \
        _Bool: "bool",                                                                             \
        signed char: "int8",                                                                       \
        unsigned char: "uint8",                                                                    \
        short: "int16",                                                                            \
        unsigned short: "uint16",                                                                  \
        int: "int32",                                                                              \
        unsigned int: "uint32",                                                                    \
        long: "int64",                                                                             \
        unsigned long: "uint64",                                                                   \
        long long: "int64",                                                                        \
        unsigned long long: "uint64",                                                              \
        float: "float",                                                                            \
        double: "double",                                                                          \
        char*: "string",                                                                           \
        const char*: "string" CSILK_USER_TYPE_MAP,                                                 \
        default: "unknown")

/**
 * @brief Convenience macro to serialise a reflected struct to a JSON string.
 *
 * Automatically deduces the type name via csilk_type_name(*(ptr)).
 *
 * @param ptr  Pointer to a registered struct instance.
 * @return A heap-allocated JSON string (caller must free), or nullptr on error.
 */
#define csilk_marshal(ptr) csilk_json_marshal(csilk_type_name(*(ptr)), ptr)

/**
 * @brief Convenience macro to deserialise a JSON string into a reflected
 * struct.
 *
 * Automatically deduces the type name via csilk_type_name(*(ptr)).
 *
 * @param json  NUL-terminated JSON string.
 * @param ptr   Pointer to a registered struct instance (must be pre-allocated).
 * @return 1 on success, 0 on failure.
 */
#define csilk_unmarshal(json, ptr) csilk_json_unmarshal(csilk_type_name(*(ptr)), json, ptr)

/* --- Macro Magic for Automatic Registration --- */

/**
 * @brief Internal helper: expand a single field into a csilk_field_desc_t
 *        initialiser.
 *
 * Used by the map macro passed to CSILK_REGISTER_REFLECT.  Each invocation
 * produces one array element.
 *
 * @param struct_type  The owning struct type name.
 * @param field        The field name.
 * @param type_enum    csilk_field_type_t enum value for this field.
 * @param size         sizeof() the field's type.
 * @param arr_len      Array element count (0 for scalar/pointer).
 * @param is_ptr       Non-zero if the field is a pointer type.
 * @param nested_name  Registered type name for CSILK_TYPE_STRUCT fields
 *                     (ignored for other types).
 */
#define CSILK_META_EXPAND(struct_type, field, type_enum, size, arr_len, is_ptr, nested_name)       \
    {#field, type_enum, offsetof(struct_type, field), size, arr_len, is_ptr, nested_name},

/**
 * @brief Automatically register a struct for reflection at program startup.
 *
 * Generates a static array of csilk_field_desc_t from the @p map_macro and
 * registers it via a GCC constructor function (runs before main()).  This
 * means no explicit initialisation call is needed — types are available
 * as soon as the program starts.
 *
 * @code
 *   // Define a struct
 *   typedef struct { int32_t id; char* name; } User;
 *
 *   // Map its fields (one _() invocation per field)
 *   #define USER_MAP(_) \
 *       _(User, id, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, nullptr) \
 *       _(User, name, CSILK_TYPE_STRING, sizeof(char*), 0, true, nullptr)
 *
 *   // Auto-register (this macro invocation must appear at file scope)
 *   CSILK_REGISTER_REFLECT(User, USER_MAP)
 * @endcode
 *
 * @param struct_type  The struct type name (used as the registration key
 *                     in the reflection hash map and for generating internal
 *                     symbol names).
 * @param map_macro    A macro that applies CSILK_META_EXPAND to each field.
 *                     Must produce exactly one CSILK_META_EXPAND(struct_type,
 *                     field, type_enum, size, arr_len, is_ptr, nested_name)
 *                     call per field.
 */
#define CSILK_REGISTER_REFLECT(struct_type, map_macro)                                             \
    static csilk_field_desc_t struct_type##_meta[] = {                                             \
        map_macro(CSILK_META_EXPAND){nullptr, 0, 0, 0, 0, false, nullptr} \
    };                        \
    static void __attribute__((constructor)) auto_reg_##struct_type(void)                          \
    {                                                                                              \
        size_t count = (sizeof(struct_type##_meta) / sizeof(csilk_field_desc_t)) - 1;              \
        csilk_reflect_register(#struct_type, struct_type##_meta, count);                           \
    }

#endif
