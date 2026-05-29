/**
 * @file group.c
 * @brief Route group implementation — prefix nesting with inherited middleware.
 *
 * ## Architecture
 * Route groups let users attach middleware + route handlers under a common
 * URL prefix. Groups form a tree: a root group (prefix "") has zero or more
 * child subgroups, each with their own prefix segment.
 *
 * ## Middleware Chain Assembly
 * When a route is registered on a group, the framework:
 *   1. Recursively collects middleware from the group and all its ancestors
 *      (see gather_handlers()). Parent middleware comes first.
 *   2. Appends the route-specific handler chain.
 *   3. Registers the combined chain with the router.
 *
 * This means parent-group middleware runs before child-group middleware
 * for every request matching that prefix.
 *
 * ## Prefix Joining
 * Path components are joined via join_path(), which normalizes slashes:
 * e.g., parent="/api", child="/v1" → "/api/v1".
 *
 * @copyright MIT License
 */

#include <stdlib.h>
#include <string.h>

#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Route group — holds a URL prefix, middleware chain, and optional
 * parent group for nesting.
 *
 * Groups form a tree via the parent pointer. The full middleware chain
 * for any route is the concatenation of parent middleware (recursively)
 * followed by this group's middleware, followed by the route handlers.
 */
#define CSILK_GROUP_MW_INIT_CAP 4

struct csilk_group_s {
	char* prefix;		      /**< URL prefix for routes in this group.
                           *   Combined with parent prefix at creation
                           *   via join_path(). Freed in csilk_group_free().
                           *   e.g., parent="/api", child="/v1" → "/api/v1". */
	csilk_router_t* router;	      /**< Router where routes are registered. Inherited
                           *   from parent or set explicitly in
                           *   csilk_group_new(). Not owned by the group. */
	csilk_handler_t* middlewares; /**< Dynamically-grown array of middleware
                                 *   handlers for this group. Grown by
                                 *   doubling (initial cap = 4). Freed in
                                 *   csilk_group_free(). */
	size_t middleware_count;      /**< Current number of middleware handlers. */
	size_t middleware_capacity;   /**< Allocated capacity (always ≥ count). */
	csilk_group_t* parent;	      /**< Parent group in the nesting tree, or NULL
                                 *   for root groups. Used by gather_handlers()
                                 *   to walk up the tree. Not owned. */
};

/** @brief Internal: join two URL path components with a single '/' separator.
 *
 * ## Algorithm
 * 1. Handle trivial cases: if either component is NULL or empty, return
 *    a strdup of the other (or "/" if both are empty).
 * 2. Strip trailing slashes from p1 by decrementing l1.
 * 3. Strip leading slashes from p2 by advancing p2_start.
 * 4. Allocate l1 + l2 + 1 (slash) + 1 (null) bytes.
 * 5. Memcpy p1, then '/', then p2_start.
 *
 * Examples:
 *   join_path("/api/", "/v1")  → "/api/v1"
 *   join_path("api", "v1")     → "api/v1"
 *   join_path(NULL, "/users")  → "/users"
 *   join_path("", "")          → "/"
 *
 * @param p1 First path component (may be empty or NULL).
 * @param p2 Second path component (may be empty or NULL).
 * @return A newly heap-allocated joined path string. Caller must free().
 * @note Returns strdup("/") if both components are NULL or empty. */
static char*
join_path(const char* p1, const char* p2)
{
	if (!p1 || *p1 == '\0') {
		return strdup(p2 ? p2 : "/");
	}
	if (!p2 || *p2 == '\0') {
		return strdup(p1);
	}

	size_t l1 = strlen(p1);
	while (l1 > 0 && p1[l1 - 1] == '/') {
		l1--;
	}

	const char* p2_start = p2;
	while (*p2_start == '/') {
		p2_start++;
	}

	size_t l2 = strlen(p2_start);

	char* res = malloc(l1 + l2 + 2);
	if (!res) {
		return NULL;
	}

	if (l1 > 0) {
		memcpy(res, p1, l1);
		res[l1] = '/';
		memcpy(res + l1 + 1, p2_start, l2);
		res[l1 + 1 + l2] = '\0';
	} else {
		res[0] = '/';
		memcpy(res + 1, p2_start, l2);
		res[1 + l2] = '\0';
	}
	return res;
}

