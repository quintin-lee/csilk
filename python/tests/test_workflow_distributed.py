import pytest
import threading
import time
import json
from csilk.app import App
from csilk.workflow import Workflow, WorkflowData

class TestWorkflowDistributed:
    def test_workflow_remote_node(self):
        app = App()
        wf = Workflow("dist_test")
        
        # A normal start node
        def n1_fn(ctx, inp):
            return ctx.data_new("text/plain", ctx.strdup("local_start"))
        n1 = wf.add("n1", n1_fn)
        
        # A remote node (handler won't be called directly)
        n2 = wf.add("n2")
        n2.set_remote()
        
        # A final node
        def n3_fn(ctx, inp):
            return ctx.data_new("text/plain", ctx.strdup(inp.value + " -> final"))
        n3 = wf.add("n3", n3_fn)
        
        n1.bind(n2)
        n2.bind(n3)
        # Enable distributed execution (connects workflow to MQ)
        import tempfile
        wal_dir = tempfile.mkdtemp()
        wf.set_persistence(wal_dir)
        wf.enable_distributed(app.mq)
        
        # Worker to process tasks from csilk.wf.tasks
        from csilk.worker import WorkflowWorker
        worker = WorkflowWorker(app.mq)
        
        def n2_remote_handler(input_data):
            return input_data + " -> remote_processed"
            
        worker.register("n2", n2_remote_handler)
        worker.start()
            
        t = threading.Thread(target=app.run, args=(0,), daemon=True)
        t.start()
        time.sleep(1) # wait for server and MQ
        
        # Run workflow synchronously ? 
        # Wait, run() is asynchronous in C if it suspends. We should use a callback.
        result_data = None
        done_event = threading.Event()
        def on_done(res):
            nonlocal result_data
            if res:
                result_data = res.value
            done_event.set()
            
        wf.run("start", callback=on_done)
        
        done = done_event.wait(timeout=5)
        
        app.stop()
        t.join(timeout=2)
        app.free()
        
        assert done, "Workflow timed out waiting for remote execution"
        assert result_data == "local_start -> remote_processed -> final"

