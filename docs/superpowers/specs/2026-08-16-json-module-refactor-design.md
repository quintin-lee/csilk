# JSON Module Refactoring Design

**Date**: 2026-08-16  
**Status**: Draft

## Overview

Split `src/core/json/json.c` (1434 lines) into focused modules.

## Target Structure

```
src/core/json/
├── json_internal.c    # View creation helpers (lines 1-125)
├── json_factory.c     # Object/array/string/number factories (126-270)
├── json_object.c      # Object manipulation: add_* (271-535)
├── json_array.c       # Array ops: append, iterate, size (536-1054)
├── json_access.c      # Get functions + type extractors (1055-1330)
├── json_type.c        # Type checks: is_*, parse, free, copy (1331-1434)
└── json.c             # Thin wrapper
```

## File Responsibilities

### json_internal.c
- TLS view ring helpers
- `json_mut_new()`, `json_imut_new()`
- Non-owning view creators

### json_factory.c
- `csilk_json_object()`, `csilk_json_array()`
- `csilk_json_string_new()`, `csilk_json_number()`, `csilk_json_int()`
- `csilk_json_bool()`, `csilk_json_null()`

### json_object.c
- `csilk_json_add_object()`, `csilk_json_add_array()`
- `csilk_json_add_string()`, `csilk_json_add_number()`
- `csilk_json_add_int()`, `csilk_json_add_bool()`, `csilk_json_add_null()`
- `csilk_json_add_item()`

### json_array.c
- `csilk_json_array_append()`
- Array iteration: `csilk_json_array_foreach*`
- Array size: `csilk_json_array_size()`

### json_access.c
- Get functions: `csilk_json_get*`
- Type extractors: `csilk_json_string_value()`, `csilk_json_number_value()`, etc.

### json_type.c
- Type checks: `csilk_json_is_*`
- Parse: `csilk_json_parse()`
- Free: `csilk_json_free()`
- Copy: `csilk_json_copy()`
- Setters: `csilk_json_set_*`

## Non-Goals
- No functional changes
- No API changes
