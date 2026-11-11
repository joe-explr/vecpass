// VecPass.cpp — the `vecpass` transform: dependence-driven reordering of a
// loop body so LoopVectorize can vectorize it.
//
// Reading guide (top to bottom):
//   1. Options and small helpers.
//   2. LoopReorderer — all the work for ONE loop:
//        checkShape()     is the loop simple enough for us?
//        collectDeps()    ask LAA which dependences block vectorization
//        buildMemoryAndSSAEdges()  "must come before" edges between
//                         instructions (collectDeps adds the LAA edges)
//        topoSort()       find a new order that satisfies every edge
//        apply()          move the instructions (also used to undo)
//        computeLAA() + acceptableAfterLLE()
//                         re-run LAA on the new body; undo if it didn't help
//   3. VecPass::run — walks the loops of a function, then (if anything
//      changed) runs LLVM's LoopLoadElimination + DCE on that function.
//
// Safety in one paragraph (details in docs/DESIGN.md): we only change the
// order of instructions *inside one iteration*. All instructions of iteration
// i still run before all instructions of iteration i+1, so cross-iteration
// order is untouched. Inside an iteration we keep (a) every SSA def->use
// order and (b) the order of every pair of memory accesses that might touch
// the same bytes in the same iteration (one of them a store). That is exactly
// the condition under which reordering a straight-line block is
// semantics-preserving; it is the same rule LLVM's SLP vectorizer scheduler
// uses. Whether the *vector* loop is legal is then decided by LoopVectorize
// itself, with its own LAA run: we never assert vector legality ourselves.
#include "VecPass.h"

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/Scalar/DCE.h"
#include "llvm/Transforms/Scalar/LoopLoadElimination.h"

#include <functional>
#include <memory>
#include <queue>
#include <string>
#include <vector>

using namespace llvm;

#define VP_NAME "vecpass" // remark pass name: -Rpass=vecpass / -pass-remarks=vecpass

// ---------------------------------------------------------------------------
// 1. Options
// ---------------------------------------------------------------------------
// cl::opt<T> declares a command-line flag (a global object whose constructor
// registers it). Use with opt directly, or `clang -mllvm` (see README).
static cl::opt<bool> RunLLE(
    "vecpass-run-lle", cl::init(true),
    cl::desc("After reordering, run LoopLoadElimination on the function so "
             "forward dependences that prevent store-to-load forwarding are "
             "turned into register recurrences before LoopVectorize runs"));

static cl::opt<bool> Debug("vecpass-debug", cl::init(false),
                           cl::desc("Print dependences and the loop body "
                                    "before/after reordering to stderr"));

static cl::opt<unsigned> MaxBodySize(
    "vecpass-max-body", cl::init(256),
    cl::desc("Skip loops whose body has more instructions than this"));

namespace {

// Which LAA dependence kinds mean "the loop is unsafe because a later
// instruction in the body must observe memory before an earlier one" — the
// kind we can fix by reordering.
bool isBackwardKind(MemoryDepChecker::Dependence::DepType T) {
  using D = MemoryDepChecker::Dependence;
  return T == D::Backward || T == D::BackwardVectorizable ||
         T == D::BackwardVectorizableButPreventsForwarding;
}

// Kinds that are fine as-is but must not be flipped by our reordering.
bool isForwardKind(MemoryDepChecker::Dependence::DepType T) {
  using D = MemoryDepChecker::Dependence;
  return T == D::Forward || T == D::ForwardButPreventsForwarding;
}

// The function-level analyses a LoopReorderer needs, bundled in one struct
// so we can pass them around as a single argument. (A struct with only
// public members, like a C struct, plus references instead of pointers.)
struct Analyses {
  LoopInfo &LI;
  DominatorTree &DT;
  ScalarEvolution &SE;
  AAResults &AA;
  TargetTransformInfo &TTI;
  const TargetLibraryInfo &TLI;
  AssumptionCache &AC;
  OptimizationRemarkEmitter &ORE;
};

// ---------------------------------------------------------------------------
// 2. LoopReorderer: everything we do to one loop.
// ---------------------------------------------------------------------------
class LoopReorderer {
public:
  LoopReorderer(Loop &L, Analyses &A)
      : L(L), A(A), AA(A.AA), SE(A.SE), ORE(A.ORE),
        DL(L.getHeader()->getDataLayout()) {}

