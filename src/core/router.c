/**
 * @file router.c
 * @brief Router implementation using a radix tree (trie) for URL path matching.
 * @copyright MIT License
 *
 * === Architecture Overview ===
 *
 * The router organises registered URL patterns into a compressed trie (radix
 * tree). Each node represents a single path segment, and the path from root
 * to a handler-holding node encodes the full route pattern.
 *
 * Node types (csilk_node_type_t):
 *   STATIC   - Matches a literal segment, e.g. "users", "api", "v1".
 *              Comparison is a simple strcmp against the request segment.
 *   PARAM    - Matches any single segment; the matched value is captured
 *              as a named parameter. Identified by a ':' prefix in the
 *              registered pattern (e.g., "/:id" -> segment="id").
 *   WILDCARD - Matches the remainder of the path (one or more segments).
 *              Identified by a '*' prefix (e.g., "WILDCARD filepath").
 *              Once a wildcard node is reached, matching stops and the
 *              entire tail of the URL is captured as a single parameter.
 *
 * Insertion (router_add_full):
 *   The path is split into segments by '/'. For each segment, the algorithm
 *   checks whether a child node of the same type and segment name already
 *   exists. If not, a new node is created and appended. The method handler
 *   chain is stored at the final (leaf) node.
 *
 * Matching (match_node):
 *   Recursive depth-first search with backtracking. At each node, the next
 *   segment from the request path is extracted via get_next_segment(). The
 *   algorithm tries all children in order: STATIC children are compared
 *   exactly; PARAM children always match (capturing the segment value);
 *   WILDCARD children match the remaining path and terminate the search.
 *   When a PARAM branch fails deeper in the tree, the capture is rolled
 *   back (params_count decremented, memory freed) so sibling STATIC or
 *   PARAM nodes can be tried. The first successful match is returned.
 *
 * Priority / ordering:
 *   Children are tried in insertion order. STATIC and PARAM nodes at the
 *   same depth are both explored, but STATIC is attempted first if inserted
 *   before a corresponding PARAM. For well-defined priority, register STATIC
 *   routes before PARAM routes at the same level. WILDCARD nodes terminate
 *   the path and are tried as a fallback.
 */

#include <stdlib.h>
#include <string.h>

#include "core/ctx_types.h"
#include "csilk/core/internal.h"
#include "csilk/csilk.h"

/** @brief Node type for router trie.
 *
 * CSILK_NODE_STATIC  - Matches a literal path segment (e.g., "users").
 *                      strcmp equality is required for a match.
 * CSILK_NODE_PARAM   - Matches any single path segment. The segment
 *                      name (without ':' prefix) becomes the parameter key.
 *                      e.g., ":id" captures "42" as ctx->params["id"].
 * CSILK_NODE_WILDCARD - Matches the remainder of the URL path (any
 *                      number of segments including none). The segment
 *                      name (without '*' prefix) becomes the parameter key.
 *                      e.g., "WILDCARD path" captures "foo/bar/baz" for request
 *                      "/foo/bar/baz". Wildcard is terminal and always
 *                      tried last during matching. */
typedef enum { CSILK_NODE_STATIC, CSILK_NODE_PARAM, CSILK_NODE_WILDCARD } csilk_node_type_t;

/** @brief Node in the router trie — represents a URL path segment.
 *
 * The router uses a radix tree (trie) structure where each node represents
 * a path segment. Nodes can be static (e.g., "users"), parameterized
 * (e.g., ":id"), or wildcard (e.g., "*"). Each node stores handlers for
 * matching HTTP methods at that path.
 */
struct csilk_router_node_s {
	char* segment;			  /**< URL path segment. */
	size_t segment_len;		  /**< Cached length of segment. */
	csilk_node_type_t type;		  /**< Type of node (static/param/wildcard). */
	csilk_method_handler_t* handlers; /**< Method handlers for this node. */
	struct csilk_router_node_s* children[CSILK_MAX_CHILDREN]; /**< Child nodes array. */
	int children_count;					  /**< Number of child nodes. */
};

