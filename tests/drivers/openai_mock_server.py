#!/usr/bin/env python3
"""
Mock OpenAI API server for csilk unit/integration tests.
Supports /v1/chat/completions (streaming, tools, usage) and /v1/embeddings.
"""
import sys
import json
import time
from http.server import HTTPServer, BaseHTTPRequestHandler

class OpenAIMockHandler(BaseHTTPRequestHandler):
    def log_message(self, format, *args):
        pass

    def do_POST(self):
        content_length = int(self.headers.get('Content-Length', 0))
        body_bytes = self.rfile.read(content_length)
        body = {}
        if body_bytes:
            try:
                body = json.loads(body_bytes.decode('utf-8'))
            except Exception:
                pass

        if self.path.endswith('/chat/completions'):
            self.handle_chat_completions(body)
        elif self.path.endswith('/embeddings'):
            self.handle_embeddings(body)
        elif self.path.endswith('/messages'):
            self.handle_anthropic_messages(body)
        else:
            self.send_response(404)
            self.end_headers()

    def handle_chat_completions(self, body):
        is_stream = body.get('stream', False)
        tools = body.get('tools', [])
        messages = body.get('messages', [])
        last_msg = messages[-1].get('content', '') if messages else ''

        if is_stream:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'close')
            self.end_headers()

            if tools or "weather" in last_msg.lower():
                # Streamed tool call
                chunks = [
                    {
                        "id": "chatcmpl-mock",
                        "object": "chat.completion.chunk",
                        "created": int(time.time()),
                        "model": "gpt-3.5-turbo",
                        "choices": [{
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "tool_calls": [{
                                    "index": 0,
                                    "id": "call_mock_001",
                                    "type": "function",
                                    "function": {
                                        "name": "get_weather",
                                        "arguments": ""
                                    }
                                }]
                            },
                            "finish_reason": None
                        }]
                    },
                    {
                        "id": "chatcmpl-mock",
                        "object": "chat.completion.chunk",
                        "created": int(time.time()),
                        "model": "gpt-3.5-turbo",
                        "choices": [{
                            "index": 0,
                            "delta": {
                                "tool_calls": [{
                                    "index": 0,
                                    "function": {
                                        "arguments": "{\"city\": "
                                    }
                                }]
                            },
                            "finish_reason": None
                        }]
                    },
                    {
                        "id": "chatcmpl-mock",
                        "object": "chat.completion.chunk",
                        "created": int(time.time()),
                        "model": "gpt-3.5-turbo",
                        "choices": [{
                            "index": 0,
                            "delta": {
                                "tool_calls": [{
                                    "index": 0,
                                    "function": {
                                        "arguments": "\"Beijing\"}"
                                    }
                                }]
                            },
                            "finish_reason": "tool_calls"
                        }]
                    },
                    {
                        "id": "chatcmpl-mock",
                        "object": "chat.completion.chunk",
                        "created": int(time.time()),
                        "model": "gpt-3.5-turbo",
                        "choices": [],
                        "usage": {
                            "prompt_tokens": 12,
                            "completion_tokens": 8,
                            "total_tokens": 20
                        }
                    }
                ]
            else:
                # Regular streamed text
                chunks = [
                    {
                        "id": "chatcmpl-mock",
                        "object": "chat.completion.chunk",
                        "created": int(time.time()),
                        "model": "gpt-3.5-turbo",
                        "choices": [{
                            "index": 0,
                            "delta": {
                                "role": "assistant",
                                "content": "Hello "
                            },
                            "finish_reason": None
                        }]
                    },
                    {
                        "id": "chatcmpl-mock",
                        "object": "chat.completion.chunk",
                        "created": int(time.time()),
                        "model": "gpt-3.5-turbo",
                        "choices": [{
                            "index": 0,
                            "delta": {
                                "content": "world!"
                            },
                            "finish_reason": "stop"
                        }]
                    },
                    {
                        "id": "chatcmpl-mock",
                        "object": "chat.completion.chunk",
                        "created": int(time.time()),
                        "model": "gpt-3.5-turbo",
                        "choices": [],
                        "usage": {
                            "prompt_tokens": 10,
                            "completion_tokens": 5,
                            "total_tokens": 15
                        }
                    }
                ]

            for chunk in chunks:
                line = f"data: {json.dumps(chunk)}\n\n"
                self.wfile.write(line.encode('utf-8'))
                self.wfile.flush()

            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()
            self.close_connection = True

        else:
            if tools or "weather" in last_msg.lower():
                resp = {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion",
                    "created": int(time.time()),
                    "model": "gpt-3.5-turbo",
                    "choices": [{
                        "index": 0,
                        "message": {
                            "role": "assistant",
                            "content": None,
                            "tool_calls": [{
                                "id": "call_mock_001",
                                "type": "function",
                                "function": {
                                    "name": "get_weather",
                                    "arguments": "{\"city\": \"Beijing\"}"
                                }
                            }]
                        },
                        "finish_reason": "tool_calls"
                    }],
                    "usage": {
                        "prompt_tokens": 12,
                        "completion_tokens": 8,
                        "total_tokens": 20
                    }
                }
            else:
                resp = {
                    "id": "chatcmpl-mock",
                    "object": "chat.completion",
                    "created": int(time.time()),
                    "model": "gpt-3.5-turbo",
                    "choices": [{
                        "index": 0,
                        "message": {
                            "role": "assistant",
                            "content": "Mock response from test server."
                        },
                        "finish_reason": "stop"
                    }],
                    "usage": {
                        "prompt_tokens": 10,
                        "completion_tokens": 5,
                        "total_tokens": 15
                    }
                }

            out_bytes = json.dumps(resp).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(out_bytes)))
            self.end_headers()
            self.wfile.write(out_bytes)

    def handle_embeddings(self, body):
        inputs = body.get('input', [])
        if isinstance(inputs, str):
            inputs = [inputs]

        data = []
        for i, _ in enumerate(inputs):
            data.append({
                "object": "embedding",
                "index": i,
                "embedding": [0.1, 0.2, 0.3, 0.4]
            })

        resp = {
            "object": "list",
            "data": data,
            "model": "text-embedding-ada-002",
            "usage": {
                "prompt_tokens": 2 * len(inputs),
                "total_tokens": 2 * len(inputs)
            }
        }
        out_bytes = json.dumps(resp).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Content-Length', str(len(out_bytes)))
        self.end_headers()
        self.wfile.write(out_bytes)

    def handle_anthropic_messages(self, body):
        is_stream = body.get('stream', False)
        tools = body.get('tools', [])
        messages = body.get('messages', [])
        last_msg = messages[-1].get('content', '') if messages else ''

        if is_stream:
            self.send_response(200)
            self.send_header('Content-Type', 'text/event-stream')
            self.send_header('Cache-Control', 'no-cache')
            self.send_header('Connection', 'close')
            self.end_headers()

            events = [
                {"type": "message_start", "message": {"id": "msg_mock", "type": "message", "role": "assistant", "model": "claude-3-5-sonnet", "usage": {"input_tokens": 12, "output_tokens": 0}}}
            ]

            if tools or "weather" in last_msg.lower():
                events.extend([
                    {"type": "content_block_start", "index": 0, "content_block": {"type": "tool_use", "id": "toolu_mock_001", "name": "get_weather", "input": {}}},
                    {"type": "content_block_delta", "index": 0, "delta": {"type": "input_json_delta", "partial_json": "{\"city\": \"Beijing\"}"}},
                    {"type": "content_block_stop", "index": 0},
                    {"type": "message_delta", "delta": {"stop_reason": "tool_use"}, "usage": {"output_tokens": 8}}
                ])
            else:
                events.extend([
                    {"type": "content_block_start", "index": 0, "content_block": {"type": "text", "text": ""}},
                    {"type": "content_block_delta", "index": 0, "delta": {"type": "text_delta", "text": "Hello Claude!"}},
                    {"type": "content_block_stop", "index": 0},
                    {"type": "message_delta", "delta": {"stop_reason": "end_turn"}, "usage": {"output_tokens": 5}}
                ])

            for ev in events:
                line = f"event: {ev['type']}\ndata: {json.dumps(ev)}\n\n"
                self.wfile.write(line.encode('utf-8'))
                self.wfile.flush()
            self.close_connection = True
        else:
            if tools or "weather" in last_msg.lower():
                resp = {
                    "id": "msg_mock",
                    "type": "message",
                    "role": "assistant",
                    "model": "claude-3-5-sonnet",
                    "content": [{
                        "type": "tool_use",
                        "id": "toolu_mock_001",
                        "name": "get_weather",
                        "input": {"city": "Beijing"}
                    }],
                    "stop_reason": "tool_use",
                    "usage": {"input_tokens": 12, "output_tokens": 8}
                }
            else:
                resp = {
                    "id": "msg_mock",
                    "type": "message",
                    "role": "assistant",
                    "model": "claude-3-5-sonnet",
                    "content": [{
                        "type": "text",
                        "text": "Hello from Claude mock."
                    }],
                    "stop_reason": "end_turn",
                    "usage": {"input_tokens": 10, "output_tokens": 6}
                }

            out_bytes = json.dumps(resp).encode('utf-8')
            self.send_response(200)
            self.send_header('Content-Type', 'application/json')
            self.send_header('Content-Length', str(len(out_bytes)))
            self.end_headers()
            self.wfile.write(out_bytes)


def run_server(port=18081):
    server = HTTPServer(('127.0.0.1', port), OpenAIMockHandler)
    server.serve_forever()

if __name__ == '__main__':
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 18081
    run_server(port)
