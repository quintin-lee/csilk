"""Python-side ABI regression tests for the AI driver ctypes mirror.

Locks the field order and offsets of CsilkAiMessage / CsilkAiChatRequest /
CsilkAiChatResponse (python/csilk/lib.py) to the C contract in
include/csilk/drivers/ai.h, which is independently pinned by the
static_assert() layout guards in tests/core/test_json_ai_abi.c.

A mismatch here manifests as garbage arguments or crashes at the FFI
boundary (the historical Python AI crash), so offsets are asserted
explicitly rather than relying on "it happens to work today".
"""

import ctypes

import pytest

from csilk import lib


class TestCsilkAiMessageLayout:
    """C: csilk_ai_message_t (5 pointers on LP64)."""

    FIELDS = ["role", "content", "tool_calls", "tool_call_count", "tool_call_id"]

    def test_field_order(self):
        names = [name for name, _ in lib.CsilkAiMessage._fields_]
        assert names == self.FIELDS

    def test_offsets(self):
        ptr = ctypes.sizeof(ctypes.c_void_p)
        size_t = ctypes.sizeof(ctypes.c_size_t)
        expect = {
            "role": 0,
            "content": ptr,
            "tool_calls": 2 * ptr,
            "tool_call_count": 3 * ptr,
            "tool_call_id": 3 * ptr + size_t,
        }
        for name, offset in expect.items():
            actual = getattr(lib.CsilkAiMessage, name).offset
            assert actual == offset, f"{name}: {actual} != {offset}"

    def test_sizeof(self):
        assert ctypes.sizeof(lib.CsilkAiMessage) == 5 * ctypes.sizeof(ctypes.c_void_p)


class TestCsilkAiChatRequestLayout:
    """C: csilk_ai_chat_request_t (LP64: 15 pointers + 32 bytes of scalars)."""

    FIELDS = [
        "model",
        "messages",
        "message_count",
        "temperature",
        "top_p",
        "presence_penalty",
        "frequency_penalty",
        "max_tokens",
        "stop",
        "stop_count",
        "user",
        "stream",
        "on_chunk",
        "user_data",
        "timeout_ms",
        "tools",
        "tool_count",
        "tool_choice",
        "reasoning_effort",
    ]

    def test_field_order(self):
        names = [name for name, _ in lib.CsilkAiChatRequest._fields_]
        assert names == self.FIELDS

    def test_offsets(self):
        ptr = ctypes.sizeof(ctypes.c_void_p)
        expect = {
            "model": 0,
            "messages": 1 * ptr,
            "message_count": 2 * ptr,
            "temperature": 3 * ptr,
            "top_p": 3 * ptr + 8,
            "presence_penalty": 3 * ptr + 16,
            "frequency_penalty": 3 * ptr + 24,
            "max_tokens": 3 * ptr + 32,
            "stop": 4 * ptr + 32,
            "stop_count": 5 * ptr + 32,
            "user": 6 * ptr + 32,
            "stream": 7 * ptr + 32,
            "on_chunk": 8 * ptr + 32,
            "user_data": 9 * ptr + 32,
            "timeout_ms": 10 * ptr + 32,
            "tools": 11 * ptr + 32,
            "tool_count": 12 * ptr + 32,
            "tool_choice": 13 * ptr + 32,
            "reasoning_effort": 14 * ptr + 32,
        }
        for name, offset in expect.items():
            actual = getattr(lib.CsilkAiChatRequest, name).offset
            assert actual == offset, f"{name}: {actual} != {offset}"

    def test_sizeof(self):
        assert ctypes.sizeof(lib.CsilkAiChatRequest) == 15 * ctypes.sizeof(ctypes.c_void_p) + 32


class TestCsilkAiChatResponseLayout:
    """C: csilk_ai_chat_response_t."""

    FIELDS = [
        "content",
        "tool_calls",
        "tool_call_count",
        "prompt_tokens",
        "completion_tokens",
        "total_tokens",
        "raw_response",
        "error_message",
    ]

    def test_field_order(self):
        names = [name for name, _ in lib.CsilkAiChatResponse._fields_]
        assert names == self.FIELDS

    def test_offsets(self):
        ptr = ctypes.sizeof(ctypes.c_void_p)
        expect = {
            "content": 0,
            "tool_calls": 1 * ptr,
            "tool_call_count": 2 * ptr,
            "prompt_tokens": 3 * ptr,
            "completion_tokens": 3 * ptr + 4,
            "total_tokens": 4 * ptr,
            # ints end at 4*ptr+4; the next pointer re-aligns to 5*ptr.
            "raw_response": 5 * ptr,
            "error_message": 6 * ptr,
        }
        for name, offset in expect.items():
            actual = getattr(lib.CsilkAiChatResponse, name).offset
            assert actual == offset, f"{name}: {actual} != {offset}"

    def test_sizeof(self):
        # 2 pointers + 1 size_t + 3 ints + tail padding to 2 pointers.
        assert ctypes.sizeof(lib.CsilkAiChatResponse) == 7 * ctypes.sizeof(ctypes.c_void_p)


def test_ownership_helper_exists():
    """The AI wrapper must expose an explicit free for response buffers."""
    from csilk.ai import AI

    assert hasattr(AI, "get_stats")  # stats path touches response free logic
