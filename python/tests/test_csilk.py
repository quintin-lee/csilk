import unittest
import os
import sys
import threading
import time
import urllib.request
import urllib.error
import json

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from csilk import (
    App, Context, Group,
    request_id_middleware, cors, jwt_middleware,
    AI, AIContext, DBPool, Crypto, Perm, VectorDB
)


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
        
        # Test server config setting
        app.set_server_config(
            idle_timeout_ms=15000,
            tcp_nodelay=1,
            max_body_size=1024 * 1024
        )
        
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

        # 8b. Class-based WebSocket Route
        class MyWS:
            def on_connect(self, ctx):
                pass
            def on_message(self, ctx, payload, opcode):
                ctx.ws_send(b"CLASS:" + payload, opcode)
        
        app.ws("/ws-class", MyWS)

        # 9. Permission Route
        @app.get("/perm-check", perm_required="read", perm_resource="users:*")
        def handle_perm(ctx: Context):
            ctx.string(200, "has permission")

        # 10. Cookies Route
        @app.get("/cookies")
        def handle_cookies(ctx: Context):
            c_val = ctx.get_cookie("my_cookie")
            ctx.set_cookie("response_cookie", "cookie-ok", path="/", secure=True, http_only=True)
            ctx.string(200, c_val if c_val else "no-cookie")

        # 11. Form and Iterators Route
        @app.post("/form-test")
        def handle_form(ctx: Context):
            ctx.parse_form_urlencoded()
            queries = ctx.queries
            headers = ctx.headers
            form_fields = ctx.form_fields
            ctx.json(200, {
                "query_val": queries.get("q"),
                "header_val": headers.get("x-test-header"),
                "form_val": form_fields.get("field"),
                "debug_headers": list(headers.keys())
            })

        # 12. Handler Metadata Route
        @app.get("/meta/:some_param")
        def handle_meta(ctx: Context):
            ctx.json(200, {
                "path": ctx.handler_path,
                "perm_req": ctx.handler_perm_required,
                "perm_res": ctx.handler_perm_resource
            })

        # 13. Storage Route
        @app.get("/storage")
        def handle_storage(ctx: Context):
            ctx.set_string("mystr", "hello_storage")
            val = ctx.get_string("mystr")
            
            c1 = ctx.incr("counter", 0)
            c2 = ctx.incr("counter", 0)
            ctx.json(200, {
                "val": val,
                "c1": c1,
                "c2": c2
            })

        # Register global request ID middleware
        app.use(request_id_middleware)

        # 14. Response Mutation Route
        @app.get("/response-body-test")
        def handle_response_body(ctx: Context):
            ctx.string(200, "original")
            ctx.response_body = "mutated"

        # 15. Session Routes
        @app.get("/session-write")
        def handle_session_write(ctx: Context):
            ctx.session_start()
            ctx.session["name"] = "gemini"
            ctx.session["count"] = 100
            ctx.string(200, f"session_created:{ctx.session_id}")

        @app.get("/session-read")
        def handle_session_read(ctx: Context):
            ctx.session_start()
            name = ctx.session.get("name", "none")
            count = ctx.session.get("count", 0)
            ctx.string(200, f"name={name},count={count}")

        # 16. CORS test group
        app.use_group("/cors-test", cors(allow_origin="http://test.com"))
        @app.get("/cors-test/ok")
        def handle_cors_ok(ctx: Context):
            ctx.string(200, "cors-ok")

        # 17. MQ test setup
        mq_received = []

        @app.mq.subscribe("test.topic")
        def on_mq_message(ctx):
            mq_received.append(ctx.payload.decode('utf-8'))

        @app.get("/mq-publish")
        def handle_mq_publish(ctx: Context):
            app.mq.publish("test.topic", "hello-mq-bus")
            ctx.string(200, "published")

        @app.get("/mq-result")
        def handle_mq_result(ctx: Context):
            time.sleep(0.05)
            ctx.json(200, {
                "received": mq_received,
                "stats": app.mq.stats
            })

        # 18. Admin Dashboard
        app.admin_serve("/admin")

        # 19. JWT Routes
        @app.get("/jwt-generate")
        def handle_jwt_generate(ctx: Context):
            token = ctx.jwt_generate({"user": "gemini", "role": "tester"}, "mysecret")
            if token:
                ctx.string(200, token)
            else:
                ctx.string(500, "generation failed")

        app.use_group("/jwt-protected", jwt_middleware("mysecret"))
        @app.get("/jwt-protected/info")
        def handle_jwt_info(ctx: Context):
            payload = ctx.jwt_payload
            if payload:
                ctx.json(200, {"user": payload.get("user"), "role": payload.get("role")})
            else:
                ctx.string(400, "no-payload")

        # 20. SSE Route
        @app.get("/sse")
        def handle_sse(ctx: Context):
            ctx.sse_init()
            is_sse_flag = ctx.is_sse
            ctx.sse_send("update", f"is_sse:{is_sse_flag}")
            ctx.sse_close()

        # 21. Multipart Form Route
        @app.post("/multipart-test")
        def handle_multipart(ctx: Context):
            parts = []
            def on_part(part):
                parts.append({
                    "name": part["name"],
                    "filename": part["filename"],
                    "content_type": part["content_type"],
                    "data": part["data"].decode('utf-8') if part["data"] else ""
                })
            ctx.multipart_parse(on_part)
            ctx.json(200, {"parts": parts})




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

            # 8b. Test Class-based WebSocket
            s2 = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s2.connect(("127.0.0.1", 8082))
            handshake2 = (
                "GET /ws-class HTTP/1.1\r\n"
                "Host: 127.0.0.1:8082\r\n"
                "Upgrade: websocket\r\n"
                "Connection: Upgrade\r\n"
                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                "Sec-WebSocket-Version: 13\r\n\r\n"
            )
            s2.sendall(handshake2.encode('utf-8'))
            resp2 = s2.recv(4096).decode('utf-8')
            self.assertIn("101 Switching Protocols", resp2)
            
            frame2 = bytes([0x81, 0x05]) + b"hello"
            s2.sendall(frame2)
            
            resp_frame2 = s2.recv(1024)
            self.assertEqual(resp_frame2[0], 0x81)
            self.assertEqual(resp_frame2[1], 11)
            self.assertEqual(resp_frame2[2:13], b"CLASS:hello")
            s2.close()

            # 9. Test route registered with permissions
            res9 = request_with_retry("http://127.0.0.1:8082/perm-check")
            self.assertEqual(res9.status, 200)
            self.assertEqual(res9.read().decode('utf-8'), "has permission")

            # 10. Test Cookies
            res10 = request_with_retry(
                "http://127.0.0.1:8082/cookies",
                headers={"Cookie": "my_cookie=hello-cookie"}
            )
            self.assertEqual(res10.status, 200)
            self.assertEqual(res10.read().decode('utf-8'), "hello-cookie")
            cookie_header = res10.headers.get("Set-Cookie")
            self.assertIsNotNone(cookie_header)
            self.assertIn("response_cookie=cookie-ok", cookie_header)
            self.assertIn("Path=/", cookie_header)
            self.assertIn("Secure", cookie_header)
            self.assertIn("HttpOnly", cookie_header)

            # 11. Test Form parsing and map properties (headers, queries, form_fields)
            form_data = b"field=hello-form"
            res11 = request_with_retry(
                "http://127.0.0.1:8082/form-test?q=gemini",
                data=form_data,
                headers={
                    "Content-Type": "application/x-www-form-urlencoded",
                    "X-Test-Header": "header-val"
                },
                method="POST"
            )
            self.assertEqual(res11.status, 200)
            res11_json = json.loads(res11.read().decode('utf-8'))
            self.assertEqual(res11_json["query_val"], "gemini")
            self.assertEqual(res11_json["header_val"], "header-val")
            self.assertEqual(res11_json["form_val"], "hello-form")

            # 12. Test Handler Metadata
            res12 = request_with_retry("http://127.0.0.1:8082/meta/hello")
            self.assertEqual(res12.status, 200)
            res12_json = json.loads(res12.read().decode('utf-8'))
            self.assertEqual(res12_json["path"], "/meta/:some_param")

            # 13. Test Storage String & Incr
            res13 = request_with_retry("http://127.0.0.1:8082/storage")
            self.assertEqual(res13.status, 200)
            res13_json = json.loads(res13.read().decode('utf-8'))
            self.assertEqual(res13_json["val"], "hello_storage")
            self.assertEqual(res13_json["c1"], 1)
            self.assertEqual(res13_json["c2"], 2)

            # 14. Test Response Body Mutation
            res14 = request_with_retry("http://127.0.0.1:8082/response-body-test")
            self.assertEqual(res14.status, 200)
            self.assertEqual(res14.read().decode('utf-8'), "mutated")
            # Also check if request_id_middleware is working globally
            self.assertIsNotNone(res14.headers.get("X-Request-Id"))

            # 15. Test Session Management (Write then Read)
            res15_write = request_with_retry("http://127.0.0.1:8082/session-write")
            self.assertEqual(res15_write.status, 200)
            write_body = res15_write.read().decode('utf-8')
            self.assertTrue(write_body.startswith("session_created:"))
            session_cookie = res15_write.headers.get("Set-Cookie")
            self.assertIsNotNone(session_cookie)
            
            # Extract cookie value: "csilk_session=UUID; ..."
            # We want to send back Cookie: csilk_session=UUID
            cookie_parts = [part.strip() for part in session_cookie.split(";")]
            session_cookie_val = [part for part in cookie_parts if part.startswith("csilk_session=")][0]
            
            res15_read = request_with_retry(
                "http://127.0.0.1:8082/session-read",
                headers={"Cookie": session_cookie_val}
            )
            self.assertEqual(res15_read.status, 200)
            self.assertEqual(res15_read.read().decode('utf-8'), "name=gemini,count=100")

            # 16. Test CORS middleware on group
            res16 = request_with_retry("http://127.0.0.1:8082/cors-test/ok")
            self.assertEqual(res16.status, 200)
            self.assertEqual(res16.headers.get("Access-Control-Allow-Origin"), "http://test.com")

            # 17. Test MQ Event Bus (Publish & Subscribe)
            res17_pub = request_with_retry("http://127.0.0.1:8082/mq-publish")
            self.assertEqual(res17_pub.status, 200)
            self.assertEqual(res17_pub.read().decode('utf-8'), "published")

            res17_res = request_with_retry("http://127.0.0.1:8082/mq-result")
            self.assertEqual(res17_res.status, 200)
            res17_json = json.loads(res17_res.read().decode('utf-8'))
            self.assertIn("hello-mq-bus", res17_json["received"])
            self.assertGreaterEqual(res17_json["stats"]["published_total"], 1)
            self.assertGreaterEqual(res17_json["stats"]["delivered_total"], 1)

            # 18. Test Admin Dashboard stats endpoint
            res18 = request_with_retry("http://127.0.0.1:8082/admin/stats")
            self.assertEqual(res18.status, 200)
            res18_json = json.loads(res18.read().decode('utf-8'))
            self.assertIn("process", res18_json)
            self.assertIn("mq", res18_json)

            # 19. Test JWT token generation and verification
            res19_gen = request_with_retry("http://127.0.0.1:8082/jwt-generate")
            self.assertEqual(res19_gen.status, 200)
            token = res19_gen.read().decode('utf-8')
            self.assertEqual(len(token.split(".")), 3)

            # Verification: request without token
            res19_no_token = request_with_retry("http://127.0.0.1:8082/jwt-protected/info")
            self.assertEqual(res19_no_token.status, 401)

            # Verification: request with valid token
            res19_valid = request_with_retry(
                "http://127.0.0.1:8082/jwt-protected/info",
                headers={"Authorization": f"Bearer {token}"}
            )
            self.assertEqual(res19_valid.status, 200)
            res19_json = json.loads(res19_valid.read().decode('utf-8'))
            self.assertEqual(res19_json["user"], "gemini")
            self.assertEqual(res19_json["role"], "tester")

            # 20. Test Server-Sent Events (SSE)
            res20 = request_with_retry("http://127.0.0.1:8082/sse")
            self.assertEqual(res20.status, 200)
            self.assertEqual(res20.headers.get("Content-Type"), "text/event-stream")
            body = res20.read().decode('utf-8')
            self.assertIn("event: update\n", body)
            self.assertIn("data: is_sse:True\n", body)

            # 21. Test Multipart Form Parsing
            boundary = "----WebKitFormBoundary7MA4YWxkTrZu0gW"
            multipart_body = (
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="text_field"\r\n\r\n'
                f"hello-multipart\r\n"
                f"--{boundary}\r\n"
                f'Content-Disposition: form-data; name="file_field"; filename="test.txt"\r\n'
                f"Content-Type: text/plain\r\n\r\n"
                f"file-content-here\r\n"
                f"--{boundary}--\r\n"
            ).encode('utf-8')
            
            res21 = request_with_retry(
                "http://127.0.0.1:8082/multipart-test",
                data=multipart_body,
                headers={"Content-Type": f"multipart/form-data; boundary={boundary}"},
                method="POST"
            )
            self.assertEqual(res21.status, 200)
            res21_json = json.loads(res21.read().decode('utf-8'))
            self.assertEqual(len(res21_json["parts"]), 2)
            self.assertEqual(res21_json["parts"][0]["name"], "text_field")
            self.assertEqual(res21_json["parts"][0]["data"], "hello-multipart")
            self.assertEqual(res21_json["parts"][1]["name"], "file_field")
            self.assertEqual(res21_json["parts"][1]["filename"], "test.txt")
            self.assertEqual(res21_json["parts"][1]["content_type"], "text/plain")
            self.assertEqual(res21_json["parts"][1]["data"], "file-content-here")




        finally:
            app.stop()
            t.join(timeout=1.0)
            app.free()

