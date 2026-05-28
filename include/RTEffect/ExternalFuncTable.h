#ifndef RTEFFECT_EXTERNAL_FUNC_TABLE_H
#define RTEFFECT_EXTERNAL_FUNC_TABLE_H

#include "RTEffect/EffectSummary.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/ErrorOr.h"
#include <string>
#include <vector>

struct ExternalFuncInfo {
  bool MayBlock = false;
  bool MayAlloc = false;
};

class ExternalFuncTable {
  llvm::StringMap<ExternalFuncInfo> Table;

public:
  static llvm::ErrorOr<ExternalFuncTable>
  load(const std::string &FilePath);

  const ExternalFuncInfo *lookup(llvm::StringRef Name) const {
    auto It = Table.find(Name);
    if (It != Table.end())
      return &It->second;
    return nullptr;
  }
};

#endif
