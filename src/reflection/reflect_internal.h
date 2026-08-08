/**
 * @file reflect_internal.h
 * @brief Internal declarations shared across reflection split files.
 *
 * This header is NOT part of the public API. It exists so that
 * reflect_marshal.c, reflect_unmarshal.c, and reflect_free.c can call
 * get_basic_type() which is defined in reflect.c.
 *
 * @copyright MIT License
 */

#ifndef CSILK_REFLECT_INTERNAL_H
#define CSILK_REFLECT_INTERNAL_H

#include "csilk/reflection/reflect.h"

/**
 * @brief Map a primitive type name string to a csilk_field_desc_t.
 *
 * If @p type_name matches a known scalar type ("bool", "int8", "uint8", …,
 * "string"), fills @p out_desc with the corresponding CSILK_TYPE_* enum and
 * returns 1.  Returns 0 for unrecognised names.
 *
 * @param type_name  Type name to look up.
 * @param out_desc   Output field descriptor (zeroed before population).
 * @return 1 if the name matched a built-in type, 0 otherwise.
 */
int get_basic_type(const char* type_name, csilk_field_desc_t* out_desc);

#endif /* CSILK_REFLECT_INTERNAL_H */
