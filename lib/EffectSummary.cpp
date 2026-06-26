#include "RTEffect/EffectSummary.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
#include <sstream>

using namespace llvm;

const StringLiteral FunctionEffectSummary::MetadataName = "rt.effect";

namespace {

// Format token. Bump when on-disk layout changes incompatibly.
const char *MetadataVersionTag = "rt.effect.v3";

std::string escapeField(StringRef S) {
  std::string Out;
  for (char C : S) {
    switch (C) {
    case '\\':
      Out += "\\\\";
      break;
    case '|':
      Out += "\\p";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\r':
      Out += "\\r";
      break;
    default:
      Out += C;
      break;
    }
  }
  return Out;
}

std::string unescapeField(StringRef S) {
  std::string Out;
  for (size_t I = 0; I < S.size(); ++I) {
    if (S[I] != '\\' || I + 1 >= S.size()) {
      Out += S[I];
      continue;
    }
    char N = S[++I];
    switch (N) {
    case '\\':
      Out += '\\';
      break;
    case 'p':
      Out += '|';
      break;
    case 'n':
      Out += '\n';
      break;
    case 'r':
      Out += '\r';
      break;
    default:
      Out += N;
      break;
    }
  }
  return Out;
}

std::string encodeChain(const std::vector<RTProvenanceFrame> &Chain) {
  std::string Out;
  for (const RTProvenanceFrame &Frame : Chain) {
    if (!Out.empty())
      Out += "\n";
    Out += escapeField(Frame.FunctionName);
    Out += "|";
    Out += escapeField(Frame.CalleeName);
    Out += "|";
    Out += escapeField(Frame.Kind);
    Out += "|";
    Out += escapeField(Frame.File);
    Out += "|";
    Out += std::to_string(Frame.Line);
    Out += "|";
    Out += std::to_string(Frame.Column);
    Out += "|";
    Out += escapeField(Frame.InstructionText);
  }
  return Out;
}

std::vector<RTProvenanceFrame> decodeChain(StringRef Encoded) {
  std::vector<RTProvenanceFrame> Chain;
  SmallVector<StringRef, 8> Lines;
  Encoded.split(Lines, '\n');
  for (StringRef Line : Lines) {
    if (Line.empty())
      continue;
    SmallVector<StringRef, 8> Fields;
    Line.split(Fields, '|');
    if (Fields.size() < 7)
      continue;
    RTProvenanceFrame Frame;
    Frame.FunctionName = unescapeField(Fields[0]);
    Frame.CalleeName = unescapeField(Fields[1]);
    Frame.Kind = unescapeField(Fields[2]);
    Frame.File = unescapeField(Fields[3]);
    Fields[4].getAsInteger(10, Frame.Line);
    Fields[5].getAsInteger(10, Frame.Column);
    Frame.InstructionText = unescapeField(Fields[6]);
    Chain.push_back(std::move(Frame));
  }
  return Chain;
}

std::string encodePolyArgs(const std::vector<unsigned> &Args) {
  std::string Out;
  for (unsigned A : Args) {
    if (!Out.empty())
      Out += ",";
    Out += std::to_string(A);
  }
  return Out;
}

std::vector<unsigned> decodePolyArgs(StringRef Encoded) {
  std::vector<unsigned> Out;
  SmallVector<StringRef, 4> Parts;
  Encoded.split(Parts, ',');
  for (StringRef P : Parts) {
    if (P.empty())
      continue;
    unsigned V = 0;
    if (!P.getAsInteger(10, V))
      Out.push_back(V);
  }
  return Out;
}

std::string instructionToString(Instruction &I) {
  std::string Text;
  raw_string_ostream OS(Text);
  I.print(OS);
  OS.flush();
  return Text;
}

} // namespace

bool *FunctionEffectSummary::flagPtr(int K) {
  switch (K) {
  case RTE_Block: return &MayBlock;
  case RTE_Alloc: return &MayAlloc;
  case RTE_Throw: return &MayThrow;
  case RTE_Lock: return &MayLock;
  case RTE_Recurse: return &MayRecurse;
  case RTE_SignalUnsafe: return &MaySignalUnsafe;
  case RTE_Unknown: return &HasUnknown;
  }
  return nullptr;
}

