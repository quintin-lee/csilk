/**
 * @file server_rcu.c
 * @brief Router RCU (Read-Copy-Update) management — hot reload safe access.
 *
 * Implements the RCU router mechanism: thread-local slot acquisition,
 * epoch-based reader tracking, retired router reclamation, and the
 * csilk_server_set_router* family. Called exclusively by lifecycle and
 * http1_parse paths; declared via srv_internal.h.
 *
 * @copyright MIT License
 */

#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _WIN32
#include <sched.h>
#include <unistd.h>
#include <dlfcn.h>
#else
#include <windows.h>
#endif

#include "../ctx/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/core/sync.h"
#include "csilk/core/hot_reload.h"
#include "../internal/srv_internal.h"
#include "../internal/srv_impl.h"
#include "csilk/messaging/mq.h"
csilk_mq_t*
csilk_server_get_mq(csilk_server_t* server)
{
    if (!server) {
        return NULL;
    }
    csilk_mq_t* mq = atomic_load_explicit(&server->mq, memory_order_acquire);
    if (!mq) {
        csilk_mutex_lock(&server->config_mutex);
        mq = atomic_load_explicit(&server->mq, memory_order_relaxed);
        if (!mq) {
            mq = _csilk_mq_new(server->loop);
            atomic_store_explicit(&server->mq, mq, memory_order_release);
        }
        csilk_mutex_unlock(&server->config_mutex);
    }
    return mq;
}

/** @brief Get the server's active radix-tree router atomically. */
csilk_router_t*
csilk_server_get_router(csilk_server_t* server)
{
    return server ? atomic_load_explicit(&server->router, memory_order_acquire) : NULL;
}

static _Atomic(uint32_t) g_rcu_server_gen_seq = 1;

typedef struct {
    csilk_rcu_slot_t*   slot;
    csilk_reload_mgr_t* mgr;
    uint32_t            server_gen;
} csilk_tls_rcu_t;

static _Thread_local csilk_tls_rcu_t g_tls_rcu = {0};
static pthread_key_t                 g_rcu_tls_key;
static pthread_once_t                g_rcu_tls_once = PTHREAD_ONCE_INIT;

static void
rcu_thread_exit_destructor(void* val)
{
    (void)val;
    if (g_tls_rcu.slot) {
        csilk_rcu_slot_t* slot = g_tls_rcu.slot;
        atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
        atomic_store_explicit(&slot->nesting_depth, 0, memory_order_relaxed);
        atomic_store_explicit(&slot->owner_tid, 0, memory_order_release);

        g_tls_rcu.slot = NULL;
        g_tls_rcu.mgr = NULL;
        g_tls_rcu.server_gen = 0;
    }
}

static void
rcu_init_tls_key(void)
{
    pthread_key_create(&g_rcu_tls_key, rcu_thread_exit_destructor);
}

static inline void
ensure_rcu_tls_registered(void)
{
    pthread_once(&g_rcu_tls_once, rcu_init_tls_key);
    if (!pthread_getspecific(g_rcu_tls_key)) {
        pthread_setspecific(g_rcu_tls_key, (void*)1);
    }
}

CSILK_INTERNAL void
_csilk_reload_mgr_init(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;
    atomic_init(&mgr->global_epoch, 1);
    atomic_init(&mgr->reclaim_lock, 0);
    atomic_init(&mgr->retired_count, 0);
    atomic_init(&mgr->retired_head, NULL);
    atomic_init(&mgr->overflow_head, NULL);

    uint32_t gen = atomic_fetch_add_explicit(&g_rcu_server_gen_seq, 1, memory_order_relaxed);
    if (gen == 0) {
        gen = atomic_fetch_add_explicit(&g_rcu_server_gen_seq, 1, memory_order_relaxed);
    }
    mgr->server_gen = gen;

    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        atomic_init(&mgr->reader_slots[i].active_epoch, 0);
        atomic_init(&mgr->reader_slots[i].owner_tid, 0);
        atomic_init(&mgr->reader_slots[i].nesting_depth, 0);
        atomic_init(&mgr->reader_slots[i].next_overflow, NULL);
        mgr->reader_slots[i].owner_mgr = mgr;
        mgr->reader_slots[i].server_gen = gen;
        mgr->reader_slots[i].is_dynamic = false;
    }
}

