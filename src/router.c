/**
 * @file router.c
 * @brief Router implementation.
 * @license MIT
 */

#include <stdlib.h>
#include <string.h>

#include "gin.h"

#define GIN_MAX_CHILDREN 128

/** @brief Node type for router trie. */
typedef enum {
  GIN_NODE_STATIC,
  GIN_NODE_PARAM,
  GIN_NODE_WILDCARD
} gin_node_type_t;

/** @brief Method-specific handler mapping. */
typedef struct gin_method_handler_s {
  char* method;
  gin_handler_t* handlers;
  struct gin_method_handler_s* next;
} gin_method_handler_t;

/** @brief Node in the router trie. */
struct gin_router_node_s {
  char* segment;
  gin_node_type_t type;
  gin_method_handler_t* handlers;
  struct gin_router_node_s* children[GIN_MAX_CHILDREN];
  int children_count;
};

/** @brief Create a new node.
 * @param segment The segment name.
 * @param type The node type.
 * @return A new gin_router_node_t instance. */
static gin_router_node_t* node_new(const char* segment, gin_node_type_t type) {
  gin_router_node_t* node = calloc(1, sizeof(gin_router_node_t));
  if (!node) return NULL;
  node->segment = strdup(segment);
  node->type = type;
  return node;
}

/** @brief Free a node and its children.
 * @param node The node to free. */
static void node_free(gin_router_node_t* node) {
  if (!node) return;
  free(node->segment);
  gin_method_handler_t* mh = node->handlers;
  while (mh) {
    gin_method_handler_t* next = mh->next;
    free(mh->method);
    free(mh->handlers);
    free(mh);
    mh = next;
  }
  for (int i = 0; i < node->children_count; i++) {
    node_free(node->children[i]);
  }
  free(node);
}

/** @brief Helper to get next path segment.
 * @param p Pointer to the current path string pointer.
 * @return The next segment, or NULL if none. */
static char* get_next_segment(const char** p) {
  if (!*p || **p == '\0') return NULL;

  while (**p == '/') (*p)++;
  if (**p == '\0') return NULL;

  const char* start = *p;
  while (**p != '/' && **p != '\0') (*p)++;

  size_t len = (size_t)(*p - start);
  char* seg = malloc(len + 1);
  if (seg) {
    memcpy(seg, start, len);
    seg[len] = '\0';
  }
  return seg;
}

/** @brief Create a new router.
 * @return A new gin_router_t instance. */
gin_router_t* gin_router_new() {
  gin_router_t* r = malloc(sizeof(gin_router_t));
  if (!r) return NULL;
  r->root = node_new("", GIN_NODE_STATIC);
  return r;
}

/** @brief Free the router.
 * @param r The router to free. */
void gin_router_free(gin_router_t* r) {
  if (!r) return;
  node_free(r->root);
  free(r);
}

/** @brief Add a route to the router.
 * @param r The router.
 * @param method The HTTP method.
 * @param path The route path.
 * @param handlers Array of handlers.
 * @param handler_count Number of handlers. */
void gin_router_add(gin_router_t* r, const char* method, const char* path,
                    gin_handler_t* handlers, size_t handler_count) {
  if (!r || !r->root || !method || !path || !handlers) return;
  gin_router_node_t* curr = r->root;
  const char* p = path;
  char* seg;

  while ((seg = get_next_segment(&p)) != NULL) {
    gin_node_type_t type = GIN_NODE_STATIC;
    char* seg_name = seg;
    if (seg[0] == ':') {
      type = GIN_NODE_PARAM;
      seg_name = seg + 1;
    } else if (seg[0] == '*') {
      type = GIN_NODE_WILDCARD;
      seg_name = seg + 1;
    }

    gin_router_node_t* found = NULL;
    for (int i = 0; i < curr->children_count; i++) {
      if (curr->children[i]->type == type &&
          strcmp(curr->children[i]->segment, seg_name) == 0) {
        found = curr->children[i];
        break;
      }
    }

    if (!found) {
      if (curr->children_count < GIN_MAX_CHILDREN) {
        found = node_new(seg_name, type);
        curr->children[curr->children_count++] = found;
      }
    }
    free(seg);
    if (!found) return; // Should not happen unless GIN_MAX_CHILDREN exceeded
    curr = found;
    if (type == GIN_NODE_WILDCARD) break;
  }

  gin_method_handler_t* mh = curr->handlers;
  while (mh) {
    if (strcmp(mh->method, method) == 0) {
      return;
    }
    mh = mh->next;
  }

  mh = malloc(sizeof(gin_method_handler_t));
  if (mh) {
    mh->method = strdup(method);
    mh->handlers = malloc(sizeof(gin_handler_t) * (handler_count + 1));
    if (mh->handlers) {
      memcpy(mh->handlers, handlers, sizeof(gin_handler_t) * handler_count);
      mh->handlers[handler_count] = NULL;
    }
    mh->next = curr->handlers;
    curr->handlers = mh;
  }
}

