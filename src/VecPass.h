// VecPass.h — declares `vecpass`, the transform pass.
//
// What it does, in one sentence: for an innermost, single-block loop that
// LoopVectorize would reject because of a *lexically backward* memory
// dependence, it reorders the instructions of the loop body (within one
// iteration) so that every loop-carried dependence becomes lexically forward,
// which LoopAccessAnalysis (LAA) accepts.
//
// Example (TSVC s212):
//     a[i] *= c[i];            // store a[i]
//     b[i] += a[i+1] * d[i];   // load a[i+1]  <- reads a value the NEXT
//                              //    iteration's store overwrites
// The load of a[i+1] can never touch the same address as the store to a[i]
// in the same iteration, so it may legally move above that store. Once it
// does, the dependence is "forward" and the loop vectorizes.
//
// See docs/DESIGN.md for the safety argument.
#ifndef VECPASS_VECPASS_H
#define VECPASS_VECPASS_H

#include "llvm/IR/PassManager.h"

namespace vecpass {

struct VecPass : llvm::PassInfoMixin<VecPass> {
  llvm::PreservedAnalyses run(llvm::Function &F,
                              llvm::FunctionAnalysisManager &FAM);
};

} // namespace vecpass

#endif