/** @brief Create a new router trie node.
 *
 * Allocates and initializes a node with the given path segment and type.
 * The segment string is duplicated internally.
 *
 * @param segment The path segment name (e.g., "users", "id" for ":id").
 * @param type    The node type (static, param, or wildcard).
 * @return A new csilk_router_node_t, or nullptr on allocation failure.
 * @note The returned node must be freed with node_free(). */
static csilk_router_node_t*
node_new(const char* segment, csilk_node_type_t type)
{
	csilk_router_node_t* node = calloc(1, sizeof(csilk_router_node_t));
	if (!node) {
		return nullptr;
	}
	node->segment = strdup(segment);
	if (!node->segment) {
		free(node);
		return nullptr;
	}
	node->segment_len = strlen(node->segment);
	node->type = type;
	return node;
}

/** @brief Recursively free a router trie node and all its descendants.
 *
 * Frees the segment string, the linked list of method handlers (including
 * method, handlers array, and path strings), recursively frees all child
 * nodes, and finally frees the node itself.
 *
 * @param node The node to free (may be nullptr). */
static void
node_free(csilk_router_node_t* node)
{
	if (!node) {
		return;
	}
	free(node->segment);
	csilk_method_handler_t* mh = node->handlers;
	while (mh) {
		csilk_method_handler_t* next = mh->next;
		free(mh->method);
		free(mh->handlers);
		free(mh->path);
		free(mh);
		mh = next;
	}
	for (int i = 0; i < node->children_count; i++) {
		node_free(node->children[i]);
	}
	free(node);
}

/** @brief Extract the next path segment from a URL path string.
 *
 * get_next_segment is the bridge between the raw URL path and the trie
 * traversal. It consumes one segment at a time, providing the atomic
 * unit that each trie node represents.
 *
 * Behaviour:
 *   1. Skip all leading '/' characters — this normalises paths such as
 *      "//foo//bar" into the same segments as "/foo/bar".
 *   2. If the remaining string is empty after skipping slashes, return
 *      nullptr (no more segments).
 *   3. Record the start pointer, then advance until the next '/' or
 *      end-of-string. The characters in between form one segment.
 *   4. Allocate and return a heap copy of that segment.
 *   5. Advance the caller's pointer (@p p) past the consumed segment
 *      so that repeated calls walk sequentially through the path.
 *
 * Example: for path "/users/42/profile"
 *   Call 1: returns "users",    p -> "/42/profile"
 *   Call 2: returns "42",       p -> "/profile"
 *   Call 3: returns "profile",  p -> ""
 *   Call 4: returns nullptr
 *
 * Edge cases:
 *   - Leading slashes are always skipped, so path "/" yields nullptr
 *     (zero segments), matching the root node.
 *   - Paths with trailing slashes like "/users/" return "users" then
 *     nullptr — the trailing '/' is consumed but produces no segment.
 *
 * @param p [in/out] Pointer to the current position in the path string.
 *           Updated to point past the extracted segment.
 * @param len [out] Pointer to receive the length of the segment.
 * @return A pointer to the start of the segment within the original path string,
 *         or nullptr if no more segments are found. */
static const char*
get_next_segment(const char** p, size_t* len)
{
	if (!*p || **p == '\0') {
		return nullptr;
	}

	while (**p == '/') {
		(*p)++;
	}
	if (**p == '\0') {
		return nullptr;
	}

	const char* start = *p;
	while (**p != '/' && **p != '\0') {
		(*p)++;
	}

	*len = (size_t)(*p - start);
	return start;
}

/** @brief Create a new router instance with an empty root ("") static node.
 *
 * @return A new csilk_router_t, or nullptr on allocation failure.
 * @note The router must be freed with csilk_router_free(). The root node
 *       is a static node with an empty segment, representing the root "/". */