/** @brief Match a node against path segments.
 * @param node The current node.
 * @param method The HTTP method.
 * @param path The remaining path.
 * @param ctx The request context (optional).
 * @return Array of handlers, or NULL if no match. */
static gin_handler_t* match_node(gin_router_node_t* node, const char* method,
                                 const char* path, gin_ctx_t* ctx) {
  if (!path || *path == '\0' || strcmp(path, "/") == 0) {
    gin_method_handler_t* mh = node->handlers;
    while (mh) {
      if (strcmp(mh->method, method) == 0) return mh->handlers;
      mh = mh->next;
    }
    return NULL;
  }

  const char* p = path;
  char* seg = get_next_segment(&p);
  if (!seg) return NULL;

  gin_handler_t* result = NULL;
  for (int i = 0; i < node->children_count; i++) {
    gin_router_node_t* child = node->children[i];
    if (child->type == GIN_NODE_STATIC) {
      if (strcmp(child->segment, seg) == 0) {
        result = match_node(child, method, p, ctx);
      }
    } else if (child->type == GIN_NODE_PARAM) {
      if (ctx && ctx->params_count < GIN_MAX_PARAMS) {
        ctx->params[ctx->params_count].key = strdup(child->segment);
        ctx->params[ctx->params_count].value = strdup(seg);
        ctx->params_count++;
      }
      result = match_node(child, method, p, ctx);
      if (!result && ctx) {
        ctx->params_count--;
        free(ctx->params[ctx->params_count].key);
        free(ctx->params[ctx->params_count].value);
      }
    } else if (child->type == GIN_NODE_WILDCARD) {
      if (ctx && ctx->params_count < GIN_MAX_PARAMS) {
        ctx->params[ctx->params_count].key = strdup(child->segment);
        ctx->params[ctx->params_count].value = strdup(path + 1);
        ctx->params_count++;
      }
      gin_method_handler_t* mh = child->handlers;
      while (mh) {
        if (strcmp(mh->method, method) == 0) {
          result = mh->handlers;
          break;
        }
        mh = mh->next;
      }
    }
    if (result) break;
  }

  free(seg);
  return result;
}

/** @brief Match a route to handlers.
 * @param r The router.
 * @param method The HTTP method.
 * @param path The route path.
 * @return Array of handlers, or NULL if no match. */
gin_handler_t* gin_router_match(gin_router_t* r, const char* method,
                                const char* path) {
  if (!r || !r->root || !method || !path) return NULL;
  return match_node(r->root, method, path, NULL);
}

/** @brief Match a route and update context.
 * @param r The router.
 * @param c The request context.
 * @return 1 if matched, 0 otherwise. */
int gin_router_match_ctx(gin_router_t* r, gin_ctx_t* c) {
  if (!r || !c || !r->root || !c->request.method || !c->request.path) return 0;
  c->params_count = 0;
  gin_handler_t* handlers =
      match_node(r->root, c->request.method, c->request.path, c);
  if (handlers) {
    c->handlers = handlers;
    c->handler_index = -1;
    return 1;
  }
  return 0;
}
