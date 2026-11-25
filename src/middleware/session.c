/**
 * @file session.c
 * @brief Cookie-based session management middleware with sharded hash table
 * storage and background expiry sweep.
 *
 * The session store is a 1024-bucket sharded hash table; each bucket has its
 * own mutex so concurrent lookups on different session IDs do not contend.
 * A global atomic counter tracks total session count for LRU eviction when
 * csilk_session_set_max() has been called.
 *
 * All store state (buckets, counters, limits, init guard) is converged into a
 * single heap-allocated session_store_t reached via one global pointer
 * (g_session_store). The pointer is lazily initialized on first use and stays
 * alive for the process lifetime, which preserves the public API's
 * server-less behavior (csilk_session_start() works on a bare context) while
 * eliminating the scattering of independent statics.
 *
 * @copyright MIT License
 */
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "csilk/core/sync.h"
#include "csilk/core/internal.h"
#include "csilk/core/middleware.h"
#include "core/internal/srv_internal.h"

/** @brief Default session TTL in seconds. */
enum { SESSION_TTL = 3600 };

/** @brief Number of hash buckets in the sharded session store. */
#define SESSION_BUCKET_COUNT 1024

/** @brief Maximum number of active sessions before LRU eviction. */
#define SESSION_MAX_DEFAULT 10000

/**
 * @brief Session data item (key-value pair).
 *
 * Each session contains a singly-linked list of these items, each holding
 * a string key and an opaque void* value.
 */
typedef struct csilk_session_data_s {
    char*                          key;
    void*                          value;
    size_t                         value_len; /* 0 = caller-managed size */
    csilk_session_value_cleanup_fn cleanup;   /* optional; NULL = caller-owned */
    struct csilk_session_data_s*   next;
} csilk_session_data_t;

/**
 * @brief Session structure.
 *
 * Represents an individual session with a UUID-based ID, an expiry timestamp,
 * a linked list of key-value data items, and a pointer to the next session
 * in the same hash bucket (singly-linked list).
 */
typedef struct csilk_session_s {
    char                    id[CSILK_UUID_BUF_SIZE];
    csilk_session_data_t*   data;
    time_t                  expires_at;
    _Atomic time_t          last_access; /* LRU tracking */
    struct csilk_session_s* next;        /* next in same bucket */
    _Atomic int             ref_count;
    atomic_bool             removed;
    csilk_mutex_t           lock;
} csilk_session_t;

/**
 * @brief Per-bucket state in the sharded session store.
 *
 * Each bucket holds a mutex protecting its linked list head, so concurrent
 * lookups on different session IDs do not block each other.
 */
typedef struct {
    csilk_session_t* head;
    csilk_mutex_t    mutex;
} session_bucket_t;

/**
 * @brief Converged session store state.
 *
 * Holds every piece of process-level session subsystem state that was
 * previously scattered as independent statics (bucket array, counters,
 * limit, init guard). Lazily allocated once via g_session_store.
 */
typedef struct {
    session_bucket_t buckets[SESSION_BUCKET_COUNT];
    csilk_once_t     init_once;
    _Atomic size_t   session_count;
    _Atomic size_t   session_max;
} session_store_t;

/** @brief Single global pointer to the lazily-initialized session store. */
static session_store_t* g_session_store = NULL;

/**
 * @brief Initialize all bucket mutexes (called once via csilk_once).
 */
static void
init_session_buckets(void)
{
    session_store_t* store = g_session_store;
    for (size_t i = 0; i < SESSION_BUCKET_COUNT; i++) {
        store->buckets[i].head = NULL;
        csilk_mutex_init(&store->buckets[i].mutex);
    }
    CSILK_LOG_I("Session: Sharded store initialized (%zu buckets)", (size_t)SESSION_BUCKET_COUNT);
}

/**
 * @brief Lazily obtain the global session store, allocating and initializing
 * it on first use. The store persists for the process lifetime.
 *
 * @return The global session store (never NULL on success).
 */
