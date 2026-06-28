"""AI Workflow orchestration engine.

Provides a Pythonic interface to the csilk C workflow engine, supporting
directed acyclic graphs (DAGs) of AI, vector search, and custom handler
nodes with conditional routing, retries, tool calling, and tracing.
"""

import ctypes
import json
from csilk.lib import (
    get_bindings, CsilkAppPtr, CsilkCtxPtr,
    CsilkData, CsilkDataPtr, CsilkAiConfig, CsilkVectorSearchConfig,
    CsilkWfToolEntry, CsilkWfHandler, CsilkWfRouter,
    CsilkWfToolFn, CsilkWfRunCallback, CsilkWfRunTracedCallback,
    CsilkAiMeta,
)


class WorkflowData:
    """Wrapper around a ``CsilkData`` value flowing through a workflow graph.

    Provides read-only access to the data's type, value, and optional
    AI metadata (model, token counts).
    """

    def __init__(self, data_ptr):
        self._ptr = data_ptr

    @property
    def type(self) -> str:
        """MIME-type of the data (e.g. ``"text/plain"``, ``"application/json"``)."""
        if self._ptr and self._ptr.contents.type:
            return self._ptr.contents.type.decode('utf-8')
        return ""

    @property
    def value(self):
        """The data value — decoded as ``str`` for text/json, raw pointer otherwise."""
        if not self._ptr or not self._ptr.contents.value:
            return None
        t = self.type
        if t in ("text/plain", "application/json"):
            return ctypes.string_at(self._ptr.contents.value).decode('utf-8')
        return self._ptr.contents.value

    @property
    def meta(self):
        """Optional AI metadata dict (``model``, ``prompt_tokens``, ``completion_tokens``)."""
        if not self._ptr or not self._ptr.contents.meta:
            return None
        meta_ptr = ctypes.cast(self._ptr.contents.meta, ctypes.POINTER(CsilkAiMeta))
        if meta_ptr:
            return {
                "model": meta_ptr.contents.model.decode('utf-8') if meta_ptr.contents.model else "",
                "prompt_tokens": meta_ptr.contents.prompt_tokens,
                "completion_tokens": meta_ptr.contents.completion_tokens,
            }
        return None


class WorkflowContext:
    """Arena-allocated memory context for a workflow execution.

    Use to allocate strings and data that live for the duration of a
    single node handler invocation.
    """

    def __init__(self, ctx_ptr):
        self._ptr = ctx_ptr
        self._lib = get_bindings()

    def strdup(self, s: str):
        """Duplicate a Python string into arena-allocated memory."""
        res = self._lib.csilk_wf_strdup(self._ptr, s.encode('utf-8'))
        return res

    def data_new(self, data_type: str, value_ptr):
        """Create a ``CsilkData`` object within the arena."""
        return self._lib.csilk_wf_data_new(self._ptr, data_type.encode('utf-8'), value_ptr)

    def data_new_string(self, value: str, data_type: str = "text/plain"):
        """Create a ``CsilkData`` from a Python string."""
        c_str = self.strdup(value)
        return self.data_new(data_type, c_str)

    def alloc(self, size: int):
        """Allocate raw arena memory."""
        return self._lib.csilk_wf_alloc(self._ptr, size)


