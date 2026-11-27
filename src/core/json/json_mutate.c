/**
 * @file src/core/json/json_mutate.c
 * @brief JSON mutation helpers: set_string, object add_*, and array append.
 */

#include "json_internal.h"

bool
csilk_json_set_string(csilk_json_t* v, const char* new_value)
{
    if (!v || !new_value || !v->u.raw) {
        return false;
    }
    if (json_is_mutable(v)) {
        if (!v->doc.mdoc || !yyjson_mut_is_str((yyjson_mut_val*)v->u.mval)) {
            return false;
        }
        return yyjson_mut_set_str((yyjson_mut_val*)v->u.mval, new_value);
    }
    if (!v->doc.idoc || !yyjson_is_str((yyjson_val*)v->u.ival)) {
        return false;
    }

    if (!json_is_owner(v)) {
        /* Borrowed view of an immutable doc: the value below would be
         * rebound onto a private mutable copy that no parent ever sees
         * (and the converted doc would leak), so refuse explicitly. */
        return false;
    }

    /* Convert immutable doc to mutable, replace root string, then swap. */
    yyjson_mut_doc* mdoc = yyjson_doc_mut_copy((yyjson_doc*)v->doc.idoc, NULL);
    if (!mdoc) {
        return false;
    }

    yyjson_mut_val* mroot = yyjson_mut_doc_get_root(mdoc);
    if (!mroot) {
        yyjson_mut_doc_free(mdoc);
        return false;
    }

    yyjson_mut_val* mnew_str = yyjson_mut_strcpy(mdoc, new_value);
    if (!mnew_str) {
        yyjson_mut_doc_free(mdoc);
        return false;
    }

    if (json_is_owner(v) && v->doc.idoc) {
        yyjson_doc_free((yyjson_doc*)v->doc.idoc);
    }
    v->u.mval = mnew_str;
    v->doc.mdoc = mdoc;
    v->flags |= (CSILK_JSON_F_OWNER | CSILK_JSON_F_MUTABLE);
    return true;
}

/* ====================================================================
 * Object Mutation
 * ==================================================================== */

static void
json_rebind_copied_item(csilk_json_t* item, yyjson_mut_doc* target_mdoc, yyjson_mut_val* mval)
{
    if (json_is_owner(item)) {
        if (json_is_mutable(item)) {
            if (item->doc.mdoc) {
                json_release_doc_views(item->doc.mdoc);
                yyjson_mut_doc_free((yyjson_mut_doc*)item->doc.mdoc);
            }
        } else {
            if (item->doc.idoc) {
                json_release_doc_views(item->doc.idoc);
                yyjson_doc_free((yyjson_doc*)item->doc.idoc);
            }
        }
    }
    bool is_heap = (item->flags & CSILK_JSON_F_HEAP) != 0;
    item->u.mval = mval;
    item->doc.mdoc = target_mdoc;
    item->flags &= ~(CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP);
    item->flags |= CSILK_JSON_F_MUTABLE;
    if (is_heap) {
        free(item);
    }
}

bool
json_add_to_obj(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    if (!obj || !key || !item) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }

    yyjson_mut_doc* target_mdoc = (yyjson_mut_doc*)obj->doc.mdoc;
    if (json_is_mutable(item) && item->doc.mdoc == obj->doc.mdoc) {
        return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                                  yyjson_mut_strcpy(target_mdoc, key),
                                  (yyjson_mut_val*)item->u.mval);
    }

    yyjson_mut_val* mval = json_is_mutable(item)
                               ? yyjson_mut_val_mut_copy(target_mdoc, (yyjson_mut_val*)item->u.mval)
                               : yyjson_val_mut_copy(target_mdoc, (yyjson_val*)item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_obj_add(
            (yyjson_mut_val*)obj->u.mval, yyjson_mut_strcpy(target_mdoc, key), mval)) {
        return false;
    }
    json_rebind_copied_item(item, target_mdoc, mval);
    return true;
}

bool
csilk_json_add_object(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

bool
csilk_json_add_array(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    return json_add_to_obj(obj, key, item);
}

csilk_json_t*
csilk_json_add_array_obj(csilk_json_t* obj, const char* key, csilk_json_t* item)
{
    if (!json_add_to_obj(obj, key, item)) {
        return NULL;
    }
    return csilk_json_get(obj, key);
}

bool
csilk_json_add_string(csilk_json_t* obj, const char* key, const char* value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    if (!value) {
        return csilk_json_add_null(obj, key);
    }
    return yyjson_mut_obj_add_strcpy(
        (yyjson_mut_doc*)obj->doc.mdoc, (yyjson_mut_val*)obj->u.mval, key, value);
}

bool
csilk_json_add_number(csilk_json_t* obj, const char* key, double value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_double((yyjson_mut_doc*)obj->doc.mdoc, value));
}

bool
csilk_json_add_int(csilk_json_t* obj, const char* key, int64_t value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_sint((yyjson_mut_doc*)obj->doc.mdoc, value));
}

bool
csilk_json_add_bool(csilk_json_t* obj, const char* key, bool value)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_bool((yyjson_mut_doc*)obj->doc.mdoc, value));
}

bool
csilk_json_add_null(csilk_json_t* obj, const char* key)
{
    if (!obj || !key) {
        return false;
    }
    if (!json_is_mutable(obj) || !obj->doc.mdoc) {
        return false;
    }
    return yyjson_mut_obj_add((yyjson_mut_val*)obj->u.mval,
                              yyjson_mut_strcpy((yyjson_mut_doc*)obj->doc.mdoc, key),
                              yyjson_mut_null((yyjson_mut_doc*)obj->doc.mdoc));
}

bool
csilk_json_add_item(csilk_json_t* obj, csilk_json_t* item)
{
    return csilk_json_array_append(obj, item);
}

/* ====================================================================
 * Array Mutation
 * ==================================================================== */

bool
csilk_json_array_append(csilk_json_t* arr, csilk_json_t* item)
{
    if (!arr || !item) {
        return false;
    }
    if (!json_is_mutable(arr) || !arr->doc.mdoc) {
        return false;
    }
    yyjson_mut_doc* target_mdoc = (yyjson_mut_doc*)arr->doc.mdoc;
    if (json_is_mutable(item) && item->doc.mdoc == arr->doc.mdoc) {
        return yyjson_mut_arr_add_val((yyjson_mut_val*)arr->u.mval, (yyjson_mut_val*)item->u.mval);
    }
    yyjson_mut_val* mval = json_is_mutable(item)
                               ? yyjson_mut_val_mut_copy(target_mdoc, (yyjson_mut_val*)item->u.mval)
                               : yyjson_val_mut_copy(target_mdoc, (yyjson_val*)item->u.ival);
    if (!mval) {
        return false;
    }
    if (!yyjson_mut_arr_add_val((yyjson_mut_val*)arr->u.mval, mval)) {
        return false;
    }
    json_rebind_copied_item(item, target_mdoc, mval);
    return true;
}
