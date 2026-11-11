// LoopPrinter.h — declares `vecpass-print`, a read-only analysis-printing pass.
//
// For every function it lists the loops LoopInfo found, with nesting depth,
// header block name, and the trip count when ScalarEvolution can compute it.
// It changes nothing; it is the "hello world" that proves the plugin loads and
// that we can query LLVM's analyses.
#ifndef VECPASS_LOOPPRINTER_H
#define VECPASS_LOOPPRINTER_H

#include "llvm/IR/PassManager.h"

namespace vecpass {

// PassInfoMixin<T> is a CRTP helper: it gives our pass a name() and other
// boilerplate the new pass manager expects. We only supply run().
struct LoopPrinterPass : llvm::PassInfoMixin<LoopPrinterPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
  // Run even on functions marked `optnone` (e.g. clang -O0 output).
  static bool isRequired() { return true; }
};

} // namespace vecpass

#endif
