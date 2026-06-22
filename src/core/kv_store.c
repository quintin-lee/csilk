/**
 * @file kv_store.c
 * @brief Context key-value storage implementation.
 *
 * Provides the default in-memory linked-list storage for per-request
 * key-value pairs, plus delegation to pluggable storage drivers
 * (e.g., Redis).  All local allocations come from the request arena.
 *
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Store a value in the context's key-value storage.
 *
 * If a storage driver is set on the context, delegates to it. Otherwise,
 * uses a simple linked list allocated from the arena. If the key already
 * exists, its value is replaced. There is a hard limit of CSILK_MAX_STORAGE
 * items per request to prevent excessive arena consumption.
 *
 * @param c     The request context.
 * @param key   Storage key (null-terminated string).
 * @param value Opaque pointer to store (may be nullptr to clear a previous value).
 * @note The key is duplicated into arena memory. The value is stored as a
 *       raw pointer — no deep copy or freeing is performed. */
void
csilk_set(csilk_ctx_t* c, const char* key, void* value)
{
	if (!c || !key) {
		return;
	}

	if (c->storage_driver && c->storage_driver->set) {
		c->storage_driver->set(c, key, value);
		return;
	}

	if (!c->arena) {
		return;
	}

	csilk_storage_item_t* item = c->storage_head;
	int count = 0;
	while (item) {
		if (strcmp(item->key, key) == 0) {
			item->value = value;
			return;
		}
		count++;
		item = item->next;
	}

	/* Limit storage items to prevent excessive allocation in a single request */
	if (count >= CSILK_MAX_STORAGE) {
		CSILK_LOG_E("Context: storage limit reached (%d items) for key: %s",
			    CSILK_MAX_STORAGE,
			    key);
		return;
	}

	csilk_storage_item_t* new_item = csilk_arena_alloc(c->arena, sizeof(csilk_storage_item_t));
	if (new_item) {
		new_item->key = csilk_arena_strdup(c->arena, key);
		new_item->value = value;
		new_item->next = c->storage_head;
		c->storage_head = new_item;
	}
}

/** @brief Retrieve a value from the context's key-value storage.
 *
 * If a storage driver is set on the context, delegates to it. Otherwise,
 * searches the internal linked list for the given key.
 *
 * @param c   The request context.
 * @param key Storage key to look up.
 * @return The value pointer previously stored with csilk_set(), or nullptr if
 *         the key is not found or the context is nullptr. */
void*
csilk_get(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}

	if (c->storage_driver && c->storage_driver->get) {
		return c->storage_driver->get(c, key);
	}

	csilk_storage_item_t* item = c->storage_head;
	while (item) {
		if (strcmp(item->key, key) == 0) {
			return item->value;
		}
		item = item->next;
	}
	return nullptr;
}

/** @brief Store a string value with an optional TTL.
 *
 * Delegates to the storage driver if available. Falls back to arena-backed
 * in-memory storage (TTL is ignored in fallback mode since the key lives
 * only for the request lifecycle).
 *
 * @param c        The request context.
 * @param key      Storage key.
 * @param value    String value to store.
 * @param ttl_sec  Time-to-live in seconds (0 = no expiry).
 * @return 0 on success, -1 on error. */
int
csilk_set_string(csilk_ctx_t* c, const char* key, const char* value, int ttl_sec)
{
	if (!c || !key || !value) {
		return -1;
	}
	if (c->storage_driver && c->storage_driver->set_string) {
		return c->storage_driver->set_string(c, key, value, ttl_sec);
	}
	/* Fallback: allocate the string in the arena and use standard set
	 * (ignores TTL because local storage lives only for the request). */
	if (!c->arena) {
		return -1;
	}
	char* arena_val = csilk_arena_strdup(c->arena, value);
	csilk_set(c, key, arena_val);
	return 0;
}

/** @brief Retrieve a string value by key.
 *
 * Delegates to the storage driver if available. Falls back to returning
 * a strdup'd copy of the arena-stored value (caller must free).
 *
 * @param c    The request context.
 * @param key  Storage key.
 * @return Heap-allocated string (caller must free), or nullptr if not found. */
char*
csilk_get_string(csilk_ctx_t* c, const char* key)
{
	if (!c || !key) {
		return nullptr;
	}
	if (c->storage_driver && c->storage_driver->get_string) {
		return c->storage_driver->get_string(c, key);
	}
	/* Fallback: return a strdup of the arena-stored string so caller can free it */
	void* val = csilk_get(c, key);
	if (val) {
		return strdup((const char*)val);
	}
	return nullptr;
}

/** @brief Increment a numeric value by 1 with an optional TTL.
 *
 * Delegates to the storage driver if available. Falls back to incrementing
 * in the local in-memory store (TTL is ignored in fallback mode).
 *
 * @param c        The request context.
 * @param key      Storage key.
 * @param ttl_sec  Time-to-live in seconds (0 = no expiry; only meaningful
 *                 when a storage driver is set).
 * @return The new value after incrementing, or -1 on error. */
long long
csilk_incr(csilk_ctx_t* c, const char* key, int ttl_sec)
{
	if (!c || !key) {
		return -1;
	}
	if (c->storage_driver && c->storage_driver->incr) {
		return c->storage_driver->incr(c, key, ttl_sec);
	}
	/* Fallback: increment inside the request context's local map */
	long long val = 0;
	void* existing = csilk_get(c, key);
	if (existing) {
		val = atoll((const char*)existing);
	}
	val++;
	char buf[32];
	snprintf(buf, sizeof(buf), "%lld", val);
	char* arena_val = csilk_arena_strdup(c->arena, buf);
	if (!arena_val) {
		return -1;
	}
	csilk_set(c, key, arena_val);
	return val;
}
