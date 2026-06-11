#include "RTEffect/EffectSummary.h"
#include "RTEffect/Passes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>
#include <string>

using namespace llvm;

#define DEBUG_TYPE "rt-san-place"

namespace {

bool hasAnnotation(Function &F, StringRef Annotation) {
  GlobalVariable *GA = F.getParent()->getNamedGlobal("llvm.global.annotations");
  if (!GA)
    return false;

  if (auto *CA = dyn_cast<ConstantArray>(GA->getInitializer())) {
    for (unsigned i = 0; i < CA->getNumOperands(); ++i) {
      if (auto *CS = dyn_cast<ConstantStruct>(CA->getOperand(i))) {
        if (CS->getNumOperands() >= 2) {
          if (auto *Val = dyn_cast<Constant>(
                  CS->getOperand(0)->stripPointerCasts())) {
            if (Val->stripPointerCasts() == &F) {
              Value *AnnPtr = CS->getOperand(1)->stripPointerCasts();
              if (auto *GV = dyn_cast<GlobalVariable>(AnnPtr)) {
                if (auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer())) {
                  if (CDA->getAsString().contains(Annotation))
                    return true;
                }
              }
            }
          }
        }
      }
    }
  }
  return false;
}

bool hasConstraint(Function &F, const char *Constraint) {
  if (F.hasFnAttribute(Constraint))
    return true;

  return hasAnnotation(F, Constraint);
}

bool isRTFunction(Function &F) {
  return hasConstraint(F, "nonblocking") ||
         hasConstraint(F, "nonallocating") ||
         hasConstraint(F, "rt_nonblocking") ||
         hasConstraint(F, "rt_nonallocating");
}

bool isRuntimeHook(Function &F) {
  return F.getName() == "__rtsan_realtime_enter" ||
         F.getName() == "__rtsan_realtime_exit";
}

void markNoSanitizeRealtime(Function &F) {
  LLVMContext &Ctx = F.getContext();
  F.setMetadata("nosanitize_realtime", MDNode::get(Ctx, {}));
}

Value *getFunctionNamePtr(Function &F, Module &M) {
  LLVMContext &Ctx = M.getContext();
  std::string GlobalName = "__rtsan_func_name." + F.getName().str();
  auto MakePtr = [&](GlobalVariable *GV) -> Constant * {
    SmallVector<Constant *, 2> Indices = {
        ConstantInt::get(Type::getInt32Ty(Ctx), 0),
        ConstantInt::get(Type::getInt32Ty(Ctx), 0)};
    return ConstantExpr::getInBoundsGetElementPtr(
        GV->getValueType(), GV, Indices);
  };

  if (auto *GV = M.getNamedGlobal(GlobalName))
    return MakePtr(GV);

  Constant *NameConst = ConstantDataArray::getString(Ctx, F.getName(), true);
  auto *GV = new GlobalVariable(M, NameConst->getType(), true,
                                GlobalValue::PrivateLinkage, NameConst,
                                GlobalName);
  GV->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);
  GV->setAlignment(Align(1));
  return MakePtr(GV);
}

bool isFunctionExitTerminator(Instruction *Term) {
  return isa<ReturnInst>(Term) || isa<ResumeInst>(Term) ||
         isa<CleanupReturnInst>(Term) || isa<CatchReturnInst>(Term);
}

} // namespace

PreservedAnalyses RTSanPlacementPass::run(Module &M,
                                          ModuleAnalysisManager &AM) {
  errs() << "[RTSanPlacementPass] Placing RTSan instrumentation"
         << (InstrumentAll ? " (instrument-all mode)" : " (selective mode)")
         << "...\n";

  LLVMContext &Ctx = M.getContext();
  FunctionType *HookTy =
      FunctionType::get(Type::getVoidTy(Ctx), {PointerType::getUnqual(Ctx)}, false);

  FunctionCallee EnterHook =
      M.getOrInsertFunction("__rtsan_realtime_enter", HookTy);
  FunctionCallee ExitHook =
      M.getOrInsertFunction("__rtsan_realtime_exit", HookTy);

  int Instrumented = 0;
  int Skipped = 0;

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (isRuntimeHook(F)) {
      Skipped++;
      continue;
    }

    bool IsRT = isRTFunction(F);

    if (!IsRT) {
      Skipped++;
      continue;
    }

    FunctionEffectSummary Summary = FunctionEffectSummary::readFromMetadata(F);

    if (!InstrumentAll) {
      if (IsRT && Summary.status == FunctionEffectSummary::ProvenSafe) {
        markNoSanitizeRealtime(F);
        errs() << "  Skipping (ProvenSafe RT): " << F.getName() << "\n";
        Skipped++;
        continue;
      }
    }

    errs() << "  Instrumenting: " << F.getName()
           << " [status=" << (int)Summary.status;
    if (IsRT)
      errs() << ", RT";
    errs() << "]\n";

    BasicBlock &EntryBB = F.getEntryBlock();
    IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());

    Value *FuncName = getFunctionNamePtr(F, M);

    Builder.CreateCall(EnterHook, {FuncName});

    std::vector<Instruction *> ExitTerms;
    for (auto &BB : F) {
      Instruction *Term = BB.getTerminator();
      if (isFunctionExitTerminator(Term))
        ExitTerms.push_back(Term);
    }

    for (Instruction *Term : ExitTerms) {
      IRBuilder<> ExitBuilder(Term);
      ExitBuilder.CreateCall(ExitHook, {FuncName});
    }

    Instrumented++;
  }

  errs() << "[RTSanPlacementPass] Instrumented " << Instrumented
         << " functions, skipped " << Skipped << "\n";

  return PreservedAnalyses::none();
}
