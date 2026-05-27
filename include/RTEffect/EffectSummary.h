#ifndef RTEFFECT_EFFECT_SUMMARY_H
#define RTEFFECT_EFFECT_SUMMARY_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Metadata.h"
#include <string>

struct FunctionEffectSummary {
  bool MayBlock = false;
  bool MayAlloc = false;

  enum Status {
    ProvenSafe = 0,
    Violating = 1,
    Unknown = 2,
  };
  Status status = Unknown;

  std::string ReasonBlockFn;
  std::string ReasonAllocFn;

  void merge(const FunctionEffectSummary &Other) {
    if (Other.MayBlock && !MayBlock) {
      MayBlock = true;
      ReasonBlockFn = Other.ReasonBlockFn;
    }
    if (Other.MayAlloc && !MayAlloc) {
      MayAlloc = true;
      ReasonAllocFn = Other.ReasonAllocFn;
    }
  }

  static void writeToMetadata(llvm::Function &F,
                              const FunctionEffectSummary &S);
  static FunctionEffectSummary readFromMetadata(llvm::Function &F);

  static const llvm::StringLiteral MetadataName;
};

#endif
