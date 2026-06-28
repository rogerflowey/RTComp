#!/usr/bin/env bash
set -euo pipefail

# Matrix benchmark for realistic audio callback configurations.
#
# Defaults keep runtime reasonable while covering common buffer sizes and
# light/heavy DSP loads:
#   FRAMES_LIST="64 128 256 512" DSP_PASSES_LIST="1 4"
#
# Example:
#   HOT_ITERS=1000000 ROUNDS=5 scripts/bench_audio_matrix.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
OUT_ROOT="${OUT_DIR:-$ROOT/build/audio_matrix}"
TARGET="$ROOT/bench/audio_target/audio_target.cpp"
STUBS="$ROOT/bench/audio_target/audio_target_stubs.cpp"
SHIM="$ROOT/rtsan_runtime/rtsan_shim.c"
PLUGIN="$BUILD/libRTEffect.so"
YAML="$ROOT/external_funcs.yaml"

HOT_ITERS="${HOT_ITERS:-1000000}"
ROUNDS="${ROUNDS:-5}"
SAMPLE_RATE="${SAMPLE_RATE:-48000}"
FRAMES_LIST="${FRAMES_LIST:-64 128 256 512}"
DSP_PASSES_LIST="${DSP_PASSES_LIST:-1 4}"

need_tool() {
  local var="$1" name="$2"
  local cur="${!var:-}"
  [[ -n "$cur" ]] && return
  cur="$(command -v "$name" || true)"
  [[ -n "$cur" ]] || { echo "error: $name not found" >&2; exit 1; }
  printf -v "$var" '%s' "$cur"
}

need_tool CLANG clang
need_tool CLANGXX clang++
need_tool OPT opt

if [[ ! -f "$PLUGIN" ]]; then
  echo "error: plugin not found at $PLUGIN; build with cmake first" >&2
  exit 1
fi

mkdir -p "$OUT_ROOT"

SHIM_O="$OUT_ROOT/rtsan_shim.o"
STUBS_O="$OUT_ROOT/audio_target_stubs.o"
"$CLANG" -O2 -c "$SHIM" -o "$SHIM_O"
"$CLANGXX" -O2 -std=c++17 -c "$STUBS" -o "$STUBS_O"

printf 'frames,dsp_passes,hot_iters,rounds,mode,median_s,mean_s,stdev_s,final_enter,final_exit,avg_us_per_callback,budget_ms_at_%s_hz,budget_pct\n' "$SAMPLE_RATE"

for frames in $FRAMES_LIST; do
  for passes in $DSP_PASSES_LIST; do
    CASE_DIR="$OUT_ROOT/f${frames}_p${passes}"
    mkdir -p "$CASE_DIR"
    SRC_BC="$CASE_DIR/audio_target.bc"
    ALL_LL="$CASE_DIR/instrument_all.ll"
    SEL_LL="$CASE_DIR/selective.ll"

    "$CLANGXX" -std=c++17 -O2 -g \
      -DRT_BENCH_HOT_ITERS="$HOT_ITERS" \
      -DRT_BENCH_FRAMES="$frames" \
      -DRT_BENCH_DSP_PASSES="$passes" \
      -emit-llvm -c "$TARGET" -o "$SRC_BC"

    RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
      "$OPT" -load-pass-plugin="$PLUGIN" \
        -passes="rt-effect-infer,rt-san-place-all" \
        -S "$SRC_BC" -o "$ALL_LL" 2>"$CASE_DIR/instrument_all.pass.log"

    RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
      "$OPT" -load-pass-plugin="$PLUGIN" \
        -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
        -S "$SRC_BC" -o "$SEL_LL" 2>"$CASE_DIR/selective.pass.log"

    "$CLANGXX" -O2 -g "$SRC_BC" "$SHIM_O" "$STUBS_O" \
      -o "$CASE_DIR/baseline.bin"
    "$CLANGXX" -O2 -g "$ALL_LL" "$SHIM_O" "$STUBS_O" \
      -o "$CASE_DIR/instrument_all.bin"
    "$CLANGXX" -O2 -g "$SEL_LL" "$SHIM_O" "$STUBS_O" \
      -o "$CASE_DIR/selective.bin"

    python3 - "$CASE_DIR" "$frames" "$passes" "$HOT_ITERS" "$ROUNDS" "$SAMPLE_RATE" <<'PY'
import os
import re
import statistics
import subprocess
import sys
import time

case_dir = sys.argv[1]
frames = int(sys.argv[2])
passes = int(sys.argv[3])
hot_iters = int(sys.argv[4])
rounds = int(sys.argv[5])
sample_rate = int(sys.argv[6])
modes = ["baseline", "instrument_all", "selective"]

for mode in modes:
    subprocess.run([os.path.join(case_dir, f"{mode}.bin")],
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   check=False)

budget_ms = frames / sample_rate * 1000.0

for mode in modes:
    times = []
    enters = []
    exits = []
    for _ in range(rounds):
        start = time.perf_counter()
        proc = subprocess.run([os.path.join(case_dir, f"{mode}.bin")],
                              stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE,
                              text=True,
                              check=False)
        elapsed = time.perf_counter() - start
        times.append(elapsed)
        final = re.search(r"RTSAN_COUNTS final enter=(\d+) exit=(\d+)",
                          proc.stdout)
        enters.append(int(final.group(1)) if final else -1)
        exits.append(int(final.group(2)) if final else -1)

    median = statistics.median(times)
    mean = statistics.mean(times)
    stdev = statistics.stdev(times) if len(times) > 1 else 0.0
    avg_us = median / hot_iters * 1_000_000.0
    budget_pct = (avg_us / 1000.0) / budget_ms * 100.0
    print(
        f"{frames},{passes},{hot_iters},{rounds},{mode},"
        f"{median:.6f},{mean:.6f},{stdev:.6f},"
        f"{enters[-1]},{exits[-1]},{avg_us:.6f},"
        f"{budget_ms:.6f},{budget_pct:.6f}",
        flush=True,
    )
PY
  done
done
