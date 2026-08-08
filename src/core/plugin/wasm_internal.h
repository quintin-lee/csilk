/**
 * @file wasm_internal.h
 * @brief Internal header for WASM VM structures, operand stack, and memory limits.
 * @copyright MIT License
 */

#ifndef CSILK_WASM_INTERNAL_H
#define CSILK_WASM_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "csilk/core/sync.h"
#include "csilk/core/wasm.h"

#define CSILK_WASM_MAGIC 0x6d736100u   /* \0asm */
#define CSILK_WASM_VERSION 0x01000000u /* v1.0 */
#define CSILK_WASM_PAGE_SIZE 65536u    /* 64 KB */

typedef union {
    int32_t i32;
    int64_t i64;
    float   f32;
    double  f64;
} csilk_wasm_val_t;

typedef struct {
    uint8_t* data;
    uint32_t initial_pages;
    uint32_t max_pages;
    size_t   current_size;
} csilk_wasm_memory_t;

typedef struct {
    csilk_wasm_val_t stack[256];
    size_t           sp;
    uint64_t         fuel;
} csilk_wasm_exec_ctx_t;

struct csilk_wasm_plugin_s {
    csilk_mutex_t       mutex;
    csilk_wasm_config_t config;
    csilk_wasm_memory_t memory;
    uint8_t*            bytecode;
    size_t              bytecode_size;
    char                entry_point[64];
};

void* csilk_wasm_get_memory_slice(csilk_wasm_memory_t* mem, uint32_t offset, uint32_t len);

#endif /* CSILK_WASM_INTERNAL_H */
