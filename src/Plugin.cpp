// Plugin.cpp — the plugin's entry point: tells `opt` (or clang) which pass
// names this .so provides, and where vecpass goes in the -O2/-O3 pipeline.
//
// When opt runs with -load-pass-plugin=libVecPass.so it dlopen()s the file,
// looks up the C symbol `llvmGetPassPluginInfo`, and calls our registration
// callback with its PassBuilder. clang does the same for -fpass-plugin=.
#include "LoopPrinter.h"
#include "VecPass.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;

// Default true: loading the plugin into an -O2/-O3 pipeline turns vecpass on.
// Set -vecpass-auto=false to load the plugin but only use explicit pass names.
static cl::opt<bool> AutoInsert(
    "vecpass-auto", cl::init(true),
    cl::desc("Insert vecpass at the VectorizerStart extension point of the "
             "default O2/O3 pipelines"));

// Called once by opt/clang after loading the plugin.
static void registerCallbacks(PassBuilder &PB) {
  // (1) Pipeline-parsing callback: opt calls this for every pass name in
  // -passes='...' that it doesn't recognize. Return true = "I handled it".
  // The lambda [] captures nothing; it is just an inline function.
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "vecpass-print") {
          FPM.addPass(vecpass::LoopPrinterPass());
          return true;
        }
        if (Name == "vecpass") {
          FPM.addPass(vecpass::VecPass());
          return true;
        }
        return false;
      });

  // (2) Extension point: VectorizerStart is the spot in the -O2/-O3 function
  // pipeline just before LoopVectorize. Loops there are already simplified,
  // rotated and LICM'd (so the body is usually a single block), and nothing
  // runs between us and the vectorizer that could undo our reordering.
  PB.registerVectorizerStartEPCallback(
      [](FunctionPassManager &FPM, OptimizationLevel Level) {
        if (AutoInsert)
          FPM.addPass(vecpass::VecPass());
      });
}

// extern "C" turns off C++ name mangling so opt can find the symbol by its
// plain name. LLVM_ATTRIBUTE_WEAK lets the same code also be linked statically
// into a tool without a duplicate-symbol error.
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "VecPass", "0.1", registerCallbacks};
}
