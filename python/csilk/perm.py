from csilk.lib import get_bindings

class Perm:
    _rule_refs = []

    @staticmethod
    def init():
        lib = get_bindings()
        lib.csilk_perm_init()

    @staticmethod
    def set_default(driver_name):
        lib = get_bindings()
        res = lib.csilk_perm_set_default(driver_name.encode('utf-8'))
        if res != 0:
            raise RuntimeError(f"Failed to set default permission driver to '{driver_name}'")

    @staticmethod
    def check(ctx, permission, resource):
        """
        Check if the current context has the given permission on the resource.
        Returns:
            0 if allowed, non-zero if denied
        """
        lib = get_bindings()
        return lib.csilk_perm_check(ctx._ctx, permission.encode('utf-8'), resource.encode('utf-8'))

    @staticmethod
    def require(ctx, permission, resource):
        """
        Require permission on resource, aborts the context with 403 Forbidden if check fails.
        """
        lib = get_bindings()
        lib.csilk_perm_require(ctx._ctx, permission.encode('utf-8'), resource.encode('utf-8'))

    @staticmethod
    def simple_init():
        lib = get_bindings()
        lib.csilk_perm_simple_init()

    @staticmethod
    def simple_allow(role, permission, resource):
        """
        Add a rule to the built-in simple RBAC driver.
        Returns:
            0 on success, non-zero on failure
        """
        lib = get_bindings()
        role_bytes = role.encode('utf-8')
        perm_bytes = permission.encode('utf-8')
        res_bytes = resource.encode('utf-8')
        
        Perm._rule_refs.append((role_bytes, perm_bytes, res_bytes))
        
        return lib.csilk_perm_simple_allow(
            role_bytes,
            perm_bytes,
            res_bytes
        )

    @staticmethod
    def simple_clear():
        lib = get_bindings()
        Perm._rule_refs.clear()
        lib.csilk_perm_simple_clear()

    @staticmethod
    def auto_middleware(ctx):
        """
        Automatic permission-check middleware using metadata of the route.
        """
        lib = get_bindings()
        lib.csilk_perm_auto_middleware(ctx._ctx)