class TestCsilkAi(unittest.TestCase):
    def test_ai_lifecycle(self):
        ai = AI("openai", "mock-key")
        self.assertIsNotNone(ai)
        
        stats = AI.get_stats()
        self.assertIn("requests_total", stats)
        
        ai.free()

        with self.assertRaises(RuntimeError):
            AI("non-existent", "key")

    def test_ai_context(self):
        ctx = AIContext(2)
        self.assertEqual(ctx.count, 0)
        
        ctx.add("user", "msg1")
        ctx.add("assistant", "msg2")
        self.assertEqual(ctx.count, 2)
        self.assertEqual(ctx.messages[0]["content"], "msg1")
        self.assertEqual(ctx.messages[1]["content"], "msg2")

        ctx.add("user", "msg3")
        self.assertEqual(ctx.count, 2)
        self.assertEqual(ctx.messages[0]["content"], "msg2")
        self.assertEqual(ctx.messages[1]["content"], "msg3")

        ctx.clear()
        self.assertEqual(ctx.count, 0)
        
        ctx.free()

class TestCsilkDb(unittest.TestCase):
    def test_db_lifecycle_and_queries(self):
        # 1. Initialize SQLite in-memory DB pool
        db = DBPool("sqlite", ":memory:")
        self.assertIsNotNone(db)

        # 2. Exec CREATE TABLE
        db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")

        # 3. Exec INSERT
        db.exec("INSERT INTO users (name, age) VALUES ('alice', 30)")
        db.exec("INSERT INTO users (name, age) VALUES ('bob', 25)")

        # 4. Query without params
        rows = db.query("SELECT * FROM users ORDER BY age DESC")
        self.assertEqual(len(rows), 2)
        self.assertEqual(rows[0]["name"], "alice")
        self.assertEqual(int(rows[0]["age"]), 30)
        self.assertEqual(rows[1]["name"], "bob")
        self.assertEqual(int(rows[1]["age"]), 25)

        # 5. Query with params
        rows_param = db.query("SELECT name FROM users WHERE age = ?", ["25"])
        self.assertEqual(len(rows_param), 1)
        self.assertEqual(rows_param[0]["name"], "bob")

        # 6. Check stats
        stats = DBPool.get_stats()
        self.assertIn("queries_total", stats)

        db.free()

