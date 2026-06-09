#include "RTEffect/Passes.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

PassPluginLibraryInfo getRTEffectPluginInfo() {
  return {LLVM_PLUGIN_API_VERSION, "RTEffect", LLVM_VERSION_STRING,
          [](PassBuilder &PB) {
            PB.registerPipelineParsingCallback(
                [](StringRef Name, ModulePassManager &MPM,
                   ArrayRef<PassBuilder::PipelineElement>) {
                  if (Name == "rt-effect-infer") {
                    MPM.addPass(RTEffectInferPass());
                    return true;
                  }
                  if (Name == "rt-constraint-check") {
                    MPM.addPass(RTConstraintCheckPass());
                    return true;
                  }
                  if (Name == "rt-san-place") {
                    MPM.addPass(RTSanPlacementPass());
                    return true;
                  }
                  if (Name == "rt-san-place-all") {
                    auto P = RTSanPlacementPass();
                    P.InstrumentAll = true;
                    MPM.addPass(std::move(P));
                    return true;
                  }
                  return false;
                });
          }};
}

extern "C" LLVM_ATTRIBUTE_WEAK PassPluginLibraryInfo llvmGetPassPluginInfo() {
  return getRTEffectPluginInfo();
}
