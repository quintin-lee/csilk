/**
 * @file src/core/json/json_internal.c
 * @brief View creation helpers and memory management.
 */

#include "json_internal.h"
#include <stdatomic.h>

enum {
    JSON_DOC_BUCKETS = 256,
    JSON_VIEW_SLAB_CAP = 256,
};

typedef struct json_view_slab_s {
    struct json_view_slab_s* next;
    size_t                   used;
    csilk_json_t             slots[JSON_VIEW_SLAB_CAP];
} json_view_slab_t;

typedef struct json_doc_entry_s {
    void*                    doc_ptr;
    json_view_slab_t*        slabs;
    struct json_doc_entry_s* next;
} json_doc_entry_t;

typedef struct {
    atomic_flag       lock;
    json_doc_entry_t* head;
} json_doc_bucket_t;

enum { JSON_TLS_CACHE_SLOTS = 4 };

typedef struct {
    void*             doc_ptr;
    json_view_slab_t* slab;
    uint64_t          epoch;
} json_tls_cache_slot_t;

static json_doc_bucket_t                   g_json_doc_buckets[JSON_DOC_BUCKETS];
static _Atomic uint64_t                    g_json_free_epoch = 1;
static _Thread_local json_tls_cache_slot_t tls_doc_cache[JSON_TLS_CACHE_SLOTS];
static _Thread_local size_t                tls_doc_next = 0;

static inline size_t
json_doc_hash(const void* p)
{
    uintptr_t v = (uintptr_t)p;
    v = (v >> 4) ^ (v >> 13) ^ (v >> 22);
    return (size_t)(v & (JSON_DOC_BUCKETS - 1));
}

static inline void
json_bucket_lock(json_doc_bucket_t* b)
{
    while (atomic_flag_test_and_set_explicit(&b->lock, memory_order_acquire)) {
    }
}

static inline void
json_bucket_unlock(json_doc_bucket_t* b)
{
    atomic_flag_clear_explicit(&b->lock, memory_order_release);
}

static csilk_json_t*
json_alloc_doc_view(void* doc_ptr)
{
    if (!doc_ptr) {
        csilk_json_t* j = malloc(sizeof(csilk_json_t));
        if (j) {
            j->flags = CSILK_JSON_F_HEAP;
        }
        return j;
    }

    /* Fast path: check this thread's private slab for doc_ptr (zero locks). */
    uint64_t cur_epoch = atomic_load_explicit(&g_json_free_epoch, memory_order_acquire);
    int      cache_idx = -1;
    for (int i = 0; i < JSON_TLS_CACHE_SLOTS; i++) {
        if (tls_doc_cache[i].doc_ptr == doc_ptr) {
            if (tls_doc_cache[i].epoch == cur_epoch && tls_doc_cache[i].slab &&
                tls_doc_cache[i].slab->used < JSON_VIEW_SLAB_CAP) {
                csilk_json_t* slot = &tls_doc_cache[i].slab->slots[tls_doc_cache[i].slab->used++];
                slot->flags = 0;
                slot->_pad = 0;
                return slot;
            }
            cache_idx = i;
            break;
        }
    }

    size_t             idx = json_doc_hash(doc_ptr);
    json_doc_bucket_t* b = &g_json_doc_buckets[idx];
    json_bucket_lock(b);

    cur_epoch = atomic_load_explicit(&g_json_free_epoch, memory_order_acquire);
    json_doc_entry_t* entry = b->head;
    while (entry && entry->doc_ptr != doc_ptr) {
        entry = entry->next;
    }
    if (!entry) {
        entry = calloc(1, sizeof(json_doc_entry_t));
        if (!entry) {
            json_bucket_unlock(b);
            return NULL;
        }
        entry->doc_ptr = doc_ptr;
        entry->next = b->head;
        b->head = entry;
    }

    json_view_slab_t* slab = malloc(sizeof(json_view_slab_t));
    if (!slab) {
        json_bucket_unlock(b);
        return NULL;
    }
    slab->used = 1;
    slab->next = entry->slabs;
    entry->slabs = slab;
    json_bucket_unlock(b);

    if (cache_idx < 0) {
        cache_idx = (int)((tls_doc_next++) % JSON_TLS_CACHE_SLOTS);
    }
    tls_doc_cache[cache_idx].doc_ptr = doc_ptr;
    tls_doc_cache[cache_idx].slab = slab;
    tls_doc_cache[cache_idx].epoch = cur_epoch;

    csilk_json_t* slot = &slab->slots[0];
    slot->flags = 0;
    slot->_pad = 0;
    return slot;
}

void
json_release_doc_views(void* doc_ptr)
{
    if (!doc_ptr) {
        return;
    }
    size_t             idx = json_doc_hash(doc_ptr);
    json_doc_bucket_t* b = &g_json_doc_buckets[idx];
    json_bucket_lock(b);

    json_doc_entry_t** prev = &b->head;
    json_doc_entry_t*  entry = b->head;
    while (entry && entry->doc_ptr != doc_ptr) {
        prev = &entry->next;
        entry = entry->next;
    }
    if (entry) {
        *prev = entry->next;
        atomic_fetch_add_explicit(&g_json_free_epoch, 1, memory_order_release);
    }
    json_bucket_unlock(b);

    if (entry) {
        json_view_slab_t* slab = entry->slabs;
        while (slab) {
            json_view_slab_t* next = slab->next;
            free(slab);
            slab = next;
        }
        free(entry);
    }
}

csilk_json_t*
json_mut_new(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        if (mdoc) {
            json_release_doc_views(mdoc);
            yyjson_mut_doc_free(mdoc);
        }
        return NULL;
    }
    csilk_json_t* j = malloc(sizeof(csilk_json_t));
    if (!j) {
        if (mdoc) {
            json_release_doc_views(mdoc);
            yyjson_mut_doc_free(mdoc);
        }
        return NULL;
    }
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->flags = CSILK_JSON_F_OWNER | CSILK_JSON_F_MUTABLE | CSILK_JSON_F_HEAP;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_imut_new(yyjson_doc* doc, yyjson_val* val)
{
    if (!val) {
        if (doc) {
            json_release_doc_views(doc);
            yyjson_doc_free(doc);
        }
        return NULL;
    }
    csilk_json_t* j = malloc(sizeof(csilk_json_t));
    if (!j) {
        if (doc) {
            json_release_doc_views(doc);
            yyjson_doc_free(doc);
        }
        return NULL;
    }
    j->u.ival = val;
    j->doc.idoc = doc;
    j->flags = CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_view_immutable(yyjson_doc* idoc, yyjson_val* val)
{
    if (!val) {
        return NULL;
    }
    csilk_json_t* j = json_alloc_doc_view(idoc);
    if (!j) {
        return NULL;
    }
    j->u.ival = val;
    j->doc.idoc = idoc;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_view_mutable(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    csilk_json_t* j = json_alloc_doc_view(mdoc);
    if (!j) {
        return NULL;
    }
    j->u.mval = mval;
    j->doc.mdoc = mdoc;
    j->flags |= CSILK_JSON_F_MUTABLE;
    j->_pad = 0;
    return j;
}
