#include "RTEffect/EffectSummary.h"
#include "RTEffect/ExternalFuncTable.h"
#include "RTEffect/Passes.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Metadata.h"
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

// Region-marker function names.
constexpr const char *RegionEnterName = "__rt_region_enter";
constexpr const char *RegionExitName  = "__rt_region_exit";

bool isRegionMarker(const Function *F) {
  if (!F) return false;
  StringRef Name = F->getName();
  return Name == RegionEnterName || Name == RegionExitName;
}

bool isRegionEnter(const Function *F) {
  return F && F->getName() == RegionEnterName;
}

bool isRegionExit(const Function *F) {
  return F && F->getName() == RegionExitName;
}

// Returns true if the callee is one of the well-known C++/longjmp throw
// vehicles. The external table also reports may_throw=true for these names,
// so this is mostly a safety net for builds without a YAML table.
bool isThrowVehicle(StringRef Name) {
  return Name == "__cxa_throw" || Name == "__cxa_rethrow" ||
         Name == "_Unwind_RaiseException" || Name == "_Unwind_Resume" ||
         Name == "longjmp" || Name == "siglongjmp";
}

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

void setEffect(FunctionEffectSummary &S, int Kind, Function &F, Instruction &I,
               StringRef CalleeName, StringRef FrameKind) {
  if (*S.flagPtr(Kind))
    return;
  *S.flagPtr(Kind) = true;
  RTProvenanceFrame Frame =
      FunctionEffectSummary::makeFrame(F, I, CalleeName, FrameKind);
  *S.chainPtr(Kind) = {Frame};
  *S.reasonPtr(Kind) = chainToReason(*S.chainPtr(Kind), CalleeName);
}

void mergeChainPrepended(
    FunctionEffectSummary &Caller, int Kind, Function &CallerF,
    CallBase &CB, Function &Callee,
    const std::vector<RTProvenanceFrame> &CalleeChain) {
  if (*Caller.flagPtr(Kind))
    return;
  *Caller.flagPtr(Kind) = true;
  std::vector<RTProvenanceFrame> Chain;
  Chain.push_back(FunctionEffectSummary::makeFrame(
      CallerF, CB, Callee.getName(), "call"));
  Chain.insert(Chain.end(), CalleeChain.begin(), CalleeChain.end());
  *Caller.chainPtr(Kind) = std::move(Chain);
  *Caller.reasonPtr(Kind) =
      chainToReason(*Caller.chainPtr(Kind), Callee.getName());
}

// Attaches !rt.witness = {!"<kind>", ...} to an instruction so the
// placement pass can recover per-call-site witnesses.
void appendWitnessMetadata(Instruction &I, ArrayRef<int> Kinds) {
  if (Kinds.empty())
    return;
  LLVMContext &Ctx = I.getContext();
  std::set<std::string> Existing;
  if (MDNode *Prev = I.getMetadata("rt.witness")) {
    for (auto &Op : Prev->operands())
      if (auto *S = dyn_cast<MDString>(Op))
        Existing.insert(S->getString().str());
  }
  for (int K : Kinds)
    Existing.insert(FunctionEffectSummary::kindName(K));
  std::vector<Metadata *> Ops;
  for (const std::string &S : Existing)
    Ops.push_back(MDString::get(Ctx, S));
  I.setMetadata("rt.witness", MDNode::get(Ctx, Ops));
}

struct RegionInfo {
  bool UsesRegions = false;
  std::set<const Instruction *> InRegion;
};

bool functionUsesRegions(Function &F) {
  for (auto &BB : F) {
    for (auto &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        if (isRegionMarker(CB->getCalledFunction()))
          return true;
      }
    }
  }
  return false;
}

