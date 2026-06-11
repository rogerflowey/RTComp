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
#include <sstream>
#include <system_error>
#include <vector>

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

//   * function attribute "rt_stack_bound"="<N>"
//   * annotation string "rt_stack_bound=<N>"
// Returns -1 when no bound is set.
int parseStackBound(Function &F) {
  if (F.hasFnAttribute("rt_stack_bound")) {
    StringRef V = F.getFnAttribute("rt_stack_bound").getValueAsString();
    int N = -1;
    if (!V.getAsInteger(10, N))
      return N;
  }
  GlobalVariable *GA =
      F.getParent()->getNamedGlobal("llvm.global.annotations");
  if (!GA)
    return -1;
  auto *CA = dyn_cast<ConstantArray>(GA->getInitializer());
  if (!CA)
    return -1;
  for (unsigned i = 0; i < CA->getNumOperands(); ++i) {
    auto *CS = dyn_cast<ConstantStruct>(CA->getOperand(i));
    if (!CS || CS->getNumOperands() < 2)
      continue;
    auto *Val =
        dyn_cast<Constant>(CS->getOperand(0)->stripPointerCasts());
    if (!Val || Val->stripPointerCasts() != &F)
      continue;
    auto *GV = dyn_cast<GlobalVariable>(
        CS->getOperand(1)->stripPointerCasts());
    if (!GV)
      continue;
    auto *CDA = dyn_cast<ConstantDataArray>(GV->getInitializer());
    if (!CDA)
      continue;
    StringRef S = CDA->getAsString();
    constexpr const char *Tag = "rt_stack_bound=";
    size_t Pos = S.find(Tag);
    if (Pos == StringRef::npos)
      continue;
    StringRef Rest = S.substr(Pos + std::strlen(Tag));
    size_t End = Rest.find_first_not_of("0123456789");
    if (End != StringRef::npos)
      Rest = Rest.substr(0, End);
    int N = -1;
    if (!Rest.getAsInteger(10, N))
      return N;
  }
  return -1;
}

std::string escapeJson(StringRef S) {
  std::string Out;
  for (char C : S) {
    switch (C) {
    case '"':  Out += "\\\""; break;
    case '\\': Out += "\\\\"; break;
    case '\n': Out += "\\n";  break;
    case '\r': Out += "\\r";  break;
    case '\t': Out += "\\t";  break;
    default:   Out += C;      break;
    }
  }
  return Out;
}

