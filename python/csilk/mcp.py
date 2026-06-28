"""Model Context Protocol (MCP) client for integrating external MCP servers
with csilk workflows.

Provides a lightweight, zero-dependency ``MCPStdioClient`` that communicates
with MCP servers over stdio using JSON-RPC 2.0.
"""

import asyncio
import json
import subprocess
from typing import Dict, Any, List, Optional
import os


class MCPStdioClient:
    """A minimal MCP client that communicates with MCP servers via stdio.

    Supports the standard MCP lifecycle (initialize, tool listing, tool call)
    using JSON-RPC 2.0 messages over the child process's stdin/stdout.

    Usage::

        client = MCPStdioClient("npx", ["-y", "@modelcontextprotocol/server-filesystem"])
        await client.connect()
        tools = await client.list_tools()
        result = await client.call_tool("read_file", {"path": "/tmp/test.txt"})
        await client.close()
    """

    def __init__(self, command: str, args: List[str], env: Optional[Dict[str, str]] = None):
        self.command = command
        self.args = args
        self.env = env or os.environ.copy()
        self.process = None
        self._msg_id = 0
        self._pending_requests = {}
        self._loop = None

    async def connect(self):
        self._loop = asyncio.get_running_loop()
        
        self.process = await asyncio.create_subprocess_exec(
            self.command, *self.args,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=self.env
        )
        
        # Start read loop
        self._read_task = asyncio.create_task(self._read_loop())
        
        # Initialize sequence
        init_res = await self.request("initialize", {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "csilk", "version": "1.0.0"}
        })
        
        # Send initialized notification
        await self.notify("notifications/initialized", {})
        
        return init_res

    async def _read_loop(self):
        """Background task: read JSON-RPC responses from the server's stdout."""
        while True:
            line = await self.process.stdout.readline()
            if not line:
                break

            try:
                msg = json.loads(line.decode('utf-8'))
            except json.JSONDecodeError:
                continue

            if "id" in msg and msg["id"] in self._pending_requests:
                future = self._pending_requests.pop(msg["id"])
                if not future.done():
                    if "error" in msg:
                        future.set_exception(RuntimeError(msg["error"]))
                    else:
                        future.set_result(msg.get("result", {}))

    async def request(self, method: str, params: dict = None) -> dict:
        """Send a JSON-RPC request and wait for the response.

        Args:
            method: The method name.
            params: Optional parameters dict.

        Returns:
            The ``result`` field from the response.

        Raises:
            RuntimeError: If the server returns an error.
        """
        self._msg_id += 1
        msg_id = self._msg_id

        msg = {
            "jsonrpc": "2.0",
            "id": msg_id,
            "method": method,
            "params": params or {}
        }

        future = self._loop.create_future()
        self._pending_requests[msg_id] = future

        self.process.stdin.write(json.dumps(msg).encode('utf-8') + b'\n')
        await self.process.stdin.drain()

        return await future

    async def notify(self, method: str, params: dict = None):
        """Send a JSON-RPC notification (no response expected)."""
        msg = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params or {}
        }
        self.process.stdin.write(json.dumps(msg).encode('utf-8') + b'\n')
        await self.process.stdin.drain()

    async def list_tools(self) -> List[dict]:
        """List all tools exposed by the MCP server.

        Returns:
            A list of tool descriptor dicts (each with ``name``,
            ``description``, ``inputSchema``, etc.).
        """
        res = await self.request("tools/list")
        return res.get("tools", [])

    async def call_tool(self, name: str, arguments: dict) -> Any:
        """Call a tool on the MCP server.

        Args:
            name: Tool name.
            arguments: Tool arguments dict.

        Returns:
            Concatenated text content from the tool response.
        """
        res = await self.request("tools/call", {
            "name": name,
            "arguments": arguments
        })

        content = res.get("content", [])
        output = ""
        for c in content:
            if c.get("type") == "text":
                output += c.get("text", "")
        return output

    async def close(self):
        """Terminate the MCP server process."""
        if self.process:
            try:
                self.process.terminate()
            except OSError:
                pass
            await self.process.wait()
            
        if self._loop and self._loop.is_running():
            self._loop.call_soon_threadsafe(self._loop.stop)

    def shutdown(self):
        """Thread-safe synchronous shutdown for background loop clients."""
        if self._loop and self._loop.is_running():
            asyncio.run_coroutine_threadsafe(self.close(), self._loop)

def bind_mcp_to_workflow(wf, command: str, args: List[str], env: Optional[Dict[str, str]] = None):
    """
    Binds an MCP server to a Csilk Workflow engine.
    It automatically discovers tools from the MCP server and registers them to the workflow.
    """
    client = MCPStdioClient(command, args, env)
    

    def create_tool_handler(tool_name):
        def mcp_tool_handler(args_json: str) -> str:
            args_dict = json.loads(args_json)
            if client._loop and client._loop.is_running():
                future = asyncio.run_coroutine_threadsafe(client.call_tool(tool_name, args_dict), client._loop)
                return future.result()
            else:
                raise RuntimeError("MCP client loop is not running. Call connect() first.")
        return mcp_tool_handler

    # Since bind_mcp_to_workflow doesn't know how to run the background loop seamlessly without blocking,
    # it's best to require the user to start the client before binding, OR we spawn a thread.
    # We will spawn a background thread with its own event loop to manage the MCP client lifecycle.
    
    import threading
    
    def run_mcp_loop():
        loop = asyncio.new_event_loop()
        client._loop = loop
        asyncio.set_event_loop(loop)
        
        async def init():
            await client.connect()
            tools = await client.list_tools()
            for t in tools:
                name = t["name"]
                desc = t.get("description", "")
                schema = t.get("inputSchema", {})
                schema_json = json.dumps(schema)
                wf.register_tool(name, desc, schema_json, tool_fn=create_tool_handler(name))
                
        loop.run_until_complete(init())
        loop.run_forever()
        
    t = threading.Thread(target=run_mcp_loop, daemon=True)
    t.start()
    
    # Wait for the client to be initialized
    import time
    timeout = 10.0
    start = time.time()
    while not client._loop or not client._loop.is_running() or not client.process:
        if time.time() - start > timeout:
            raise RuntimeError("MCP Client failed to initialize within timeout.")
        time.sleep(0.05)

    return client