csilk_router_t*
csilk_router_new()
{
	csilk_router_t* r = malloc(sizeof(csilk_router_t));
	if (!r) {
		return nullptr;
	}
	r->root = node_new("", CSILK_NODE_STATIC);
	return r;
}

/** @brief Free a router instance and all its trie nodes.
 *
 * Recursively frees the entire trie starting from the root node, then
 * frees the router struct itself.
 *
 * @param r The router to free (may be nullptr). */
void
csilk_router_free(csilk_router_t* r)
{
	if (!r) {
		return;
	}
	node_free(r->root);
	free(r);
}

/** @brief Internal: recursively traverse the trie and collect all route
 * metadata.
 *
 * For each node, iterates the method handler linked list and adds a route
 * object (method, path, input_type, output_type, summary, description) to
 * the cJSON array. Then recurses into child nodes.
 *
 * @param node   Current trie node.
 * @param routes cJSON array to append route objects to. */
static void
node_collect_routes(csilk_router_node_t* node, cJSON* routes)
{
	if (!node) {
		return;
	}

	// Collect routes for this node
	csilk_method_handler_t* mh = node->handlers;
	while (mh) {
		cJSON* route = cJSON_CreateObject();
		if (route) {
			cJSON_AddStringToObject(route, "method", mh->method);
			cJSON_AddStringToObject(route, "path", mh->path ? mh->path : "");
			cJSON_AddStringToObject(
			    route, "input_type", mh->input_type ? mh->input_type : "");
			cJSON_AddStringToObject(
			    route, "output_type", mh->output_type ? mh->output_type : "");
			cJSON_AddStringToObject(route, "summary", mh->summary ? mh->summary : "");
			cJSON_AddStringToObject(
			    route, "description", mh->description ? mh->description : "");
			cJSON_AddItemToArray(routes, route);
		}
		mh = mh->next;
	}

	// Recurse into children
	for (int i = 0; i < node->children_count; i++) {
		node_collect_routes(node->children[i], routes);
	}
}

/** @brief Collect all registered routes from the router tree as a cJSON array.
 *
 * Traverses the entire trie and returns a JSON array where each element is
 * an object with method, path, input_type, output_type, summary, and
 * description fields.
 *
 * @param r The router instance.
 * @return A cJSON array of route objects. Caller must free with cJSON_Delete().
 *         Returns nullptr if the router is nullptr or allocation fails. */
cJSON*
csilk_router_collect_routes(csilk_router_t* r)
{
	if (!r || !r->root) {
		return nullptr;
	}
	cJSON* array = cJSON_CreateArray();
	if (!array) {
		return nullptr;
	}
	node_collect_routes(r->root, array);
	return array;
}

/** @brief Register a route with method, path pattern, handler chain, and
 * optional OpenAPI metadata.
 *
 * Walks the path string, segment by segment, inserting nodes into the trie.
 * Path segments prefixed with ':' are treated as parameters (e.g., "/:id"),
 * and segments prefixed with '*' are treated as wildcards that match the
 * remainder of the path (e.g., "/assets/star-path"). The method handler
 * is stored at the final node
 * with the provided metadata.
 * If a route with the same method already exists at the same path, the new
 * registration is silently ignored (no override).
 *
 * @param r              The router instance.
 * @param method         HTTP method (e.g., "GET", "POST").
 * @param path           URL path pattern (e.g., "/users/:id").
 * @param handlers       Array of handler functions (nullptr-terminated).
 * @param handler_count  Number of handlers (excluding the nullptr terminator).
 * @param path_pattern   Original path pattern for OpenAPI metadata (may differ
 *                       from @p path if prefix was prepended by group).
 * @param input_type     Registered type name for request body, or nullptr.
 * @param output_type    Registered type name for response body, or nullptr.
 * @param summary        OpenAPI operation summary, or nullptr.
 * @param description    OpenAPI operation description, or nullptr.
 * @note The router does NOT support overriding an existing route — duplicate
 *       method+path registrations are silently dropped.
 * @note The handler array is copied; the caller may free it after this call. */
