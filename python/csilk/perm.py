"""Permission and role-based access control (RBAC) system.

Provides a pluggable permission-checking interface backed by the csilk C
engine, plus a built-in simple RBAC driver that can be used without an
external database.
"""

from csilk.lib import get_bindings


class Perm:
    """Static permission-checking helpers.

    Usage::

        Perm.init()
        Perm.simple_init()
        Perm.simple_allow("admin", "write", "orders")

        # In a middleware:
        Perm.require(ctx, "write", "orders")  # aborts with 403 if denied
    """

    _rule_refs = []  # Keep byte references alive to prevent GC

    @staticmethod
    def init():
        """Initialise the pluggable permission subsystem.

        Must be called once before any other ``Perm`` method.
        """
        lib = get_bindings()
        lib.csilk_perm_init()

    @staticmethod
    def set_default(driver_name: str):
        """Set the active permission driver by name.

        Args:
            driver_name: Driver identifier (e.g. ``"simple"``).
        Raises:
            RuntimeError: If the driver cannot be set.
        """
        lib = get_bindings()
        res = lib.csilk_perm_set_default(driver_name.encode('utf-8'))
        if res != 0:
            raise RuntimeError(f"Failed to set default permission driver to '{driver_name}'")

    @staticmethod
    def check(ctx, permission: str, resource: str) -> int:
        """Check whether the current context holds *permission* on *resource*.

        Args:
            ctx: The request context.
            permission: Permission string (e.g. ``"read"``, ``"write"``).
            resource: Target resource identifier (e.g. ``"orders"``).

        Returns:
            0 if allowed, non-zero if denied.
        """
        lib = get_bindings()
        return lib.csilk_perm_check(ctx._ctx, permission.encode('utf-8'), resource.encode('utf-8'))

    @staticmethod
    def require(ctx, permission: str, resource: str):
        """Require *permission* on *resource* — aborts with 403 if denied.

        This is a convenience wrapper around :meth:`check` that calls
        ``ctx.abort()`` when the check fails.

        Args:
            ctx: The request context.
            permission: Permission string.
            resource: Target resource identifier.
        """
        lib = get_bindings()
        lib.csilk_perm_require(ctx._ctx, permission.encode('utf-8'), resource.encode('utf-8'))

    @staticmethod
    def simple_init():
        """Initialise the built-in in-memory RBAC driver.

        Call before adding rules via :meth:`simple_allow`.
        """
        lib = get_bindings()
        lib.csilk_perm_simple_init()

    @staticmethod
    def simple_allow(role: str, permission: str, resource: str) -> int:
        """Grant *permission* on *resource* to *role*.

        Args:
            role: Role name (e.g. ``"admin"``).
            permission: Permission string.
            resource: Resource identifier.

        Returns:
            0 on success, non-zero on failure.
        """
        lib = get_bindings()
        role_bytes = role.encode('utf-8')
        perm_bytes = permission.encode('utf-8')
        res_bytes = resource.encode('utf-8')

        Perm._rule_refs.append((role_bytes, perm_bytes, res_bytes))

        return lib.csilk_perm_simple_allow(role_bytes, perm_bytes, res_bytes)

    @staticmethod
    def simple_clear():
        """Remove all rules from the in-memory RBAC driver."""
        lib = get_bindings()
        Perm._rule_refs.clear()
        lib.csilk_perm_simple_clear()

    @staticmethod
    def auto_middleware(ctx):
        """Automatic permission-check middleware using per-route metadata.

        Reads ``perm_required`` and ``perm_resource`` from the matched
        route definition and checks them against the current context.
        """
        lib = get_bindings()
        lib.csilk_perm_auto_middleware(ctx._ctx)
