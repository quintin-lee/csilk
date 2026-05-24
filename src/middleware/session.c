#include <stdlib.h>
#include <string.h>
#include <time.h>

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
  char id[33];
  csilk_session_data_t* data;
  time_t expires_at;
  struct csilk_session_s* next;
} csilk_session_t;

/** @brief Global session store. */
static csilk_session_t* session_store = NULL;

/** @brief Session cookie name. */
#define SESSION_COOKIE "csilk_session"

/** @brief Default session TTL (seconds). */
#define SESSION_TTL 3600

/** @brief Generate a random 32-character hex session ID. */
static void generate_session_id(char id[33]) {
  const char hex[] = "0123456789abcdef";
  for (int i = 0; i < 32; i++) {
    id[i] = hex[rand() % 16];
  }
  id[32] = '\0';
}

/** @brief Find a session by ID in the global store. */
static csilk_session_t* find_session(const char* id) {
  csilk_session_t* s = session_store;
  while (s) {
    if (strcmp(s->id, id) == 0) return s;
    s = s->next;
  }
  return NULL;
}

/** @brief Remove expired sessions from the global store. */
static void cleanup_expired(void) {
  time_t now = time(NULL);
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
}

/** @brief Initialize the session system. */
void csilk_session_init(void) { cleanup_expired(); }

/** @brief Start or resume a session for the current request. */
void csilk_session_start(csilk_ctx_t* c) {
  if (!c) return;

  const char* sid = csilk_get_cookie(c, SESSION_COOKIE);
  csilk_session_t* session = NULL;

  if (sid) {
    session = find_session(sid);
  }

  if (!session) {
    session = calloc(1, sizeof(csilk_session_t));
    if (!session) return;

    generate_session_id(session->id);
    session->expires_at = time(NULL) + SESSION_TTL;

    session->next = session_store;
    session_store = session;

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

  csilk_session_t** prev = &session_store;
  csilk_session_t* s = session_store;
  while (s) {
    if (s == session) {
      csilk_session_data_t* d = s->data;
      while (d) {
        csilk_session_data_t* next = d->next;
        free(d->key);
        free(d);
        d = next;
      }
      *prev = s->next;
      free(s);
      break;
    }
    prev = &s->next;
    s = s->next;
  }

  csilk_set(c, "_session", NULL);
  csilk_set_cookie(c, SESSION_COOKIE, "", -1, "/", NULL, 0, 1);
}