RegionInfo computeRegionInfo(Function &F) {
  RegionInfo R;
  R.UsesRegions = functionUsesRegions(F);
  if (!R.UsesRegions)
    return R;

  std::map<BasicBlock *, int> DepthIn, DepthOut;
  for (auto &BB : F) {
    DepthIn[&BB] = 0;
    DepthOut[&BB] = 0;
  }

  // Forward dataflow: depth-in is max over predecessors.
  bool Changed = true;
  while (Changed) {
    Changed = false;
    for (auto &BB : F) {
      int In = 0;
      bool IsEntry = (&BB == &F.getEntryBlock());
      if (!IsEntry) {
        bool Seen = false;
        for (BasicBlock *Pred : predecessors(&BB)) {
          int Po = DepthOut[Pred];
          if (!Seen || Po > In) {
            In = Po;
            Seen = true;
          }
        }
        if (!Seen)
          In = 0;
      }
      int D = In;
      for (auto &I : BB) {
        if (auto *CB = dyn_cast<CallBase>(&I)) {
          Function *Callee = CB->getCalledFunction();
          if (isRegionEnter(Callee)) {
            D++;
            continue;
          }
          if (isRegionExit(Callee)) {
            if (D > 0)
              D--;
            continue;
          }
        }
      }
      if (DepthIn[&BB] != In || DepthOut[&BB] != D) {
        DepthIn[&BB] = In;
        DepthOut[&BB] = D;
        Changed = true;
      }
    }
  }

  for (auto &BB : F) {
    int D = DepthIn[&BB];
    for (auto &I : BB) {
      if (auto *CB = dyn_cast<CallBase>(&I)) {
        Function *Callee = CB->getCalledFunction();
        if (isRegionEnter(Callee)) {
          D++;
          continue;
        }
        if (isRegionExit(Callee)) {
          if (D > 0)
            D--;
          continue;
        }
      }
      if (D > 0)
        R.InRegion.insert(&I);
    }
  }
  return R;
}

bool isCallTargetUse(const Use &U) {
  // If a function value is used as the called value of a CallBase, that's
  // not "address-taken".
  const User *Usr = U.getUser();
  if (auto *CB = dyn_cast<CallBase>(Usr))
    return CB->isCallee(&U);
  return false;
}

void collectAddressTaken(Module &M, std::vector<Function *> &Out) {
  for (Function &F : M) {
    if (F.isIntrinsic())
      continue;
    if (isRegionMarker(&F))
      continue;
    bool Taken = false;
    for (const Use &U : F.uses()) {
      if (!isCallTargetUse(U)) {
        Taken = true;
        break;
      }
    }
    if (Taken)
      Out.push_back(&F);
  }
}

std::vector<Function *>
resolveIndirectTargets(CallBase &CB, ArrayRef<Function *> AddressTaken) {
  std::vector<Function *> Out;
  FunctionType *FT = CB.getFunctionType();
  if (!FT)
    return Out;
  for (Function *F : AddressTaken) {
    if (F->getFunctionType() == FT)
      Out.push_back(F);
  }
  return Out;
}

// Returns true if the indirect call's called-value is one of the
// caller's function-typed Arguments. Sets ArgNoOut to that argument index.
bool isArgumentForwardCall(CallBase &CB, Function &Caller, unsigned &ArgNoOut) {
  Value *Called = CB.getCalledOperand();
  if (!Called)
    return false;
  if (auto *Arg = dyn_cast<Argument>(Called)) {
    if (Arg->getParent() != &Caller)
      return false;
    ArgNoOut = Arg->getArgNo();
    return true;
  }
  return false;
}

// A polymorphic site: the caller invokes an Argument of itself.
struct PolySite {
  unsigned ArgNo = 0;
  CallBase *CB = nullptr;
};

} // namespace

// === Main entry ===

