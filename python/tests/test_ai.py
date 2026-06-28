import pytest
from csilk.ai import AI, AIContext

class TestAI:
    def test_invalid_driver(self):
        with pytest.raises(RuntimeError) as exc:
            AI("invalid_driver_name")
        assert "Failed to create AI instance for driver 'invalid_driver_name'" in str(exc.value)

    def test_ai_context(self):
        ctx = AIContext(max_history=5)
        ctx.add("system", "You are a helpful assistant.")
        ctx.add("user", "Hello")
        
        messages = ctx.messages
        assert len(messages) == 2
        assert messages[0]["role"] == "system"
        assert messages[0]["content"] == "You are a helpful assistant."
        assert messages[1]["role"] == "user"
        assert messages[1]["content"] == "Hello"
        
        ctx.free()
        
    def test_ai_chat_failure(self):
        # We don't have a valid API key, so this should fail
        ai = AI("openai", api_key="invalid", base_url="http://localhost:12345")
        with pytest.raises(RuntimeError):
            ai.chat("gpt-4", [{"role": "user", "content": "hi"}], timeout_ms=10)
        ai.free()
        
    def test_ai_embeddings_failure(self):
        ai = AI("openai", api_key="invalid", base_url="http://localhost:12345")
        with pytest.raises(RuntimeError):
            ai.generate_embeddings("text-embedding-3-small", ["hello"])
        ai.free()
        
    def test_get_stats(self):
        stats = AI.get_stats()
        assert "requests_total" in stats
        assert "tokens_total" in stats