static void
router_add_full(csilk_router_t* r,
		const char* method,
		const char* path,
		csilk_handler_t* handlers,
		size_t handler_count,
		const char* path_pattern,
		const char* input_type,
		const char* output_type,
		const char* summary,
		const char* description,
		const char* perm_required,
		const char* perm_resource)
{
	if (!r || !r->root || !method || !path || !handlers) {
		return;
	}
	csilk_router_node_t* curr = r->root;
	const char* p = path;
	const char* seg;
	size_t len;

	while ((seg = get_next_segment(&p, &len)) != nullptr) {
		// Classify the segment based on its first character:
		//   ':' prefix -> PARAM node (matches any single segment, captured as named
		//   param)
		//   '*' prefix -> WILDCARD node (matches all remaining segments, captured
		//   as named param) otherwise  -> STATIC node (exact literal match
		//   required)
		// The prefix character is stripped from seg_name so the node stores
		// only the "key" portion (e.g., "id" for ":id", "path" for "*path").
		csilk_node_type_t type = CSILK_NODE_STATIC;
		const char* seg_name_start = seg;
		size_t seg_name_len = len;

		if (seg[0] == ':') {
			type = CSILK_NODE_PARAM;
			seg_name_start = seg + 1;
			seg_name_len = len - 1;
		} else if (seg[0] == '*') {
			type = CSILK_NODE_WILDCARD;
			seg_name_start = seg + 1;
			seg_name_len = len - 1;
		}

		// Temporary null-terminated string for existing comparison logic
		char* seg_name = malloc(seg_name_len + 1);
		if (!seg_name) {
			return;
		}
		memcpy(seg_name, seg_name_start, seg_name_len);
		seg_name[seg_name_len] = '\0';

		csilk_router_node_t* found = nullptr;
		int insert_pos = curr->children_count;

		for (int i = 0; i < curr->children_count; i++) {
			if (curr->children[i]->type == type &&
			    strcmp(curr->children[i]->segment, seg_name) == 0) {
				found = curr->children[i];
				break;
			}
			// Maintain order: STATIC < PARAM < WILDCARD
			if (found == nullptr && curr->children[i]->type > type) {
				insert_pos = i;
				// Continue searching to see if it already exists further down
				// Actually, if we maintain order, we could optimize this search.
			}
		}

		if (!found) {
			if (curr->children_count < CSILK_MAX_CHILDREN) {
				found = node_new(seg_name, type);
				if (found) {
					// Shift children to insert in order
					for (int i = curr->children_count; i > insert_pos; i--) {
						curr->children[i] = curr->children[i - 1];
					}
					curr->children[insert_pos] = found;
					curr->children_count++;
				}
			}
		}
		free(seg_name);
		if (!found) {
			return; // Should not happen unless CSILK_MAX_CHILDREN exceeded
		}
		curr = found;
		// WILDCARD segments are terminal — they consume the remainder of the
		// path registration, so stop segment processing after a wildcard.
		// Any segments registered after a wildcard in the same path are ignored.
		if (type == CSILK_NODE_WILDCARD) {
			break;
		}
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
		mh->handlers[handler_count] = nullptr;
		mh->path = path_pattern ? strdup(path_pattern) : nullptr;
		mh->input_type = input_type;
		mh->output_type = output_type;
		mh->summary = summary;
		mh->description = description;
		mh->perm_required = perm_required;
		mh->perm_resource = perm_resource;
		mh->next = curr->handlers;
		curr->handlers = mh;
	}
}

