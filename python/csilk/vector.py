"""Vector database interface.

Provides a ``VectorDB`` class that wraps the csilk C vector database
layer (Qdrant, Milvus, etc.) for upserting points and performing
similarity search.
"""

import ctypes
import json
from csilk.lib import get_bindings, CsilkVectorPoint, CsilkVectorSearchResponse


class VectorDB:
    """Client for a vector database backed by a csilk driver.

    Usage::

        vdb = VectorDB("qdrant", "http://localhost:6333", api_key="...")
        vdb.upsert("my_collection", [
            {"id": "doc1", "vector": [0.1, 0.2, ...], "payload": {"text": "hello"}}
        ])
        results = vdb.search("my_collection", [0.1, 0.2, ...], limit=5)
    """

    def __init__(self, driver_name: str, endpoint: str, api_key: str = None):
        """Open a connection to a vector database.

        Args:
            driver_name: Driver identifier (e.g. ``"qdrant"``, ``"milvus"``).
            endpoint: Server URL.
            api_key: Optional API key.
        Raises:
            RuntimeError: If the connection cannot be established.
        """
        self._lib = get_bindings()
        self._db = self._lib.csilk_vector_db_new(
            driver_name.encode('utf-8'),
            endpoint.encode('utf-8'),
            api_key.encode('utf-8') if api_key else None,
        )
        if not self._db:
            raise RuntimeError(
                f"Failed to create Vector DB connection for driver '{driver_name}' at '{endpoint}'"
            )

    def __del__(self):
        if hasattr(self, "_db") and self._db:
            self._lib.csilk_vector_db_free(self._db)
            self._db = None

    def upsert(self, collection: str, points: list):
        """Upsert points into a collection.

        Args:
            collection: Collection name.
            points: List of point dicts::

                [
                  {
                    "id": "uuid-string",
                    "vector": [0.1, 0.2, ...],
                    "payload": {"key": "value"}   # optional
                  }
                ]

        Raises:
            RuntimeError: If the upsert fails.
        """
        count = len(points)
        c_points = (CsilkVectorPoint * count)()

        # Keep references to keep memory alive during call
        refs = []

        for i, pt in enumerate(points):
            pt_id = pt["id"].encode('utf-8')
            vector_data = pt["vector"]
            dim = len(vector_data)
            c_float_array = (ctypes.c_float * dim)(*vector_data)

            payload_json = pt.get("payload")
            c_payload = None
            if payload_json is not None:
                json_str = json.dumps(payload_json).encode('utf-8')
                c_payload = self._lib.cJSON_Parse(json_str)
                if not c_payload:
                    for j in range(i):
                        if c_points[j].payload:
                            self._lib.cJSON_Delete(c_points[j].payload)
                    raise ValueError("Failed to parse payload JSON")

            c_points[i].id = pt_id
            c_points[i].vector = ctypes.cast(c_float_array, ctypes.POINTER(ctypes.c_float))
            c_points[i].dimension = dim
            c_points[i].payload = c_payload

            refs.append((pt_id, c_float_array))

        res = self._lib.csilk_vector_db_upsert(
            self._db,
            collection.encode('utf-8'),
            c_points,
            count,
        )

        for i in range(count):
            if c_points[i].payload:
                self._lib.cJSON_Delete(c_points[i].payload)

        if res != 0:
            raise RuntimeError("Failed to upsert vector points")

    def search(self, collection: str, vector: list, limit: int = 5) -> list:
        """Search for the nearest neighbours of a vector.

        Args:
            collection: Collection name.
            vector: Query vector as a list of floats.
            limit: Maximum number of results.

        Returns:
            A list of result dicts::

                [
                  {
                    "id": "uuid-string",
                    "score": 0.95,
                    "payload": {...}
                  }
                ]

        Raises:
            RuntimeError: If the search fails.
        """
        dim = len(vector)
        c_float_array = (ctypes.c_float * dim)(*vector)

        c_res = CsilkVectorSearchResponse()

        res = self._lib.csilk_vector_db_search(
            self._db,
            collection.encode('utf-8'),
            c_float_array,
            dim,
            limit,
            ctypes.byref(c_res),
        )

        if res != 0:
            err_msg = c_res.error_message.decode('utf-8') if c_res.error_message else "Unknown error"
            self._lib.csilk_vector_search_response_free(ctypes.byref(c_res))
            raise RuntimeError(f"Vector search failed: {err_msg}")

        results = []
        try:
            for i in range(c_res.count):
                item = c_res.results[i]
                item_id = item.id.decode('utf-8') if item.id else ""
                score = item.score

                payload = None
                if item.payload:
                    c_str_ptr = self._lib.cJSON_PrintUnformatted(item.payload)
                    if c_str_ptr:
                        json_str = ctypes.string_at(c_str_ptr).decode('utf-8')
                        self._lib.csilk_free(c_str_ptr)
                        payload = json.loads(json_str)

                results.append({
                    "id": item_id,
                    "score": score,
                    "payload": payload,
                })
        finally:
            self._lib.csilk_vector_search_response_free(ctypes.byref(c_res))

        return results
