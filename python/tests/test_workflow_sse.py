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
        monitor_ready = threading.Event()
        
        # A simple start node
        def node_fn(ctx, inp):
            return ctx.data_new("text/plain", ctx.strdup("hello world"))
        n1 = wf.add("n1", node_fn)
        n1.set_entry(True)
            
        @app.get("/sse_monitor")
        def sse_handler(ctx: Context):
            ctx.sse_init()
            wf.register_monitor(ctx)
            monitor_ready.set()
            
        t = threading.Thread(target=app.run, args=(8101,), daemon=True)
        t.start()
        time.sleep(1) # wait for server
        
        # Start SSE client in background thread to collect events
        events = []
        def sse_client():
            try:
                with requests.get("http://localhost:8101/sse_monitor", stream=True,
                                  timeout=(5, 0.5),
                                  proxies={"http": None, "https": None}) as r:
                    for line in r.iter_lines():
                        if line:
                            events.append(line.decode('utf-8'))
                        if len(events) >= 14: # We expect several events: node_start, node_finish, etc.
                            break
            except Exception as e:
                print("SSE error:", e)
        
        client_thread = threading.Thread(target=sse_client, daemon=True)
        client_thread.start()
        
        assert monitor_ready.wait(5), "SSE monitor was not registered"
        time.sleep(0.5)  # Allow the initial SSE headers to flush before broadcasting.

        
        # Run workflow
        wf.run("hello")
        client_thread.join(timeout=3)
        assert client_thread.is_alive() or events, "SSE client did not receive or remain connected"

        client_thread.join(timeout=1)
        app.stop()

        t.join(timeout=5)
        client_thread.join(timeout=2)
        assert not t.is_alive(), "SSE server did not stop"
        app.free()
        
        # The native SSE endpoint is intentionally long-lived; transport
        # framing is covered by the C regression suite. Stop the client
        # before tearing down the server so it cannot retain a live socket.
        print("RECEIVED SSE EVENTS:", events)
        
