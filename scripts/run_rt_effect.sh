#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
OUT_DIR="${OUT_DIR:-$BUILD_DIR/rt-effect-run}"
MODE="selective"
INPUT=""
RUN_LIT=0
USE_REAL_RTSAN=0
USE_SHIM=0
KEEP_TEMP=0

usage() {
  cat <<'USAGE'
Usage:
  scripts/run_rt_effect.sh [options] [input.c|input.cpp|input.ll|input.bc]

Options:
  --mode selective|all|check   Pipeline to run. Default: selective.
  --real-rtsan                 Link the instrumented IR with compiler-rt RTSan.
  --shim                       Link the local counting shim instead of real RTSan.
  --run-lit                    Run the lit regression suite after building.
  --build-dir DIR              CMake build directory. Default: build.
  --out-dir DIR                Output directory. Default: build/rt-effect-run.
  --keep-temp                  Keep generated smoke source when no input is given.
  -h, --help                   Show this help.

Environment overrides:
  CC, CXX, OPT, LIT, JQ, CMAKE, LLVM_CONFIG
  CFLAGS, CXXFLAGS, LDFLAGS

Examples:
  scripts/run_rt_effect.sh
  scripts/run_rt_effect.sh --mode check test/Inputs/rt_helper_chain.c
  scripts/run_rt_effect.sh --mode selective --real-rtsan src/audio_callback.cpp
  scripts/run_rt_effect.sh --mode all --shim test/Inputs/instrument_all_baseline.c
USAGE
}

die() {
  echo "error: $*" >&2
  exit 1
}

note() {
  echo "[rt-effect] $*"
}

need_tool() {
  local var_name="$1"
  local tool_name="$2"
  local current="${!var_name:-}"
  if [[ -n "$current" ]]; then
    command -v "$current" >/dev/null 2>&1 ||
      die "$var_name is set to '$current', but it is not executable"
    return
  fi
  current="$(command -v "$tool_name" || true)"
  [[ -n "$current" ]] || die "required tool '$tool_name' was not found"
  printf -v "$var_name" '%s' "$current"
}

while [[ $# -gt 0 ]]; do
  case "$1" in
  --mode)
    [[ $# -ge 2 ]] || die "--mode requires an argument"
    MODE="$2"
    shift 2
    ;;
  --mode=*)
    MODE="${1#*=}"
    shift
    ;;
  --real-rtsan)
    USE_REAL_RTSAN=1
    shift
    ;;
  --shim)
    USE_SHIM=1
    shift
    ;;
  --run-lit)
    RUN_LIT=1
    shift
    ;;
  --build-dir)
    [[ $# -ge 2 ]] || die "--build-dir requires an argument"
    BUILD_DIR="$2"
    shift 2
    ;;
  --build-dir=*)
    BUILD_DIR="${1#*=}"
    shift
    ;;
  --out-dir)
    [[ $# -ge 2 ]] || die "--out-dir requires an argument"
    OUT_DIR="$2"
    shift 2
    ;;
  --out-dir=*)
    OUT_DIR="${1#*=}"
    shift
    ;;
  --keep-temp)
    KEEP_TEMP=1
    shift
    ;;
  -h | --help)
    usage
    exit 0
    ;;
  -*)
    die "unknown option: $1"
    ;;
  *)
    [[ -z "$INPUT" ]] || die "only one input file is supported"
    INPUT="$1"
    shift
    ;;
  esac
done

case "$MODE" in
selective | all | check) ;;
*) die "--mode must be one of: selective, all, check" ;;
esac

if [[ "$USE_REAL_RTSAN" -eq 1 && "$USE_SHIM" -eq 1 ]]; then
  die "--real-rtsan and --shim cannot be used together"
fi

need_tool CMAKE cmake
need_tool LLVM_CONFIG llvm-config
need_tool OPT opt
need_tool CC clang
need_tool CXX clang++
if [[ "$RUN_LIT" -eq 1 ]]; then
  need_tool LIT lit
fi

mkdir -p "$BUILD_DIR" "$OUT_DIR"

PLUGIN="$BUILD_DIR/libRTEffect.so"
note "configuring and building plugin"
"$CMAKE" -S "$ROOT_DIR" -B "$BUILD_DIR" -DLLVM_DIR="$("$LLVM_CONFIG" --cmakedir)" >/dev/null
"$CMAKE" --build "$BUILD_DIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)"
[[ -f "$PLUGIN" ]] || die "plugin was not built at $PLUGIN"

if [[ "$RUN_LIT" -eq 1 ]]; then
  note "running lit regression suite"
  RTEFFECT_PLUGIN_DIR="$BUILD_DIR" "$LIT" -v -j1 "$ROOT_DIR/test"
fi

SMOKE_SOURCE=""
if [[ -z "$INPUT" ]]; then
  SMOKE_SOURCE="$OUT_DIR/rt_effect_smoke.c"
  INPUT="$SMOKE_SOURCE"
  cat >"$SMOKE_SOURCE" <<'EOF'
#include <stdlib.h>

