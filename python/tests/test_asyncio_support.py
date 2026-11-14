"""Unit tests for native Python asyncio coroutine route handling in csilk."""

import asyncio
import json
import os
import sys
import threading
import time
import unittest
import urllib.request

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "python"))

from csilk import App


class TestAsyncioSupport(unittest.TestCase):
    def test_async_route_json_and_error(self):
        app = App()

        @app.get("/async-hello")
        async def handle_hello(ctx):
            await asyncio.sleep(0.01)
            return {"message": "hello from asyncio", "status": "ok"}

        @app.get("/async-string")
        async def handle_string(ctx):
            await asyncio.sleep(0.01)
            return "plain text result"

        @app.get("/async-error")
        async def handle_error(ctx):
            await asyncio.sleep(0.01)
            raise ValueError("coroutine failed")

        import socket

        port = 8092

        def run_server():
            app.run(port)

        t = threading.Thread(target=run_server, daemon=True)
        t.start()

        # Wait for server ready
        for _ in range(20):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            self.fail("Server did not start in time")

        proxy_handler = urllib.request.ProxyHandler({})
        opener = urllib.request.build_opener(proxy_handler)
        urllib.request.install_opener(opener)

        try:
            # 1. Test async JSON return
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/async-hello") as res:
                self.assertEqual(res.status, 200)
                data = json.loads(res.read().decode("utf-8"))
                self.assertEqual(data["message"], "hello from asyncio")
                self.assertEqual(data["status"], "ok")

            # 2. Test async text return
            with urllib.request.urlopen(f"http://127.0.0.1:{port}/async-string") as res:
                self.assertEqual(res.status, 200)
                self.assertEqual(res.read().decode("utf-8"), "plain text result")

            # 3. Test async exception handling (500)
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{port}/async-error")
                self.fail("Expected 500 error")
            except urllib.error.HTTPError as e:
                with e:
                    self.assertEqual(e.code, 500)
                    body = e.read().decode("utf-8")
                    self.assertIn("coroutine failed", body)
        finally:
            app.stop()
            t.join(timeout=5.0)
            self.assertFalse(t.is_alive(), "server thread did not exit cleanly")
            app.free()


if __name__ == "__main__":
    unittest.main()
