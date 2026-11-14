"""Automated ABI layout check comparing C structs against Python ctypes.Structure."""

import ctypes
import glob
import json
import os
import subprocess
import sys
import unittest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
sys.path.insert(0, os.path.join(REPO_ROOT, "python"))

import csilk.lib as csilk_lib


def _find_probe_binary():
    env_bin = os.environ.get("CSILK_ABI_PROBE_BIN")
    if env_bin:
        return env_bin

    exe_suffix = ".exe" if sys.platform.startswith("win") else ""
    candidates = [
        os.path.join(REPO_ROOT, "build", f"test_python_abi{exe_suffix}"),
        os.path.join(REPO_ROOT, "build_uring", f"test_python_abi{exe_suffix}"),
    ]
    candidates.extend(
        sorted(glob.glob(os.path.join(REPO_ROOT, "build*", f"test_python_abi{exe_suffix}")))
    )
    for cand in candidates:
        if os.path.isfile(cand) and os.access(cand, os.X_OK):
            return cand
    return None


class TestPythonCAbiLayout(unittest.TestCase):
    def test_ctypes_struct_layouts_match_c(self):
        probe_bin = _find_probe_binary()
        if not probe_bin or not os.path.isfile(probe_bin):
            if os.environ.get("CSILK_REQUIRE_ABI_PROBE") == "1":
                self.fail(f"Required ABI probe binary not found: {probe_bin!r}")
            self.skipTest("test_python_abi probe binary not built")

        probe_env = {
            k: v
            for k, v in os.environ.items()
            if k not in ("LD_PRELOAD", "DYLD_INSERT_LIBRARIES")
        }
        proc = subprocess.run(
            [probe_bin, "--dump-json"],
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
            env=probe_env,
        )
        c_structs = json.loads(proc.stdout)
        self.assertGreaterEqual(len(c_structs), 23)

        # Every non-opaque ctypes.Structure defined in csilk.lib must be covered.
        py_named_structs = {}
        for name, obj in vars(csilk_lib).items():
            if (
                isinstance(obj, type)
                and issubclass(obj, ctypes.Structure)
                and obj is not ctypes.Structure
                and hasattr(obj, "_fields_")
            ):
                py_named_structs[name] = obj

        self.assertEqual(
            set(py_named_structs.keys()),
            set(c_structs.keys()),
            "Mismatch between csilk.lib ctypes.Structure classes and C ABI probe list",
        )

        for py_name, spec in c_structs.items():
            cls = py_named_structs[py_name]
            with self.subTest(struct=py_name, c_type=spec["c_name"]):
                self.assertEqual(
                    ctypes.sizeof(cls),
                    spec["size"],
                    f"sizeof mismatch for {py_name} ({spec['c_name']})",
                )
                py_field_names = [field[0] for field in cls._fields_]
                c_field_names = [field[0] for field in spec["fields"]]
                self.assertEqual(
                    py_field_names,
                    c_field_names,
                    f"field list mismatch for {py_name}",
                )
                for field_name, c_offset, c_size in spec["fields"]:
                    descriptor = getattr(cls, field_name)
                    self.assertEqual(
                        descriptor.offset,
                        c_offset,
                        f"offsetof({spec['c_name']}, {field_name}) mismatch",
                    )
                    self.assertEqual(
                        descriptor.size,
                        c_size,
                        f"sizeof({spec['c_name']}.{field_name}) mismatch",
                    )


if __name__ == "__main__":
    unittest.main()
