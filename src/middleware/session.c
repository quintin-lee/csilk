/**
 * @file session.c
 * @brief Cookie-based session management middleware with thread-safe
 * protection.
 * @copyright MIT License
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

#include "csilk/core/internal.h"
#include "csilk/core/ctx_types.h"
#include "csilk/csilk.h"

/**
 * @brief Session data item (key-value pair).
 *
 * Each session contains a singly-linked list of these items, each holding
 * a string key and an opaque void* value.
 */
typedef struct csilk_session_data_s {
	char* key;
	void* value;
	struct csilk_session_data_s* next;
} csilk_session_data_t;

/**
 * @brief Session structure.
 *
 * Represents an individual session with a UUID-based ID, an expiry timestamp,
 * a linked list of key-value data items, and a pointer to the next session
 * in the global store (singly-linked list).
 */
typedef struct csilk_session_s {
	char id[37];
	csilk_session_data_t* data;
	time_t expires_at;
	struct csilk_session_s* next;
} csilk_session_t;

/**
 * @brief Global session store (singly-linked list of active sessions).
 *
 * Protected by a libuv mutex. All read/write access to this pointer must
 * occur while session_mutex is held. The linked-list design is chosen
 * over a hash table for simplicity; with typical concurrency levels the
 * O(n) traversal is acceptable for <10K active sessions.
 */
static csilk_session_t* session_store = nullptr;

/**
 * @brief Session store mutex — guards session_store and all linked-list
 *        operations against concurrent access.
 */
static uv_mutex_t session_mutex;

/**
 * @brief One-time initialization guard for session_mutex.
 *
 * Ensures uv_mutex_init is called exactly once across all threads,
 * regardless of which thread triggers the first session operation.
 */
static uv_once_t session_mutex_once = UV_ONCE_INIT;

/**
 * @brief Initialize the session store mutex (called once via uv_once).
 */
static void
init_session_mutex(void)
{
	uv_mutex_init(&session_mutex);
}

/**
 * @brief Lock the session store mutex.
 *
 * Ensures the mutex is initialized (via uv_once) before acquiring the lock.
 * Blocks until the lock is held.
 */
static void
session_lock(void)
{
	uv_once(&session_mutex_once, init_session_mutex);
	uv_mutex_lock(&session_mutex);
}

/**
 * @brief Unlock the session store mutex.
 *
 * Releases the lock previously acquired by session_lock().
 */
