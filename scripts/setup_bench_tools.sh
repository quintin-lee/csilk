#!/usr/bin/env bash
#
# Install the full benchmark toolchain: perf, wrk, FlameGraph.
# Usage: ./scripts/setup_bench_tools.sh [--ci]
#
# --ci: Skip interactive prompts, suitable for CI.
set -euo pipefail

CI_MODE=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ci) CI_MODE=1 ;;
        *) echo "Usage: $0 [--ci]"; exit 1 ;;
    esac
    shift
done

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
FLAMEGRAPH_DIR="${ROOT_DIR}/tools/FlameGraph"

echo "=== Benchmark Toolchain Setup ==="

# ── 1. wrk ──────────────────────────────────────────────────────
if command -v wrk &>/dev/null; then
    echo "[ok] wrk: $(wrk --version 2>&1 | head -1)"
else
    echo "[..] Installing wrk..."
    if [[ "$(uname -s)" == "Linux" ]]; then
        sudo apt-get install -y wrk
    elif [[ "$(uname -s)" == "Darwin" ]]; then
        brew install wrk
    fi
    echo "[ok] wrk installed: $(wrk --version 2>&1 | head -1)"
fi

# ── 2. perf ─────────────────────────────────────────────────────
if command -v perf &>/dev/null; then
    echo "[ok] perf: $(perf --version 2>&1 | head -1)"
else
    echo "[..] Installing perf..."
    if [[ "$(uname -s)" == "Linux" ]]; then
        KERNEL_VER=$(uname -r)
        sudo apt-get install -y linux-tools-common linux-tools-generic
        # Some Ubuntu images have a version-specific package
        sudo apt-get install -y "linux-tools-${KERNEL_VER%%-*}-${KERNEL_VER##*-}" 2>/dev/null || true
        # If perf is still missing, try perf-tools-unstable as fallback
        if ! command -v perf &>/dev/null; then
            sudo apt-get install -y linux-tools-${KERNEL_VER%%-*} 2>/dev/null || true
        fi
    else
        echo "[warn] perf is Linux-only; skipping"
    fi
    if command -v perf &>/dev/null; then
        echo "[ok] perf installed"
    fi
fi

# ── 3. FlameGraph (Brendan Gregg's stack visualization scripts) ─
if [[ -d "$FLAMEGRAPH_DIR" && -f "$FLAMEGRAPH_DIR/flamegraph.pl" ]]; then
    echo "[ok] FlameGraph: $FLAMEGRAPH_DIR ($(git -C "$FLAMEGRAPH_DIR" rev-parse --short HEAD 2>/dev/null || echo 'unknown'))"
else
    echo "[..] Cloning FlameGraph..."
    mkdir -p "$(dirname "$FLAMEGRAPH_DIR")"
    if [[ -d "$FLAMEGRAPH_DIR" ]]; then
        git -C "$FLAMEGRAPH_DIR" pull --ff-only 2>/dev/null || true
    else
        git clone --depth=1 https://github.com/brendangregg/FlameGraph.git "$FLAMEGRAPH_DIR"
    fi
    echo "[ok] FlameGraph cloned to $FLAMEGRAPH_DIR"
fi

# ── Check kernel.perf_event_paranoid for perf (Linux) ───────────
if [[ "$(uname -s)" == "Linux" ]]; then
    PARANOID=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo "2")
    if [[ "$PARANOID" -gt 1 && "$CI_MODE" -eq 0 ]]; then
        echo ""
        echo "[warn] /proc/sys/kernel/perf_event_paranoid = ${PARANOID}"
        echo "       perf may need elevated privileges or sysctl setting."
        echo "       Run: sudo sysctl kernel.perf_event_paranoid=1"
    elif [[ "$PARANOID" -gt 1 && "$CI_MODE" -eq 1 ]]; then
        echo "[..] CI: perf_event_paranoid=${PARANOID}; perf may be limited"
    fi
fi

echo ""
echo "=== Setup complete ==="
echo "  wrk:        $(command -v wrk || echo 'not found')"
echo "  perf:       $(command -v perf || echo 'not found')"
echo "  FlameGraph: ${FLAMEGRAPH_DIR}"
