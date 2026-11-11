# vecpass — design notes

This file records what was decided, why, and the evidence. Everything
quoted here was produced in this repo; commands are given so it can be
re-checked.

## 1. Environment decisions (Phase 0)

| Decision | Why |
|---|---|
| LLVM 23.1.5 from conda-forge (`llvmdev`, `clang`, `clangxx`, `llvm-tools`, `lit`) via micromamba in `$HOME/micromamba/envs/llvm` | The SeaWulf `llvm/23.0.0` module has `clang`/`opt` and `LLVMConfig.cmake` but **no C++ headers** (`include/llvm/` only holds generated `.inc` files; `llvm-config --includedir` points to a directory we can't read). A plugin can't be built against it. The conda packages give clang, opt, headers and `libLLVM.so` from one build, so plugin, `opt` and `clang` all agree on ABI and IR format. |
| Plugin is not linked against libLLVM | `opt` and `clang` already load `libLLVM.so.23.1`; the plugin's undefined symbols resolve against that copy at `dlopen` time. Linking a second copy would duplicate global registries (`cl::opt`, pass IDs). |
| `-fno-rtti` | LLVM is built with `LLVM_ENABLE_RTTI=OFF`. Plugins deriving from LLVM classes must match. |
| Benchmark node: partition `hbm-short-96core`, CPU **Intel Xeon CPU Max 9468** (Sapphire Rapids, `avx512f` present) | `srun -p hbm-short-96core lscpu` (saved in `results/lscpu_xm053.txt`). The 40-core Skylake partitions were drained or busy, with an estimated start ~36 h out. The hbm nodes started within minutes. |
| Login node is AMD (`milan2`) | So no AVX-512 binaries run there. Login-node correctness tests use `-march=x86-64-v3` (AVX2); AVX-512 tests run inside the Slurm job. |
| Target ISA is a variable: `VECPASS_MARCH` (default `sapphirerapids`), plus `VECPASS_EXTRA_CFLAGS` | `sapphirerapids` alone makes LLVM *prefer 256-bit vectors* (ymm) by default, so zmm needs `-mprefer-vector-width=512`. The benchmark builds both. |
| Reference compiler: icx 2025.0.0 (`module load intel/oneAPI/2025.0; module load tbb/latest umf/latest compiler-rt/latest compiler/latest`) | Available; the `compiler` module needs `tbb`, `umf` and `compiler-rt` loaded first. |
| Repo lives in `$HOME/vecpass` (not scratch) | That is where the git repo already was. TSVC is fetched into `third_party/` (git-ignored) by `scripts/get_tsvc.sh`. |
| **No `git push`** | The repo has **no remote configured** (`git remote -v` is empty), `gh` is not installed, and GitHub rejects this machine's SSH keys (`Permission denied (publickey)`). Every phase is committed locally; see README "Pushing". |

## 2. Which loop class, and why (Phase 2)

Full data: `analysis/missed.md`. Summary: of 44 TSVC kernel loops clang 23
leaves scalar, 25 fail with *"Backward loop carried data dependence"*, the
largest class.

**Rejected candidate: runtime offset (`a[i] = a[i+k] + b[i]`).** LLVM 23
already vectorizes this. LAA emits a dependence-*distance* runtime check
(`diff.check = icmp ugt i64 %(src-sink), -128`) rather than a range-overlap
check, so the vector loop runs for every `k` outside a small window. TSVC s162
and s174 (the runtime-offset kernels) are vectorized by plain clang. Re-run:
`analysis/experiments/run.sh` (12 `diff.check`s in `runtime_offset.ll`).

**Chosen class: backward dependences caused only by statement order.**
Within the 25 backward-dependence loops, 8 are not true recurrences.
The dependence is backward only because of how the statements are ordered in
the body:

```c
// s212
a[i] *= c[i];            // store a[i]
b[i] += a[i+1] * d[i];   // load a[i+1]: the NEXT iteration overwrites it
```

LAA calls a dependence *backward* when the access that happens first in time
(the load of `a[i+1]` in iteration i) comes lexically after the access that
happens second (the store to `a[i+1]` in iteration i+1). A vector loop runs
statement by statement over VF iterations at once, so the store for lanes
i..i+VF-1 would run before the load for those lanes and clobber `a[i+1]`. If
the load is moved above the store, the dependence becomes *forward* and the
vector loop is correct.

Evidence (`analysis/experiments/reorder.c`, `run.sh`):

| kernel | clang -O3 original | clang -O3 hand-reordered | icx original | icx hand-reordered |
|---|---|---|---|---|
| s212, s241, s1244 | not vectorized | **vectorized** | not vectorized | vectorized |
| s1213, s211, s261 | not vectorized | not vectorized: "Forward ... prevents store-to-load forwarding" | not vectorized | vectorized |

Why this class:
* It's the biggest *fixable* slice of the biggest reason class.
* **clang 23 doesn't handle it, and icx 2025.0's loop vectorizer doesn't either**
  (icx gets 128-bit SLP-style code on some of these kernels; RESULTS.md), so
  it isn't a re-implementation.
* The transform is local (one basic block), so its safety argument is short
  and checkable (section 4).
* Vector legality is still decided by LLVM's own LAA, which reduces the risk
  of the transform being wrong.

## 3. Why LLVM 23 misses it

* **LoopVectorize never reorders memory operations.** VPlan widens each scalar
  instruction in place. LAA's `MemoryDepChecker` classifies each dependence
  against the *existing* order, and a `Backward` dependence with a distance
  smaller than VF makes the loop illegal (`canVectorizeMemory() == false`).
* **LoopDistribute** can sometimes split such loops (the remark even suggests
  `#pragma clang loop distribute(enable)`). But it is **off by default**, and it
  only separates statements into different loops; it does not reorder within
  one loop.
* The **SLP vectorizer** has an intra-block scheduler, but it works on straight-line code,
  not across loop iterations.
* For s1213-type loops, reordering alone isn't enough. After it, LAA sees
  `store b[i]` followed one iteration later by `load b[i-1]`, a forward
  dependence of distance 1, and returns `ForwardButPreventsForwarding`: the
  vector load would partially overlap the vector store just issued, causing a
  store-to-load-forwarding stall. LoopVectorize treats this as unsafe.
  **LoopLoadElimination** (LLE) fixes it by replacing the load with the value
  stored in the previous iteration (a PHI), which LoopVectorize vectorizes as a
  first-order recurrence. But in the default `-O3` pipeline, LLE runs *after*
  LoopVectorize (`PassBuilder::buildModuleOptimizationPipeline`), so it's too
  late for these loops. Experiment: `opt -passes='function(loop-load-elim,loop-vectorize)'`
  on the reordered IR vectorizes all six.

## 4. The transform and its safety argument

### What vecpass does, per loop (`src/VecPass.cpp`, `LoopReorderer::run`)

1. **Shape check.** Innermost loop, body is a single basic block (rotated loop:
   header == latch), has a preheader. Only simple (non-volatile, non-atomic)
   loads/stores. No other instruction that may read or write memory or has side
   effects (calls, fences, ...). At most 256 instructions. Otherwise: skip
   (remark).
2. **Ask LAA** (a fresh `LoopAccessInfo`) for the dependence list.
   * Legal with unlimited VF: skip ("already vectorizable").
   * Legal but VF-capped by a backward dependence: *CappedVF* mode.
   * Any `Unknown`, `IndirectUnsafe`, `InvariantUnsafe` dependence: skip.
3. **Build a "must come before" graph** on the body's non-PHI, non-terminator
   instructions:
   * (a) SSA: each in-block operand → its user.
   * (b) Same-iteration memory order: for every earlier/later pair where at
     least one is a store and the two *may touch the same bytes in the same
     iteration*: earlier → later.
   * (c) LAA forward dependences: earlier → later (never flip them).
   * (d) LAA backward dependences: **later → earlier** (the change we want).
4. **Topological sort** (Kahn's algorithm, ties broken by original position,
   so nothing moves unless an edge forces it). If there's a cycle, no valid order
   exists: skip ("true recurrence, or accesses that may alias").
5. **Move** the instructions into the new order.
6. **Re-run LAA from scratch.** Accept if it is now legal, or if the only
   remaining problems are `...PreventsForwarding` dependences (fixed by LLE in
   step 8). In CappedVF mode, accept only if the max safe width grew.
   Otherwise **restore the original order** and skip ("reverted").
7. A loop whose only problem is `ForwardButPreventsForwarding` (no reordering
   needed) is accepted directly ("ForwardingOnly").
8. If anything in the function was accepted: run LLVM's
   `LoopLoadEliminationPass` and then `DCEPass` on that function. DCE is
   needed because LLE leaves the now-dead load behind, and LoopVectorize's LAA
   would still count it.

### Why the transformed scalar loop is equivalent to the original

Claim: steps 3–5 never change the program's behavior.

* Only instructions **inside one basic block** move, and they stay in that block.
  Every one of them still executes exactly once per iteration, and all of
  iteration *i* still executes before any of iteration *i+1*. So the order
  between different iterations is unchanged; only the order within one
  iteration changes.
* Within one iteration this is a reordering of a straight-line block. Such a
  reordering preserves semantics iff it preserves:
  1. every **def→use** order (edge a), so every instruction sees the same
     operand values; and
  2. the relative order of every pair of **memory accesses that may overlap,
     where at least one writes** (edge b). Pairs of loads may be swapped
     freely. Non-overlapping accesses commute.
  3. Anything else with ordering constraints (calls, volatile, atomics, fences)
     is excluded by step 1. Pure arithmetic has no ordering constraint other
     than (1). Trapping instructions like integer division only run if all
     earlier code in the block ran; since the whole block always runs, moving
     them can't introduce or remove a trap on a path that didn't already
     have one (a trap in the block was UB/abort either way).
* "May overlap in the same iteration" (`mayAliasInSameIteration`) is answered
  conservatively, by two independent proofs; anything unproven counts as
  overlapping:
  * **Alias analysis** (`AA.isNoAlias`: BasicAA, TBAA, `restrict`/noalias
    scopes, globals). Both pointers are SSA values in the same block, so the
    query is about their values *in the same iteration*. This is the same use
    of AA as LLVM's SLP scheduler and MemorySSA.
  * **ScalarEvolution**: if `ptrA − ptrB` is a compile-time constant D and
    `D ≥ size(B)` or `−D ≥ size(A)`, the byte ranges are disjoint (`a[i]` vs
    `a[i+1]`: D = 4, size 4).
* Edges (c) and (d) only *add* constraints. Adding edges can make a sort fail
  (we then do nothing), but it can't make an illegal order pass.

LLE and DCE are stock LLVM passes that are correct on any valid IR.

### Why the vector loop is correct

vecpass makes no claim here. LoopVectorize runs its own LAA on the new
body, adds runtime checks where needed, and vectorizes only if it can prove
legality. If vecpass computed a reordering that LAA still rejects, the only
cost is a different scalar order (which step 6 also undoes).

### Pipeline placement

vecpass is registered at **`registerVectorizerStartEPCallback`**: the
function pipeline position just before LoopVectorize in `-O2/-O3`. There,
loops have already been simplified, rotated (single-block bodies) and LICM'd, and
no pass between us and LoopVectorize can undo the new order. Loading the
plugin is enough: `clang -O3 -fpass-plugin=libVecPass.so` or
`opt -load-pass-plugin=libVecPass.so -passes='default<O3>'`. The pass is
also available by name (`-passes=vecpass`) for testing; `-vecpass-auto=false`
disables the automatic insertion.

We tried, and rejected, `-passes='function(vecpass),default<O3>'` (with
`-vecpass-auto=false`) on `kernels/kernels.c`: every loop was skipped with
"loop body has 3 basic blocks", because before the O3 simplification
pipeline the loops are neither SROA'd nor rotated, and nothing vecpass-specific
got vectorized. Running later than VectorizerStart is impossible without
landing after LoopVectorize.

## 5. Problems hit during implementation (and fixes)

| Symptom | Cause | Fix |
|---|---|---|
| Every reordering was "reverted: Backward dependence remains" even when the new IR was clearly forward | `LoopAccessInfoManager::clear()` only evicts entries **with runtime checks**; the cached analysis of the old order came back | Build a fresh `LoopAccessInfo` (`computeLAA()`) before and after reordering |
| s1213/s211/s261 reordered, LLE ran, still not vectorized | LLE rewires uses to a PHI but leaves the dead load; LAA still sees it | Run `DCEPass` after LLE |
| Test IR from `clang -O3 -fno-vectorize` didn't match what LV sees | That pipeline still runs LLE (which normally follows LV), so loops were already partly fixed | lit tests use unoptimized IR + the real `default<O3>`, plus hand-written `.ll` files shaped like VectorizerStart IR |
| `clang -Rpass-missed=vecpass -Rpass-missed=loop-vectorize` showed no vecpass remarks | clang keeps only the last `-Rpass*=` of each kind | Use one regex: `-Rpass-missed='vecpass|loop-vectorize'` |

## 6. Limitations (also in README)

* Single-block innermost loops only. If-converted bodies (`s161`, `s277`)
  and multi-block bodies are skipped.
* Plain pointers that may alias block statement reordering (`p_s1213` is
  refused); load hoisting (`s212`) still works because SCEV proves `a[i]`
  and `a[i+1]` apart.
* No cost model of its own. A reordered loop may still be rejected by
  LoopVectorize's cost model (s116 at AVX2), leaving a harmlessly
  reordered scalar loop.
* LLE runs on the whole function when any loop in it was accepted. LLE is a
  standard O3 pass, but running it earlier could change how other loops in
  that function vectorize.
* LAA records at most 100 dependences (`-max-dependences`). Loops beyond that
  are skipped.
* Measured regressions (RESULTS.md): s116 at `-march=sapphirerapids` 0.64–0.69×
  (LV cost model accepted a slow stride-5 vector loop); some kernels at
  memory-resident sizes 0.87–0.98×; and with clustered array addresses
  (all arrays within 128 B mod 4096) s212/s1213/s211 at 256-bit 0.76–0.91×.
