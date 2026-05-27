#include "RTEffect/EffectSummary.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"

using namespace llvm;

const StringLiteral FunctionEffectSummary::MetadataName = "rt.effect";

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

  return S;
}