/** @brief Register a route with full OpenAPI/reflection metadata.
 *
 * Extended version of csilk_router_add that also stores metadata for
 * automatic OpenAPI spec generation and request/response binding.
 * The route is inserted into the radix tree with the same matching
 * semantics (dynamic :param and *wildcard segments) as the base
 * registration function.
 *
 * @param r             Router instance.
 * @param method        HTTP method string.
 * @param path          URL pattern (e.g., "/users/:id").
 * @param handlers      Array of handler function pointers.
 * @param handler_count Number of handlers in @p handlers.
 * @param path_pattern  Canonical path pattern string for documentation
 *                      (may differ from the radix-tree path).
 * @param input_type    Registered type name for request-body binding
 *                      (nullptr if there is no request body).
 * @param output_type   Registered type name for response serialisation
 *                      (nullptr if raw response is used).
 * @param summary       Short summary of the operation (nullptr to omit from spec).
 * @param description   Detailed description of the operation (nullptr to omit).
 * @note The handlers array is stored by pointer — the caller must ensure
 *       the function pointers remain valid for the lifetime of the router. */
void
csilk_router_add_extended(csilk_router_t* r,
			  const char* method,
			  const char* path,
			  csilk_handler_t* handlers,
			  size_t handler_count,
			  const char* path_pattern,
			  const char* input_type,
			  const char* output_type,
			  const char* summary,
			  const char* description)
{
	router_add_full(r,
			method,
			path,
			handlers,
			handler_count,
			path_pattern,
			input_type,
			output_type,
			summary,
			description,
			nullptr,
			nullptr);
}

/** @brief Register a route with method, path pattern, and handler chain (no
 * OpenAPI metadata).
 *
 * Convenience wrapper around csilk_router_add_extended() that passes nullptr
 * for all optional metadata fields.
 *
 * @param r             The router instance.
 * @param method        HTTP method.
 * @param path          URL path pattern.
 * @param handlers      Array of handler functions.
 * @param handler_count Number of handlers. */
void
csilk_router_add(csilk_router_t* r,
		 const char* method,
		 const char* path,
		 csilk_handler_t* handlers,
		 size_t handler_count)
{
	csilk_router_add_extended(
	    r, method, path, handlers, handler_count, path, nullptr, nullptr, nullptr, nullptr);
}

/** @brief Register a route with permission metadata.
 *
 * Same as csilk_router_add but also stores permission requirement for
 * interface-level access control.  The auto-check middleware
 * (csilk_perm_auto_middleware) reads these fields at request time.
 *
 * @param r             The router instance.
 * @param method        HTTP method.
 * @param path          URL path pattern.
 * @param handlers      Array of handler functions.
 * @param handler_count Number of handlers.
 * @param perm_required Permission identifier (e.g., "read", "write"), or nullptr.
 * @param perm_resource Resource pattern (e.g., "users:*"), or nullptr. */
void
csilk_router_add_perm(csilk_router_t* r,
		      const char* method,
		      const char* path,
		      csilk_handler_t* handlers,
		      size_t handler_count,
		      const char* perm_required,
		      const char* perm_resource)
{
	router_add_full(r,
			method,
			path,
			handlers,
			handler_count,
			path,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			perm_required,
			perm_resource);
}

/** @brief Register a route with full metadata including permissions.
 *
 * Combines csilk_router_add_extended with permission metadata in a single
 * call.  The permission fields are used by csilk_perm_auto_middleware.
 *
 * @param r             The router instance.
 * @param method        HTTP method.
 * @param path          URL path pattern.
 * @param handlers      Array of handler functions.
 * @param handler_count Number of handlers.
 * @param path_pattern  Original path pattern for OpenAPI metadata.
 * @param input_type    Registered type name for request body, or nullptr.
 * @param output_type   Registered type name for response body, or nullptr.
 * @param summary       OpenAPI operation summary, or nullptr.
 * @param description   OpenAPI operation description, or nullptr.
 * @param perm_required Permission identifier (e.g., "read"), or nullptr.
 * @param perm_resource Resource pattern (e.g., "users:*"), or nullptr. */
