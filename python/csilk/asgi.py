import asyncio
from typing import Dict, Any, Callable

class ASGIAdapter:
    def __init__(self, asgi_app: Callable):
        self.asgi_app = asgi_app

    async def __call__(self, ctx):
        ctx.set_async(True)

        scope = {
            "type": "http",
            "asgi": {"version": "3.0", "spec_version": "2.3"},
            "http_version": "1.1",
            "method": ctx.method.decode('utf-8') if isinstance(ctx.method, bytes) else ctx.method,
            "scheme": "http", # We assume http for now. TODO check tls
            "path": ctx.path.decode('utf-8') if isinstance(ctx.path, bytes) else ctx.path,
            "raw_path": ctx.path if isinstance(ctx.path, bytes) else ctx.path.encode('utf-8'),
            # URL query is usually stored as bytes in ASGI
            # csilk's context doesn't expose raw query string directly, so we can try getting it if it exists.
            "query_string": b"", 
            "headers": [(k.encode('utf-8'), v.encode('utf-8')) for k, v in ctx.headers.items()],
            "client": None,
            "server": None,
        }

        # Retrieve request body
        body_bytes = ctx.body_view or b""
        if isinstance(body_bytes, str):
            body_bytes = body_bytes.encode('utf-8')
        body_sent = False

        async def receive() -> Dict[str, Any]:
            nonlocal body_sent
            if not body_sent:
                body_sent = True
                return {
                    "type": "http.request",
                    "body": body_bytes,
                    "more_body": False,
                }
            # For simplicity, if we get called again after body is sent, we just disconnect.
            # In a real streaming scenario we'd yield chunks.
            return {"type": "http.disconnect"}

        async def send(message: Dict[str, Any]) -> None:
            if message["type"] == "http.response.start":
                status = message["status"]
                headers = message.get("headers", [])
                
                def _start(c):
                    for k, v in headers:
                        c.set_header(k.decode('utf-8'), v.decode('utf-8'))
                    c._asgi_status = status
                ctx.dispatch(_start)

            elif message["type"] == "http.response.body":
                body = message.get("body", b"")
                more_body = message.get("more_body", False)
                
                def _write(c):
                    if not hasattr(c, '_asgi_headers_sent'):
                        c.status(getattr(c, '_asgi_status', 200))
                        c._asgi_headers_sent = True
                    if body:
                        c.write(body)
                    if not more_body:
                        c.response_end()
                ctx.dispatch(_write)

        try:
            await self.asgi_app(scope, receive, send)
        except Exception as e:
            def _err(c):
                import traceback
                traceback.print_exc()
                if not hasattr(c, '_asgi_headers_sent'):
                    c.string(500, "Internal Server Error from ASGI App")
                c.response_end()
            ctx.dispatch(_err)
