from typing import Any, Callable, Dict

class WorkflowWorker:
    mq: Any
    topic_tasks: str
    topic_results: str
    handlers: Dict[str, Callable[[str], Any]]
    def __init__(
        self,
        mq: Any,
        topic_tasks: str = ...,
        topic_results: str = ...,
    ) -> None: ...
    def register(self, node_id: str, handler_fn: Callable[[str], Any]) -> None: ...
    def start(self) -> None: ...
