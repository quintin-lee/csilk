/**
 * @file reflect.c
 * @brief Reflection and JSON binding implementation.
 *
 * Architecture:
 *   The reflection system provides runtime type introspection via a global
 *   registry of struct descriptors.  Each registered type stores an array of
 *   csilk_field_desc_t entries with field name, type, offset, size, and
 *   metadata (is_pointer, array_length, nested_type_name).  The registry is
 *   a fixed-size array (256 entries) protected by a mutex for thread safety.
 *
 *   JSON marshalling (struct → JSON, csilk_json_marshal):
 *     Walk each field descriptor, compute the field's address by adding the
 *     compile-time offset to the struct base pointer, and serialize the value
 *     to a cJSON node.  Nested structs trigger recursive serialization via
 *     a registry lookup of the nested type.  Arrays serialize each element
 *     by stride (field->size) into a cJSON array.
 *
 *   JSON unmarshalling (JSON → struct, csilk_json_unmarshal):
 *     Parse the JSON string with cJSON_Parse, then for each field descriptor,
 *     look up the matching JSON key and deserialize the cJSON value into the
 *     field's memory address.  String fields support both fixed-size buffers
 *     (strncpy) and heap-allocated pointers (malloc + copy).  Nested structs
 *     are recursively deserialized with optional auto-allocation for pointer
 *     fields.
 *
 *   Both marshal and unmarshal have a fast path for basic scalar types
 *   (int, float, bool, string) — if type_name matches a known primitive,
 *   serialization bypasses the registry entirely.
 *
 * Thread safety:
 *   All public functions lock the registry mutex for read/write access.
 *   csilk_reflect_foreach() uses two-phase iteration (collect names under
 *   lock, invoke callbacks outside lock) to avoid deadlock when callbacks
 *   re-enter the reflection API (e.g., add_schema() in swagger.c).
 *
 * @copyright MIT License
 */

#include "csilk/reflection/reflect.h"
#include "csilk/core/server.h"

#include <stdio.h>
#include "csilk/core/sync.h"
#include <stdlib.h>
#include <string.h>
#include <csilk/core/sys_io.h>

static csilk_reflect_entry_t g_registry[MAX_REG_STRUCTS];
static size_t                g_registry_count = 0;
static csilk_mutex_t         g_registry_mutex;
static int                   g_registry_mutex_init = 0;

/** @brief Initialize the reflection system (called once at startup).
 *
 * Creates the global registry mutex. Idempotent — safe to call multiple times.
 * Automatically called by csilk_server_new() and other entry points.
 *
 * @note The reflection registry is a global array of up to MAX_REG_STRUCTS
 * (256) entries protected by a mutex. All public reflection functions are
 *       thread-safe. */
void
csilk_reflect_init(void)
{
    if (!g_registry_mutex_init) {
        csilk_mutex_init(&g_registry_mutex);
        g_registry_mutex_init = 1;
    }
}

/** @brief Internal: acquire the global reflection registry mutex.
 *
 * Initializes the mutex on first call if not yet initialized.
 * Blocks until the lock is acquired. */
static void
registry_lock(void)
{
    if (!g_registry_mutex_init) {
        csilk_reflect_init();
    }
    csilk_mutex_lock(&g_registry_mutex);
}

/** @brief Internal: unlock the global reflection registry mutex.
 *
 * Must be called after registry_lock() to release the lock around the
 * registered types table. */
static void
registry_unlock(void)
{
    csilk_mutex_unlock(&g_registry_mutex);
}

static const csilk_reflect_entry_t*
find_entry_unlocked(const char* name)
{
    if (!name) {
        return NULL;
    }
    for (size_t i = 0; i < g_registry_count; i++) {
        if (strcmp(g_registry[i].name, name) == 0) {
            return &g_registry[i];
        }
    }
    return NULL;
}

static int
detect_cycle_dfs(const char* type_name, const char** stack, size_t stack_depth)
{
    for (size_t i = 0; i < stack_depth; i++) {
        if (strcmp(stack[i], type_name) == 0) {
            return 1;
        }
    }

    if (stack_depth >= 32) {
        return 0;
    }

    const csilk_reflect_entry_t* entry = find_entry_unlocked(type_name);
    if (!entry) {
        return 0;
    }

    stack[stack_depth] = type_name;

    for (size_t i = 0; i < entry->count; i++) {
        if (entry->fields[i].type == CSILK_TYPE_STRUCT && entry->fields[i].nested_type_name) {
            if (detect_cycle_dfs(entry->fields[i].nested_type_name, stack, stack_depth + 1)) {
                return 1;
            }
        }
    }

    return 0;
}

/** @brief Register a struct type with the reflection engine.
 *
 * Adds a type name and its field descriptors to the global registry. Once
 * registered, the type can be serialized/deserialized to/from JSON via
 * csilk_json_marshal() / csilk_json_unmarshal(). The CSILK_REGISTER_REFLECT()
 * macro generates the field array and calls this function automatically.
 *
 * @param name   Type name string (e.g., "my_request_t"). Must remain valid
 *               for the lifetime of the registration.
 * @param fields Array of csilk_field_desc_t describing each struct field.
 * @param count  Number of fields in the array.
 * @note Thread-safe. If the registry is full (256 types), the registration
 *       is silently dropped. Types registered first take precedence. */
