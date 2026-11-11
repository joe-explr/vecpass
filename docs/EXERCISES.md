# Exercises: learn by changing vecpass

Ordered easy → hard. After each, run `tests/run_all.sh` (it must still
pass), and add a lit test for your change in `tests/ir/`.

---

### 1. Print the reason counts (easy, C++ basics)

Make `vecpass-print` also print, for each innermost loop, whether LAA
thinks its memory accesses are vectorizable.

*Hints:* get `LoopAccessAnalysis` from the FAM the same way `LoopAnalysis`
is fetched: `FAM.getResult<LoopAccessAnalysis>(F)` returns a
`LoopAccessInfoManager &`. Call `.getInfo(*L)` and then `.canVectorizeMemory()`.
Only call it for `L->isInnermost()`. You'll need
`#include "llvm/Analysis/LoopAccessAnalysis.h"`. Note that the loop
variable is a `const Loop *` but `getInfo` wants `Loop &`; see how
`printLoop` already handles that with `const_cast`.

### 2. Count what the pass does (easy, LLVM Statistic)

Add counters for "loops reordered", "loops forwarding-only" and "loops
skipped", printed by `opt -stats`.

*Hints:* `#include "llvm/ADT/Statistic.h"`, `#define DEBUG_TYPE "vecpass"`
*before* it, then `STATISTIC(NumReordered, "Loops reordered");` at file
scope and `++NumReordered;` where the remark is emitted. Release builds of
LLVM only print stats if built with `LLVM_FORCE_ENABLE_STATS`. Check whether the
conda build prints anything, and if not, explain why in a comment.

### 3. Tell the user which instruction moved (medium, remarks)

The "reordered" remark says how many instructions moved. Make it also name
the first moved load or store, with its source location.

*Hints:* `ore::NV("Inst", I)` accepts an `Instruction *` and prints it with
its debug location. You have `Order[K] != K` in `run()` to find moved
nodes. Use `isa<LoadInst>(Insts[Order[K]])`. Update the FileCheck lines in
`tests/ir/s212_hoist.ll`.

### 4. Don't make useless reorders (medium, cost awareness)

s116 is reordered at AVX2 but LoopVectorize still rejects it (cost model),
so the reordering buys nothing. Add a simple filter: refuse loops whose
memory accesses have a non-unit constant stride, unless
`-vecpass-allow-strided` is given. (Asking LoopVectorize's own cost model
would be more precise, but its classes are internal to the vectorizer.)

*Hints:* `getPtrStride(PSE, AccessTy, Ptr, &L, DT)` (in
`LoopAccessAnalysis.h`) returns `std::optional<int64_t>` with the stride in
elements. You need a `PredicatedScalarEvolution PSE(SE, L);`. Then compare
`*Stride` with `1`. Check with `-vecpass-debug` that s116 (stride 5) is now
skipped and the rest still transform.

### 5. Support `if` bodies by if-converting first (harder, pipeline)

TSVC s161/s277 have multi-block bodies, so vecpass skips them. LLVM's
LoopVectorize can if-convert, but only *after* our check. Experiment: is it
enough to run `simplifycfg` with speculation, or LLVM's
`LoopSimplifyCFG`, before vecpass in the extension point, so some bodies
become one block? Measure how many TSVC loops change with
`analysis/run_tsvc_analysis.sh`, and write up whether it's worth it.

*Hints:* in `Plugin.cpp`, the VectorizerStart callback receives an `FPM`;
you can `FPM.addPass(SimplifyCFGPass(SimplifyCFGOptions().hoistCommonInsts(true)
.sinkCommonInsts(true)))` before `VecPass()`. Then think about whether
**moving a load out of a conditional block is legal**. It isn't, in general:
the load might trap when the condition is false. This is why the safety
argument requires one block.

### 6. Statement-level reordering with loop distribution (hard, design)

Some loops have a real recurrence in one statement *and* an order-only
dependence in another. For example:

```c
for (i = 1; i < n; i++) {
  a[i] = a[i-1] + b[i];      // true recurrence: must stay scalar
  c[i] = d[i] * 2;  e[i] = c[i+1];   // order-only problem
}
```

vecpass refuses this, because the whole loop has a cycle. Extend it: find
the strongly connected components (SCCs) of the dependence graph, reorder
the acyclic part, and set `llvm.loop.distribute.enable` metadata so
LoopDistribute splits off the recurrence into its own loop and the rest
vectorizes.

*Hints:* `llvm/ADT/SCCIterator.h` needs a `GraphTraits` adaptor, so it's
easier to write Tarjan's algorithm yourself on `Succ`. Metadata:
`L.setLoopAlreadyUnrolled()` shows how loop metadata is edited, and
`llvm/Transforms/Utils/LoopUtils.h` has `addStringMetadataToLoop(&L,
"llvm.loop.distribute.enable", 1)`. LoopDistribute only runs on loops with
that hint (it's off by default), so check with `-Rpass=loop-distribute`.
Write the safety argument first. Is splitting into two loops still
semantics-preserving when the two parts share an array?