  // Returns true if the loop body was changed.
  bool run();

private:
  // References are like non-null pointers that can't be re-seated. We store
  // them because the analyses outlive this short-lived object.
  Loop &L;
  Analyses &A;
  AAResults &AA;
  ScalarEvolution &SE;
  OptimizationRemarkEmitter &ORE;
  const DataLayout &DL;

  // The body instructions we are allowed to reorder (no PHIs, no terminator),
  // in their original order; Pos maps an instruction back to its index.
  SmallVector<Instruction *, 64> Insts;
  DenseMap<Instruction *, unsigned> Pos;

  // Graph: Succ[i] = nodes that must come after node i.
  std::vector<SmallVector<unsigned, 4>> Succ;
  std::vector<unsigned> InDegree;

  unsigned NumBackward = 0; // backward deps LAA reported (what we fix)

  // Two situations we handle:
  //  * Illegal: LAA rejects the loop (canVectorizeMemory() == false).
  //  * CappedVF: LAA accepts it, but only up to a maximum vector width,
  //    because a backward dependence has a small distance (e.g. a[i+2] read
  //    after a[i] is written -> at most 2 lanes). Reordering can lift the cap.
  enum class Mode { Illegal, CappedVF } M = Mode::Illegal;
  uint64_t CapBitsBefore = 0; // max safe vector width before, in bits

  bool checkShape();
  bool collectDeps(const LoopAccessInfo &LAI);
  void addEdge(unsigned From, unsigned To);
  bool mayAliasInSameIteration(Instruction *A, Instruction *B);
  void buildMemoryAndSSAEdges();
  bool topoSort(SmallVectorImpl<unsigned> &Order);
  void apply(ArrayRef<Instruction *> NewOrder);
  bool acceptableAfterLLE(const LoopAccessInfo &LAI, std::string &Why);
  std::unique_ptr<LoopAccessInfo> computeLAA();

  // Emit a "missed" remark: we looked at this loop and left it alone.
  // Always returns false, so callers can write `return skip(...);`.
  bool skip(StringRef Tag, const Twine &Why) {
    std::string Msg = Why.str(); // Twine is a lazy string; materialize it
    ORE.emit([&]() {
      return OptimizationRemarkMissed(VP_NAME, Tag, L.getStartLoc(),
                                      L.getHeader())
             << "loop not reordered: " << Msg;
    });
    return false;
  }
};

// --- checkShape -------------------------------------------------------------
// We only handle the simplest, most common shape at the vectorizer's input:
// an innermost loop whose body is ONE basic block (after loop rotation the
// header is also the latch). A single block means every instruction runs
// exactly once per iteration, which is what makes reordering easy to reason
// about: no instruction becomes conditional or unconditional.
bool LoopReorderer::checkShape() {
  if (!L.isInnermost())
    return skip("NotInnermost", "not an innermost loop");
  if (L.getNumBlocks() != 1)
    return skip("MultiBlock", "loop body has " + Twine(L.getNumBlocks()) +
                                  " basic blocks (only 1 supported)");
  if (!L.getLoopPreheader())
    return skip("NoPreheader", "loop has no preheader");

  BasicBlock *BB = L.getHeader();
  // getFirstNonPHIIt() skips the PHI nodes at the top of the block; PHIs
  // must stay first, so we never move them.
  for (auto It = BB->getFirstNonPHIIt(); It != BB->end(); ++It) {
    Instruction &I = *It;
    if (I.isTerminator())
      continue; // stays last
    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!LI->isSimple()) // volatile or atomic: order is observable
        return skip("Volatile", "volatile/atomic load in loop");
    } else if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (!SI->isSimple())
        return skip("Volatile", "volatile/atomic store in loop");
    } else if (I.mayReadOrWriteMemory() || I.mayHaveSideEffects()) {
      // Calls, fences, memcpy, etc.: we don't model their memory effects.
      return skip("UnsupportedInst",
                  "instruction with side effects: " +
                      Twine(I.getOpcodeName()));
    }
    Pos[&I] = Insts.size();
    Insts.push_back(&I);
  }
  if (Insts.size() > MaxBodySize)
    return skip("TooBig", "loop body has " + Twine(Insts.size()) +
                              " instructions (limit " + Twine(MaxBodySize) +
                              ")");
  return true;
}

