"""Dependency injection for csilk route handlers.

Provides a ``Depends`` marker and an ``inject`` wrapper that resolves
nested dependencies recursively, similar to FastAPI's ``Depends``.
"""

import inspect
import asyncio
from csilk.context import Context


class Depends:
    """Marker for a parameter that should be resolved as a dependency.

    Usage::

        def get_db(ctx: Context):
            return DBPool("sqlite", "test.db")

        @app.get("/users")
        def list_users(ctx: Context, db: Depends(get_db)):
            rows = db.query("SELECT * FROM users")
            ctx.json(200, rows)

    Dependencies can be nested — a dependency function may itself declare
    ``Depends`` parameters and they will be resolved recursively.
    """

    def __init__(self, dependency=None):
        self.dependency = dependency


async def resolve_async_dependency(dep_func, ctx: Context, cache: dict):
    """Resolve a single async (or sync) dependency function, recursing into its
    own sub-dependencies.  Results are cached so each function is called once.

    Args:
        dep_func: The dependency callable.
        ctx: The request context — injected for ``Context``-typed params.
        cache: Shared dict mapping resolved functions to their results.

    Returns:
        The return value of *dep_func*.
    """
    if dep_func in cache:
        return cache[dep_func]

    sig = inspect.signature(dep_func)
    kwargs = {}
    for param_name, param in sig.parameters.items():
        if isinstance(param.default, Depends):
            sub_dep = param.default.dependency or param.annotation
            if sub_dep is inspect.Parameter.empty:
                raise ValueError(
                    f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated."
                )
            kwargs[param_name] = await resolve_async_dependency(sub_dep, ctx, cache)
        elif param.annotation == Context or param_name == "ctx":
            kwargs[param_name] = ctx

    if inspect.iscoroutinefunction(dep_func):
        result = await dep_func(**kwargs)
    else:
        result = dep_func(**kwargs)

    cache[dep_func] = result
    return result


def resolve_sync_dependency(dep_func, ctx: Context, cache: dict):
    """Resolve a single sync dependency function, recursing into sub-dependencies.

    Args:
        dep_func: The dependency callable.
        ctx: The request context.
        cache: Shared result cache.

    Returns:
        The return value of *dep_func*.

    Raises:
        RuntimeError: If *dep_func* is async but called from a sync handler.
    """
    if dep_func in cache:
        return cache[dep_func]

    sig = inspect.signature(dep_func)
    kwargs = {}
    for param_name, param in sig.parameters.items():
        if isinstance(param.default, Depends):
            sub_dep = param.default.dependency or param.annotation
            if sub_dep is inspect.Parameter.empty:
                raise ValueError(
                    f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated."
                )
            kwargs[param_name] = resolve_sync_dependency(sub_dep, ctx, cache)
        elif param.annotation == Context or param_name == "ctx":
            kwargs[param_name] = ctx

    if inspect.iscoroutinefunction(dep_func):
        raise RuntimeError(
            f"Cannot resolve async dependency '{dep_func.__name__}' in a synchronous handler."
        )

    result = dep_func(**kwargs)
    cache[dep_func] = result
    return result


def inject(handler):
    """Decorator that introspects the handler's signature and automatically
    resolves ``Depends`` markers.

    If the handler has no ``Depends`` parameters, it is returned unchanged.
    Otherwise a wrapper is created that resolves each dependency (and any
    transitive sub-dependencies) before calling the original handler.

    Args:
        handler: A synchronous or asynchronous route handler function.

    Returns:
        The original handler (if no injection needed) or a wrapper.
    """
    sig = inspect.signature(handler)

    # Check if we need dependency injection
    needs_injection = any(isinstance(p.default, Depends) for p in sig.parameters.values())
    if not needs_injection:
        return handler

    is_coro = inspect.iscoroutinefunction(handler)

    if is_coro:

        async def async_wrapper(ctx: Context, *args, **kwargs):
            cache = {}
            for param_name, param in sig.parameters.items():
                if isinstance(param.default, Depends):
                    dep_func = param.default.dependency or param.annotation
                    if dep_func is inspect.Parameter.empty:
                        raise ValueError(
                            f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated."
                        )
                    kwargs[param_name] = await resolve_async_dependency(dep_func, ctx, cache)
                elif param.annotation == Context or param_name == "ctx":
                    kwargs[param_name] = ctx
            return await handler(*args, **kwargs)

        # Preserve original signature for other decorators (like @app.validate)
        async_wrapper.__signature__ = sig
        async_wrapper.__name__ = getattr(handler, "__name__", "async_wrapper")
        return async_wrapper
    else:

        def sync_wrapper(ctx: Context, *args, **kwargs):
            cache = {}
            for param_name, param in sig.parameters.items():
                if isinstance(param.default, Depends):
                    dep_func = param.default.dependency or param.annotation
                    if dep_func is inspect.Parameter.empty:
                        raise ValueError(
                            f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated."
                        )
                    kwargs[param_name] = resolve_sync_dependency(dep_func, ctx, cache)
                elif param.annotation == Context or param_name == "ctx":
                    kwargs[param_name] = ctx
            try:
                return handler(*args, **kwargs)
            except Exception as e:
                import traceback

                traceback.print_exc()
                raise e

        sync_wrapper.__signature__ = sig
        sync_wrapper.__name__ = getattr(handler, "__name__", "sync_wrapper")
        return sync_wrapper
