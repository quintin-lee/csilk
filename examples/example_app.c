/**
 * @file example_app.c
 * @brief Example using the high-level csilk_app_t API.
 *
 * Demonstrates csilk_app_new, reflection binding, nested structs,
 * arrays, route groups, SSE, gzip, and Swagger UI.
 * @copyright MIT License
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "csilk/app/app.h"
#include "csilk/csilk.h"
#include "csilk/reflection/reflect.h"

/* =================================================================
 *  Complex Reflection Structs
 *
 *  These structs demonstrate the reflection engine's ability to
 *  handle nested types, arrays, booleans, and mixed-size ints.
 * ================================================================= */

/** @brief Nested address type. */
typedef struct {
	char street[128];
	char city[64];
	char zip[16];
	bool is_primary;
} reflect_address_t;

/** @brief Complex user profile with nested structs, arrays, and mixed types. */
typedef struct {
	int64_t id;
	char name[64];
	char email[128];
	bool active;
	double score;
	float rating;
	int16_t level;
	uint8_t flags[4];
	reflect_address_t address;
	char* bio;
} reflect_user_t;

/** @brief Order item inside a nested array. */
typedef struct {
	char sku[32];
	int32_t quantity;
	double unit_price;
} reflect_order_item_t;

/** @brief Full order with nested items array via struct fields. */
typedef struct {
	int64_t order_id;
	char customer_email[128];
	double total;
	reflect_order_item_t items[3];
	char notes[256];
} reflect_order_t;

/* ---- Field descriptor macros ---- */

#define REFLECT_ADDRESS_MAP(_, ...)                                                                \
	_(reflect_address_t,                                                                       \
	  street,                                                                                  \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_address_t*)0)->street),                                                 \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)                                                                                 \
	_(reflect_address_t,                                                                       \
	  city,                                                                                    \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_address_t*)0)->city),                                                   \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)                                                                                 \
	_(reflect_address_t,                                                                       \
	  zip,                                                                                     \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_address_t*)0)->zip),                                                    \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)                                                                                 \
	_(reflect_address_t, is_primary, CSILK_TYPE_BOOL, 0, 0, false, nullptr)

#define REFLECT_USER_MAP(_, ...)                                                                   \
	_(reflect_user_t, id, CSILK_TYPE_INT64, 0, 0, false, nullptr)                              \
	_(reflect_user_t,                                                                          \
	  name,                                                                                    \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_user_t*)0)->name),                                                      \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)                                                                                 \
	_(reflect_user_t,                                                                          \
	  email,                                                                                   \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_user_t*)0)->email),                                                     \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)                                                                                 \
	_(reflect_user_t, active, CSILK_TYPE_BOOL, 0, 0, false, nullptr)                           \
	_(reflect_user_t, score, CSILK_TYPE_DOUBLE, 0, 0, false, nullptr)                          \
	_(reflect_user_t, rating, CSILK_TYPE_FLOAT, 0, 0, false, nullptr)                          \
	_(reflect_user_t, level, CSILK_TYPE_INT16, 0, 0, false, nullptr)                           \
	_(reflect_user_t, flags, CSILK_TYPE_UINT8, sizeof(uint8_t), 4, false, nullptr)             \
	_(reflect_user_t,                                                                          \
	  address,                                                                                 \
	  CSILK_TYPE_STRUCT,                                                                       \
	  sizeof(reflect_address_t),                                                               \
	  0,                                                                                       \
	  false,                                                                                   \
	  "reflect_address_t")                                                                     \
	_(reflect_user_t, bio, CSILK_TYPE_STRING, 0, 0, true, nullptr)

#define REFLECT_ORDER_ITEM_MAP(_, ...)                                                             \
	_(reflect_order_item_t,                                                                    \
	  sku,                                                                                     \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_order_item_t*)0)->sku),                                                 \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)                                                                                 \
	_(reflect_order_item_t, quantity, CSILK_TYPE_INT32, 0, 0, false, nullptr)                  \
	_(reflect_order_item_t, unit_price, CSILK_TYPE_DOUBLE, 0, 0, false, nullptr)