void LoopReorderer::addEdge(unsigned From, unsigned To) {
  if (From == To)
    return;
  Succ[From].push_back(To);
  ++InDegree[To];
}

// --- collectDeps --------------------------------------------------------------
// Ask LAA for the dependences between memory accesses and translate them into
// graph edges. Two kinds matter:
//   * backward (later instruction in the body must run first) -> we add an
//     edge later->earlier: this is the reordering we WANT.
//   * forward  (already fine) -> edge earlier->later, so we never flip it.
// Anything LAA couldn't classify (Unknown, indirect A[B[i]], invariant
// address) is not something reordering can fix: give up on the loop.
bool LoopReorderer::collectDeps(const LoopAccessInfo &LAI) {
  const MemoryDepChecker &DC = LAI.getDepChecker();
  if (LAI.canVectorizeMemory()) {
    if (DC.isSafeForAnyVectorWidth())
      return skip("AlreadyLegal",
                  "LAA already considers memory accesses vectorizable");
    M = Mode::CappedVF;
    CapBitsBefore = DC.getMaxSafeVectorWidthInBits();
  }
  const SmallVectorImpl<MemoryDepChecker::Dependence> *Deps =
      DC.getDependences();
  if (!Deps)
    return skip("NoDepInfo", "LAA did not record dependences (too many)");

  for (const MemoryDepChecker::Dependence &D : *Deps) {
    using DT = MemoryDepChecker::Dependence;
    if (Debug)
      errs() << "[vecpass] dep " << DT::DepName[D.Type] << ":\n    "
             << *D.getSource(DC) << "\n -> " << *D.getDestination(DC) << "\n";
    if (D.Type == DT::NoDep)
      continue;
    Instruction *Src = D.getSource(DC), *Dst = D.getDestination(DC);
    auto SI = Pos.find(Src), DI = Pos.find(Dst);
    if (SI == Pos.end() || DI == Pos.end())
      return skip("DepOutsideBody", "dependence involves an instruction "
                                    "outside the loop body");
    unsigned Lo = std::min(SI->second, DI->second);
    unsigned Hi = std::max(SI->second, DI->second);
    if (isBackwardKind(D.Type)) {
      addEdge(Hi, Lo);
      ++NumBackward;
    } else if (isForwardKind(D.Type)) {
      addEdge(Lo, Hi);
    } else {
      return skip("UnfixableDep", Twine("LAA reports a dependence of kind '") +
                                      DT::DepName[D.Type] +
                                      "', which reordering cannot fix");
    }
  }
  return true;
}

// --- mayAliasInSameIteration ---------------------------------------------------
// Could A and B touch overlapping bytes *in the same iteration*?
// Both instructions are in the same basic block, so their pointer operands
// are SSA values from the same iteration; an alias query on them is exactly a
// same-iteration question.
bool LoopReorderer::mayAliasInSameIteration(Instruction *A, Instruction *B) {
  // 1) Ask alias analysis (BasicAA, TBAA, ScopedNoAlias for `restrict`, ...).
  if (AA.isNoAlias(MemoryLocation::get(A), MemoryLocation::get(B)))
    return false;

  // 2) Ask ScalarEvolution: if both addresses are "the same base + i*step"
  //    and differ by a known constant, compare that constant to the access
  //    sizes. Typical case: a[i] vs a[i+1] -> difference is 4 bytes, and a
  //    4-byte access can't overlap something 4 bytes away.
  const SCEV *PA = SE.getSCEV(getLoadStorePointerOperand(A));
  const SCEV *PB = SE.getSCEV(getLoadStorePointerOperand(B));
  const SCEV *Diff = SE.getMinusSCEV(PA, PB); // CouldNotCompute if bases differ
  if (const auto *C = dyn_cast<SCEVConstant>(Diff)) {
    int64_t D = C->getAPInt().getSExtValue(); // addr(A) - addr(B), in bytes
    TypeSize SA = DL.getTypeStoreSize(getLoadStoreType(A));
    TypeSize SB = DL.getTypeStoreSize(getLoadStoreType(B));
    if (!SA.isScalable() && !SB.isScalable()) {
      // A covers [D, D+SA), B covers [0, SB) relative to B's address.
      bool Disjoint = D >= (int64_t)SB.getFixedValue() ||
                      -D >= (int64_t)SA.getFixedValue();
      if (Disjoint)
        return false;
    }
  }
  return true; // can't prove otherwise: assume they may alias
}