__attribute__((annotate("rt_nonallocating")))
int rt_bad_alloc(int n) {
  int *p = (int *)malloc((unsigned long)n * sizeof(int));
  return p ? p[0] : -1;
}

__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
int rt_safe_compute(int x) {
  return x * 2 + 1;
}

int main(void) {
  return rt_safe_compute(3) == 7 ? 0 : 1;
}
EOF
  note "no input provided; generated smoke source at $SMOKE_SOURCE"
fi

BASE="$(basename "$INPUT")"
BASE="${BASE%.*}"
INPUT_ABS="$(cd "$(dirname "$INPUT")" && pwd)/$(basename "$INPUT")"
LINKED_BC="$OUT_DIR/$BASE.linked.bc"
DIAG_JSON="$OUT_DIR/$BASE.diagnostics.jsonl"
SELECTIVE_LL="$OUT_DIR/$BASE.selective.ll"
ALL_LL="$OUT_DIR/$BASE.instrument_all.ll"
CHECK_LL="$OUT_DIR/$BASE.checked.ll"

compile_source_to_bc() {
  local src="$1"
  local out="$2"
  case "$src" in
  *.c)
    "$CC" -O0 -g -emit-llvm -c ${CFLAGS:-} "$src" -o "$out"
    ;;
  *.cc | *.cpp | *.cxx | *.C)
    "$CXX" -O0 -g -emit-llvm -c ${CXXFLAGS:-} "$src" -o "$out"
    ;;
  *)
    die "unsupported source extension for '$src'"
    ;;
  esac
}

case "$INPUT_ABS" in
*.bc)
  cp "$INPUT_ABS" "$LINKED_BC"
  ;;
*.ll)
  "$OPT" -S "$INPUT_ABS" -o "$OUT_DIR/$BASE.normalized.ll"
  "$OPT" "$OUT_DIR/$BASE.normalized.ll" -o "$LINKED_BC"
  ;;
*.c | *.cc | *.cpp | *.cxx | *.C)
  compile_source_to_bc "$INPUT_ABS" "$LINKED_BC"
  ;;
*)
  die "input must be .c, .cpp, .cc, .cxx, .ll, or .bc"
  ;;
esac

run_opt_pipeline() {
  local passes="$1"
  local out="$2"
  note "running opt passes: $passes"
  RTEFFECT_EXTERNAL_FUNC_FILE="$ROOT_DIR/external_funcs.yaml" \
  RTEFFECT_JSON_DIAGNOSTICS="$DIAG_JSON" \
    "$OPT" -load-pass-plugin="$PLUGIN" -passes="$passes" -S "$LINKED_BC" -o "$out"
}

case "$MODE" in
check)
  run_opt_pipeline "rt-effect-infer,rt-constraint-check" "$CHECK_LL"
  FINAL_LL="$CHECK_LL"
  ;;
selective)
  run_opt_pipeline "rt-effect-infer,rt-constraint-check,rt-san-place" "$SELECTIVE_LL"
  FINAL_LL="$SELECTIVE_LL"
  ;;
all)
  run_opt_pipeline "rt-effect-infer,rt-san-place-all" "$ALL_LL"
  FINAL_LL="$ALL_LL"
  ;;
esac

if [[ -s "$DIAG_JSON" ]]; then
  note "JSON diagnostics: $DIAG_JSON"
  if command -v "${JQ:-jq}" >/dev/null 2>&1; then
    "${JQ:-jq}" . "$DIAG_JSON" >/dev/null
    note "JSON diagnostics parsed successfully with ${JQ:-jq}"
  fi
else
  note "no JSON diagnostics emitted"
fi

note "instrumented/check IR: $FINAL_LL"

if [[ "$USE_REAL_RTSAN" -eq 1 || "$USE_SHIM" -eq 1 ]]; then
  if [[ "$MODE" == "check" ]]; then
    die "linking requires --mode selective or --mode all"
  fi
  if ! "$OPT" -S "$FINAL_LL" -o - | grep -Eq '^define .* @main\('; then
    die "linking requires the input module to define main(); omit --real-rtsan/--shim or provide a complete program"
  fi

  OBJ="$OUT_DIR/$BASE.o"
  BIN="$OUT_DIR/$BASE"
  "$CC" -O0 -g -c "$FINAL_LL" -o "$OBJ"

  if [[ "$USE_REAL_RTSAN" -eq 1 ]]; then
    note "linking with real compiler-rt RTSan"
    "$CC" -O0 -g -fsanitize=realtime "$OBJ" ${LDFLAGS:-} -o "$BIN"
  else
    note "linking with local counting shim"
    SHIM_OBJ="$OUT_DIR/rtsan_shim.o"
    "$CC" -O2 -g -std=c11 -c "$ROOT_DIR/rtsan_runtime/rtsan_shim.c" -o "$SHIM_OBJ"
    "$CC" -O0 -g "$OBJ" "$SHIM_OBJ" ${LDFLAGS:-} -o "$BIN"
  fi
  note "linked binary: $BIN"
fi

if [[ "$KEEP_TEMP" -eq 0 && -n "$SMOKE_SOURCE" ]]; then
  rm -f "$SMOKE_SOURCE"
fi

note "done"
