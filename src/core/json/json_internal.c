/**
 * @file src/core/json/json_internal.c
 * @brief View creation helpers and memory management.
 */

#include "json_internal.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define JSON_VA_CHUNK_BYTES 4096

json_view_arena_t*
json_va_ensure(csilk_json_t* src)
{
    struct json_view_arena* va = atomic_load(&src->va);
    if (va) {
        return va;
    }
    struct json_view_arena* fresh = calloc(1, sizeof(*fresh));
    if (!fresh) {
        return NULL;
    }
    struct json_view_arena* expected = NULL;
    if (!atomic_compare_exchange_strong(&src->va, &expected, fresh)) {
        free(fresh);
        return expected;
    }
    return fresh;
}

/* Bump-allocate @p n bytes (8-byte aligned) from @p src's root view arena,
 * adding chunks as needed. OOM returns NULL. */
static void*
json_va_alloc(csilk_json_t* src, size_t n)
{
    json_view_arena_t* va = json_va_ensure(src);
    if (!va) {
        return NULL;
    }
    n = (n + 7u) & ~(size_t)7u;

    struct json_va_chunk* chunk = atomic_load(&va->head);
    while (chunk) {
        size_t prev = atomic_fetch_add_explicit(&chunk->used, n, memory_order_relaxed);
        if (prev + n <= chunk->cap) {
            return chunk->data + prev;
        }
        /* Chunk full — give the bytes back and move on. */
        atomic_fetch_sub_explicit(&chunk->used, n, memory_order_relaxed);
        struct json_va_chunk* cur = atomic_load(&va->head);
        if (cur == chunk) {
            break; /* Head is full and nothing newer was pushed: grow. */
        }
        chunk = cur;
    }

    size_t cap = JSON_VA_CHUNK_BYTES;
    if (n > cap) {
        cap = n;
    }
    struct json_va_chunk* fresh = malloc(offsetof(struct json_va_chunk, data) + cap);
    if (!fresh) {
        return NULL;
    }
    fresh->next = NULL;
    atomic_init(&fresh->used, n);
    fresh->cap = cap;

    struct json_va_chunk* expected = NULL;
    if (!atomic_compare_exchange_strong(&va->head, &expected, fresh)) {
        /* Push onto the lock-free head (LIFO), retrying on lost races. */
        do {
            fresh->next = expected;
        } while (!atomic_compare_exchange_weak(&va->head, &expected, fresh));
    }
    return fresh->data;
}

csilk_json_t*
json_mut_new(yyjson_mut_doc* mdoc, yyjson_mut_val* mval)
{
    if (!mval) {
        return NULL;
    }
    csilk_json_t* j = malloc(sizeof(csilk_json_t));
    if (!j) {
        return NULL;
    }
    atomic_init(&j->va, NULL);
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
        return NULL;
    }
    csilk_json_t* j = malloc(sizeof(csilk_json_t));
    if (!j) {
        return NULL;
    }
    atomic_init(&j->va, NULL);
    j->u.ival = val;
    j->doc.idoc = doc;
    j->flags = CSILK_JSON_F_OWNER | CSILK_JSON_F_HEAP;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_view_immutable(const csilk_json_t* src, yyjson_val* val)
{
    if (!val || !src) {
        return NULL;
    }
    /* Arena bookkeeping (creating src's va on first view) mutates only the
     * atomics, never the JSON payload — const is safe to drop here. */
    csilk_json_t* s = (csilk_json_t*)(uintptr_t)src;
    csilk_json_t* j = json_va_alloc(s, sizeof(*j));
    if (!j) {
        return NULL;
    }
    j->u.ival = val;
    j->doc.idoc = src->doc.idoc;
    atomic_init(&j->va, atomic_load(&s->va));
    /* No F_HEAP: views live in the arena, so csilk_json_free() on a view
     * is a safe no-op instead of freeing arena memory. */
    j->flags = 0;
    j->_pad = 0;
    return j;
}

csilk_json_t*
json_view_mutable(const csilk_json_t* src, yyjson_mut_val* mval)
{
    if (!mval || !src) {
        return NULL;
    }
    csilk_json_t* s = (csilk_json_t*)(uintptr_t)src;
    csilk_json_t* j = json_va_alloc(s, sizeof(*j));
    if (!j) {
        return NULL;
    }
    j->u.mval = mval;
    j->doc.mdoc = src->doc.mdoc;
    atomic_init(&j->va, atomic_load(&s->va));
    j->flags = CSILK_JSON_F_MUTABLE;
    j->_pad = 0;
    return j;
}

/* Free the view arena rooted at @p v (whole-tree reclamation). Only the
 * root owner should call this — freeing a shared arena from a view would
 * leave the root's va dangling. */
void
json_va_release(csilk_json_t* v)
{
    struct json_view_arena* va = atomic_exchange(&v->va, NULL);
    if (!va) {
        return;
    }
    struct json_va_chunk* c = atomic_load(&va->head);
    while (c) {
        struct json_va_chunk* next = c->next;
        free(c);
        c = next;
    }
    free(va);
}
