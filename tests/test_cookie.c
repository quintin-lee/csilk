#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "csilk.h"

void test_set_cookie() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);

    csilk_set_cookie(&c, "session", "12345", 3600, "/", "example.com", 1, 1);
    
    assert(c.response.headers != NULL);
    assert(strcmp(c.response.headers->key, "Set-Cookie") == 0);
    assert(strstr(c.response.headers->value, "session=12345") != NULL);
    assert(strstr(c.response.headers->value, "Max-Age=3600") != NULL);
    assert(strstr(c.response.headers->value, "Path=/") != NULL);
    assert(strstr(c.response.headers->value, "Domain=example.com") != NULL);
    assert(strstr(c.response.headers->value, "Secure") != NULL);
    assert(strstr(c.response.headers->value, "HttpOnly") != NULL);

    csilk_ctx_cleanup(&c);
    printf("test_set_cookie passed\n");
}

void test_get_cookie() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);

    // Mock request header
    csilk_set_request_header(&c, "Cookie", "user=admin; theme=dark; extra=foo=bar");

    const char* user = csilk_get_cookie(&c, "user");
    assert(user != NULL);
    assert(strcmp(user, "admin") == 0);

    const char* theme = csilk_get_cookie(&c, "theme");
    assert(theme != NULL);
    assert(strcmp(theme, "dark") == 0);

    const char* extra = csilk_get_cookie(&c, "extra");
    assert(extra != NULL);
    assert(strcmp(extra, "foo=bar") == 0);

    const char* missing = csilk_get_cookie(&c, "missing");
    assert(missing == NULL);

    csilk_ctx_cleanup(&c);
    printf("test_get_cookie passed\n");
}

void test_multiple_cookies() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);

    csilk_set_cookie(&c, "c1", "v1", 0, NULL, NULL, 0, 0);
    csilk_set_cookie(&c, "c2", "v2", 0, NULL, NULL, 0, 0);

    int count = 0;
    csilk_header_t* h = c.response.headers;
    while (h) {
        if (strcmp(h->key, "Set-Cookie") == 0) {
            count++;
        }
        h = h->next;
    }
    assert(count == 2);

    csilk_ctx_cleanup(&c);
    printf("test_multiple_cookies passed\n");
}

int main() {
    test_set_cookie();
    test_get_cookie();
    test_multiple_cookies();
    return 0;
}