#define REFLECT_ORDER_MAP(_, ...)                                                                  \
	_(reflect_order_t, order_id, CSILK_TYPE_INT64, 0, 0, false, nullptr)                       \
	_(reflect_order_t,                                                                         \
	  customer_email,                                                                          \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_order_t*)0)->customer_email),                                           \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)                                                                                 \
	_(reflect_order_t, total, CSILK_TYPE_DOUBLE, 0, 0, false, nullptr)                         \
	_(reflect_order_t,                                                                         \
	  items,                                                                                   \
	  CSILK_TYPE_STRUCT,                                                                       \
	  sizeof(reflect_order_item_t),                                                            \
	  3,                                                                                       \
	  false,                                                                                   \
	  "reflect_order_item_t")                                                                  \
	_(reflect_order_t,                                                                         \
	  notes,                                                                                   \
	  CSILK_TYPE_STRING,                                                                       \
	  sizeof(((reflect_order_t*)0)->notes),                                                    \
	  0,                                                                                       \
	  false,                                                                                   \
	  nullptr)

/* Auto-register at startup */
CSILK_REGISTER_REFLECT(reflect_address_t, REFLECT_ADDRESS_MAP)
CSILK_REGISTER_REFLECT(reflect_user_t, REFLECT_USER_MAP)
CSILK_REGISTER_REFLECT(reflect_order_item_t, REFLECT_ORDER_ITEM_MAP)
CSILK_REGISTER_REFLECT(reflect_order_t, REFLECT_ORDER_MAP)

/* =================================================================
 *  Handlers
 * ================================================================= */

/** @brief Handler for GET / — "Hello from csilk easy API!" */
static void
hello(csilk_ctx_t* c)
{
	csilk_string(c, CSILK_STATUS_OK, "Hello from csilk easy API!");
}

/** @brief Handler for GET /user/:id — returns the user ID from path param. */
static void
user(csilk_ctx_t* c)
{
	const char* id = csilk_get_param(c, "id");
	char buf[64];
	snprintf(buf, sizeof(buf), "user id: %s", id ? id : "?");
	csilk_string(c, CSILK_STATUS_OK, buf);
}

/** @brief Handler for GET /ping — returns a JSON status response. */
static void
ping(csilk_ctx_t* c)
{
	cJSON* obj = cJSON_CreateObject();
	cJSON_AddStringToObject(obj, "status", "ok");
	cJSON_AddNumberToObject(obj, "time", (double)time(nullptr));
	csilk_json(c, CSILK_STATUS_OK, obj);
}

/** @brief Handler for POST /login — authenticates user and returns a token. */
static void
login(csilk_ctx_t* c)
{
	cJSON* in = csilk_bind_json(c);
	if (!in) {
		csilk_string(c, CSILK_STATUS_BAD_REQUEST, "bad json");
		return;
	}
	cJSON* u = cJSON_GetObjectItem(in, "user");
	if (cJSON_IsString(u) && !strcmp(u->valuestring, "admin")) {
		cJSON* out = cJSON_CreateObject();
		cJSON_AddStringToObject(out, "token", "demo-token");
		csilk_json(c, CSILK_STATUS_OK, out);
	} else {
		csilk_string(c, CSILK_STATUS_UNAUTHORIZED, "unauthorized");
	}
	cJSON_Delete(in);
}

/** @brief Handler for GET /echo?msg=... — echoes back the query parameter. */
static void
echo(csilk_ctx_t* c)
{
	const char* q = csilk_get_query(c, "msg");
	char buf[256];
	snprintf(buf, sizeof(buf), "echo: %s", q ? q : "(nothing)");
	csilk_string(c, CSILK_STATUS_OK, buf);
}

/* ---- Reflection-based handlers ---- */

/** @brief Handler for POST /api/users — creates a user via reflection binding.
 *  Accepts JSON body with nested address, arrays, booleans, etc. */
