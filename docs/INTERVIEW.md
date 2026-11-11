# Interview questions about vecpass, with short answers

Answers are grounded in this repo's code and measurements. File references
are to `src/VecPass.cpp` unless noted. Numbers come from `RESULTS.md`.

---

**1. What does vecpass do, in one sentence?**
For innermost single-block loops that LoopVectorize rejects because of a
*lexically backward* memory dependence, it reorders the instructions inside
one iteration so every loop-carried dependence becomes forward, then lets
LLVM's own LoopVectorize vectorize the loop.

**2. Give a concrete example.**
TSVC s212: `a[i] *= c[i]; b[i] += a[i+1]*d[i];`. The load of `a[i+1]` comes
after the store to `a[i]`, and next iteration's store overwrites `a[i+1]`.
LAA calls that Backward. In one iteration `a[i]` and `a[i+1]` are 4 bytes
apart, so the load can move above the store. After that, clang vectorizes it.

**3. Why this transform? How did you choose it?**
From data. I compiled TSVC_2 with clang 23 `-Rpass-analysis` and icx 2025.0
`-qopt-report` (`analysis/missed.md`). 25 of the 44 missed loops are "backward
loop-carried dependence", the largest class. Eight of those are not true
recurrences but order-only. Neither clang nor icx vectorizes those eight,
and I verified that hand-reordered source does vectorize.

**4. The obvious candidate was a runtime offset `a[i] = a[i+k]`. Why not that?**
I checked first. LLVM 23 already handles it: LAA emits a dependence-distance
check (`diff.check: (src - sink) ugt -VF*UF*size`) instead of an overlap
check, so the vector loop runs for nearly all k. TSVC s162 and s174 are
vectorized by stock clang. Implementing it would have duplicated LLVM.

**5. Why doesn't LLVM do this already?**
LoopVectorize widens instructions in their existing order. LAA's
`MemoryDepChecker` only classifies the order it's given. LoopDistribute
(off by default) splits loops but doesn't reorder within one. The SLP
scheduler reorders, but within straight-line code, not across iterations. For the
s1213 family there's also a pass-ordering issue: LoopLoadElimination, which
removes the "prevents store-to-load forwarding" dependence, runs *after*
LoopVectorize in the O3 pipeline.

**6. What's the safety argument?**
Only instructions inside one basic block move, so cross-iteration order is
unchanged: all of iteration i still precedes iteration i+1. Within an
iteration, reordering straight-line code is correct iff every def→use
order is kept and every pair of possibly-overlapping accesses (at least
one a store) keeps its order. The graph edges (a) and (b)
(`buildMemoryAndSSAEdges`) encode exactly that. Calls, volatile and atomics are
excluded. Vector legality isn't my claim: LoopVectorize re-runs LAA.
Full version in docs/DESIGN.md §4.

**7. How do you decide two accesses can't overlap in the same iteration?**
`mayAliasInSameIteration`: (1) AA `isNoAlias` (BasicAA, TBAA, `restrict`
scopes). (2) If SCEV says `ptrA - ptrB` is a constant D and the byte ranges
`[D, D+sizeA)` and `[0, sizeB)` are disjoint. Otherwise I assume they
overlap. Both pointers are SSA values in the same block, so the query is about
the same iteration.

**8. Why can you use AA for a same-iteration question?**
An AA query compares two SSA values at a program point where both are
defined. In a single block in one iteration, those are the values of that
iteration. LLVM's SLP scheduler and MemorySSA use AA the same way for
intra-block ordering.

**9. What if the pointers might alias, e.g. plain `float *a, *b`?**
Then edge (b) forces the original order between them. Moving a whole
statement (s1213) creates a cycle with the backward-dependence edge, so the
topological sort fails and the loop is skipped with a remark (`p_s1213` in
the tests). Hoisting `a[i+1]` above `a[i]` (s212) still works with plain
pointers, because SCEV proves those two apart. LoopVectorize then adds its own
runtime alias checks for `a` vs `b`.

**10. How do you find the new order?**
Kahn's topological sort (`topoSort`), with ties broken by original index
(a min-heap), so instructions move only when an edge forces them to. A cycle
means no legal order exists: a true recurrence, or may-alias blocking.

**11. What happens if your reorder doesn't make the loop legal?**
I re-run LAA from scratch on the new order (`computeLAA`). If it isn't legal
(or legal-after-LLE), I move everything back (`apply(OldOrder)`) and emit a
"reverted" remark. The pass never leaves a change that didn't help LAA.

**12. Why a fresh LoopAccessInfo rather than the analysis manager's?**
A bug I hit. `LoopAccessInfoManager::clear()` only evicts entries that have
runtime checks, so after reordering it returned the analysis of the *old*
order, and every reorder was "reverted". Building a fresh `LoopAccessInfo`
fixed it.