static csilk_rcu_slot_t*
acquire_rcu_slot_slow(csilk_reload_mgr_t* mgr)
{
    ensure_rcu_tls_registered();

    uintptr_t my_tid = (uintptr_t)pthread_self();
    if (my_tid == 0) {
        my_tid = 1;
    }

    size_t start = (size_t)(my_tid % CSILK_RELOAD_MAX_READERS);

    /* 1. Fast path: check if this thread already owns a static slot */
    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        size_t idx = (start + i) % CSILK_RELOAD_MAX_READERS;
        if (atomic_load_explicit(&mgr->reader_slots[idx].owner_tid, memory_order_relaxed) ==
            my_tid) {
            csilk_rcu_slot_t* s = &mgr->reader_slots[idx];
            s->owner_mgr = mgr;
            s->server_gen = mgr->server_gen;
            s->is_dynamic = false;
            g_tls_rcu.slot = s;
            g_tls_rcu.mgr = mgr;
            g_tls_rcu.server_gen = mgr->server_gen;
            return s;
        }
    }

    /* 2. Slow path: claim an unused static slot */
    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        size_t    idx = (start + i) % CSILK_RELOAD_MAX_READERS;
        uintptr_t expected = 0;
        if (atomic_compare_exchange_strong_explicit(&mgr->reader_slots[idx].owner_tid,
                                                    &expected,
                                                    my_tid,
                                                    memory_order_acq_rel,
                                                    memory_order_relaxed)) {
            csilk_rcu_slot_t* s = &mgr->reader_slots[idx];
            atomic_store_explicit(&s->active_epoch, 0, memory_order_relaxed);
            atomic_store_explicit(&s->nesting_depth, 0, memory_order_relaxed);
            s->owner_mgr = mgr;
            s->server_gen = mgr->server_gen;
            s->is_dynamic = false;
            g_tls_rcu.slot = s;
            g_tls_rcu.mgr = mgr;
            g_tls_rcu.server_gen = mgr->server_gen;
            return s;
        }
    }

    /* 3. Static slots saturated (>256 concurrent readers): check dynamic overflow slots */
    csilk_rcu_slot_t* cur_ov = atomic_load_explicit(&mgr->overflow_head, memory_order_acquire);
    while (cur_ov) {
        if (atomic_load_explicit(&cur_ov->owner_tid, memory_order_relaxed) == my_tid) {
            cur_ov->owner_mgr = mgr;
            cur_ov->server_gen = mgr->server_gen;
            g_tls_rcu.slot = cur_ov;
            g_tls_rcu.mgr = mgr;
            g_tls_rcu.server_gen = mgr->server_gen;
            return cur_ov;
        }
        uintptr_t exp = 0;
        if (atomic_compare_exchange_strong_explicit(
                &cur_ov->owner_tid, &exp, my_tid, memory_order_acq_rel, memory_order_relaxed)) {
            atomic_store_explicit(&cur_ov->active_epoch, 0, memory_order_relaxed);
            atomic_store_explicit(&cur_ov->nesting_depth, 0, memory_order_relaxed);
            cur_ov->owner_mgr = mgr;
            cur_ov->server_gen = mgr->server_gen;
            g_tls_rcu.slot = cur_ov;
            g_tls_rcu.mgr = mgr;
            g_tls_rcu.server_gen = mgr->server_gen;
            return cur_ov;
        }
        cur_ov = atomic_load_explicit(&cur_ov->next_overflow, memory_order_acquire);
    }

    /* 4. Allocate a dedicated new dynamic overflow slot */
    csilk_rcu_slot_t* new_slot = calloc(1, sizeof(csilk_rcu_slot_t));
    if (!new_slot) {
        return NULL;
    }
    atomic_init(&new_slot->active_epoch, 0);
    atomic_init(&new_slot->owner_tid, my_tid);
    atomic_init(&new_slot->nesting_depth, 0);
    atomic_init(&new_slot->next_overflow, NULL);
    new_slot->owner_mgr = mgr;
    new_slot->server_gen = mgr->server_gen;
    new_slot->is_dynamic = true;

    csilk_rcu_slot_t* old_head = atomic_load_explicit(&mgr->overflow_head, memory_order_relaxed);
    do {
        atomic_store_explicit(&new_slot->next_overflow, old_head, memory_order_relaxed);
    } while (!atomic_compare_exchange_weak_explicit(
        &mgr->overflow_head, &old_head, new_slot, memory_order_release, memory_order_relaxed));

    g_tls_rcu.slot = new_slot;
    g_tls_rcu.mgr = mgr;
    g_tls_rcu.server_gen = mgr->server_gen;
    return new_slot;
}

