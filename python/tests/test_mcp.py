import pytest
import asyncio
import sys
import json
import os
from csilk.mcp import MCPStdioClient, bind_mcp_to_workflow
from csilk.workflow import Workflow

# A dummy MCP server script that will be executed as a subprocess
DUMMY_MCP_SERVER = """
import sys
import json

def handle_request(msg):
    req_id = msg.get("id")
    method = msg.get("method")
    
    if method == "initialize":
        return {"jsonrpc": "2.0", "id": req_id, "result": {"serverInfo": {"name": "dummy"}}}
    elif method == "tools/list":
        return {"jsonrpc": "2.0", "id": req_id, "result": {"tools": [{"name": "echo", "description": "echoes input", "inputSchema": {}}]}}
    elif method == "tools/call":
        args = msg.get("params", {}).get("arguments", {})
        text = args.get("text", "")
        return {"jsonrpc": "2.0", "id": req_id, "result": {"content": [{"type": "text", "text": text}]}}
    
    return {"jsonrpc": "2.0", "id": req_id, "error": "unknown method"}

for line in sys.stdin:
    if not line.strip(): continue
    msg = json.loads(line)
    if "method" in msg and msg["method"] == "notifications/initialized":
        continue # Ignore notification
    res = handle_request(msg)
    sys.stdout.write(json.dumps(res) + "\\n")
    sys.stdout.flush()
"""

class TestMCP:
    @pytest.mark.asyncio
    async def test_mcp_client(self, tmp_path):
        server_path = tmp_path / "dummy_server.py"
        server_path.write_text(DUMMY_MCP_SERVER)
        
        client = MCPStdioClient(sys.executable, [str(server_path)])
        
        # Test connection
        init_res = await client.connect()
        assert init_res.get("serverInfo", {}).get("name") == "dummy"
        
        # Test tools list
        tools = await client.list_tools()
        assert len(tools) == 1
        assert tools[0]["name"] == "echo"
        
        # Test tool call
        result = await client.call_tool("echo", {"text": "hello mcp"})
        assert result == "hello mcp"
        
        await client.close()

    @pytest.mark.asyncio
    async def test_bind_mcp(self, tmp_path):
        server_path = tmp_path / "dummy_server.py"
        server_path.write_text(DUMMY_MCP_SERVER)
        
        wf = Workflow("test_wf")
        
        # Bind MCP
        client = bind_mcp_to_workflow(wf, sys.executable, [str(server_path)])
        assert client is not None
        
        # Wait a bit for the async task inside bind_mcp_to_workflow to finish
        await asyncio.sleep(0.5)
        
        client.shutdown()
        wf.free()