class TestCsilkCrypto(unittest.TestCase):
    def test_random_bytes(self):
        b1 = Crypto.random_bytes(16)
        b2 = Crypto.random_bytes(16)
        self.assertEqual(len(b1), 16)
        self.assertEqual(len(b2), 16)
        # Highly unlikely to be equal
        self.assertNotEqual(b1, b2)

    def test_generate_uuid(self):
        u1 = Crypto.generate_uuid()
        u2 = Crypto.generate_uuid()
        self.assertEqual(len(u1), 36)
        self.assertEqual(len(u2), 36)
        self.assertNotEqual(u1, u2)
        # Check standard UUID format (8-4-4-4-12)
        parts = u1.split("-")
        self.assertEqual(len(parts), 5)
        self.assertEqual(len(parts[0]), 8)
        self.assertEqual(len(parts[1]), 4)
        self.assertEqual(len(parts[2]), 4)
        self.assertEqual(len(parts[3]), 4)
        self.assertEqual(len(parts[4]), 12)

    def test_generate_csrf_token(self):
        t1 = Crypto.generate_csrf_token()
        t2 = Crypto.generate_csrf_token()
        self.assertEqual(len(t1), 32)
        self.assertEqual(len(t2), 32)
        self.assertNotEqual(t1, t2)
        import re
        self.assertTrue(re.match(r"^[0-9a-f]{32}$", t1))

