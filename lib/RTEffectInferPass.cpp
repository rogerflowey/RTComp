#include "RTEffect/EffectSummary.h"
#include "RTEffect/ExternalFuncTable.h"
#include "RTEffect/Passes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"
#include <set>
#include <map>
#include <queue>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "rt-effect-infer"

RTConfig GlobalRTConfig;

static const char *getExternalFuncFile() {
  const char *Env = getenv("RTEFFECT_EXTERNAL_FUNC_FILE");
  if (Env && Env[0])
    return Env;
  if (!GlobalRTConfig.ExternalFuncFile.empty())
    return GlobalRTConfig.ExternalFuncFile.c_str();
  return "external_funcs.yaml";
}

namespace {

void scanDirectEffects(Function &F, FunctionEffectSummary &Summary,
                       const ExternalFuncTable &ExtTable) {
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->isIndirectCall()) {
          Summary.MayBlock = true;
          Summary.MayAlloc = true;
          Summary.ReasonBlockFn = "<indirect call>";
          Summary.ReasonAllocFn = "<indirect call>";
          continue;
        }

        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;

        StringRef CalleeName = Callee->getName();

        if (Callee->isIntrinsic())
          continue;

        if (auto *Info = ExtTable.lookup(CalleeName)) {
          if (Info->MayBlock) {
            Summary.MayBlock = true;
            Summary.ReasonBlockFn = CalleeName.str();
          }
          if (Info->MayAlloc) {
            Summary.MayAlloc = true;
            Summary.ReasonAllocFn = CalleeName.str();
          }
          continue;
        }

        if (Callee->empty()) {
          Summary.MayBlock = true;
          Summary.MayAlloc = true;
          Summary.ReasonBlockFn = CalleeName.str() + " <unknown external>";
          Summary.ReasonAllocFn = CalleeName.str() + " <unknown external>";
        }
      }
    }
  }
}

void computeSCC(const std::map<Function *, std::set<Function *>> &CallGraph,
                std::vector<std::vector<Function *>> &SCCs) {
  std::map<Function *, int> Index, LowLink;
  std::vector<Function *> Stack;
  std::set<Function *> OnStack;
  int CurrIndex = 0;

  for (auto &Pair : CallGraph) {
    Index[Pair.first] = -1;
    LowLink[Pair.first] = -1;
    for (auto *Callee : Pair.second) {
      Index[Callee] = -1;
      LowLink[Callee] = -1;
    }
  }

  std::function<void(Function *)> StrongConnect = [&](Function *F) {
    Index[F] = CurrIndex;
    LowLink[F] = CurrIndex;
    CurrIndex++;
    Stack.push_back(F);
    OnStack.insert(F);

    auto It = CallGraph.find(F);
    if (It != CallGraph.end()) {
      for (Function *W : It->second) {
        if (Index[W] == -1) {
          StrongConnect(W);
          LowLink[F] = std::min(LowLink[F], LowLink[W]);
        } else if (OnStack.count(W)) {
          LowLink[F] = std::min(LowLink[F], Index[W]);
        }
      }
    }

    if (LowLink[F] == Index[F]) {
      std::vector<Function *> SCC;
      Function *W;
      do {
        W = Stack.back();
        Stack.pop_back();
        OnStack.erase(W);
        SCC.push_back(W);
      } while (W != F);
      SCCs.push_back(std::move(SCC));
    }
  };

  for (auto &Pair : CallGraph) {
    if (Index[Pair.first] == -1)
      StrongConnect(Pair.first);
  }
}

} // namespace

PreservedAnalyses RTEffectInferPass::run(Module &M,
                                         ModuleAnalysisManager &AM) {
  const char *FilePath = getExternalFuncFile();
  errs() << "[RTEffectInferPass] Starting effect inference...\n";

  auto ExtTableOrErr = ExternalFuncTable::load(FilePath);
  if (!ExtTableOrErr) {
    errs() << "[RTEffectInferPass] Warning: Could not load external func "
              "table from '"
           << FilePath << "'. Using conservative defaults.\n";
  }
  ExternalFuncTable ExtTable =
      ExtTableOrErr ? *ExtTableOrErr : ExternalFuncTable();

  std::map<Function *, FunctionEffectSummary> DirectEffects;
  std::map<Function *, std::set<Function *>> CallGraph;
  std::map<Function *, std::set<Function *>> CallersOf;

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;

    DirectEffects[&F] = FunctionEffectSummary();
    CallGraph[&F] = {};
    CallersOf[&F] = {};

    scanDirectEffects(F, DirectEffects[&F], ExtTable);

    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB)
          continue;
        if (CB->isIndirectCall())
          continue;
        Function *Callee = CB->getCalledFunction();
        if (!Callee || Callee->isIntrinsic() || Callee->getName().empty())
          continue;

        if (!Callee->empty()) {
          CallGraph[&F].insert(Callee);
          CallersOf[Callee].insert(&F);
        }
      }
    }
  }

  std::vector<std::vector<Function *>> SCCs;
  computeSCC(CallGraph, SCCs);

  auto FuncSummaries = DirectEffects;

  std::queue<Function *> Worklist;
  for (auto &Pair : FuncSummaries) {
    Worklist.push(Pair.first);
  }

  while (!Worklist.empty()) {
    Function *F = Worklist.front();
    Worklist.pop();

    FunctionEffectSummary NewSummary = DirectEffects[F];

    auto CGIt = CallGraph.find(F);
    if (CGIt != CallGraph.end()) {
      for (Function *Callee : CGIt->second) {
        auto It = FuncSummaries.find(Callee);
        if (It != FuncSummaries.end()) {
          if (It->second.MayBlock) {
            if (!NewSummary.MayBlock) {
              NewSummary.MayBlock = true;
              NewSummary.ReasonBlockFn =
                  Callee->getName().str() + " -> " + It->second.ReasonBlockFn;
            }
          }
          if (It->second.MayAlloc) {
            if (!NewSummary.MayAlloc) {
              NewSummary.MayAlloc = true;
              NewSummary.ReasonAllocFn =
                  Callee->getName().str() + " -> " + It->second.ReasonAllocFn;
            }
          }
        }
      }
    }

    auto &Old = FuncSummaries[F];
    if (NewSummary.MayBlock != Old.MayBlock ||
        NewSummary.MayAlloc != Old.MayAlloc ||
        NewSummary.ReasonBlockFn != Old.ReasonBlockFn ||
        NewSummary.ReasonAllocFn != Old.ReasonAllocFn) {
      Old = NewSummary;

      auto It = CallersOf.find(F);
      if (It != CallersOf.end()) {
        for (Function *Caller : It->second) {
          Worklist.push(Caller);
        }
      }
    }
  }

  for (auto &Pair : FuncSummaries) {
    FunctionEffectSummary::writeToMetadata(*Pair.first, Pair.second);

    auto &S = Pair.second;
    errs() << "  " << Pair.first->getName() << ": may_block=" << S.MayBlock
           << " may_alloc=" << S.MayAlloc;
    if (S.MayBlock)
      errs() << " [via " << S.ReasonBlockFn << "]";
    if (S.MayAlloc)
      errs() << " [via " << S.ReasonAllocFn << "]";
    errs() << "\n";
  }

  return PreservedAnalyses::all();
}
