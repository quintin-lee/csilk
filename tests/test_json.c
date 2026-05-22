#include "gin.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

void test_bind_json() {
    printf("Testing gin_bind_json...\n");
    gin_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.request.body = "{\"key\": \"value\"}";
    
    cJSON *json = gin_bind_json(&ctx);
    assert(json != NULL);
    
    cJSON *val = cJSON_GetObjectItem(json, "key");
    assert(val != NULL);
    assert(strcmp(val->valuestring, "value") == 0);
    
    cJSON_Delete(json);
    printf("gin_bind_json test passed.\n");
}

void test_json_response() {
    printf("Testing gin_json...\n");
    gin_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    
    cJSON *json = cJSON_CreateObject();
    cJSON_AddStringToObject(json, "status", "ok");
    
    gin_json(&ctx, 200, json);
    
    assert(ctx.response.status == 200);
    assert(ctx.response.body != NULL);
    // Note: gin_set_header is not implemented for checking, but we rely on gin_json implementation
    // The body should be "{"status":"ok"}"
    assert(strstr(ctx.response.body, "\"status\":\"ok\"") != NULL);
    
    free((void*)ctx.response.body);
    printf("gin_json test passed.\n");
}

int main() {
    test_bind_json();
    test_json_response();
    printf("All JSON tests passed!\n");
    return 0;
}