const bool *FunctionEffectSummary::flagPtr(int K) const {
  return const_cast<FunctionEffectSummary *>(this)->flagPtr(K);
}

std::string *FunctionEffectSummary::reasonPtr(int K) {
  switch (K) {
  case RTE_Block: return &ReasonBlockFn;
  case RTE_Alloc: return &ReasonAllocFn;
  case RTE_Throw: return &ReasonThrowFn;
  case RTE_Lock: return &ReasonLockFn;
  case RTE_Recurse: return &ReasonRecurseFn;
  case RTE_SignalUnsafe: return &ReasonSignalUnsafeFn;
  case RTE_Unknown: return &ReasonUnknown;
  }
  return nullptr;
}

const std::string *FunctionEffectSummary::reasonPtr(int K) const {
  return const_cast<FunctionEffectSummary *>(this)->reasonPtr(K);
}

std::vector<RTProvenanceFrame> *FunctionEffectSummary::chainPtr(int K) {
  switch (K) {
  case RTE_Block: return &BlockChain;
  case RTE_Alloc: return &AllocChain;
  case RTE_Throw: return &ThrowChain;
  case RTE_Lock: return &LockChain;
  case RTE_Recurse: return &RecurseChain;
  case RTE_SignalUnsafe: return &SignalUnsafeChain;
  case RTE_Unknown: return &UnknownChain;
  }
  return nullptr;
}

const std::vector<RTProvenanceFrame> *
FunctionEffectSummary::chainPtr(int K) const {
  return const_cast<FunctionEffectSummary *>(this)->chainPtr(K);
}

const char *FunctionEffectSummary::kindName(int K) {
  switch (K) {
  case RTE_Block: return "may_block";
  case RTE_Alloc: return "may_alloc";
  case RTE_Throw: return "may_throw";
  case RTE_Lock: return "may_lock";
  case RTE_Recurse: return "may_recurse";
  case RTE_SignalUnsafe: return "may_signal_unsafe";
  case RTE_Unknown: return "unknown";
  }
  return "?";
}

const char *FunctionEffectSummary::heapKindName(int HK) {
  switch (HK) {
  case HK_None:       return "none";
  case HK_Stack:      return "stack";
  case HK_Global:     return "global";
  case HK_RTHeap:     return "rtheap";
  case HK_NormalHeap: return "normal_heap";
  }
  return "?";
}

void FunctionEffectSummary::merge(const FunctionEffectSummary &Other) {
  for (int K = 0; K < RTE_Count; ++K) {
    if (*Other.flagPtr(K) && !*flagPtr(K)) {
      *flagPtr(K) = true;
      *reasonPtr(K) = *Other.reasonPtr(K);
      *chainPtr(K) = *Other.chainPtr(K);
    }
  }
  if (Other.MaxStackDepth == -1)
    MaxStackDepth = -1;
  else if (MaxStackDepth != -1 && Other.MaxStackDepth > MaxStackDepth)
    MaxStackDepth = Other.MaxStackDepth;
  if (Other.MayAllocHeapKind > MayAllocHeapKind) {
    MayAllocHeapKind = Other.MayAllocHeapKind;
    AllocHeapChain = Other.AllocHeapChain;
  }
}

bool FunctionEffectSummary::hasAnyEffect() const {
  for (int K = 0; K < RTE_Count; ++K)
    if (*flagPtr(K))
      return true;
  return false;
}

RTProvenanceFrame FunctionEffectSummary::makeFrame(Function &F, Instruction &I,
                                                   StringRef CalleeName,
                                                   StringRef Kind) {
  RTProvenanceFrame Frame;
  Frame.FunctionName = F.getName().str();
  Frame.CalleeName = CalleeName.str();
  Frame.Kind = Kind.str();
  Frame.InstructionText = instructionToString(I);

  if (DebugLoc DL = I.getDebugLoc()) {
    Frame.Line = DL.getLine();
    Frame.Column = DL.getCol();
    if (auto *Scope = dyn_cast_or_null<DIScope>(DL.getScope()))
      Frame.File = Scope->getFilename().str();
  } else if (DISubprogram *SP = F.getSubprogram()) {
    Frame.File = SP->getFilename().str();
    Frame.Line = SP->getLine();
  }

  return Frame;
}

