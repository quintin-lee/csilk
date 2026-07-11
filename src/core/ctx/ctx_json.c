/**
 * @file ctx_json.c
 * @brief JSON binding, cookie parsing, and reflection helpers for csilk context.
 *
 * @copyright MIT License
 */

#include <string.h>

#include "cJSON.h"
#include "ctx_internal.h"
#include "csilk/core/internal.h"
#include "csilk/reflection/reflect.h"

/** @brief Parse the request body as JSON using cJSON.
 *
 * @param c The request context.
 * @return A cJSON object parsed from the request body, or nullptr if the body
 *         is nullptr or the JSON is invalid.
 * @note The caller owns the returned cJSON object and must free it with
 *       cJSON_Delete(). For error details use csilk_bind_json_err(). */
cJSON*
csilk_bind_json(csilk_ctx_t* c)
{
    if (!c || !c->request.body) {
        return nullptr;
    }
    return cJSON_Parse(c->request.body);
}

/** @brief Parse the request body as JSON with detailed error feedback.
 *
 * Like csilk_bind_json() but sets @p error to a descriptive string on
 * failure (e.g., "Null context", "No request body", or the cJSON parse
 * error position).
 *
 * @param c     The request context.
 * @param error [out] Optional pointer to receive a static error string.
 * @return A cJSON object parsed from the request body, or nullptr on failure.
 * @note The caller owns the returned cJSON object and must free it.
 *       The @p error string is a static pointer (do not free). */
cJSON*
csilk_bind_json_err(csilk_ctx_t* c, const char** error)
{
    if (error) {
        *error = nullptr;
    }
    if (!c) {
        if (error) {
            *error = "Null context";
        }
        return nullptr;
    }
    if (!c->request.body) {
        if (error) {
            *error = "No request body";
        }
        return nullptr;
    }
    cJSON* json = cJSON_Parse(c->request.body);
    if (!json) {
        if (error) {
            *error = cJSON_GetErrorPtr();
        }
        if (error && !*error) {
            *error = "Invalid JSON";
        }
        return nullptr;
    }
    return json;
}

/** @brief Get a cookie value by its name from the Cookie header.
 *
 * Parses the "Cookie" request header by splitting on "; " and then on "=".
 * Returns the value for the first cookie matching @p name.
 *
 * @param c    The request context.
 * @param name Cookie name to look up.
 * @return The cookie value string (arena-allocated), or nullptr if the cookie
 *         is not found, the header is absent, or the context/arena is nullptr.
 * @note The returned value is URL-decoded only as much as the raw header
 *       contains. Cookie attributes (path, domain, etc.) are not supported. */
const char*
csilk_get_cookie(csilk_ctx_t* c, const char* name)
{
    if (!c || !name || !c->arena) {
        return nullptr;
    }
    const char* cookie_header = csilk_get_header(c, "Cookie");
    if (!cookie_header) {
        return nullptr;
    }

    char* cookies = csilk_arena_strdup(c->arena, cookie_header);
    if (!cookies) {
        return nullptr;
    }

    char* saveptr;
    char* cookie = strtok_r(cookies, "; ", &saveptr);

    while (cookie) {
        char* eq = strchr(cookie, '=');
        if (eq) {
            *eq = '\0';
            if (strcmp(cookie, name) == 0) {
                return csilk_arena_strdup(c->arena, eq + 1);
            }
        }
        cookie = strtok_r(nullptr, "; ", &saveptr);
    }

    return nullptr;
}

/** @brief Bind the request body JSON to a registered struct via reflection.
 *
 * Deserializes the JSON request body into the provided struct pointer using
 * the csilk reflection engine. If @p type_name is nullptr, the type is inferred
 * from the current handler's input_type metadata (if available).
 *
 * @param c         The request context.
 * @param type_name Registered type name (e.g., "my_request_t"), or nullptr to
 *                  infer from the route handler's metadata.
 * @param ptr       Pointer to the target struct to populate.
 * @return 1 on success, 0 on failure (nullptr context, no body, type not found,
 *         or JSON parse error).
 * @note Uses csilk_json_unmarshal() internally. The struct should have been
 *       registered via CSILK_REGISTER_REFLECT(). */
int
csilk_bind_reflect(csilk_ctx_t* c, const char* type_name, void* ptr)
{
    if (!c || !c->request.body || !ptr) {
        CSILK_LOG_D("Context: csilk_bind_reflect failed: null context, body, or target pointer");
        return 0;
    }
    if (!type_name && c->current_handler) {
        type_name = c->current_handler->input_type;
    }
    if (!type_name) {
        CSILK_LOG_D("Context: csilk_bind_reflect failed: no type name specified or inferred");
        return 0;
    }
    int ret = csilk_json_unmarshal(type_name, c->request.body, ptr);
    if (!ret) {
        CSILK_LOG_W("Context: failed to unmarshal request body JSON to type '%s'", type_name);
    } else {
        CSILK_LOG_D("Context: successfully bound request body JSON to type '%s'", type_name);
    }
    return ret;
}