StringRef effectName(StringRef Constraint) {
  if (Constraint == "nonblocking")    return "may_block";
  if (Constraint == "nonallocating")  return "may_alloc";
  if (Constraint == "nothrow")        return "may_throw";
  if (Constraint == "nolock")         return "may_lock";
  if (Constraint == "norecurse")      return "may_recurse";
  if (Constraint == "async_signal_safe") return "may_signal_unsafe";
  if (Constraint == "stackbound")     return "stack_depth";
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

StringRef suggestionFor(StringRef Kind, StringRef Constraint) {
  if (Kind == "Unknown")
    return "Add the callee to external_funcs.yaml or replace the "
           "indirect call with an analyzable wrapper.";
  if (Constraint == "nonblocking")
    return "Move blocking work outside the real-time path or use a "
           "nonblocking API.";
  if (Constraint == "nonallocating")
    return "Preallocate memory, use stack/RT heap storage, or remove "
           "heap allocation from the real-time path.";
  if (Constraint == "nothrow")
    return "Replace the throwing call with an error-code variant or "
           "guard the call with noexcept-equivalent wrappers.";
  if (Constraint == "nolock")
    return "Avoid mutex acquisition; use lock-free data structures or "
           "single-producer/consumer queues.";
  if (Constraint == "norecurse")
    return "Convert recursion into iteration or break the recursive "
           "cycle in the call graph.";
  if (Constraint == "async_signal_safe")
    return "Use only async-signal-safe primitives in the signal path.";
  if (Constraint == "stackbound")
    return "Reduce maximum call depth by inlining or refactoring the "
           "deepest paths.";
  return "Review the call chain and replace problematic operations.";
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

// We accumulate one SARIF "result" per diagnostic and serialize a single
// run at the end of the pass.

struct SarifResult {
  std::string FunctionName;
  std::string Kind;
  std::string Constraint;
  std::string Effect;
  std::string Confidence;
  std::string Suggestion;
  std::vector<RTProvenanceFrame> Chain;
};

void writeSarifLocation(raw_ostream &OS,
                        const RTProvenanceFrame &Frame) {
  OS << "{";
  OS << "\"physicalLocation\":{";
  OS << "\"artifactLocation\":{\"uri\":\""
     << escapeJson(Frame.File.empty() ? "<unknown>" : Frame.File)
     << "\"},";
  OS << "\"region\":{\"startLine\":"
     << (Frame.Line ? Frame.Line : 1u);
  if (Frame.Column)
    OS << ",\"startColumn\":" << Frame.Column;
  OS << "}";
  OS << "},";
  OS << "\"message\":{\"text\":\""
     << escapeJson(Frame.FunctionName + " -> " + Frame.CalleeName)
     << "\"}";
  OS << "}";
}

void writeSarifReport(raw_ostream &OS,
                      const std::vector<SarifResult> &Results) {
  // Collect rule definitions (one per constraint kind seen).
  std::vector<std::string> Rules;
  auto addRule = [&](StringRef Id) {
    for (const std::string &R : Rules)
      if (R == Id.str())
        return;
    Rules.push_back(Id.str());
  };
  for (const SarifResult &R : Results)
    addRule(StringRef("RT-" + R.Constraint));

  OS << "{\"$schema\":\"https://json.schemastore.org/sarif-2.1.0.json\",";
  OS << "\"version\":\"2.1.0\",";
  OS << "\"runs\":[{";
  OS << "\"tool\":{\"driver\":{";
  OS << "\"name\":\"RTEffect\",";
  OS << "\"informationUri\":\"https://example.com/rteffect\",";
  OS << "\"rules\":[";
  for (size_t I = 0; I < Rules.size(); ++I) {
    if (I)
      OS << ",";
    OS << "{\"id\":\"" << escapeJson(Rules[I])
       << "\",\"shortDescription\":{\"text\":\""
       << escapeJson(Rules[I]) << "\"}}";
  }
  OS << "]}},";
  OS << "\"results\":[";
  for (size_t I = 0; I < Results.size(); ++I) {
    if (I)
      OS << ",";
    const SarifResult &R = Results[I];
    OS << "{";
    OS << "\"ruleId\":\"RT-" << escapeJson(R.Constraint) << "\",";
    OS << "\"level\":\""
       << (R.Confidence == "high" ? "error" : "warning") << "\",";
    OS << "\"message\":{\"text\":\""
       << escapeJson("[" + R.Kind + "] " + R.FunctionName + " violates " +
                     R.Constraint + " (" + R.Effect + "): " + R.Suggestion)
       << "\"},";
    if (!R.Chain.empty()) {
      OS << "\"locations\":[";
      writeSarifLocation(OS, R.Chain.front());
      OS << "],";
      OS << "\"codeFlows\":[{\"threadFlows\":[{\"locations\":[";
      for (size_t J = 0; J < R.Chain.size(); ++J) {
        if (J)
          OS << ",";
        OS << "{\"location\":";
        writeSarifLocation(OS, R.Chain[J]);
        OS << "}";
      }
      OS << "]}]}],";
    }
    OS << "\"properties\":{";
    OS << "\"function\":\"" << escapeJson(R.FunctionName) << "\",";
    OS << "\"kind\":\"" << escapeJson(R.Kind) << "\",";
    OS << "\"effect\":\"" << escapeJson(R.Effect) << "\",";
    OS << "\"confidence\":\"" << escapeJson(R.Confidence) << "\"";
    OS << "}";
    OS << "}";
  }
  OS << "]}]}\n";
}

void emitDiagnostic(Function &F, StringRef Kind, StringRef Constraint,
                    StringRef Effect, StringRef Reason,
                    const std::vector<RTProvenanceFrame> &Chain,
                    raw_ostream *JsonOS,
                    std::vector<SarifResult> *SarifAcc) {
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

  if (SarifAcc) {
    SarifResult R;
    R.FunctionName = F.getName().str();
    R.Kind = Kind.str();
    R.Constraint = Constraint.str();
    R.Effect = Effect.str();
    R.Confidence = Confidence.str();
    R.Suggestion = Suggestion.str();
    R.Chain = Chain;
    SarifAcc->push_back(std::move(R));
  }
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

  std::vector<SarifResult> SarifBuf;
  const char *SarifPath = getenv("RTEFFECT_SARIF_DIAGNOSTICS");
  bool SarifEnabled = (SarifPath && SarifPath[0]);

  // Lookup table: (constraint name -> effect kind index for booleans).
  // For boolean effects we walk this list; stack-bound is special.
  struct ConstraintMap {
    const char *Constraint;
    int EffectKind;
  };
  static const ConstraintMap Map[] = {
      {"nonblocking", RTE_Block},
      {"nonallocating", RTE_Alloc},
      {"nothrow", RTE_Throw},
      {"nolock", RTE_Lock},
      {"norecurse", RTE_Recurse},
      {"async_signal_safe", RTE_SignalUnsafe},
  };

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;

    FunctionEffectSummary Summary = FunctionEffectSummary::readFromMetadata(F);

    bool HasAnyConstraint = false;
    bool AnyViolation = false;

    // Check each boolean constraint.
    for (const ConstraintMap &CM : Map) {
      if (!hasConstraint(F, CM.Constraint))
        continue;
      HasAnyConstraint = true;
      if (*Summary.flagPtr(CM.EffectKind)) {
        AnyViolation = true;
        Violations++;
        StringRef KindStr =
            classifyChain(F, *Summary.chainPtr(CM.EffectKind), false);
        emitDiagnostic(F, KindStr, CM.Constraint,
                       effectName(CM.Constraint),
                       *Summary.reasonPtr(CM.EffectKind),
                       *Summary.chainPtr(CM.EffectKind),
                       JsonFile.get(),
                       SarifEnabled ? &SarifBuf : nullptr);
      }
    }

    int StackBound = parseStackBound(F);
    if (StackBound >= 0) {
      HasAnyConstraint = true;
      bool Bad = Summary.MaxStackDepth == -1 ||
                 Summary.MaxStackDepth > StackBound;
      if (Bad) {
        AnyViolation = true;
        Violations++;
        std::vector<RTProvenanceFrame> Chain;
        RTProvenanceFrame Frame;
        Frame.FunctionName = F.getName().str();
        Frame.CalleeName =
            std::string("<stack_depth=") +
            (Summary.MaxStackDepth == -1
                 ? std::string("unbounded")
                 : std::to_string(Summary.MaxStackDepth)) +
            ">";
        Frame.Kind = "stack-bound";
        if (DISubprogram *SP = F.getSubprogram()) {
          Frame.File = SP->getFilename().str();
          Frame.Line = SP->getLine();
        }
        Chain.push_back(Frame);
        emitDiagnostic(F,
                       Summary.MaxStackDepth == -1 ? "Unknown" : "direct",
                       "stackbound", "stack_depth",
                       Frame.CalleeName, Chain, JsonFile.get(),
                       SarifEnabled ? &SarifBuf : nullptr);
      }
    }

    if (!HasAnyConstraint) {
      Summary.status = FunctionEffectSummary::Unknown;
      FunctionEffectSummary::writeToMetadata(F, Summary);
      continue;
    }

    if (AnyViolation) {
      Summary.status = FunctionEffectSummary::Violating;
    } else if (Summary.HasUnknown) {
      Summary.status = FunctionEffectSummary::Unknown;
      Unknowns++;
      // Pick the strongest constraint name to attribute the Unknown to.
      const char *PickedConstraint = nullptr;
      for (const ConstraintMap &CM : Map)
        if (hasConstraint(F, CM.Constraint)) {
          PickedConstraint = CM.Constraint;
          break;
        }
      emitDiagnostic(F, "Unknown",
                     PickedConstraint ? PickedConstraint : "rt",
                     "unknown-call", Summary.ReasonUnknown,
                     Summary.UnknownChain, JsonFile.get(),
                     SarifEnabled ? &SarifBuf : nullptr);
    } else {
      Summary.status = FunctionEffectSummary::ProvenSafe;
      errs() << "[RT-FEA] SAFE: '" << F.getName().str()
             << "' passes all real-time constraints\n";
    }

    FunctionEffectSummary::writeToMetadata(F, Summary);
  }

  errs() << "[RTConstraintCheckPass] Found " << Violations
         << " violation(s), " << Unknowns << " unknown entrie(s)\n";

  if (SarifEnabled) {
    std::error_code EC;
    raw_fd_ostream Out(SarifPath, EC, sys::fs::OF_Text);
    if (EC) {
      errs() << "[RTConstraintCheckPass] Warning: could not open SARIF "
                "diagnostics file '"
             << SarifPath << "': " << EC.message() << "\n";
    } else {
      writeSarifReport(Out, SarifBuf);
    }
  }

  return PreservedAnalyses::all();
}
