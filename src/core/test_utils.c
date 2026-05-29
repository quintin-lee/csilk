/**
 * @file test_utils.c
 * @brief Internal test helpers implementation.
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "context_internal.h"
#include "csilk/csilk.h"
#include "csilk/test/test.h"

csilk_ctx_t*
csilk_test_ctx_new(void)
{
	csilk_ctx_t* c = calloc(1, sizeof(csilk_ctx_t));
	if (c) {
		c->handler_index = -1;
		c->arena = csilk_arena_new(4096);
		c->file_fd = -1;
	}
	return c;
}

void
csilk_test_ctx_free(csilk_ctx_t* c)
{
	if (c) {
		csilk_ctx_cleanup(c);
		if (c->arena) {
			csilk_arena_free(c->arena);
		}
		free(c);
	}
}

void
csilk_test_ctx_set_handlers(csilk_ctx_t* c, csilk_handler_t* handlers)
{
	if (c) {
		c->handlers = handlers;
	}
}

void
csilk_test_ctx_set_request(csilk_ctx_t* c, const char* method, const char* path)
{
	if (c) {
		c->request.method = (char*)method;
		if (c->request.path) {
			free((void*)c->request.path);
		}
		c->request.path = path ? strdup(path) : NULL;
	}
}

void
csilk_test_ctx_set_handler_metadata(csilk_ctx_t* c,
				    const char* perm_required,
				    const char* perm_resource)
{
	if (c) {
		if (!c->current_handler) {
			c->current_handler =
			    csilk_arena_alloc(c->arena, sizeof(csilk_method_handler_t));
			if (c->current_handler) {
				memset(c->current_handler, 0, sizeof(csilk_method_handler_t));
			}
		}
		if (c->current_handler) {
			c->current_handler->perm_required = (char*)perm_required;
			c->current_handler->perm_resource = (char*)perm_resource;
		}
	}
}

void
csilk_test_ctx_set_body(csilk_ctx_t* c, const char* body, size_t len)
{
	if (c) {
		if (c->request.body) {
			free(c->request.body);
		}
		c->request.body = body ? strdup(body) : NULL;
		c->request.body_len = len;
	}
}

void
csilk_test_ctx_add_param(csilk_ctx_t* c, const char* key, const char* value)
{
	if (c && c->params_count < CSILK_MAX_PARAMS) {
		c->params[c->params_count].key = strdup(key);
		c->params[c->params_count].value = strdup(value);
		c->params_count++;
	}
}

int
csilk_test_ctx_count_response_headers(csilk_ctx_t* c, const char* key, const char* value_contains)
{
	if (!c || !key) {
		return 0;
	}
	int count = 0;
	for (int i = 0; i < CSILK_HEADER_BUCKETS; i++) {
		csilk_header_t* h = c->response.headers.buckets[i];
		while (h) {
			if (strcasecmp(h->key, key) == 0) {
				if (!value_contains || strstr(h->value, value_contains)) {
					count++;
				}
			}
			h = h->next;
		}
	}
	return count;
}