static session_store_t*
session_store_ensure(void)
{
    session_store_t* store = g_session_store;
    if (!store) {
        store = calloc(1, sizeof(session_store_t));
        if (!store) {
            CSILK_LOG_E("Session: failed to allocate session store");
            return NULL;
        }
        csilk_once_init(&store->init_once);
        atomic_init(&store->session_count, 0);
        atomic_init(&store->session_max, SESSION_MAX_DEFAULT);
        g_session_store = store;
    }
    csilk_once(&store->init_once, init_session_buckets);
    return store;
}

/**
 * @brief Lazily obtain the per-server session store, allocating and
 * initializing it on first use. The store is owned by the server and freed
 * by _csilk_session_store_free() at server teardown.
 *
 * @param server  The owning server (must have a non-NULL session_store slot).
 * @return The per-server session store (never NULL on success).
 */
static session_store_t*
session_store_ensure_server(csilk_server_t* server)
{
    session_store_t* store = (session_store_t*)server->session_store;
    if (!store) {
        store = calloc(1, sizeof(session_store_t));
        if (!store) {
            CSILK_LOG_E("Session: failed to allocate per-server session store");
            return NULL;
        }
        csilk_once_init(&store->init_once);
        atomic_init(&store->session_count, 0);
        atomic_init(&store->session_max, SESSION_MAX_DEFAULT);
        server->session_store = store;
    }
    csilk_once(&store->init_once, init_session_buckets);
    return store;
}

/**
 * @brief Resolve the session store for a given request context.
 *
 * Prefers the per-server store when the context is bound to a server, falling
 * back to the process-level global store for server-less contexts (tests).
 *
 * @param c  The request context.
 * @return The session store to use (never NULL on success).
 */
static session_store_t*
session_store_for_ctx(csilk_ctx_t* c)
{
    csilk_server_t* server = csilk_ctx_get_server(c);
    if (server) {
        return session_store_ensure_server(server);
    }
    return session_store_ensure();
}

/**
 * @brief Compute the FNV-1a hash of a session ID string.
 *
 * Maps to a bucket index in [0, SESSION_BUCKET_COUNT).
 */
static size_t
session_hash(const char* id)
{
    uint64_t h = 14695981039346656037ULL; /* FNV offset basis */
    for (const char* p = id; *p; p++) {
        h ^= (uint8_t)*p;
        h *= 1099511628211ULL;            /* FNV prime */
    }
    return (size_t)(h % SESSION_BUCKET_COUNT);
}

/**
 * @brief Lock a specific bucket by index.
 */
static inline void
session_bucket_lock(session_store_t* store, size_t idx)
{
    csilk_mutex_lock(&store->buckets[idx].mutex);
}

/**
 * @brief Unlock a specific bucket by index.
 */
static inline void
session_bucket_unlock(session_store_t* store, size_t idx)
{
    csilk_mutex_unlock(&store->buckets[idx].mutex);
}

/** @brief Session cookie name. */
#define SESSION_COOKIE "csilk_session"

/**
 * @brief Generate a cryptographically random session ID.
 *
 * Delegates to _csilk_generate_uuid() to produce a UUID v4 string and
 * writes it into the provided 37-character buffer.
 *
 * @param c   The request context.
 * @param id  Output buffer of at least 37 bytes to receive the
 *            null-terminated UUID string.
 */
static void
generate_session_id(csilk_ctx_t* c, char id[CSILK_UUID_BUF_SIZE])
{
    _csilk_generate_uuid(c, id);
}

/**
 * @brief Find a session by ID in the sharded store (bucket-locked).
 *
 * @param id  The session ID string to search for.
 * @return Pointer to the csilk_session_t if found, NULL otherwise.
 */
