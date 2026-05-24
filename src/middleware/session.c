#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

#include "context_internal.h"
#include "csilk.h"
#include "csilk_internal.h"

/** @brief Session data item (key-value pair). */
typedef struct csilk_session_data_s {
  char* key;
  void* value;
  struct csilk_session_data_s* next;
} csilk_session_data_t;

/** @brief Session structure. */
typedef struct csilk_session_s {
  char id[37];
  csilk_session_data_t* data;
  time_t expires_at;
  struct csilk_session_s* next;
} csilk_session_t;

/** @brief Global session store. */
static csilk_session_t* session_store = NULL;

/** @brief Session store mutex. */
static uv_mutex_t session_mutex;

/** @brief Mutex initialization guard. */
static uv_once_t session_mutex_once = UV_ONCE_INIT;

/** @brief Initialize the session store mutex. */
static void init_session_mutex(void) { uv_mutex_init(&session_mutex); }

/** @brief Lock the session store. */
static void session_lock(void) {
  uv_once(&session_mutex_once, init_session_mutex);
  uv_mutex_lock(&session_mutex);
}

/** @brief Unlock the session store. */
static void session_unlock(void) { uv_mutex_unlock(&session_mutex); }

/** @brief Session cookie name. */
#define SESSION_COOKIE "csilk_session"

/** @brief Default session TTL (seconds). */
#define SESSION_TTL 3600

/** @brief Generate a cryptographically random session ID. */
static void generate_session_id(char id[37]) { csilk_generate_uuid(id); }

/** @brief Find a session by ID in the global store. */
static csilk_session_t* find_session(const char* id) {
  csilk_session_t* s = session_store;
  while (s) {
    if (strcmp(s->id, id) == 0) return s;
    s = s->next;
  }
  return NULL;
}

/** @brief Find a session by ID (with lock held). */
static csilk_session_t* find_session_locked(const char* id) {
  session_lock();
  csilk_session_t* s = find_session(id);
  session_unlock();
  return s;
}

/** @brief Add a session to the store (with lock held). */
static void add_session_locked(csilk_session_t* session) {
  session_lock();
  session->next = session_store;
  session_store = session;
  session_unlock();
}

/** @brief Remove a session from the store (with lock held). */
static void remove_session_locked(csilk_session_t* session) {
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

/** @brief Remove expired sessions from the global store. */
static void cleanup_expired(void) {
  time_t now = time(NULL);
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

/** @brief Initialize the session system. */
void csilk_session_init(void) { cleanup_expired(); }

/** @brief Start or resume a session for the current request. */
void csilk_session_start(csilk_ctx_t* c) {
  if (!c) return;

  const char* sid = csilk_get_cookie(c, SESSION_COOKIE);
  csilk_session_t* session = NULL;

  if (sid) {
    session = find_session_locked(sid);
  }

  if (!session) {
    session = calloc(1, sizeof(csilk_session_t));
    if (!session) return;

    generate_session_id(session->id);
    session->expires_at = time(NULL) + SESSION_TTL;

    add_session_locked(session);

    csilk_set_cookie(c, SESSION_COOKIE, session->id, 60 * 60 * 24, "/", NULL, 0,
                     1);
  } else {
    session->expires_at = time(NULL) + SESSION_TTL;
  }

  csilk_set(c, "_session", session);
}

/** @brief Store a value in the session. */
void csilk_session_set(csilk_ctx_t* c, const char* key, void* value) {
  if (!c || !key) return;

  csilk_session_t* session = csilk_get(c, "_session");
  if (!session) return;

  csilk_session_data_t* d = session->data;
  while (d) {
    if (strcmp(d->key, key) == 0) {
      d->value = value;
      return;
    }
    d = d->next;
  }

  csilk_session_data_t* new_d = calloc(1, sizeof(csilk_session_data_t));
  if (!new_d) return;

  new_d->key = strdup(key);
  new_d->value = value;
  new_d->next = session->data;
  session->data = new_d;
}

/** @brief Retrieve a value from the session. */
void* csilk_session_get(csilk_ctx_t* c, const char* key) {
  if (!c || !key) return NULL;

  csilk_session_t* session = csilk_get(c, "_session");
  if (!session) return NULL;

  csilk_session_data_t* d = session->data;
  while (d) {
    if (strcmp(d->key, key) == 0) return d->value;
    d = d->next;
  }
  return NULL;
}

/** @brief Destroy the current session. */
void csilk_session_destroy(csilk_ctx_t* c) {
  if (!c) return;

  csilk_session_t* session = csilk_get(c, "_session");
  if (!session) return;

  remove_session_locked(session);

  csilk_session_data_t* d = session->data;
  while (d) {
    csilk_session_data_t* next = d->next;
    free(d->key);
    free(d);
    d = next;
  }
  free(session);

  csilk_set(c, "_session", NULL);
  csilk_set_cookie(c, SESSION_COOKIE, "", -1, "/", NULL, 0, 1);
}
