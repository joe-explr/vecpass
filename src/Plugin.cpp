// Plugin.cpp — the plugin's entry point: tells `opt` (or clang) which pass
// names this .so provides.
//
// When opt runs with -load-pass-plugin=libVecPass.so it dlopen()s the file,
// looks up the C symbol `llvmGetPassPluginInfo`, and calls our registration
// callback with its PassBuilder. We hook into the PassBuilder so that the
// names in -passes='...' map to our pass classes.
#include "LoopPrinter.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Plugins/PassPlugin.h"

using namespace llvm;

// Called once by opt/clang after loading the plugin.
static void registerCallbacks(PassBuilder &PB) {
  // Pipeline-parsing callback: opt calls this for every pass name in
  // -passes='...' that it doesn't recognize. Return true = "I handled it".
  // The lambda [] captures nothing; it is just an inline function.
  PB.registerPipelineParsingCallback(
      [](StringRef Name, FunctionPassManager &FPM,
         ArrayRef<PassBuilder::PipelineElement>) {
        if (Name == "vecpass-print") {
          FPM.addPass(vecpass::LoopPrinterPass());
          return true;
        }
        return false;
      });
}

// extern "C" turns off C++ name mangling so opt can find the symbol by its
// plain name. LLVM_ATTRIBUTE_WEAK lets the same code also be linked statically
// into a tool without a duplicate-symbol error.
extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "VecPass", "0.1", registerCallbacks};
}
