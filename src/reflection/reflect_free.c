/**
 * @file reflect_free.c
 * @brief Recursive struct free implementation.
 *
 * Split from reflect.c.  Contains free_scalar, free_struct_internal,
 * and csilk_struct_free_reflect.
 *
 * @copyright MIT License
 */

#include "reflect_internal.h"
#include "csilk/core/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declaration — free_scalar calls free_struct_internal for nested
 * struct fields, and free_struct_internal calls free_scalar per field. */
static void free_struct_internal(void*                     struct_ptr,
                                 const csilk_field_desc_t* descs,
                                 size_t                    field_count,
                                 int                       depth,
                                 void**                    visited);

static void
free_scalar(void* addr, const csilk_field_desc_t* desc, int depth, void** visited)
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
                *ptr = NULL;
            }
        }
        break;
    case CSILK_TYPE_STRUCT: {
        if (desc->nested_type_name) {
            const csilk_reflect_entry_t* entry = csilk_reflect_find(desc->nested_type_name);
            if (entry) {
                void* struct_addr = addr;
                if (desc->is_pointer) {
                    void** ptr = (void**)addr;
                    if (*ptr) {
                        struct_addr = *ptr;
                        int is_visited = 0;
                        for (int d = 0; d <= depth; d++) {
                            if (visited[d] == struct_addr) {
                                is_visited = 1;
                                break;
                            }
                        }
                        if (is_visited) {
                            // Break cycle to avoid recursion and double freeing
                            *ptr = NULL;
                        } else {
                            free_struct_internal(
                                struct_addr, entry->fields, entry->count, depth + 1, visited);
                            free(*ptr);
                            *ptr = NULL;
                        }
                    }
                } else {
                    int is_visited = 0;
                    for (int d = 0; d <= depth; d++) {
                        if (visited[d] == struct_addr) {
                            is_visited = 1;
                            break;
                        }
                    }
                    if (!is_visited) {
                        free_struct_internal(
                            struct_addr, entry->fields, entry->count, depth + 1, visited);
                    }
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
free_struct_internal(void*                     struct_ptr,
                     const csilk_field_desc_t* descs,
                     size_t                    field_count,
                     int                       depth,
                     void**                    visited)
{
    if (!struct_ptr || !descs) {
        return;
    }

    if (depth >= 32) {
        fprintf(stderr,
                "ERROR: Max recursion depth exceeded in csilk_struct_free_reflect "
                "(circular reference?)\n");
        return;
    }

    visited[depth] = struct_ptr;

    for (size_t i = 0; i < field_count; i++) {
        void* field_addr = (char*)struct_ptr + descs[i].offset;

        if (descs[i].array_length > 0) {
            for (size_t j = 0; j < descs[i].array_length; j++) {
                void* item_addr = (char*)field_addr + (j * descs[i].size);
                free_scalar(item_addr, &descs[i], depth, visited);
            }
        } else {
            free_scalar(field_addr, &descs[i], depth, visited);
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

    void* visited[32];
    memset(visited, 0, sizeof(visited));

    csilk_field_desc_t basic_desc;
    if (get_basic_type(type_name, &basic_desc)) {
        free_scalar(ptr, &basic_desc, 0, visited);
        return;
    }

    const csilk_reflect_entry_t* entry = csilk_reflect_find(type_name);
    if (!entry) {
        return;
    }

    free_struct_internal(ptr, entry->fields, entry->count, 0, visited);
}
