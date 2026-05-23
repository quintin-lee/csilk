#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "csilk.h"
#include "csilk_internal.h"

void redirect_handler(csilk_ctx_t* c) {
    csilk_redirect(c, 302, "/new-location");
}

const char* get_resp_header(csilk_ctx_t* c, const char* key) {
    uint32_t hash = 5381;
    int ch;
    const char* k = key;
    while ((ch = (unsigned char)*k++)) {
        hash = ((hash << 5) + hash) + tolower(ch);
    }
    uint32_t bucket = hash % CSILK_HEADER_BUCKETS;
    
    csilk_header_t* h = c->response.headers.buckets[bucket];
    while (h) {
        if (strcasecmp(h->key, key) == 0) return h->value;
        h = h->next;
    }
    return NULL;
}

void test_redirect() {
    csilk_ctx_t c = {0};
    c.arena = csilk_arena_new(1024);
    
    // Manually ensure response.headers structure is clean
    memset(&c.response.headers, 0, sizeof(csilk_header_map_t));
    
    redirect_handler(&c);
    
    assert(c.response.status == 302);
    assert(c.aborted == 1);
    
    const char* loc = get_resp_header(&c, "Location");
    if (loc == NULL) {
        printf("Location header not found!\n");
    }
    assert(loc != NULL);
    assert(strcmp(loc, "/new-location") == 0);
    
    csilk_ctx_cleanup(&c);
    csilk_arena_free(c.arena);
    printf("test_redirect passed\n");
}

int main() {
    test_redirect();
    return 0;
}
