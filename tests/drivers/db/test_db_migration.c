#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "csilk/csilk.h"
#include "csilk/drivers/db.h"
#include "csilk/drivers/db/migration.h"

static void
test_checksum_normalization(void)
{
    const char* crlf_text = "CREATE TABLE t1 (\r\n  id INT\r\n);\r\n\r\n   ";
    const char* lf_text = "CREATE TABLE t1 (\n  id INT\n);";

    char c1[65] = {0};
    char c2[65] = {0};

    assert(csilk_migration_checksum_content(crlf_text, strlen(crlf_text), c1) == 0);
    assert(csilk_migration_checksum_content(lf_text, strlen(lf_text), c2) == 0);

    assert(strlen(c1) == 64);
    assert(strcmp(c1, c2) == 0);
}

static void
test_distributed_lock(csilk_db_pool_t* pool)
{
    assert(csilk_migration_lock_init(pool) == 0);

    /* Proc A acquires lock */
    assert(csilk_migration_lock_acquire(pool, "proc_a", 0) == 0);

    /* Proc B attempts to acquire without timeout -> should fail */
    assert(csilk_migration_lock_acquire(pool, "proc_b", 0) == -1);

    /*
     * csilk#7 regression: a foreign release must be refused.
     * proc_b did NOT acquire the lock, so it must not be able to
     * release proc_a's lock.  The old implementation would have
     * cleared the lock here (WHERE id = 1, no holder check) and
     * allowed proc_c to grab it while proc_a was still writing.
     */
    assert(csilk_migration_lock_release(pool, "proc_b") == -1);

    /* A release with NULL/empty holder identifier must also be refused. */
    assert(csilk_migration_lock_release(pool, NULL) == -1);

    /* Proc A releases its own lock -> succeeds */
    assert(csilk_migration_lock_release(pool, "proc_a") == 0);

    /* Proc B acquires now -> should succeed */
    assert(csilk_migration_lock_acquire(pool, "proc_b", 0) == 0);

    /* Release */
    assert(csilk_migration_lock_release(pool, "proc_b") == 0);
}

/*
 * csilk#7 regression: a crashed holder's lock must auto-expire via the
 * lease.  We acquire with a 1-second lease, then wait ~1.2s without
 * releasing; a second acquire should succeed (the lease has expired).
 */
static void
test_distributed_lock_lease_expiry(csilk_db_pool_t* pool)
{
    assert(csilk_migration_lock_init(pool) == 0);

    /* Acquire with a 1-second lease. */
    assert(csilk_migration_lock_acquire_lease(pool, "crashed_holder", 0, 1) == 0);

    /* Simulate the holder crashing: it never releases.  Wait ~1.2s so
     * the 1-second lease expires. */
    usleep(1200000);

    /* A second instance should now be able to acquire, because the
     * expired lease makes the lock re-acquirable. */
    assert(csilk_migration_lock_acquire(pool, "new_holder", 0) == 0);

    /* Clean up. */
    assert(csilk_migration_lock_release(pool, "new_holder") == 0);
}

