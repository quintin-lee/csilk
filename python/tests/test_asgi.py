import pytest
import threading
import time
import httpx
from fastapi import FastAPI, Request
from csilk.app import App

def _run_app_in_thread(app, port):
    """Run app.run(port) in a daemon thread, capturing any exception.

    Returns (thread, error_box) where error_box['value'] holds the raised
    exception (e.g. UnboundLocalError) or None after a clean shutdown.
    """
    error_box = {"value": None}

    def target():
        try:
            app.run(port)
        except BaseException as exc:  # surfaced by the caller, not the excepthook
            error_box["value"] = exc

    t = threading.Thread(target=target, daemon=True)
    t.start()
    return t, error_box


def _wait_until_serving(port, timeout=5.0):
    """Poll until the server answers on `port`; return the first response.

    Asserts real startup instead of relying on a fixed sleep: if run() fails
    before binding (the UnboundLocalError regression), this raises.
    """
    deadline = time.monotonic() + timeout
    last_exc = None
    with httpx.Client(base_url=f"http://127.0.0.1:{port}", trust_env=False) as client:
        while time.monotonic() < deadline:
            try:
                return client.get("/api/hello?name=fastapi", timeout=1.0)
            except httpx.HTTPError as exc:
                last_exc = exc
                time.sleep(0.05)
    raise AssertionError(f"server on port {port} never became ready: {last_exc!r}")


def test_fastapi_basic():
    # Setup FastAPI app
    fastapp = FastAPI()

    @fastapp.get("/api/hello")
    def hello(request: Request, name: str = "world"):
        return {
            "message": f"Hello, {name}!",
            "query_string": request.url.query,
            "client_host": request.client.host if request.client else None
        }

    @fastapp.post("/api/echo")
    async def echo(request: Request):
        body = await request.body()
        return {"body": body.decode("utf-8")}

    # Mount in C-Silk App
    app = App(asgi_app=fastapp)

    # Start server
    t = threading.Thread(target=app.run, args=(8087,), daemon=True)
    t.start()
    time.sleep(1) # wait for startup

    try:
        with httpx.Client(base_url="http://127.0.0.1:8087", trust_env=False) as client:
            # 1. Test basic GET with query params
            r1 = client.get("/api/hello?name=fastapi")
            assert r1.status_code == 200, f"Failed: {r1.text}"
            data1 = r1.json()
            assert data1["message"] == "Hello, fastapi!"
            assert data1["query_string"] == "name=fastapi"

            # 2. Test POST with body
            r2 = client.post("/api/echo", content="hello world")
            assert r2.status_code == 200, f"Failed: {r2.text}"
            data2 = r2.json()
            assert data2["body"] == "hello world"

    finally:
        app.stop()
        t.join(timeout=2.0)
        app.free()


def test_asgi_run_actually_starts_server():
    """Regression: run() must start the server, not fail before binding.

    A function-level `import asyncio` only in the finally-block made asyncio
    a local name and raised UnboundLocalError during ASGI startup; the run
    thread died silently, later stop() hit a never-run server and segfaulted
    in uv_async_send. Asserts: no exception in the run thread, the server
    actually serves requests, and a clean stop afterwards.
    """
    fastapp = FastAPI()

    @fastapp.get("/api/hello")
    def hello(name: str = "world"):
        return {"message": f"Hello, {name}!"}

    app = App(asgi_app=fastapp)
    t, error_box = _run_app_in_thread(app, 8088)

    try:
        resp = _wait_until_serving(8088)
        assert resp.status_code == 200, f"Failed: {resp.text}"
        assert resp.json()["message"] == "Hello, fastapi!"
        # run() blocks serving: the thread must still be alive at this point
        assert t.is_alive(), "run thread exited while the server should be running"
        assert error_box["value"] is None, f"run thread raised: {error_box['value']!r}"
    finally:
        app.stop()
        t.join(timeout=2.0)
        app.free()

    assert not t.is_alive(), "run thread did not exit after stop()"
    assert error_box["value"] is None, f"run thread raised: {error_box['value']!r}"
