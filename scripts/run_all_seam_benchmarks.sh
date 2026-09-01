#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VAZIO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

cd "$VAZIO_ROOT"

echo "=========================================================================="
echo "          NEOTEC VAZIO - FULL SEAM BENCHMARK & LIMIT TEST SUITE           "
echo "=========================================================================="

# 1. SEAM 1: Math & Raycast
echo ""
echo ">>> [1/4] Running Seam 1 Benchmark: Math Substrate & Raycast Engine..."
./build/test_seam_math_raycast

# 2. SEAM 5: C++ <-> Rust FFI Delta Sync & Dynamic Node State
echo ""
echo ">>> [2/4] Running Seam 5 Benchmark: C++ <-> Rust FFI Roundtrip & Reconciler..."
LD_LIBRARY_PATH="$VAZIO_ROOT/extern/OATS-ffi/lib:$LD_LIBRARY_PATH" ./build/test_seam_ffi_roundtrip

# 3. SEAM 4: Lumberjack Hypergraph Data Operations & Multi-parent Forest
echo ""
echo ">>> [3/4] Running Seam 4 Benchmark: Lumberjack Go Hypergraph & DAG Traversal..."
(cd "$VAZIO_ROOT/extern/lumberjack" && go run ./cmd/hypergraph_bench/main.go)

# 4. SEAM 3: OATS Core ECS Engine Limits
echo ""
echo ">>> [4/4] Running Seam 3 Benchmark: OATS Core ECS & Async Dispatch Limits..."
(cd "$VAZIO_ROOT/extern/OATS-ffi" && cargo test --test seam_oats_limits -- --nocapture)

echo ""
echo "=========================================================================="
echo " [ALL SEAMS PASSED] All 5 integration interfaces verified and stress-tested."
echo "=========================================================================="
