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
    time_t                  last_access; /* LRU tracking */
    struct csilk_session_s* next;        /* next in same bucket */
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

/** @brief Global sharded session store (array of SESSION_BUCKET_COUNT buckets). */
static session_bucket_t session_buckets[SESSION_BUCKET_COUNT];

/** @brief One-time initialization guard for all bucket mutexes. */
static csilk_once_t session_init_once = CSILK_ONCE_INIT;

/** @brief Total active session count (atomic, for LRU eviction). */
static atomic_size_t session_count = 0;

/** @brief Maximum session count limit (0 = unlimited).
 *
 * Atomic because csilk_session_set_max() writes it from the configuration
 * thread while worker threads read it during LRU eviction; a plain size_t
 * would be a data race. The read in the eviction path is relaxed, so the
 * ordering only matters for visibility of the new limit, not sequencing. */
static atomic_size_t session_max = SESSION_MAX_DEFAULT;

/**
 * @brief Initialize all bucket mutexes (called once via csilk_once).
 */
static void
init_session_buckets(void)
{
    for (size_t i = 0; i < SESSION_BUCKET_COUNT; i++) {
        session_buckets[i].head = NULL;
        csilk_mutex_init(&session_buckets[i].mutex);
    }
    CSILK_LOG_I("Session: Sharded store initialized (%zu buckets)", (size_t)SESSION_BUCKET_COUNT);
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
session_bucket_lock(size_t idx)
{
    csilk_mutex_lock(&session_buckets[idx].mutex);
}

/**
 * @brief Unlock a specific bucket by index.
 */
static inline void
session_bucket_unlock(size_t idx)
{
    csilk_mutex_unlock(&session_buckets[idx].mutex);
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
static csilk_session_t*
find_session_locked(const char* id)
{
    size_t idx = session_hash(id);
    session_bucket_lock(idx);
    csilk_session_t* s = session_buckets[idx].head;
    while (s) {
        if (strcmp(s->id, id) == 0) {
            s->last_access = time(NULL);
            session_bucket_unlock(idx);
            return s;
        }
        s = s->next;
    }
    session_bucket_unlock(idx);
    return NULL;
}

/**
 * @brief Free a session's data list and the session struct itself.
 * @param session  The session to free (not currently in the store).
 */
static void
free_session(csilk_session_t* session)
{
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
    free(session);
}

/**
 * @brief Add a session to the sharded store.
 *
 * Prepends the session to the head of the appropriate bucket. Increments
 * the global session count; if the max limit is exceeded the least-recently
 * accessed session is evicted from all buckets.
 *
 * @param session  The session to add. Must not be NULL.
 */
static void
add_session_locked(csilk_session_t* session)
{
    session->last_access = time(NULL);
    size_t idx = session_hash(session->id);
    session_bucket_lock(idx);
    session->next = session_buckets[idx].head;
    session_buckets[idx].head = session;
    session_bucket_unlock(idx);
    atomic_fetch_add_explicit(&session_count, 1, memory_order_relaxed);

    /* LRU eviction: if over limit, find and evict the least-recently-accessed session */
    if (atomic_load_explicit(&session_count, memory_order_relaxed) >
        atomic_load_explicit(&session_max, memory_order_relaxed)) {
        /* Find the session with the oldest last_access across all buckets */
        csilk_session_t* victim = NULL;
        time_t           victim_ts = 0;
        for (size_t b = 0; b < SESSION_BUCKET_COUNT; b++) {
            session_bucket_lock(b);
            for (csilk_session_t* s = session_buckets[b].head; s; s = s->next) {
                if (s->last_access < victim_ts || victim == NULL) {
                    victim_ts = s->last_access;
                    victim = s;
                }
            }
            session_bucket_unlock(b);
        }
        if (victim) {
            /* Unlink victim from its bucket */
            size_t vidx = session_hash(victim->id);
            session_bucket_lock(vidx);
            csilk_session_t** prev = &session_buckets[vidx].head;
            csilk_session_t*  s = session_buckets[vidx].head;
            while (s) {
                if (s == victim) {
                    *prev = s->next;
                    break;
                }
                prev = &s->next;
                s = s->next;
            }
            session_bucket_unlock(vidx);
            atomic_fetch_sub_explicit(&session_count, 1, memory_order_relaxed);
            free_session(victim);
            CSILK_LOG_D("Session: LRU-evicted session '%s'", victim->id);
        }
    }
}

/**
 * @brief Remove a session from the sharded store.
 *
 * Unlinks the session from its bucket. Does NOT free the session or its data.
 *
 * @param session  The session to remove. Must be a valid pointer currently in the store.
 */
static void
remove_session_locked(csilk_session_t* session)
{
    size_t idx = session_hash(session->id);
    session_bucket_lock(idx);
    csilk_session_t** prev = &session_buckets[idx].head;
    csilk_session_t*  s = session_buckets[idx].head;
    while (s) {
        if (s == session) {
            *prev = s->next;
            break;
        }
        prev = &s->next;
        s = s->next;
    }
    session_bucket_unlock(idx);
    atomic_fetch_sub_explicit(&session_count, 1, memory_order_relaxed);
}

/**
 * @brief Remove expired sessions from all buckets.
 *
 * Iterates every bucket; removes (frees) every session whose expires_at is
 * in the past. Each bucket is locked individually so the sweep does not
 * block other buckets.
 */
static void
cleanup_expired(void)
{
    time_t now = time(NULL);
    for (size_t b = 0; b < SESSION_BUCKET_COUNT; b++) {
        session_bucket_lock(b);
        csilk_session_t** prev = &session_buckets[b].head;
        csilk_session_t*  s = session_buckets[b].head;
        while (s) {
            if (s->expires_at <= now) {
                CSILK_LOG_D("Session: Cleaning up expired session '%s'", s->id);
                *prev = s->next;
                session_bucket_unlock(b);
                free_session(s);
                atomic_fetch_sub_explicit(&session_count, 1, memory_order_relaxed);
                s = session_buckets[b].head; /* re-read head after unlock */
                session_bucket_lock(b);
                /* s may have advanced; continue from current head */
            } else {
                prev = &s->next;
                s = s->next;
            }
        }
        session_bucket_unlock(b);
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
    atomic_store_explicit(&session_max, max, memory_order_relaxed);
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
    csilk_once(&session_init_once, init_session_buckets);
    CSILK_LOG_I("Session: Initializing session subsystem");
    cleanup_expired();
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

    if (sid) {
        session = find_session_locked(sid);
    }

    if (!session) {
        /* No session found — allocate a new one, generate an ID, and insert
        into the global store. The session cookie (HTTP-only, no JS access)
        is set with a 24-hour lifetime. */
        session = calloc(1, sizeof(csilk_session_t));
        if (!session) {
            CSILK_LOG_E("Session: Memory allocation failed for new session");
            return;
        }

        generate_session_id(c, session->id);
        session->expires_at = time(NULL) + SESSION_TTL;

        add_session_locked(session);

        CSILK_LOG_D("Session: Created new session '%s' for request %p", session->id, (void*)c);
        csilk_set_cookie(c, SESSION_COOKIE, session->id, 60 * 60 * 24, "/", NULL, 1, 1);
    } else {
        /* Existing session: extend the expiry window. */
        session->expires_at = time(NULL) + SESSION_TTL;
        CSILK_LOG_D("Session: Resumed existing session '%s' for request %p", session->id, (void*)c);
    }

    /* Store session pointer in context for downstream handlers to access
      via csilk_session_get() / csilk_session_set(). */
    csilk_set(c, "_session", session);
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
    if (!c || !key) {
        return;
    }

    csilk_session_t* session = csilk_get(c, "_session");
    if (!session) {
        return;
    }

    CSILK_LOG_T("Session: Storing key '%s' in session '%s'", key, session->id);

    csilk_session_data_t* d = session->data;
    while (d) {
        if (strcmp(d->key, key) == 0) {
            d->value = value;
            return;
        }
        d = d->next;
    }

    csilk_session_data_t* new_d = calloc(1, sizeof(csilk_session_data_t));
    if (!new_d) {
        CSILK_LOG_E("Session: Memory allocation failed for session key '%s'", key);
        return;
    }

    new_d->key = strdup(key);
    if (!new_d->key) {
        CSILK_LOG_E("Session: failed to duplicate key '%s'", key);
        free(new_d);
        return;
    }
    new_d->value = value;
    new_d->next = session->data;
    session->data = new_d;
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

    csilk_session_data_t* d = session->data;
    while (d) {
        if (strcmp(d->key, key) == 0) {
            if (d->cleanup && d->value != value) {
                d->cleanup(d->value);
            }
            d->value = value;
            d->value_len = len;
            d->cleanup = cleanup;
            return;
        }
        d = d->next;
    }

    csilk_session_data_t* new_d = calloc(1, sizeof(csilk_session_data_t));
    if (!new_d) {
        CSILK_LOG_E("Session: Memory allocation failed for session key '%s'", key);
        return;
    }

    new_d->key = strdup(key);
    if (!new_d->key) {
        CSILK_LOG_E("Session: failed to duplicate key '%s'", key);
        free(new_d);
        return;
    }
    new_d->value = value;
    new_d->value_len = len;
    new_d->cleanup = cleanup;
    new_d->next = session->data;
    session->data = new_d;
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

    csilk_session_data_t* d = session->data;
    while (d) {
        if (strcmp(d->key, key) == 0) {
            return d->value;
        }
        d = d->next;
    }

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

    csilk_session_data_t* d = session->data;
    while (d) {
        if (strcmp(d->key, key) == 0) {
            if (out_len) {
                *out_len = d->value_len;
            }
            return d->value;
        }
        d = d->next;
    }

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

    remove_session_locked(session);

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
    free(session);

    csilk_set(c, "_session", NULL);
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
