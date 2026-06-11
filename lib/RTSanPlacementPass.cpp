#include "RTEffect/EffectSummary.h"
#include "RTEffect/Passes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"
#include <vector>

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
  static const char *Constraints[] = {
      "nonblocking",        "nonallocating",      "nothrow",
      "nolock",             "norecurse",          "async_signal_safe",
      "rt_nonblocking",     "rt_nonallocating",   "rt_nothrow",
      "rt_nolock",          "rt_norecurse",       "rt_async_signal_safe",
  };
  for (const char *C : Constraints)
    if (hasConstraint(F, C))
      return true;
  if (F.hasFnAttribute("rt_stack_bound"))
    return true;
  return false;
}

bool isRuntimeHook(Function &F) {
  return F.getName() == "__rtsan_realtime_enter" ||
         F.getName() == "__rtsan_realtime_exit";
}

void markNoSanitizeRealtime(Function &F) {
  LLVMContext &Ctx = F.getContext();
  F.setMetadata("nosanitize_realtime", MDNode::get(Ctx, {}));
}

bool isFunctionExitTerminator(Instruction *Term) {
  return isa<ReturnInst>(Term) || isa<ResumeInst>(Term) ||
         isa<CleanupReturnInst>(Term) || isa<CatchReturnInst>(Term);
}

void wrapWholeFunction(Function &F, FunctionCallee EnterHook,
                       FunctionCallee ExitHook) {
  BasicBlock &EntryBB = F.getEntryBlock();
  IRBuilder<> Builder(&EntryBB, EntryBB.getFirstInsertionPt());
  Builder.CreateCall(EnterHook);

  std::vector<Instruction *> ExitTerms;
  for (auto &BB : F) {
    Instruction *Term = BB.getTerminator();
    if (isFunctionExitTerminator(Term))
      ExitTerms.push_back(Term);
  }

  for (Instruction *Term : ExitTerms) {
    IRBuilder<> ExitBuilder(Term);
    ExitBuilder.CreateCall(ExitHook);
  }
}

std::vector<CallBase *> collectWitnesses(Function &F) {
  std::vector<CallBase *> Out;
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->getMetadata("rt.witness"))
          Out.push_back(CB);
      }
    }
  }
  return Out;
}

// Wrap a single call site with enter/exit hooks. For an InvokeInst we
// place the exit hook on both the normal-return edge (at the start of
// the normal destination block) and the unwind edge.
void wrapCallSite(CallBase *CB, FunctionCallee EnterHook,
                  FunctionCallee ExitHook) {
  IRBuilder<> EnterBuilder(CB);
  EnterBuilder.CreateCall(EnterHook);

  if (auto *Inv = dyn_cast<InvokeInst>(CB)) {
    BasicBlock *Normal = Inv->getNormalDest();
    BasicBlock *Unwind = Inv->getUnwindDest();

    // Drop the exit hook at the first non-PHI / non-landingpad
    // insertion point in each successor.
    IRBuilder<> NB(Normal, Normal->getFirstNonPHIIt());
    NB.CreateCall(ExitHook);

    // For unwind dest we have to skip the landingpad / cleanuppad.
    Instruction *FirstInsert = &*Unwind->getFirstNonPHIIt();
    while (FirstInsert &&
           (isa<LandingPadInst>(FirstInsert) || isa<CleanupPadInst>(FirstInsert) ||
            isa<CatchPadInst>(FirstInsert) || isa<CatchSwitchInst>(FirstInsert))) {
      FirstInsert = FirstInsert->getNextNode();
    }
    if (FirstInsert) {
      IRBuilder<> UB(FirstInsert);
      UB.CreateCall(ExitHook);
    } else {
      IRBuilder<> UB(Unwind->getTerminator());
      UB.CreateCall(ExitHook);
    }
    return;
  }

  // Plain call instruction: exit goes immediately after.
  IRBuilder<> ExitBuilder(CB->getNextNode());
  ExitBuilder.CreateCall(ExitHook);
}

} // namespace

PreservedAnalyses RTSanPlacementPass::run(Module &M, ModuleAnalysisManager &AM) {
  errs() << "[RTSanPlacementPass] Placing RTSan instrumentation"
         << (InstrumentAll ? " (instrument-all mode)"
                           : (PerCallSite ? " (selective per-callsite mode)"
                                          : " (selective whole-function mode)"))
         << "...\n";

  LLVMContext &Ctx = M.getContext();
  FunctionType *HookTy = FunctionType::get(Type::getVoidTy(Ctx), false);

  FunctionCallee EnterHook =
      M.getOrInsertFunction("__rtsan_realtime_enter", HookTy);
  FunctionCallee ExitHook =
      M.getOrInsertFunction("__rtsan_realtime_exit", HookTy);

  int InstrumentedFunctions = 0;
  int InstrumentedSites = 0;
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

    if (!InstrumentAll &&
        IsRT && Summary.status == FunctionEffectSummary::ProvenSafe) {
      markNoSanitizeRealtime(F);
      errs() << "  Skipping (ProvenSafe RT): " << F.getName() << "\n";
      Skipped++;
      continue;
    }

    bool UseCallSite = PerCallSite && !InstrumentAll &&
                       Summary.status == FunctionEffectSummary::Violating;
    if (UseCallSite) {
      auto Witnesses = collectWitnesses(F);
      if (Witnesses.empty()) {
        // No leaf instructions found (e.g. transitive-only chains where
        // the witness lives in a callee); fall back to whole-function.
        wrapWholeFunction(F, EnterHook, ExitHook);
        errs() << "  Instrumenting (whole, no local witnesses): "
               << F.getName() << "\n";
      } else {
        for (CallBase *CB : Witnesses)
          wrapCallSite(CB, EnterHook, ExitHook);
        InstrumentedSites += Witnesses.size();
        errs() << "  Instrumenting (per-callsite x" << Witnesses.size()
               << "): " << F.getName() << "\n";
      }
    } else {
      wrapWholeFunction(F, EnterHook, ExitHook);
      errs() << "  Instrumenting (whole): " << F.getName()
             << " [status=" << (int)Summary.status << "]\n";
    }

    InstrumentedFunctions++;
  }

  errs() << "[RTSanPlacementPass] Instrumented " << InstrumentedFunctions
         << " function(s), " << InstrumentedSites
         << " call-site(s), skipped " << Skipped << "\n";

  return PreservedAnalyses::none();
}
