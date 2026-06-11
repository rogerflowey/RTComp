#ifndef RTEFFECT_EFFECT_SUMMARY_H
#define RTEFFECT_EFFECT_SUMMARY_H

#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Metadata.h"
#include <string>
#include <vector>

struct RTProvenanceFrame {
  std::string FunctionName;
  std::string CalleeName;
  std::string Kind;
  std::string File;
  unsigned Line = 0;
  unsigned Column = 0;
  std::string InstructionText;
};

struct FunctionEffectSummary {
  bool MayBlock = false;
  bool MayAlloc = false;
  bool HasUnknown = false;

  enum Status {
    ProvenSafe = 0,
    Violating = 1,
    Unknown = 2,
  };
  Status status = Unknown;

  std::string ReasonBlockFn;
  std::string ReasonAllocFn;
  std::string ReasonUnknown;

  std::vector<RTProvenanceFrame> BlockChain;
  std::vector<RTProvenanceFrame> AllocChain;
  std::vector<RTProvenanceFrame> UnknownChain;

  void merge(const FunctionEffectSummary &Other) {
    if (Other.MayBlock && !MayBlock) {
      MayBlock = true;
      ReasonBlockFn = Other.ReasonBlockFn;
      BlockChain = Other.BlockChain;
    }
    if (Other.MayAlloc && !MayAlloc) {
      MayAlloc = true;
      ReasonAllocFn = Other.ReasonAllocFn;
      AllocChain = Other.AllocChain;
    }
    if (Other.HasUnknown && !HasUnknown) {
      HasUnknown = true;
      ReasonUnknown = Other.ReasonUnknown;
      UnknownChain = Other.UnknownChain;
    }
  }

  bool hasAnyEffect() const { return MayBlock || MayAlloc || HasUnknown; }

  static RTProvenanceFrame makeFrame(llvm::Function &F, llvm::Instruction &I,
                                     llvm::StringRef CalleeName,
                                     llvm::StringRef Kind);

  static void writeToMetadata(llvm::Function &F,
                              const FunctionEffectSummary &S);
  static FunctionEffectSummary readFromMetadata(llvm::Function &F);

  static const llvm::StringLiteral MetadataName;
};

#endif
