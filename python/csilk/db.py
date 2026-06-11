import ctypes
import json
from csilk.lib import get_bindings, CsilkDbStats

class DBPool:
    def __init__(self, driver_name, dsn):
        self._lib = get_bindings()
        self._lib.csilk_db_init()
        self._pool = self._lib.csilk_db_pool_new(driver_name.encode('utf-8'), dsn.encode('utf-8'))
        if not self._pool:
            raise RuntimeError(f"Failed to create database pool for driver '{driver_name}' and DSN '{dsn}'")

    def free(self):
        if self._pool:
            self._lib.csilk_db_pool_free(self._pool)
            self._pool = None

    def __del__(self):
        self.free()

    def query(self, sql, params=None):
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

    def exec(self, sql):
        res = self._lib.csilk_db_exec(self._pool, sql.encode('utf-8'))
        if res != 0:
            raise RuntimeError("Database execution failed")

    @staticmethod
    def get_stats():
        lib = get_bindings()
        stats = CsilkDbStats()
        lib.csilk_db_get_stats(ctypes.byref(stats))
        return {
            "queries_total": stats.queries_total,
            "execs_total": stats.execs_total,
            "errors_total": stats.errors_total,
            "duration_us_total": stats.duration_us_total
        }
