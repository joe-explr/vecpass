# Learning guide: how vecpass works, file by file

This guide assumes you know C well and some LLVM (from USCC), but are new to
C++. Read it with the source open. Each C++ feature is explained the first
time it shows up, with a C comparison where one helps.

Contents
1. Big picture: where vecpass sits
2. Walkthrough of every file, in reading order
3. LLVM concepts used
4. How LoopVectorize decides, and why it missed our loops

---

## 1. Big picture

```
 kernel.c ──clang frontend──▶ LLVM IR (unoptimized)
                                   │
                         opt pipeline (-O3), function passes, simplified:
                         SROA → EarlyCSE → InstCombine → SimplifyCFG → ...
                         LoopRotate → LICM → IndVarSimplify → GVN → ...
                                   │
                    ┌──────────────┴──────────────┐
                    │  VectorizerStart extension  │ ◀── vecpass runs HERE
                    │  point (our callback)       │     (+ LLE + DCE on
                    └──────────────┬──────────────┘      changed functions)
                                   │
                         LoopVectorize ─ LoopLoadElimination ─ SLP ─ ...
                                   │
                         backend: instruction selection (x86), regalloc
                                   │
                              kernel.s  (ymm/zmm instructions)
```

* **clang** turns C into IR. With `-fpass-plugin=libVecPass.so` it also
  loads our plugin, which inserts vecpass into its `-O3` pipeline.
* **opt** runs IR→IR passes. `opt -load-pass-plugin=build/libVecPass.so
  -passes='default<O3>'` does the same as clang's `-O3` pipeline, plus vecpass.
* vecpass is a **function pass** that runs right before **LoopVectorize**
  (LV). It reorders instructions in some loop bodies so that LV's legality
  check (LoopAccessAnalysis) passes. It never vectorizes anything itself.

Mini example, TSVC s212:

```c
for (i = 0; i < n-1; i++) { a[i] *= c[i];  b[i] += a[i+1] * d[i]; }
```

The IR body, in the order LV sees it (simplified):

```llvm
  %ci   = load c[i]
  %ai   = ...                      ; a[i] (GVN reuses last iteration's a[i+1])
  %m    = fmul %ci, %ai
  store %m, a[i]                   ; (1) store a[i]
  %ai1  = load a[i+1]              ; (2) load a[i+1]   <-- after (1)
  ...
  store ..., b[i]
```

LAA: "(2) in iteration i reads `a[i+1]`, which (1) writes in iteration i+1.
Since (2) is *after* (1) in the body, a vector loop would do all the stores
(1) for 8 lanes first and then the loads (2), so lanes 0..6 would read the
NEW `a[i+1]`. Backward dependence, unsafe." vecpass moves (2) above (1)
(they can't touch the same address in one iteration: `a[i]` vs `a[i+1]`).
Now the loads come first and LV is happy.

---

## 2. Walkthrough, in reading order

### 2.1 `CMakeLists.txt`

* `find_package(LLVM REQUIRED CONFIG)` reads `LLVMConfig.cmake` from the LLVM
  install (`-DLLVM_DIR=...`). It defines `LLVM_INCLUDE_DIRS`,
  `LLVM_DEFINITIONS`, `LLVM_ENABLE_RTTI`, and so on.
* `add_compile_options(-fno-rtti)`: LLVM is built without C++ RTTI
  (runtime type information: what `dynamic_cast`/`typeid` use). Our classes
  derive from LLVM classes, so we must match, or the `.so` would reference
  `typeinfo` symbols that don't exist. LLVM has its own cheaper type test,
  `isa<>/dyn_cast<>` (2.6).
* `add_library(VecPass MODULE ...)`: `MODULE` = a shared object only for
  `dlopen()` (like a C plugin `.so`). We don't link libLLVM; the symbols
  come from the `opt`/`clang` process that loads us.

### 2.2 `scripts/env.sh`

Puts the conda LLVM 23.1.5 first in `PATH`, sets `VECPASS_MARCH`. Why
conda: the cluster's LLVM module has no headers (docs/DESIGN.md §1).

### 2.3 `src/LoopPrinter.h`: first C++ file

```cpp
namespace vecpass {
struct LoopPrinterPass : llvm::PassInfoMixin<LoopPrinterPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
  static bool isRequired() { return true; }
};
}
```

New C++ here:

* **`namespace vecpass { ... }`**: a named scope for identifiers. Our
  `LoopPrinterPass` is really `vecpass::LoopPrinterPass`, so it can't clash
  with anything in LLVM (whose names are in `namespace llvm`). In C you'd
  prefix every name (`vecpass_loop_printer`); namespaces make that
  automatic. `llvm::Function` = "the `Function` from namespace `llvm`".
* **`struct` with functions inside.** In C++ a `struct` can contain
  *member functions* (methods). `run` is called as `pass.run(F, FAM)`, and
  inside it the object is available as `this` (like an implicit first
  argument `struct LoopPrinterPass *self`). `struct` and `class` are the same
  thing, except members of a `struct` are public by default and members of
  a `class` are private by default.
* **`: llvm::PassInfoMixin<LoopPrinterPass>`** is **inheritance**: our struct
  gets all the members of `PassInfoMixin<...>` too. `PassInfoMixin` is a
  **template**: a struct parameterized by a type, written `Name<Type>`. The
  compiler stamps out a copy for each type it's used with (think of a
  type-safe macro). Passing *our own type* to our base class
  (`X : Base<X>`) is a known pattern called CRTP. It lets `PassInfoMixin` provide
  `name()` and similar helpers that know the concrete pass type, at compile time
  and with no virtual calls.