void FunctionEffectSummary::writeToMetadata(Function &F,
                                            const FunctionEffectSummary &S) {
  LLVMContext &Ctx = F.getContext();
  SmallVector<Metadata *, 32> Ops;

  // Header: version tag, status, stack depth, polymorphism flag, poly arg list,
  // heap-kind, heap-chain.
  Ops.push_back(MDString::get(Ctx, MetadataVersionTag));
  Ops.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), static_cast<int>(S.status))));
  Ops.push_back(ConstantAsMetadata::get(
      ConstantInt::getSigned(Type::getInt32Ty(Ctx), S.MaxStackDepth)));
  Ops.push_back(ConstantAsMetadata::get(
      ConstantInt::getBool(Ctx, S.IsEffectPolymorphic)));
  Ops.push_back(MDString::get(Ctx, encodePolyArgs(S.PolyArgs)));
  Ops.push_back(ConstantAsMetadata::get(
      ConstantInt::get(Type::getInt32Ty(Ctx), static_cast<int>(S.MayAllocHeapKind))));
  Ops.push_back(MDString::get(Ctx, encodeChain(S.AllocHeapChain)));

  // Per-effect: flag, reason, chain.
  for (int K = 0; K < RTE_Count; ++K) {
    Ops.push_back(
        ConstantAsMetadata::get(ConstantInt::getBool(Ctx, *S.flagPtr(K))));
    Ops.push_back(MDString::get(Ctx, *S.reasonPtr(K)));
    Ops.push_back(MDString::get(Ctx, encodeChain(*S.chainPtr(K))));
  }

  MDNode *N = MDNode::get(Ctx, Ops);
  F.setMetadata(MetadataName, N);
}

FunctionEffectSummary FunctionEffectSummary::readFromMetadata(Function &F) {
  FunctionEffectSummary S;
  MDNode *N = F.getMetadata(MetadataName);
  if (!N || N->getNumOperands() < 5)
    return S;

  auto *Tag = dyn_cast<MDString>(N->getOperand(0));
  if (!Tag)
    return S;

  StringRef TagStr = Tag->getString();
  bool IsV2 = (TagStr == "rt.effect.v2");
  if (TagStr != MetadataVersionTag && !IsV2)
    return S; // unsupported / older format -> defaults

  if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(1)))
    S.status =
        static_cast<FunctionEffectSummary::Status>(M->getZExtValue());
  if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(2)))
    S.MaxStackDepth = static_cast<int>(M->getSExtValue());
  if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(3)))
    S.IsEffectPolymorphic = M->getZExtValue();
  if (auto *M = dyn_cast<MDString>(N->getOperand(4)))
    S.PolyArgs = decodePolyArgs(M->getString());

  unsigned Idx = 5;

  if (!IsV2 && Idx + 1 < N->getNumOperands()) {
    if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(Idx)))
      S.MayAllocHeapKind = static_cast<HeapKind>(M->getZExtValue());
    if (auto *M = dyn_cast<MDString>(N->getOperand(Idx + 1)))
      S.AllocHeapChain = decodeChain(M->getString());
    Idx += 2;
  }

  for (int K = 0; K < RTE_Count; ++K) {
    if (Idx + 2 >= N->getNumOperands())
      break;
    if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(Idx)))
      *S.flagPtr(K) = M->getZExtValue();
    if (auto *M = dyn_cast<MDString>(N->getOperand(Idx + 1)))
      *S.reasonPtr(K) = M->getString().str();
    if (auto *M = dyn_cast<MDString>(N->getOperand(Idx + 2)))
      *S.chainPtr(K) = decodeChain(M->getString());
    Idx += 3;
  }

  return S;
}