void
csilk_router_add_extended_perm(csilk_router_t* r,
			       const char* method,
			       const char* path,
			       csilk_handler_t* handlers,
			       size_t handler_count,
			       const char* path_pattern,
			       const char* input_type,
			       const char* output_type,
			       const char* summary,
			       const char* description,
			       const char* perm_required,
			       const char* perm_resource)
{
	router_add_full(r,
			method,
			path,
			handlers,
			handler_count,
			path_pattern,
			input_type,
			output_type,
			summary,
			description,
			perm_required,
			perm_resource);
}

#if defined(__AVX2__)
#include <immintrin.h>
#elif defined(__ARM_NEON)
#include <arm_neon.h>
#endif

/**
 * @brief Fast memory comparison for URL segments.
 *
 * Employs SIMD instructions (if available) or word-sized operations to
 * compare small strings extremely quickly, bypassing the overhead of
 * libc memcmp/strncmp.
 *
 * @param s1  First string.
 * @param s2  Second string.
 * @param n   Number of bytes to compare.
 * @return 1 if equal, 0 if different.
 */
static inline int
csilk_memcmp_fast(const char* s1, const char* s2, size_t n)
{
#if defined(__AVX2__)
	if (n >= 32) {
		__m256i v1 = _mm256_loadu_si256((const __m256i*)s1);
		__m256i v2 = _mm256_loadu_si256((const __m256i*)s2);
		__m256i cmp = _mm256_cmpeq_epi8(v1, v2);
		int mask = _mm256_movemask_epi8(cmp);
		if (mask != (int)0xFFFFFFFF) {
			return 0;
		}
		if (n == 32) {
			return 1;
		}
		return memcmp(s1 + 32, s2 + 32, n - 32) == 0;
	}
#elif defined(__ARM_NEON)
	if (n >= 16) {
		uint8x16_t v1 = vld1q_u8((const uint8_t*)s1);
		uint8x16_t v2 = vld1q_u8((const uint8_t*)s2);
		uint8x16_t cmp = vceqq_u8(v1, v2);
		uint64_t mask_low = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 0);
		uint64_t mask_high = vgetq_lane_u64(vreinterpretq_u64_u8(cmp), 1);
		if (mask_low != UINT64_MAX || mask_high != UINT64_MAX) {
			return 0;
		}
		if (n == 16) {
			return 1;
		}
		return memcmp(s1 + 16, s2 + 16, n - 16) == 0;
	}
#endif
	/* Fast path for short segments */
	if (n == 0) {
		return 1;
	}

	/* Use unaligned word comparisons for small segments (common in URLs).
       Assumes architecture supports unaligned access (x86, modern ARM). */
#if defined(__x86_64__) || defined(__aarch64__)
	if (n >= 8) {
		if (*(const uint64_t*)s1 != *(const uint64_t*)s2) {
			return 0;
		}
		if (n == 8) {
			return 1;
		}
		s1 += 8;
		s2 += 8;
		n -= 8;
	}
	if (n >= 4) {
		if (*(const uint32_t*)s1 != *(const uint32_t*)s2) {
			return 0;
		}
		if (n == 4) {
			return 1;
		}
		s1 += 4;
		s2 += 4;
		n -= 4;
	}
	if (n >= 2) {
		if (*(const uint16_t*)s1 != *(const uint16_t*)s2) {
			return 0;
		}
		if (n == 2) {
			return 1;
		}
		s1 += 2;
		s2 += 2;
		n -= 2;
	}
	return *s1 == *s2;
#else
	return memcmp(s1, s2, n) == 0;
#endif
}

