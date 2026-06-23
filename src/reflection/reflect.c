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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

static csilk_reflect_entry_t g_registry[MAX_REG_STRUCTS];
static size_t g_registry_count = 0;
static uv_mutex_t g_registry_mutex;
static int g_registry_mutex_init = 0;

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
		uv_mutex_init(&g_registry_mutex);
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
	uv_mutex_lock(&g_registry_mutex);
}

/** @brief Internal: unlock the global reflection registry mutex.
 *
 * Must be called after registry_lock() to release the lock around the
 * registered types table. */
static void
registry_unlock(void)
{
	uv_mutex_unlock(&g_registry_mutex);
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
	}
	registry_unlock();
}

/** @brief Look up a registered type descriptor by name.
 *
 * Searches the global registry for a type matching @p name.
 *
 * @param name Type name to find (case-sensitive).
 * @return Pointer to the type's reflection entry, or nullptr if not found.
 * @note Thread-safe. The returned pointer is valid for the lifetime of the
 *       registration. */
const csilk_reflect_entry_t*
csilk_reflect_find(const char* name)
{
	if (!name) {
		return nullptr;
	}
	registry_lock();
	for (size_t i = 0; i < g_registry_count; i++) {
		if (strcmp(g_registry[i].name, name) == 0) {
			registry_unlock();
			return &g_registry[i];
		}
	}
	registry_unlock();
	return nullptr;
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
	size_t count = 0;

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

static cJSON* serialize_scalar(const void* addr, const csilk_field_desc_t* desc);
static void struct_to_cjson_internal(cJSON* obj,
				     const void* struct_ptr,
				     const csilk_field_desc_t* descs,
				     size_t field_count);

static void free_scalar(void* addr, const csilk_field_desc_t* desc);
static void
free_struct_internal(void* struct_ptr, const csilk_field_desc_t* descs, size_t field_count);

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
struct_to_cjson_internal(cJSON* obj,
			 const void* struct_ptr,
			 const csilk_field_desc_t* descs,
			 size_t field_count)
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
			cJSON_AddItemToObject(
			    obj, descs[i].json_key, serialize_scalar(field_addr, &descs[i]));
		}
	}
}

static void deserialize_scalar(const cJSON* item, void* addr, const csilk_field_desc_t* desc);
static void cjson_to_struct_internal(const cJSON* obj,
				     void* struct_ptr,
				     const csilk_field_desc_t* descs,
				     size_t field_count);

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
				const csilk_reflect_entry_t* entry =
				    csilk_reflect_find(desc->nested_type_name);
				if (entry) {
					cjson_to_struct_internal(
					    item, struct_addr, entry->fields, entry->count);
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
cjson_to_struct_internal(const cJSON* obj,
			 void* struct_ptr,
			 const csilk_field_desc_t* descs,
			 size_t field_count)
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
			size_t limit =
			    (arr_size < descs[i].array_length) ? arr_size : descs[i].array_length;

			for (size_t j = 0; j < limit; j++) {
				char* item_addr = field_addr + (j * descs[i].size);
				deserialize_scalar(
				    cJSON_GetArrayItem(item, j), item_addr, &descs[i]);
			}
		} else {
			deserialize_scalar(item, field_addr, &descs[i]);
		}
	}
}

static int
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

static void
free_scalar(void* addr, const csilk_field_desc_t* desc)
{
	if (!addr || !desc) {
		return;
	}

	switch (desc->type) {
	case CSILK_TYPE_STRING:
		if (desc->is_pointer) {
			char** ptr = (char**)addr;
			if (*ptr) {
				free(*ptr);
				*ptr = nullptr;
			}
		}
		break;
	case CSILK_TYPE_STRUCT: {
		if (desc->nested_type_name) {
			const csilk_reflect_entry_t* entry =
			    csilk_reflect_find(desc->nested_type_name);
			if (entry) {
				void* struct_addr = addr;
				if (desc->is_pointer) {
					void** ptr = (void**)addr;
					if (*ptr) {
						struct_addr = *ptr;
						free_struct_internal(
						    struct_addr, entry->fields, entry->count);
						free(*ptr);
						*ptr = nullptr;
					}
				} else {
					free_struct_internal(
					    struct_addr, entry->fields, entry->count);
				}
			}
		}
		break;
	}
	default:
		break;
	}
}

static void
free_struct_internal(void* struct_ptr, const csilk_field_desc_t* descs, size_t field_count)
{
	if (!struct_ptr || !descs) {
		return;
	}

	for (size_t i = 0; i < field_count; i++) {
		void* field_addr = (char*)struct_ptr + descs[i].offset;

		if (descs[i].array_length > 0) {
			for (size_t j = 0; j < descs[i].array_length; j++) {
				void* item_addr = (char*)field_addr + (j * descs[i].size);
				free_scalar(item_addr, &descs[i]);
			}
		} else {
			free_scalar(field_addr, &descs[i]);
		}
	}
}

/** @brief Recursively free heap-allocated memory of a reflected struct's fields. */
void
csilk_struct_free_reflect(const char* type_name, void* ptr)
{
	if (!type_name || !ptr) {
		return;
	}

	csilk_field_desc_t basic_desc;
	if (get_basic_type(type_name, &basic_desc)) {
		free_scalar(ptr, &basic_desc);
		return;
	}

	const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
	if (!entry) {
		return;
	}

	free_struct_internal(ptr, entry->fields, entry->count);
}
