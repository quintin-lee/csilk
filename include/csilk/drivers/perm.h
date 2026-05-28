/**
 * @file perm.h
 * @brief Pluggable permission/access-control (ACL) driver interface.
 *
 * Provides an abstraction layer for role-based access control (RBAC) or
 * relationship-based access control (ReBAC).  Routes can declare required
 * permissions and resources via route-registration metadata (e.g.,
 * csilk_router_add_perm).  A pluggable driver evaluates whether the
 * authenticated user (identified in the request context) has the required
 * permission on the target resource.
 *
 * Built-in: csilk_perm_simple_* provides an in-memory RBAC implementation.
 *
 * @copyright MIT License
 */

#ifndef CSILK_PERM_H
#define CSILK_PERM_H

/** @brief Forward declaration of the request context (defined in csilk.h). */
typedef struct csilk_ctx_s csilk_ctx_t;

/**
 * @brief Virtual function table for a permission/ACL driver.
 *
 * Implementations evaluate whether a given user (identified through the
 * request context) holds a specific permission on a resource.
 *
 * @note The check function is called synchronously on the event-loop thread.
 *       For database-backed drivers, use a fast cache or async offload.
 */
struct csilk_perm_driver_s {
	const char* name; /**< Driver identifier (e.g., "simple", "casbin"). */
	/** @brief Evaluate whether the request is allowed.
   *  @param c          Request context (used to identify the authenticated
   * user).
   *  @param permission Permission string (e.g., "read", "write", "delete").
   *  @param resource   Resource pattern (e.g., "users:*", "documents:42").
   *  @return 1 if allowed, 0 if denied, -1 on error. */
	int (*check)(csilk_ctx_t* c, const char* permission, const char* resource);
};

typedef struct csilk_perm_driver_s csilk_perm_driver_t;

/**
 * @brief A single permission rule for the built-in RBAC driver.
 *
 * Associates a role with a permission on a resource pattern.
 * Rules are managed via csilk_perm_simple_allow / csilk_perm_simple_clear.
 */
typedef struct {
	const char* role;	/**< Role identifier (e.g., "admin", "editor"). */
	const char* permission; /**< Action/permission string (e.g., "read"). */
	const char* resource;	/**< Resource pattern (e.g., "articles:*"). */
} csilk_perm_rule_t;

/** @brief Initialise the permission subsystem.
 *  Safe to call multiple times.  Must be called before any driver operations.
 */
void csilk_perm_init(void);

/** @brief Register a permission driver implementation.
 *  @param name   Unique driver name (must not already be registered).
 *  @param driver Pointer to driver vtable (must remain valid for program
 * lifetime).
 *  @return 0 on success, -1 if @p name is already registered. */
int csilk_perm_register_driver(const char* name, csilk_perm_driver_t* driver);

/** @brief Look up a registered driver by name.
 *  @param name Driver identifier string.
 *  @return The registered driver vtable, or NULL if not found. */
csilk_perm_driver_t* csilk_perm_get_driver(const char* name);

/** @brief Set the default permission driver used by csilk_perm_check.
 *  @param name Driver identifier.
 *  @return 0 on success, -1 if @p name is not registered. */
int csilk_perm_set_default(const char* name);

/** @brief Check the current request against the default permission driver.
 *  @param c          Request context.
 *  @param permission Permission to check.
 *  @param resource   Resource to check.
 *  @return 1 if allowed, 0 if denied, -1 on error (or no driver set). */
int csilk_perm_check(csilk_ctx_t* c, const char* permission, const char* resource);

/** @brief Abort the handler chain with 403 Forbidden if the check fails.
 *  Convenience wrapper: calls csilk_perm_check and csilk_abort on denial.
 *  @param c          Request context.
 *  @param permission Permission to check.
 *  @param resource   Resource to check. */
void csilk_perm_require(csilk_ctx_t* c, const char* permission, const char* resource);

/** @brief Initialise the built-in in-memory RBAC driver.
 *  Registers as "simple".  Must be called before any simple_* functions. */
void csilk_perm_simple_init(void);

/** @brief Grant a permission on a resource to a role.
 *  @param role       Role name (e.g., "admin").
 *  @param permission Permission string (e.g., "write").
 *  @param resource   Resource pattern (e.g., "articles:*").
 *  @return 1 if the rule was added, 0 if it already existed. */
int csilk_perm_simple_allow(const char* role, const char* permission, const char* resource);

/** @brief Remove all rules from the simple driver.
 *  After calling this, all checks will deny until new rules are added. */
void csilk_perm_simple_clear(void);

/** @brief Automatic permission-check middleware.
 *  Looks up the permission and resource from the route's metadata
 *  (registered via csilk_router_add_perm) and checks them.  Aborts with
 *  403 if the check fails.  Safe to call even if the route has no
 *  permission metadata (passes through). */
void csilk_perm_auto_middleware(csilk_ctx_t* c);

#endif
