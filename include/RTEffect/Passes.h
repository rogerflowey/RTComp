#ifndef RTEFFECT_PASSES_H
#define RTEFFECT_PASSES_H

#include "llvm/IR/PassManager.h"
#include <string>

struct RTConfig {
  std::string ExternalFuncFile;
  bool InstrumentAll = false;
  // per-call-site instrumentation. When false, instrument the whole
  // function (legacy behaviour); when true, instrument only the witness
  // call sites carried by the inferred provenance chains.
  bool PerCallSite = true;
};

extern RTConfig GlobalRTConfig;

struct RTEffectInferPass : public llvm::PassInfoMixin<RTEffectInferPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
};

struct RTConstraintCheckPass
    : public llvm::PassInfoMixin<RTConstraintCheckPass> {
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
};

struct RTSanPlacementPass : public llvm::PassInfoMixin<RTSanPlacementPass> {
  bool InstrumentAll = false;
  // Mirrors RTConfig::PerCallSite. Defaults to per-call-site for the
  // selective pipeline.
  bool PerCallSite = true;
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
};

#endif
