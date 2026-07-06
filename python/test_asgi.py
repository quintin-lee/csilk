import sys
import os
import asyncio

sys.path.insert(0, os.getcwd())
from csilk.app import App
from csilk.asgi import ASGIAdapter

async def dummy_asgi_app(scope, receive, send):
    if scope['type'] == 'lifespan':
        while True:
            message = await receive()
            if message['type'] == 'lifespan.startup':
                await send({'type': 'lifespan.startup.complete'})
            elif message['type'] == 'lifespan.shutdown':
                await send({'type': 'lifespan.shutdown.complete'})
                return
    assert scope['type'] == 'http'
    assert scope['method'] == 'GET' or scope['method'] == 'POST'
    
    # Send headers
    await send({
        'type': 'http.response.start',
        'status': 200,
        'headers': [
            (b'content-type', b'text/plain'),
            (b'x-custom-asgi', b'true')
        ]
    })
    
    # Read body if it's a POST
    body = b""
    if scope['method'] == 'POST':
        more_body = True
        while more_body:
            message = await receive()
            body += message.get('body', b'')
            more_body = message.get('more_body', False)

    # Send response body
    response_text = f"Hello from ASGI! method={scope['method']} path={scope['path']}"
    if body:
        response_text += f" body={body.decode('utf-8')}"

    await send({
        'type': 'http.response.body',
        'body': response_text.encode('utf-8'),
        'more_body': False
    })

def test_asgi_bridge():
    import threading
    import time
    import urllib.request

    app = App(asgi_app=dummy_asgi_app)
    
    # Also add a native route
    @app.get("/")
    def home(ctx):
        ctx.string(200, "Native route")

    @app.get("/zero-copy")
    def zero_copy_test(ctx):
        # We simulate a zero-copy buffer (must be writable for from_buffer)
        mv = memoryview(bytearray(b"Zero-Copy Response Data"))
        ctx.status_code = 200
        ctx.add_header("Content-Type", "text/plain")
        ctx.write(mv)
        ctx.response_end()
        
    # Start server in thread
    t = threading.Thread(target=app.run, kwargs={"port": 8081})
    t.start()
    time.sleep(0.5)
    
    proxy_handler = urllib.request.ProxyHandler({})
    opener = urllib.request.build_opener(proxy_handler)
    urllib.request.install_opener(opener)

    try:
        # Test native
        req = urllib.request.Request("http://127.0.0.1:8081/")
        with urllib.request.urlopen(req) as response:
            assert response.read() == b"Native route"

        # Test zero-copy write
        req = urllib.request.Request("http://127.0.0.1:8081/zero-copy")
        with urllib.request.urlopen(req) as response:
            assert response.read() == b"Zero-Copy Response Data"
            
        # Test ASGI GET
        req = urllib.request.Request("http://127.0.0.1:8081/test")
        with urllib.request.urlopen(req) as response:
            assert response.getheader("x-custom-asgi") == "true"
            assert response.read() == b"Hello from ASGI! method=GET path=/test"
            
        # Test ASGI POST
        req = urllib.request.Request("http://127.0.0.1:8081/submit", data=b"my_test_data")
        with urllib.request.urlopen(req) as response:
            assert response.getheader("x-custom-asgi") == "true"
            assert response.read() == b"Hello from ASGI! method=POST path=/submit body=my_test_data"
            
        print("ASGI Adapter tests passed successfully!")
    finally:
        app.stop()
        t.join()
        app.free()

def test_asgi_websocket():
        # A simple ASGI websocket echo app
        async def ws_echo_app(scope, receive, send):
            if scope["type"] == "websocket":
                while True:
                    message = await receive()
                    if message["type"] == "websocket.connect":
                        await send({"type": "websocket.accept"})
                    elif message["type"] == "websocket.receive":
                        text = message.get("text")
                        if text:
                            await send({"type": "websocket.send", "text": "ECHO: " + text})
                        else:
                            await send({"type": "websocket.send", "bytes": message.get("bytes")})
                    elif message["type"] == "websocket.disconnect":
                        break
            else:
                await send({
                    "type": "http.response.start",
                    "status": 404,
                })
                await send({"type": "http.response.body", "body": b""})

        # We can test the adapter directly by mocking the context
        adapter = ASGIAdapter(ws_echo_app)
        
        class MockWSContext:
            def __init__(self):
                self.is_websocket = True
                self.method = b"GET"
                self.path = b"/ws"
                self.headers = {"host": "localhost"}
                self.body_view = b""
                self.dispatched = []
                self.ws_messages_sent = []
                self.ws_cb = None

            def ws_handshake(self):
                self.ws_messages_sent.append("handshake")

            def ws_send(self, payload, opcode):
                self.ws_messages_sent.append(payload)

            def set_on_ws_message(self, cb):
                self.ws_cb = cb
                
            def dispatch(self, cb):
                cb()

        ctx = MockWSContext()
        
        async def run_test():
            # Start adapter in a task
            task = asyncio.create_task(adapter(ctx))
            # Yield to let it process connect
            await asyncio.sleep(0.01)
            
            # Simulate a message from C
            if ctx.ws_cb:
                ctx.ws_cb(ctx, b"Hello", 0x1)
                
            await asyncio.sleep(0.01)
            
            # Simulate close
            if ctx.ws_cb:
                ctx.ws_cb(ctx, b"", 0x8)
                
            await task
            
        asyncio.run(run_test())
        assert "handshake" in ctx.ws_messages_sent
        assert "ECHO: Hello" in ctx.ws_messages_sent

if __name__ == '__main__':
    test_asgi_bridge()
    test_asgi_websocket()
    print("All ASGI tests (HTTP + WebSocket) passed!")
