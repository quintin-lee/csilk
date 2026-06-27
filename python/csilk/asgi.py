import asyncio
from typing import Dict, Any, Callable

class ASGIAdapter:
    def __init__(self, asgi_app: Callable):
        self.asgi_app = asgi_app

    async def __call__(self, ctx):
        ctx.is_async = True

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
                
                def _start():
                    for k, v in headers:
                        ctx.set_header(k.decode('utf-8'), v.decode('utf-8'))
                    ctx._asgi_status = status
                ctx.dispatch(_start)

            elif message["type"] == "http.response.body":
                body = message.get("body", b"")
                more_body = message.get("more_body", False)
                
                def _write():
                    if not hasattr(ctx, '_asgi_headers_sent'):
                        ctx.status_code = getattr(ctx, '_asgi_status', 200)
                        ctx._asgi_headers_sent = True
                    if body:
                        ctx.write(body)
                    if not more_body:
                        ctx.response_end()
                ctx.dispatch(_write)

        try:
            await self.asgi_app(scope, receive, send)
        except Exception as e:
            def _err():
                import traceback
                traceback.print_exc()
                if not hasattr(ctx, '_asgi_headers_sent'):
                    ctx.string(500, "Internal Server Error from ASGI App")
                else:
                    ctx.response_end()
            ctx.dispatch(_err)

class LifespanManager:
    def __init__(self, app):
        self.app = app
        self.receive_queue = asyncio.Queue()
        self.send_queue = asyncio.Queue()
        self.task = None

    async def startup(self):
        scope = {
            'type': 'lifespan',
            'asgi': {'version': '3.0', 'spec_version': '2.0'},
        }
        self.task = asyncio.create_task(self.app(scope, self.receive_queue.get, self.send_queue.put))
        await self.receive_queue.put({'type': 'lifespan.startup'})
        
        get_task = asyncio.create_task(self.send_queue.get())
        done, pending = await asyncio.wait([self.task, get_task], return_when=asyncio.FIRST_COMPLETED)
        
        if get_task in done:
            message = get_task.result()
            if message['type'] == 'lifespan.startup.failed':
                raise RuntimeError(f"ASGI startup failed: {message.get('message', '')}")
            elif message['type'] == 'lifespan.startup.complete':
                return
            else:
                raise RuntimeError(f"Unexpected ASGI message: {message['type']}")
        else:
            # The app task completed or crashed before sending a message.
            # This usually means the app doesn't support lifespan (e.g. simple functions).
            get_task.cancel()
            return

    async def shutdown(self):
        if self.task is None or self.task.done():
            return
        await self.receive_queue.put({'type': 'lifespan.shutdown'})
        
        get_task = asyncio.create_task(self.send_queue.get())
        done, pending = await asyncio.wait([self.task, get_task], return_when=asyncio.FIRST_COMPLETED)
        
        if get_task in done:
            message = get_task.result()
            if message['type'] == 'lifespan.shutdown.failed':
                raise RuntimeError(f"ASGI shutdown failed: {message.get('message', '')}")
            elif message['type'] == 'lifespan.shutdown.complete':
                pass
        
        # Wait for the task to finish
        try:
            await asyncio.wait_for(self.task, timeout=2.0)
        except asyncio.TimeoutError:
            pass