static inline csilk_rcu_slot_t*
acquire_rcu_slot(csilk_reload_mgr_t* mgr)
{
    if (__builtin_expect(g_tls_rcu.slot != NULL && g_tls_rcu.mgr == mgr &&
                             g_tls_rcu.server_gen == mgr->server_gen,
                         1)) {
        return g_tls_rcu.slot;
    }
    return acquire_rcu_slot_slow(mgr);
}

csilk_router_t*
csilk_server_router_acquire(csilk_server_t* server, csilk_rcu_token_t* token)
{
    if (!server) {
        if (token) {
            token->active = 0;
            token->slot = NULL;
            token->epoch = 0;
        }
        return NULL;
    }

    csilk_reload_mgr_t* mgr = &server->reload_mgr;
    csilk_rcu_slot_t*   slot = acquire_rcu_slot(mgr);
    if (!slot) {
        if (token) {
            token->active = 0;
            token->slot = NULL;
            token->epoch = 0;
        }
        return atomic_load_explicit(&server->router, memory_order_acquire);
    }

    uint32_t depth = atomic_load_explicit(&slot->nesting_depth, memory_order_relaxed);
    atomic_store_explicit(&slot->nesting_depth, depth + 1, memory_order_relaxed);
    uint64_t epoch = 0;
    if (depth == 0) {
        epoch = atomic_load_explicit(&mgr->global_epoch, memory_order_acquire);
        atomic_store_explicit(&slot->active_epoch, epoch, memory_order_release);
        atomic_thread_fence(memory_order_seq_cst);
    } else {
        epoch = atomic_load_explicit(&slot->active_epoch, memory_order_relaxed);
    }

    if (token) {
        token->slot = slot;
        token->epoch = epoch;
        token->active = 1;
    }

    return atomic_load_explicit(&server->router, memory_order_acquire);
}

void
csilk_server_router_release(csilk_server_t* server, csilk_rcu_token_t* token)
{
    if (!token || !token->active || !token->slot) {
        return;
    }
    (void)server;
    csilk_rcu_slot_t* slot = (csilk_rcu_slot_t*)token->slot;
    token->active = 0;
    token->slot = NULL;
    token->epoch = 0;

    uint32_t depth = atomic_load_explicit(&slot->nesting_depth, memory_order_relaxed);
    if (depth > 0) {
        atomic_store_explicit(&slot->nesting_depth, depth - 1, memory_order_relaxed);
    }
    if (depth <= 1) {
        atomic_store_explicit(&slot->active_epoch, 0, memory_order_release);
    }
}

