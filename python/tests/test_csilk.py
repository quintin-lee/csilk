import unittest
import os
import sys
import threading
import time
import urllib.request
import json

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from csilk import App, Context

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

    def test_app_instantiation(self):
        from csilk.app import App
        app = App()
        self.assertIsNotNone(app)
        app.free()

class TestCsilkIntegration(unittest.TestCase):
    def test_routing_and_requests(self):
        app = App()
        
        @app.get("/hello")
        def handle_hello(ctx: Context):
            ctx.string(200, "hello world from python")

        @app.post("/echo")
        def handle_echo(ctx: Context):
            req_body = ctx.body.decode('utf-8')
            data = json.loads(req_body)
            ctx.json(200, {"echoed": data})

        # Run server in thread
        def run_server():
            app.run(8081)
            
        t = threading.Thread(target=run_server)
        t.daemon = True
        t.start()
        
        # Give server time to spin up
        time.sleep(0.3)
        
        try:
            # 1. Test GET request
            res = urllib.request.urlopen("http://127.0.0.1:8081/hello")
            self.assertEqual(res.status, 200)
            self.assertEqual(res.read().decode('utf-8'), "hello world from python")

            # 2. Test POST JSON request
            req = urllib.request.Request(
                "http://127.0.0.1:8081/echo",
                data=json.dumps({"test": "value"}).encode('utf-8'),
                headers={"Content-Type": "application/json"}
            )
            res2 = urllib.request.urlopen(req)
            self.assertEqual(res2.status, 200)
            res_data = json.loads(res2.read().decode('utf-8'))
            self.assertEqual(res_data["echoed"]["test"], "value")
        finally:
            app.stop()
            t.join(timeout=1.0)

if __name__ == '__main__':
    unittest.main()

