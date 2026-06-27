import inspect
import asyncio
from csilk.context import Context

class Depends:
    """Dependency injection marker."""
    def __init__(self, dependency=None):
        self.dependency = dependency


async def resolve_async_dependency(dep_func, ctx: Context, cache: dict):
    if dep_func in cache:
        return cache[dep_func]
    
    sig = inspect.signature(dep_func)
    kwargs = {}
    for param_name, param in sig.parameters.items():
        if isinstance(param.default, Depends):
            sub_dep = param.default.dependency or param.annotation
            if sub_dep is inspect.Parameter.empty:
                raise ValueError(f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated.")
            kwargs[param_name] = await resolve_async_dependency(sub_dep, ctx, cache)
        elif param.annotation == Context or param_name == 'ctx':
            kwargs[param_name] = ctx
            
    if inspect.iscoroutinefunction(dep_func):
        result = await dep_func(**kwargs)
    else:
        result = dep_func(**kwargs)
        
    cache[dep_func] = result
    return result


def resolve_sync_dependency(dep_func, ctx: Context, cache: dict):
    if dep_func in cache:
        return cache[dep_func]
        
    sig = inspect.signature(dep_func)
    kwargs = {}
    for param_name, param in sig.parameters.items():
        if isinstance(param.default, Depends):
            sub_dep = param.default.dependency or param.annotation
            if sub_dep is inspect.Parameter.empty:
                raise ValueError(f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated.")
            kwargs[param_name] = resolve_sync_dependency(sub_dep, ctx, cache)
        elif param.annotation == Context or param_name == 'ctx':
            kwargs[param_name] = ctx
            
    if inspect.iscoroutinefunction(dep_func):
        raise RuntimeError(f"Cannot resolve async dependency '{dep_func.__name__}' in a synchronous handler.")
        
    result = dep_func(**kwargs)
    cache[dep_func] = result
    return result


def inject(handler):
    """
    Decorator/wrapper that introspects the handler's signature and automatically
    resolves and injects dependencies marked with `Depends`.
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
                        raise ValueError(f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated.")
                    kwargs[param_name] = await resolve_async_dependency(dep_func, ctx, cache)
                elif param.annotation == Context or param_name == 'ctx':
                    kwargs[param_name] = ctx
            return await handler(*args, **kwargs)
        
        # Preserve original signature for other decorators (like @app.validate)
        async_wrapper.__signature__ = sig
        async_wrapper.__name__ = getattr(handler, '__name__', 'async_wrapper')
        return async_wrapper
    else:
        def sync_wrapper(ctx: Context, *args, **kwargs):
            cache = {}
            for param_name, param in sig.parameters.items():
                if isinstance(param.default, Depends):
                    dep_func = param.default.dependency or param.annotation
                    if dep_func is inspect.Parameter.empty:
                        raise ValueError(f"Dependency for parameter '{param_name}' must be explicitly provided or type-annotated.")
                    kwargs[param_name] = resolve_sync_dependency(dep_func, ctx, cache)
                elif param.annotation == Context or param_name == 'ctx':
                    kwargs[param_name] = ctx
            try:
                return handler(*args, **kwargs)
            except Exception as e:
                import traceback
                traceback.print_exc()
                raise e
            
        sync_wrapper.__signature__ = sig
        sync_wrapper.__name__ = getattr(handler, '__name__', 'sync_wrapper')
        return sync_wrapper