CSILK_INTERNAL void
_csilk_reload_try_reclaim(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;

    uint32_t expected = 0;
    if (!atomic_compare_exchange_strong_explicit(
            &mgr->reclaim_lock, &expected, 1, memory_order_acquire, memory_order_relaxed)) {
        return; /* Another thread is currently performing reclamation */
    }

    uint64_t current_epoch = atomic_load_explicit(&mgr->global_epoch, memory_order_acquire);
    uint64_t min_active_epoch = UINT64_MAX;
    bool     has_active_readers = false;

    /* Scan static reader slots */
    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        uint64_t r_epoch =
            atomic_load_explicit(&mgr->reader_slots[i].active_epoch, memory_order_acquire);
        if (r_epoch != 0) {
            has_active_readers = true;
            if (r_epoch < min_active_epoch) {
                min_active_epoch = r_epoch;
            }
        }
    }

    /* Scan dynamic overflow slots */
    csilk_rcu_slot_t* ov = atomic_load_explicit(&mgr->overflow_head, memory_order_acquire);
    while (ov) {
        uint64_t r_epoch = atomic_load_explicit(&ov->active_epoch, memory_order_acquire);
        if (r_epoch != 0) {
            has_active_readers = true;
            if (r_epoch < min_active_epoch) {
                min_active_epoch = r_epoch;
            }
        }
        ov = atomic_load_explicit(&ov->next_overflow, memory_order_acquire);
    }

    if (!has_active_readers) {
        min_active_epoch = current_epoch + 2;
    }

    csilk_retired_router_t* list =
        atomic_exchange_explicit(&mgr->retired_head, NULL, memory_order_acq_rel);
    csilk_retired_router_t* retain_head = NULL;
    uint32_t                retained_count = 0;

    while (list) {
        csilk_retired_router_t* next =
            atomic_load_explicit(&list->retired_next, memory_order_relaxed);
        if (list->retired_epoch < min_active_epoch) {
            /* Safe to free: no active reader can reach this router or dynamic library */
            if (list->router) {
                csilk_router_free(list->router);
            }
            if (list->dl_handle) {
#ifndef _WIN32
                dlclose(list->dl_handle);
#else
                FreeLibrary((HMODULE)list->dl_handle);
#endif
            }
            if (list->tmp_path) {
                unlink(list->tmp_path);
                free(list->tmp_path);
            }
            free(list);
        } else {
            atomic_store_explicit(&list->retired_next, retain_head, memory_order_relaxed);
            retain_head = list;
            retained_count++;
        }
        list = next;
    }

    if (retain_head) {
        csilk_retired_router_t* tail = retain_head;
        while (atomic_load_explicit(&tail->retired_next, memory_order_relaxed) != NULL) {
            tail = atomic_load_explicit(&tail->retired_next, memory_order_relaxed);
        }
        csilk_retired_router_t* cur_retired =
            atomic_load_explicit(&mgr->retired_head, memory_order_relaxed);
        do {
            atomic_store_explicit(&tail->retired_next, cur_retired, memory_order_relaxed);
        } while (!atomic_compare_exchange_weak_explicit(&mgr->retired_head,
                                                        &cur_retired,
                                                        retain_head,
                                                        memory_order_release,
                                                        memory_order_relaxed));
    }

    atomic_store_explicit(&mgr->retired_count, retained_count, memory_order_relaxed);
    atomic_store_explicit(&mgr->reclaim_lock, 0, memory_order_release);
}

void
csilk_server_set_router_full(csilk_server_t* server,
                             csilk_router_t* router,
                             void*           dl_handle,
                             const char*     tmp_path)
{
    if (!server || !router) {
        return;
    }

    if (server->middleware_count > 0) {
        csilk_router_compile(router, server->middlewares, (size_t)server->middleware_count);
    }

    csilk_reload_mgr_t* mgr = &server->reload_mgr;

    csilk_mutex_lock(&server->config_mutex);

    /* Atomically swap new router into place */
    csilk_router_t* old_router =
        atomic_exchange_explicit(&server->router, router, memory_order_acq_rel);

    /* Monotonically advance global reload epoch */
    uint64_t retired_epoch = atomic_fetch_add_explicit(&mgr->global_epoch, 1, memory_order_acq_rel);

    if (old_router || dl_handle || tmp_path) {
        csilk_retired_router_t* rec = calloc(1, sizeof(csilk_retired_router_t));
        if (rec) {
            rec->router = old_router;
            rec->dl_handle = dl_handle;
            rec->tmp_path = tmp_path ? strdup(tmp_path) : NULL;
            rec->retired_epoch = retired_epoch;
            atomic_init(&rec->retired_next, NULL);

            csilk_retired_router_t* old_head =
                atomic_load_explicit(&mgr->retired_head, memory_order_relaxed);
            do {
                atomic_store_explicit(&rec->retired_next, old_head, memory_order_relaxed);
            } while (!atomic_compare_exchange_weak_explicit(
                &mgr->retired_head, &old_head, rec, memory_order_release, memory_order_relaxed));

            atomic_fetch_add_explicit(&mgr->retired_count, 1, memory_order_relaxed);
        }
    }

    /* Opportunistic non-blocking reclamation */
    _csilk_reload_try_reclaim(server);

    csilk_mutex_unlock(&server->config_mutex);
}

