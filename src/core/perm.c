#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_perm.h"

static csilk_perm_driver_t* drivers[16];
static int driver_count = 0;
static csilk_perm_driver_t* default_driver = NULL;
static int perm_initialized = 0;

void csilk_perm_init(void) {
  if (__sync_val_compare_and_swap(&perm_initialized, 0, 1) == 0) {
    csilk_perm_simple_init();
  }
}

int csilk_perm_register_driver(const char* name, csilk_perm_driver_t* driver) {
  if (!name || !driver || driver_count >= 16) return -1;

  driver->name = name;
  drivers[driver_count++] = driver;
  if (!default_driver) default_driver = driver;
  return 0;
}

csilk_perm_driver_t* csilk_perm_get_driver(const char* name) {
  if (!name) return NULL;
  for (int i = 0; i < driver_count; i++) {
    if (strcmp(drivers[i]->name, name) == 0) return drivers[i];
  }
  return NULL;
}

int csilk_perm_set_default(const char* name) {
  csilk_perm_driver_t* d = csilk_perm_get_driver(name);
  if (!d) return -1;
  default_driver = d;
  return 0;
}

int csilk_perm_check(csilk_ctx_t* c, const char* permission,
                     const char* resource) {
  if (!default_driver || !default_driver->check) return -1;
  return default_driver->check(c, permission, resource);
}

void csilk_perm_require(csilk_ctx_t* c, const char* permission,
                        const char* resource) {
  if (csilk_perm_check(c, permission, resource) != 0) {
    csilk_string(c, CSILK_STATUS_FORBIDDEN, "{\"error\":\"Forbidden\"}");
    csilk_abort(c);
  }
}