/** @brief Create a new root route group with the given URL prefix.
 *
 * Root groups are attached directly to a router. All routes added to this
 * group will be prefixed with @p prefix.
 *
 * @param router The router instance this group belongs to.
 * @param prefix URL prefix for all routes in this group (e.g., "/api/v1").
 *               Pass NULL or "/" for no prefix.
 * @return A new csilk_group_t, or NULL on allocation failure.
 * @note The group must be freed with csilk_group_free(). */
csilk_group_t*
csilk_group_new(csilk_router_t* router, const char* prefix)
{
	csilk_group_t* group = calloc(1, sizeof(csilk_group_t));
	if (!group) {
		return NULL;
	}

	group->router = router;
	group->prefix = strdup(prefix ? prefix : "/");
	if (!group->prefix) {
		free(group);
		return NULL;
	}
	return group;
}

/** @brief Create a child subgroup nested under a parent group.
 *
 * The child inherits the parent's router and its prefix is joined with the
 * parent's prefix (e.g., parent="/api", child="/v1" -> combined prefix
 * "/api/v1").
 *
 * @param parent The parent group (cannot be NULL).
 * @param prefix URL prefix for this subgroup (e.g., "/v1").
 * @return A new csilk_group_t, or NULL on failure.
 * @note The child must be freed separately with csilk_group_free() — freeing
 *       the parent does NOT free its children. */
csilk_group_t*
csilk_group_group(csilk_group_t* parent, const char* prefix)
{
	if (!parent) {
		return NULL;
	}

	csilk_group_t* group = calloc(1, sizeof(csilk_group_t));
	if (!group) {
		return NULL;
	}

	group->parent = parent;
	group->router = parent->router;
	group->prefix = join_path(parent->prefix, prefix);

	if (!group->prefix) {
		free(group);
		return NULL;
	}
	return group;
}

/** @brief Register a middleware handler that applies to all routes in this
 * group.
 *
 * Middleware handlers are executed before route handlers in the order they
 * are registered. The internal middleware array grows dynamically (doubling
 * capacity) as needed.
 *
 * @param group   The route group.
 * @param handler Middleware handler function. Receives the request context
 *                and should call csilk_next() to pass control forward.
 * @note Middleware from parent groups is automatically inherited by child
 *       groups and prepended before child middleware. */
void
csilk_group_use(csilk_group_t* group, csilk_handler_t handler)
{
	if (!group) {
		return;
	}
	if (group->middleware_count >= group->middleware_capacity) {
		size_t new_cap = group->middleware_capacity ? group->middleware_capacity * 2
							    : CSILK_GROUP_MW_INIT_CAP;
		csilk_handler_t* new_mw =
		    realloc(group->middlewares, new_cap * sizeof(csilk_handler_t));
		if (!new_mw) {
			return;
		}
		group->middlewares = new_mw;
		group->middleware_capacity = new_cap;
	}
	group->middlewares[group->middleware_count++] = handler;
}

/** @brief Internal: recursively collect all middleware handlers from a group
 *        and its ancestors into a flat array.
 *
 * ## How middleware inheritance works
 *
 * The recursion goes depth-first to the root, then appends middleware on
 * the way back up:
 *
 *   gather_handlers(child, &arr, &n)
 *     → gather_handlers(parent, &arr, &n)       // recurse to root first
 *       → gather_handlers(grandparent, &arr, &n) // root's parent is NULL
 *         → (no parent — return)
 *       → memcpy grandparent's middlewares into arr[n..]
 *       → n += grandparent.middleware_count
 *     → memcpy parent's middlewares into arr[n..]
 *     → n += parent.middleware_count
 *   → memcpy child's middlewares into arr[n..]
 *   → n += child.middleware_count
 *
 * Result: [grandparent_mw..., parent_mw..., child_mw...]
 * This guarantees parent middleware executes before child middleware.
 *
 * @param group    The leaf group.
 * @param handlers [in/out] Pointer to the handlers array (realloc'd as needed).
 * @param count    [in/out] Number of handlers collected so far.
 * @return 0 on success, -1 on realloc failure.
 * @note The caller must free the returned handlers array with free(). */
