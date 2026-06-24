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

#if defined(__x86_64__)
#include <cpuid.h>
#include <immintrin.h>
#endif

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "core/ctx_internal.h"
#include "core/srv_internal.h"
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
		CSILK_LOG_E("Router: failed to allocate memory for router node");
		return nullptr;
	}
	node->segment = strdup(segment);
	if (!node->segment) {
		CSILK_LOG_E("Router: failed to duplicate segment string '%s'", segment);
		free(node);
		return nullptr;
	}
	node->segment_len = strlen(node->segment);
	node->type = type;
	CSILK_LOG_T("Router: allocated new node (segment: '%s', type: %d)", segment, type);
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
	CSILK_LOG_T("Router: freeing node (segment: '%s', type: %d)", node->segment, node->type);
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
#if defined(CSILK_HAS_AVX512)
__attribute__((target("avx512f,avx512bw"))) static inline const char*
get_next_segment_avx512(const char** p, size_t* len)
{
	while (**p == '/') {
		(*p)++;
	}
	if (**p == '\0') {
		return nullptr;
	}

	const char* start = *p;
	const char* curr = *p;

	__m512i slash_vec = _mm512_set1_epi8('/');
	__m512i zero_vec = _mm512_setzero_si512();

	while (1) {
		uintptr_t addr = (uintptr_t)curr;
		if ((addr & 4095) <= 4096 - 64) {
			__m512i data = _mm512_loadu_si512((const __m512i*)curr);
			__mmask64 cmp_slash = _mm512_cmpeq_epi8_mask(data, slash_vec);
			__mmask64 cmp_zero = _mm512_cmpeq_epi8_mask(data, zero_vec);
			__mmask64 cmp_combined = cmp_slash | cmp_zero;
			if (cmp_combined != 0) {
				int idx = __builtin_ctzll(cmp_combined);
				curr += idx;
				break;
			}
			curr += 64;
		} else {
			if (*curr == '/' || *curr == '\0') {
				break;
			}
			curr++;
		}
	}

	*p = curr;
	*len = (size_t)(curr - start);
	return start;
}
#endif

#if defined(__x86_64__)
__attribute__((target("avx2"))) static inline const char*
get_next_segment_avx2(const char** p, size_t* len)
{
	while (**p == '/') {
		(*p)++;
	}
	if (**p == '\0') {
		return nullptr;
	}

	const char* start = *p;
	const char* curr = *p;

	__m256i slash_vec = _mm256_set1_epi8('/');
	__m256i zero_vec = _mm256_setzero_si256();

	while (1) {
		uintptr_t addr = (uintptr_t)curr;
		if ((addr & 4095) <= 4096 - 32) {
			__m256i data = _mm256_loadu_si256((const __m256i*)curr);
			__m256i cmp_slash = _mm256_cmpeq_epi8(data, slash_vec);
			__m256i cmp_zero = _mm256_cmpeq_epi8(data, zero_vec);
			__m256i cmp_combined = _mm256_or_si256(cmp_slash, cmp_zero);
			int mask = _mm256_movemask_epi8(cmp_combined);
			if (mask != 0) {
				int idx = __builtin_ctz(mask);
				curr += idx;
				break;
			}
			curr += 32;
		} else {
			if (*curr == '/' || *curr == '\0') {
				break;
			}
			curr++;
		}
	}

	*p = curr;
	*len = (size_t)(curr - start);
	return start;
}
#endif

#if defined(__ARM_NEON)
static inline const char*
get_next_segment_neon(const char** p, size_t* len)
{
	while (**p == '/') {
		(*p)++;
	}
	if (**p == '\0') {
		return nullptr;
	}

	const char* start = *p;
	const char* curr = *p;

	uint8x16_t slash_vec = vdupq_n_u8('/');
	uint8x16_t zero_vec = vdupq_n_u8('\0');

	while (1) {
		uintptr_t addr = (uintptr_t)curr;
		if ((addr & 4095) <= 4096 - 16) {
			uint8x16_t data = vld1q_u8((const uint8_t*)curr);
			uint8x16_t cmp_slash = vceqq_u8(data, slash_vec);
			uint8x16_t cmp_zero = vceqq_u8(data, zero_vec);
			uint8x16_t cmp_combined = vorrq_u8(cmp_slash, cmp_zero);

			uint64_t mask_low = vgetq_lane_u64(vreinterpretq_u64_u8(cmp_combined), 0);
			uint64_t mask_high = vgetq_lane_u64(vreinterpretq_u64_u8(cmp_combined), 1);

			if (mask_low != 0 || mask_high != 0) {
				if (mask_low != 0) {
					int idx = __builtin_ctzll(mask_low) / 8;
					curr += idx;
				} else {
					int idx = __builtin_ctzll(mask_high) / 8;
					curr += 8 + idx;
				}
				break;
			}
			curr += 16;
		} else {
			if (*curr == '/' || *curr == '\0') {
				break;
			}
			curr++;
		}
	}

	*p = curr;
	*len = (size_t)(curr - start);
	return start;
}
#endif

