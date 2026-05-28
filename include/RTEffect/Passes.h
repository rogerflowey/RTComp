#ifndef RTEFFECT_PASSES_H
#define RTEFFECT_PASSES_H

#include "llvm/IR/PassManager.h"
#include <string>

struct RTConfig {
  std::string ExternalFuncFile;
  bool InstrumentAll = false;
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
  llvm::PreservedAnalyses run(llvm::Module &M,
                              llvm::ModuleAnalysisManager &AM);
};

#endif