**13. Why do you run LoopLoadElimination, and why DCE after it?**
After reordering s1213, `store b[i]` precedes the next iteration's
`load b[i-1]`: forward, distance 1, which LAA marks
`ForwardButPreventsForwarding` (a partially overlapping vector load right after
a vector store would stall store forwarding). LLE replaces that load with a PHI
of the previously stored value, which LV handles as a first-order recurrence.
LLE leaves the dead load in the IR, and LAA would still count it, so I run
DCE. Both run only on functions vecpass changed.

**14. Where in the pipeline does it run, and why there?**
`registerVectorizerStartEPCallback`, immediately before LoopVectorize. By then
loops are rotated (single-block bodies), LICM'd and GVN'd, and nothing between
us and LV can undo the order. I tested `function(vecpass),default<O3>`:
every loop was still multi-block and got skipped.

**15. What do the remarks look like?**
`-Rpass=vecpass`: `reordered loop body (4 of 15 instructions moved) so that
1 backward dependence(s) become forward`. `-Rpass-missed=vecpass` gives one
reason per skipped loop: not innermost, multi-block, call, volatile,
unfixable dependence kind, cycle, reverted, already legal.

**16. How did you test correctness?**
Three layers. (1) lit/FileCheck IR tests: exact before/after order,
skip reasons, and "loading the plugin changes nothing" for skipped
functions (diff of O3 output). (2) Differential test: the same driver linked
with kernels built with and without vecpass, 2668 bit-exact hash lines over
trip counts 0–4097 (around every VF multiple), overlapping pointer
arguments, runtime offsets k = -33..33, float/double/int. Run at AVX2 on the
login node and AVX2/AVX-512 on the Xeon node. (3) `-verify-each` over all of
TSVC.

**17. Why are results bit-identical? Vectorization usually changes FP results.**
Only reductions get reassociated, and only under fast-math. Our loops are
element-wise, and reordering independent operations doesn't change any
operation's operands. Each element is computed by the same operations on the
same inputs, so bit-exact equality is the right test.

**18. What speedup did you get?**
See RESULTS.md for the table. Gains are largest for L1/L2-resident data,
where the scalar loop is compute/latency-bound. At memory size, both versions
are bandwidth-bound and the gap shrinks.

**19. AVX2 vs AVX-512: what's different?**
With `-march=sapphirerapids` LLVM *prefers 256-bit* vectors (ymm, VF 8 for
float); `-mprefer-vector-width=512` gives zmm (VF 16). We build three variants
(x86-64-v3 AVX2, SPR default 256-bit, SPR 512-bit). AVX-512 also gives masked
epilogues and more registers. The asm summary in RESULTS.md shows ymm vs zmm
usage per kernel. s116 is one case where the ISA matters: its stride-5
interleaving is vectorized at SPR but rejected by the AVX2 cost model.

**20. What do runtime checks cost here?**
For TSVC-style global arrays: none, since AA proves the arrays distinct.
For pointer arguments, LV adds the usual alias checks: a few compares and a
branch per loop entry, which matters only for tiny trip counts. vecpass adds
no runtime check of its own. That's a design advantage over versioning.

**21. Does it ever make code slower?**
It can reorder a loop that LV then still rejects on cost (s116 at AVX2),
which gives a different but equivalent scalar order. RESULTS.md reports every
kernel, including those that didn't improve.

**22. What are the limitations?**
Innermost single-block loops only. No calls, volatile or atomics. May-alias
pointers block statement moves. No cost model of its own. LLE runs on the
whole changed function. LAA's 100-dependence cap. Doesn't help true
recurrences (s321) or reductions LLVM doesn't recognise (s315, argmax).

**23. How would you upstream it?**
Not as a separate pass. Most naturally inside LoopVectorize's legality/VPlan
stage: when LAA reports only order-only backward dependences, compute the
reordering (or record it in VPlan so the widened recipes are emitted in the
new order) without touching scalar IR. Alternatively, as an LAA-guided
"LoopReorder" utility called from LoopDistribute. In both cases: an RFC on
Discourse with the TSVC data, lit tests in `llvm/test/Transforms/LoopVectorize`,
compile-time numbers on CTMark, and a flag defaulting to off at first. The
LLE-before-LV ordering would also need discussion: maybe let LV query LLE's
forwarding candidates directly.

**24. What's the complexity?**
O(n²) alias queries for n ≤ 256 instructions (configurable), plus two LAA
runs per candidate loop. Only loops LAA already rejected (or VF-capped) get
past the cheap checks. Compiling all of TSVC through `opt -O3 -verify-each`
with the plugin took about 4 s.

**25. If you had another month, what next?**
(1) Multi-block bodies, after if-conversion, with a speculation-safety argument.
(2) Partial reordering plus loop distribution for loops with one true
recurrence (EXERCISES.md #6). (3) A VPlan-level version that doesn't change
scalar IR. (4) Run on SPEC/real code to measure how often the pattern
appears outside TSVC. (5) Compare against icx's statement reordering
(it vectorized all hand-reordered versions, so it has the codegen but not
the dependence-driven reordering).
