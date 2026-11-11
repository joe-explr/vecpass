#!/usr/bin/env bash
# analysis/experiments/run.sh — reproduce the Phase 2 experiments (compile only).
set -euo pipefail
D="$(cd "$(dirname "$0")" && pwd)"; source "$D/../../scripts/env.sh"
O="$D/../../build/experiments"; mkdir -p "$O"
echo "== runtime offset: clang -O3 remarks"
clang -O3 -march=$VECPASS_MARCH -c "$D/runtime_offset.c" -o /dev/null -Rpass=loop-vectorize 2>&1 | grep remark
clang -O3 -march=$VECPASS_MARCH -fno-discard-value-names -S -emit-llvm "$D/runtime_offset.c" -o "$O/runtime_offset.ll"
echo "distance checks emitted:"; grep -c "diff.check" "$O/runtime_offset.ll" || true
echo "== reorder: clang -O3"
clang -O3 -march=$VECPASS_MARCH -c "$D/reorder.c" -o /dev/null -Rpass=loop-vectorize -Rpass-missed=loop-vectorize 2>&1 | grep remark | sed 's/ \[-R.*//'
echo "== reorder: clang -O3 + vecpass"
clang -O3 -march=$VECPASS_MARCH -fpass-plugin="$D/../../build/libVecPass.so" -c "$D/reorder.c" -o /dev/null -Rpass=loop-vectorize -Rpass-missed=loop-vectorize 2>&1 | grep remark | sed 's/ \[-R.*//'
VECPASS_ICX=1 source "$D/../../scripts/env.sh"
if command -v icx >/dev/null; then
  echo "== reorder: icx -O3"
  icx -O3 -march=$VECPASS_MARCH -c "$D/reorder.c" -o /dev/null -qopt-report=2 -qopt-report-phase=vec -qopt-report-file="$O/reorder.optrpt"
  awk '/LOOP BEGIN/{l=$4" "$5} /#15300/{print l": vectorized"} /#15344/{print l": NOT vectorized"}' "$O/reorder.optrpt" | sort -u -t: -k1,1
fi
