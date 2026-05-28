#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/core/context_internal.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

void
test_get_header()
{
	printf("Testing csilk_get_header...\n");
	csilk_ctx_t c = {0};
	c.arena = csilk_arena_new(1024);

	csilk_set_request_header(&c, "Content-Type", "application/json");

	const char* val = csilk_get_header(&c, "Content-Type");
	assert(val != NULL);
	assert(strcmp(val, "application/json") == 0);

	// Test case insensitivity
	val = csilk_get_header(&c, "content-type");
	assert(val != NULL);
	assert(strcmp(val, "application/json") == 0);

	val = csilk_get_header(&c, "X-Not-Found");
	assert(val == NULL);

	csilk_ctx_cleanup(&c);
	csilk_arena_free(c.arena);
	printf("csilk_get_header passed!\n");
}

void
test_set_header()
{
	printf("Testing csilk_set_header...\n");
	csilk_ctx_t c = {0};
	c.arena = csilk_arena_new(1024);

	csilk_set_header(&c, "Server", "Csilk/0.1.0");
	const char* val =
	    csilk_get_header(&c,
			     "Server"); // Note: get_header works on request.headers in current API?
	// Wait, let's check csilk_get_header implementation.
	// It uses map_get(&c->request.headers, key).
	// We should have a way to get response headers too, but public API usually
	// only provides get_header for request.

	// For testing purposes, let's look at the map directly or just use internal
	// knowledge. In context.c, map_get is static. I should probably add a way to
	// verify response headers in tests.

	// Actually, for this test, I'll just check if it was set in the response map.
	int found = 0;
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		csilk_header_t* h = c.response.headers.buckets[i];
		while (h) {
			if (strcasecmp(h->key, "Server") == 0 &&
			    strcmp(h->value, "Csilk/0.1.0") == 0) {
				found = 1;
				break;
			}
			h = h->next;
		}
	}
	assert(found);

	// Update existing header
	csilk_set_header(&c, "Server", "Csilk/1.0.0");
	found = 0;
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		csilk_header_t* h = c.response.headers.buckets[i];
		while (h) {
			if (strcasecmp(h->key, "Server") == 0 &&
			    strcmp(h->value, "Csilk/1.0.0") == 0) {
				found = 1;
				break;
			}
			h = h->next;
		}
	}
	assert(found);

	csilk_ctx_cleanup(&c);
	csilk_arena_free(c.arena);
	printf("csilk_set_header passed!\n");
}

void
test_add_header()
{
	printf("Testing csilk_add_header...\n");
	csilk_ctx_t c = {0};
	c.arena = csilk_arena_new(1024);

	csilk_add_header(&c, "Set-Cookie", "a=1");
	csilk_add_header(&c, "Set-Cookie", "b=2");
	csilk_add_header(&c, "X-Custom", "value");

	int cookie_count = 0;
	int custom_found = 0;
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		csilk_header_t* h = c.response.headers.buckets[i];
		while (h) {
			if (strcasecmp(h->key, "Set-Cookie") == 0) {
				cookie_count++;
			}
			if (strcasecmp(h->key, "X-Custom") == 0 && strcmp(h->value, "value") == 0) {
				custom_found = 1;
			}
			h = h->next;
		}
	}
	assert(cookie_count == 2);
	assert(custom_found == 1);

	csilk_ctx_cleanup(&c);
	csilk_arena_free(c.arena);
	printf("csilk_add_header passed!\n");
}

int
main()
{
	test_get_header();
	test_set_header();
	test_add_header();
	printf("All header tests passed!\n");
	return 0;
}
