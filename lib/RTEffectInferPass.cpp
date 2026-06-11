#include "RTEffect/EffectSummary.h"
#include "RTEffect/ExternalFuncTable.h"
#include "RTEffect/Passes.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Passes/PassBuilder.h"
#if __has_include("llvm/Passes/PassPlugin.h")
#include "llvm/Passes/PassPlugin.h"
#else
#include "llvm/Plugins/PassPlugin.h"
#endif
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <map>
#include <queue>
#include <set>
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

std::string chainToReason(const std::vector<RTProvenanceFrame> &Chain,
                          StringRef Fallback) {
  std::string Reason;
  for (const RTProvenanceFrame &Frame : Chain) {
    StringRef Name = !Frame.CalleeName.empty() ? StringRef(Frame.CalleeName)
                                               : StringRef(Frame.FunctionName);
    if (Name.empty())
      continue;
    if (!Reason.empty())
      Reason += " -> ";
    Reason += Name.str();
  }
  if (Reason.empty())
    Reason = Fallback.str();
  return Reason;
}

void setKnownEffect(FunctionEffectSummary &Summary, Function &F,
                    Instruction &I, StringRef CalleeName, StringRef Kind,
                    bool MayBlock, bool MayAlloc) {
  RTProvenanceFrame Frame =
      FunctionEffectSummary::makeFrame(F, I, CalleeName, Kind);
  if (MayBlock && !Summary.MayBlock) {
    Summary.MayBlock = true;
    Summary.BlockChain = {Frame};
    Summary.ReasonBlockFn = chainToReason(Summary.BlockChain, CalleeName);
  }
  if (MayAlloc && !Summary.MayAlloc) {
    Summary.MayAlloc = true;
    Summary.AllocChain = {Frame};
    Summary.ReasonAllocFn = chainToReason(Summary.AllocChain, CalleeName);
  }
}

void setUnknownEffect(FunctionEffectSummary &Summary, Function &F,
                      Instruction &I, StringRef CalleeName, StringRef Kind) {
  if (Summary.HasUnknown)
    return;
  Summary.HasUnknown = true;
  Summary.UnknownChain = {
      FunctionEffectSummary::makeFrame(F, I, CalleeName, Kind)};
  Summary.ReasonUnknown = chainToReason(Summary.UnknownChain, CalleeName);
}

void scanDirectEffects(Function &F, FunctionEffectSummary &Summary,
                       const ExternalFuncTable &ExtTable) {
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (CB->isIndirectCall()) {
          setUnknownEffect(Summary, F, I, "<indirect call>",
                           "indirect-call");
          continue;
        }

        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;

        StringRef CalleeName = Callee->getName();

        if (Callee->isIntrinsic())
          continue;

        if (auto *Info = ExtTable.lookup(CalleeName)) {
          setKnownEffect(Summary, F, I, CalleeName, "external",
                         Info->MayBlock, Info->MayAlloc);
          continue;
        }

        if (Callee->empty()) {
          setUnknownEffect(Summary, F, I, CalleeName, "unknown-external");
        }
      }
    }
  }
}

bool isRecursiveSCC(const std::vector<Function *> &SCC,
                    const std::map<Function *, std::set<Function *>> &CallGraph) {
  if (SCC.size() > 1)
    return true;
  if (SCC.empty())
    return false;
  auto It = CallGraph.find(SCC.front());
  return It != CallGraph.end() && It->second.count(SCC.front());
}