PreservedAnalyses RTEffectInferPass::run(Module &M, ModuleAnalysisManager &AM) {
  const char *FilePath = getExternalFuncFile();
  errs() << "[RTEffectInferPass] Starting effect inference...\n";

  auto ExtTableOrErr = ExternalFuncTable::load(FilePath);
  if (!ExtTableOrErr)
    errs() << "[RTEffectInferPass] Warning: Could not load external func "
              "table from '"
           << FilePath << "'. Using conservative defaults.\n";
  ExternalFuncTable ExtTable =
      ExtTableOrErr ? *ExtTableOrErr : ExternalFuncTable();

  std::vector<Function *> AddressTaken;
  collectAddressTaken(M, AddressTaken);

  std::map<Function *, FunctionEffectSummary> DirectEffects;
  std::map<Function *, std::set<Function *>> CallGraph;
  std::map<Function *, std::set<Function *>> CallersOf;

  // For polymorphism resolution: caller-side polymorphic sites and
  // outgoing calls to polymorphic callees.
  std::map<Function *, std::vector<PolySite>> CallerPolySites;
  // For each caller, list the (callee, callsite, actual function args)
  // where callee is polymorphic — applied AFTER initial fixpoint.
  struct ResolvedPolyEdge {
    Function *Callee = nullptr;
    CallBase *CB = nullptr;
  };
  std::map<Function *, std::vector<ResolvedPolyEdge>> PolyOutEdges;

  // keep track of witness instructions per (function, kind) so
  // RTSanPlacementPass can find them later.
  std::map<Function *, std::vector<std::pair<Instruction *, int>>>
      DirectWitnesses;

  for (auto &F : M) {
    if (F.isDeclaration() || F.empty())
      continue;
    if (isRegionMarker(&F))
      continue;

    DirectEffects[&F] = FunctionEffectSummary();
    FunctionEffectSummary &Summary = DirectEffects[&F];
    CallGraph[&F] = {};
    CallersOf[&F] = {};

    RegionInfo Region = computeRegionInfo(F);
    auto inScope = [&](const Instruction *I) {
      return !Region.UsesRegions || Region.InRegion.count(I);
    };

    // scan for polymorphic sites first; if we find any
    // "Argument forwarding", remember them.
    std::vector<PolySite> Sites;
    bool HasNonPolyUnknown = false;

    for (auto &BB : F) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB || !inScope(&I))
          continue;

        // Track may_throw for invoke instructions (LLVM has marked them
        // as potentially-unwinding).
        if (isa<InvokeInst>(CB)) {
          // Only attribute may_throw if we cannot prove the callee is
          // nounwind. Direct-callee proven-nothrow is later refined.
          Function *Callee = CB->getCalledFunction();
          bool CalleeNoThrow = Callee && Callee->doesNotThrow();
          if (!CalleeNoThrow) {
            setEffect(Summary, RTE_Throw, F, I,
                      Callee ? Callee->getName() : "<indirect>",
                      "invoke");
            DirectWitnesses[&F].push_back({&I, RTE_Throw});
          }
        }

        if (CB->isIndirectCall()) {
          unsigned ArgNo = 0;
          if (isArgumentForwardCall(*CB, F, ArgNo)) {
            Sites.push_back({ArgNo, CB});
            continue; // will be resolved in polymorphism phase
          }

          auto Targets = resolveIndirectTargets(*CB, AddressTaken);
          if (!Targets.empty()) {
            for (Function *T : Targets) {
              CallGraph[&F].insert(T);
              CallersOf[T].insert(&F);
            }
            continue;
          }

          // Truly unknown.
          setEffect(Summary, RTE_Unknown, F, I, "<indirect call>",
                    "indirect-call");
          HasNonPolyUnknown = true;
          DirectWitnesses[&F].push_back({&I, RTE_Unknown});
          continue;
        }

        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;
        if (Callee->isIntrinsic())
          continue;
        if (isRegionMarker(Callee))
          continue;

        StringRef CalleeName = Callee->getName();

        if (auto *Info = ExtTable.lookup(CalleeName)) {
          std::vector<int> Witnesses;
          if (Info->MayBlock) {
            setEffect(Summary, RTE_Block, F, I, CalleeName, "external");
            Witnesses.push_back(RTE_Block);
          }
          if (Info->MayAlloc) {
            setEffect(Summary, RTE_Alloc, F, I, CalleeName, "external");
            Witnesses.push_back(RTE_Alloc);
          }
          if (Info->MayThrow) {
            setEffect(Summary, RTE_Throw, F, I, CalleeName, "external");
            Witnesses.push_back(RTE_Throw);
          }
          if (Info->MayLock) {
            setEffect(Summary, RTE_Lock, F, I, CalleeName, "external");
            Witnesses.push_back(RTE_Lock);
          }
          if (Info->MaySignalUnsafe) {
            setEffect(Summary, RTE_SignalUnsafe, F, I, CalleeName,
                      "external");
            Witnesses.push_back(RTE_SignalUnsafe);
          }
          for (int K : Witnesses)
            DirectWitnesses[&F].push_back({&I, K});
          continue;
        }

        if (Callee->empty()) {
          // Unknown external. May still be a known throw vehicle.
          if (isThrowVehicle(CalleeName)) {
            setEffect(Summary, RTE_Throw, F, I, CalleeName, "external");
            DirectWitnesses[&F].push_back({&I, RTE_Throw});
          } else {
            setEffect(Summary, RTE_Unknown, F, I, CalleeName,
                      "unknown-external");
            HasNonPolyUnknown = true;
            DirectWitnesses[&F].push_back({&I, RTE_Unknown});
          }
          continue;
        }

        // Internal direct call.
        CallGraph[&F].insert(Callee);
        CallersOf[Callee].insert(&F);
      }
    }

    if (!Sites.empty() && !HasNonPolyUnknown) {
      Summary.IsEffectPolymorphic = true;
      std::set<unsigned> Args;
      for (const PolySite &PS : Sites)
        Args.insert(PS.ArgNo);
      Summary.PolyArgs.assign(Args.begin(), Args.end());
      CallerPolySites[&F] = std::move(Sites);
    } else if (!Sites.empty()) {
      // Drop the assumption: keep Unknown behavior.
      for (const PolySite &PS : Sites) {
        setEffect(Summary, RTE_Unknown, F, *PS.CB,
                  "<argument-forward call>", "indirect-call");
        DirectWitnesses[&F].push_back({PS.CB, RTE_Unknown});
      }
    }
  }

  // SCC + recursion handling. Functions in non-trivial SCCs receive
  // MayRecurse=true and a "<recursive-scc>" provenance prefix.
  std::vector<std::vector<Function *>> SCCs;
  {
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
    for (auto &Pair : CallGraph)
      if (Index[Pair.first] == -1)
        StrongConnect(Pair.first);
  }

  auto isRecursiveSCC = [&](const std::vector<Function *> &SCC) {
    if (SCC.size() > 1)
      return true;
    if (SCC.empty())
      return false;
    auto It = CallGraph.find(SCC.front());
    return It != CallGraph.end() && It->second.count(SCC.front());
  };

  std::set<Function *> InRecursiveSCC;
  for (const auto &SCC : SCCs)
    if (isRecursiveSCC(SCC))
      for (Function *F : SCC)
        InRecursiveSCC.insert(F);

  for (Function *F : InRecursiveSCC) {
    auto It = DirectEffects.find(F);
    if (It == DirectEffects.end())
      continue;
    FunctionEffectSummary &S = It->second;
    if (!S.MayRecurse) {
      RTProvenanceFrame Frame;
      Frame.FunctionName = F->getName().str();
      Frame.CalleeName = "<recursive-scc>";
      Frame.Kind = "cycle";
      if (DISubprogram *SP = F->getSubprogram()) {
        Frame.File = SP->getFilename().str();
        Frame.Line = SP->getLine();
      }
      S.MayRecurse = true;
      S.RecurseChain = {Frame};
      S.ReasonRecurseFn = chainToReason(S.RecurseChain, "recursive-scc");
    }

    // Prefix every existing direct-effect chain with the cycle frame so
    // textual reasons make the recursion visible (e.g. "<recursive-scc>
    // -> malloc").
    RTProvenanceFrame CycleFrame;
    CycleFrame.FunctionName = F->getName().str();
    CycleFrame.CalleeName = "<recursive-scc>";
    CycleFrame.Kind = "cycle";
    if (DISubprogram *SP = F->getSubprogram()) {
      CycleFrame.File = SP->getFilename().str();
      CycleFrame.Line = SP->getLine();
    }
    for (int K = 0; K < RTE_Count; ++K) {
      if (K == RTE_Recurse)
        continue;
      auto *Chain = S.chainPtr(K);
      if (!Chain || Chain->empty())
        continue;
      if (Chain->front().Kind == "cycle")
        continue;
      Chain->insert(Chain->begin(), CycleFrame);
      *S.reasonPtr(K) =
          chainToReason(*Chain, *S.reasonPtr(K));
    }
  }

  // Worklist propagation: for each effect kind, propagate transitively
  // through the call graph.
  auto FuncSummaries = DirectEffects;

  std::queue<Function *> Worklist;
  for (auto &Pair : FuncSummaries)
    Worklist.push(Pair.first);

  while (!Worklist.empty()) {
    Function *F = Worklist.front();
    Worklist.pop();

    FunctionEffectSummary NewSummary = DirectEffects[F];

    // Walk callees and merge their summaries (per-kind chains with
    // call-frame prefix).
    auto CGIt = CallGraph.find(F);
    if (CGIt != CallGraph.end()) {
      for (auto &BB : *F) {
        for (auto &I : BB) {
          auto *CB = dyn_cast<CallBase>(&I);
          if (!CB)
            continue;
          if (CB->isIndirectCall())
            continue;
          Function *Callee = CB->getCalledFunction();
          if (!Callee || !CGIt->second.count(Callee))
            continue;

          auto It = FuncSummaries.find(Callee);
          if (It == FuncSummaries.end())
            continue;

          for (int K = 0; K < RTE_Count; ++K) {
            if (*It->second.flagPtr(K) && !*NewSummary.flagPtr(K)) {
              mergeChainPrepended(NewSummary, K, *F, *CB, *Callee,
                                  *It->second.chainPtr(K));
            }
          }
        }
      }

      // Indirect-call resolution edges: when we resolved an indirect
      // call to a set of address-taken targets, the call graph contains
      // the union, but the IR walk above only sees direct calls. Add
      // any callees that aren't reachable via direct walk.
      std::set<Function *> Seen;
      for (auto &BB : *F)
        for (auto &I : BB)
          if (auto *CB = dyn_cast<CallBase>(&I))
            if (!CB->isIndirectCall())
              if (Function *D = CB->getCalledFunction())
                Seen.insert(D);
      for (Function *C : CGIt->second) {
        if (Seen.count(C))
          continue;
        auto It = FuncSummaries.find(C);
        if (It == FuncSummaries.end())
          continue;
        for (int K = 0; K < RTE_Count; ++K) {
          if (*It->second.flagPtr(K) && !*NewSummary.flagPtr(K)) {
            *NewSummary.flagPtr(K) = true;
            std::vector<RTProvenanceFrame> Chain;
            RTProvenanceFrame Frame;
            Frame.FunctionName = F->getName().str();
            Frame.CalleeName = C->getName().str();
            Frame.Kind = "indirect-resolved";
            Chain.push_back(Frame);
            Chain.insert(Chain.end(),
                         It->second.chainPtr(K)->begin(),
                         It->second.chainPtr(K)->end());
            *NewSummary.chainPtr(K) = std::move(Chain);
            *NewSummary.reasonPtr(K) =
                chainToReason(*NewSummary.chainPtr(K), C->getName());
          }
        }
      }
    }

    auto &Old = FuncSummaries[F];
    bool Changed = false;
    for (int K = 0; K < RTE_Count; ++K) {
      if (*NewSummary.flagPtr(K) != *Old.flagPtr(K) ||
          *NewSummary.reasonPtr(K) != *Old.reasonPtr(K)) {
        Changed = true;
        break;
      }
    }
    if (Changed) {
      Old = NewSummary;
      auto It = CallersOf.find(F);
      if (It != CallersOf.end())
        for (Function *Caller : It->second)
          Worklist.push(Caller);
    }
  }

  auto resolvePolymorphism = [&](Function *Caller,
                                 FunctionEffectSummary &CallerSummary) {
    auto It = CallerPolySites.find(Caller);
    if (It == CallerPolySites.end())
      return false;
    bool Changed = false;
    for (PolySite &PS : It->second) {
      Value *V = PS.CB->getCalledOperand();
      if (auto *Arg = dyn_cast<Argument>(V)) {
        // Caller calls its own arg. Keep as poly-forward in caller's
        // summary; nothing to merge here.
        (void)Arg;
        continue;
      }
    }
    return Changed;
  };
  // Pass over callers of polymorphic functions: at each call site
  // calleeF(actualFn, ...), merge actualFn's effects into callerF if the
  // actual is a known function constant.
  for (auto &Pair : FuncSummaries) {
    Function *CallerF = Pair.first;
    FunctionEffectSummary &CallerSum = Pair.second;
    bool DirtyForCallers = false;
    for (auto &BB : *CallerF) {
      for (auto &I : BB) {
        auto *CB = dyn_cast<CallBase>(&I);
        if (!CB || CB->isIndirectCall())
          continue;
        Function *Callee = CB->getCalledFunction();
        if (!Callee)
          continue;
        auto SIt = FuncSummaries.find(Callee);
        if (SIt == FuncSummaries.end() || !SIt->second.IsEffectPolymorphic)
          continue;
        for (unsigned ArgNo : SIt->second.PolyArgs) {
          if (ArgNo >= CB->arg_size())
            continue;
          Value *Actual = CB->getArgOperand(ArgNo)->stripPointerCasts();
          auto *ActualF = dyn_cast<Function>(Actual);
          if (!ActualF)
            continue;
          auto AIt = FuncSummaries.find(ActualF);
          if (AIt == FuncSummaries.end())
            continue;
          for (int K = 0; K < RTE_Count; ++K) {
            if (*AIt->second.flagPtr(K) && !*CallerSum.flagPtr(K)) {
              std::vector<RTProvenanceFrame> Chain;
              RTProvenanceFrame Frame;
              Frame.FunctionName = CallerF->getName().str();
              Frame.CalleeName = ActualF->getName().str();
              Frame.Kind = "poly-resolved";
              Chain.push_back(Frame);
              Chain.insert(Chain.end(),
                           AIt->second.chainPtr(K)->begin(),
                           AIt->second.chainPtr(K)->end());
              *CallerSum.flagPtr(K) = true;
              *CallerSum.chainPtr(K) = std::move(Chain);
              *CallerSum.reasonPtr(K) =
                  chainToReason(*CallerSum.chainPtr(K), ActualF->getName());
              DirtyForCallers = true;
            }
          }
        }
      }
    }
    (void)DirtyForCallers;
    (void)resolvePolymorphism;
  }

  // Map each function to its SCC index; recursive SCC -> -1 sentinel.
  std::map<Function *, int> SCCIdx;
  std::vector<std::vector<Function *>> SCCNodes;
  for (size_t I = 0; I < SCCs.size(); ++I) {
    SCCNodes.push_back(SCCs[I]);
    for (Function *F : SCCs[I])
      SCCIdx[F] = static_cast<int>(I);
  }
  std::vector<bool> SCCRecursive(SCCs.size(), false);
  for (size_t I = 0; I < SCCs.size(); ++I)
    SCCRecursive[I] = isRecursiveSCC(SCCs[I]);
  // For each function: depth in its SCC = -1 if recursive, else 1 + max
  // over callees' depths in callee SCCs (callees in same SCC contribute 0
  // additional). We compute via topological order of SCCs.
  std::vector<int> SCCDepth(SCCs.size(), 1);
  // Build SCC-level edges.
  std::vector<std::set<int>> SCCEdges(SCCs.size());
  for (auto &Pair : CallGraph) {
    int From = SCCIdx.count(Pair.first) ? SCCIdx[Pair.first] : -1;
    if (From < 0)
      continue;
    for (Function *Callee : Pair.second) {
      if (!SCCIdx.count(Callee))
        continue;
      int To = SCCIdx[Callee];
      if (To != From)
        SCCEdges[From].insert(To);
    }
  }
  // Tarjan emits SCCs in reverse topological order; iterate in that
  // order so callees (later indices) are processed before callers.
  for (size_t I = 0; I < SCCs.size(); ++I) {
    if (SCCRecursive[I]) {
      SCCDepth[I] = -1;
      continue;
    }
    int Best = 0;
    bool Unbounded = false;
    for (int Succ : SCCEdges[I]) {
      if (SCCDepth[Succ] == -1) {
        Unbounded = true;
        break;
      }
      if (SCCDepth[Succ] > Best)
        Best = SCCDepth[Succ];
    }
    SCCDepth[I] = Unbounded ? -1 : (1 + Best);
  }
  // Propagate to per-function summaries (every function in the SCC
  // shares the SCC's depth — for non-recursive SCCs, that's a singleton).
  for (auto &Pair : FuncSummaries) {
    auto It = SCCIdx.find(Pair.first);
    if (It == SCCIdx.end()) {
      Pair.second.MaxStackDepth = 1;
    } else {
      Pair.second.MaxStackDepth = SCCDepth[It->second];
    }
    if (Pair.second.MayRecurse)
      Pair.second.MaxStackDepth = -1;
  }

  // Materialize witness metadata on instructions.
  for (auto &Pair : DirectWitnesses) {
    std::map<Instruction *, std::vector<int>> Grouped;
    for (auto &W : Pair.second)
      Grouped[W.first].push_back(W.second);
    for (auto &Entry : Grouped)
      appendWitnessMetadata(*Entry.first, Entry.second);
  }

  // Transitive witnesses: for any function with a flagged effect whose
  // chain points at an internal callee, mark the call instruction(s) in
  // the caller as witnesses so per-callsite instrumentation can wrap
  // exactly that site instead of the whole function.
  for (auto &Pair : FuncSummaries) {
    Function *F = Pair.first;
    FunctionEffectSummary &S = Pair.second;
    for (int K = 0; K < RTE_Count; ++K) {
      if (!*S.flagPtr(K))
        continue;
      const auto &Chain = *S.chainPtr(K);
      if (Chain.empty())
        continue;
      StringRef CalleeName = Chain.front().CalleeName;
      if (CalleeName.empty() || CalleeName.starts_with("<"))
        continue;
      for (auto &BB : *F) {
        for (auto &I : BB) {
          auto *CB = dyn_cast<CallBase>(&I);
          if (!CB || CB->isIndirectCall())
            continue;
          Function *Cal = CB->getCalledFunction();
          if (Cal && Cal->getName() == CalleeName)
            appendWitnessMetadata(*CB, {K});
        }
      }
    }
  }

  // Write summaries + emit a textual log line per function.
  for (auto &Pair : FuncSummaries) {
    FunctionEffectSummary::writeToMetadata(*Pair.first, Pair.second);
    auto &S = Pair.second;
    errs() << "  " << Pair.first->getName()
           << ": may_block=" << S.MayBlock
           << " may_alloc=" << S.MayAlloc
           << " may_throw=" << S.MayThrow
           << " may_lock=" << S.MayLock
           << " may_recurse=" << S.MayRecurse
           << " may_signal_unsafe=" << S.MaySignalUnsafe
           << " unknown=" << S.HasUnknown
           << " stack_depth=" << S.MaxStackDepth;
    if (S.IsEffectPolymorphic) {
      errs() << " poly_args=[";
      for (size_t I = 0; I < S.PolyArgs.size(); ++I) {
        if (I) errs() << ",";
        errs() << S.PolyArgs[I];
      }
      errs() << "]";
    }
    if (S.MayBlock)         errs() << " [via " << S.ReasonBlockFn << "]";
    if (S.MayAlloc)         errs() << " [via " << S.ReasonAllocFn << "]";
    if (S.MayThrow)         errs() << " [throw via " << S.ReasonThrowFn << "]";
    if (S.MayLock)          errs() << " [lock via " << S.ReasonLockFn << "]";
    if (S.MayRecurse)       errs() << " [recurse via " << S.ReasonRecurseFn << "]";
    if (S.MaySignalUnsafe)  errs() << " [sigunsafe via " << S.ReasonSignalUnsafeFn << "]";
    if (S.HasUnknown)       errs() << " [unknown via " << S.ReasonUnknown << "]";
    errs() << "\n";
  }

  return PreservedAnalyses::all();
}
