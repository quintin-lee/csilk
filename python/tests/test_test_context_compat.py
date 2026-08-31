import unittest

from csilk import context as context_module
from csilk.context import Context


class TestContextCompatibility(unittest.TestCase):
    def test_production_bindings_do_not_require_test_context_symbols(self):
        """Loading production ABI bindings must not require test-only symbols."""
        class ProductionLib:
            pass

        original_get_bindings = context_module.get_bindings
        context_module.get_bindings = lambda: ProductionLib()
        try:
            with self.assertRaisesRegex(RuntimeError, "test context support is unavailable"):
                Context.create_test_context()
        finally:
            context_module.get_bindings = original_get_bindings


if __name__ == "__main__":
    unittest.main()
