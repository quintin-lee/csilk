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

/** @brief Initialize the reflection system. */
void csilk_reflect_init(void) {
    if (!g_registry_mutex_init) {
        uv_mutex_init(&g_registry_mutex);
        g_registry_mutex_init = 1;
    }
}

/** @brief Lock the reflection registry mutex. */
static void registry_lock(void) {
    if (!g_registry_mutex_init) {
        csilk_reflect_init();
    }
    uv_mutex_lock(&g_registry_mutex);
}

/** @brief Unlock the reflection registry mutex. */
static void registry_unlock(void) {
    uv_mutex_unlock(&g_registry_mutex);
}

/** @brief Register a type with the reflection engine. */
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

/** @brief Find a registered type descriptor by name. */
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

static cJSON* serialize_scalar(const void* addr, const csilk_field_desc_t* desc);
static void struct_to_cjson_internal(cJSON* obj, const void* struct_ptr,
                                   const csilk_field_desc_t* descs,
                                   size_t field_count);

/** @brief Serialize a single field value to a cJSON node.
 * @param addr Field address.
 * @param desc Field descriptor.
 * @return cJSON node, or NULL on failure. */
static cJSON* serialize_scalar(const void* addr, const csilk_field_desc_t* desc) {
  switch (desc->type) {
    case CSILK_TYPE_INT8:   return cJSON_CreateNumber(*(const int8_t*)addr);
    case CSILK_TYPE_UINT8:  return cJSON_CreateNumber(*(const uint8_t*)addr);
    case CSILK_TYPE_INT16:  return cJSON_CreateNumber(*(const int16_t*)addr);
    case CSILK_TYPE_UINT16: return cJSON_CreateNumber(*(const uint16_t*)addr);
    case CSILK_TYPE_INT32:  return cJSON_CreateNumber(*(const int32_t*)addr);
    case CSILK_TYPE_UINT32: return cJSON_CreateNumber(*(const uint32_t*)addr);
    case CSILK_TYPE_INT64:  return cJSON_CreateNumber((double)*(const int64_t*)addr);
    case CSILK_TYPE_UINT64: return cJSON_CreateNumber((double)*(const uint64_t*)addr);
    case CSILK_TYPE_FLOAT:  return cJSON_CreateNumber(*(const float*)addr);
    case CSILK_TYPE_DOUBLE: return cJSON_CreateNumber(*(const double*)addr);
    case CSILK_TYPE_BOOL:   return cJSON_CreateBool(*(const bool*)addr);
    case CSILK_TYPE_STRING: {
      const char* str = desc->is_pointer ? *(const char**)addr : (const char*)addr;
      return str ? cJSON_CreateString(str) : cJSON_CreateNull();
    }
    case CSILK_TYPE_STRUCT: {
      const void* struct_addr = desc->is_pointer ? *(const void**)addr : addr;
      if (!struct_addr) return cJSON_CreateNull();
      
      const csilk_reflect_entry_t* entry = csilk_reflect_find(desc->nested_type_name);
      if (!entry) return cJSON_CreateNull();

      cJSON* sub_obj = cJSON_CreateObject();
      if (!sub_obj) return NULL;
      struct_to_cjson_internal(sub_obj, struct_addr, entry->fields, entry->count);
      return sub_obj;
    }
  }
  return cJSON_CreateNull();
}

/** @brief Walk struct fields and build cJSON object recursively.
 * @param obj Target cJSON object.
 * @param struct_ptr Source struct.
 * @param descs Field descriptors.
 * @param field_count Number of fields. */
static void struct_to_cjson_internal(cJSON* obj, const void* struct_ptr,
                                   const csilk_field_desc_t* descs,
                                   size_t field_count) {
  for (size_t i = 0; i < field_count; i++) {
    const char* field_addr = (const char*)struct_ptr + descs[i].offset;

    if (descs[i].array_length > 0 && descs[i].type != CSILK_TYPE_STRING) {
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

/** @brief Deserialize a cJSON value into a struct field.
 * @param item Source cJSON.
 * @param addr Field address.
 * @param desc Field descriptor. */
static void deserialize_scalar(const cJSON* item, void* addr,
                             const csilk_field_desc_t* desc) {
  if (!item || cJSON_IsNull(item)) return;

  switch (desc->type) {
    case CSILK_TYPE_INT8:   *(int8_t*)addr  = (int8_t)item->valueint; break;
    case CSILK_TYPE_UINT8:  *(uint8_t*)addr = (uint8_t)item->valueint; break;
    case CSILK_TYPE_INT16:  *(int16_t*)addr = (int16_t)item->valueint; break;
    case CSILK_TYPE_UINT16: *(uint16_t*)addr = (uint16_t)item->valueint; break;
    case CSILK_TYPE_INT32:  *(int32_t*)addr = (int32_t)item->valueint; break;
    case CSILK_TYPE_UINT32: *(uint32_t*)addr = (uint32_t)item->valueint; break;
    case CSILK_TYPE_INT64:  *(int64_t*)addr  = (int64_t)item->valuedouble; break;
    case CSILK_TYPE_UINT64: *(uint64_t*)addr = (uint64_t)item->valuedouble; break;
    case CSILK_TYPE_FLOAT:  *(float*)addr   = (float)item->valuedouble; break;
    case CSILK_TYPE_DOUBLE: *(double*)addr  = item->valuedouble; break;
    case CSILK_TYPE_BOOL:   *(bool*)addr    = cJSON_IsTrue(item); break;
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
          const csilk_reflect_entry_t* entry = csilk_reflect_find(desc->nested_type_name);
          if (entry) {
            cjson_to_struct_internal(item, struct_addr, entry->fields, entry->count);
          }
        }
      }
      break;
  }
}

/** @brief Walk cJSON object and populate struct fields recursively.
 * @param obj Source cJSON.
 * @param struct_ptr Target struct.
 * @param descs Field descriptors.
 * @param field_count Number of fields. */
static void cjson_to_struct_internal(const cJSON* obj, void* struct_ptr,
                                   const csilk_field_desc_t* descs,
                                   size_t field_count) {
  for (size_t i = 0; i < field_count; i++) {
    cJSON* item = cJSON_GetObjectItemCaseSensitive(obj, descs[i].json_key);
    if (!item) continue;

    char* field_addr = (char*)struct_ptr + descs[i].offset;

    if (descs[i].array_length > 0 && descs[i].type != CSILK_TYPE_STRING) {
      if (!cJSON_IsArray(item)) continue;
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

/** @brief Serialize a registered struct to a JSON string. */
char* csilk_json_marshal(const char* type_name, const void* ptr) {
  const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
  if (!entry) return NULL;

  cJSON* root = cJSON_CreateObject();
  if (!root) return NULL;

  struct_to_cjson_internal(root, ptr, entry->fields, entry->count);
  char* out = cJSON_PrintUnformatted(root);
  cJSON_Delete(root);
  return out;
}

/** @brief Deserialize a JSON string into a registered struct. */
int csilk_json_unmarshal(const char* type_name, const char* json_str, void* ptr) {
  const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
  if (!entry || !json_str) return 0;

  cJSON* root = cJSON_Parse(json_str);
  if (!root) return 0;

  cjson_to_struct_internal(root, ptr, entry->fields, entry->count);
  cJSON_Delete(root);
  return 1;
}