class WorkflowNode:
    def __init__(self, node_ptr):
        self._ptr = node_ptr
        self._lib = get_bindings()

    def set_entry(self, is_entry=True):
        self._lib.csilk_wf_node_set_entry(self._ptr, 1 if is_entry else 0)

    def set_entry(self, is_entry=True):
        """Mark this node as an entry (starting) node."""
        self._lib.csilk_wf_node_set_entry(self._ptr, 1 if is_entry else 0)

    def set_join_policy(self, policy):
        """Set the join policy for this node.

        Args:
            policy: ``"AND"`` (wait for all predecessors) or ``"OR"``
                (proceed when any predecessor completes).
        """
        p = 0 if policy.upper() == "AND" else 1
        self._lib.csilk_wf_node_set_join(self._ptr, p)

    def set_interactive(self, is_interactive=True):
        """Mark this node as requiring human interaction."""
        self._lib.csilk_wf_node_set_interactive(self._ptr, 1 if is_interactive else 0)

    def set_schema(self, schema_json):
        """Set a JSON schema describing this node's expected input."""
        s = schema_json.encode('utf-8') if schema_json else None
        self._lib.csilk_wf_node_set_schema(self._ptr, s)

    def set_timeout(self, timeout_ms):
        """Set a timeout (ms) for this node's execution."""
        self._lib.csilk_wf_node_set_timeout(self._ptr, timeout_ms)

    def set_retry(self, max_retries, retry_delay_ms):
        """Configure automatic retries on failure."""
        self._lib.csilk_wf_node_set_retry(self._ptr, max_retries, retry_delay_ms)

    def set_remote(self, is_remote=True):
        """Mark this node for remote (distributed) execution."""
        self._lib.csilk_wf_node_set_remote(self._ptr, 1 if is_remote else 0)

    def bind(self, target_node):
        """Create a default edge from this node to *target_node*."""
        self._lib.csilk_wf_bind(self._ptr, target_node._ptr)

    def on(self, condition, target_node):
        """Create a conditional edge — route to *target_node* when *condition* matches."""
        self._lib.csilk_wf_on(self._ptr, condition.encode('utf-8'), target_node._ptr)

    def on_loop(self, condition, target_node):
        """Create a looping edge — return to *target_node* when *condition* matches."""
        self._lib.csilk_wf_on_loop(self._ptr, condition.encode('utf-8'), target_node._ptr)

    def on_error(self, target_node):
        """Create an error edge — route to *target_node* when this node fails."""
        self._lib.csilk_wf_on_error(self._ptr, target_node._ptr)

    def route(self, router_fn):
        """Register a dynamic router function for this node.

        The router receives the input data and returns the name of the
        next node to route to.
        """
        @CsilkWfRouter
        def wrapper(input_ptr):
            res = router_fn(WorkflowData(input_ptr))
            # Store in self to prevent GC before C code uses it
            self._last_router_res = res.encode('utf-8') if res else None
            return self._last_router_res
        self._router_wrapper = wrapper
        self._lib.csilk_wf_route(self._ptr, wrapper)

