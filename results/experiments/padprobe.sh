#!/usr/bin/env bash
module load perf/5.14.0 2>/dev/null
B=build/pad; CPU=$(taskset -pc $$ | awk "{print \$NF}" | cut -d, -f1 | cut -d- -f1); PIN="numactl --physcpubind=$CPU --localalloc"
echo "host $(hostname) cpu $CPU"
for n in 1024 65536; do for pad in 8 200; do for isa in avx2 avx512; do for cc in base vecpass; do
  $PIN $B/bench_${cc}_${isa}_p$pad all $n 21 2>/dev/null | sed "s/^/p$pad,$isa,$cc,/"
done; done; done; done > build/pad/pad.csv
EV="cycles,instructions,cpu/event=0x07,umask=0x01,name=ld_blocks_partial.address_alias/,cpu/event=0x03,umask=0x82,name=ld_blocks.store_forward/"
for pad in 8 200; do for isa in avx2 avx512; do for cc in base vecpass; do for k in s212 s1213 s211; do
  perf stat -x, -e "$EV" $PIN $B/bench_${cc}_${isa}_p$pad $k 65536 5 2000 2>&1 >/dev/null | grep -v layout | awk -F, -v p="p$pad,$isa,$cc,$k" '{print p","$3","$1}'
done; done; done; done > build/pad/padperf.csv
echo done