* **`llvm::Function &F`** is a **reference**. Think "a pointer that can't be
  null, can't be re-pointed, and is used without `*` or `->`". `F.getName()`
  instead of `F->getName()`. In C you'd write `Function *F`. References say
  "this must exist", so there's no NULL check to forget.
* **`static bool isRequired()`**: a `static` member function belongs to the
  type, not to an object (no `this`). The pass manager calls
  `LoopPrinterPass::isRequired()` to decide whether to run the pass even on
  `optnone` functions (clang `-O0`).

### 2.4 `src/LoopPrinter.cpp`

```cpp
static void printLoop(const Loop &L, ScalarEvolution &SE, StringRef FnName)
```

* **`using namespace llvm;`** at the top lets us write `Loop` instead of
  `llvm::Loop`. That's fine in a `.cpp`; never do it in a header.
* **`const Loop &L`**: a reference to a loop we promise not to modify. Calling
  a non-`const` method on it is a compile error.
* **`StringRef`**: LLVM's "pointer + length" view of a string. It doesn't
  own the characters (like passing `const char *s, size_t len` in C). It's cheap
  to copy and never mallocs.
* `raw_ostream &OS = errs();` then `OS << "text" << number << *BTC;`:
  LLVM's `fprintf`. `<<` is an **overloaded operator**: C++ lets a type define
  what `<<` means for it. Each `<<` appends and returns the stream, so they
  chain. `*BTC` dereferences a `const SCEV *` and prints the expression.
* `if (unsigned TC = SE.getSmallConstantTripCount(LP)) {...}`: a declaration
  inside an `if`. `TC` exists only in that if/else and the branch is taken
  when it's non-zero. Same as C99 `for (int i...)` scoping, but for `if`.
* `isa<SCEVCouldNotCompute>(BTC)`: an LLVM type test (2.6).
* `const_cast<Loop *>(&L)` removes `const` from a pointer. Used because
  `getSmallConstantTripCount` takes a non-const `Loop *` even though it
  doesn't modify the loop. It's a C++ named cast; the comment says why it's safe.
* **Range-for**: `for (const Loop *Sub : L.getSubLoops())` is equivalent to
  `for (it = begin; it != end; ++it) { const Loop *Sub = *it; ... }`. It works
  on anything with `begin()`/`end()` **iterators** (generalized pointers:
  `*it` reads the element, `++it` advances). For a C array it's
  `for (T *p = a; p != a + n; ++p)`.

The pass entry point:

```cpp
PreservedAnalyses LoopPrinterPass::run(Function &F, FunctionAnalysisManager &FAM) {
  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);
  ...
  return PreservedAnalyses::all();
}
```

* `LoopPrinterPass::run` = "the `run` that belongs to `LoopPrinterPass`"
  (defining a member function outside the struct).
* `FAM.getResult<LoopAnalysis>(F)` is a **template member function**: the
  type in `<...>` selects which analysis to compute. The analysis manager
  runs it if needed, caches it, and returns a reference to the result
  (`LoopInfo`). The manager owns that object. We must not delete it.
