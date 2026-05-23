/**
 * @file router.c
 * @brief Router implementation.
 * @license MIT
 */

#include <stdlib.h>
#include <string.h>

#include "gin.h"

/** @brief Node type for router trie. */
typedef enum {
  GIN_NODE_STATIC,
  GIN_NODE_PARAM,
  GIN_NODE_WILDCARD
} gin_node_type_t;

/** @brief Handler for a specific method at a node. */
typedef struct gin_method_handler_s {
  char* method;
  gin_handler_t* handlers;
  struct gin_method_handler_s* next;
} gin_method_handler_t;

/** @brief Router trie node. */
struct gin_router_node_s {
  char* segment;
  gin_node_type_t type;
  gin_method_handler_t* handlers;
  struct gin_router_node_s* children;
  struct gin_router_node_s* sibling;
};
 

/** @brief Create a new router.
 * @return A new gin_router_t instance. */
gin_router_t* gin_router_new() {
  gin_router_t* r = calloc(1, sizeof(gin_router_t));
  if (r) {
    r->root = calloc(1, sizeof(gin_router_node_t));
    if (!r->root) {
      free(r);
      return NULL;
    }
    r->root->segment = strdup("");
    if (!r->root->segment) {
      free(r->root);
      free(r);
      return NULL;
    }
    r->root->type = GIN_NODE_STATIC;
  }
  return r;
}

/** @brief Internal helper to free router nodes.
 * @param node The node to free. */
static void free_node(gin_router_node_t* node) {
  while (node) {
    free_node(node->children);
    gin_router_node_t* sibling = node->sibling;

    free(node->segment);
    gin_method_handler_t* h = node->handlers;
    while (h) {
      gin_method_handler_t* next = h->next;
      free(h->method);
      free(h->handlers);
      free(h);
      h = next;
    }

    free(node);
    node = sibling;
  }
}

/** @brief Free the router.
 * @param r The router to free. */
void gin_router_free(gin_router_t* r) {
  if (r) {
    free_node(r->root);
    free(r);
  }
}

/** @brief Internal helper to get the next URL segment.
 * @param path Pointer to the path string.
 * @return The next segment string. */
static char* get_next_segment(const char** path) {
  if (**path == '/') (*path)++;
  if (**path == '\0') return NULL;

  const char* start = *path;
  while (**path != '/' && **path != '\0') (*path)++;

  size_t len = *path - start;
  char* segment = malloc(len + 1);
  if (!segment) return NULL;
  memcpy(segment, start, len);
  segment[len] = '\0';
  return segment;
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
    gin_router_node_t* prev = NULL;
    for (gin_router_node_t* child = curr->children; child != NULL;
         child = child->sibling) {
      if (child->type == type && strcmp(child->segment, seg_name) == 0) {
        found = child;
        break;
      }
      prev = child;
    }

    if (!found) {
      found = calloc(1, sizeof(gin_router_node_t));
      if (!found) {
        free(seg);
        return;
      }
      found->segment = strdup(seg_name);
      if (!found->segment) {
        free(found);
        free(seg);
        return;
      }
      found->type = type;
      if (prev) {
        prev->sibling = found;
      } else {
        curr->children = found;
      }
    }
    free(seg);
    curr = found;
    if (type == GIN_NODE_WILDCARD) break;
  }

  gin_method_handler_t* mh = malloc(sizeof(gin_method_handler_t));
  if (!mh) return;
  mh->method = strdup(method);
  if (!mh->method) {
    free(mh);
    return;
  }

  mh->handlers = malloc((handler_count + 1) * sizeof(gin_handler_t));
  if (!mh->handlers) {
    free(mh->method);
    free(mh);
    return;
  }
  memcpy(mh->handlers, handlers, handler_count * sizeof(gin_handler_t));
  mh->handlers[handler_count] = NULL;

  mh->next = curr->handlers;
  curr->handlers = mh;
}

/** @brief Internal helper to match route.
 * @param node The current node.
 * @param method The HTTP method.
 * @param path The path.
 * @param ctx The request context.
 * @return The handler array, or NULL if no match. */
static gin_handler_t* match_node(gin_router_node_t* node, const char* method,
                                 const char* path, gin_ctx_t* ctx) {
  if (*path == '/') path++;

  if (*path == '\0') {
    for (gin_method_handler_t* h = node->handlers; h != NULL; h = h->next) {
      if (strcmp(h->method, method) == 0) {
        return h->handlers;
      }
    }
    return NULL;
  }

  const char* next_path = path;
  while (*next_path != '/' && *next_path != '\0') next_path++;
  size_t seg_len = next_path - path;

  for (gin_router_node_t* child = node->children; child != NULL;
       child = child->sibling) {
    if (child->type == GIN_NODE_STATIC) {
      if (strlen(child->segment) == seg_len &&
          strncmp(child->segment, path, seg_len) == 0) {
        gin_handler_t* h = match_node(child, method, next_path, ctx);
        if (h) return h;
      }
    } else if (child->type == GIN_NODE_PARAM) {
      int old_params_count = ctx ? ctx->params_count : 0;
      int param_added = 0;
      if (ctx && ctx->params_count < GIN_MAX_PARAMS) {
        char* key = strdup(child->segment);
        char* val = malloc(seg_len + 1);
        if (key && val) {
          memcpy(val, path, seg_len);
          val[seg_len] = '\0';
          ctx->params[ctx->params_count].key = key;
          ctx->params[ctx->params_count].value = val;
          ctx->params_count++;
          param_added = 1;
        } else {
          free(key);
          free(val);
        }
      }

      gin_handler_t* h = match_node(child, method, next_path, ctx);
      if (h) return h;

      if (ctx && param_added) {
        while (ctx->params_count > old_params_count) {
          ctx->params_count--;
          free(ctx->params[ctx->params_count].key);
          free(ctx->params[ctx->params_count].value);
        }
      }
    } else if (child->type == GIN_NODE_WILDCARD) {
      int old_params_count = ctx ? ctx->params_count : 0;
      int param_added = 0;
      if (ctx && ctx->params_count < GIN_MAX_PARAMS) {
        char* key = strdup(child->segment);
        char* val = strdup(path);
        if (key && val) {
          ctx->params[ctx->params_count].key = key;
          ctx->params[ctx->params_count].value = val;
          ctx->params_count++;
          param_added = 1;
        } else {
          free(key);
          free(val);
        }
      }
      for (gin_method_handler_t* h = child->handlers; h != NULL; h = h->next) {
        if (strcmp(h->method, method) == 0) {
          return h->handlers;
        }
      }
      if (ctx && param_added) {
        while (ctx->params_count > old_params_count) {
          ctx->params_count--;
          free(ctx->params[ctx->params_count].key);
          free(ctx->params[ctx->params_count].value);
        }
      }
    }
  }

  return NULL;
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