class TestCsilkPerm(unittest.TestCase):
    def test_permission_flow(self):
        # Initialize permission subsystem
        Perm.init()
        Perm.simple_init()
        Perm.set_default("simple")

        # Create test context
        ctx = Context.create_test_context()

        # Set role in context
        ctx.set("role", "user")
        self.assertEqual(ctx.get("role"), "user")

        # Allow user to read articles
        res = Perm.simple_allow("user", "read", "articles:*")
        self.assertEqual(res, 0)

        # Check permission allowed
        self.assertEqual(Perm.check(ctx, "read", "articles:123"), 0)

        # Check permission denied
        self.assertEqual(Perm.check(ctx, "write", "articles:123"), -1)

        # Test require does not abort for allowed permission
        Perm.require(ctx, "read", "articles:456")
        self.assertFalse(ctx.is_aborted)

        # Test require aborts for denied permission
        Perm.require(ctx, "write", "articles:456")
        self.assertTrue(ctx.is_aborted)
        self.assertEqual(ctx.status_code, 403)

        # Clear rules and verify checks on a fresh test context
        Perm.simple_clear()
        ctx2 = Context.create_test_context()
        ctx2.set("role", "user")
        self.assertEqual(Perm.check(ctx2, "read", "articles:123"), -1)

        ctx.free_test_context()
        ctx2.free_test_context()