* **`PreservedAnalyses`** is how a pass tells the manager what it didn't
  break. `all()` = "I changed nothing, keep every cached analysis".
  `none()` = "throw everything away". vecpass returns something in between
  (2.8).

### 2.5 `src/Plugin.cpp`: registration

```cpp
static cl::opt<bool> AutoInsert("vecpass-auto", cl::init(true), cl::desc(...));
```

* A **global object with a constructor**. C++ runs constructors of globals
  when the `.so` is loaded. `cl::opt`'s constructor registers the flag
  `-vecpass-auto` with LLVM's command-line parser. `AutoInsert` then behaves
  like a `bool` (`if (AutoInsert)`). `static` at file scope means the same as in C:
  private to this file.

```cpp
PB.registerPipelineParsingCallback(
    [](StringRef Name, FunctionPassManager &FPM, ArrayRef<PassBuilder::PipelineElement>) {
      if (Name == "vecpass") { FPM.addPass(vecpass::VecPass()); return true; }
      return false;
    });
```

* **Lambda**: `[](args) { body }` is an unnamed function written inline. The
  `[]` is the **capture list**: which local variables of the enclosing
  function the lambda may use. `[]` = none (so it's just a function
  pointer, like passing a C callback). `[&]` (used later) = "use any local
  variable, by reference".
* `Name == "vecpass"`: `StringRef` overloads `==` to compare contents (no
  `strcmp`).
* `vecpass::VecPass()` creates a temporary object. `addPass` stores a copy.
* `ArrayRef<T>`: a pointer + length view of an array, like `StringRef` for
  arrays.

`registerVectorizerStartEPCallback` is the same idea for an *extension
point*: "when you build an O2/O3 pipeline, call me at the point just before
the vectorizer, and I'll add my pass there".

```cpp
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "VecPass", "0.1", registerCallbacks};
}
```

* **`extern "C"`**: C++ mangles names (encodes argument types into symbol names,
  so `f(int)` and `f(float)` can coexist). `extern "C"` turns that off, so `opt`
  can `dlsym(handle, "llvmGetPassPluginInfo")`.
* `return {a, b, c, d};` builds the returned struct field by field, like a C
  compound literal.

### 2.6 LLVM's `isa<>`, `cast<>`, `dyn_cast<>`

LLVM IR classes form a hierarchy: `Value` → `Instruction` → `LoadInst`,
`StoreInst`, ... Given an `Instruction *I`:

| | meaning | C analogue |
|---|---|---|
| `isa<LoadInst>(I)` | is it a load? → `bool` | `I->kind == LOAD` |
| `cast<LoadInst>(I)` | it **is** a load (asserts in debug builds) → `LoadInst *` | `(LoadInst *)I` after a check |
| `dyn_cast<LoadInst>(I)` | a `LoadInst *` if it is one, else `nullptr` | `I->kind == LOAD ? (LoadInst *)I : NULL` |