static void
create_user(csilk_ctx_t* c)
{
	reflect_user_t u;
	memset(&u, 0, sizeof(u));
	u.bio = nullptr;

	if (!csilk_bind_reflect(c, "reflect_user_t", &u)) {
		csilk_json_error(c, CSILK_STATUS_BAD_REQUEST, "Invalid user JSON");
		return;
	}

	u.id = (int64_t)(time(nullptr) % 100000);
	csilk_set_header(c, "Content-Type", "application/json");

	if (u.bio) {
		char buf[320];
		snprintf(buf,
			 sizeof(buf),
			 "{\"received\":true,\"id\":%lld,\"name\":\"%s\","
			 "\"email\":\"%s\",\"active\":%s,\"score\":%.2f,"
			 "\"rating\":%.1f,\"level\":%d,\"bio\":\"%s\"}",
			 (long long)u.id,
			 u.name,
			 u.email,
			 u.active ? "true" : "false",
			 u.score,
			 u.rating,
			 u.level,
			 u.bio);
		free(u.bio);
		csilk_string(c, CSILK_STATUS_OK, buf);
	} else {
		char buf[512];
		snprintf(buf,
			 sizeof(buf),
			 "{\"received\":true,\"id\":%lld,\"name\":\"%s\","
			 "\"email\":\"%s\",\"active\":%s,\"score\":%.2f,"
			 "\"rating\":%.1f,\"level\":%d}",
			 (long long)u.id,
			 u.name,
			 u.email,
			 u.active ? "true" : "false",
			 u.score,
			 u.rating,
			 u.level);
		csilk_string(c, CSILK_STATUS_OK, buf);
	}
}

/** @brief Handler for GET /api/users/:id — returns a user profile via
 * reflection. */
static void
get_user(csilk_ctx_t* c)
{
	reflect_user_t u;
	memset(&u, 0, sizeof(u));

	const char* id_str = csilk_get_param(c, "id");
	u.id = id_str ? atol(id_str) : 0;
	snprintf(u.name, sizeof(u.name), "User_%lld", (long long)u.id);
	snprintf(u.email, sizeof(u.email), "user%lld@example.com", (long long)u.id);
	u.active = (u.id % 2) == 0;
	u.score = u.id * 1.5;
	u.rating = 4.5f;
	u.level = (int16_t)(u.id % 100);
	u.flags[0] = 1;
	u.flags[1] = 2;
	u.flags[2] = 3;
	u.flags[3] = 4;
	snprintf(u.address.street, sizeof(u.address.street), "%lld Main St", (long long)u.id);
	snprintf(u.address.city, sizeof(u.address.city), "City_%lld", (long long)u.id);
	snprintf(u.address.zip, sizeof(u.address.zip), "%05lld", (long long)(u.id % 99999));
	u.address.is_primary = true;
	u.bio = strdup("Reflection demo bio text");

	csilk_json_reflect(c, CSILK_STATUS_OK, "reflect_user_t", &u);
	free(u.bio);
}

/** @brief Handler for POST /api/orders — creates an order via reflection
 * binding. */
static void
create_order(csilk_ctx_t* c)
{
	reflect_order_t o;
	memset(&o, 0, sizeof(o));

	if (!csilk_bind_reflect(c, "reflect_order_t", &o)) {
		csilk_json_error(c, CSILK_STATUS_BAD_REQUEST, "Invalid order JSON");
		return;
	}

	o.order_id = (int64_t)(time(nullptr) % 100000);

	csilk_json_reflect(c, CSILK_STATUS_OK, "reflect_order_t", &o);
}

/** @brief Handler for GET /api/orders/:id — returns a mock order via
 * reflection. */
static void
get_order(csilk_ctx_t* c)
{
	reflect_order_t o;
	memset(&o, 0, sizeof(o));
	const char* id_str = csilk_get_param(c, "id");
	o.order_id = id_str ? atol(id_str) : 0;
	snprintf(o.customer_email,
		 sizeof(o.customer_email),
		 "customer%lld@example.com",
		 (long long)o.order_id);
	o.total = 299.99;

	snprintf(o.items[0].sku, sizeof(o.items[0].sku), "SKU-A");
	o.items[0].quantity = 2;
	o.items[0].unit_price = 49.99;

	snprintf(o.items[1].sku, sizeof(o.items[1].sku), "SKU-B");
	o.items[1].quantity = 1;
	o.items[1].unit_price = 199.99;

	snprintf(o.items[2].sku, sizeof(o.items[2].sku), "SKU-C");
	o.items[2].quantity = 3;
	o.items[2].unit_price = 0.00;

	snprintf(o.notes, sizeof(o.notes), "Order placed via reflection API");

	csilk_json_reflect(c, CSILK_STATUS_OK, "reflect_order_t", &o);
}

