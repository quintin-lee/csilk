import pytest
import asyncio
import json
import requests
import threading
import time
from csilk.app import App
from csilk.context import Context
from csilk.workflow import Workflow, WorkflowNode, WorkflowData

class TestWorkflowSSE:
    def test_workflow_sse_monitor(self):
        app = App()
        wf = Workflow("sse_test")
        
        # A simple start node
        def node_fn(ctx, inp):
            return ctx.data_new("text/plain", ctx.strdup("hello world"))
        n1 = wf.add("n1", node_fn)
            
        @app.get("/sse_monitor")
        async def sse_handler(ctx: Context):
            ctx.sse_init()
            wf.register_monitor(ctx)
            while True:
                await asyncio.sleep(1)
            
        t = threading.Thread(target=app.run, args=(8101,), daemon=True)
        t.start()
        time.sleep(1) # wait for server
        
        # Start SSE client in background thread to collect events
        events = []
        def sse_client():
            try:
                r = requests.get("http://localhost:8101/sse_monitor", stream=True, proxies={"http": None, "https": None})
                for line in r.iter_lines():
                    if line:
                        events.append(line.decode('utf-8'))
                    if len(events) >= 14: # We expect several events: node_start, node_finish, etc.
                        break
            except Exception as e:
                print("SSE error:", e)
        
        client_thread = threading.Thread(target=sse_client, daemon=True)
        client_thread.start()
        
        time.sleep(1) # Let SSE connect and register
        
        # Run workflow
        wf.run("start")
        
        client_thread.join(timeout=3)
        
        app.stop()
        t.join(timeout=2)
        app.free()
        
        # Check that we received SSE events formatted properly
        print("RECEIVED SSE EVENTS:", events)
        assert any(e.startswith("event: node_start") for e in events)
        assert any(e.startswith("event: node_finish") for e in events)
        assert any(e.startswith("event: workflow_start") for e in events)
        
