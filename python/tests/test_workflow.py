import unittest
import json
import threading
import time
import sys
import os

sys.path.insert(0, os.path.abspath(os.path.join(os.path.dirname(__file__), '..')))
from csilk import App
from csilk.workflow import Workflow, WorkflowContext, WorkflowData, WorkflowNode

class TestWorkflow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # Start a dummy app on a separate port to run the libuv default loop
        cls.app = App()
        def run_app():
            cls.app.run(8083)
        cls.thread = threading.Thread(target=run_app)
        cls.thread.daemon = True
        cls.thread.start()
        
        # Wait for the loop to start
        time.sleep(0.5)

    @classmethod
    def tearDownClass(cls):
        cls.app.stop()
        cls.thread.join(timeout=1.0)

    def test_workflow_lifecycle(self):
        wf = Workflow("lifecycle_test")
        self.assertIsNotNone(wf)
        
        n1 = wf.add("node1", lambda ctx, inp: None)
        n2 = wf.add("node2", lambda ctx, inp: None)
        self.assertIsNotNone(n1)
        self.assertIsNotNone(n2)
        
        # Get node
        got1 = wf.get_node("node1")
        self.assertIsNotNone(got1)
        self.assertEqual(got1._ptr, n1._ptr)
        
        # Free
        wf.free()

    def test_workflow_execution(self):
        wf = Workflow("execution_test")
        
        def step1(ctx: WorkflowContext, inp: WorkflowData):
            val = inp.value
            return ctx.data_new("text/plain", ctx.strdup(val.upper()))

        def step2(ctx: WorkflowContext, inp: WorkflowData):
            val = inp.value
            return ctx.data_new("text/plain", ctx.strdup(val + "!!!"))

        n1 = wf.add("n1", step1)
        n2 = wf.add("n2", step2)
        
        n1.bind(n2)
        
        results = []
        event = threading.Event()
        
        def on_complete(res):
            if res:
                results.append(res.value)
            event.set()

        exec_id = wf.run("hello", callback=on_complete)
        self.assertIsNotNone(exec_id)
        
        # Wait for the workflow to complete
        completed = event.wait(timeout=2.0)
        self.assertTrue(completed)
        self.assertEqual(results, ["HELLO!!!"])

    def test_workflow_dynamic_routing(self):
        wf = Workflow("routing_test")

        def start_node(ctx: WorkflowContext, inp: WorkflowData):
            return ctx.data_new("text/plain", ctx.strdup(inp.value))

        def left_node(ctx: WorkflowContext, inp: WorkflowData):
            return ctx.data_new("text/plain", ctx.strdup("left"))

        def right_node(ctx: WorkflowContext, inp: WorkflowData):
            return ctx.data_new("text/plain", ctx.strdup("right"))

        n_start = wf.add("start", start_node)
        n_left = wf.add("left_node", left_node)
        n_right = wf.add("right_node", right_node)

        # Dynamic router
        def my_router(inp: WorkflowData):
            val = inp.value
            if val == "go_left":
                return "left_node"
            return "right_node"

        n_start.route(my_router)

        # Test case 1: route left
        results = []
        event = threading.Event()
        def cb1(res):
            results.append(res.value)
            event.set()
        
        wf.run("go_left", callback=cb1)
        self.assertTrue(event.wait(timeout=2.0))
        self.assertEqual(results, ["left"])

        # Test case 2: route right
        results2 = []
        event2 = threading.Event()
        def cb2(res):
            results2.append(res.value)
            event2.set()
        
        wf.run("go_right", callback=cb2)
        self.assertTrue(event2.wait(timeout=2.0))
        self.assertEqual(results2, ["right"])

    def test_workflow_declarative(self):
        # 1. Register a python handler globally
        def dec_handler(ctx, inp):
            return ctx.data_new_string(inp.value.upper() + " FROM DECA")

        Workflow.register_handler("upper_deca", dec_handler)

        # 2. Create workflow from JSON string
        wf_json = """{
            "name": "declarative_test",
            "steps": [
                {
                    "id": "step_one",
                    "type": "handler",
                    "handler": "upper_deca",
                    "entry": true
                }
            ]
        }"""
        wf = Workflow.from_json(wf_json)
        self.assertIsNotNone(wf)

        # 3. Run and verify
        results = []
        event = threading.Event()
        def cb(res):
            if res:
                results.append(res.value)
            event.set()

        wf.run("hello", callback=cb)
        self.assertTrue(event.wait(timeout=2.0))
        self.assertEqual(results, ["HELLO FROM DECA"])

    def test_workflow_traced_and_interactive(self):
        # 1. Test run_traced and trace_to_json
        wf = Workflow("traced_test")
        
        def step_upper(ctx, inp):
            return ctx.data_new_string(inp.value.upper())
            
        n = wf.add("node1", step_upper)
        n.set_entry(True)
        
        results = []
        event = threading.Event()
        trace_ptrs = []
        
        def traced_cb(res, trace_ptr):
            if res:
                results.append(res.value)
            if trace_ptr:
                trace_ptrs.append(trace_ptr)
            event.set()
            
        wf.run_traced("hello", callback=traced_cb)
        self.assertTrue(event.wait(timeout=2.0))
        self.assertEqual(results, ["HELLO"])
        self.assertEqual(len(trace_ptrs), 1)
        
        # Convert trace to JSON
        json_str = wf.trace_to_json(trace_ptrs[0])
        self.assertIsNotNone(json_str)
        trace_data = json.loads(json_str)
        self.assertIn("exec_id", trace_data)
        self.assertIn("nodes", trace_data)
        
        # Free trace and workflow
        wf.trace_free(trace_ptrs[0])
        wf.free()

        # 2. Test interactive pause, resume, and signal_continue
        wf2 = Workflow("interactive_test")
        
        import tempfile
        import shutil
        wal_dir = tempfile.mkdtemp()
        wf2.set_persistence(wal_dir)
        
        def step1(ctx, inp):
            return ctx.data_new_string(inp.value + "-step1")
            
        def step2(ctx, inp):
            return ctx.data_new_string(inp.value + "-step2")
            
        n1 = wf2.add("node1", step1)
        n1.set_entry(True)
        n1.set_interactive(True)
        
        n2 = wf2.add("node2", step2)
        n1.bind(n2)
        
        results_int = []
        event_int = threading.Event()
        def cb_int(res):
            if res:
                results_int.append(res.value)
            event_int.set()
            
        # Run workflow: should pause at node1 immediately
        exec_id = wf2.run("hello", callback=cb_int)
        self.assertIsNotNone(exec_id)
        
        # Verify it hasn't completed
        self.assertFalse(event_int.wait(timeout=0.2))
        self.assertEqual(len(results_int), 0)
        
        # Simulating a reload/resume of the paused context
        wf2.resume(exec_id)
        
        # Verify it still hasn't completed (paused at node1 again)
        self.assertFalse(event_int.wait(timeout=0.2))
        
        # Signal continue to approve the interactive node and run to completion
        wf2.signal_continue(exec_id, "approved-hello", callback=cb_int)
        
        self.assertTrue(event_int.wait(timeout=2.0))
        self.assertEqual(results_int, ["approved-hello-step1-step2"])
        
        # Free resources
        wf2.free()
        shutil.rmtree(wal_dir)

if __name__ == '__main__':
    unittest.main()
