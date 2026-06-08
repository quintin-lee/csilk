import unittest
import os
import sys

# Add python path
sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))

class TestCsilkLoader(unittest.TestCase):
    def test_library_load(self):
        from csilk.lib import load_lib
        lib = load_lib()
        self.assertIsNotNone(lib)

if __name__ == '__main__':
    unittest.main()
