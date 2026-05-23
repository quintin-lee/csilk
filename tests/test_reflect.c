#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "csilk.h"
#include "csilk_reflect.h"

// Define test structures
typedef struct {
    int32_t id;
    char name[32];
    float score;
} TestUser;

#define USER_REFLECT_MAP(X) \
    X(TestUser, id,    CSILK_TYPE_INT32,  sizeof(int32_t), 0, false, NULL, 0) \
    X(TestUser, name,  CSILK_TYPE_STRING, 32,              0, false, NULL, 0) \
    X(TestUser, score, CSILK_TYPE_FLOAT,  sizeof(float),   0, false, NULL, 0)

CSILK_REGISTER_REFLECT(TestUser, "User", USER_REFLECT_MAP)

void test_marshal() {
    TestUser user = { .id = 1, .name = "Alice", .score = 95.5f };
    char* json = csilk_json_marshal("User", &user);
    
    assert(json != NULL);
    assert(strstr(json, "\"id\":1") != NULL);
    assert(strstr(json, "\"name\":\"Alice\"") != NULL);
    assert(strstr(json, "\"score\":95.5") != NULL);
    
    free(json);
    printf("test_marshal passed\n");
}

void test_unmarshal() {
    const char* json = "{\"id\":2, \"name\":\"Bob\", \"score\":88.0}";
    TestUser user = {0};
    
    int ok = csilk_json_unmarshal("User", json, &user);
    assert(ok == 1);
    assert(user.id == 2);
    assert(strcmp(user.name, "Bob") == 0);
    assert(user.score == 88.0f);
    
    printf("test_unmarshal passed\n");
}

void test_context_reflect() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);
    
    // Test binding
    c.request.body = strdup("{\"id\":3, \"name\":\"Charlie\", \"score\":77.5}");
    TestUser user = {0};
    int ok = csilk_bind_reflect(&c, "User", &user);
    assert(ok == 1);
    assert(user.id == 3);
    assert(strcmp(user.name, "Charlie") == 0);
    
    // Test response
    csilk_json_reflect(&c, 200, "User", &user);
    assert(c.response.status == 200);
    assert(c.response.body != NULL);
    assert(strstr(c.response.body, "\"name\":\"Charlie\"") != NULL);
    
    csilk_ctx_cleanup(&c);
    printf("test_context_reflect passed\n");
}

int main() {
    test_marshal();
    test_unmarshal();
    test_context_reflect();
    return 0;
}