class TestCsilkVector(unittest.TestCase):
    def test_vector_db_lifecycle_and_error(self):
        db = VectorDB("qdrant", "http://localhost:6333")
        self.assertIsNotNone(db)

        points = [
            {
                "id": "5ae35d5b-607a-470e-ac97-63ddee295837",
                "vector": [0.1, 0.2, 0.3],
                "payload": {"name": "test"}
            }
        ]
        with self.assertRaises(RuntimeError):
            db.upsert("test_collection", points)

        with self.assertRaises(RuntimeError):
            db.search("test_collection", [0.1, 0.2, 0.3])

class TestCsilkGroups(unittest.TestCase):
    def test_route_groups_and_middleware(self):
        app = App()

        # Create a route group
        api = app.group("/api/v1")
        self.assertIsNotNone(api)

        # Add a group-level middleware
        calls = []
        def group_middleware(ctx):
            calls.append("middleware")
            ctx.next()
        api.use(group_middleware)

        # Register a route on the group
        @api.get("/users")
        def list_users(ctx):
            calls.append("handler")
            ctx.string(200, "user1, user2")

        # Create a nested sub-group
        admin = api.group("/admin")
        self.assertIsNotNone(admin)

        @admin.get("/stats")
        def get_stats(ctx):
            calls.append("admin_handler")
            ctx.string(200, "stats")

        import threading
        import urllib.request
        import urllib.error
        import time
        import socket

        # Bypass system proxy for tests
        proxy_handler = urllib.request.ProxyHandler({})
        opener = urllib.request.build_opener(proxy_handler)
        urllib.request.install_opener(opener)

        def run_server():
            app.run(8085)

        t = threading.Thread(target=run_server)
        t.daemon = True
        t.start()

        # Wait for the server to be ready with retries
        for _ in range(15):
            try:
                with socket.create_connection(("127.0.0.1", 8085), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            self.fail("Server did not start in time")

        try:
            # 1. Test group route "/api/v1/users"
            req = urllib.request.urlopen("http://localhost:8085/api/v1/users")
            body = req.read().decode('utf-8')
            self.assertEqual(req.status, 200)
            self.assertEqual(body, "user1, user2")
            self.assertEqual(calls, ["middleware", "handler"])

            # 2. Test nested group route "/api/v1/admin/stats"
            req2 = urllib.request.urlopen("http://localhost:8085/api/v1/admin/stats")
            body2 = req2.read().decode('utf-8')
            self.assertEqual(req2.status, 200)
            self.assertEqual(body2, "stats")
            self.assertEqual(calls, ["middleware", "handler", "middleware", "admin_handler"])
        finally:
            app.stop()
            t.join(timeout=1.0)
            app.free()

class TestCsilkResponseExtensionsAndHooks(unittest.TestCase):
    def test_response_extensions_and_hooks(self):
        app = App()
        
        hook_calls = []

        @app.on_server_start
        def on_start(srv_app):
            self.assertEqual(srv_app, app)
            hook_calls.append("server_start")

        @app.on_server_stop
        def on_stop(srv_app):
            self.assertEqual(srv_app, app)
            hook_calls.append("server_stop")

        @app.on_conn_open
        def on_conn_open(ctx):
            self.assertIsNotNone(ctx)
            hook_calls.append("conn_open")

        @app.on_conn_close
        def on_conn_close(ctx):
            self.assertIsNotNone(ctx)
            hook_calls.append("conn_close")

        @app.on_request_begin
        def on_req_begin(ctx):
            self.assertIsNotNone(ctx)
            hook_calls.append("request_begin")

        @app.on_request_end
        def on_req_end(ctx):
            self.assertIsNotNone(ctx)
            hook_calls.append("request_end")

        # 1. Route to test add_header (dual cookies/keys)
        @app.get("/add-headers")
        def handle_add_headers(ctx: Context):
            ctx.add_header("X-Custom-Header", "value1")
            ctx.add_header("X-Custom-Header", "value2")
            ctx.string(200, "headers-added")

        # 2. Route to test redirect_simple
        @app.get("/redirect-src")
        def handle_redirect_src(ctx: Context):
            ctx.redirect_simple("/redirect-dst")

        @app.get("/redirect-dst")
        def handle_redirect_dst(ctx: Context):
            ctx.string(200, "redirected-ok")

        # 3. Route to test json_error
        @app.get("/json-error")
        def handle_json_error(ctx: Context):
            ctx.json_error(400, "bad request parameter")

        # 4. Route to test is_async, response_write, response_end
        @app.get("/stream-chunks")
        def handle_stream_chunks(ctx: Context):
            ctx.is_async = True
            self.assertTrue(ctx.is_async)
            ctx.response_write("chunk_one ")
            ctx.response_write("chunk_two")
            ctx.response_end()

        # 5. Route to test custom 404 handler
        def handle_custom_404(ctx: Context):
            ctx.string(404, "custom-404-body")
        app.set_not_found_handler(handle_custom_404)

        # Run server on port 8086 in thread
        import socket
        def run_server():
            app.run(8086)

        t = threading.Thread(target=run_server)
        t.daemon = True
        t.start()

        # Wait for the server to be ready with retries
        for _ in range(15):
            try:
                with socket.create_connection(("127.0.0.1", 8086), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            self.fail("Server did not start in time")

        # Bypass system proxy for tests
        proxy_handler = urllib.request.ProxyHandler({})
        opener = urllib.request.build_opener(proxy_handler)
        urllib.request.install_opener(opener)

        try:
            # Test 1: add_header (dual custom headers)
            res1 = urllib.request.urlopen("http://127.0.0.1:8086/add-headers")
            self.assertEqual(res1.status, 200)
            self.assertEqual(res1.read().decode('utf-8'), "headers-added")
            custom_headers = res1.headers.get_all("X-Custom-Header")
            self.assertEqual(custom_headers, ["value2", "value1"])

            # Test 2: redirect_simple
            res2 = urllib.request.urlopen("http://127.0.0.1:8086/redirect-src")
            self.assertEqual(res2.status, 200)
            self.assertEqual(res2.geturl(), "http://127.0.0.1:8086/redirect-dst")
            self.assertEqual(res2.read().decode('utf-8'), "redirected-ok")

            # Test 3: json_error
            try:
                urllib.request.urlopen("http://127.0.0.1:8086/json-error")
                self.fail("Expected HTTPError 400")
            except urllib.error.HTTPError as e:
                self.assertEqual(e.code, 400)
                err_data = json.loads(e.read().decode('utf-8'))
                self.assertEqual(err_data["error"], "bad request parameter")

            # Test 4: async streaming response
            res4 = urllib.request.urlopen("http://127.0.0.1:8086/stream-chunks")
            self.assertEqual(res4.status, 200)
            self.assertEqual(res4.read().decode('utf-8'), "chunk_one chunk_two")

            # Test 5: custom 404 handler
            try:
                urllib.request.urlopen("http://127.0.0.1:8086/non-existent-route")
                self.fail("Expected HTTPError 404")
            except urllib.error.HTTPError as e:
                self.assertEqual(e.code, 404)
                self.assertEqual(e.read().decode('utf-8'), "custom-404-body")

        finally:
            app.stop()
            t.join(timeout=1.0)
            app.free()

        # Check lifecycle hook triggers
        time.sleep(0.2)
        self.assertIn("server_start", hook_calls)
        self.assertIn("server_stop", hook_calls)
        self.assertIn("conn_open", hook_calls)
        self.assertIn("conn_close", hook_calls)
        self.assertIn("request_begin", hook_calls)
        self.assertIn("request_end", hook_calls)

    def test_spa_fallback_and_stats(self):
        app = App()
        
        # Create temp dir and index.html for SPA fallback test
        import tempfile
        import shutil
        temp_dir = tempfile.mkdtemp()
        try:
            with open(os.path.join(temp_dir, "index.html"), "w") as f:
                f.write("spa-fallback-content")

            app.set_spa_fallback(temp_dir)
            
            # Test max connections setting
            app.set_max_connections(50)

            # Run server on port 8087 in thread
            import socket
            def run_server():
                app.run(8087)

            t = threading.Thread(target=run_server)
            t.daemon = True
            t.start()

            # Wait for the server to be ready with retries
            for _ in range(15):
                try:
                    with socket.create_connection(("127.0.0.1", 8087), timeout=0.5):
                        break
                except OSError:
                    time.sleep(0.1)
            else:
                self.fail("Server did not start in time")

            # Bypass system proxy for tests
            proxy_handler = urllib.request.ProxyHandler({})
            opener = urllib.request.build_opener(proxy_handler)
            urllib.request.install_opener(opener)

            try:
                # Test SPA fallback triggers on unmatched GET route
                res = urllib.request.urlopen("http://127.0.0.1:8087/some-unmatched-route")
                self.assertEqual(res.status, 200)
                self.assertEqual(res.read().decode('utf-8'), "spa-fallback-content")

                # Test server stats
                stats = app.server_stats
                self.assertIn("active_connections", stats)
                self.assertIn("pooled_connections", stats)
                self.assertGreaterEqual(stats["active_connections"], 0)
            finally:
                app.stop()
                t.join(timeout=1.0)
                app.free()
        finally:
            shutil.rmtree(temp_dir)

    def test_global_exception_handler(self):
        app = App()

        class CustomError(Exception):
            pass

        @app.exception_handler(CustomError)
        def handle_custom_error(ctx, exc):
            ctx.string(400, f"Handled: {str(exc)}")

        @app.get("/error")
        def route_error(ctx):
            raise CustomError("test exception")

        import socket
        def run_server():
            app.run(8088)

        t = threading.Thread(target=run_server)
        t.daemon = True
        t.start()

        for _ in range(15):
            try:
                with socket.create_connection(("127.0.0.1", 8088), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            self.fail("Server did not start in time")

        proxy_handler = urllib.request.ProxyHandler({})
        opener = urllib.request.build_opener(proxy_handler)
        urllib.request.install_opener(opener)

        try:
            try:
                urllib.request.urlopen("http://127.0.0.1:8088/error")
                self.fail("Expected HTTPError 400")
            except urllib.error.HTTPError as e:
                self.assertEqual(e.code, 400)
                self.assertEqual(e.read().decode('utf-8'), "Handled: test exception")
        finally:
            app.stop()
            t.join(timeout=1.0)
            app.free()

    def test_pydantic_validate_decorator(self):
        from csilk.app import validate
        app = App()

        class MockModel:
            def __init__(self, **kwargs):
                for k, v in kwargs.items():
                    setattr(self, k, v)
        
        @app.post("/validate-test")
        @validate(body=MockModel, query=MockModel)
        def handle_validation(ctx):
            body_val = getattr(ctx.validated_body, 'name', 'missing')
            query_val = getattr(ctx.validated_query, 'age', 'missing')
            ctx.json(200, {"b_name": body_val, "q_age": query_val})

        import socket
        def run_server():
            app.run(8089)
        t = threading.Thread(target=run_server)
        t.daemon = True
        t.start()

        for _ in range(15):
            try:
                with socket.create_connection(("127.0.0.1", 8089), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.1)
        else:
            self.fail("Server did not start in time")

        proxy_handler = urllib.request.ProxyHandler({})
        opener = urllib.request.build_opener(proxy_handler)
        urllib.request.install_opener(opener)

        try:
            # Test success
            req = urllib.request.Request("http://127.0.0.1:8089/validate-test?age=30", data=b'{"name":"csilk"}', method="POST")
            req.add_header('Content-Type', 'application/json')
            res = urllib.request.urlopen(req)
            self.assertEqual(res.status, 200)
            data = json.loads(res.read().decode('utf-8'))
            self.assertEqual(data["b_name"], "csilk")
            self.assertEqual(data["q_age"], "30")

            # Test failure
            try:
                req = urllib.request.Request("http://127.0.0.1:8089/validate-test?age=30", data=b'invalid json', method="POST")
                req.add_header('Content-Type', 'application/json')
                urllib.request.urlopen(req)
                self.fail("Expected 422 Error")
            except urllib.error.HTTPError as e:
                self.assertEqual(e.code, 422)
                
        finally:
            app.stop()
            t.join(timeout=1.0)
            app.free()

if __name__ == '__main__':
    import gc
    import sys
    result = unittest.main(exit=False)
    gc.collect()
    sys.exit(not result.result.wasSuccessful())



def test_asyncio_support():
    import requests
    import asyncio
    
    app = App()
    
    @app.get("/async_route")
    async def handle_async(ctx):
        await asyncio.sleep(0.1)
        return {"async": "success"}

    import threading
    t = threading.Thread(target=app.run, args=(8086,), daemon=True)
    t.start()
    
    import time
    time.sleep(1) # wait for server to start
    
    response = requests.get("http://localhost:8086/async_route")
    assert response.status_code == 200
    assert response.json()["async"] == "success"
    
    app.stop()
    t.join(timeout=2.0)
    app.free()