// --- buildMemoryAndSSAEdges -------------------------------------------------------
// The edges that keep a single iteration correct.
void LoopReorderer::buildMemoryAndSSAEdges() {
  for (unsigned J = 0; J < Insts.size(); ++J) {
    Instruction *I = Insts[J];
    // (a) SSA: an operand defined in the body must stay before its user.
    for (Value *Op : I->operands())
      if (auto *OpI = dyn_cast<Instruction>(Op)) {
        auto It = Pos.find(OpI);
        if (It != Pos.end())
          addEdge(It->second, J);
      }
    // (b) Memory: keep the order of any earlier access that may hit the same
    //     bytes in this iteration, when at least one of the two is a store.
    //     (Two loads may be swapped freely.)
    if (!I->mayReadOrWriteMemory())
      continue;
    for (unsigned K = 0; K < J; ++K) {
      Instruction *Prev = Insts[K];
      if (!Prev->mayReadOrWriteMemory())
        continue;
      if (!isa<StoreInst>(I) && !isa<StoreInst>(Prev))
        continue;
      if (mayAliasInSameIteration(Prev, I))
        addEdge(K, J);
    }
  }
}

// --- topoSort -----------------------------------------------------------------
// Kahn's algorithm: repeatedly emit a node with no unsatisfied predecessors.
// Among the ready nodes we always pick the one that came first originally,
// so instructions only move when an edge forces them to (smallest change).
// If we can't emit every node, the edges form a cycle: e.g. a true
// recurrence like a[i] = a[i-1] + x, or a reordering blocked by possible
// aliasing. Then no valid order exists and we leave the loop alone.
bool LoopReorderer::topoSort(SmallVectorImpl<unsigned> &Order) {
  // A min-heap of indices: std::priority_queue is a max-heap by default, so
  // we pass std::greater to flip it.
  std::priority_queue<unsigned, std::vector<unsigned>, std::greater<unsigned>>
      Ready;
  std::vector<unsigned> Deg = InDegree; // copy; we consume it
  for (unsigned I = 0; I < Insts.size(); ++I)
    if (Deg[I] == 0)
      Ready.push(I);
  while (!Ready.empty()) {
    unsigned N = Ready.top();
    Ready.pop();
    Order.push_back(N);
    for (unsigned S : Succ[N])
      if (--Deg[S] == 0)
        Ready.push(S);
  }
  return Order.size() == Insts.size();
}

// Run LoopAccessAnalysis on L right now, from scratch.
// Why not use the cached LoopAccessInfoManager? Its clear() only evicts
// entries that have runtime checks, so after we move instructions it can
// hand back an analysis of the OLD order. A fresh LoopAccessInfo always
// reflects the current IR. unique_ptr = an owning pointer that deletes the
// object automatically when it goes out of scope (RAII), like a free() you
// can't forget.
std::unique_ptr<LoopAccessInfo> LoopReorderer::computeLAA() {
  return std::make_unique<LoopAccessInfo>(&L, &A.SE, &A.TTI, &A.TLI, &A.AA,
                                          &A.DT, &A.LI, &A.AC);
}

// Put the instructions in NewOrder, right before the terminator.
// Moving each one to "just before the terminator" in sequence produces
// exactly the sequence NewOrder.
void LoopReorderer::apply(ArrayRef<Instruction *> NewOrder) {
  Instruction *Term = L.getHeader()->getTerminator();
  for (Instruction *I : NewOrder)
    I->moveBefore(Term->getIterator());
}

