# vecpass: an LLVM pass plugin that recovers missed auto-vectorization

`vecpass` is an out-of-tree LLVM 23 pass plugin (new pass manager, C++17,
CMake). It makes clang's LoopVectorize vectorize loops that it otherwise
rejects with **"Backward loop carried data dependence"** when that dependence
is caused only by the *order of statements* in the loop body. Example, TSVC
s212:

```c
for (int i = 0; i < n-1; i++) {
  a[i] *= c[i];            // store a[i]
  b[i] += a[i+1] * d[i];   // load a[i+1], after the store: "backward"
}
```

`a[i]` and `a[i+1]` never alias within one iteration, so the load can legally
move above the store. vecpass finds such reorderings with a dependence graph
built from LoopAccessAnalysis, alias analysis and ScalarEvolution, applies
them only when LAA confirms they help (otherwise it reverts), and runs
LoopLoadElimination where needed. **clang 23 `-O3` leaves these loops
scalar. Intel icx 2025.0's loop vectorizer rejects them too (icx emits only
128-bit SLP-style code for some of them). With vecpass, clang emits 256-bit
or 512-bit vector loops.**

**Fallback-ladder rung reached: 1** (enables vectorization of a loop class
clang misses). It also partly reaches rung 2: it lifts a VF=2 cap on an
already-vectorized loop to VF=8 (`tests/ir/capped_vf.c`).

## Results (measured; details in [RESULTS.md](RESULTS.md))

Speedup of `clang -O3` + vecpass over plain `clang -O3`, median of 21 samples, Intel Xeon CPU Max 9468, one pinned core, job 2413619. `avx512` = `-march=sapphirerapids -mprefer-vector-width=512`, `avx2` = `-march=x86-64-v3`. `p200`/`p8` = array layout (`p8` puts all arrays within 128 B mod 4096; see RESULTS.md).

| kernel | L1 avx2 p200 | L1 avx512 p200 | L2 avx2 p200 | L2 avx512 p200 | mem avx512 p200 | L2 avx2 p8 | L2 avx512 p8 |
|---|---:|---:|---:|---:|---:|---:|---:|
| s212 | 3.90x | 5.20x | 3.07x | 3.29x | 1.19x | 0.81x | 1.34x |
| s1213 | 4.39x | 5.68x | 2.42x | 2.85x | 0.90x | 0.76x | 1.58x |
| s211 | 4.12x | 5.56x | 2.26x | 2.66x | 0.90x | 0.83x | 1.73x |
| s241 | 4.34x | 5.94x | 3.28x | 3.52x | 1.20x | 1.72x | 2.91x |
| s1244 | 4.62x | 6.01x | 2.55x | 2.94x | 0.90x | 1.81x | 2.58x |
| s261 | 4.46x | 5.41x | 3.12x | 3.29x | 1.18x | 3.06x | 3.21x |
| s244 | 3.38x | 4.06x | 2.72x | 3.09x | 1.18x | 1.02x | 1.44x |
| s116 | 1.00x | 1.01x | 1.00x | 1.16x | 1.23x | 1.00x | 1.16x |
| fwd1213 | 4.40x | 5.26x | 2.30x | 3.10x | 0.94x | 1.32x | 2.47x |
| s000 | 1.00x | 0.95x | 1.00x | 1.00x | 1.00x | 1.00x | 1.00x |
| s321 | 1.00x | 1.00x | 1.00x | 1.00x | 1.01x | 1.00x | 1.00x |

s000 (already vectorized) and s321 (true recurrence) are controls that vecpass must leave alone. Numbers below 1.0x are slowdowns, discussed in RESULTS.md: layout p8 at 256-bit (attributed to 4K aliasing by a controlled layout experiment; a PMU counter could not confirm it), bandwidth-bound memory sizes, and s116 at `-march=sapphirerapids` (0.64–0.69x, not shown here; a LoopVectorize cost-model misprediction). Correctness: bit-identical outputs (2668 differential-test lines per ISA) and identical benchmark checksums.

## Build and run

```bash
# one-time: LLVM 23.1.5 with headers (the SeaWulf llvm module has none)
curl -sL https://micro.mamba.pm/api/micromamba/linux-64/latest | tar -xj -C ~/micromamba bin/micromamba
MAMBA_ROOT_PREFIX=~/micromamba ~/micromamba/bin/micromamba create -y -n llvm -c conda-forge \
    llvmdev=23.1.5 clang=23.1.5 clangxx=23.1.5 llvm-tools=23.1.5 lit

source scripts/env.sh                  # PATH, VECPASS_MARCH (default sapphirerapids)
cmake -S . -B build -DLLVM_DIR=$VECPASS_LLVM_PREFIX/lib/cmake/llvm -DCMAKE_CXX_COMPILER=clang++
cmake --build build -j

# Phase 1 hello pass
clang -O1 -S -emit-llvm tests/c/loops.c -o x.ll
opt -load-pass-plugin=./build/libVecPass.so -passes='function(vecpass-print)' -disable-output x.ll

# vecpass inside clang -O3 (inserted just before LoopVectorize)
clang -O3 -march=sapphirerapids -fpass-plugin=build/libVecPass.so -c kernels/kernels.c \
      -Rpass='vecpass|loop-vectorize' -Rpass-missed='vecpass|loop-vectorize'

# same with opt
opt -load-pass-plugin=build/libVecPass.so -passes='default<O3>' -pass-remarks=vecpass in.ll -S -o out.ll
# just the pass (for experiments)
opt -load-pass-plugin=build/libVecPass.so -passes=vecpass -vecpass-debug in.ll -S
```