static void
test_migration_engine_flow(csilk_db_pool_t* pool)
{
    char tmp_dir[] = "/tmp/csilk_mig_test_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    char f1[512], f2[512];
    snprintf(f1, sizeof(f1), "%s/V001__init_schema.sql", tmp_dir);
    snprintf(f2, sizeof(f2), "%s/V002__add_email.sql", tmp_dir);

    FILE* fp1 = fopen(f1, "w");
    assert(fp1 != NULL);
    fputs("CREATE TABLE members (id INTEGER PRIMARY KEY, name TEXT);\n", fp1);
    fclose(fp1);

    FILE* fp2 = fopen(f2, "w");
    assert(fp2 != NULL);
    fputs("ALTER TABLE members ADD COLUMN email TEXT;\n", fp2);
    fclose(fp2);

    csilk_migration_engine_t* engine = NULL;
    assert(csilk_migration_engine_new(pool, tmp_dir, &engine) == 0);
    assert(engine != NULL);

    /* 1. Discover */
    csilk_migration_item_t* items = NULL;
    int                     count = 0;
    assert(csilk_migration_discover(engine, &items, &count) == 0);
    assert(count == 2);
    assert(items[0].version == 1);
    assert(strcmp(items[0].name, "init_schema") == 0);
    assert(items[1].version == 2);
    assert(strcmp(items[1].name, "add_email") == 0);
    free(items);

    /* 2. Apply */
    int applied = 0;
    assert(csilk_migration_apply(engine, &applied) == 0);
    assert(applied == 2);

    /* Verify table works */
    assert(csilk_db_exec(
               pool,
               "INSERT INTO members (id, name, email) VALUES (1, 'Alice', 'a@example.com');") == 0);

    /* 3. Re-apply (idempotent, 0 new) */
    assert(csilk_migration_apply(engine, &applied) == 0);
    assert(applied == 0);

    /* 4. Status */
    items = NULL;
    count = 0;
    assert(csilk_migration_status(engine, &items, &count) == 0);
    assert(count == 2);
    assert(items[0].is_applied == true);
    assert(items[1].is_applied == true);
    free(items);

    /* 5. Validate before tamper */
    assert(csilk_migration_validate(engine) == 0);

    /* 6. Tamper with V001 file */
    fp1 = fopen(f1, "a");
    assert(fp1 != NULL);
    fputs("-- tampered modification\n", fp1);
    fclose(fp1);

    /* Validation should now fail */
    assert(csilk_migration_validate(engine) == -1);

    csilk_migration_engine_free(engine);

    /* Cleanup temp files */
    unlink(f1);
    unlink(f2);
    rmdir(tmp_dir);
}

static void
test_migration_auto_baseline(void)
{
    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(pool != NULL);

    /* Simulate legacy database with pre-existing table 'users' */
    assert(csilk_db_exec(pool, "CREATE TABLE users (id INTEGER PRIMARY KEY);") == 0);

    char tmp_dir[] = "/tmp/csilk_mig_base_XXXXXX";
    assert(mkdtemp(tmp_dir) != NULL);

    char f1[512], f2[512];
    snprintf(f1, sizeof(f1), "%s/V001__create_users.sql", tmp_dir);
    snprintf(f2, sizeof(f2), "%s/V002__create_posts.sql", tmp_dir);

    /* V001 would fail if executed because 'users' already exists */
    FILE* fp1 = fopen(f1, "w");
    assert(fp1 != NULL);
    fputs("CREATE TABLE users (id INTEGER PRIMARY KEY);\n", fp1);
    fclose(fp1);

    FILE* fp2 = fopen(f2, "w");
    assert(fp2 != NULL);
    fputs("CREATE TABLE posts (id INTEGER PRIMARY KEY, title TEXT);\n", fp2);
    fclose(fp2);

    csilk_migration_engine_t* engine = NULL;
    assert(csilk_migration_engine_new(pool, tmp_dir, &engine) == 0);
    assert(csilk_migration_engine_set_baseline(engine, 1, "users") == 0);

    int applied = 0;
    assert(csilk_migration_apply(engine, &applied) == 0);
    /* Only V002 was actually executed, V001 was baselined! */
    assert(applied == 1);

    /* Both versions must exist in schema_migrations */
    csilk_migration_item_t* items = NULL;
    int                     count = 0;
    assert(csilk_migration_status(engine, &items, &count) == 0);
    assert(count == 2);
    assert(items[0].version == 1 && items[0].is_applied == true);
    assert(items[1].version == 2 && items[1].is_applied == true);
    free(items);

    /* Verify posts table was created */
    assert(csilk_db_exec(pool, "INSERT INTO posts (id, title) VALUES (1, 'Hello');") == 0);

    csilk_migration_engine_free(engine);
    unlink(f1);
    unlink(f2);
    rmdir(tmp_dir);
    csilk_db_pool_free(pool);
}

int
main(void)
{
    csilk_db_init();

    test_checksum_normalization();

    csilk_db_pool_t* pool = csilk_db_pool_new("sqlite", ":memory:");
    assert(pool != NULL);

    test_distributed_lock(pool);
    test_distributed_lock_lease_expiry(pool);
    test_migration_engine_flow(pool);

    csilk_db_pool_free(pool);

    test_migration_auto_baseline();
    return 0;
}
