import os
import sys
import unittest

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import csilk.lib as csilk_lib


OPTIONAL_CORE_ABSENT_SYMBOLS = (
    ("csilk_db_init", "CSILK_ENABLE_DB"),
    ("csilk_server_get_mq", "CSILK_ENABLE_MQ"),
    ("csilk_mq_publish", "CSILK_ENABLE_MQ"),
    ("csilk_ai_new", "CSILK_ENABLE_AI"),
    ("csilk_vector_db_new", "CSILK_ENABLE_VECTOR"),
    ("csilk_wf_new", "CSILK_ENABLE_WORKFLOW"),
    ("csilk_perm_init", "CSILK_ENABLE_PERM"),
    ("csilk_trace_middleware", "CSILK_ENABLE_OBSERVABILITY"),
    ("csilk_ctx_get_trace_id", "CSILK_ENABLE_OBSERVABILITY"),
    ("csilk_ctx_get_span_id", "CSILK_ENABLE_OBSERVABILITY"),
    ("csilk_admin_serve", "CSILK_ENABLE_OBSERVABILITY"),
)


class TestWebCoreCompatibility(unittest.TestCase):
    def test_get_bindings_succeeds(self):
        lib = csilk_lib.get_bindings()
        self.assertIsNotNone(lib)

    def test_has_symbol_core_and_optional(self):
        lib = csilk_lib.get_bindings()
        self.assertTrue(csilk_lib.has_symbol(lib, "csilk_app_new"))
        self.assertTrue(csilk_lib.has_symbol(lib, "csilk_app_free"))
        self.assertFalse(csilk_lib.has_symbol(lib, "csilk_nonexistent_symbol_xyz"))

        if not hasattr(lib, "csilk_ai_new"):
            for sym, _feature in OPTIONAL_CORE_ABSENT_SYMBOLS:
                with self.subTest(symbol=sym):
                    self.assertFalse(csilk_lib.has_symbol(lib, sym))

    def test_require_symbol_behavior(self):
        lib = csilk_lib.get_bindings()
        fn = csilk_lib.require_symbol(lib, "csilk_app_new", "CORE")
        self.assertTrue(callable(fn))

        with self.assertRaisesRegex(
            NotImplementedError,
            r"csilk built without TEST_FEATURE support \(missing csilk_nonexistent_symbol_xyz\)",
        ):
            csilk_lib.require_symbol(lib, "csilk_nonexistent_symbol_xyz", "TEST_FEATURE")

        if not csilk_lib.has_symbol(lib, "csilk_ai_new"):
            with self.assertRaisesRegex(
                NotImplementedError,
                r"csilk built without CSILK_ENABLE_AI support \(missing csilk_ai_new\)",
            ):
                csilk_lib.require_symbol(lib, "csilk_ai_new", "CSILK_ENABLE_AI")

    def test_core_app_lifecycle_works_with_loaded_bindings(self):
        lib = csilk_lib.get_bindings()
        app = lib.csilk_app_new(b"0.0.0.0:0")
        self.assertTrue(bool(app))
        lib.csilk_app_free(app)


if __name__ == "__main__":
    unittest.main()