static void
free_session(csilk_session_t* session)
{
    if (!session) {
        return;
    }
    csilk_session_data_t* d = session->data;
    while (d) {
        csilk_session_data_t* next = d->next;
        if (d->cleanup) {
            d->cleanup(d->value);
        }
        free(d->key);
        free(d);
        d = next;
    }
    csilk_mutex_destroy(&session->lock);
    free(session);
}

static inline void
session_ref(csilk_session_t* s)
{
    if (s) {
        atomic_fetch_add_explicit(&s->ref_count, 1, memory_order_relaxed);
    }
}

static inline void
session_unref(csilk_session_t* s)
{
    if (!s) {
        return;
    }
    if (atomic_fetch_sub_explicit(&s->ref_count, 1, memory_order_acq_rel) == 1) {
        free_session(s);
    }
}

static void
_session_ctx_release(void* ptr)
{
    session_unref((csilk_session_t*)ptr);
}

/**
 * @brief Free a server's per-server session store. Called from csilk_server_free().
 *
 * Walks all buckets, unrefing each session (drops the store membership ref; the
 * session is freed when its ref count reaches zero). Safe to call when the
 * store is NULL (no-op). Must be called after all in-flight contexts have been
 * drained so no context ref outlives the store.
 */
void
_csilk_session_store_free(csilk_server_t* server)
{
    session_store_t* store = (session_store_t*)server->session_store;
    if (!store) {
        return;
    }
    for (size_t i = 0; i < SESSION_BUCKET_COUNT; i++) {
        csilk_session_t* session = store->buckets[i].head;
        store->buckets[i].head = NULL;
        while (session) {
            csilk_session_t* next = session->next;
            session->next = NULL;
            session_unref(session); /* Drop store membership ref */
            session = next;
        }
    }
    free(store);
    server->session_store = NULL;
}

static csilk_session_t*
find_session_locked(session_store_t* store, const char* id)
{
    if (!store) {
        return NULL;
    }
    size_t idx = session_hash(id);
    time_t now = time(NULL);
    session_bucket_lock(store, idx);
    csilk_session_t** prev = &store->buckets[idx].head;
    csilk_session_t*  s = store->buckets[idx].head;
    while (s) {
        if (strcmp(s->id, id) == 0) {
            if (s->expires_at <= now) {
                *prev = s->next;
                s->next = NULL;
                bool was_removed =
                    atomic_exchange_explicit(&s->removed, true, memory_order_acq_rel);
                session_bucket_unlock(store, idx);
                if (!was_removed) {
                    atomic_fetch_sub_explicit(&store->session_count, 1, memory_order_relaxed);
                    session_unref(s);
                }
                return NULL;
            }
            atomic_store_explicit(&s->last_access, now, memory_order_relaxed);
            s->expires_at = now + SESSION_TTL;
            session_ref(s);
            session_bucket_unlock(store, idx);
            return s;
        }
        prev = &s->next;
        s = s->next;
    }
    session_bucket_unlock(store, idx);
    return NULL;
}

