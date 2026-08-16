/**
 * @file src/core/json/json_free.c
 * @brief Free a csilk_json_t value.
 */

#include "json_internal.h"

#include <stdlib.h>

void
csilk_json_free(csilk_json_t* v)
{
    if (!v) {
        return;
    }
    if (v->is_owner) {
        if (v->kind == CSILK_JSON_MUTABLE) {
            if (v->doc.mdoc) {
                yyjson_mut_doc_free(v->doc.mdoc);
            }
        } else {
            if (v->doc.idoc) {
                yyjson_doc_free(v->doc.idoc);
            }
        }
    }
    if (!v->is_static) {
        free(v);
    }
}
