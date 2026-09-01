import unittest

from csilk.ai import AI


class TestAISafety(unittest.TestCase):
    def test_failed_chat_does_not_crash_process(self):
        ai = AI("openai", api_key="invalid", base_url="http://127.0.0.1:12345")
        try:
            with self.assertRaises(RuntimeError):
                ai.chat("gpt-4", [{"role": "user", "content": "hello"}], timeout_ms=10)
        finally:
            ai.free()


if __name__ == "__main__":
    unittest.main()