// Is this LAA result good enough for LoopVectorize once LoopLoadElimination
// has run? Yes if every dependence is safe, or is one that only "prevents
// store-to-load forwarding" (LLE removes those by keeping the stored value in
// a register; see VecPass::run).
bool LoopReorderer::acceptableAfterLLE(const LoopAccessInfo &LAI,
                                       std::string &Why) {
  if (LAI.canVectorizeMemory())
    return true;
  const auto *Deps = LAI.getDepChecker().getDependences();
  if (!Deps) {
    Why = "LAA recorded no dependences";
    return false;
  }
  bool SawPreventsForwarding = false;
  for (const auto &D : *Deps) {
    using DT = MemoryDepChecker::Dependence;
    if (Debug)
      errs() << "[vecpass] check: dep " << DT::DepName[D.Type] << ":\n    "
             << *D.getSource(LAI.getDepChecker()) << "\n -> "
             << *D.getDestination(LAI.getDepChecker()) << "\n";
    if (D.Type == DT::ForwardButPreventsForwarding ||
        D.Type == DT::BackwardVectorizableButPreventsForwarding) {
      SawPreventsForwarding = true;
      continue;
    }
    if (DT::isSafeForVectorization(D.Type) !=
        MemoryDepChecker::VectorizationSafetyStatus::Safe) {
      Why = std::string("a '") + DT::DepName[D.Type] +
            "' dependence remains";
      return false;
    }
  }
  if (!SawPreventsForwarding) {
    Why = "LAA rejects the loop for a reason other than dependences";
    return false;
  }
  if (!RunLLE) {
    Why = "only store-to-load-forwarding dependences remain, but "
          "-vecpass-run-lle=false";
    return false;
  }
  return true;
}

bool LoopReorderer::run() {
  if (!checkShape())
    return false;

  Succ.assign(Insts.size(), {});
  InDegree.assign(Insts.size(), 0);

  std::unique_ptr<LoopAccessInfo> Before = computeLAA();
  if (!collectDeps(*Before))
    return false;

  if (M == Mode::CappedVF && NumBackward == 0)
    return skip("AlreadyLegal", "vector width is capped, but not by a "
                                "backward dependence");

  // No backward dependence: reordering has nothing to do. But if the only
  // problem is forward dependences that "prevent store-to-load forwarding"
  // (e.g. b[i] = ...; ... = b[i-1]; written in that order by the programmer),
  // LoopLoadElimination alone makes the loop vectorizable. Ask for it.
  if (NumBackward == 0) {
    std::string Why;
    if (!acceptableAfterLLE(*Before, Why))
      return skip("NoBackwardDep",
                  "not a backward-dependence problem (" + Why +
                      "; see -Rpass-analysis=loop-vectorize)");
    ORE.emit([&]() {
      return OptimizationRemark(VP_NAME, "ForwardingOnly", L.getStartLoc(),
                                L.getHeader())
             << "no reordering needed; only store-to-load-forwarding "
                "dependences block vectorization, scheduling "
                "LoopLoadElimination";
    });
    return true;
  }
  buildMemoryAndSSAEdges();

  SmallVector<unsigned, 64> Order;
  if (!topoSort(Order))
    return skip("Cycle", "no legal order makes all dependences forward "
                         "(true recurrence, or accesses that may alias)");

  SmallVector<Instruction *, 64> NewOrder, OldOrder(Insts.begin(), Insts.end());
  unsigned Moved = 0;
  for (unsigned K = 0; K < Order.size(); ++K) {
    NewOrder.push_back(Insts[Order[K]]);
    Moved += Order[K] != K;
  }
  apply(NewOrder);
  if (Debug)
    errs() << "[vecpass] body after reordering:\n" << *L.getHeader() << "\n";

  // Re-run LAA from scratch on the new order (see computeLAA for why fresh).
  std::string Why;
  std::unique_ptr<LoopAccessInfo> After = computeLAA();
  bool OK;
  if (M == Mode::CappedVF) {
    // Must stay legal AND allow wider vectors than before.
    const MemoryDepChecker &DC = After->getDepChecker();
    OK = After->canVectorizeMemory() &&
         (DC.isSafeForAnyVectorWidth() ||
          DC.getMaxSafeVectorWidthInBits() > CapBitsBefore);
    if (!OK)
      Why = "the vector-width cap did not increase";
  } else {
    OK = acceptableAfterLLE(*After, Why);
  }
  if (!OK) {
    apply(OldOrder); // undo: put everything back exactly as it was
    return skip("Reverted", "reordering did not make the loop legal (" + Why +
                         "); original order restored");
  }

  ORE.emit([&]() {
    OptimizationRemark R(VP_NAME, "Reordered", L.getStartLoc(), L.getHeader());
    R << "reordered loop body (" << ore::NV("Moved", Moved) << " of "
           << ore::NV("Insts", (unsigned)Insts.size())
           << " instructions moved) so that "
           << ore::NV("Backward", NumBackward)
           << " backward dependence(s) become forward";
    if (M == Mode::CappedVF) {
      const MemoryDepChecker &DC = After->getDepChecker();
      R << "; max safe vector width was " << ore::NV("CapBits", CapBitsBefore)
        << " bits, now ";
      if (DC.isSafeForAnyVectorWidth())
        R << "unlimited";
      else
        R << ore::NV("NewCapBits", DC.getMaxSafeVectorWidthInBits()) << " bits";
    }
    return R;
  });
  return true;
}

} // anonymous namespace

