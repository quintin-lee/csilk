import pytest
from csilk.vector import VectorDB

class TestVectorDB:
    def test_invalid_driver(self):
        with pytest.raises(RuntimeError) as exc:
            VectorDB("invalid_driver", "http://localhost:6333")
        assert "Failed to create Vector DB connection" in str(exc.value)

    def test_upsert_failure(self):
        vdb = VectorDB("qdrant", "http://localhost:12345")
        points = [
            {"id": "doc1", "vector": [0.1, 0.2], "payload": {"text": "hello"}}
        ]
        with pytest.raises(RuntimeError):
            vdb.upsert("my_collection", points)

    def test_search_failure(self):
        vdb = VectorDB("qdrant", "http://localhost:12345")
        with pytest.raises(RuntimeError):
            vdb.search("my_collection", [0.1, 0.2], limit=5)
