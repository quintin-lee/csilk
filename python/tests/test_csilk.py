import unittest
import os
import sys

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

class TestCsilkLoader(unittest.TestCase):
    def test_library_load(self):
        from csilk.lib import load_lib
        lib = load_lib()
        self.assertIsNotNone(lib)

    def test_ctypes_bindings(self):
        from csilk.lib import get_bindings
        lib = get_bindings()
        self.assertIsNotNone(lib.csilk_app_new)
        self.assertIsNotNone(lib.csilk_app_free)
        self.assertIsNotNone(lib.csilk_app_add_route)

    def test_context_class_existence(self):
        from csilk.context import Context
        # Ensure Context constructor takes a ctypes pointer
        from csilk.lib import CsilkCtxPtr
        ctx = Context(CsilkCtxPtr())
        self.assertIsNotNone(ctx)

if __name__ == '__main__':
    unittest.main()
