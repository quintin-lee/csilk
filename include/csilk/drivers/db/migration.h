#pragma once
/**
 * @file migration.h
 * @brief Flyway-style database schema migration engine with distributed locks.
 *
 * Provides structured, versioned database schema migrations with SHA-256
 * tampering detection, transactional execution, and multi-instance distributed
 * concurrency locking.
 *
 * @copyright MIT License
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "csilk/drivers/db.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Migration item metadata.
 */
typedef struct {
    int  version;
    char name[128];
    char filepath[512];
    char checksum[65];
    bool is_applied;
    int  execution_time_ms;
} csilk_migration_item_t;

/**
 * @brief Opaque migration engine handle.
 */
typedef struct csilk_migration_engine_s csilk_migration_engine_t;

/**
 * @brief Compute normalized SHA-256 checksum of migration script content.
 *
 * Normalizes CRLF -> LF and trims trailing whitespace.
 *
 * @param content Input text.
 * @param len Length of content.
 * @param[out] out_checksum Output hex string buffer (at least 65 bytes).
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_checksum_content(const char* content, size_t len, char out_checksum[65]);

/**
 * @brief Compute normalized SHA-256 checksum of a migration file.
 *
 * @param filepath File path.
 * @param[out] out_checksum Output hex string buffer (at least 65 bytes).
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_checksum_file(const char* filepath, char out_checksum[65]);

/**
 * @brief Initialize the migration lock table if not present.
 *
 * @param pool Database pool handle.
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_lock_init(csilk_db_pool_t* pool);

/**
 * @brief Acquire distributed migration lock on schema_migration_lock table.
 *
 * @param pool Database pool handle.
 * @param locked_by Identifier of the locker (e.g. hostname or process name).
 * @param timeout_seconds Timeout in seconds (<= 0 for single attempt).
 * @return 0 on success, -1 on failure or timeout.
 */
int csilk_migration_lock_acquire(csilk_db_pool_t* pool, const char* locked_by, int timeout_seconds);

/**
 * @brief Release distributed migration lock.
 *
 * @param pool Database pool handle.
 * @param locked_by Identifier of the locker.
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_lock_release(csilk_db_pool_t* pool, const char* locked_by);

/**
 * @brief Create a new migration engine instance.
 *
 * @param pool Database pool handle.
 * @param migrations_base_dir Migration scripts directory. If it contains a dialect subdirectory
 *                            matching the current database driver (e.g. "sqlite", "postgres"),
 *                            that dialect subdirectory will be used automatically.
 * @param out_engine Output pointer to receive engine handle.
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_engine_new(csilk_db_pool_t*           pool,
                               const char*                migrations_base_dir,
                               csilk_migration_engine_t** out_engine);

/**
 * @brief Configure baseline version for legacy databases.
 *
 * If the migrations table does not exist and sentinel_table exists (or is NULL),
 * migrations up to baseline_version will be marked as applied without executing.
 *
 * @param engine Engine instance.
 * @param baseline_version Maximum version to baseline.
 * @param sentinel_table Table whose existence indicates a legacy database (or NULL).
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_engine_set_baseline(csilk_migration_engine_t* engine,
                                        int                       baseline_version,
                                        const char*               sentinel_table);

/**
 * @brief Free migration engine.
 *
 * @param engine Engine instance.
 */
void csilk_migration_engine_free(csilk_migration_engine_t* engine);

/**
 * @brief Discover migration scripts sorted by version ascending.
 *
 * @param engine Engine instance.
 * @param[out] out_items Allocated array of items (caller frees with free()).
 * @param[out] out_count Number of discovered items.
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_discover(csilk_migration_engine_t* engine,
                             csilk_migration_item_t**  out_items,
                             int*                      out_count);

/**
 * @brief Validate existing applied migrations against on-disk files.
 *
 * Checks that every applied migration exists on disk and has an identical checksum.
 *
 * @param engine Engine instance.
 * @return 0 if valid, -1 if tampered or missing.
 */
int csilk_migration_validate(csilk_migration_engine_t* engine);

/**
 * @brief Apply pending migrations in sequential transactions.
 *
 * @param engine Engine instance.
 * @param[out] out_applied_count Optional pointer to receive count of newly applied migrations.
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_apply(csilk_migration_engine_t* engine, int* out_applied_count);

/**
 * @brief Get migration status overview.
 *
 * @param engine Engine instance.
 * @param[out] out_items Allocated array of items (caller frees with free()).
 * @param[out] out_count Number of items.
 * @return 0 on success, -1 on failure.
 */
int csilk_migration_status(csilk_migration_engine_t* engine,
                           csilk_migration_item_t**  out_items,
                           int*                      out_count);

#ifdef __cplusplus
}
#endif
