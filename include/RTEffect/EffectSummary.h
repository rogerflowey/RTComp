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

// Effect-kind enumeration. Order is part of the serialized format.
enum RTEffectKind {
  RTE_Block = 0,
  RTE_Alloc,
  RTE_Throw,
  RTE_Lock,
  RTE_Recurse,
  RTE_SignalUnsafe,
  RTE_Unknown,
  RTE_Count,
};

struct FunctionEffectSummary {
  bool MayBlock = false;
  bool MayAlloc = false;
  bool MayThrow = false;
  bool MayLock = false;
  bool MayRecurse = false;
  bool MaySignalUnsafe = false;
  bool HasUnknown = false;

  // Bounded stack-depth (number of frames including this one). -1 is unbounded.
  int MaxStackDepth = 1;

  // Effect-polymorphism: this function forwards effects through one or more
  // function-typed parameters. The vector lists the parameter indices.
  bool IsEffectPolymorphic = false;
  std::vector<unsigned> PolyArgs;

  enum Status {
    ProvenSafe = 0,
    Violating = 1,
    Unknown = 2,
  };
  Status status = Unknown;

  std::string ReasonBlockFn;
  std::string ReasonAllocFn;
  std::string ReasonThrowFn;
  std::string ReasonLockFn;
  std::string ReasonRecurseFn;
  std::string ReasonSignalUnsafeFn;
  std::string ReasonUnknown;

  std::vector<RTProvenanceFrame> BlockChain;
  std::vector<RTProvenanceFrame> AllocChain;
  std::vector<RTProvenanceFrame> ThrowChain;
  std::vector<RTProvenanceFrame> LockChain;
  std::vector<RTProvenanceFrame> RecurseChain;
  std::vector<RTProvenanceFrame> SignalUnsafeChain;
  std::vector<RTProvenanceFrame> UnknownChain;

  // Indexed accessors so loops over effect kinds stay short.
  bool *flagPtr(int K);
  const bool *flagPtr(int K) const;
  std::string *reasonPtr(int K);
  const std::string *reasonPtr(int K) const;
  std::vector<RTProvenanceFrame> *chainPtr(int K);
  const std::vector<RTProvenanceFrame> *chainPtr(int K) const;
  static const char *kindName(int K);

  void merge(const FunctionEffectSummary &Other);
  bool hasAnyEffect() const;

  static RTProvenanceFrame makeFrame(llvm::Function &F, llvm::Instruction &I,
                                     llvm::StringRef CalleeName,
                                     llvm::StringRef Kind);

  static void writeToMetadata(llvm::Function &F,
                              const FunctionEffectSummary &S);
  static FunctionEffectSummary readFromMetadata(llvm::Function &F);

  static const llvm::StringLiteral MetadataName;
};

#endif
