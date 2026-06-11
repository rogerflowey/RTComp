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

std::string instructionToString(Instruction &I) {
  std::string Text;
  raw_string_ostream OS(Text);
  I.print(OS);
  OS.flush();
  return Text;
}

} // namespace

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
  Metadata *Ops[] = {
      ConstantAsMetadata::get(ConstantInt::getBool(Ctx, S.MayBlock)),
      ConstantAsMetadata::get(ConstantInt::getBool(Ctx, S.MayAlloc)),
      ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(Ctx), static_cast<int>(S.status))),
      MDString::get(Ctx, S.ReasonBlockFn),
      MDString::get(Ctx, S.ReasonAllocFn),
      ConstantAsMetadata::get(ConstantInt::getBool(Ctx, S.HasUnknown)),
      MDString::get(Ctx, S.ReasonUnknown),
      MDString::get(Ctx, encodeChain(S.BlockChain)),
      MDString::get(Ctx, encodeChain(S.AllocChain)),
      MDString::get(Ctx, encodeChain(S.UnknownChain)),
  };
  MDNode *N = MDNode::get(Ctx, Ops);
  F.setMetadata(MetadataName, N);
}

FunctionEffectSummary
FunctionEffectSummary::readFromMetadata(Function &F) {
  FunctionEffectSummary S;
  MDNode *N = F.getMetadata(MetadataName);
  if (!N || N->getNumOperands() < 5)
    return S;

  if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(0)))
    S.MayBlock = M->getZExtValue();
  if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(1)))
    S.MayAlloc = M->getZExtValue();
  if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(2)))
    S.status =
        static_cast<FunctionEffectSummary::Status>(M->getZExtValue());
  if (auto *M = dyn_cast<MDString>(N->getOperand(3)))
    S.ReasonBlockFn = M->getString().str();
  if (auto *M = dyn_cast<MDString>(N->getOperand(4)))
    S.ReasonAllocFn = M->getString().str();
  if (N->getNumOperands() >= 10) {
    if (auto *M = mdconst::dyn_extract<ConstantInt>(N->getOperand(5)))
      S.HasUnknown = M->getZExtValue();
    if (auto *M = dyn_cast<MDString>(N->getOperand(6)))
      S.ReasonUnknown = M->getString().str();
    if (auto *M = dyn_cast<MDString>(N->getOperand(7)))
      S.BlockChain = decodeChain(M->getString());
    if (auto *M = dyn_cast<MDString>(N->getOperand(8)))
      S.AllocChain = decodeChain(M->getString());
    if (auto *M = dyn_cast<MDString>(N->getOperand(9)))
      S.UnknownChain = decodeChain(M->getString());
  }

  return S;
}