They are templates that use a small `getValueID()` tag inside each object
(LLVM's replacement for RTTI). Typical use, from `checkShape`:

```cpp
if (auto *LI = dyn_cast<LoadInst>(&I)) {   // only true for loads
  if (!LI->isSimple()) ...
}
```

* **`auto`**: "let the compiler figure out the type". Here `auto *LI` is a
  `LoadInst *`. Use it when the type is obvious from the right-hand side;
  don't use it to hide what something is.

### 2.7 `src/VecPass.h`

Same shape as `LoopPrinter.h`. `VecPass` has no data members. Each run
creates its own helper objects, so the pass is safe to run on many functions.

### 2.8 `src/VecPass.cpp`: the transform

Read it top to bottom. Section by section:

**Options** (`RunLLE`, `Debug`, `MaxBodySize`): `cl::opt` globals as above.
Try `opt ... -passes=vecpass -vecpass-debug` to see dependences and bodies.

**Anonymous namespace** `namespace { ... }`: everything inside is private to
this file. It's C's file-scope `static`, applied to types too.

**`isBackwardKind` / `isForwardKind`**: classify LAA's dependence kinds.
`using D = MemoryDepChecker::Dependence;` is a local type alias, like a
`typedef`. `D::Backward` names an enumerator inside that struct. Enums can
live inside structs in C++.

**`struct Analyses`**: a bundle of references to the analyses one loop
needs. Members that are references must be set when the object is created:
`Analyses A{LI, DT, SE, ...};` (brace initialization, in member order).

**`class LoopReorderer`**: one object per loop, created in `VecPass::run`
and destroyed at the end of that loop iteration.

* The constructor: `LoopReorderer(Loop &L, Analyses &A) : L(L), A(A), ...
  {}`. The part after `:` is the **member initializer list**: it
  initializes each member *before* the body runs. References *must* be
  initialized this way. (`L(L)` means "member `L` from parameter `L`".)
* `public:` / `private:` sections: `run()` is the only thing callers may use.
* Data members:
  * `SmallVector<Instruction *, 64> Insts;` is LLVM's growable array.
    `SmallVector<T, N>` keeps the first N elements *inside the object* (no
    malloc), and only heap-allocates if it grows past N. It's like a C struct
    with `T inline_buf[64]; T *data; size_t size, cap;`. `push_back(x)` appends.
    `Insts[i]` indexes. `Insts.size()` is the length.
  * `DenseMap<Instruction *, unsigned> Pos;` is a hash map (instruction →
    its original index). `Pos[&I] = k` inserts. `Pos.find(X)` returns an
    iterator, and `== Pos.end()` means "not found". `It->second` is the
    value (`It->first` the key).
  * `std::vector<SmallVector<unsigned,4>> Succ;` is the graph's adjacency
    lists. `std::vector` is the standard library's growable array.
  * `enum class Mode { Illegal, CappedVF } M = Mode::Illegal;` is a *scoped*
    enum: you must write `Mode::Illegal`, and it doesn't silently convert to
    `int` (safer than C `enum`).
* `bool skip(StringRef Tag, const Twine &Why)` emits a "missed" remark and
  returns `false`, so callers write `return skip("Tag", "why");`.
  * `Twine` is a lazy string concatenation: `"has " + Twine(n) + " blocks"`
    builds nothing until `.str()` is called. Use it only as a function parameter.
    Never store one, because it points at temporaries that disappear.
  * `ORE.emit([&]() { return OptimizationRemarkMissed(...) << "..."; });`
    passes a lambda that *builds* the remark. ORE calls it only if remarks are
    enabled (`-Rpass-missed=vecpass`), so normal compiles pay nothing.
    `[&]` captures `Msg` and `L` by reference.

**`checkShape()`**: the loop must be innermost, one block, and have a
preheader. It walks the block from `getFirstNonPHIIt()` (PHIs stay at the top)
and records each instruction in `Insts`/`Pos`. Stores and loads must be
`isSimple()` (not volatile/atomic). Any other instruction that touches memory
or has side effects (a call) means we give up.

**`collectDeps()`**: asks LAA (2.9 and §3) for the dependence list.
For each dependence it finds the two instructions' positions `Lo < Hi`:
* backward kind → `addEdge(Hi, Lo)`: the later instruction must move first.
* forward kind → `addEdge(Lo, Hi)`: keep it as it is.
* anything LAA couldn't analyze → give up.
If LAA already says "legal, any width", there's nothing to do. If legal but
width-capped, we switch to `Mode::CappedVF`.

**`mayAliasInSameIteration(A, B)`**: the safety core. It returns `false` only
with a proof:
1. `AA.isNoAlias(MemoryLocation::get(A), MemoryLocation::get(B))`: LLVM alias
   analysis (different globals, `restrict`, TBAA types, ...).
2. SCEV: `SE.getMinusSCEV(PA, PB)` is the address difference as a SCEV
   expression. If it's a constant `D` (a `SCEVConstant`) and `|D|` is at
   least the access size in the right direction, the byte ranges can't overlap.
   `TypeSize` exists because scalable vectors (ARM SVE) have sizes unknown at
   compile time. We only use fixed sizes.

**`buildMemoryAndSSAEdges()`**: the "never break one iteration" edges:
(a) operand → user, (b) earlier → later for every may-alias pair involving
a store. It's an O(n²) loop over pairs, fine for ≤ 256 instructions.

**`topoSort()`**: Kahn's algorithm. `std::priority_queue<unsigned,
std::vector<unsigned>, std::greater<unsigned>>` is a min-heap of
indices (the three template arguments: element type, storage, comparison).
We always emit the ready instruction with the smallest original index, so
the new order is as close to the old one as the edges allow. If fewer than
all nodes come out, there's a cycle and no valid order exists.

**`computeLAA()`**: `std::make_unique<LoopAccessInfo>(args...)` allocates
a `LoopAccessInfo` on the heap and returns a **`std::unique_ptr`** owning it.
This is **RAII** (Resource Acquisition Is Initialization): the object frees
itself when the `unique_ptr` variable goes out of scope, so there's no
`free()` to forget and no leak on early `return`. You can't copy a
`unique_ptr` (there's only one owner), but you can move it.

**`apply(NewOrder)`**: `I->moveBefore(Term->getIterator())` moves each
instruction to just before the terminator, in order, which produces exactly
the new sequence. Calling `apply(OldOrder)` undoes it.

**`acceptableAfterLLE()`**: is the new LAA result good enough, assuming
LoopLoadElimination will run? `std::string &Why` is an *out-parameter by
reference* (like `char **why` in C, but it can't be null).

**`run()`**: the driver, in the order of DESIGN.md §4. Note
`std::unique_ptr<LoopAccessInfo> Before = computeLAA();` then
`collectDeps(*Before)`: `*` gives the object the smart pointer owns.

**`VecPass::run(Function &F, FunctionAnalysisManager &FAM)`**:
1. Gather analyses into `Analyses A{...}`.
2. `for (Loop *L : A.LI.getLoopsInPreorder())` creates a `LoopReorderer` per loop.
3. Nothing changed? `return PreservedAnalyses::all();`
4. Something changed? Build
   `PreservedAnalyses PA; PA.preserveSet<CFGAnalyses>(); PA.preserve<ScalarEvolutionAnalysis>();`.
   We moved instructions but didn't touch the control-flow graph, so
   LoopInfo/DominatorTree stay valid, and SCEV describes values (not
   positions), so it stays valid too. LAA and MemorySSA depend on
   instruction order and are dropped.
5. `FAM.invalidate(F, PA)` tells the manager now, before we run two
   stock LLVM passes ourselves: `LoopLoadEliminationPass().run(F, FAM)` and
   `DCEPass().run(F, FAM)`. `PA.intersect(...)` keeps only what *all three*
   steps preserved.

### 2.9 Tests

* `tests/test_print.sh` + `tests/c/loops.check`: Phase 1, FileCheck on stderr.
* `tests/ir/*.ll|*.c` + `lit.cfg.py`: lit runs every `; RUN:` line,
  substituting `%opt`, `%s`, `%t`. FileCheck then matches the `CHECK:` lines in
  order. `CHECK-NOT` = must not appear. `CHECK-LABEL` = anchor. Prefixes
  (`--check-prefix=VEC`) let one file hold checks for several RUN lines.
  Run: `lit -v tests/ir`.
* `tests/test_diff.sh`: builds `kernels.c` and `tests/diff/ptr_kernels.c` with and
  without vecpass, runs the same driver, and requires identical output.

---

## 3. LLVM concepts used

**Loop / LoopInfo.** A natural loop: a *header* block that dominates every
block in the loop, plus *back edges* to it. `LoopInfo` finds all of them and
their nesting. Canonical-form terms:
* *preheader*: the single block that enters the header from outside.
* *latch*: the block with the back edge.
* *rotated loop* (after LoopRotate): the exit test is at the bottom
  (`do { body } while (cond)` guarded by an initial check). The header is also
  the latch, so a simple loop has a **single-block body**, which is what
  vecpass needs.

**Dominator tree.** Block A *dominates* B if every path from entry to B goes
through A. It's needed to know where a value is available. LAA and LLE use it.

**ScalarEvolution (SCEV).** Symbolic algebra over integer and pointer values in
loops. An induction variable is an *add recurrence* `{start,+,step}<loop>`.
`a[i+1]`'s address is `{@a + 4,+,4}`. SCEV can subtract two such
expressions (`getMinusSCEV`), giving the constant `4`. It also computes trip
counts (`getBackedgeTakenCount`) and is the basis of LAA's distance
computations.

**Alias analysis (AA).** "Can these two memory locations overlap?" Answers
`NoAlias`, `MayAlias`, `PartialAlias`, `MustAlias`. The AA manager
chains several analyses: BasicAA (different objects, constant offsets),
TBAA (C type rules: a `float *` store can't modify an `int`), ScopedNoAlias
(`restrict`), GlobalsAA.

**LoopAccessAnalysis (LAA).** The vectorizer's memory legality checker. For
each pair of accesses to possibly the same array, at least one a write, it
computes the dependence distance (via SCEV) and classifies it with
`MemoryDepChecker`:

| kind | meaning | vectorizable? |
|---|---|---|
| `NoDep` | never the same location | yes |
| `Forward` | earlier-in-time access is earlier in the body | yes |
| `ForwardButPreventsForwarding` | forward, but vector load would partially overlap a just-stored vector (stall) | no (LV treats as unsafe) |
| `Backward` | earlier-in-time access is *later* in the body, distance < VF | no |
| `BackwardVectorizable` | backward, but distance ≥ some VF | yes, capped VF |
| `Unknown` | distance not computable | maybe, with runtime checks |
| `IndirectUnsafe`, `InvariantUnsafe` | `a[b[i]]`, invariant address written | no |

LAA also creates **runtime checks** when pointers might alias (memchecks on
address ranges, or the cheaper "diff checks" on pointer distance that made
the runtime-offset idea unnecessary).

**Loop versioning.** Clone a loop, guard it with a runtime condition, and use
the fast version when the condition holds. LV, LLE and LoopVersioningLICM all use
`llvm::LoopVersioning`. vecpass doesn't version: reordering is legal
unconditionally.

**Loop metadata.** `!llvm.loop !N` on the latch branch carries hints such as
`llvm.loop.vectorize.width`, `llvm.loop.distribute.enable`, and "already
vectorized" markers. vecpass doesn't add any. (EXERCISES.md #4 asks you to.)

**Optimization remarks.** `OptimizationRemark` (did something),
`...Missed` (couldn't), `...Analysis` (why). Seen with
`clang -Rpass=vecpass -Rpass-missed=vecpass` or
`opt -pass-remarks=vecpass -pass-remarks-missed=vecpass`. clang keeps only
the *last* `-Rpass-missed=` flag, so combine them with a regex:
`-Rpass-missed='vecpass|loop-vectorize'`.

**New pass manager.** Passes are plain classes with
`run(IRUnit&, AnalysisManager&)`, with no inheritance from a `Pass` base. Analyses
are computed on demand by the **analysis manager**, which caches results
and drops them when a pass's `PreservedAnalyses` says they're stale. Pipelines
are text (`-passes='function(vecpass,loop-vectorize)'`) or built by
`PassBuilder` (`default<O3>`), with **extension points** where plugins add
passes.

---

## 4. How LoopVectorize decides

1. **Legality** (`LoopVectorizationLegality`): a countable loop, a
   supported CFG, only vectorizable instructions, recognised inductions,
   reductions and first-order recurrences, and **memory safety from
   LAA** (`canVectorizeMemory()`, maybe with runtime checks and a max safe
   VF). Our loops failed here, on the LAA step.
2. **Cost model** (`LoopVectorizationCostModel`, in VPlan): for each
   candidate VF (2, 4, 8, 16 for floats with 512-bit vectors) it asks TTI
   for the cost of each widened instruction (loads, stores, shuffles for
   interleaved groups, gathers) and compares cost per scalar iteration to
   the scalar loop. It picks the cheapest VF. On Sapphire Rapids LLVM *prefers
   256-bit* vectors unless `-mprefer-vector-width=512`, because of AVX-512
   frequency licensing on older Xeons, so max VF for float is 8 (ymm) by
   default.
3. **Interleave count (UF)**: unroll the vector body to hide latency, chosen
   from register pressure and trip-count estimates. In remarks this is
   "interleaved count: 2".
4. **Epilogue**: a scalar remainder loop (or vectorized epilogue) for
   `n % (VF*UF)` iterations, plus runtime-check blocks.

Why our loops were missed: in step 1, LAA reported `Backward` (s212-type)
or, after reordering, `ForwardButPreventsForwarding` (s1213-type). LV only
classifies the dependences in the order it was given; it never reorders.
And the pass that would fix the second problem (LLE) runs after LV in the
default pipeline. vecpass changes the input to step 1 and lets LLVM do
everything else.

What vecpass can't change: step 2. s116 is reordered and legal, but at AVX2
the cost model still rejects its stride-5 interleaved groups.
