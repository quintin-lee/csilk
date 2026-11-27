import asyncio
import json
import os
import socket
import sys
import threading
import time
import unittest
import urllib.error
import urllib.request

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), "..")))

import csilk
import csilk.lib as csilk_lib
from csilk import (
    AI,
    AIContext,
    ASGIAdapter,
    App,
    Context,
    DBPool,
    Group,
    MQ,
    MqContext,
    Perm,
    VectorDB,
    Workflow,
    WorkflowContext,
    WorkflowData,
    WorkflowNode,
    WorkflowWorker,
    cors,
    jwt_middleware,
    logger_middleware,
)
from csilk.middleware import trace_middleware


def _pick_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


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

        if not csilk_lib.has_symbol(lib, "csilk_ai_new"):
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

    def test_package_import_and_high_level_app_lifecycle(self):
        self.assertIsNotNone(csilk.__version__)
        self.assertIs(csilk.App, App)
        self.assertIs(csilk.Context, Context)
        self.assertIs(csilk.Group, Group)
        self.assertIs(csilk.AI, AI)
        self.assertIs(csilk.AIContext, AIContext)
        self.assertIs(csilk.DBPool, DBPool)
        self.assertIs(csilk.Perm, Perm)
        self.assertIs(csilk.VectorDB, VectorDB)
        self.assertIs(csilk.Workflow, Workflow)
        self.assertIs(csilk.WorkflowNode, WorkflowNode)
        self.assertIs(csilk.WorkflowContext, WorkflowContext)
        self.assertIs(csilk.WorkflowData, WorkflowData)
        self.assertIs(csilk.WorkflowWorker, WorkflowWorker)
        self.assertIs(csilk.MQ, MQ)
        self.assertIs(csilk.MqContext, MqContext)
        self.assertIs(csilk.ASGIAdapter, ASGIAdapter)

        app = csilk.App()
        app.free()

    def test_optional_features_raise_not_implemented_in_web_core(self):
        lib = csilk_lib.get_bindings()
        if csilk_lib.has_symbol(lib, "csilk_ai_new"):
            self.skipTest("Running against Full build; optional features are enabled")

        app = App()
        ctx = Context.create_test_context()
        try:
            cases = [
                ("AI()", lambda: csilk.AI()),
                ("AI('openai')", lambda: AI("openai")),
                ("AIContext()", lambda: AIContext()),
                ("DBPool()", lambda: csilk.DBPool()),
                ("DBPool('sqlite', ':memory:')", lambda: DBPool("sqlite", ":memory:")),
                ("MQ()", lambda: csilk.MQ()),
                ("MQ(None)", lambda: MQ(None)),
                ("MqContext()", lambda: csilk.MqContext()),
                ("MqContext(None)", lambda: MqContext(None)),
                ("Workflow()", lambda: csilk.Workflow()),
                ("Workflow('test')", lambda: Workflow("test")),
                ("VectorDB()", lambda: csilk.VectorDB()),
                ("VectorDB('qdrant', 'http://127.0.0.1:6333')", lambda: VectorDB("qdrant", "http://127.0.0.1:6333")),
                ("Perm()", lambda: csilk.Perm()),
                ("Perm.init()", lambda: Perm.init()),
                ("app.mq", lambda: app.mq),
                ("app.admin_serve", lambda: app.admin_serve("/admin")),
                ("app.admin_serve_secure", lambda: app.admin_serve_secure("/admin", lambda c: None)),
                ("trace_middleware(ctx)", lambda: trace_middleware(ctx)),
            ]
            for label, fn in cases:
                with self.subTest(case=label):
                    with self.assertRaises(NotImplementedError):
                        fn()

            self.assertIsNone(ctx.trace_id)
            self.assertIsNone(ctx.span_id)
        finally:
            ctx.free_test_context()
            app.free()

    def test_core_routing_middlewares_asyncio_and_asgi(self):
        app = App()
        app.use(logger_middleware)
        app.use(cors(allow_origin="*"))

        @app.get("/hello")
        def hello(ctx: Context):
            ctx.json(200, {"ok": True})

        @app.get("/async")
        async def async_hello(ctx: Context):
            await asyncio.sleep(0.005)
            return {"async": True}

        api = app.group("/api")
        api.use(jwt_middleware("test-secret"))

        @api.get("/protected")
        def protected(ctx: Context):
            ctx.string(200, "secret-ok")

        async def mini_asgi(scope, receive, send):
            assert scope["type"] == "http"
            await send({
                "type": "http.response.start",
                "status": 200,
                "headers": [(b"content-type", b"application/json")],
            })
            await send({
                "type": "http.response.body",
                "body": json.dumps({"asgi": scope["path"]}).encode("utf-8"),
                "more_body": False,
            })

        app.mount_asgi("/asgi/*path", mini_asgi)

        port = _pick_free_port()
        t = threading.Thread(target=app.run, args=(port,), daemon=True)
        t.start()

        for _ in range(30):
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                    break
            except OSError:
                time.sleep(0.05)
        else:
            app.stop()
            t.join(timeout=2.0)
            app.free()
            self.fail("Server did not start in time")

        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
        try:
            with opener.open(f"http://127.0.0.1:{port}/hello") as resp:
                self.assertEqual(resp.status, 200)
                self.assertEqual(resp.headers.get("Access-Control-Allow-Origin"), "*")
                self.assertEqual(json.loads(resp.read().decode("utf-8")), {"ok": True})

            with opener.open(f"http://127.0.0.1:{port}/async") as resp:
                self.assertEqual(resp.status, 200)
                self.assertEqual(json.loads(resp.read().decode("utf-8")), {"async": True})

            with opener.open(f"http://127.0.0.1:{port}/asgi/ping") as resp:
                self.assertEqual(resp.status, 200)
                self.assertEqual(json.loads(resp.read().decode("utf-8")), {"asgi": "/asgi/ping"})

            with self.assertRaises(urllib.error.HTTPError) as cm:
                opener.open(f"http://127.0.0.1:{port}/api/protected")
            with cm.exception:
                self.assertEqual(cm.exception.code, 401)
        finally:
            app.stop()
            t.join(timeout=5.0)
            self.assertFalse(t.is_alive())
            app.free()


if __name__ == "__main__":
    unittest.main()
