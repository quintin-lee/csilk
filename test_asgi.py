import sys
import os
import asyncio

sys.path.insert(0, os.getcwd())
from csilk.app import App
from csilk.asgi import ASGIAdapter

async def dummy_asgi_app(scope, receive, send):
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

if __name__ == '__main__':
    test_asgi_bridge()
