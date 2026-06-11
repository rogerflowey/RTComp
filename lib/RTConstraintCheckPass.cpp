#include "RTEffect/EffectSummary.h"
#include "RTEffect/Passes.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include <memory>
#include <system_error>

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

std::string escapeJson(StringRef S) {
  std::string Out;
  for (char C : S) {
    switch (C) {
    case '"':
      Out += "\\\"";
      break;
    case '\\':
      Out += "\\\\";
      break;
    case '\n':
      Out += "\\n";
      break;
    case '\r':
      Out += "\\r";
      break;
    case '\t':
      Out += "\\t";
      break;
    default:
      Out += C;
      break;
    }
  }
  return Out;
}

StringRef effectName(StringRef Constraint) {
  if (Constraint == "nonblocking")
    return "may_block";
  if (Constraint == "nonallocating")
    return "may_alloc";
  return "unknown";
}

StringRef classifyChain(Function &F,
                        const std::vector<RTProvenanceFrame> &Chain,
                        bool IsUnknown) {
  if (IsUnknown)
    return "Unknown";
  if (!Chain.empty() && Chain.front().FunctionName == F.getName() &&
      Chain.size() == 1)
    return "direct";
  return "transitive";
}

std::string locationString(const RTProvenanceFrame &Frame) {
  if (Frame.File.empty())
    return "<unknown>";
  std::string Loc = Frame.File + ":" + std::to_string(Frame.Line);
  if (Frame.Column)
    Loc += ":" + std::to_string(Frame.Column);
  return Loc;
}

void printChain(raw_ostream &OS,
                const std::vector<RTProvenanceFrame> &Chain) {
  for (size_t I = 0; I < Chain.size(); ++I) {
    const RTProvenanceFrame &Frame = Chain[I];
    OS << "    #" << I << " " << Frame.FunctionName;
    if (!Frame.CalleeName.empty())
      OS << " -> " << Frame.CalleeName;
    OS << " [" << Frame.Kind << "]";
    OS << " at " << locationString(Frame) << "\n";
    if (!Frame.InstructionText.empty())
      OS << "        " << Frame.InstructionText << "\n";
  }
}

void writeJsonDiagnostic(raw_ostream &OS, Function &F, StringRef Kind,
                         StringRef Constraint, StringRef Effect,
                         StringRef Confidence,
                         const std::vector<RTProvenanceFrame> &Chain,
                         StringRef Suggestion) {
  OS << "{\"function\":\"" << escapeJson(F.getName()) << "\",";
  OS << "\"kind\":\"" << Kind << "\",";
  OS << "\"constraint\":\"" << Constraint << "\",";
  OS << "\"effect\":\"" << Effect << "\",";
  OS << "\"confidence\":\"" << Confidence << "\",";
  OS << "\"suggestion\":\"" << escapeJson(Suggestion) << "\",";
  OS << "\"chain\":[";
  for (size_t I = 0; I < Chain.size(); ++I) {
    if (I)
      OS << ",";
    const RTProvenanceFrame &Frame = Chain[I];
    OS << "{\"function\":\"" << escapeJson(Frame.FunctionName) << "\",";
    OS << "\"callee\":\"" << escapeJson(Frame.CalleeName) << "\",";
    OS << "\"kind\":\"" << escapeJson(Frame.Kind) << "\",";
    OS << "\"instruction\":\"" << escapeJson(Frame.InstructionText)
       << "\"}";
  }
  OS << "],\"locations\":[";
  for (size_t I = 0; I < Chain.size(); ++I) {
    if (I)
      OS << ",";
    const RTProvenanceFrame &Frame = Chain[I];
    OS << "{\"file\":\"" << escapeJson(Frame.File) << "\",";
    OS << "\"line\":" << Frame.Line << ",";
    OS << "\"column\":" << Frame.Column << "}";
  }
  OS << "]}\n";
}

StringRef suggestionFor(StringRef Kind, StringRef Constraint) {
  if (Kind == "Unknown")
    return "Add the callee to external_funcs.yaml or replace the indirect call with an analyzable wrapper.";
  if (Constraint == "nonblocking")
    return "Move blocking work outside the real-time path or use a nonblocking API.";
  return "Preallocate memory, use stack/RT heap storage, or remove heap allocation from the real-time path.";
}

void emitDiagnostic(Function &F, StringRef Kind, StringRef Constraint,
                    StringRef Effect, StringRef Reason,
                    const std::vector<RTProvenanceFrame> &Chain,
                    raw_ostream *JsonOS) {
  StringRef Confidence = Kind == "Unknown" ? "low" : "high";
  StringRef Suggestion = suggestionFor(Kind, Constraint);

  errs() << "[RT-FEA] " << Constraint << " " << Kind << " violation in "
         << F.getName() << ": " << Effect;
  if (!Reason.empty())
    errs() << " via " << Reason;
  errs() << "\n";
  printChain(errs(), Chain);
  errs() << "    suggestion: " << Suggestion << "\n";

  if (JsonOS)
    writeJsonDiagnostic(*JsonOS, F, Kind, Constraint, Effect, Confidence, Chain,
                        Suggestion);
}

} // namespace

PreservedAnalyses RTConstraintCheckPass::run(Module &M,
                                             ModuleAnalysisManager &AM) {
  errs() << "[RTConstraintCheckPass] Checking real-time constraints...\n";
  int Violations = 0;
  int Unknowns = 0;

  std::unique_ptr<raw_fd_ostream> JsonFile;
  const char *JsonPath = getenv("RTEFFECT_JSON_DIAGNOSTICS");
  if (JsonPath && JsonPath[0]) {
    std::error_code EC;
    JsonFile = std::make_unique<raw_fd_ostream>(JsonPath, EC,
                                                sys::fs::OF_Text);
    if (EC) {
      errs() << "[RTConstraintCheckPass] Warning: could not open JSON "
                "diagnostics file '"
             << JsonPath << "': " << EC.message() << "\n";
      JsonFile.reset();
    }
  }

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
    bool UnknownViolation = Summary.HasUnknown;
    bool AnyViolation = BlockViolation || AllocViolation;

    if (AnyViolation) {
      Summary.status = FunctionEffectSummary::Violating;
      if (BlockViolation) {
        Violations++;
        StringRef Kind = classifyChain(F, Summary.BlockChain, false);
        emitDiagnostic(F, Kind, "nonblocking", effectName("nonblocking"),
                       Summary.ReasonBlockFn, Summary.BlockChain,
                       JsonFile.get());
      }
      if (AllocViolation) {
        Violations++;
        StringRef Kind = classifyChain(F, Summary.AllocChain, false);
        emitDiagnostic(F, Kind, "nonallocating", effectName("nonallocating"),
                       Summary.ReasonAllocFn, Summary.AllocChain,
                       JsonFile.get());
      }
    } else if (UnknownViolation) {
      Summary.status = FunctionEffectSummary::Unknown;
      Unknowns++;
      emitDiagnostic(F, "Unknown",
                     IsNonblocking ? "nonblocking" : "nonallocating",
                     "unknown-call", Summary.ReasonUnknown,
                     Summary.UnknownChain, JsonFile.get());
    } else {
      Summary.status = FunctionEffectSummary::ProvenSafe;
      errs() << "[RT-FEA] SAFE: '" << F.getName().str()
             << "' passes all real-time constraints\n";
    }

    FunctionEffectSummary::writeToMetadata(F, Summary);
  }

  errs() << "[RTConstraintCheckPass] Found " << Violations
         << " violation(s), " << Unknowns << " unknown entrie(s)\n";

  return PreservedAnalyses::all();
}
