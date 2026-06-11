#include "RTEffect/ExternalFuncTable.h"
#include "RTEffect/EffectSummary.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ErrorOr.h"
#include <system_error>
#include <vector>

using namespace llvm;

LLVM_YAML_IS_SEQUENCE_VECTOR(ExternalFuncInfo)

namespace llvm {
namespace yaml {

template <> struct MappingTraits<ExternalFuncInfo> {
  static void mapping(IO &io, ExternalFuncInfo &E) {
    io.mapOptional("may_block", E.MayBlock, false);
    io.mapOptional("may_alloc", E.MayAlloc, false);
    io.mapOptional("may_throw", E.MayThrow, false);
    io.mapOptional("may_lock", E.MayLock, false);
    io.mapOptional("may_signal_unsafe", E.MaySignalUnsafe, false);
  }
};

} // namespace yaml
} // namespace llvm

namespace {

struct ExternalFuncYAMLEntry {
  std::string Name;
  ExternalFuncInfo Info;
};

} // namespace

LLVM_YAML_IS_SEQUENCE_VECTOR(ExternalFuncYAMLEntry)

namespace llvm {
namespace yaml {

template <> struct MappingTraits<ExternalFuncYAMLEntry> {
  static void mapping(IO &io, ExternalFuncYAMLEntry &E) {
    io.mapRequired("name", E.Name);
    io.mapOptional("may_block", E.Info.MayBlock, false);
    io.mapOptional("may_alloc", E.Info.MayAlloc, false);
    io.mapOptional("may_throw", E.Info.MayThrow, false);
    io.mapOptional("may_lock", E.Info.MayLock, false);
    io.mapOptional("may_signal_unsafe", E.Info.MaySignalUnsafe, false);
  }
};

} // namespace yaml
} // namespace llvm

ErrorOr<ExternalFuncTable>
ExternalFuncTable::load(const std::string &FilePath) {
  auto BufOrErr = MemoryBuffer::getFile(FilePath);
  if (!BufOrErr)
    return BufOrErr.getError();

  yaml::Input Yin((*BufOrErr)->getBuffer());

  std::vector<ExternalFuncYAMLEntry> Entries;
  Yin >> Entries;

  if (Yin.error())
    return make_error_code(std::errc::io_error);

  ExternalFuncTable Tbl;
  for (auto &E : Entries) {
    Tbl.Table[E.Name] = E.Info;
  }
  return Tbl;
}