static int
gather_handlers(csilk_group_t* group, csilk_handler_t** handlers, size_t* count)
{
	if (group->parent) {
		if (gather_handlers(group->parent, handlers, count) != 0) {
			return -1;
		}
	}

	if (group->middleware_count > 0) {
		csilk_handler_t* new_handlers = realloc(
		    *handlers, (*count + group->middleware_count) * sizeof(csilk_handler_t));
		if (!new_handlers) {
			return -1;
		}
		*handlers = new_handlers;
		memcpy(*handlers + *count,
		       group->middlewares,
		       group->middleware_count * sizeof(csilk_handler_t));
		*count += group->middleware_count;
	}
	return 0;
}

/** @brief Register a route on this group with a single handler.
 *
 * The route's path is automatically prefixed with the group's prefix. The
 * handler is wrapped in a 1-element array and passed to
 * csilk_group_add_handlers() which combines group middleware.
 *
 * @param group   The route group.
 * @param method  HTTP method (e.g., "GET", "POST").
 * @param path    Path relative to the group prefix (e.g., "/users").
 * @param handler The route handler function. */
void
csilk_group_add_route(csilk_group_t* group,
		      const char* method,
		      const char* path,
		      csilk_handler_t handler)
{
	csilk_handler_t handlers[] = {handler};
	csilk_group_add_handlers(group, method, path, handlers, 1);
}

/** @brief Register a route with OpenAPI metadata (input/output types, summary,
 * description).
 *
 * Like csilk_group_add_route() but enriches the route with metadata used by
 * the OpenAPI spec generator. The metadata is stored in the method handler
 * for later retrieval by csilk_generate_openapi_json().
 *
 * @param group       The route group.
 * @param method      HTTP method.
 * @param path        Path relative to the group prefix.
 * @param handler     The route handler function.
 * @param input_type  Registered reflection type name for the request body
 *                    (e.g., "CreateUserRequest"), or NULL.
 * @param output_type Registered reflection type name for the response body,
 *                    or NULL.
 * @param summary     Short description for OpenAPI operation summary.
 * @param description Detailed description for OpenAPI operation. */
void
csilk_group_add_route_extended(csilk_group_t* group,
			       const char* method,
			       const char* path,
			       csilk_handler_t handler,
			       const char* input_type,
			       const char* output_type,
			       const char* summary,
			       const char* description)
{
	if (!group || !method || !path || !handler) {
		return;
	}

	char* full_path = join_path(group->prefix, path);
	if (!full_path) {
		return;
	}

	csilk_handler_t* combined_handlers = NULL;
	size_t combined_count = 0;

	if (gather_handlers(group, &combined_handlers, &combined_count) != 0) {
		free(full_path);
		free(combined_handlers);
		return;
	}

	csilk_handler_t* new_handlers =
	    realloc(combined_handlers, (combined_count + 1) * sizeof(csilk_handler_t));
	if (!new_handlers) {
		free(full_path);
		free(combined_handlers);
		return;
	}
	combined_handlers = new_handlers;
	combined_handlers[combined_count] = handler;
	combined_count++;

	csilk_router_add_extended(group->router,
				  method,
				  full_path,
				  combined_handlers,
				  combined_count,
				  full_path,
				  input_type,
				  output_type,
				  summary,
				  description);

	free(full_path);
	free(combined_handlers);
}