static void
add_session_locked(session_store_t* store, csilk_session_t* session)
{
    if (!store) {
        return;
    }
    time_t now = time(NULL);
    atomic_store_explicit(&session->last_access, now, memory_order_relaxed);
    size_t idx = session_hash(session->id);
    session_bucket_lock(store, idx);
    session_ref(session); /* Store membership reference */
    session->next = store->buckets[idx].head;
    store->buckets[idx].head = session;
    session_bucket_unlock(store, idx);
    atomic_fetch_add_explicit(&store->session_count, 1, memory_order_relaxed);

    /* LRU eviction: if over limit, find and evict the least-recently-accessed session */
    size_t max_lim = atomic_load_explicit(&store->session_max, memory_order_relaxed);
    if (max_lim > 0 &&
        atomic_load_explicit(&store->session_count, memory_order_relaxed) > max_lim) {
        csilk_session_t* victim = NULL;
        time_t           victim_ts = 0;
        for (size_t b = 0; b < SESSION_BUCKET_COUNT; b++) {
            session_bucket_lock(store, b);
            for (csilk_session_t* s = store->buckets[b].head; s; s = s->next) {
                if (s == session) {
                    continue;
                }
                time_t ts = atomic_load_explicit(&s->last_access, memory_order_relaxed);
                if (victim == NULL || ts < victim_ts) {
                    if (victim) {
                        session_unref(victim);
                    }
                    victim_ts = ts;
                    victim = s;
                    session_ref(victim);
                }
            }
            session_bucket_unlock(store, b);
        }
        if (victim) {
            char victim_id[CSILK_UUID_BUF_SIZE];
            strncpy(victim_id, victim->id, sizeof(victim_id) - 1);
            victim_id[sizeof(victim_id) - 1] = '\0';

            size_t vidx = session_hash(victim_id);
            session_bucket_lock(store, vidx);
            csilk_session_t** prev = &store->buckets[vidx].head;
            csilk_session_t*  s = store->buckets[vidx].head;
            bool              unlinked = false;
            while (s) {
                if (s == victim) {
                    *prev = s->next;
                    s->next = NULL;
                    unlinked =
                        !atomic_exchange_explicit(&victim->removed, true, memory_order_acq_rel);
                    break;
                }
                prev = &s->next;
                s = s->next;
            }
            session_bucket_unlock(store, vidx);
            if (unlinked) {
                atomic_fetch_sub_explicit(&store->session_count, 1, memory_order_relaxed);
                session_unref(victim); /* Drop store ref */
                CSILK_LOG_D("Session: LRU-evicted session '%s'", victim_id);
            }
            session_unref(victim);     /* Drop scan ref */
        }
    }
}

static void
remove_session_locked(session_store_t* store, csilk_session_t* session)
{
    if (!store) {
        return;
    }
    size_t idx = session_hash(session->id);
    session_bucket_lock(store, idx);
    csilk_session_t** prev = &store->buckets[idx].head;
    csilk_session_t*  s = store->buckets[idx].head;
    bool              unlinked = false;
    while (s) {
        if (s == session) {
            *prev = s->next;
            s->next = NULL;
            unlinked = !atomic_exchange_explicit(&session->removed, true, memory_order_acq_rel);
            break;
        }
        prev = &s->next;
        s = s->next;
    }
    session_bucket_unlock(store, idx);
    if (unlinked) {
        atomic_fetch_sub_explicit(&store->session_count, 1, memory_order_relaxed);
        session_unref(session); /* Drop store membership ref */
    }
}

static void
cleanup_expired(session_store_t* store)
{
    if (!store) {
        return;
    }
    time_t now = time(NULL);
    for (size_t b = 0; b < SESSION_BUCKET_COUNT; b++) {
        csilk_session_t* expired_list = NULL;

        session_bucket_lock(store, b);
        csilk_session_t** prev = &store->buckets[b].head;
        csilk_session_t*  s = store->buckets[b].head;
        while (s) {
            if (s->expires_at <= now) {
                *prev = s->next;
                if (!atomic_exchange_explicit(&s->removed, true, memory_order_acq_rel)) {
                    atomic_fetch_sub_explicit(&store->session_count, 1, memory_order_relaxed);
                    s->next = expired_list;
                    expired_list = s;
                }
                s = *prev;
            } else {
                prev = &s->next;
                s = s->next;
            }
        }
        session_bucket_unlock(store, b);

        while (expired_list) {
            csilk_session_t* cur = expired_list;
            expired_list = cur->next;
            cur->next = NULL;
            CSILK_LOG_D("Session: Cleaning up expired session '%s'", cur->id);
            session_unref(cur);
        }
    }
}

/**
 * @brief Set the maximum number of active sessions.
 *
 * When the count exceeds this limit the least-recently-accessed session is
 * evicted. A value of 0 disables the limit (infinite sessions).
 *
 * @param max  Maximum session count (0 = unlimited).
 */
