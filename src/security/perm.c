/**
 * @file perm.c
 * @brief Permission / authorization subsystem.
 *
 * Architecture: Facade over pluggable permission driver backends.
 * A global registry holds up to 16 drivers, with a single default
 * driver selected for authorization checks. The built-in "simple"
 * driver is installed on first call to csilk_perm_init().
 *
 * The auto middleware (csilk_perm_auto_middleware) is designed to
 * be registered as a global middleware — it reads the route's
 * permission metadata from the request context and invokes the
 * driver's check() function, aborting the request with 403 on
 * failure.
 *
 * @copyright MIT License
 */

#include "csilk/drivers/perm.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "csilk/csilk.h"

/** @brief Global driver registry (fixed-size array, max 16). */
static csilk_perm_driver_t* drivers[16];
static int driver_count = 0;
/** @brief Currently active default driver for authorization checks. */
static csilk_perm_driver_t* default_driver = nullptr;
static atomic_int perm_initialized = 0;

/** @brief Initialize the permission subsystem.
 *
 * Installs the built-in "simple" permission driver on first call.
 * Idempotent via atomic CAS — subsequent calls are no-ops.
 * @note Must be called before any authorization checks. */
void
csilk_perm_init(void)
{
	int expected = 0;
	if (atomic_compare_exchange_strong(&perm_initialized, &expected, 1)) {
		csilk_perm_simple_init();
	}
}

/** @brief Register a permission driver in the global registry.
 *
 * The first registered driver automatically becomes the default.
 * @param name   Driver name (e.g., "simple", "rbac").
 * @param driver Driver vtable with check() callback.
 * @return 0 on success, -1 if name is nullptr, driver is nullptr,
 *         or the registry is full. */
int
csilk_perm_register_driver(const char* name, csilk_perm_driver_t* driver)
{
	if (!name || !driver || driver_count >= 16) {
		return -1;
	}

	driver->name = name;
	drivers[driver_count++] = driver;
	if (!default_driver) {
		default_driver = driver;
	}
	return 0;
}

/** @brief Look up a registered permission driver by name.
 *
 * Linear search of the driver registry.
 * @param name Driver name to find (case-sensitive).
 * @return Driver pointer, or nullptr if not found. */
csilk_perm_driver_t*
csilk_perm_get_driver(const char* name)
{
	if (!name) {
		return nullptr;
	}
	for (int i = 0; i < driver_count; i++) {
		if (strcmp(drivers[i]->name, name) == 0) {
			return drivers[i];
		}
	}
	return nullptr;
}

/** @brief Set the default permission driver by name.
 *
 * @param name Driver name (must be already registered).
 * @return 0 on success, -1 if the driver is not found. */
int
csilk_perm_set_default(const char* name)
{
	csilk_perm_driver_t* d = csilk_perm_get_driver(name);
	if (!d) {
		return -1;
	}
	default_driver = d;
	return 0;
}

/** @brief Check a permission against the default driver.
 *
 * Delegates to the default driver's check() callback.
 * @param c          The request context.
 * @param permission Permission identifier (e.g., "read", "write").
 * @param resource   Resource pattern (e.g., "users:*").
 * @return 0 if allowed, non-zero if denied or no driver is set. */
int
csilk_perm_check(csilk_ctx_t* c, const char* permission, const char* resource)
{
	if (!default_driver || !default_driver->check) {
		return -1;
	}
	return default_driver->check(c, permission, resource);
}

/** @brief Require a permission and abort the request if denied.
 *
 * Calls csilk_perm_check() and sends a 403 Forbidden JSON response
 * followed by csilk_abort() if the check fails.
 * @param c          The request context.
 * @param permission Permission to require.
 * @param resource   Resource to check against. */
void
csilk_perm_require(csilk_ctx_t* c, const char* permission, const char* resource)
{
	if (csilk_perm_check(c, permission, resource) != 0) {
		csilk_string(c, CSILK_STATUS_FORBIDDEN, "{\"error\":\"Forbidden\"}");
		csilk_abort(c);
	}
}

/** @brief Global middleware that auto-checks route-level permissions.
 *
 * Reads perm_required and perm_resource from the current handler
 * metadata (set via csilk_app_add_route_extended_perm or the
 * *_perm variants). If the route has a permission requirement,
 * enforces it via csilk_perm_require().
 *
 * @param c The request context.
 * @note This middleware is designed to be registered with
 *       csilk_app_use() or csilk_server_use(). It is a no-op
 *       for routes without permission metadata. */
void
csilk_perm_auto_middleware(csilk_ctx_t* c)
{
	const char* perm = csilk_ctx_get_handler_perm_required(c);
	if (!perm) {
		return;
	}
	csilk_perm_require(c, perm, csilk_ctx_get_handler_perm_resource(c));
}
