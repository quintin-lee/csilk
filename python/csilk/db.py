"""Database connection pool management.

Provides a unified ``DBPool`` class that wraps the csilk C database
abstraction layer.  Supports SQLite, MySQL, PostgreSQL, MongoDB, and
Redis drivers.
"""

import ctypes
import json
from csilk.lib import get_bindings, CsilkDbStats


class DBPool:
    """A pool of database connections managed by a named driver.

    Usage::

        pool = DBPool("sqlite", "file.db")
        rows = pool.query("SELECT * FROM users WHERE id = ?", [1])
        pool.exec("DELETE FROM temp")
        print(DBPool.get_stats())
    """

    def __init__(self, driver_name: str, dsn: str):
        """Open a connection pool.

        Args:
            driver_name: Driver identifier — ``"sqlite"``, ``"mysql"``,
                ``"postgres"``, ``"mongodb"``, or ``"redis"``.
            dsn: Driver-specific connection string.
        Raises:
            RuntimeError: If the pool cannot be created.
        """
        self._lib = get_bindings()
        self._lib.csilk_db_init()
        self._pool = self._lib.csilk_db_pool_new(driver_name.encode('utf-8'), dsn.encode('utf-8'))
        if not self._pool:
            raise RuntimeError(f"Failed to create database pool for driver '{driver_name}' and DSN '{dsn}'")

    def free(self):
        """Release the pool and all underlying connections."""
        if self._pool:
            self._lib.csilk_db_pool_free(self._pool)
            self._pool = None

    def __del__(self):
        self.free()

    def query(self, sql: str, params=None) -> list:
        """Execute a SELECT query and return the result rows as a list of dicts.

        Args:
            sql: SQL query string.  May contain positional placeholders
                (``?``) when *params* is provided.
            params: Optional list of parameter values for placeholders.
                A bare string is wrapped in a single-element list.

        Returns:
            List of dicts mapping column names to values.  Returns an
            empty list when no rows match.

        Raises:
            RuntimeError: If the query fails at the C level.
        """
        if params is None:
            cjson_ptr = self._lib.csilk_db_query_json(self._pool, sql.encode('utf-8'))
        else:
            if isinstance(params, str):
                params = [params]

            c_arr = (ctypes.c_char_p * (len(params) + 1))()
            for i, p in enumerate(params):
                c_arr[i] = str(p).encode('utf-8')
            c_arr[len(params)] = None

            cjson_ptr = self._lib.csilk_db_query_param_json(self._pool, sql.encode('utf-8'), c_arr)

        if not cjson_ptr:
            raise RuntimeError("Database query failed")

        try:
            res_str_ptr = self._lib.cJSON_PrintUnformatted(cjson_ptr)
            if res_str_ptr:
                val = ctypes.string_at(res_str_ptr).decode('utf-8')
                self._lib.csilk_free(res_str_ptr)
                return json.loads(val)
            return []
        finally:
            self._lib.cJSON_Delete(cjson_ptr)

    def exec(self, sql: str):
        """Execute a SQL statement that does not return rows.

        Suitable for INSERT, UPDATE, DELETE, CREATE TABLE, etc.

        Args:
            sql: SQL statement string.
        Raises:
            RuntimeError: If execution fails.
        """
        res = self._lib.csilk_db_exec(self._pool, sql.encode('utf-8'))
        if res != 0:
            raise RuntimeError("Database execution failed")

    @staticmethod
    def get_stats() -> dict:
        """Return global database statistics across all pools.

        Returns:
            A dict with ``queries_total``, ``execs_total``,
            ``errors_total``, and ``duration_us_total``.
        """
        lib = get_bindings()
        stats = CsilkDbStats()
        lib.csilk_db_get_stats(ctypes.byref(stats))
        return {
            "queries_total": stats.queries_total,
            "execs_total": stats.execs_total,
            "errors_total": stats.errors_total,
            "duration_us_total": stats.duration_us_total,
        }
