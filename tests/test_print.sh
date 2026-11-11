#!/usr/bin/env bash
# tests/test_print.sh — Phase 1 test: run vecpass-print on tests/c/loops.c and
# check the reported loops with FileCheck-style expectations (via grep).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
OUT="$ROOT/build/test-out"; mkdir -p "$OUT"
# -O1 + -Xclang -disable-llvm-passes would give un-simplified IR; instead we
# emit -O1 IR with loop unrolling off so loops survive in a canonical form.
clang -O1 -fno-unroll-loops -fno-discard-value-names -S -emit-llvm "$ROOT/tests/c/loops.c" -o "$OUT/loops.ll"
opt -load-pass-plugin="$ROOT/build/libVecPass.so" -passes=vecpass-print \
    -disable-output "$OUT/loops.ll" 2> "$OUT/print.txt"
cat "$OUT/print.txt"
FileCheck "$ROOT/tests/c/loops.check" < "$OUT/print.txt"
echo "PASS: vecpass-print"