class Workflow:
    """A directed acyclic graph (DAG) of AI, vector search, and custom
    handler nodes.

    Usage::

        wf = Workflow("my-pipeline")
        node1 = wf.add("step1", lambda ctx, data: ctx.data_new_string("hello"))
        node2 = wf.add_ai("step2", model="gpt-4")
        node1.bind(node2)
        wf.run("start", callback=lambda res: print(res))
    """

    def __init__(self, name_or_ptr):
        """Create a new workflow or wrap an existing C pointer.

        Args:
            name_or_ptr: Workflow name (str) for a new workflow, or an
                opaque C pointer to wrap an existing one.
        Raises:
            RuntimeError: If the workflow cannot be created.
        """
        self._lib = get_bindings()
        self._handlers = []      # To prevent GC of handler callbacks
        self._tools = []         # To prevent GC of tool callbacks
        self._discovery_cb = None
        self._run_callbacks = [] # To prevent GC of run callbacks

        if isinstance(name_or_ptr, str):
            self._wf = self._lib.csilk_wf_new(name_or_ptr.encode('utf-8'))
            self._owned = True
        else:
            self._wf = name_or_ptr
            self._owned = False

        if not self._wf:
            raise RuntimeError("Failed to create workflow instance")

    def free(self):
        """Release the workflow and all associated resources."""
        if self._wf and self._owned:
            self._lib.csilk_wf_free(self._wf)
            self._wf = None

    def __del__(self):
        self.free()

    _global_handlers = []

    @classmethod
    def register_handler(cls, name, handler_fn):
        """Globally register a handler function by name (for declarative workflows)."""
        lib = get_bindings()
        @CsilkWfHandler
        def wrapper(ctx_ptr, input_ptr, user_data):
            try:
                res_ptr = handler_fn(WorkflowContext(ctx_ptr), WorkflowData(input_ptr))
                if res_ptr:
                    return ctypes.cast(res_ptr, ctypes.c_void_p).value
            except Exception as e:
                import traceback
                traceback.print_exc()
            return None
        cls._global_handlers.append(wrapper)
        lib.csilk_wf_register_handler(name.encode('utf-8'), wrapper)

    @classmethod
    def load_yaml(cls, path):
        """Load a workflow definition from a YAML file.

        Returns:
            A new ``Workflow`` instance, or ``None`` on failure.
        """
        lib = get_bindings()
        ptr = lib.csilk_wf_load_yaml(path.encode('utf-8'))
        return cls(ptr) if ptr else None

    @classmethod
    def from_json(cls, json_str):
        """Load a workflow definition from a JSON string.

        Returns:
            A new ``Workflow`` instance, or ``None`` on failure.
        """
        lib = get_bindings()
        ptr = lib.csilk_wf_from_json(json_str.encode('utf-8'))
        return cls(ptr) if ptr else None

    @staticmethod
    def serve_ui(app, path="/workflow/ui"):
        """Serve the workflow visualisation UI at *path* on the given app."""
        get_bindings().csilk_wf_serve_ui(app._app, path.encode('utf-8'))

    def add(self, node_id, handler_fn):
        """Add a custom handler node to the workflow.

        Args:
            node_id: Unique node identifier.
            handler_fn: Callback ``(ctx: WorkflowContext, data: WorkflowData) -> ctypes.c_void_p``.

        Returns:
            A ``WorkflowNode``, or ``None`` on failure.
        """
        @CsilkWfHandler
        def wrapper(ctx_ptr, input_ptr, user_data):
            res_ptr = handler_fn(WorkflowContext(ctx_ptr), WorkflowData(input_ptr))
            if res_ptr:
                return ctypes.cast(res_ptr, ctypes.c_void_p).value
            return None
        self._handlers.append(wrapper)
        node_ptr = self._lib.csilk_wf_add(self._wf, node_id.encode('utf-8'), wrapper, None)
        return WorkflowNode(node_ptr) if node_ptr else None

    def add_ai(self, node_id, model="gpt-3.5-turbo", system_msg=None, prompt=None, temperature=0.7, max_tokens=1024, stream=False, max_history_messages=0):
        """Add an AI chat-completion node.

        Args:
            node_id: Unique node identifier.
            model: Model name.
            system_msg: Optional system prompt.
            prompt: Optional user prompt template.
            temperature: Sampling temperature.
            max_tokens: Maximum response tokens.
            stream: Enable streaming.
            max_history_messages: Max conversation history length.

        Returns:
            A ``WorkflowNode``, or ``None`` on failure.
        """
        config = CsilkAiConfig(
            model=model.encode('utf-8') if model else None,
            system_msg=system_msg.encode('utf-8') if system_msg else None,
            prompt=prompt.encode('utf-8') if prompt else None,
            temperature=temperature,
            max_tokens=max_tokens,
            stream=1 if stream else 0,
            max_history_messages=max_history_messages
        )
        node_ptr = self._lib.csilk_wf_add_ai(self._wf, node_id.encode('utf-8'), ctypes.byref(config))
        return WorkflowNode(node_ptr) if node_ptr else None

    def add_vector_search(self, node_id, ai_handle, embedding_model, db_handle, collection, limit=5, input_template=None):
        """Add a vector-search node that retrieves relevant documents.

        Args:
            node_id: Unique node identifier.
            ai_handle: Opaque AI driver handle.
            embedding_model: Embedding model name.
            db_handle: Opaque vector-db handle.
            collection: Collection name.
            limit: Max results.
            input_template: Optional prompt template for reformatting input.

        Returns:
            A ``WorkflowNode``, or ``None`` on failure.
        """
        config = CsilkVectorSearchConfig(
            ai=ai_handle,
            embedding_model=embedding_model.encode('utf-8') if embedding_model else None,
            db=db_handle,
            collection=collection.encode('utf-8') if collection else None,
            limit=limit,
            input_template=input_template.encode('utf-8') if input_template else None
        )
        node_ptr = self._lib.csilk_wf_add_vector_search(self._wf, node_id.encode('utf-8'), ctypes.byref(config))
        return WorkflowNode(node_ptr) if node_ptr else None

    def register_tool(self, name, description, parameters_json, tool_fn):
        """Register a tool that LLM nodes can invoke.

        Args:
            name: Tool name.
            description: Tool description for the LLM.
            parameters_json: JSON schema for tool arguments.
            tool_fn: Callback ``(args_json: str) -> str``.
        """
        @CsilkWfToolFn
        def wrapper(args_json_ptr, user_data):
            args_str = args_json_ptr.decode('utf-8') if args_json_ptr else "{}"
            try:
                res = tool_fn(args_str)
            except Exception as e:
                import traceback
                traceback.print_exc()
                res = json.dumps({"error": str(e)})

            if not isinstance(res, str):
                res = json.dumps(res)

            res_ptr = self._lib.csilk_strdup(res.encode('utf-8'))
            return res_ptr

        self._tools.append(wrapper)
        self._lib.csilk_wf_register_tool(
            self._wf,
            name.encode('utf-8'),
            description.encode('utf-8') if description else None,
            parameters_json.encode('utf-8') if parameters_json else None,
            wrapper,
            None
        )

    def set_tool_discovery(self, discovery_fn):
        """Register a dynamic tool discovery function.

        The discovery callback receives a list of statically registered
        tools and returns a list of dynamically discovered tool dicts.
        """
        from csilk.lib import CsilkWfToolEntry, CsilkWfToolDiscovery


        @CsilkWfToolDiscovery
        def wrapper(wf_ptr, static_tools_ptr, static_count, discovered_out_ptr, discovered_count_out_ptr, user_data):
            try:
                static_tools = []
                if static_tools_ptr and static_count > 0:
                    p_array = ctypes.cast(static_tools_ptr, ctypes.POINTER(CsilkWfToolEntry * static_count))
                    for i in range(static_count):
                        entry = p_array.contents[i]
                        static_tools.append({
                            "name": entry.name.decode('utf-8') if entry.name else "",
                            "description": entry.description.decode('utf-8') if entry.description else "",
                            "parameters_json": entry.parameters_json.decode('utf-8') if entry.parameters_json else ""
                        })

                dynamic_tools = discovery_fn(static_tools)
                if not dynamic_tools:
                    discovered_count_out_ptr[0] = 0
                    discovered_out_ptr[0] = None
                    return 0

                count = len(dynamic_tools)
                entry_sz = ctypes.sizeof(CsilkWfToolEntry)
                array_ptr_val = self._lib.csilk_malloc(count * entry_sz)
                if not array_ptr_val:
                    discovered_count_out_ptr[0] = 0
                    discovered_out_ptr[0] = None
                    return -1

                p_array = ctypes.cast(array_ptr_val, ctypes.POINTER(CsilkWfToolEntry * count))

                for i, tool in enumerate(dynamic_tools):
                    tool_name = tool["name"]
                    tool_desc = tool.get("description", "")
                    tool_params = tool.get("parameters_json", "")
                    tool_fn = tool["fn"]

                    @CsilkWfToolFn
                    def tool_wrapper(args_json_ptr, u_data):
                        args_str = args_json_ptr.decode('utf-8') if args_json_ptr else "{}"
                        try:
                            res = tool_fn(args_str)
                        except Exception as ex:
                            import traceback
                            traceback.print_exc()
                            res = json.dumps({"error": str(ex)})
                        if not isinstance(res, str):
                            res = json.dumps(res)
                        return self._lib.csilk_strdup(res.encode('utf-8'))

                    self._tools.append(tool_wrapper)

                    entry = p_array.contents[i]
                    entry.name = self._lib.csilk_strdup(tool_name.encode('utf-8'))
                    entry.description = self._lib.csilk_strdup(tool_desc.encode('utf-8')) if tool_desc else None
                    entry.parameters_json = self._lib.csilk_strdup(tool_params.encode('utf-8')) if tool_params else None
                    entry.fn = ctypes.cast(tool_wrapper, ctypes.c_void_p).value
                    entry.user_data = None

                discovered_count_out_ptr[0] = count
                discovered_out_ptr[0] = array_ptr_val
                return 0

            except Exception as e:
                import traceback
                traceback.print_exc()
                discovered_count_out_ptr[0] = 0
                discovered_out_ptr[0] = None
                return -1

        self._discovery_cb = wrapper
        self._lib.csilk_wf_set_tool_discovery(self._wf, wrapper, None)

    def get_node(self, node_id):
        """Look up a previously registered node by identifier.

        Args:
            node_id: Node identifier.

        Returns:
            A ``WorkflowNode``, or ``None`` if not found.
        """
        node_ptr = self._lib.csilk_wf_get_node(self._wf, node_id.encode('utf-8'))
        return WorkflowNode(node_ptr) if node_ptr else None

    def signal_continue(self, exec_id, input_data=None, callback=None):
        """Signal a waiting workflow to continue with new input.

        Used when a human-in-the-loop or external system provides input
        to a paused *interrupt*-node.

        Args:
            exec_id: Execution instance identifier.
            input_data: Optional input string to pass to the workflow.
            callback: Optional ``WorkflowData`` callback invoked with the
                      continuation result.
        """
        c_exec_id = exec_id.encode('utf-8')
        
        c_input = None
        c_val = None
        c_data = None
        if input_data:
            if isinstance(input_data, str):
                c_val = self._lib.csilk_strdup(input_data.encode('utf-8'))
                c_data = CsilkData(
                    type=b"text/plain",
                    value=c_val,
                    free_fn=None,
                    meta=None
                )
                c_input = ctypes.pointer(c_data)

        @CsilkWfRunCallback
        def run_cb(res_ptr):
            try:
                res = WorkflowData(res_ptr) if res_ptr else None
                if callback:
                    callback(res)
            finally:
                if c_val:
                    self._lib.csilk_free(c_val)
                _ = (c_data, c_input)
                if run_cb in self._run_callbacks:
                    self._run_callbacks.remove(run_cb)

        self._run_callbacks.append(run_cb)
        self._lib.csilk_wf_signal_continue(self._wf, c_exec_id, c_input, run_cb)

    def set_ttl(self, ttl_sec):
        """Set the execution time-to-live in seconds.

        Workflows exceeding this wall-clock limit are automatically
        cancelled.

        Args:
            ttl_sec: Maximum wall-clock time in seconds.
        """
        self._lib.csilk_wf_set_ttl(self._wf, ttl_sec)

    def set_budget(self, max_tokens):
        """Limit total LLM token consumption for a run.

        Args:
            max_tokens: Maximum cumulative token count across all
                        LLM nodes in the run.
        """
        self._lib.csilk_wf_set_budget(self._wf, max_tokens)

    def enable_distributed(self, mq):
        """Enable distributed execution over a message queue.

        Args:
            mq: An ``MQ`` instance (from :mod:`csilk.app`).
        """
        self._lib.csilk_wf_enable_distributed(self._wf, mq._mq)

    def set_persistence(self, wal_dir):
        """Enable crash recovery via write-ahead log.

        Args:
            wal_dir: Directory path for the WAL.
        """
        self._lib.csilk_wf_set_persistence(self._wf, wal_dir.encode('utf-8'))

    def run(self, input_val, input_type="text/plain", callback=None):
        """Execute the workflow asynchronously.

        Args:
            input_val: Input string payload.
            input_type: MIME type of the input (default ``"text/plain"``).
            callback: Optional ``WorkflowData`` callback invoked on
                      completion.

        Returns:
            The execution instance identifier string, or ``None`` on
            failure.
        """
        c_type = input_type.encode('utf-8')
        c_val = self._lib.csilk_strdup(input_val.encode('utf-8'))
        c_data = CsilkData(
            type=c_type,
            value=c_val,
            free_fn=None,
            meta=None
        )
        c_input = ctypes.pointer(c_data)

        @CsilkWfRunCallback
        def run_cb(res_ptr):
            try:
                res = WorkflowData(res_ptr) if res_ptr else None
                if callback:
                    callback(res)
            finally:
                self._lib.csilk_free(c_val)
                _ = (c_data, c_input, c_type)
                if run_cb in self._run_callbacks:
                    self._run_callbacks.remove(run_cb)

        self._run_callbacks.append(run_cb)
        res_exec_id = self._lib.csilk_wf_run(self._wf, c_input, run_cb)
        return res_exec_id.decode('utf-8') if res_exec_id else None

    def resume(self, exec_id, callback=None):
        """Resume a previously interrupted workflow execution.

        Used when continuing a workflow that was persisted and
        interrupted (e.g. server restart).

        Args:
            exec_id: Execution instance identifier.
            callback: Optional ``WorkflowData`` callback invoked on
                      completion.
        """
        @CsilkWfRunCallback
        def run_cb(res_ptr):
            try:
                res = WorkflowData(res_ptr) if res_ptr else None
                if callback:
                    callback(res)
            finally:
                if run_cb in self._run_callbacks:
                    self._run_callbacks.remove(run_cb)

        self._run_callbacks.append(run_cb)
        self._lib.csilk_wf_resume(self._wf, exec_id.encode('utf-8'), run_cb)

    def run_traced(self, input_val, input_type="text/plain", callback=None):
        """Execute the workflow and capture a trace of the run.

        Useful for debugging, observability, or replay.

        Args:
            input_val: Input string payload.
            input_type: MIME type of the input.
            callback: Optional callback ``(WorkflowData, trace_ptr)``
                      invoked on completion.
        """
        c_type = input_type.encode('utf-8')
        c_val = self._lib.csilk_strdup(input_val.encode('utf-8'))
        c_data = CsilkData(
            type=c_type,
            value=c_val,
            free_fn=None,
            meta=None
        )
        c_input = ctypes.pointer(c_data)

        @CsilkWfRunTracedCallback
        def run_cb(res_ptr, trace_ptr):
            try:
                res = WorkflowData(res_ptr) if res_ptr else None
                if callback:
                    callback(res, trace_ptr)
            finally:
                self._lib.csilk_free(c_val)
                _ = (c_data, c_input, c_type)
                if run_cb in self._run_callbacks:
                    self._run_callbacks.remove(run_cb)

        self._run_callbacks.append(run_cb)
        self._lib.csilk_wf_run_traced(self._wf, c_input, run_cb)


    def trace_to_json(self, trace_ptr):
        """Serialize a captured trace to JSON.

        Args:
            trace_ptr: Opaque trace pointer from :meth:`run_traced`.

        Returns:
            JSON string representation of the trace.
        """
        res = self._lib.csilk_wf_trace_to_json(trace_ptr)
        if res:
            val = ctypes.string_at(res).decode('utf-8')
            self._lib.csilk_free(res)
            return val
        return None

    def trace_free(self, trace_ptr):
        """Free a captured trace allocated by the runtime.

        Args:
            trace_ptr: Opaque trace pointer to deallocate.
        """
        self._lib.csilk_wf_trace_free(trace_ptr)

    def to_mermaid(self):
        """Render the workflow as a Mermaid.js flowchart.

        Returns:
            Mermaid diagram source string.
        """
        res = self._lib.csilk_wf_to_mermaid(self._wf)
        if res:
            val = ctypes.string_at(res).decode('utf-8')
            self._lib.csilk_free(res)
            return val
        return None

    def _repr_html_(self):
        """Render the workflow as an interactive Mermaid diagram in Jupyter.

        Returns:
            HTML ``<div>`` with embedded Mermaid source and loader script.
        """
        mermaid_code = self.to_mermaid()
        if not mermaid_code:
            return "<div>Empty Workflow</div>"
        return f"""
        <div class="mermaid">
        {mermaid_code}
        </div>
        <script type="module">
        import mermaid from 'https://cdn.jsdelivr.net/npm/mermaid@10/dist/mermaid.esm.min.mjs';
        mermaid.initialize({{ startOnLoad: true, theme: 'default' }});
        </script>
        """

    def register_monitor(self, ctx):
        """Register a monitor context for observability.

        Args:
            ctx: A ``Context`` instance from :mod:`csilk.app`.
        """
        self._lib.csilk_wf_register_monitor(self._wf, ctx._ctx)

