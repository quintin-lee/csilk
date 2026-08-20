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
    if (v->flags & CSILK_JSON_F_OWNER) {
        if (v->flags & CSILK_JSON_F_MUTABLE) {
            if (v->doc.mdoc) {
                yyjson_mut_doc_free((yyjson_mut_doc*)v->doc.mdoc);
            }
        } else {
            if (v->doc.idoc) {
                yyjson_doc_free((yyjson_doc*)v->doc.idoc);
            }
        }
        v->flags &= ~CSILK_JSON_F_OWNER;
    }
    if (v->flags & CSILK_JSON_F_HEAP) {
        free(v);
    }
}
