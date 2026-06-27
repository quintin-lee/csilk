import unittest
import json
import ctypes
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

    def test_tool_discovery_registration(self):
        wf = Workflow("tool_test")
        
        # 1. Register static tool
        def my_static_tool(args):
            return {"result": f"static_ok: {args}"}
            
        wf.register_tool(
            name="static_tool",
            description="a static test tool",
            parameters_json='{"type":"object"}',
            tool_fn=my_static_tool
        )
        
        # 2. Register dynamic discovery callback
        def my_discovery(static_tools):
            self.assertEqual(len(static_tools), 1)
            self.assertEqual(static_tools[0]["name"], "static_tool")
            self.assertEqual(static_tools[0]["description"], "a static test tool")
            self.assertEqual(static_tools[0]["parameters_json"], '{"type":"object"}')
            
            def my_dynamic_tool(args):
                return {"result": f"dynamic_ok: {args}"}
                
            return [
                {
                    "name": "dynamic_tool",
                    "description": "a dynamic test tool",
                    "parameters_json": '{"type":"string"}',
                    "fn": my_dynamic_tool
                }
            ]
            
        wf.set_tool_discovery(my_discovery)
        
        # 3. Verify it is set
        self.assertIsNotNone(wf._discovery_cb)
        
        # 4. Construct a mock static tool array to pass to discovery callback
        from csilk.lib import CsilkWfToolEntry
        static_array = (CsilkWfToolEntry * 1)()
        static_array[0].name = b"static_tool"
        static_array[0].description = b"a static test tool"
        static_array[0].parameters_json = b'{"type":"object"}'
        static_array[0].fn = None
        static_array[0].user_data = None
        
        discovered_out = ctypes.c_void_p()
        discovered_count_out = ctypes.c_size_t()
        
        # Invoke callback directly
        ret = wf._discovery_cb(
            wf._wf,
            ctypes.cast(static_array, ctypes.c_void_p),
            1,
            ctypes.byref(discovered_out),
            ctypes.byref(discovered_count_out),
            None
        )
        
        self.assertEqual(ret, 0)
        self.assertEqual(discovered_count_out.value, 1)
        self.assertIsNotNone(discovered_out.value)
        
        # Cast the returned memory to verify the dynamic tool properties
        discovered_array = ctypes.cast(discovered_out, ctypes.POINTER(CsilkWfToolEntry * 1))
        entry = discovered_array.contents[0]
        self.assertEqual(entry.name, b"dynamic_tool")
        self.assertEqual(entry.description, b"a dynamic test tool")
        self.assertEqual(entry.parameters_json, b'{"type":"string"}')
        self.assertIsNotNone(entry.fn)
        
        # 5. Call the dynamic tool wrapper function pointer directly
        from csilk.lib import CsilkWfToolFn
        fn_callable = CsilkWfToolFn(entry.fn)
        res_ptr = fn_callable(b'{"arg": 123}', None)
        self.assertIsNotNone(res_ptr)
        
        res_val = ctypes.string_at(res_ptr).decode('utf-8')
        res_json = json.loads(res_val)
        self.assertEqual(res_json, {"result": "dynamic_ok: {\"arg\": 123}"})
        
        # Free the string returned by CsilkWfToolFn
        wf._lib.csilk_free(res_ptr)
        
        # Free the allocated entries and array to avoid leaks
        raw_ptrs = ctypes.cast(discovered_out, ctypes.POINTER(ctypes.c_void_p * 5)).contents
        if raw_ptrs[0]:
            wf._lib.csilk_free(raw_ptrs[0])
        if raw_ptrs[1]:
            wf._lib.csilk_free(raw_ptrs[1])
        if raw_ptrs[2]:
            wf._lib.csilk_free(raw_ptrs[2])
        wf._lib.csilk_free(discovered_out)
        
        wf.free()

    def test_workflow_configurations(self):
        wf = Workflow("config_test")
        self.assertIsNotNone(wf)
        
        # Test workflow settings
        wf.set_ttl(3600)
        wf.set_budget(50000)
        
        # Add a node and configure it
        def handler(ctx, inp):
            return None
        n1 = wf.add("node1", handler)
        self.assertIsNotNone(n1)
        
        # Test node configurations
        n1.set_join_policy("OR")
        n1.set_join_policy("AND")
        n1.set_timeout(5000)
        n1.set_retry(3, 1000)
        n1.set_remote(True)
        
        # Add another node to test bind/on/on_loop/on_error/to_mermaid
        n2 = wf.add("node2", handler)
        n1.bind(n2)
        n1.on("custom_cond", n2)
        n1.on_loop("loop_cond", n2)
        n1.on_error(n2)
        
        # Test to_mermaid
        mermaid_graph = wf.to_mermaid()
        self.assertIsNotNone(mermaid_graph)
        self.assertTrue("graph TD" in mermaid_graph)
        self.assertTrue("node1" in mermaid_graph)
        self.assertTrue("node2" in mermaid_graph)
        self.assertTrue("custom_cond" in mermaid_graph)
        self.assertTrue("loop_cond" in mermaid_graph)

        # Test _repr_html_
        repr_html = wf._repr_html_()
        self.assertIsNotNone(repr_html)
        self.assertTrue("mermaid" in repr_html)
        self.assertTrue(mermaid_graph in repr_html)
        
        wf.free()


if __name__ == '__main__':
    unittest.main()


