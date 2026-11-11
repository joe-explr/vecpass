#!/usr/bin/env bash
# scripts/run_bench.sh — build everything and run the benchmarks end to end.
#
#   scripts/run_bench.sh              build + correctness + asm check + bench
#   scripts/run_bench.sh --build-only build + asm check only (login-node safe)
#
# Run the full version on a compute node only (scripts/bench.slurm does that).
# Results go to results/raw/<tag>/ ; the summary table is printed by
# bench/summarize.py.
#
# Variants (ISA x compiler):
#   ISA:      avx2      = -march=x86-64-v3
#             spr       = -march=sapphirerapids          (LLVM prefers 256-bit)
#             avx512    = -march=sapphirerapids -mprefer-vector-width=512
#   compiler: base      = clang -O3
#             vecpass   = clang -O3 -fpass-plugin=libVecPass.so
#             icx       = icx -O3 -fp-model=precise   (if icx is available)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_ONLY=0; [ "${1:-}" = "--build-only" ] && BUILD_ONLY=1
VECPASS_ICX=1 source "$ROOT/scripts/env.sh"
TAG="${VECPASS_TAG:-${SLURM_JOB_ID:-local}-$(date +%Y%m%d-%H%M%S)}"
OUT="$ROOT/results/raw/$TAG"; mkdir -p "$OUT"
B="$ROOT/build/bench"; mkdir -p "$B"
SAMPLES="${VECPASS_SAMPLES:-21}"
SIZES="${VECPASS_SIZES:-1024 65536 16777216}"   # L1, L2, memory (see README)
KERNELS="${VECPASS_KERNELS:-all}"

log() { echo "[run_bench] $*" | tee -a "$OUT/log.txt"; }

# ---------------- 1. machine + toolchain info ----------------
{
  echo "date: $(date -Is)"; echo "host: $(hostname)"
  echo "slurm job: ${SLURM_JOB_ID:-none} partition: ${SLURM_JOB_PARTITION:-none}"
  lscpu | grep -E "Model name|^CPU\(s\)|Thread|Socket|NUMA node\(s\)|L1d|L2|L3|MHz"
  echo "avx512f: $(grep -qw avx512f /proc/cpuinfo && echo yes || echo no)"
  echo "clang: $(clang --version | head -1)"; echo "opt: $(opt --version | grep version)"
  command -v icx >/dev/null && echo "icx: $(icx --version | head -1)" || echo "icx: not available"
  echo "module list:"; (module list 2>&1 || true)
  echo "git: $(git -C "$ROOT" rev-parse HEAD) $(git -C "$ROOT" status --porcelain | wc -l) dirty files"
} > "$OUT/machine.txt" 2>&1
cat "$OUT/machine.txt"

# ---------------- 2. build plugin ----------------
cmake -S "$ROOT" -B "$ROOT/build" -DLLVM_DIR="$VECPASS_LLVM_PREFIX/lib/cmake/llvm" \
      -DCMAKE_CXX_COMPILER=clang++ > /dev/null
cmake --build "$ROOT/build" -j 8 > /dev/null
PLUGIN="$ROOT/build/libVecPass.so"

declare -A ISA=( [avx2]="-march=x86-64-v3"
                 [spr]="-march=sapphirerapids"
                 [avx512]="-march=sapphirerapids -mprefer-vector-width=512" )
ISAS="${VECPASS_ISAS:-avx2 spr avx512}"
COMPILERS="base vecpass"; command -v icx >/dev/null && COMPILERS="$COMPILERS icx"

# ---------------- 3. build bench variants ----------------
for isa in $ISAS; do
  F="-O3 ${ISA[$isa]}"
  clang $F -c "$ROOT/bench/bench_main.c" -o "$B/main_$isa.o"
  for cc in $COMPILERS; do
    obj="$B/kernels_${cc}_$isa.o"; exe="$B/bench_${cc}_$isa"
    case $cc in
      base)    clang $F -gline-tables-only -c "$ROOT/kernels/kernels.c" -o "$obj" \
                 -Rpass=loop-vectorize -Rpass-missed=loop-vectorize 2> "$OUT/remarks_${cc}_$isa.txt"
               clang $F "$B/main_$isa.o" "$obj" -o "$exe" ;;
      vecpass) clang $F -gline-tables-only -fpass-plugin="$PLUGIN" -c "$ROOT/kernels/kernels.c" -o "$obj" \
                 -Rpass='vecpass|loop-vectorize' -Rpass-missed='vecpass|loop-vectorize' 2> "$OUT/remarks_${cc}_$isa.txt"
               clang $F "$B/main_$isa.o" "$obj" -o "$exe" ;;
      icx)     icx $F -fp-model=precise -c "$ROOT/kernels/kernels.c" -o "$obj" \
                 -qopt-report=2 -qopt-report-phase=vec -qopt-report-file="$OUT/optrpt_icx_$isa.txt"
               icx $F "$B/main_$isa.o" "$obj" -o "$exe" ;;
    esac
  done
