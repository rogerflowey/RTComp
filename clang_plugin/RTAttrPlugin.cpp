// RTAttrPlugin: a Clang plugin that exposes the `rt::*` C++11/C2x attribute
// namespace, lowering each attribute to an `annotate("rt_*")` so the
// existing RT-Effect IR pipeline can pick it up unchanged.
//
//   [[rt::nonblocking]]                -> annotate("rt_nonblocking")
//   [[rt::nonallocating]]              -> annotate("rt_nonallocating")
//   [[rt::nothrow]]                    -> annotate("rt_nothrow")
//   [[rt::nolock]]                     -> annotate("rt_nolock")
//   [[rt::norecurse]]                  -> annotate("rt_norecurse")
//   [[rt::async_signal_safe]]          -> annotate("rt_async_signal_safe")
//   [[rt::stack_bound(N)]]             -> annotate("rt_stack_bound=N")
//
// Usage:
//   clang -fplugin=./libRTAttrPlugin.so -c foo.cpp
//
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/AttributeCommonInfo.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Sema/ParsedAttr.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaDiagnostic.h"

using namespace clang;

namespace {

void attachAnnotation(Sema &S, Decl *D, llvm::StringRef Tag,
                      const ParsedAttr &Attr) {
  D->addAttr(AnnotateAttr::Create(S.Context, Tag.str(), nullptr, 0,
                                  Attr.getRange()));
}

bool requireFunction(Sema &S, const Decl *D, const ParsedAttr &Attr) {
  if (isa<FunctionDecl>(D))
    return true;
  S.Diag(Attr.getLoc(), diag::warn_attribute_wrong_decl_type_str)
      << Attr << Attr.isRegularKeywordAttribute() << "functions";
  return false;
}

#define SIMPLE_RT_ATTR(StructName, SpellStr, TagStr)                    \
  struct StructName : public ParsedAttrInfo {                            \
    StructName() {                                                       \
      OptArgs = 0;                                                       \
      NumArgs = 0;                                                       \
      static constexpr Spelling S[] = {                                  \
          {ParsedAttr::AS_CXX11, SpellStr},                              \
          {ParsedAttr::AS_C23, SpellStr},                                \
          {ParsedAttr::AS_GNU, SpellStr},                                \
      };                                                                 \
      Spellings = S;                                                     \
    }                                                                    \
    bool diagAppertainsToDecl(Sema &Sm, const ParsedAttr &Attr,          \
                              const Decl *D) const override {            \
      return requireFunction(Sm, D, Attr);                               \
    }                                                                    \
    AttrHandling handleDeclAttribute(Sema &Sm, Decl *D,                  \
                                     const ParsedAttr &Attr) const override { \
      attachAnnotation(Sm, D, TagStr, Attr);                             \
      return AttributeApplied;                                           \
    }                                                                    \
  };

SIMPLE_RT_ATTR(NonblockingAttrInfo,     "rt::nonblocking",     "rt_nonblocking")
SIMPLE_RT_ATTR(NonallocatingAttrInfo,   "rt::nonallocating",   "rt_nonallocating")
SIMPLE_RT_ATTR(NothrowAttrInfo,         "rt::nothrow",         "rt_nothrow")
SIMPLE_RT_ATTR(NolockAttrInfo,          "rt::nolock",          "rt_nolock")
SIMPLE_RT_ATTR(NorecurseAttrInfo,       "rt::norecurse",       "rt_norecurse")
SIMPLE_RT_ATTR(AsyncSignalSafeAttrInfo, "rt::async_signal_safe", "rt_async_signal_safe")

#undef SIMPLE_RT_ATTR

// stack_bound takes a single integer constant.
// Note: only the GNU spelling __attribute__((rt_stack_bound(N))) is
// supported because Clang's plugin attribute parser does not run the
// expression parser for C++11 attribute argument lists.
struct StackBoundAttrInfo : public ParsedAttrInfo {
  StackBoundAttrInfo() {
    NumArgs = 1;
    OptArgs = 0;
    HasCustomParsing = false;
    static constexpr Spelling S[] = {
        {ParsedAttr::AS_GNU, "rt_stack_bound"},
    };
    Spellings = S;
  }

  bool diagAppertainsToDecl(Sema &S, const ParsedAttr &Attr,
                            const Decl *D) const override {
    return requireFunction(S, D, Attr);
  }

  AttrHandling handleDeclAttribute(Sema &S, Decl *D,
                                   const ParsedAttr &Attr) const override {
    if (Attr.getNumArgs() != 1) {
      S.Diag(Attr.getLoc(), diag::err_attribute_wrong_number_arguments)
          << Attr << 1;
      return AttributeNotApplied;
    }
    Expr *E = Attr.getArgAsExpr(0);
    Expr::EvalResult Res;
    if (!E || !E->EvaluateAsInt(Res, S.Context)) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_type)
          << Attr << 1 << /* int */ 1;
      return AttributeNotApplied;
    }
    int64_t N = Res.Val.getInt().getSExtValue();
    if (N < 0) {
      S.Diag(Attr.getLoc(), diag::err_attribute_argument_n_type)
          << Attr << 1 << /* unsigned int */ 5;
      return AttributeNotApplied;
    }
    std::string Annot = std::string("rt_stack_bound=") + std::to_string(N);
    D->addAttr(AnnotateAttr::Create(S.Context, Annot, nullptr, 0,
                                    Attr.getRange()));
    return AttributeApplied;
  }
};

} // namespace

static ParsedAttrInfoRegistry::Add<NonblockingAttrInfo>
    A1("rt-nonblocking", "");
static ParsedAttrInfoRegistry::Add<NonallocatingAttrInfo>
    A2("rt-nonallocating", "");
static ParsedAttrInfoRegistry::Add<NothrowAttrInfo>
    A3("rt-nothrow", "");
static ParsedAttrInfoRegistry::Add<NolockAttrInfo>
    A4("rt-nolock", "");
static ParsedAttrInfoRegistry::Add<NorecurseAttrInfo>
    A5("rt-norecurse", "");
static ParsedAttrInfoRegistry::Add<AsyncSignalSafeAttrInfo>
    A6("rt-async-signal-safe", "");
static ParsedAttrInfoRegistry::Add<StackBoundAttrInfo>
    A7("rt-stack-bound", "");
