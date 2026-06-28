import pytest
import threading
import time
import httpx
from fastapi import FastAPI, Request
from csilk.app import App

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
