#!/usr/bin/env bash
#
# Install the full benchmark toolchain: perf, wrk, FlameGraph.
# Usage: ./scripts/setup_bench_tools.sh [--ci] [--bench-only]
#
# --ci:         Skip interactive prompts, suitable for CI.
# --bench-only: Only verify/install wrk; skip perf and FlameGraph.
set -euo pipefail

CI_MODE=0
BENCH_ONLY=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --ci) CI_MODE=1 ;;
        --bench-only) BENCH_ONLY=1 ;;
        *) echo "Usage: $0 [--ci] [--bench-only]"; exit 1 ;;
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

if [[ "$BENCH_ONLY" -eq 1 ]]; then
    echo ""
    echo "=== Setup complete (bench-only) ==="
    echo "  wrk: $(command -v wrk || echo 'not found')"
    exit 0
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
    echo "[ok] FlameGraph: $FLAMEGRAPH_DIR ($(git -C "$FLAMEGRAPH_DIR" rev-parse --short HEAD 2>/dev/null || echo 'tarball'))"
else
    echo "[..] Fetching FlameGraph..."
    mkdir -p "$(dirname "$FLAMEGRAPH_DIR")"
    fg_ok=0
    for attempt in 1 2 3; do
        rm -rf "$FLAMEGRAPH_DIR"
        if git clone --depth=1 https://github.com/brendangregg/FlameGraph.git "$FLAMEGRAPH_DIR"; then
            fg_ok=1
            break
        fi
        echo "[warn] git clone attempt ${attempt}/3 failed; retrying..."
        sleep $((attempt * 3))
    done
    if [[ "$fg_ok" -eq 0 ]]; then
        echo "[..] Falling back to FlameGraph archive tarball..."
        rm -rf "$FLAMEGRAPH_DIR"
        mkdir -p "$FLAMEGRAPH_DIR"
        if curl -fsSL --retry 3 --retry-delay 3 \
            "https://github.com/brendangregg/FlameGraph/archive/refs/heads/master.tar.gz" \
            | tar -xz --strip-components=1 -C "$FLAMEGRAPH_DIR"; then
            fg_ok=1
        fi
    fi
    if [[ "$fg_ok" -ne 1 || ! -f "$FLAMEGRAPH_DIR/flamegraph.pl" ]]; then
        echo "[error] Failed to obtain FlameGraph" >&2
        exit 1
    fi
    echo "[ok] FlameGraph ready at $FLAMEGRAPH_DIR"
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
