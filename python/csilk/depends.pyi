from typing import Any, Callable, Dict, Optional

from csilk.context import Context

class Depends:
    dependency: Optional[Callable[..., Any]]
    def __init__(self, dependency: Optional[Callable[..., Any]] = ...) -> None: ...

async def resolve_async_dependency(
    dep_func: Callable[..., Any],
    ctx: Context,
    cache: Dict[Callable[..., Any], Any],
) -> Any: ...

def resolve_sync_dependency(
    dep_func: Callable[..., Any],
    ctx: Context,
    cache: Dict[Callable[..., Any], Any],
) -> Any: ...

def inject(handler: Callable[..., Any]) -> Callable[..., Any]: ...