Note: clang keeps only the **last** `-Rpass-missed=`; combine passes in one
regex as above.

Options: `-vecpass-auto=false` (don't auto-insert into O2/O3),
`-vecpass-run-lle=false`, `-vecpass-debug`, `-vecpass-max-body=N`. With clang
23 pass them as `-mllvm -vecpass-...` next to `-fpass-plugin=...` (checked:
the plugin is loaded before `-mllvm` options are parsed).

### Tests

```bash
tests/run_all.sh          # build + vecpass-print + lit IR tests + differential test (AVX2)
lit -v tests/ir           # FileCheck IR before/after tests
tests/test_diff.sh x86-64-v3          # differential: baseline vs vecpass, bit-exact
analysis/run_tsvc_analysis.sh         # regenerate analysis/missed.md data
analysis/experiments/run.sh           # Phase 2 experiments
```

### Benchmarks (compute node only)

```bash
sbatch scripts/bench.slurm            # partition hbm-short-96core, exclusive node
scripts/run_bench.sh --build-only     # login-node safe: build + asm check
```

`scripts/run_bench.sh` builds clang / clang+vecpass / icx variants for
`-march=x86-64-v3` (AVX2), `-march=sapphirerapids` (LLVM's default 256-bit
preference) and `-march=sapphirerapids -mprefer-vector-width=512` (zmm), for
two array layouts. It reruns the differential tests on the node, then
benchmarks pinned to one core at L1, L2 and memory sizes, runs `perf stat`,
and writes `results/raw/<tag>/`.

## Layout

| path | what |
|---|---|
| `src/Plugin.cpp` | plugin entry point, pass registration, VectorizerStart extension point |
| `src/LoopPrinter.*` | `vecpass-print` (Phase 1) |
| `src/VecPass.*` | the transform |
| `kernels/` | TSVC-style kernels for tests and benchmarks |
| `tests/` | lit IR tests, differential tests, Phase 1 test |
| `bench/` | timing harness, results summarizer |
| `analysis/` | TSVC missed-vectorization study (`missed.md`), experiments |
| `scripts/` | env, TSVC fetch, benchmark driver, Slurm job |
| `results/` | raw benchmark outputs (CSV, remarks, asm, perf), machine info |
| `docs/` | DESIGN (decisions, safety proof), LEARNING (C++/LLVM walkthrough), INTERVIEW, EXERCISES |

## Limitations

* Innermost loops with a **single-block body** only (if-converted bodies,
  e.g. TSVC s161/s277, are skipped).
* No calls, volatile or atomic accesses in the body.
* Possibly-aliasing pointers block moving whole statements (`restrict`
  or distinct globals are needed). Load hoisting proven by SCEV still works.
* No cost model of its own: it may reorder a loop that LoopVectorize then
  rejects on cost (s116 at AVX2), which leaves an equivalent scalar loop.
* When any loop in a function is changed, LoopLoadElimination + DCE run on
  the whole function, earlier than in the stock pipeline.
* True recurrences (s321 and most of the 25 TSVC backward-dependence loops)
  are out of scope.
* Speedups depend strongly on array placement (4K aliasing; see RESULTS.md).
* At memory-resident sizes, kernels with a write-only output array run
  0.91–0.99× (slower); a controlled pair confirms the pattern, but the mechanism
  is not identified (RESULTS.md).

## Future work

* Do the reordering inside LoopVectorize/VPlan (emit widened recipes in a
  dependence-respecting order) instead of rewriting scalar IR. That would be
  the natural upstream design.
* Partial reordering plus loop distribution for loops that also contain a
  real recurrence (docs/EXERCISES.md #6).
* Multi-block bodies after if-conversion, with a speculation-safety argument.
* Measure prevalence beyond TSVC (SPEC CPU, llvm-test-suite).
* Try non-temporal stores for write-only streams at large trip counts, and
  find the cause of the memory-size slowdown.

## Resume bullet drafts (measured numbers only)

* Built an out-of-tree LLVM 23 pass plugin (C++17, new pass manager) that reorders loop bodies using
  LoopAccessAnalysis, alias analysis and SCEV, so clang -O3 vectorizes 8 TSVC loops it left scalar (128→136).
* Achieved 2.3–4.6× (AVX2) and 2.7–6.0× (AVX-512) single-core speedups on 7 of 8 target TSVC kernels
  (L1/L2 data, spread-out arrays) on an Intel Xeon Max 9468; Slurm, perf stat, medians of 21 runs.
* Validated with lit/FileCheck IR tests and a differential harness (2,668 bit-identical results per ISA);
  traced a layout-dependent vector slowdown to array placement via controlled padding experiments.

## Pushing

This repository has **no git remote configured**, and this machine's SSH keys are
not accepted by GitHub, so the phase commits were made locally but not pushed.
To publish:

```bash
git remote add origin git@github.com:<you>/vecpass.git   # or https://...
git push -u origin master
```
