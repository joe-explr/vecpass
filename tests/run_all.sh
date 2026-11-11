#!/usr/bin/env bash
# tests/run_all.sh — every login-node-safe test: build, vecpass-print,
# lit/FileCheck IR tests, and the differential test at AVX2.
# (AVX-512 differential tests run on the compute node inside run_bench.sh.)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
cmake -S "$ROOT" -B "$ROOT/build" -DLLVM_DIR="$VECPASS_LLVM_PREFIX/lib/cmake/llvm" -DCMAKE_CXX_COMPILER=clang++ > /dev/null
cmake --build "$ROOT/build" -j 8 > /dev/null
"$ROOT/tests/test_print.sh" | tail -1
lit -q "$ROOT/tests/ir" && echo "PASS: lit IR tests"
"$ROOT/tests/test_diff.sh" "${1:-x86-64-v3}"
