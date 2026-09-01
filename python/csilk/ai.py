"""Unified AI interface for chat, embeddings, and conversation context.

Wraps the csilk C AI engine, which supports multiple AI providers
(OpenAI, Ollama, etc.) through a pluggable driver system.
"""

import ctypes
import json
from csilk.lib import (
    get_bindings, CsilkAiMessage, CsilkAiChatRequest, CsilkAiChatResponse,
    CsilkAiEmbeddingsResponse, CsilkAiStats, CsilkAiStreamCb, CsilkAiContext
)


class AI:
    """High-level wrapper around the csilk AI engine.

    Provides chat completions (with streaming and tool calls) and
    text embeddings through a unified interface.

    Usage::

        ai = AI("openai", api_key="sk-...", base_url=None)
        resp = ai.chat("gpt-4", [{"role": "user", "content": "hi"}])
        vectors = ai.generate_embeddings("text-embedding-3-small", ["hello"])
        print(AI.get_stats())
    """

    def __init__(self, driver_name: str, api_key: str = None, base_url: str = None):
        """Open an AI driver instance.

        Args:
            driver_name: Driver identifier (e.g. ``"openai"``, ``"ollama"``).
            api_key: Optional API key.
            base_url: Optional custom base URL.
        Raises:
            RuntimeError: If the driver cannot be initialised.
        """
        self._lib = get_bindings()
        c_driver = driver_name.encode('utf-8')
        c_key = api_key.encode('utf-8') if api_key else None
        c_url = base_url.encode('utf-8') if base_url else None
        self._ai = self._lib.csilk_ai_new(c_driver, c_key, c_url)
        if not self._ai:
            raise RuntimeError(f"Failed to create AI instance for driver '{driver_name}'")
        self._stream_callbacks = []  # Keep refs to prevent GC

    def free(self):
        """Release the AI driver and all associated resources."""
        if self._ai:
            self._lib.csilk_ai_free(self._ai)
            self._ai = None

    def __del__(self):
        self.free()

    @staticmethod
    def get_stats() -> dict:
        """Return global AI engine statistics.

        Returns:
            A dict with ``requests_total``, ``tokens_total``,
            ``prompt_tokens``, ``completion_tokens``, ``errors_total``,
            and ``duration_us_total``.
        """
        lib = get_bindings()
        stats = CsilkAiStats()
        lib.csilk_ai_get_stats(ctypes.byref(stats))
        return {
            "requests_total": stats.requests_total,
            "tokens_total": stats.tokens_total,
            "prompt_tokens": stats.prompt_tokens,
            "completion_tokens": stats.completion_tokens,
            "errors_total": stats.errors_total,
            "duration_us_total": stats.duration_us_total,
        }

    @staticmethod
    def register_monitor(ctx):
        """Register a context for AI workflow monitoring."""
        get_bindings().csilk_ai_register_monitor(ctx._ctx)

    def chat(self, model: str, messages: list, temperature: float = 0.7,
             max_tokens: int = 1024, on_chunk=None, timeout_ms: int = 0) -> dict:
        """Send a chat completion request.

        Args:
            model: Model identifier (e.g. ``"gpt-4"``, ``"llama3"``).
            messages: List of message dicts, each with ``"role"`` and
                ``"content"`` keys.
            temperature: Sampling temperature (0.0 – 2.0).
            max_tokens: Maximum tokens in the response.
            on_chunk: Optional callback ``(chunk: str) -> None`` for
                streaming responses.
            timeout_ms: Request timeout in milliseconds (0 = no timeout).

        Returns:
            A dict with ``content``, ``tool_calls`` (list),
            ``prompt_tokens``, ``completion_tokens``, ``total_tokens``.

        Raises:
            RuntimeError: If the API call fails.
        """
        # messages is list of dicts: [{"role": "user", "content": "hi"}]
        c_msg_array = (CsilkAiMessage * len(messages))()
        for i, msg in enumerate(messages):
            c_msg_array[i].role = msg["role"].encode('utf-8')
            c_msg_array[i].content = msg["content"].encode('utf-8')

        c_req = CsilkAiChatRequest()
        c_req.model = model.encode('utf-8')
        c_req.messages = ctypes.cast(c_msg_array, ctypes.POINTER(CsilkAiMessage))
        c_req.message_count = len(messages)
        c_req.temperature = temperature
        c_req.max_tokens = max_tokens
        c_req.timeout_ms = timeout_ms

        stream_wrapper = None
        if on_chunk:
            @CsilkAiStreamCb
            def wrapper(chunk_ptr, user_data):
                try:
                    chunk_str = chunk_ptr.decode('utf-8') if chunk_ptr else ""
                    on_chunk(chunk_str)
                except Exception:
                    import traceback
                    traceback.print_exc()
            self._stream_callbacks.append(wrapper)
            c_req.stream = True
            c_req.on_chunk = ctypes.cast(wrapper, ctypes.c_void_p).value
            stream_wrapper = wrapper

        c_res = CsilkAiChatResponse()
        res_code = self._lib.csilk_ai_chat(self._ai, ctypes.byref(c_req), ctypes.byref(c_res))

        if stream_wrapper and stream_wrapper in self._stream_callbacks:
            self._stream_callbacks.remove(stream_wrapper)

        if res_code != 0:
            err = c_res.error_message.decode('utf-8') if c_res.error_message else "Unknown AI error"
            self._lib.csilk_ai_chat_response_free(ctypes.byref(c_res))
            raise RuntimeError(err)

        try:
            content = c_res.content.decode('utf-8') if c_res.content else ""
            tool_calls = []
            if c_res.tool_call_count > 0:
                for i in range(c_res.tool_call_count):
                    tc = c_res.tool_calls[i]
                    tool_calls.append({
                        "id": tc.id.decode('utf-8') if tc.id else "",
                        "name": tc.name.decode('utf-8') if tc.name else "",
                        "arguments": tc.arguments.decode('utf-8') if tc.arguments else ""
                    })
            return {
                "content": content,
                "tool_calls": tool_calls,
                "prompt_tokens": c_res.prompt_tokens,
                "completion_tokens": c_res.completion_tokens,
                "total_tokens": c_res.total_tokens
            }
        finally:
            self._lib.csilk_ai_chat_response_free(ctypes.byref(c_res))

    def generate_embeddings(self, model: str, input_texts: list) -> list:
        """Generate embeddings for one or more input texts.

        Args:
            model: Embedding model identifier.
            input_texts: A single string or a list of strings to embed.

        Returns:
            A list of embedding vectors, each being a list of floats.

        Raises:
            RuntimeError: If the embedding request fails.
        """
        if isinstance(input_texts, str):
            input_texts = [input_texts]

        c_arr = (ctypes.c_char_p * len(input_texts))()
        for i, text in enumerate(input_texts):
            c_arr[i] = text.encode('utf-8')

        c_res = CsilkAiEmbeddingsResponse()
        res_code = self._lib.csilk_ai_embeddings(self._ai, model.encode('utf-8'), c_arr,
                                                  len(input_texts), ctypes.byref(c_res))

        if res_code != 0:
            err = c_res.error_message.decode('utf-8') if c_res.error_message else "Unknown embeddings error"
            self._lib.csilk_ai_embeddings_response_free(ctypes.byref(c_res))
            raise RuntimeError(err)

        try:
            results = []
            dim = c_res.dimension
            for idx in range(c_res.count):
                start_ptr = ctypes.cast(c_res.values, ctypes.c_void_p).value + idx * dim * ctypes.sizeof(ctypes.c_float)
                floats_ptr = ctypes.cast(start_ptr, ctypes.POINTER(ctypes.c_float))
                results.append([floats_ptr[i] for i in range(dim)])
            return results
        finally:
            self._lib.csilk_ai_embeddings_response_free(ctypes.byref(c_res))


