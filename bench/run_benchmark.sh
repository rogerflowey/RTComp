#!/usr/bin/env bash
# Benchmark driver: compare three instrumentation modes against a
# baseline build of the same audio-DSP workload.
#
#   1. baseline       — no analysis pass, no hooks
#   2. instrument-all — every RT function wrapped (legacy)
#   3. selective      — only Violating witnesses wrapped
#
# Reports hook counts and wall-clock time for each.

set -eu

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BENCH="$ROOT/bench"
BUILD="$ROOT/build"

CLANG="${CLANG:-$(command -v clang || echo /opt/homebrew/opt/llvm/bin/clang)}"
CLANGXX="${CLANGXX:-$(command -v clang++ || echo /opt/homebrew/opt/llvm/bin/clang++)}"
OPT="${OPT:-$(command -v opt || echo /opt/homebrew/opt/llvm/bin/opt)}"
PLUGIN="${RTEFFECT_PLUGIN:-$BUILD/libRTEffect.so}"
SHIM="$ROOT/rtsan_runtime/rtsan_shim.c"
YAML="${RTEFFECT_EXTERNAL_FUNC_FILE:-$ROOT/external_funcs.yaml}"

if [[ ! -f "$PLUGIN" ]]; then
  echo "error: plugin not found at $PLUGIN; build with cmake first" >&2
  exit 1
fi

OUT="$BENCH/out"
mkdir -p "$OUT"

# 1) Compile shim once (used by all modes).
SHIM_O="$OUT/rtsan_shim.o"
"$CLANG" -O2 -c "$SHIM" -o "$SHIM_O"

# 2) Lower the workload to LLVM IR once.
SRC_LL="$OUT/audio_dsp.ll"
"$CLANGXX" -std=c++17 -O2 -S -emit-llvm "$BENCH/audio_dsp.cpp" -o "$SRC_LL"

run_mode() {
  local label="$1"
  local pipeline="$2"
  local outll="$OUT/$label.ll"
  local outbin="$OUT/$label.bin"
  local logfile="$OUT/$label.log"

  if [[ -z "$pipeline" ]]; then
    cp "$SRC_LL" "$outll"
    : > "$logfile"
  else
    RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
      "$OPT" -load-pass-plugin="$PLUGIN" -passes="$pipeline" \
        -S "$SRC_LL" -o "$outll" 2>"$logfile"
  fi

  "$CLANGXX" -O2 "$outll" "$SHIM_O" -o "$outbin"

  local instr_summary=""
  if [[ -s "$logfile" ]]; then
    instr_summary=$(grep -E 'Instrumented [0-9]+ function' "$logfile" || true)
  fi

  printf "[%s]\n" "$label"
  /usr/bin/time -p "$outbin" 2>&1 | sed 's/^/    /'
  if [[ -n "$instr_summary" ]]; then
    printf "    instr: %s\n" "$instr_summary"
  fi
}

echo "=== RT-Effect benchmark ==="
echo "plugin:   $PLUGIN"
echo "workload: $BENCH/audio_dsp.cpp"
echo

run_mode "baseline"        ""
run_mode "instrument_all"  "rt-effect-infer,rt-san-place-all"
run_mode "selective_whole" "rt-effect-infer,rt-constraint-check,rt-san-place-whole"
run_mode "selective"       "rt-effect-infer,rt-constraint-check,rt-san-place"

echo
echo "Logs and IR live under $OUT/"