void markRecursiveSources(
    std::map<Function *, FunctionEffectSummary> &DirectEffects,
    const std::map<Function *, std::set<Function *>> &CallGraph,
    const std::vector<std::vector<Function *>> &SCCs) {
  for (const auto &SCC : SCCs) {
    if (!isRecursiveSCC(SCC, CallGraph))
      continue;
    for (Function *F : SCC) {
      auto It = DirectEffects.find(F);
      if (It == DirectEffects.end() || !It->second.hasAnyEffect())
        continue;
      RTProvenanceFrame Frame;
      Frame.FunctionName = F->getName().str();
      Frame.CalleeName = "<recursive-scc>";
      Frame.Kind = "cycle";
      if (DISubprogram *SP = F->getSubprogram()) {
        Frame.File = SP->getFilename().str();
        Frame.Line = SP->getLine();
      }
      auto Prefix = [&](std::vector<RTProvenanceFrame> &Chain) {
        if (!Chain.empty() && Chain.front().Kind != "cycle")
          Chain.insert(Chain.begin(), Frame);
      };
      Prefix(It->second.BlockChain);
      Prefix(It->second.AllocChain);
      Prefix(It->second.UnknownChain);
      if (It->second.MayBlock)
        It->second.ReasonBlockFn =
            chainToReason(It->second.BlockChain, It->second.ReasonBlockFn);
      if (It->second.MayAlloc)
        It->second.ReasonAllocFn =
            chainToReason(It->second.AllocChain, It->second.ReasonAllocFn);
      if (It->second.HasUnknown)
        It->second.ReasonUnknown =
            chainToReason(It->second.UnknownChain, It->second.ReasonUnknown);
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

std::vector<RTProvenanceFrame>
prependCallFrame(Function &Caller, CallBase &CB, Function &Callee,
                 StringRef Kind,
                 const std::vector<RTProvenanceFrame> &CalleeChain) {
  std::vector<RTProvenanceFrame> Chain;
  Chain.push_back(
      FunctionEffectSummary::makeFrame(Caller, CB, Callee.getName(), Kind));
  Chain.insert(Chain.end(), CalleeChain.begin(), CalleeChain.end());
  return Chain;
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
  markRecursiveSources(DirectEffects, CallGraph, SCCs);

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
      for (auto &BB : *F) {
        for (auto &I : BB) {
          auto *CB = dyn_cast<CallBase>(&I);
          if (!CB || CB->isIndirectCall())
            continue;
          Function *Callee = CB->getCalledFunction();
          if (!Callee || !CGIt->second.count(Callee))
            continue;

          auto It = FuncSummaries.find(Callee);
          if (It == FuncSummaries.end())
            continue;

          if (It->second.MayBlock && !NewSummary.MayBlock) {
            NewSummary.MayBlock = true;
            NewSummary.BlockChain =
                prependCallFrame(*F, *CB, *Callee, "call",
                                 It->second.BlockChain);
            NewSummary.ReasonBlockFn =
                chainToReason(NewSummary.BlockChain, Callee->getName());
          }
          if (It->second.MayAlloc && !NewSummary.MayAlloc) {
            NewSummary.MayAlloc = true;
            NewSummary.AllocChain =
                prependCallFrame(*F, *CB, *Callee, "call",
                                 It->second.AllocChain);
            NewSummary.ReasonAllocFn =
                chainToReason(NewSummary.AllocChain, Callee->getName());
          }
          if (It->second.HasUnknown && !NewSummary.HasUnknown) {
            NewSummary.HasUnknown = true;
            NewSummary.UnknownChain =
                prependCallFrame(*F, *CB, *Callee, "call",
                                 It->second.UnknownChain);
            NewSummary.ReasonUnknown =
                chainToReason(NewSummary.UnknownChain, Callee->getName());
          }
        }
      }
    }

    auto &Old = FuncSummaries[F];
    if (NewSummary.MayBlock != Old.MayBlock ||
        NewSummary.MayAlloc != Old.MayAlloc ||
        NewSummary.HasUnknown != Old.HasUnknown ||
        NewSummary.ReasonBlockFn != Old.ReasonBlockFn ||
        NewSummary.ReasonAllocFn != Old.ReasonAllocFn ||
        NewSummary.ReasonUnknown != Old.ReasonUnknown) {
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
           << " may_alloc=" << S.MayAlloc
           << " unknown=" << S.HasUnknown;
    if (S.MayBlock)
      errs() << " [via " << S.ReasonBlockFn << "]";
    if (S.MayAlloc)
      errs() << " [via " << S.ReasonAllocFn << "]";
    if (S.HasUnknown)
      errs() << " [unknown via " << S.ReasonUnknown << "]";
    errs() << "\n";
  }

  return PreservedAnalyses::all();
}
