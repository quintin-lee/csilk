#!/usr/bin/env python3
"""Reproducible built-in C benchmark runner and JSON aggregator for csilk."""

from __future__ import annotations

import argparse
import datetime
import json
import os
import platform
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def detect_version() -> str:
    opts = (ROOT / "cmake" / "options.cmake").read_text(encoding="utf-8")
    maj = re.search(r'CSILK_VERSION_MAJOR\s+(\d+)', opts)
    min_ = re.search(r'CSILK_VERSION_MINOR\s+(\d+)', opts)
    pat = re.search(r'CSILK_VERSION_PATCH\s+(\d+)', opts)
    if maj and min_ and pat:
        return f"{maj.group(1)}.{min_.group(1)}.{pat.group(1)}"
    return "1.2.0"


def detect_git_hash() -> str:
    try:
        out = subprocess.check_output(
            ["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, text=True
        ).strip()
        return out or "unknown"
    except Exception:
        return "unknown"


def detect_cpu() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text(encoding="utf-8").splitlines():
            if line.lower().startswith("model name"):
                return line.split(":", 1)[1].strip()
    except Exception:
        pass
    return platform.processor() or platform.machine()


def detect_compiler(build_dir: Path) -> str:
    cache = build_dir / "CMakeCache.txt"
    cc = "cc"
    if cache.exists():
        for line in cache.read_text(encoding="utf-8", errors="ignore").splitlines():
            if line.startswith("CMAKE_C_COMPILER:FILEPATH="):
                cc = line.split("=", 1)[1].strip()
                break
    try:
        return subprocess.check_output([cc, "--version"], text=True).splitlines()[0].strip()
    except Exception:
        return cc


def run_bin(build_dir: Path, name: str) -> str:
    exe = build_dir / name
    if not exe.exists():
        raise FileNotFoundError(f"Benchmark binary not found: {exe}")
    res = subprocess.run([str(exe)], cwd=build_dir, text=True, capture_output=True, check=True)
    return res.stdout


def parse_e2e(text: str) -> list[dict]:
    rows = []
    pat = re.compile(
        r"HTTP1_E2E workers=(\d+) clients=(\d+) rounds=(\d+) requests=(\d+) "
        r"qps=([\d.]+) qps_ci95=\+/-([\d.]+) p50_ns=(\d+) p95_ns=(\d+) p99_ns=(\d+)"
    )
    for m in pat.finditer(text):
        rows.append(
            {
                "workers": int(m.group(1)),
                "clients": int(m.group(2)),
                "rounds": int(m.group(3)),
                "requests": int(m.group(4)),
                "qps": float(m.group(5)),
                "qps_ci95": float(m.group(6)),
                "p50_ns": int(m.group(7)),
                "p95_ns": int(m.group(8)),
                "p99_ns": int(m.group(9)),
            }
        )
    return rows


def parse_pipeline(text: str) -> dict:
    stages: dict[str, list[float]] = {}
    for m in re.finditer(r"HTTP1 stage=(\S+) operations=\d+ ns_per_op=([\d.]+)", text):
        stages.setdefault(m.group(1), []).append(float(m.group(2)))
    return {k: round(sum(v) / len(v), 2) for k, v in stages.items()}


def parse_h2(text: str) -> dict:
    rounds = []
    for m in re.finditer(
        r"H2_ROUNDS streams=(\d+) rounds=(\d+) insert_median_ns=([\d.]+) "
        r"lookup_median_ns=([\d.]+) delete_median_ns=([\d.]+)",
        text,
    ):
        rounds.append(
            {
                "streams": int(m.group(1)),
                "rounds": int(m.group(2)),
                "insert_median_ns": float(m.group(3)),
                "lookup_median_ns": float(m.group(4)),
                "delete_median_ns": float(m.group(5)),
            }
        )
    pool_m = re.search(r"Pool throughput:\s+([\d.]+) ns/cycle", text)
    return {
        "pool_ns_per_cycle": float(pool_m.group(1)) if pool_m else None,
        "rounds": rounds,
    }


def parse_router(text: str) -> list[dict]:
    rows = []
    pat = re.compile(
        r"Scale:\s+(\d+) routes \| Nodes:\s+(\d+) \(Overflow:\s+(\d+)\).*?"
        r"Lookup Perf :\s+([\d.]+) ns/op \(\s*(\d+) ops/sec",
        re.S,
    )
    for m in pat.finditer(text):
        rows.append(
            {
                "routes": int(m.group(1)),
                "nodes": int(m.group(2)),
                "overflow": int(m.group(3)),
                "lookup_ns": float(m.group(4)),
                "ops_per_sec": int(m.group(5)),
            }
        )
    return rows


def parse_arena(text: str) -> list[dict]:
    rows = []
    for m in re.finditer(
        r"(\d+) bytes \|\s+([\d.]+) ns/alloc \|\s+([\d.]+) cycles/alloc \|\s+([\d.]+) M ops/sec",
        text,
    ):
        rows.append(
            {
                "size_bytes": int(m.group(1)),
                "ns_per_alloc": float(m.group(2)),
                "cycles_per_alloc": float(m.group(3)),
                "mops_per_sec": float(m.group(4)),
            }
        )
    return rows


def parse_header_map(text: str) -> dict:
    modes = {}
    for m in re.finditer(
        r"\d+\.\s+(.+?)\s*\|\s+([\d.]+)\s*\|\s+([\d.]+)\s*\|\s+([\d.]+)\s*\|",
        text,
    ):
        modes[m.group(1).strip()] = {
            "total_cycles_per_req": float(m.group(2)),
            "cycles_per_hdr": float(m.group(3)),
            "ns_per_req": float(m.group(4)),
        }
    return modes


def parse_dispatch(text: str) -> list[dict]:
    rows = []
    for m in re.finditer(
        r"\[\s*(\d+) Producers\]\s+(\d+) ops \| Dispatch:\s+([\d.]+) Kops/s \(\s*([\d.]+) ns/op\) \| E2E:\s+([\d.]+) Kops/s",
        text,
    ):
        rows.append(
            {
                "producers": int(m.group(1)),
                "ops": int(m.group(2)),
                "dispatch_kops_per_sec": float(m.group(3)),
                "ns_per_op": float(m.group(4)),
                "e2e_kops_per_sec": float(m.group(5)),
            }
        )
    return rows


def parse_uring_recv(text: str) -> list[dict]:
    rows = []
    for m in re.finditer(
        r"^\s+(.+?)\s+\|\s+(\d+) reqs \(\s*(\d+) conn\) \|\s+([\d.]+) req/s \| p50:\s+([\d.]+) us \| p95:\s+([\d.]+) us \| p99:\s+([\d.]+) us",
        text,
        re.M,
    ):
        rows.append(
            {
                "scenario": m.group(1).strip(),
                "requests": int(m.group(2)),
                "connections": int(m.group(3)),
                "req_per_sec": float(m.group(4)),
                "p50_us": float(m.group(5)),
                "p95_us": float(m.group(6)),
                "p99_us": float(m.group(7)),
            }
        )
    return rows


def parse_json_accessor(text: str) -> dict:
    vo = re.search(r"Value Object.*?:\s+([\d.]+) ns/lookup\s+\(\s*([\d.]+) M ops/sec\)", text)
    pv = re.search(r"Pointer View.*?:\s+([\d.]+) ns/lookup\s+\(\s*([\d.]+) M ops/sec\)", text)
    return {
        "value_object_ns": float(vo.group(1)) if vo else None,
        "value_object_mops": float(vo.group(2)) if vo else None,
        "pointer_view_ns": float(pv.group(1)) if pv else None,
        "pointer_view_mops": float(pv.group(2)) if pv else None,
    }


def parse_server_stats(text: str) -> dict:
    tp = re.search(r"Query Throughput:\s+([\d.]+) M queries/sec", text)
    lat = re.search(r"Avg Query Latency:\s+([\d.]+) ns/query", text)
    return {
        "throughput_mqps": float(tp.group(1)) if tp else None,
        "avg_latency_ns": float(lat.group(1)) if lat else None,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uv-build-dir", required=True, type=Path, help="Release build dir using libuv")
    ap.add_argument("--uring-build-dir", type=Path, default=None, help="Optional Release build dir using CSILK_USE_URING=ON")
    ap.add_argument("--output", required=True, type=Path, help="Output JSON path")
    args = ap.parse_args()

    payload = {
        "meta": {
            "version": detect_version(),
            "git_hash": detect_git_hash(),
            "timestamp": datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
            "host": {
                "os": platform.system(),
                "kernel": platform.release(),
                "arch": platform.machine(),
                "cpu": detect_cpu(),
                "logical_cores": os.cpu_count(),
            },
            "compiler": detect_compiler(args.uv_build_dir),
            "cmake_profile": "Release + -DCSILK_ENABLE_NATIVE_ARCH=ON",
        },
        "libuv": {
            "http1_e2e": parse_e2e(run_bin(args.uv_build_dir, "test_http1_e2e_bench")),
            "http1_pipeline_ns": parse_pipeline(run_bin(args.uv_build_dir, "test_http1_pipeline_bench")),
            "h2_stream": parse_h2(run_bin(args.uv_build_dir, "test_h2_stream_bench")),
            "router_trie": parse_router(run_bin(args.uv_build_dir, "test_router_bench")),
            "arena": parse_arena(run_bin(args.uv_build_dir, "test_arena_bench")),
            "header_map": parse_header_map(run_bin(args.uv_build_dir, "test_header_map_bench")),
            "json_accessor": parse_json_accessor(run_bin(args.uv_build_dir, "test_json_accessor_bench")),
            "dispatch": parse_dispatch(run_bin(args.uv_build_dir, "test_dispatch_bench")),
            "server_stats": parse_server_stats(run_bin(args.uv_build_dir, "test_server_stats_bench")),
        },
    }

    if args.uring_build_dir is not None:
        payload["io_uring"] = {
            "http1_e2e": parse_e2e(run_bin(args.uring_build_dir, "test_http1_e2e_bench")),
            "uring_recv": parse_uring_recv(run_bin(args.uring_build_dir, "test_uring_recv_bench")),
            "dispatch": parse_dispatch(run_bin(args.uring_build_dir, "test_dispatch_bench")),
        }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")
    print(f"Wrote reproducible benchmark JSON: {args.output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
