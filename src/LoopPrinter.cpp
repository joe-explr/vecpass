// LoopPrinter.cpp — implementation of `vecpass-print` (see LoopPrinter.h).
//
// Output goes to stderr (llvm::errs()) so it doesn't mix with IR on stdout.
// Example line:
//   [vecpass-print] function 'foo': loop depth=1 header=for.body tripcount=1024
#include "LoopPrinter.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace vecpass {

// Print one loop, then recurse into the loops nested inside it.
// `L` is a reference-to-const: we read the loop but never modify it.
static void printLoop(const Loop &L, ScalarEvolution &SE, StringRef FnName) {
  raw_ostream &OS = errs();
  OS << "[vecpass-print] function '" << FnName << "': loop depth="
     << L.getLoopDepth() << " header=";
  // Blocks without a name (e.g. after -O3 strips names) print as "<unnamed>".
  BasicBlock *H = L.getHeader();
  if (H->hasName())
    OS << H->getName();
  else
    OS << "<unnamed>";

  // getSmallConstantTripCount returns 0 when the trip count is not a small
  // compile-time constant. SCEV can still often describe it symbolically
  // (e.g. "(zext i32 %n to i64)"), which is just as useful for a human.
  // Note: SCEV needs the loop in "loop-simplify" form to say much; clang -O0
  // output usually isn't, so run with -passes='loop-simplify,vecpass-print'
  // or on -O1+ IR.
  // `const_cast` because SCEV's API takes a non-const Loop*; it does not
  // modify the loop.
  Loop *LP = const_cast<Loop *>(&L);
  if (unsigned TC = SE.getSmallConstantTripCount(LP)) {
    OS << " tripcount=" << TC;
  } else {
    const SCEV *BTC = SE.getBackedgeTakenCount(LP);
    if (isa<SCEVCouldNotCompute>(BTC))
      OS << " tripcount=unknown";
    else
      OS << " tripcount=(" << *BTC << ") + 1";
  }
  OS << "\n";

  // A Loop owns its child loops; range-for walks them like a C array.
  for (const Loop *Sub : L.getSubLoops())
    printLoop(*Sub, SE, FnName);
}

PreservedAnalyses LoopPrinterPass::run(Function &F,
                                       FunctionAnalysisManager &FAM) {
  // getResult<> computes the analysis (or returns a cached copy). We get a
  // reference back; the analysis manager owns the object.
  LoopInfo &LI = FAM.getResult<LoopAnalysis>(F);
  ScalarEvolution &SE = FAM.getResult<ScalarEvolutionAnalysis>(F);

  if (LI.empty())
    errs() << "[vecpass-print] function '" << F.getName() << "': no loops\n";
  // Iterating LoopInfo yields only top-level loops; printLoop recurses.
  for (const Loop *L : LI)
    printLoop(*L, SE, F.getName());

  // We changed nothing, so every analysis stays valid.
  return PreservedAnalyses::all();
}

} // namespace vecpass
