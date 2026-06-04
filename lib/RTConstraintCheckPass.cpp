#include "RTEffect/EffectSummary.h"
#include "RTEffect/Passes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "rt-constraint-check"

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

} // namespace

PreservedAnalyses RTConstraintCheckPass::run(Module &M,
                                             ModuleAnalysisManager &AM) {
  errs() << "[RTConstraintCheckPass] Checking real-time constraints...\n";
  int Violations = 0;

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;

    FunctionEffectSummary Summary = FunctionEffectSummary::readFromMetadata(F);

    bool IsNonblocking = hasConstraint(F, "nonblocking");
    bool IsNonallocating = hasConstraint(F, "nonallocating");
    bool HasAnyConstraint = IsNonblocking || IsNonallocating;

    if (!HasAnyConstraint) {
      Summary.status = FunctionEffectSummary::Unknown;
      FunctionEffectSummary::writeToMetadata(F, Summary);
      continue;
    }

    bool BlockViolation = IsNonblocking && Summary.MayBlock;
    bool AllocViolation = IsNonallocating && Summary.MayAlloc;
    bool AnyViolation = BlockViolation || AllocViolation;

    if (AnyViolation) {
      Summary.status = FunctionEffectSummary::Violating;
      Violations++;

      std::string Diag = "[RTConstraintCheck] VIOLATION in '";
      Diag += F.getName().str();
      Diag += "':";

      if (BlockViolation) {
        Diag += " blocks despite 'nonblocking' constraint";
        if (!Summary.ReasonBlockFn.empty()) {
          Diag += " (via: " + Summary.ReasonBlockFn + ")";
        }
      }
      if (AllocViolation) {
        if (BlockViolation)
          Diag += "; ";
        Diag += " allocates despite 'nonallocating' constraint";
        if (!Summary.ReasonAllocFn.empty()) {
          Diag += " (via: " + Summary.ReasonAllocFn + ")";
        }
      }

      if (DISubprogram *SP = F.getSubprogram()) {
        Diag += " at " + SP->getFilename().str() + ":" +
                std::to_string(SP->getLine());
      }

      errs() << Diag << "\n";
    } else {
      Summary.status = FunctionEffectSummary::ProvenSafe;
      errs() << "[RTConstraintCheck] SAFE: '" << F.getName().str()
             << "' passes all real-time constraints\n";
    }

    FunctionEffectSummary::writeToMetadata(F, Summary);
  }

  errs() << "[RTConstraintCheckPass] Found " << Violations
         << " violation(s)\n";

  return PreservedAnalyses::all();
}