/* ---- SSE demo ---- */

/** @brief Handler for GET /stream — SSE connection demo. */
static void
sse_handler(csilk_ctx_t* c)
{
	csilk_sse_init(c);
	csilk_sse_send(c, nullptr, "connected");
	csilk_sse_send(c, "greeting", "{\"hello\":\"world\"}");
	csilk_sse_close(c);
}

/* ---- custom middleware ---- */

/** @brief Custom middleware — measures and logs request processing time. */
static void
timer_mw(csilk_ctx_t* c)
{
	clock_t t0 = clock();
	csilk_next(c);
	printf("  req %s %s → %d  (%.2f ms)\n",
	       csilk_get_method(c),
	       csilk_get_path(c),
	       csilk_get_status(c),
	       1000.0 * (clock() - t0) / CLOCKS_PER_SEC);
}

/* ================================================================= */

int
main(void)
{
	csilk_app_t* app = csilk_app_new(nullptr);

	csilk_app_log_level(app, CSILK_LOG_DEBUG);

	/* enable JSON structured logging (comment out for plain text) */
	/* csilk_app_log_json(app, 1); */

	/* global middleware */
	csilk_app_use(app, timer_mw);

	/* simple routes */
	csilk_app_get(app, "/", hello);
	csilk_app_get(app, "/user/:id", user);
	csilk_app_get(app, "/ping", ping);
	csilk_app_post(app, "/login", login);
	csilk_app_get(app, "/echo", echo);
	csilk_app_get(app, "/stream", sse_handler);

	/* reflection-based routes with OpenAPI metadata.
   * Schemas auto-register in components/schemas; the _ext macros
   * also link each route to its request/response types. */
	csilk_app_post_ext(app,
			   "/api/users",
			   create_user,
			   "reflect_user_t",
			   "reflect_user_t",
			   "Create user",
			   "Creates a user with nested address, flags array, and bio");
	csilk_app_get_ext(app,
			  "/api/users/:id",
			  get_user,
			  nullptr,
			  "reflect_user_t",
			  "Get user profile",
			  "Returns a user profile with nested address and array fields");
	csilk_app_post_ext(app,
			   "/api/orders",
			   create_order,
			   "reflect_order_t",
			   "reflect_order_t",
			   "Create order",
			   "Creates an order with line items, customer email, and notes");
	csilk_app_get_ext(app,
			  "/api/orders/:id",
			  get_order,
			  nullptr,
			  "reflect_order_t",
			  "Get order",
			  "Returns an order with items array, totals, and notes");

	/* group-level middleware */
	csilk_app_use_group(app, "/api", csilk_gzip_middleware);

	printf("\n"
	       "  Reflection API endpoints:\n"
	       "    POST /api/users            Create user (nested address, "
	       "flags[])\n"
	       "    GET  /api/users/:id        Get user profile (reflection "
	       "marshal)\n"
	       "    POST /api/orders           Create order (items[3], nested "
	       "structs)\n"
	       "    GET  /api/orders/:id       Get order (reflection marshal)\n"
	       "\n"
	       "  Simple endpoints:\n"
	       "    GET  /                    hello\n"
	       "    GET  /user/:id            path params\n"
	       "    GET  /ping                json\n"
	       "    POST /login               {\"user\":\"admin\"}\n"
	       "    GET  /echo?msg=...        query params\n"
	       "    GET  /stream              SSE demo\n"
	       "    GET  /api/ping            gzip-compressed json\n"
	       "    GET  /openapi.json        auto-generated OpenAPI/Swagger "
	       "spec\n"
	       "    GET  /docs                interactive Swagger UI\n"
	       "\n");

	int rc = csilk_app_run(app, 8080);
	csilk_app_free(app);
	return rc;
}
