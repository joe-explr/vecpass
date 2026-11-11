#!/usr/bin/env bash
# Build both variants and run on a compute node:  srun -p hbm-short-96core -N1 -n1 --exclusive results/experiments/writeonly/run.sh
set -eu
D="$(cd "$(dirname "$0")" && pwd)"; source "$D/../../../scripts/env.sh"
O="$D/../../../build/writeonly"; mkdir -p "$O"
F="-O3 -march=sapphirerapids -mprefer-vector-width=512"
clang $F -c "$D/main.c" -o "$O/main.o"
clang $F -c "$D/kern.c" -o "$O/kb.o"
clang $F -fpass-plugin="$D/../../../build/libVecPass.so" -c "$D/kern.c" -o "$O/kv.o" -Rpass=loop-vectorize 2>"$O/remarks.txt"
clang $F "$O/main.o" "$O/kb.o" -o "$O/base"; clang $F "$O/main.o" "$O/kv.o" -o "$O/vp"
CPU=$(taskset -pc $$ | awk '{print $NF}' | cut -d, -f1 | cut -d- -f1)
echo "host $(hostname) cpu $CPU; vectorized loops with vecpass: $(grep -c 'vectorized loop' "$O/remarks.txt")"
for n in 65536 1048576 16777216; do for k in wo rw; do
  bt=$(numactl --physcpubind=$CPU --localalloc "$O/base" $k $n | cut -d, -f3)
  vt=$(numactl --physcpubind=$CPU --localalloc "$O/vp" $k $n | cut -d, -f3)
  echo "$k n=$n base=$bt vecpass=$vt speedup=$(python3 -c "print(f'{$bt/$vt:.2f}')")"
done; done
