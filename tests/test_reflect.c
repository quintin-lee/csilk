#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk_internal.h"

// Extend type map BEFORE including csilk.h/csilk_reflect.h
#undef CSILK_USER_TYPE_MAP
#define CSILK_USER_TYPE_MAP \
  , struct TestUser_s : "TestUser", struct TestPoint_s : "TestPoint"

#include "csilk.h"
#include "csilk_reflect.h"

// Define test structures with explicit tags for _Generic
/** @brief Test structure for 2D point reflection tests. */
typedef struct TestPoint_s {
  int16_t x; /**< X coordinate. */
  int16_t y; /**< Y coordinate. */
} TestPoint;

/** @brief Test structure for user data reflection tests. */
typedef struct TestUser_s {
  int32_t id;    /**< User ID. */
  char name[32]; /**< User name string. */
  float score;   /**< User score value. */
  TestPoint pos; /**< Position (nested TestPoint). */
} TestUser;

#define POINT_REFLECT_MAP(X)                                         \
  X(TestPoint, x, CSILK_TYPE_INT16, sizeof(int16_t), 0, false, NULL) \
  X(TestPoint, y, CSILK_TYPE_INT16, sizeof(int16_t), 0, false, NULL)

#define USER_REFLECT_MAP(X)                                           \
  X(TestUser, id, CSILK_TYPE_INT32, sizeof(int32_t), 0, false, NULL)  \
  X(TestUser, name, CSILK_TYPE_STRING, 32, 0, false, NULL)            \
  X(TestUser, score, CSILK_TYPE_FLOAT, sizeof(float), 0, false, NULL) \
  X(TestUser, pos, CSILK_TYPE_STRUCT, sizeof(TestPoint), 0, false, "TestPoint")

// Automatic registration
CSILK_REGISTER_REFLECT(TestPoint, POINT_REFLECT_MAP)
CSILK_REGISTER_REFLECT(TestUser, USER_REFLECT_MAP)

void test_marshal() {
  TestUser user = {.id = 1, .name = "Alice", .score = 95.5f, .pos = {10, 20}};
  // Use automatic type dispatch!
  char* json = csilk_marshal(&user);

  assert(json != NULL);
  assert(strstr(json, "\"id\":1") != NULL);
  assert(strstr(json, "\"name\":\"Alice\"") != NULL);
  assert(strstr(json, "\"pos\":{\"x\":10,\"y\":20}") != NULL);

  free(json);
  printf("test_marshal passed\n");
}

void test_unmarshal() {
  const char* json =
      "{\"id\":2, \"name\":\"Bob\", \"score\":88.0, "
      "\"pos\":{\"x\":30,\"y\":40}}";
  TestUser user = {0};

  // Use automatic type dispatch!
  int ok = csilk_unmarshal(json, &user);
  assert(ok == 1);
  assert(user.id == 2);
  assert(user.pos.x == 30);

  printf("test_unmarshal passed\n");
}

void test_context_reflect() {
  csilk_ctx_t c = {0};
  c.arena = csilk_arena_new(1024);

  // Test binding with macro (uses type string conversion)
  c.request.body = strdup("{\"id\":3, \"name\":\"Charlie\", \"score\":77.5}");
  TestUser user = {0};
  int ok = csilk_bind(&c, TestUser, &user);
  assert(ok == 1);
  assert(user.id == 3);

  // Test response with macro
  csilk_json_t(&c, CSILK_STATUS_OK, TestUser, &user);
  assert(c.response.status == CSILK_STATUS_OK);
  assert(c.response.body != NULL);

  csilk_ctx_cleanup(&c);
  csilk_arena_free(c.arena);
  printf("test_context_reflect passed\n");
}

int main() {
  test_marshal();
  test_unmarshal();
  test_context_reflect();
  return 0;
}
