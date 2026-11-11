#!/usr/bin/env bash
# Size sweep: where does the vecpass speedup fall below 1 between L2 and memory?
set -u
module load perf/5.14.0 2>/dev/null
B=build/sweep
CPU=$(taskset -pc $$ | awk '{print $NF}' | cut -d, -f1 | cut -d- -f1)
PIN="numactl --physcpubind=$CPU --localalloc"
echo "host $(hostname) cpu $CPU" >&2
for n in 65536 262144 524288 1048576 2097152 4194304 8388608 16777216; do
  for isa in avx2 avx512; do for cc in base vecpass; do for k in s212 s241 s261 s1213 s211 s1244 fwd1213; do
    $PIN $B/bench_${cc}_$isa $k $n 11 2>/dev/null | sed "s/^/$isa,$cc,/"
  done; done; done
done > $B/sweep.csv
# perf at memory size: same fixed work for both builds
for isa in avx512; do for cc in base vecpass; do for k in s212 s1213 s1244; do
  perf stat -x, -e cycles,instructions,cache-misses,cache-references \
    $PIN $B/bench_${cc}_$isa $k 16777216 3 4 2>&1 >/dev/null | grep -v layout | awk -F, -v p="$isa,$cc,$k" '{print p","$3","$1}'
done; done; done > $B/sweep_perf.csv
echo done >&2
