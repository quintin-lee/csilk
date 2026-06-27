import asyncio
import json
import subprocess
from typing import Dict, Any, List, Optional
import os

class MCPStdioClient:
    """
    A minimal zero-dependency Model Context Protocol (MCP) Client
    that communicates with MCP servers via stdio.
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
        msg = {
            "jsonrpc": "2.0",
            "method": method,
            "params": params or {}
        }
        self.process.stdin.write(json.dumps(msg).encode('utf-8') + b'\n')
        await self.process.stdin.drain()

    async def list_tools(self) -> List[dict]:
        res = await self.request("tools/list")
        return res.get("tools", [])

    async def call_tool(self, name: str, arguments: dict) -> Any:
        res = await self.request("tools/call", {
            "name": name,
            "arguments": arguments
        })
        
        # Format the result back into a single string for csilk
        content = res.get("content", [])
        output = ""
        for c in content:
            if c.get("type") == "text":
                output += c.get("text", "")
        return output

    async def close(self):
        if self.process:
            try:
                self.process.terminate()
            except OSError:
                pass
            await self.process.wait()

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
                return asyncio.run(client.call_tool(tool_name, args_dict))
        return mcp_tool_handler

    async def init_client():
        await client.connect()
        tools = await client.list_tools()
        for t in tools:
            name = t["name"]
            desc = t.get("description", "")
            schema = t.get("inputSchema", {})
            schema_json = json.dumps(schema)
            
            # Register to C engine
            wf.register_tool(name, desc, schema_json, tool_fn=create_tool_handler(name))
            
        return client

    # Synchronous or Asynchronous initialization depending on context
    try:
        loop = asyncio.get_running_loop()
        asyncio.create_task(init_client())
    except RuntimeError:
        asyncio.run(init_client())

    return client