// ---------------------------------------------------------------------------
// 3. The pass entry point.
// ---------------------------------------------------------------------------
namespace vecpass {

PreservedAnalyses VecPass::run(Function &F, FunctionAnalysisManager &FAM) {
  // `auto &` = "a reference whose type the compiler works out", here
  // e.g. LoopInfo &. The analysis manager owns the objects.
  Analyses A{FAM.getResult<LoopAnalysis>(F),
             FAM.getResult<DominatorTreeAnalysis>(F),
             FAM.getResult<ScalarEvolutionAnalysis>(F),
             FAM.getResult<AAManager>(F),
             FAM.getResult<TargetIRAnalysis>(F),
             FAM.getResult<TargetLibraryAnalysis>(F),
             FAM.getResult<AssumptionAnalysis>(F),
             FAM.getResult<OptimizationRemarkEmitterAnalysis>(F)};

  bool Changed = false;
  // getLoopsInPreorder lists every loop (outer and inner); non-innermost
  // ones get a "not innermost" remark from checkShape.
  for (Loop *L : A.LI.getLoopsInPreorder()) {
    LoopReorderer R(*L, A);
    Changed |= R.run();
  }
  if (!Changed)
    return PreservedAnalyses::all();

  // We moved instructions but never touched the CFG: loops, dominator tree,
  // etc. are still valid. Instruction-order-sensitive results (LAA,
  // MemorySSA) are not preserved, so they are dropped here.
  PreservedAnalyses PA;
  PA.preserveSet<CFGAnalyses>();
  PA.preserve<ScalarEvolutionAnalysis>();
  if (!RunLLE)
    return PA;

  // After reordering, a typical loop has a forward dependence like
  //   store b[i]  ...  load b[i-1]
  // which LAA flags as "prevents store-to-load forwarding". LLVM's
  // LoopLoadElimination replaces such a load with the value stored by the
  // previous iteration (a PHI), and LoopVectorize then handles it as a
  // first-order recurrence. In the default -O3 pipeline LLE runs only AFTER
  // the vectorizer, so we run it here — only on functions we changed.
  FAM.invalidate(F, PA);
  PreservedAnalyses LLEPA = LoopLoadEliminationPass().run(F, FAM);
  PA.intersect(std::move(LLEPA));

  // LLE redirects the load's users to the new PHI but leaves the load
  // itself in place (normally a later cleanup pass deletes it). A dead load
  // still counts as a memory access for LoopVectorize's LAA, so the
  // "prevents forwarding" dependence would remain. Delete dead instructions.
  FAM.invalidate(F, PA);
  PA.intersect(DCEPass().run(F, FAM));
  return PA;
}

} // namespace vecpass