void
csilk_session_set_max(size_t max)
{
    session_store_t* store = session_store_ensure();
    if (!store) {
        return;
    }
    atomic_store_explicit(&store->session_max, max, memory_order_relaxed);
    CSILK_LOG_I("Session: Max sessions set to %zu", max);
}

/**
 * @brief Initialize the session system.
 *
 * Performs an immediate cleanup of any expired sessions from the store.
 * This should be called once during server startup.
 */
void
csilk_session_init(void)
{
    session_store_t* store = session_store_ensure();
    if (!store) {
        return;
    }
    CSILK_LOG_I("Session: Initializing session subsystem");
    cleanup_expired(store);
}

/**
 * @brief Initialize the per-server session store for a specific server.
 *
 * Allocates the session store on the given server so that each server
 * maintains independent session state. The store is freed at server
 * teardown via _csilk_session_store_free().
 *
 * @param server  The server whose session store should be initialized.
 */
void
csilk_session_init_server(csilk_server_t* server)
{
    if (!server) {
        return;
    }
    session_store_t* store = session_store_ensure_server(server);
    if (!store) {
        return;
    }
    CSILK_LOG_I("Session: Initializing per-server session store for server %p", (void*)server);
    cleanup_expired(store);
}

/**
 * @brief Start or resume a session for the current request.
 *
 * Looks for an existing session cookie named "csilk_session". If a valid
 * session is found, its expiry is extended by SESSION_TTL seconds. If not,
 * a new session is created with a fresh UUID, stored in the global list,
 * and a session cookie is set on the response. The session pointer is stored
 * in the context under the key "_session".
 *
 * @param c  The request context.
 *
 * @note Must be called after csilk_request_id_middleware (or equivalent)
 *       so that _csilk_generate_uuid is available.
 * @warning The session's data items are NOT automatically serialized to
 *          persistent storage. All sessions are in-memory and are lost on
 *          process restart.
 */
void
csilk_session_start(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    CSILK_LOG_T("Session: Starting session check for request %p", (void*)c);

    /* Look for an existing session cookie. If found and valid, resume it.
     Otherwise create a fresh session with a new UUID. */
    const char*      sid = csilk_get_cookie(c, SESSION_COOKIE);
    csilk_session_t* session = NULL;

    session_store_t* store = session_store_for_ctx(c);
    if (!store) {
        return;
    }

    if (sid) {
        session = find_session_locked(store, sid);
    }

    if (!session) {
        session = calloc(1, sizeof(csilk_session_t));
        if (!session) {
            CSILK_LOG_E("Session: Memory allocation failed for new session");
            return;
        }

        csilk_mutex_init(&session->lock);
        atomic_init(&session->ref_count, 1); /* Request context ref */
        atomic_init(&session->removed, false);
        generate_session_id(c, session->id);
        session->expires_at = time(NULL) + SESSION_TTL;

        add_session_locked(store, session);

        CSILK_LOG_D("Session: Created new session '%s' for request %p", session->id, (void*)c);
        csilk_set_cookie(c, SESSION_COOKIE, session->id, 60 * 60 * 24, "/", NULL, 1, 1);
    } else {
        CSILK_LOG_D("Session: Resumed existing session '%s' for request %p", session->id, (void*)c);
    }

    csilk_set_ex(c, "_session", session, _session_ctx_release);
}

/**
 * @brief Store a value in the session.
 *
 * If the key already exists in the current session, its value is replaced.
 * Otherwise, a new key-value pair is prepended to the session's data list.
 *
 * @param c     The request context (used to look up the session via
 *              csilk_get(c, "_session")).
 * @param key   Null-terminated key string. Must not be NULL.
 * @param value Opaque pointer to the value to store. May be NULL.
 *
 * @note The key is duplicated via strdup(). The value pointer is stored
 *       as-is — the caller is responsible for managing its lifetime.
 * @warning This function does NOT acquire the session mutex. It operates
 *          on the session pointer stored in the per-request context, which
 *          is already exclusively owned by the current request handler.
 */
