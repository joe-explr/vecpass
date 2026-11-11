#!/usr/bin/env bash
# tests/test_diff.sh — differential correctness test.
#
# Builds the kernels twice (clang -O3, and clang -O3 + vecpass), links each
# with the same driver, runs both, and requires byte-identical output.
# Also checks that vecpass really transformed and vectorized the target loops
# (otherwise "identical output" would prove nothing).
#
# Usage: tests/test_diff.sh [march]    (default: $VECPASS_MARCH)
# The machine running this must support the chosen -march. On the SeaWulf
# login node (AMD Milan, no AVX-512) use: tests/test_diff.sh x86-64-v3
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
MARCH="${1:-$VECPASS_MARCH}"
EXTRA="${VECPASS_EXTRA_CFLAGS:-}"   # e.g. -mprefer-vector-width=512
# VECPASS_DIFF_DIR lets concurrent jobs use separate dirs.
OUT="${VECPASS_DIFF_DIR:-$ROOT/build/diff}-$MARCH${VECPASS_EXTRA_CFLAGS:+-wide}"; mkdir -p "$OUT"
PLUGIN="$ROOT/build/libVecPass.so"
CF="-O3 -march=$MARCH $EXTRA -gline-tables-only"

clang $CF -c "$ROOT/tests/diff/driver.c" -o "$OUT/driver.o"
for src in kernels/kernels.c tests/diff/ptr_kernels.c; do
  base=$(basename "$src" .c)
  clang $CF -c "$ROOT/$src" -o "$OUT/${base}_base.o"
  clang $CF -fpass-plugin="$PLUGIN" -c "$ROOT/$src" -o "$OUT/${base}_vp.o" \
        -Rpass='vecpass|loop-vectorize' -Rpass-missed=vecpass \
        2> "$OUT/${base}_vp.remarks"
done
clang $CF "$OUT/driver.o" "$OUT/kernels_base.o" "$OUT/ptr_kernels_base.o" -o "$OUT/diff_base"
clang $CF "$OUT/driver.o" "$OUT/kernels_vp.o"   "$OUT/ptr_kernels_vp.o"   -o "$OUT/diff_vp"

"$OUT/diff_base" > "$OUT/base.txt"
"$OUT/diff_vp"   > "$OUT/vp.txt"
lines=$(wc -l < "$OUT/base.txt")
if ! cmp -s "$OUT/base.txt" "$OUT/vp.txt"; then
  echo "FAIL: outputs differ ($MARCH):"; diff "$OUT/base.txt" "$OUT/vp.txt" | head -20
  exit 1
fi
echo "PASS: $lines result lines identical between baseline and vecpass ($MARCH $EXTRA)"

# Sanity: the transformation must actually have happened.
nre=$(cat "$OUT"/*_vp.remarks | grep -cE "remark: (reordered loop body|no reordering needed)" || true)
echo "vecpass transformed $nre loops; remarks in $OUT/*_vp.remarks"
[ "$nre" -ge 12 ] || { echo "FAIL: expected >= 12 transformed loops"; exit 1; }
# p_s1213 with plain pointers must be refused (a and b may alias).
grep -q "ptr_kernels.c:4[0-9]:.*loop not reordered: no legal order" "$OUT/ptr_kernels_vp.remarks" \
  || { echo "FAIL: p_s1213 (may-alias) should have been refused"; exit 1; }
echo "PASS: may-alias p_s1213 correctly refused"