class AIContext:
    """Maintains a conversation history for chat interactions.

    Usage::

        ctx = AIContext(max_history=20)
        ctx.add("user", "Hello")
        ctx.add("assistant", "Hi! How can I help?")
        for msg in ctx.messages:
            print(msg["role"], ":", msg["content"])
    """

    def __init__(self, max_history: int = 0):
        """Create a new AI conversation context.

        Args:
            max_history: Maximum number of messages to retain
                (0 = unlimited).
        Raises:
            RuntimeError: If the context cannot be created.
        """
        self._lib = get_bindings()
        self._ctx = self._lib.csilk_ai_context_new(max_history)
        if not self._ctx:
            raise RuntimeError("Failed to create AIContext")

    def free(self):
        """Release the conversation context."""
        if self._ctx:
            self._lib.csilk_ai_context_free(self._ctx)
            self._ctx = None

    def __del__(self):
        self.free()

    def add(self, role: str, content: str):
        """Append a message to the conversation history.

        Args:
            role: Message role (e.g. ``"user"``, ``"assistant"``,
                ``"system"``).
            content: Message content.
        """
        self._lib.csilk_ai_context_add(self._ctx, role.encode('utf-8'), content.encode('utf-8'))

    def clear(self):
        """Remove all messages from the conversation history."""
        self._lib.csilk_ai_context_clear(self._ctx)

    @property
    def count(self) -> int:
        """Return the number of messages in the history."""
        if not self._ctx:
            return 0
        ctx_struct = ctypes.cast(self._ctx, ctypes.POINTER(CsilkAiContext)).contents
        return ctx_struct.count

    @property
    def messages(self) -> list:
        """Return a list of ``{"role": str, "content": str}`` dicts."""
        if not self._ctx:
            return []
        ctx_struct = ctypes.cast(self._ctx, ctypes.POINTER(CsilkAiContext)).contents
        res = []
        for i in range(ctx_struct.count):
            msg = ctx_struct.messages[i]
            res.append({
                "role": msg.role.decode('utf-8') if msg.role else "",
                "content": msg.content.decode('utf-8') if msg.content else ""
            })
        return res