static const char*
get_next_segment(const char** p, size_t* len)
{
#if defined(CSILK_HAS_AVX512)
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
		return get_next_segment_avx512(p, len);
	}
#endif
#if defined(__x86_64__)
	if (__builtin_cpu_supports("avx2")) {
		return get_next_segment_avx2(p, len);
	}
#elif defined(__ARM_NEON)
	return get_next_segment_neon(p, len);
#endif

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
			CSILK_LOG_E("Router: failed to allocate memory for segment name '%.*s'",
				    (int)seg_name_len,
				    seg_name_start);
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
					CSILK_LOG_D("Router: inserted new node '%s' (type: %d) at "
						    "index %d under node '%s'",
						    seg_name,
						    type,
						    insert_pos,
						    curr->segment[0] ? curr->segment : "/");
				} else {
					CSILK_LOG_E(
					    "Router: failed to allocate new route node for segment "
					    "'%s' in path '%s'",
					    seg_name,
					    path);
				}
			} else {
				CSILK_LOG_E(
				    "Router: failed to insert route segment '%s' in path '%s': "
				    "maximum node children limit (%d) exceeded",
				    seg_name,
				    path,
				    CSILK_MAX_CHILDREN);
			}
		} else {
			CSILK_LOG_T("Router: matched existing node '%s' (type: %d) under node '%s'",
				    seg_name,
				    type,
				    curr->segment[0] ? curr->segment : "/");
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
			CSILK_LOG_T("Router: stopping segment processing at wildcard node '%s'",
				    curr->segment);
			break;
		}
	}

	csilk_method_handler_t* mh = curr->handlers;
	while (mh) {
		if (strcmp(mh->method, method) == 0) {
			CSILK_LOG_W(
			    "Router: duplicate route registration ignored: %s %s", method, path);
			return;
		}
		mh = mh->next;
	}

	mh = malloc(sizeof(csilk_method_handler_t));
	if (mh) {
		mh->method = strdup(method);
		if (!mh->method) {
			CSILK_LOG_E("Router: failed to duplicate method string for route: %s %s",
				    method,
				    path);
			free(mh);
			return;
		}
		mh->handlers = malloc(sizeof(csilk_handler_t) * (handler_count + 1));
		if (!mh->handlers) {
			CSILK_LOG_E("Router: failed to allocate handler array for route: %s %s",
				    method,
				    path);
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
		CSILK_LOG_D("Router: route successfully registered: %s %s", method, path);
	} else {
		CSILK_LOG_E(
		    "Router: failed to allocate method handler for route: %s %s", method, path);
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

#if defined(__x86_64__)
#if defined(CSILK_HAS_AVX512)
__attribute__((target("avx512f,avx512bw"))) static inline int
csilk_memcmp_avx512(const char* s1, const char* s2, size_t n)
{
	if (n >= 64) {
		__m512i v1 = _mm512_loadu_si512((const __m512i*)s1);
		__m512i v2 = _mm512_loadu_si512((const __m512i*)s2);
		__mmask64 cmp = _mm512_cmpeq_epi8_mask(v1, v2);
		if (cmp != 0xFFFFFFFFFFFFFFFFULL) {
			return 0;
		}
		if (n == 64) {
			return 1;
		}
		return memcmp(s1 + 64, s2 + 64, n - 64) == 0;
	}
	return memcmp(s1, s2, n) == 0;
}
#endif

__attribute__((target("avx2"))) static inline int
csilk_memcmp_avx2(const char* s1, const char* s2, size_t n)
{
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
	return memcmp(s1, s2, n) == 0;
}
#endif

#if defined(__ARM_NEON)
static inline int
csilk_memcmp_neon(const char* s1, const char* s2, size_t n)
{
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
	return memcmp(s1, s2, n) == 0;
}
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
#if defined(CSILK_HAS_AVX512)
	if (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512bw")) {
		return csilk_memcmp_avx512(s1, s2, n);
	}
#endif
#if defined(__x86_64__)
	if (__builtin_cpu_supports("avx2")) {
		return csilk_memcmp_avx2(s1, s2, n);
	}
#elif defined(__ARM_NEON)
	return csilk_memcmp_neon(s1, s2, n);
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
	int use_simd = (ctx && ctx->server) ? ctx->server->config.enable_simd : 1;

	CSILK_LOG_T("Router: matching node '%s' (type: %d) with remaining path '%s'",
		    node->segment[0] ? node->segment : "/",
		    node->type,
		    path ? path : "empty");

	if (!path || *path == '\0' || (path[0] == '/' && path[1] == '\0')) {
		CSILK_LOG_T("Router: reached leaf/terminal match at node '%s'",
			    node->segment[0] ? node->segment : "/");
		csilk_method_handler_t* mh = node->handlers;
		while (mh) {
			if (strcmp(mh->method, method) == 0) {
				if (out_mh) {
					*out_mh = mh;
				}
				CSILK_LOG_T("Router: matched handler for method '%s' at node '%s'",
					    method,
					    node->segment[0] ? node->segment : "/");
				return mh->handlers;
			}
			mh = mh->next;
		}
		CSILK_LOG_T("Router: no handler for method '%s' at node '%s'",
			    method,
			    node->segment[0] ? node->segment : "/");
		return nullptr;
	}

	const char* p = path;
	size_t len;
	const char* seg = get_next_segment(&p, &len);
	if (!seg) {
		CSILK_LOG_T("Router: no more segments found in path '%s'", path);
		return nullptr;
	}

	CSILK_LOG_T("Router: testing segment '%.*s' against %d children of node '%s'",
		    (int)len,
		    seg,
		    node->children_count,
		    node->segment[0] ? node->segment : "/");

	csilk_handler_t* result = nullptr;
	for (int i = 0; i < node->children_count; i++) {
		csilk_router_node_t* child = node->children[i];
		if (child->type == CSILK_NODE_STATIC) {
			// Fast-path: check length and first character before full comparison
			if (child->segment_len == len && child->segment[0] == seg[0]) {
				int match = 0;
				if (use_simd) {
					match = csilk_memcmp_fast(child->segment, seg, len);
				} else {
					match = (strncmp(child->segment, seg, len) == 0);
				}

				if (match) {
					CSILK_LOG_T("Router: STATIC child '%s' matches segment "
						    "'%.*s', recursing",
						    child->segment,
						    (int)len,
						    seg);
					result = match_node(child, method, p, ctx, out_mh);
					if (result) {
						break;
					}
					CSILK_LOG_T("Router: backtrack - match failed deeper for "
						    "STATIC child '%s'",
						    child->segment);
				}
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
					CSILK_LOG_T("Router: PARAM child '%s' matched segment "
						    "'%.*s', captured parameter",
						    child->segment,
						    (int)len,
						    seg);
				} else {
					// Cleanup on allocation failure (only if not using arena)
					if (!ctx->arena) {
						free(ctx->params[ctx->params_count].key);
						free(ctx->params[ctx->params_count].value);
					}
					CSILK_LOG_E("Router: failed to allocate path parameter "
						    "memory for key '%s'",
						    child->segment);
				}
			} else if (ctx) {
				CSILK_LOG_E("Router: path parameter limit (%d) exceeded while "
					    "parsing key '%s'",
					    CSILK_MAX_PARAMS,
					    child->segment);
			}
			// Recurse deeper with the remaining path
			result = match_node(child, method, p, ctx, out_mh);
			// Backtracking rollback
			if (!result && param_added) {
				CSILK_LOG_T("Router: backtrack - match failed deeper for PARAM "
					    "child '%s', rolling back parameter",
					    child->segment);
				ctx->params_count--;
				if (!ctx->arena) {
					free(ctx->params[ctx->params_count].key);
					free(ctx->params[ctx->params_count].value);
				}
			} else if (result) {
				break;
			}
		} else if (child->type == CSILK_NODE_WILDCARD) {
			// WILDCARD node: matches the entire remainder of the URL path
			CSILK_LOG_T("Router: WILDCARD child '%s' matches remaining path '%s'",
				    child->segment,
				    path);
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
					CSILK_LOG_T(
					    "Router: captured wildcard parameter '%s' = '%s'",
					    child->segment,
					    val_start);
				} else {
					if (!ctx->arena) {
						free(ctx->params[ctx->params_count].key);
						free(ctx->params[ctx->params_count].value);
					}
					CSILK_LOG_E(
					    "Router: failed to allocate wildcard path parameter "
					    "memory for key '%s'",
					    child->segment);
				}
			} else if (ctx) {
				CSILK_LOG_E(
				    "Router: path parameter limit (%d) exceeded while parsing "
				    "wildcard key '%s'",
				    CSILK_MAX_PARAMS,
				    child->segment);
			}
			// Directly check the wildcard node's handlers
			csilk_method_handler_t* mh = child->handlers;
			while (mh) {
				if (strcmp(mh->method, method) == 0) {
					if (out_mh) {
						*out_mh = mh;
					}
					result = mh->handlers;
					CSILK_LOG_T("Router: matched handler for method '%s' at "
						    "wildcard node '%s'",
						    method,
						    child->segment);
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
		CSILK_LOG_W("Invalid match parameters: router=%p, ctx=%p", (void*)r, (void*)c);
		return 0;
	}
	c->params_count = 0;
	csilk_method_handler_t* mh = nullptr;
	CSILK_LOG_T(
	    "Attempting to match route for request: %s %s", c->request.method, c->request.path);
	csilk_handler_t* handlers = match_node(r->root, c->request.method, c->request.path, c, &mh);
	if (handlers) {
		c->handlers = handlers;
		c->handler_index = -1;
		c->current_handler = mh;
		CSILK_LOG_D("Route successfully matched: %s %s (pattern: %s)",
			    c->request.method,
			    c->request.path,
			    mh->path ? mh->path : "unknown");
		return 1;
	}
	CSILK_LOG_D("Route not matched: %s %s", c->request.method, c->request.path);
	return 0;
}
