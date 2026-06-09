import unittest
import os
import sys
import threading
import time
import urllib.request
import urllib.error
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
        
        # 1. Basic Route
        @app.get("/hello")
        def handle_hello(ctx: Context):
            ctx.string(200, "hello world from python")

        # 2. JSON Echo Route
        @app.post("/echo")
        def handle_echo(ctx: Context):
            req_body = ctx.body.decode('utf-8')
            data = json.loads(req_body)
            ctx.json(200, {"echoed": data})

        # 3. Path Parameters Route
        @app.get("/user/:id")
        def handle_user(ctx: Context):
            # Test ctx.params and ctx.param()
            p_id = ctx.param("id")
            params = ctx.params
            ctx.json(200, {
                "id_direct": p_id,
                "id_params": params.get("id"),
                "all_params": params
            })

        # 4. Query Parameters Route
        @app.get("/search")
        def handle_search(ctx: Context):
            q = ctx.query("q")
            limit = ctx.query("limit")
            ctx.json(200, {"q": q, "limit": limit})

        # 5. Header Route
        @app.get("/headers-test")
        def handle_headers(ctx: Context):
            x_req = ctx.get_header("X-Request-Test")
            ctx.set_header("X-Response-Test", "response-ok")
            ctx.string(200, x_req if x_req else "no-request-header")

        # 6. Middleware Route Group
        @app.use
        def global_middleware(ctx: Context):
            # Add request header dynamically in middleware
            ctx.set_request_header("X-Middleware-Header", "processed")
            ctx.next()

        @app.get("/middleware-check")
        def handle_middleware(ctx: Context):
            val = ctx.get_header("X-Middleware-Header")
            ctx.string(200, val if val else "failed")

        # 7. Abort Middleware Route
        def aborting_middleware(ctx: Context):
            ctx.string(403, "forbidden by middleware")
            ctx.abort()

        app.use_group("/admin", aborting_middleware)

        @app.get("/admin/dashboard")
        def handle_admin(ctx: Context):
            # Should not be reached because of abort
            ctx.string(200, "welcome admin")

        # 8. WebSocket Route
        @app.get("/ws")
        def handle_ws(ctx: Context):
            ctx.ws_handshake()
            def on_msg(c, payload, opcode):
                c.ws_send(payload, opcode)
            ctx.set_on_ws_message(on_msg)

        # 9. Permission Route
        @app.get("/perm-check", perm_required="read", perm_resource="users:*")
        def handle_perm(ctx: Context):
            ctx.string(200, "has permission")

        # Run server in thread
        def run_server():
            app.run(8082)
            
        t = threading.Thread(target=run_server)
        t.daemon = True
        t.start()

        # Bypass system proxy for tests
        proxy_handler = urllib.request.ProxyHandler({})
        opener = urllib.request.build_opener(proxy_handler)
        urllib.request.install_opener(opener)

        # Wait for the server to be ready with retries
        import socket
        for _ in range(15):
            try:
                with socket.create_connection(("127.0.0.1", 8082), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            self.fail("Server did not start in time")
        
        # Helper to make request with retries
        def request_with_retry(url, data=None, headers=None, method='GET'):
            for _ in range(10):
                try:
                    req = urllib.request.Request(url, data=data, headers=headers or {}, method=method)
                    res = urllib.request.urlopen(req)
                    return res
                except urllib.error.HTTPError as e:
                    # Return error response directly so we can assert on status codes like 403
                    return e
                except Exception:
                    time.sleep(0.1)
                    continue
            self.fail("Server did not respond successfully in time")
        
        try:
            # 1. Test GET request
            res = request_with_retry("http://127.0.0.1:8082/hello")
            self.assertEqual(res.status, 200)
            self.assertEqual(res.read().decode('utf-8'), "hello world from python")
            
            # 2. Test POST JSON request
            req_data = json.dumps({"test": "value"}).encode('utf-8')
            res2 = request_with_retry(
                "http://127.0.0.1:8082/echo",
                data=req_data,
                headers={"Content-Type": "application/json"},
                method='POST'
            )
            self.assertEqual(res2.status, 200)
            res_data = json.loads(res2.read().decode('utf-8'))
            self.assertEqual(res_data["echoed"]["test"], "value")

            # 3. Test Path parameters
            res3 = request_with_retry("http://127.0.0.1:8082/user/42")
            self.assertEqual(res3.status, 200)
            res_params = json.loads(res3.read().decode('utf-8'))
            self.assertEqual(res_params["id_direct"], "42")
            self.assertEqual(res_params["id_params"], "42")
            self.assertEqual(res_params["all_params"], {"id": "42"})

            # 4. Test Query parameters
            res4 = request_with_retry("http://127.0.0.1:8082/search?q=gemini&limit=10")
            self.assertEqual(res4.status, 200)
            res_query = json.loads(res4.read().decode('utf-8'))
            self.assertEqual(res_query["q"], "gemini")
            self.assertEqual(res_query["limit"], "10")

            # 5. Test headers get & set
            res5 = request_with_retry(
                "http://127.0.0.1:8082/headers-test",
                headers={"X-Request-Test": "custom-val"}
            )
            self.assertEqual(res5.status, 200)
            self.assertEqual(res5.read().decode('utf-8'), "custom-val")
            self.assertEqual(res5.headers.get("X-Response-Test"), "response-ok")

            # 6. Test Global middleware (request header mutation + next)
            res6 = request_with_retry("http://127.0.0.1:8082/middleware-check")
            self.assertEqual(res6.status, 200)
            self.assertEqual(res6.read().decode('utf-8'), "processed")

            # 7. Test Abort middleware (use_group)
            res7 = request_with_retry("http://127.0.0.1:8082/admin/dashboard")
            self.assertEqual(res7.status, 403)
            self.assertEqual(res7.read().decode('utf-8'), "forbidden by middleware")

            # 8. Test WebSocket handshake and message echo
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.connect(("127.0.0.1", 8082))
            handshake = (
                "GET /ws HTTP/1.1\r\n"
                "Host: 127.0.0.1:8082\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            )
            s.sendall(handshake.encode('utf-8'))
            resp = s.recv(4096).decode('utf-8')
            self.assertIn("101 Switching Protocols", resp)
            self.assertIn("Upgrade: websocket", resp)
            
            # Send message "hello" in unmasked frame
            frame = bytes([0x81, 0x05]) + b"hello"
            s.sendall(frame)
            
            resp_frame = s.recv(1024)
            self.assertEqual(resp_frame[0], 0x81)
            self.assertEqual(resp_frame[1], 0x05)
            self.assertEqual(resp_frame[2:], b"hello")
            s.close()

            # 9. Test route registered with permissions
            res9 = request_with_retry("http://127.0.0.1:8082/perm-check")
            self.assertEqual(res9.status, 200)
            self.assertEqual(res9.read().decode('utf-8'), "has permission")

        finally:
            app.stop()
            t.join(timeout=1.0)

if __name__ == '__main__':
    unittest.main()
