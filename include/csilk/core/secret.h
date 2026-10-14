#pragma once
/**
 * @file secret.h
 * @brief Tiered cloud-native secret provider with safety circuit breakers.
 *
 * Provides a unified, layered access point for retrieving sensitive credentials
 * and configuration values:
 * 1. Test overrides / isolated memory injection
 * 2. External Secret Manager hook (Vault, AWS Secrets Manager, etc.)
 * 3. Environment variables (with optional custom or CSILK_ prefix)
 * 4. File-based secrets (Docker secrets, K8s mounted secrets, local files)
 *
 * Features active circuit-breaking in non-test mode to reject default placeholder
 * and weak passwords ("change-me", "default-secret", etc.).
 *
 * @copyright MIT License
 */

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief External Secret Manager callback function prototype.
 *
 * @param[in] key Secret identifier (e.g. "JWT_SECRET", "DB_PASSWORD").
 * @param[out] out Output buffer to store secret.
 * @param[in] out_size Output buffer capacity.
 * @return 0 on success, -1 on miss or failure.
 */
typedef int (*csilk_secret_manager_fn)(const char* key, char* out, size_t out_size);

/**
 * @brief Register external Secret Manager callback.
 *
 * @param fn Callback function, or NULL to unregister.
 */
void csilk_secret_set_manager(csilk_secret_manager_fn fn);

/**
 * @brief Set custom environment/file prefix (e.g. "MINEFOLIO").
 *
 * @param prefix Prefix string (alphanumeric and underscore), or NULL to clear.
 */
void csilk_secret_set_prefix(const char* prefix);

/**
 * @brief Retrieve sensitive secret value through tiered providers.
 *
 * Follows evaluation order:
 * 1. In-memory test overrides (if test mode active or override set).
 * 2. External Secret Manager callback (if registered).
 * 3. Environment variables (exact key, <PREFIX>_<KEY>, CSILK_<KEY>).
 * 4. File provider (<KEY>_FILE, <PREFIX>_<KEY>_FILE, /run/secrets/<key_lower>,
 * config/secrets/<key_lower>).
 *
 * In production mode (non-test), values matching forbidden patterns
 * ("change-me", "default-secret", etc.) will be rejected and return NULL.
 *
 * @param key Secret key identifier.
 * @param out Output buffer (if NULL, returns thread-local static ring buffer).
 * @param out_size Size of out buffer.
 * @return Secret string pointer, or NULL if not found or rejected.
 */
const char* csilk_secret_get(const char* key, char* out, size_t out_size);

/**
 * @brief Retrieve general configuration item with fallback default.
 *
 * Checks Secret Provider first, then environment variables, then falls back to default_val.
 *
 * @param key Configuration key name.
 * @param out Output buffer (if NULL, returns thread-local ring buffer).
 * @param out_size Buffer capacity.
 * @param default_val Fallback value if not found (can be NULL).
 * @return Configuration value string.
 */
const char* csilk_env_get(const char* key, char* out, size_t out_size, const char* default_val);

/**
 * @brief Set test mode flag.
 */
void csilk_secret_set_test_mode(bool enabled);

/**
 * @brief Check if test mode is currently active.
 */
bool csilk_secret_is_test_mode(void);

/**
 * @brief Inject or remove an in-memory test override.
 *
 * @param key Secret key name.
 * @param value Override value, or NULL to remove override.
 */
void csilk_secret_set_test_override(const char* key, const char* value);

/**
 * @brief Clear all test overrides.
 */
void csilk_secret_clear_test_overrides(void);

/**
 * @brief Check if a secret is configured and passes safety validation.
 *
 * @param key Secret identifier.
 * @return true if valid and configured, false otherwise.
 */
bool csilk_secret_is_valid(const char* key);

#ifdef __cplusplus
}
#endif
