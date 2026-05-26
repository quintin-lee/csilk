/**
 * @file ai.c
 * @brief AI unified interface engine implementation.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk_ai.h"

#define MAX_DRIVERS 8

static const csilk_ai_driver_t* g_drivers[MAX_DRIVERS];
static size_t g_driver_count = 0;

struct csilk_ai_s {
  const csilk_ai_driver_t* driver;
  void* driver_state;
};

void csilk_ai_register_driver(const csilk_ai_driver_t* driver) {
  if (g_driver_count < MAX_DRIVERS) {
    g_drivers[g_driver_count++] = driver;
  }
}

static const csilk_ai_driver_t* find_driver(const char* name) {
  for (size_t i = 0; i < g_driver_count; i++) {
    if (strcmp(g_drivers[i]->name, name) == 0) {
      return g_drivers[i];
    }
  }
  return NULL;
}

/* Forward declaration for the default OpenAI driver init */
extern void csilk_ai_openai_init_driver(void);

csilk_ai_t* csilk_ai_new(const char* driver_name, const char* api_key,
                         const char* base_url) {
  /* Auto-initialize default drivers on first use */
  static int initialized = 0;
  if (!initialized) {
    csilk_ai_openai_init_driver();
    initialized = 1;
  }

  const csilk_ai_driver_t* driver = find_driver(driver_name);
  if (!driver) return NULL;

  void* state = driver->init(api_key, base_url);
  if (!state) return NULL;

  csilk_ai_t* ai = malloc(sizeof(csilk_ai_t));
  if (!ai) {
    driver->free(state);
    return NULL;
  }

  ai->driver = driver;
  ai->driver_state = state;
  return ai;
}

int csilk_ai_chat(csilk_ai_t* ai, const csilk_ai_chat_request_t* req,
                  csilk_ai_chat_response_t* res) {
  if (!ai || !req || !res) return -1;
  memset(res, 0, sizeof(*res));
  return ai->driver->chat(ai->driver_state, req, res);
}

void csilk_ai_free(csilk_ai_t* ai) {
  if (!ai) return;
  ai->driver->free(ai->driver_state);
  free(ai);
}

void csilk_ai_chat_response_free(csilk_ai_chat_response_t* res) {
  if (!res) return;
  free(res->content);
  free(res->raw_response);
  free(res->error_message);
}
