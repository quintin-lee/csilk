#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"

void test_mongodb_basic(void) {
#ifndef HAS_MONGODB
  printf("test_mongodb_basic: SKIPPED (HAS_MONGODB not defined)\n");
  return;
#endif
  csilk_db_init();

  // Standard local MongoDB URI
  const char* uri = "mongodb://localhost:27017/csilk_test";
  csilk_db_pool_t* pool = csilk_db_pool_new("mongodb", uri);

  if (!pool) {
    printf("test_mongodb_basic: SKIPPED (Could not connect to %s)\n", uri);
    return;
  }

  /* 1. Insert a document using a command */
  // Note: This uses the MongoDB 'insert' command format
  const char* insert_cmd =
      "{\"insert\": \"users\", \"documents\": [{\"name\": \"Alice\", \"age\": "
      "30}]}";
  int rc = csilk_db_exec(pool, insert_cmd);
  assert(rc == 0);

  /* 2. Query the collection */
  // For our MongoDB driver, query with a string that is NOT JSON is treated as
  // collection name
  cJSON* users = csilk_db_query_json(pool, "users");
  assert(users != NULL);
  assert(cJSON_IsArray(users));

  // We expect at least Alice
  int found_alice = 0;
  for (int i = 0; i < cJSON_GetArraySize(users); i++) {
    cJSON* user = cJSON_GetArrayItem(users, i);
    cJSON* name = cJSON_GetObjectItem(user, "name");
    if (name && strcmp(name->valuestring, "Alice") == 0) {
      found_alice = 1;
      break;
    }
  }
  assert(found_alice == 1);

  /* 3. Cleanup: Drop collection */
  const char* drop_cmd = "{\"drop\": \"users\"}";
  csilk_db_exec(pool, drop_cmd);

  cJSON_Delete(users);
  csilk_db_pool_free(pool);
  printf("test_mongodb_basic: PASS\n");
}

int main(void) {
  test_mongodb_basic();
  return 0;
}
