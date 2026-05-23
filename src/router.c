/**
 * @file router.c
 * @brief Router implementation.
 * MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "csilk.h"

/** @brief Maximum number of children per router tree node. */
#define CSILK_MAX_CHILDREN 128

/** @brief Node type for router trie. */
typedef enum {
  CSILK_NODE_STATIC,
  CSILK_NODE_PARAM,
  CSILK_NODE_WILDCARD
} csilk_node_type_t;

/** @brief Method-specific handler mapping. */
typedef struct csilk_method_handler_s {
  char* method;                       /**< HTTP method string. */
  csilk_handler_t* handlers;            /**< Array of handlers for this method. */
  struct csilk_method_handler_s* next;  /**< Next method handler in list. */
} csilk_method_handler_t;

/** @brief Node in the router trie. */
struct csilk_router_node_s {
  char* segment;                                      /**< URL path segment. */
  csilk_node_type_t type;                               /**< Type of node (static/param/wildcard). */
  csilk_method_handler_t* handlers;                     /**< Method handlers for this node. */
  struct csilk_router_node_s* children[CSILK_MAX_CHILDREN]; /**< Child nodes array. */
  int children_count;                                 /**< Number of child nodes. */
};

/** @brief Create a new node.
 * @param segment The segment name.
 * @param type The node type.
 * @return A new csilk_router_node_t instance. */
static csilk_router_node_t* node_new(const char* segment, csilk_node_type_t type) {
  csilk_router_node_t* node = calloc(1, sizeof(csilk_router_node_t));
  if (!node) return NULL;
  node->segment = strdup(segment);
  if (!node->segment) {
    free(node);
    return NULL;
  }
  node->type = type;
  return node;
}

/** @brief Free a node and its children.
 * @param node The node to free. */
static void node_free(csilk_router_node_t* node) {
  if (!node) return;
  free(node->segment);
  csilk_method_handler_t* mh = node->handlers;
  while (mh) {
    csilk_method_handler_t* next = mh->next;
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

csilk_router_t* csilk_router_new() {
  csilk_router_t* r = malloc(sizeof(csilk_router_t));
  if (!r) return NULL;
  r->root = node_new("", CSILK_NODE_STATIC);
  return r;
}

void csilk_router_free(csilk_router_t* r) {
  if (!r) return;
  node_free(r->root);
  free(r);
}

void csilk_router_add(csilk_router_t* r, const char* method, const char* path,
                    csilk_handler_t* handlers, size_t handler_count) {
  if (!r || !r->root || !method || !path || !handlers) return;
  csilk_router_node_t* curr = r->root;
  const char* p = path;
  char* seg;

  while ((seg = get_next_segment(&p)) != NULL) {
    csilk_node_type_t type = CSILK_NODE_STATIC;
    char* seg_name = seg;
    if (seg[0] == ':') {
      type = CSILK_NODE_PARAM;
      seg_name = seg + 1;
    } else if (seg[0] == '*') {
      type = CSILK_NODE_WILDCARD;
      seg_name = seg + 1;
    }

    csilk_router_node_t* found = NULL;
    for (int i = 0; i < curr->children_count; i++) {
      if (curr->children[i]->type == type &&
          strcmp(curr->children[i]->segment, seg_name) == 0) {
        found = curr->children[i];
        break;
      }
    }

    if (!found) {
      if (curr->children_count < CSILK_MAX_CHILDREN) {
        found = node_new(seg_name, type);
        curr->children[curr->children_count++] = found;
      }
    }
    free(seg);
    if (!found) return; // Should not happen unless CSILK_MAX_CHILDREN exceeded
    curr = found;
    if (type == CSILK_NODE_WILDCARD) break;
  }

  csilk_method_handler_t* mh = curr->handlers;
  while (mh) {
    if (strcmp(mh->method, method) == 0) {
      return;
    }
    mh = mh->next;
  }

  mh = malloc(sizeof(csilk_method_handler_t));
  if (mh) {
    mh->method = strdup(method);
    if (!mh->method) {
      free(mh);
      return;
    }
    mh->handlers = malloc(sizeof(csilk_handler_t) * (handler_count + 1));
    if (!mh->handlers) {
      free(mh->method);
      free(mh);
      return;
    }
    memcpy(mh->handlers, handlers, sizeof(csilk_handler_t) * handler_count);
    mh->handlers[handler_count] = NULL;
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
static csilk_handler_t* match_node(csilk_router_node_t* node, const char* method,
                                 const char* path, csilk_ctx_t* ctx) {
  if (!path || *path == '\0' || strcmp(path, "/") == 0) {
    csilk_method_handler_t* mh = node->handlers;
    while (mh) {
      if (strcmp(mh->method, method) == 0) return mh->handlers;
      mh = mh->next;
    }
    return NULL;
  }

  const char* p = path;
  char* seg = get_next_segment(&p);
  if (!seg) return NULL;

  csilk_handler_t* result = NULL;
  for (int i = 0; i < node->children_count; i++) {
    csilk_router_node_t* child = node->children[i];
    if (child->type == CSILK_NODE_STATIC) {
      if (strcmp(child->segment, seg) == 0) {
        result = match_node(child, method, p, ctx);
      }
    } else if (child->type == CSILK_NODE_PARAM) {
      int param_added = 0;
      if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        char* key = strdup(child->segment);
        char* val = strdup(seg);
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
      result = match_node(child, method, p, ctx);
      if (!result && param_added) {
        ctx->params_count--;
        free(ctx->params[ctx->params_count].key);
        free(ctx->params[ctx->params_count].value);
      }
    } else if (child->type == CSILK_NODE_WILDCARD) {
      if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
        char* key = strdup(child->segment);
        char* val = strdup(path + 1);
        if (key && val) {
          ctx->params[ctx->params_count].key = key;
          ctx->params[ctx->params_count].value = val;
          ctx->params_count++;
        } else {
          free(key);
          free(val);
        }
      }
      csilk_method_handler_t* mh = child->handlers;
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

csilk_handler_t* csilk_router_match(csilk_router_t* r, const char* method,
                                const char* path) {
  if (!r || !r->root || !method || !path) return NULL;
  return match_node(r->root, method, path, NULL);
}

int csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c) {
  if (!r || !c || !r->root || !c->request.method || !c->request.path) return 0;
  c->params_count = 0;
  csilk_handler_t* handlers =
      match_node(r->root, c->request.method, c->request.path, c);
  if (handlers) {
    c->handlers = handlers;
    c->handler_index = -1;
    return 1;
  }
  return 0;
}
