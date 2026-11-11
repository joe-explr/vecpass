#!/usr/bin/env bash
# analysis/run_tsvc_analysis.sh — regenerate the data behind analysis/missed.md.
# Compiles TSVC_2 with clang -O3 (remarks), icx -O3 (opt-report, if
# available) and clang -O3 + vecpass, then builds the Markdown table.
# Compile-only, so it is fine on the login node.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
"$ROOT/scripts/get_tsvc.sh" > /dev/null
T="$ROOT/third_party/TSVC_2/src"; OUT="$ROOT/analysis/data"; mkdir -p "$OUT"
MARCH="${VECPASS_MARCH}"
# TSVC's own "relaxed" build uses -ffast-math; we do the same so that
# reductions are not counted as misses for FP-ordering reasons.
CF="-O3 -march=$MARCH -ffast-math -gline-tables-only -c $T/tsvc.c -o /dev/null"
R='-Rpass=loop-vectorize -Rpass-missed=loop-vectorize -Rpass-analysis=loop-vectorize'
( cd "$T" && clang $CF $R 2> "$OUT/tsvc_clang.remarks" )
( cd "$T" && clang $CF -fpass-plugin="$ROOT/build/libVecPass.so" \
    -Rpass='loop-vectorize|vecpass' -Rpass-missed='loop-vectorize|vecpass' \
    -Rpass-analysis=loop-vectorize 2> "$OUT/tsvc_vecpass.remarks" )
ICX_ARG=""
VECPASS_ICX=1 source "$ROOT/scripts/env.sh"
if command -v icx >/dev/null; then
  ( cd "$T" && icx -O3 -march=$MARCH -fp-model=fast -c tsvc.c -o /dev/null \
      -qopt-report=3 -qopt-report-phase=vec -qopt-report-file="$OUT/tsvc_icx.optrpt" )
  ICX_ARG="$OUT/tsvc_icx.optrpt"
  icx --version | head -1 > "$OUT/icx_version.txt"
fi
clang --version | head -1 > "$OUT/clang_version.txt"
python3 "$ROOT/analysis/parse_remarks.py" "$T/tsvc.c" "$OUT/tsvc_clang.remarks" \
    $ICX_ARG --vecpass "$OUT/tsvc_vecpass.remarks" > "$OUT/missed_table.md"
echo "clang vectorized loops (incl. timing loops):  $(grep -c 'remark: vectorized loop' $OUT/tsvc_clang.remarks)"
echo "vecpass vectorized loops (incl. timing loops): $(grep -c 'remark: vectorized loop' $OUT/tsvc_vecpass.remarks)"
echo "table: $OUT/missed_table.md"