static void
session_unlock(void)
{
	uv_mutex_unlock(&session_mutex);
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
generate_session_id(csilk_ctx_t* c, char id[37])
{
	_csilk_generate_uuid(c, id);
}

/**
 * @brief Find a session by ID in the global store.
 *
 * Performs a linear search of the singly-linked session list.
 *
 * @param id  The session ID string to search for.
 *
 * @return Pointer to the csilk_session_t if found, nullptr otherwise.
 *
 * @note This function does NOT acquire the mutex. The caller must hold
 *       session_mutex when calling this function.
 */
static csilk_session_t*
find_session(const char* id)
{
	csilk_session_t* s = session_store;
	while (s) {
		if (strcmp(s->id, id) == 0) {
			return s;
		}
		s = s->next;
	}
	return nullptr;
}

/**
 * @brief Find a session by ID (acquires and releases the mutex).
 *
 * Convenience wrapper around find_session() that handles locking for
 * single-lookup callers.
 *
 * @param id  The session ID string to search for.
 *
 * @return Pointer to the csilk_session_t if found, nullptr otherwise.
 */
static csilk_session_t*
find_session_locked(const char* id)
{
	session_lock();
	csilk_session_t* s = find_session(id);
	session_unlock();
	return s;
}

/**
 * @brief Add a session to the store (acquires and releases the mutex).
 *
 * Prepends the session to the global singly-linked list of active sessions.
 *
 * @param session  The session to add. Must not be nullptr.
 */
static void
add_session_locked(csilk_session_t* session)
{
	session_lock();
	session->next = session_store;
	session_store = session;
	session_unlock();
}

/**
 * @brief Remove a session from the store (acquires and releases the mutex).
 *
 * Unlinks the session from the global singly-linked list of active sessions.
 * Does NOT free the session or its data — the caller must do that after
 * removal.
 *
 * @param session  The session to remove. Must be a valid pointer currently
 *                 in the store.
 */
static void
remove_session_locked(csilk_session_t* session)
{
	session_lock();
	csilk_session_t** prev = &session_store;
	csilk_session_t* s = session_store;
	while (s) {
		if (s == session) {
			*prev = s->next;
			break;
		}
		prev = &s->next;
		s = s->next;
	}
	session_unlock();
}

/**
 * @brief Remove expired sessions from the global store.
 *
 * Iterates over the session list and removes (frees) every session whose
 * expires_at timestamp is <= the current time. Also frees all key-value
 * data items belonging to each expired session.
 *
 * @note Acquires and releases session_mutex during the sweep.
 */
static void
cleanup_expired(void)
{
	time_t now = time(nullptr);
	session_lock();
	csilk_session_t** prev = &session_store;
	csilk_session_t* s = session_store;
	while (s) {
		if (s->expires_at <= now) {
			csilk_session_data_t* d = s->data;
			while (d) {
				csilk_session_data_t* next = d->next;
				free(d->key);
				free(d);
				d = next;
			}
			*prev = s->next;
			free(s);
			s = *prev;
		} else {
			prev = &s->next;
			s = s->next;
		}
	}
	session_unlock();
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

	/* Look for an existing session cookie. If found and valid, resume it.
     Otherwise create a fresh session with a new UUID. */
	const char* sid = csilk_get_cookie(c, SESSION_COOKIE);
	csilk_session_t* session = nullptr;

	if (sid) {
		session = find_session_locked(sid);
	}

	if (!session) {
		/* No session found — allocate a new one, generate an ID, and insert
       into the global store. The session cookie (HTTP-only, no JS access)
       is set with a 24-hour lifetime. */
		session = calloc(1, sizeof(csilk_session_t));
		if (!session) {
			return;
		}

		generate_session_id(c, session->id);
		session->expires_at = time(nullptr) + SESSION_TTL;

		add_session_locked(session);

		csilk_set_cookie(c, SESSION_COOKIE, session->id, 60 * 60 * 24, "/", nullptr, 0, 1);
	} else {
		/* Existing session: extend the expiry window. */
		session->expires_at = time(nullptr) + SESSION_TTL;
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
 * @param key   Null-terminated key string. Must not be nullptr.
 * @param value Opaque pointer to the value to store. May be nullptr.
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
		return;
	}

	new_d->key = strdup(key);
	new_d->value = value;
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
 * @param key Null-terminated key string. Must not be nullptr.
 *
 * @return The value pointer previously stored with csilk_session_set(), or
 *         nullptr if the key is not found or no session is active.
 */
void*
csilk_session_get(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}

	csilk_session_t* session = csilk_get(c, "_session");
	if (!session) {
		return nullptr;
	}

	csilk_session_data_t* d = session->data;
	while (d) {
		if (strcmp(d->key, key) == 0) {
			return d->value;
		}
		d = d->next;
	}

	if (c->storage_driver && c->storage_driver->get_string) {
		char s_key[128];
		snprintf(s_key, sizeof(s_key), "session:%s:%s", session->id, key);
		char* val = csilk_get_string(c, s_key);
		if (val && c->arena) {
			char* arena_val = csilk_arena_strdup(c->arena, val);
			free(val);
			/* Cache it locally for subsequent gets */
			csilk_session_set(c, key, arena_val);
			return arena_val;
		} else if (val) {
			free(val);
		}
	}

	return nullptr;
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

	remove_session_locked(session);

	csilk_session_data_t* d = session->data;
	while (d) {
		csilk_session_data_t* next = d->next;
		free(d->key);
		free(d);
		d = next;
	}
	free(session);

	csilk_set(c, "_session", nullptr);
	csilk_set_cookie(c, SESSION_COOKIE, "", -1, "/", nullptr, 0, 1);
}