void
csilk_session_set(csilk_ctx_t* c, const char* key, void* value)
{
    csilk_session_set_len(c, key, value, 0, NULL);
}

/**
 * @brief Store a value in the session with an optional cleanup callback.
 *
 * Thin wrapper around csilk_session_set_len() with a value_len of 0.
 */
void
csilk_session_set_ex(csilk_ctx_t*                   c,
                     const char*                    key,
                     void*                          value,
                     csilk_session_value_cleanup_fn cleanup)
{
    csilk_session_set_len(c, key, value, 0, cleanup);
}

/**
 * @brief Store a value in the session with an explicit byte length and
 *        optional cleanup callback.
 *
 * Replaces the existing value for @p key (running the old value's cleanup if
 * one was registered) or appends a new entry.  The @p len parameter records
 * the byte size of the value so that csilk_session_get_ex() can return it to
 * the caller without a side-channel.
 */
void
csilk_session_set_len(csilk_ctx_t*                   c,
                      const char*                    key,
                      void*                          value,
                      size_t                         len,
                      csilk_session_value_cleanup_fn cleanup)
{
    if (!c || !key) {
        return;
    }

    csilk_session_t* session = csilk_get(c, "_session");
    if (!session) {
        return;
    }

    CSILK_LOG_T("Session: Storing key '%s' in session '%s' (len=%zu, cleanup=%s)",
                key,
                session->id,
                len,
                cleanup ? "yes" : "no");

    csilk_mutex_lock(&session->lock);
    csilk_session_data_t* d = session->data;
    while (d) {
        if (strcmp(d->key, key) == 0) {
            if (d->cleanup && d->value != value) {
                d->cleanup(d->value);
            }
            d->value = value;
            d->value_len = len;
            d->cleanup = cleanup;
            csilk_mutex_unlock(&session->lock);
            return;
        }
        d = d->next;
    }

    csilk_session_data_t* new_d = calloc(1, sizeof(csilk_session_data_t));
    if (!new_d) {
        csilk_mutex_unlock(&session->lock);
        CSILK_LOG_E("Session: Memory allocation failed for session key '%s'", key);
        return;
    }

    new_d->key = strdup(key);
    if (!new_d->key) {
        csilk_mutex_unlock(&session->lock);
        CSILK_LOG_E("Session: failed to duplicate key '%s'", key);
        free(new_d);
        return;
    }
    new_d->value = value;
    new_d->value_len = len;
    new_d->cleanup = cleanup;
    new_d->next = session->data;
    session->data = new_d;
    csilk_mutex_unlock(&session->lock);
}

/**
 * @brief Retrieve a value from the session.
 *
 * Searches the current session's data list for the given key and returns
 * its associated value.
 *
 * @param c   The request context (used to look up the session).
 * @param key Null-terminated key string. Must not be NULL.
 *
 * @return The value pointer previously stored with csilk_session_set(), or
 *         NULL if the key is not found or no session is active.
 */
void*
csilk_session_get(csilk_ctx_t* c, const char* key)
{
    if (!c || !key) {
        return NULL;
    }

    csilk_session_t* session = csilk_get(c, "_session");
    if (!session) {
        return NULL;
    }

    CSILK_LOG_T("Session: Retrieving key '%s' from session '%s'", key, session->id);

    csilk_mutex_lock(&session->lock);
    csilk_session_data_t* d = session->data;
    while (d) {
        if (strcmp(d->key, key) == 0) {
            void* v = d->value;
            csilk_mutex_unlock(&session->lock);
            return v;
        }
        d = d->next;
    }
    csilk_mutex_unlock(&session->lock);

    char s_key[128];
    snprintf(s_key, sizeof(s_key), "session:%s:%s", session->id, key);
    char* val = csilk_get_string(c, s_key);
    explicit_bzero(s_key, sizeof(s_key));
    if (val) {
        csilk_arena_t* arena = csilk_get_arena(c);
        if (arena) {
            char* arena_val = csilk_arena_strdup(arena, val);
            free(val);
            /* Cache it locally for subsequent gets */
            csilk_session_set(c, key, arena_val);
            return arena_val;
        }
        free(val);
    }

    return NULL;
}