void
csilk_server_set_router_ex(csilk_server_t* server, csilk_router_t* router, void* dl_handle)
{
    csilk_server_set_router_full(server, router, dl_handle, NULL);
}

void
csilk_server_set_router(csilk_server_t* server, csilk_router_t* router)
{
    csilk_server_set_router_full(server, router, NULL, NULL);
}

void
csilk_server_wait_grace_period(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;
    uint64_t target_epoch = atomic_load_explicit(&mgr->global_epoch, memory_order_acquire);

    while (1) {
        bool all_done = true;
        for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
            uint64_t r_epoch =
                atomic_load_explicit(&mgr->reader_slots[i].active_epoch, memory_order_acquire);
            if (r_epoch != 0 && r_epoch <= target_epoch) {
                all_done = false;
                break;
            }
        }
        if (all_done) {
            csilk_rcu_slot_t* ov = atomic_load_explicit(&mgr->overflow_head, memory_order_acquire);
            while (ov) {
                uint64_t r_epoch = atomic_load_explicit(&ov->active_epoch, memory_order_acquire);
                if (r_epoch != 0 && r_epoch <= target_epoch) {
                    all_done = false;
                    break;
                }
                ov = atomic_load_explicit(&ov->next_overflow, memory_order_acquire);
            }
        }
        if (all_done) {
            break;
        }
        sched_yield();
        usleep(1000); /* 1 ms */
    }

    _csilk_reload_try_reclaim(server);
}

CSILK_INTERNAL void
_csilk_reload_mgr_free(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    csilk_reload_mgr_t* mgr = &server->reload_mgr;

    /* Invalidate calling thread's cached TLS pointer if it refers to this server */
    if (g_tls_rcu.mgr == mgr) {
        g_tls_rcu.slot = NULL;
        g_tls_rcu.mgr = NULL;
        g_tls_rcu.server_gen = 0;
    }

    /* Wait for all static readers to exit */
    for (size_t i = 0; i < CSILK_RELOAD_MAX_READERS; i++) {
        while (atomic_load_explicit(&mgr->reader_slots[i].active_epoch, memory_order_acquire) !=
               0) {
            sched_yield();
        }
        atomic_store_explicit(&mgr->reader_slots[i].owner_tid, 0, memory_order_relaxed);
    }

    /* Wait for all overflow dynamic readers to exit */
    csilk_rcu_slot_t* ov = atomic_load_explicit(&mgr->overflow_head, memory_order_acquire);
    while (ov) {
        while (atomic_load_explicit(&ov->active_epoch, memory_order_acquire) != 0) {
            sched_yield();
        }
        atomic_store_explicit(&ov->owner_tid, 0, memory_order_relaxed);
        ov = atomic_load_explicit(&ov->next_overflow, memory_order_acquire);
    }

    /* Invalidate server_gen so other threads will not match */
    mgr->server_gen = 0;

    /* Free all dynamic overflow slots */
    ov = atomic_exchange_explicit(&mgr->overflow_head, NULL, memory_order_acq_rel);
    while (ov) {
        csilk_rcu_slot_t* next = atomic_load_explicit(&ov->next_overflow, memory_order_relaxed);
        free(ov);
        ov = next;
    }

    /* Reclaim all retired entries unconditionally */
    csilk_retired_router_t* list =
        atomic_exchange_explicit(&mgr->retired_head, NULL, memory_order_acq_rel);
    while (list) {
        csilk_retired_router_t* next =
            atomic_load_explicit(&list->retired_next, memory_order_relaxed);
        if (list->router) {
            csilk_router_free(list->router);
        }
        if (list->dl_handle) {
#ifndef _WIN32
            dlclose(list->dl_handle);
#else
            FreeLibrary((HMODULE)list->dl_handle);
#endif
        }
        if (list->tmp_path) {
            unlink(list->tmp_path);
            free(list->tmp_path);
        }
        free(list);
        list = next;
    }
}