void
csilk_reflect_register(const char* name, const csilk_field_desc_t* fields, size_t count)
{
    registry_lock();
    if (g_registry_count < MAX_REG_STRUCTS) {
        g_registry[g_registry_count].name = name;
        g_registry[g_registry_count].fields = fields;
        g_registry[g_registry_count].count = count;
        g_registry_count++;

        const char* stack[32];
        if (detect_cycle_dfs(name, stack, 0)) {
            fprintf(
                stderr, "WARNING: Circular reference detected in registered type '%s'!\n", name);
        }
    }
    registry_unlock();
}

/** @brief Look up a registered type descriptor by name.
 *
 * Searches the global registry for a type matching @p name.
 *
 * @param name Type name to find (case-sensitive).
 * @return Pointer to the type's reflection entry, or NULL if not found.
 * @note Thread-safe. The returned pointer is valid for the lifetime of the
 *       registration. */
const csilk_reflect_entry_t*
csilk_reflect_find(const char* name)
{
    if (!name) {
        return NULL;
    }
    registry_lock();
    for (size_t i = 0; i < g_registry_count; i++) {
        if (strcmp(g_registry[i].name, name) == 0) {
            registry_unlock();
            return &g_registry[i];
        }
    }
    registry_unlock();
    return NULL;
}

/** @brief Iterate over all registered reflection types and invoke a callback
 * for each.
 *
 * Collects type names into a temporary array while holding the registry lock,
 * then releases the lock and invokes the callback for each name. This two-phase
 * approach avoids deadlocks when the callback itself calls back into reflection
 * APIs (e.g., csilk_reflect_find()).
 *
 * @param cb        Callback invoked once per registered type.
 * @param user_data Opaque pointer passed through to the callback.
 * @note Thread-safe. The callback receives a const pointer to the entry, but
 *       this pointer should not be stored beyond the callback invocation. */
void
csilk_reflect_foreach(csilk_reflect_foreach_cb cb, void* user_data)
{
    if (!cb) {
        return;
    }
    const char* names[MAX_REG_STRUCTS];
    size_t      count = 0;

    /*
   * Phase 1 — Collect type names under the registry lock: copy all
   * registered type names into a local stack array while holding the
   * mutex.  This gives us a consistent snapshot of the registry.  The
   * lock is released immediately after the copy.
   */
    registry_lock();
    for (size_t i = 0; i < g_registry_count; i++) {
        names[count++] = g_registry[i].name;
    }
    registry_unlock();

    /*
   * Phase 2 — Invoke callbacks outside the lock: each callback calls
   * csilk_reflect_find() internally (which acquires the mutex).  This
   * two-phase design avoids deadlocks when the callback itself re-enters
   * the reflection API — e.g., swagger.c's add_schema() calls
   * csilk_reflect_find() to resolve nested struct types.
   */
    for (size_t i = 0; i < count; i++) {
        const csilk_reflect_entry_t* entry = csilk_reflect_find(names[i]);
        if (entry) {
            cb(names[i], entry, user_data);
        }
    }
}

int
get_basic_type(const char* type_name, csilk_field_desc_t* out_desc)
{
    memset(out_desc, 0, sizeof(*out_desc));
    if (strcmp(type_name, "bool") == 0) {
        out_desc->type = CSILK_TYPE_BOOL;
        return 1;
    }
    if (strcmp(type_name, "int8") == 0) {
        out_desc->type = CSILK_TYPE_INT8;
        return 1;
    }
    if (strcmp(type_name, "uint8") == 0) {
        out_desc->type = CSILK_TYPE_UINT8;
        return 1;
    }
    if (strcmp(type_name, "int16") == 0) {
        out_desc->type = CSILK_TYPE_INT16;
        return 1;
    }
    if (strcmp(type_name, "uint16") == 0) {
        out_desc->type = CSILK_TYPE_UINT16;
        return 1;
    }
    if (strcmp(type_name, "int32") == 0) {
        out_desc->type = CSILK_TYPE_INT32;
        return 1;
    }
    if (strcmp(type_name, "uint32") == 0) {
        out_desc->type = CSILK_TYPE_UINT32;
        return 1;
    }
    if (strcmp(type_name, "int64") == 0) {
        out_desc->type = CSILK_TYPE_INT64;
        return 1;
    }
    if (strcmp(type_name, "uint64") == 0) {
        out_desc->type = CSILK_TYPE_UINT64;
        return 1;
    }
    if (strcmp(type_name, "float") == 0) {
        out_desc->type = CSILK_TYPE_FLOAT;
        return 1;
    }
    if (strcmp(type_name, "double") == 0) {
        out_desc->type = CSILK_TYPE_DOUBLE;
        return 1;
    }
    if (strcmp(type_name, "string") == 0) {
        out_desc->type = CSILK_TYPE_STRING;
        out_desc->is_pointer = true;
        return 1;
    }
    return 0;
}
