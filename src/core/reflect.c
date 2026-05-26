/**
 * @file reflect.c
 * @brief Reflection and JSON binding implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uv.h>

#include "csilk_reflect.h"

#define MAX_REG_STRUCTS 256

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
void csilk_reflect_init(void) {
  if (!g_registry_mutex_init) {
    uv_mutex_init(&g_registry_mutex);
    g_registry_mutex_init = 1;
  }
}

/** @brief Internal: acquire the global reflection registry mutex.
 *
 * Initializes the mutex on first call if not yet initialized.
 * Blocks until the lock is acquired. */
static void registry_lock(void) {
  if (!g_registry_mutex_init) {
    csilk_reflect_init();
  }
  uv_mutex_lock(&g_registry_mutex);
}

/** @brief Internal: unlock the global reflection registry mutex.
 *
 * Must be called after registry_lock() to release the lock around the
 * registered types table. */
static void registry_unlock(void) { uv_mutex_unlock(&g_registry_mutex); }

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
void csilk_reflect_register(const char* name, const csilk_field_desc_t* fields,
                            size_t count) {
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
 * @return Pointer to the type's reflection entry, or NULL if not found.
 * @note Thread-safe. The returned pointer is valid for the lifetime of the
 *       registration. */
const csilk_reflect_entry_t* csilk_reflect_find(const char* name) {
  if (!name) return NULL;
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
void csilk_reflect_foreach(csilk_reflect_foreach_cb cb, void* user_data) {
  if (!cb) return;
  const char* names[MAX_REG_STRUCTS];
  size_t count = 0;

  registry_lock();
  for (size_t i = 0; i < g_registry_count; i++) {
    names[count++] = g_registry[i].name;
  }
  registry_unlock();

  for (size_t i = 0; i < count; i++) {
    const csilk_reflect_entry_t* entry = csilk_reflect_find(names[i]);
    if (entry) {
      cb(names[i], entry, user_data);
    }
  }
}

static cJSON* serialize_scalar(const void* addr,
                               const csilk_field_desc_t* desc);
static void struct_to_cjson_internal(cJSON* obj, const void* struct_ptr,
                                     const csilk_field_desc_t* descs,
                                     size_t field_count);

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
static cJSON* serialize_scalar(const void* addr,
                               const csilk_field_desc_t* desc) {
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
      const char* str =
          desc->is_pointer ? *(const char**)addr : (const char*)addr;
      return str ? cJSON_CreateString(str) : cJSON_CreateNull();
    }
    case CSILK_TYPE_STRUCT: {
      const void* struct_addr = desc->is_pointer ? *(const void**)addr : addr;
      if (!struct_addr) return cJSON_CreateNull();

      const csilk_reflect_entry_t* entry =
          csilk_reflect_find(desc->nested_type_name);
      if (!entry) return cJSON_CreateNull();

      cJSON* sub_obj = cJSON_CreateObject();
      if (!sub_obj) return NULL;
      struct_to_cjson_internal(sub_obj, struct_addr, entry->fields,
                               entry->count);
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
 * @param struct_ptr  Pointer to the source struct (must not be NULL).
 * @param descs       Array of field descriptors.
 * @param field_count Number of field descriptors. */
static void struct_to_cjson_internal(cJSON* obj, const void* struct_ptr,
                                     const csilk_field_desc_t* descs,
                                     size_t field_count) {
  for (size_t i = 0; i < field_count; i++) {
    const char* field_addr = (const char*)struct_ptr + descs[i].offset;

    if (descs[i].array_length > 0) {
      cJSON* arr = cJSON_CreateArray();
      if (!arr) continue;
      for (size_t j = 0; j < descs[i].array_length; j++) {
        const char* item_addr = field_addr + (j * descs[i].size);
        cJSON_AddItemToArray(arr, serialize_scalar(item_addr, &descs[i]));
      }
      cJSON_AddItemToObject(obj, descs[i].json_key, arr);
    } else {
      cJSON_AddItemToObject(obj, descs[i].json_key,
                            serialize_scalar(field_addr, &descs[i]));
    }
  }
}

static void deserialize_scalar(const cJSON* item, void* addr,
                               const csilk_field_desc_t* desc);
static void cjson_to_struct_internal(const cJSON* obj, void* struct_ptr,
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
 * @param item Source cJSON node (may be NULL or Null).
 * @param addr Memory address of the target field.
 * @param desc Field descriptor with type, size, and pointer flag.
 * @note For pointer string fields, any existing allocation is freed before
 *       the new value is assigned. */
static void deserialize_scalar(const cJSON* item, void* addr,
                               const csilk_field_desc_t* desc) {
  if (!item || cJSON_IsNull(item)) return;

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
          if (*ptr) free(*ptr);
          size_t len = strlen(item->valuestring) + 1;
          *ptr = (char*)malloc(len);
          if (*ptr) memcpy(*ptr, item->valuestring, len);
        } else {
          strncpy((char*)addr, item->valuestring, desc->size - 1);
          ((char*)addr)[desc->size - 1] = '\0';
        }
      }
      break;
    case CSILK_TYPE_STRUCT:
      if (cJSON_IsObject(item)) {
        void* struct_addr = addr;
        if (desc->is_pointer) {
          void** ptr = (void**)addr;
          if (!*ptr) *ptr = calloc(1, desc->size);
          struct_addr = *ptr;
        }
        if (struct_addr) {
          const csilk_reflect_entry_t* entry =
              csilk_reflect_find(desc->nested_type_name);
          if (entry) {
            cjson_to_struct_internal(item, struct_addr, entry->fields,
                                     entry->count);
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
static void cjson_to_struct_internal(const cJSON* obj, void* struct_ptr,
                                     const csilk_field_desc_t* descs,
                                     size_t field_count) {
  for (size_t i = 0; i < field_count; i++) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, descs[i].json_key);
    if (!item) continue;

    char* field_addr = (char*)struct_ptr + descs[i].offset;

    if (descs[i].array_length > 0) {
      if (!cJSON_IsArray(item)) continue;
      size_t arr_size = cJSON_GetArraySize(item);
      size_t limit =
          (arr_size < descs[i].array_length) ? arr_size : descs[i].array_length;

      for (size_t j = 0; j < limit; j++) {
        char* item_addr = field_addr + (j * descs[i].size);
        deserialize_scalar(cJSON_GetArrayItem(item, j), item_addr, &descs[i]);
      }
    } else {
      deserialize_scalar(item, field_addr, &descs[i]);
    }
  }
}

static int get_basic_type(const char* type_name, csilk_field_desc_t* out_desc) {
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
char* csilk_json_marshal(const char* type_name, const void* ptr) {
  if (!type_name || !ptr) return NULL;

  csilk_field_desc_t basic_desc;
  if (get_basic_type(type_name, &basic_desc)) {
    cJSON* node = serialize_scalar(ptr, &basic_desc);
    if (!node) return NULL;
    char* out = cJSON_PrintUnformatted(node);
    cJSON_Delete(node);
    return out;
  }

  const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
  if (!entry) return NULL;

  cJSON* root = cJSON_CreateObject();
  if (!root) return NULL;

  struct_to_cjson_internal(root, ptr, entry->fields, entry->count);
  char* out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

/** @brief Deserialize a JSON string into a registered struct or basic type
 * instance. */
int csilk_json_unmarshal(const char* type_name, const char* json_str,
                         void* ptr) {
  if (!type_name || !json_str || !ptr) return 0;

  csilk_field_desc_t basic_desc;
  if (get_basic_type(type_name, &basic_desc)) {
    cJSON* root = cJSON_Parse(json_str);
    if (!root) return 0;
    deserialize_scalar(root, ptr, &basic_desc);
    cJSON_Delete(root);
    return 1;
  }

  const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
  if (!entry) return 0;

  cJSON* root = cJSON_Parse(json_str);
  if (!root) return 0;

  cjson_to_struct_internal(root, ptr, entry->fields, entry->count);
  cJSON_Delete(root);
  return 1;
}