/** @copydoc csilk_group_add_route_extended
 *  @param perm_required  Permission required for this route, or NULL.
 *  @param perm_resource  Resource pattern for permission check, or NULL.
 *
 *  Permission metadata is forwarded to the router which stores it alongside
 *  the route for authorization middleware to inspect at request time. */
void
csilk_group_add_route_extended_perm(csilk_group_t* group,
				    const char* method,
				    const char* path,
				    csilk_handler_t handler,
				    const char* input_type,
				    const char* output_type,
				    const char* summary,
				    const char* description,
				    const char* perm_required,
				    const char* perm_resource)
{
	if (!group || !method || !path || !handler) {
		return;
	}

	char* full_path = join_path(group->prefix, path);
	if (!full_path) {
		return;
	}

	csilk_handler_t* combined_handlers = NULL;
	size_t combined_count = 0;

	if (gather_handlers(group, &combined_handlers, &combined_count) != 0) {
		free(full_path);
		free(combined_handlers);
		return;
	}

	csilk_handler_t* new_handlers =
	    realloc(combined_handlers, (combined_count + 1) * sizeof(csilk_handler_t));
	if (!new_handlers) {
		free(full_path);
		free(combined_handlers);
		return;
	}
	combined_handlers = new_handlers;
	combined_handlers[combined_count] = handler;
	combined_count++;

	csilk_router_add_extended_perm(group->router,
				       method,
				       full_path,
				       combined_handlers,
				       combined_count,
				       full_path,
				       input_type,
				       output_type,
				       summary,
				       description,
				       perm_required,
				       perm_resource);

	free(full_path);
	free(combined_handlers);
}

/** @brief Register a route with a custom chain of handlers (middleware + route
 * handler).
 *
 * ## Assembly pipeline
 *   1. join_path(group->prefix, path) → full_path (e.g., "/api/v1/users").
 *   2. gather_handlers(group, ...) → flat array of all inherited middleware
 *      (parent first, then child), grown via realloc.
 *   3. realloc to append caller's handlers[] to the end.
 *   4. csilk_router_add(group->router, method, full_path, combined, count).
 *
 * The final handler chain stored in the router looks like:
 *   [parent_mw..., group_mw..., handler_1, ..., handler_n]
 *
 * @param group    The route group.
 * @param method   HTTP method.
 * @param path     Path relative to the group prefix.
 * @param handlers Array of handler functions (the chain).
 * @param count    Number of handlers in the array.
 * @note The handlers array is combined with group middleware — group
 *       middleware always runs first, followed by the provided handlers. */
void
csilk_group_add_handlers(csilk_group_t* group,
			 const char* method,
			 const char* path,
			 csilk_handler_t* handlers,
			 size_t count)
{
	if (!group || !handlers || count == 0) {
		return;
	}

	char* full_path = join_path(group->prefix, path);
	if (!full_path) {
		return;
	}

	csilk_handler_t* combined_handlers = NULL;
	size_t combined_count = 0;

	if (gather_handlers(group, &combined_handlers, &combined_count) != 0) {
		free(full_path);
		free(combined_handlers);
		return;
	}

	csilk_handler_t* new_handlers =
	    realloc(combined_handlers, (combined_count + count) * sizeof(csilk_handler_t));
	if (!new_handlers) {
		free(full_path);
		free(combined_handlers);
		return;
	}
	combined_handlers = new_handlers;
	memcpy(combined_handlers + combined_count, handlers, count * sizeof(csilk_handler_t));
	combined_count += count;

	csilk_router_add(group->router, method, full_path, combined_handlers, combined_count);

	free(full_path);
	free(combined_handlers);
}

/** @brief Free all resources associated with a route group.
 *
 * Releases the group's prefix string, middleware handlers array, and the
 * group struct itself. Does NOT free child groups or the router.
 *
 * @param group The group to free (may be NULL).
 * @note Child groups created with csilk_group_group() must be freed
 *       separately. The router is not owned by the group. */
void
csilk_group_free(csilk_group_t* group)
{
	if (!group) {
		return;
	}
	free(group->prefix);
	free(group->middlewares);
	free(group);
}