/** @brief Recursively match a request path against the trie from the given
 * node.
 *
 * This is the core matching function. It implements a depth-first,
 * backtracking search over the radix tree:
 *
 *   1. Base case: if the remaining path is empty, "/", or nullptr, check
 *      whether the current node has a handler for the given HTTP method.
 *      If so, return it — this is a successful terminal match.
 *
 *   2. Extract the next segment from the request path via get_next_segment().
 *
 *   3. Try children in insertion order. For each child:
 *        - STATIC:   match if child->segment == extracted segment (strcmp).
 *        - PARAM:    always match; capture segment value into ctx->params.
 *        - WILDCARD: always match; capture the entire remaining path
 *                    into ctx->params. Since wildcard consumes everything,
 *                    check its handlers directly (no further recursion).
 *
 *   4. Recurse: if the child matches, call match_node() on it with the
 *      remaining path pointer (p). Propagate the result back up.
 *
 *   5. Backtrack: if recursion returns nullptr (no handler found deeper),
 *      undo any PARAM capture that was made for this branch
 *      (decrement params_count, free key/value strings). Continue
 *      to the next sibling child.
 *
 *   6. If no child matches or all branches return nullptr, free the segment
 *      and return nullptr — no route matched.
 *
 * === Why backtracking matters ===
 * Consider routes:  GET /users/:id   (PARAM)
 *                   GET /users/me    (STATIC)
 *            Request: GET /users/me
 *
 * The algorithm tries STATIC "me" first (if inserted first). If STATIC
 * has a handler for GET, it is returned immediately. If STATIC had no
 * handler, the search backtracks and tries PARAM ":id", which matches
 * "me" as a parameter. This ensures STATIC routes take priority over
 * PARAM routes when both could match.
 *
 * @param node   Current trie node to match against.
 * @param method HTTP method to match.
 * @param path   Remaining path string (may be "" or "/" for exact match).
 * @param ctx    Request context for parameter capture (may be nullptr for
 *               standalone matching).
 * @param out_mh [out] Optional pointer to receive the matched method handler
 *               metadata (for OpenAPI metadata access).
 * @return Pointer to the nullptr-terminated handler array, or nullptr if no match.
 * @note Parameter and wildcard values are strdup'd and must be freed by the
 *       caller (or during csilk_ctx_cleanup()). */