/**
 * @brief Retrieve a value from the session with an optional size out-parameter.
 *
 * Symmetric to csilk_session_set_len(): when the value was stored with a
 * known byte size, the caller can retrieve both the pointer and its length
 * in one call without needing a side-channel.
 *
 * @param c        The request context (used to look up the session).
 * @param key      Null-terminated key string. Must not be NULL.
 * @param out_len  Optional; receives the stored value's byte size.  Pass
 *                 NULL to ignore.  Set to 0 when the key is not found.
 * @return The value pointer previously stored, or NULL if not found or no
 *         session is active.
 */
void*
csilk_session_get_ex(csilk_ctx_t* c, const char* key, size_t* out_len)
{
    if (!c || !key) {
        return NULL;
    }

    csilk_session_t* session = csilk_get(c, "_session");
    if (!session) {
        if (out_len) {
            *out_len = 0;
        }
        return NULL;
    }

    CSILK_LOG_T("Session: Retrieving key '%s' from session '%s'", key, session->id);

    csilk_mutex_lock(&session->lock);
    csilk_session_data_t* d = session->data;
    while (d) {
        if (strcmp(d->key, key) == 0) {
            void* v = d->value;
            if (out_len) {
                *out_len = d->value_len;
            }
            csilk_mutex_unlock(&session->lock);
            return v;
        }
        d = d->next;
    }
    csilk_mutex_unlock(&session->lock);

    /* Fallback to context storage, same as csilk_session_get */
    char s_key[128];
    snprintf(s_key, sizeof(s_key), "session:%s:%s", session->id, key);
    char* val = csilk_get_string(c, s_key);
    explicit_bzero(s_key, sizeof(s_key));
    if (val) {
        csilk_arena_t* arena = csilk_get_arena(c);
        if (arena) {
            char* arena_val = csilk_arena_strdup(arena, val);
            free(val);
            /* Cache it locally for subsequent gets */
            csilk_session_set(c, key, arena_val);
            if (out_len) {
                *out_len = 0;
            }
            return arena_val;
        }
        free(val);
    }

    if (out_len) {
        *out_len = 0;
    }
    return NULL;
}

/**
 * @brief Destroy the current session.
 *
 * Removes the session from the global store, frees all its key-value data
 * items and the session struct itself, clears the "_session" context entry,
 * and sets an empty session cookie with a negative max-age to instruct the
 * client to delete it.
 *
 * @param c  The request context.
 */
void
csilk_session_destroy(csilk_ctx_t* c)
{
    if (!c) {
        return;
    }

    csilk_session_t* session = csilk_get(c, "_session");
    if (!session) {
        return;
    }

    CSILK_LOG_I("Session: Destroying session '%s' for request %p", session->id, (void*)c);

    session_store_t* store = session_store_for_ctx(c);
    remove_session_locked(store, session);
    /* Clears "_session" and invokes _session_ctx_release() to drop context ref. */
    csilk_set_ex(c, "_session", NULL, NULL);
    csilk_set_cookie(c, SESSION_COOKIE, "", -1, "/", NULL, 0, 1);
}

/**
 * @brief Get the session ID string for the current request.
 *
 * Returns the UUID of the session stored in the context under "_session" (set
 * by csilk_session_start()).
 *
 * @param c  The request context. Must not be NULL.
 * @return The null-terminated session ID string, or NULL if no session is
 *         active for this request.
 */
const char*
csilk_session_get_id(csilk_ctx_t* c)
{
    if (!c) {
        return NULL;
    }
    csilk_session_t* session = csilk_get(c, "_session");
    return session ? session->id : NULL;
}