done
log "built: $(ls $B | grep -c '^bench_') bench binaries"

# ---------------- 4. asm check: vector registers per kernel ----------------
# For each kernel function, count instructions that use xmm / ymm / zmm.
asm_summary() {  # $1 = object file
  llvm-objdump -d --no-show-raw-insn "$1" | awk '
    /^[0-9a-f]+ <.*>:$/ { fn=$2; gsub(/[<>:]/,"",fn); next }
    fn ~ /^k_/ { if ($0 ~ /%zmm/) z[fn]++; else if ($0 ~ /%ymm/) y[fn]++; else if ($0 ~ /%xmm/) x[fn]++; seen[fn]=1 }
    END { for (f in seen) printf "%-10s xmm=%-4d ymm=%-4d zmm=%-4d\n", f, x[f], y[f], z[f] }' | sort
}
for isa in $ISAS; do for cc in $COMPILERS; do
  echo "== $cc $isa" ; asm_summary "$B/kernels_${cc}_$isa.o"
done; done > "$OUT/asm_summary.txt"
log "asm summary: $OUT/asm_summary.txt"
llvm-objdump -d --no-show-raw-insn --disassemble-symbols=k_s212 "$B/kernels_vecpass_avx512.o" \
  > "$OUT/asm_k_s212_vecpass_avx512.txt" 2>/dev/null || true
llvm-objdump -d --no-show-raw-insn --disassemble-symbols=k_s212 "$B/kernels_base_avx512.o" \
  > "$OUT/asm_k_s212_base_avx512.txt" 2>/dev/null || true

[ $BUILD_ONLY = 1 ] && { log "build-only: done ($OUT)"; exit 0; }

# ---------------- 5. correctness on this machine ----------------
for isa in $ISAS; do
  VECPASS_EXTRA_CFLAGS="$(echo ${ISA[$isa]} | sed 's/-march=[^ ]*//')" \
    "$ROOT/tests/test_diff.sh" "$(echo ${ISA[$isa]} | sed 's/.*-march=\([^ ]*\).*/\1/')" 2>&1 | tee -a "$OUT/correctness.txt"
done

# ---------------- 6. pinning ----------------
CPU="${VECPASS_CPU:-4}"
if command -v numactl >/dev/null; then PIN="numactl --physcpubind=$CPU --localalloc"
else PIN="taskset -c $CPU"; fi
log "pinning: $PIN"

# ---------------- 7. benchmark ----------------
CSV="$OUT/bench.csv"
echo "isa,compiler,kernel,n,reps,median_ns_per_elem,min_ns_per_elem,checksum" > "$CSV"
for n in $SIZES; do for isa in $ISAS; do for cc in $COMPILERS; do
  log "bench $cc $isa n=$n"
  $PIN "$B/bench_${cc}_$isa" "$KERNELS" "$n" "$SAMPLES" | sed "s/^/$isa,$cc,/" >> "$CSV"
done; done; done

# ---------------- 8. perf stat (L2 size, avx512) ----------------
if command -v perf >/dev/null; then
  PERFCSV="$OUT/perf.csv"; echo "isa,compiler,kernel,event,value" > "$PERFCSV"
  for isa in avx512 avx2; do
    for k in s212 s1213 s211 s241 s1244 s261 s244 s116 fwd1213 s000; do
      for cc in $COMPILERS; do
        # fixed work: 5 samples x 2000 calls x 65536 elements, same for all builds
        perf stat -x, -e cycles,instructions -o "$B/perf.tmp" \
          $PIN "$B/bench_${cc}_$isa" "$k" 65536 5 2000 > /dev/null 2>&1 || continue
        grep -E "cycles|instructions" "$B/perf.tmp" | awk -F, -v p="$isa,$cc,$k" '{print p","$3","$1}' >> "$PERFCSV"
      done
    done
  done
  log "perf: $PERFCSV"
else
  log "perf not available"
fi

python3 "$ROOT/bench/summarize.py" "$OUT" | tee "$OUT/summary.md"
log "done: $OUT"