static csilk_handler_t*
match_node(csilk_router_node_t* node,
	   const char* method,
	   const char* path,
	   csilk_ctx_t* ctx,
	   csilk_method_handler_t** out_mh)
{
	if (!path || *path == '\0' || (path[0] == '/' && path[1] == '\0')) {
		csilk_method_handler_t* mh = node->handlers;
		while (mh) {
			if (strcmp(mh->method, method) == 0) {
				if (out_mh) {
					*out_mh = mh;
				}
				return mh->handlers;
			}
			mh = mh->next;
		}
		return nullptr;
	}

	const char* p = path;
	size_t len;
	const char* seg = get_next_segment(&p, &len);
	if (!seg) {
		return nullptr;
	}

	csilk_handler_t* result = nullptr;
	for (int i = 0; i < node->children_count; i++) {
		csilk_router_node_t* child = node->children[i];
		if (child->type == CSILK_NODE_STATIC) {
			// Fast-path: check first character and length before strncmp
			if (child->segment[0] == seg[0] && strlen(child->segment) == len &&
			    strncmp(child->segment, seg, len) == 0) {
				result = match_node(child, method, p, ctx, out_mh);
			}
		} else if (child->type == CSILK_NODE_PARAM) {
			// PARAM node: always matches the current segment regardless of
			// its value. Capture the segment into ctx->params using the
			// child's segment name (without ':' prefix) as the key.
			int param_added = 0;
			if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
				// Use arena for parameter strings if available (much faster
				// than strdup)
				if (ctx->arena) {
					ctx->params[ctx->params_count].key =
					    csilk_arena_strdup(ctx->arena, child->segment);
					ctx->params[ctx->params_count].value =
					    csilk_arena_strndup(ctx->arena, seg, len);
				} else {
					ctx->params[ctx->params_count].key = strdup(child->segment);
					ctx->params[ctx->params_count].value = malloc(len + 1);
					if (ctx->params[ctx->params_count].value) {
						memcpy(
						    ctx->params[ctx->params_count].value, seg, len);
						ctx->params[ctx->params_count].value[len] = '\0';
					}
				}

				if (ctx->params[ctx->params_count].key &&
				    ctx->params[ctx->params_count].value) {
					ctx->params_count++;
					param_added = 1;
				} else {
					// Cleanup on allocation failure (only if not using arena)
					if (!ctx->arena) {
						free(ctx->params[ctx->params_count].key);
						free(ctx->params[ctx->params_count].value);
					}
				}
			}
			// Recurse deeper with the remaining path
			result = match_node(child, method, p, ctx, out_mh);
			// Backtracking rollback
			if (!result && param_added) {
				ctx->params_count--;
				if (!ctx->arena) {
					free(ctx->params[ctx->params_count].key);
					free(ctx->params[ctx->params_count].value);
				}
			}
		} else if (child->type == CSILK_NODE_WILDCARD) {
			// WILDCARD node: matches the entire remainder of the URL path
			if (ctx && ctx->params_count < CSILK_MAX_PARAMS) {
				// Skip leading '/' of remainder if present
				const char* val_start = path;
				while (*val_start == '/') {
					val_start++;
				}

				if (ctx->arena) {
					ctx->params[ctx->params_count].key =
					    csilk_arena_strdup(ctx->arena, child->segment);
					ctx->params[ctx->params_count].value =
					    csilk_arena_strdup(ctx->arena, val_start);
				} else {
					ctx->params[ctx->params_count].key = strdup(child->segment);
					ctx->params[ctx->params_count].value = strdup(val_start);
				}

				if (ctx->params[ctx->params_count].key &&
				    ctx->params[ctx->params_count].value) {
					ctx->params_count++;
				} else {
					if (!ctx->arena) {
						free(ctx->params[ctx->params_count].key);
						free(ctx->params[ctx->params_count].value);
					}
				}
			}
			// Directly check the wildcard node's handlers
			csilk_method_handler_t* mh = child->handlers;
			while (mh) {
				if (strcmp(mh->method, method) == 0) {
					if (out_mh) {
						*out_mh = mh;
					}
					result = mh->handlers;
					break;
				}
				mh = mh->next;
			}
		}
		if (result) {
			break;
		}
	}

	return result;
}

/** @brief Match a method+path against the routing table (standalone, no
 * context).
 *
 * Direct matching without populating path parameters. Useful for checking
 * whether a route exists without processing a full request.
 *
 * @param r      The router instance.
 * @param method HTTP method.
 * @param path   URL path.
 * @return Pointer to the handler array if matched, nullptr otherwise.
 * @note No parameter capture is performed since no context is provided. */
csilk_handler_t*
csilk_router_match(csilk_router_t* r, const char* method, const char* path)
{
	if (!r || !r->root || !method || !path) {
		return nullptr;
	}
	return match_node(r->root, method, path, nullptr, nullptr);
}

/** @brief Match the current request context against the routing table and
 * populate path params.
 *
 * Sets the context's handlers array, handler index (reset to -1), and
 * current_handler metadata on a successful match. Path parameters are
 * captured into the context's params array.
 *
 * @param r The router instance.
 * @param c The request context (must have method and path set).
 * @return 1 if a matching route was found, 0 otherwise.
 * @note On success, the context is ready for csilk_next() to begin handler
 *       execution. On failure, the caller should send a 404 response. */
int
csilk_router_match_ctx(csilk_router_t* r, csilk_ctx_t* c)
{
	if (!r || !c || !r->root || !c->request.method || !c->request.path) {
		return 0;
	}
	c->params_count = 0;
	csilk_method_handler_t* mh = nullptr;
	csilk_handler_t* handlers = match_node(r->root, c->request.method, c->request.path, c, &mh);
	if (handlers) {
		c->handlers = handlers;
		c->handler_index = -1;
		c->current_handler = mh;
		return 1;
	}
	return 0;
}
