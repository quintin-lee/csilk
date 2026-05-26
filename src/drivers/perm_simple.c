#include <cJSON.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk.h"
#include "csilk_perm.h"

#define MAX_RULES 128

static csilk_perm_rule_t rules[MAX_RULES];
static int rule_count = 0;

static int match_pattern(const char* pattern, const char* value) {
  if (!pattern || !value) return 0;
  if (strcmp(pattern, "*") == 0) return 1;
  size_t plen = strlen(pattern);
  if (plen > 0 && pattern[plen - 1] == '*') {
    size_t pfxlen = plen - 1;
    if (pfxlen > 0 && pattern[pfxlen - 1] == ':') {
      return strncmp(pattern, value, pfxlen) == 0;
    }
  }
  return strcmp(pattern, value) == 0;
}

static const char* get_role_from_ctx(csilk_ctx_t* c) {
  const char* role = (const char*)csilk_get(c, "role");
  if (role) return role;

  cJSON* payload = (cJSON*)csilk_get(c, "jwt_payload");
  if (payload) {
    cJSON* r = cJSON_GetObjectItem(payload, "role");
    if (r && cJSON_IsString(r)) return r->valuestring;
  }

  return NULL;
}

static int simple_check(csilk_ctx_t* c, const char* permission,
                        const char* resource) {
  const char* role = get_role_from_ctx(c);
  if (!role) return -1;

  for (int i = 0; i < rule_count; i++) {
    if (match_pattern(rules[i].role, role) &&
        match_pattern(rules[i].permission, permission) &&
        match_pattern(rules[i].resource, resource)) {
      return 0;
    }
  }

  return -1;
}

csilk_perm_driver_t csilk_perm_simple_driver = {
    .name = "simple",
    .check = simple_check,
};

void csilk_perm_simple_init(void) {
  rule_count = 0;
  csilk_perm_register_driver("simple", &csilk_perm_simple_driver);
}

int csilk_perm_simple_allow(const char* role, const char* permission,
                            const char* resource) {
  if (!role || !permission || !resource || rule_count >= MAX_RULES) return -1;
  rules[rule_count].role = role;
  rules[rule_count].permission = permission;
  rules[rule_count].resource = resource;
  rule_count++;
  return 0;
}

void csilk_perm_simple_clear(void) {
  rule_count = 0;
}
