#!/usr/bin/env bash
set -euo pipefail

# Repeated wall-time benchmark for the audio target.
#
# Builds three binaries from the same scalable workload:
#   baseline       no RTSan hooks
#   instrument_all whole RT function wrapped
#   selective      only violating witness call sites wrapped
#
# Tunables:
#   HOT_ITERS=5000000 ROUNDS=9 scripts/bench_wall_time.sh
#   HOT_ITERS=1000000 RT_BENCH_FRAMES=128 RT_BENCH_DSP_PASSES=4 \
#     scripts/bench_wall_time.sh

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
OUT_DIR="${OUT_DIR:-$ROOT/build/wall_bench}"
TARGET="$ROOT/bench/audio_target/audio_target.cpp"
STUBS="$ROOT/bench/audio_target/audio_target_stubs.cpp"
SHIM="$ROOT/rtsan_runtime/rtsan_shim.c"
PLUGIN="$BUILD/libRTEffect.so"
YAML="$ROOT/external_funcs.yaml"

HOT_ITERS="${HOT_ITERS:-3000000}"
ROUNDS="${ROUNDS:-11}"
RT_BENCH_FRAMES="${RT_BENCH_FRAMES:-256}"
RT_BENCH_DSP_PASSES="${RT_BENCH_DSP_PASSES:-1}"

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

mkdir -p "$OUT_DIR"

SRC_BC="$OUT_DIR/audio_target.bc"
SHIM_O="$OUT_DIR/rtsan_shim.o"
STUBS_O="$OUT_DIR/audio_target_stubs.o"
ALL_LL="$OUT_DIR/instrument_all.ll"
SEL_LL="$OUT_DIR/selective.ll"

echo "[wall-bench] hot iterations: $HOT_ITERS"
echo "[wall-bench] rounds:         $ROUNDS"
echo "[wall-bench] frames:         $RT_BENCH_FRAMES"
echo "[wall-bench] dsp passes:     $RT_BENCH_DSP_PASSES"
echo "[wall-bench] output:         $OUT_DIR"

"$CLANG" -O2 -c "$SHIM" -o "$SHIM_O"
"$CLANGXX" -O2 -std=c++17 -c "$STUBS" -o "$STUBS_O"
"$CLANGXX" -std=c++17 -O2 -g -DRT_BENCH_HOT_ITERS="$HOT_ITERS" \
  -DRT_BENCH_FRAMES="$RT_BENCH_FRAMES" \
  -DRT_BENCH_DSP_PASSES="$RT_BENCH_DSP_PASSES" \
  -emit-llvm -c "$TARGET" -o "$SRC_BC"

RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-san-place-all" \
    -S "$SRC_BC" -o "$ALL_LL" 2>"$OUT_DIR/instrument_all.pass.log"

RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
    -S "$SRC_BC" -o "$SEL_LL" 2>"$OUT_DIR/selective.pass.log"

"$CLANGXX" -O2 -g "$SRC_BC" "$SHIM_O" "$STUBS_O" \
  -o "$OUT_DIR/baseline.bin"
"$CLANGXX" -O2 -g "$ALL_LL" "$SHIM_O" "$STUBS_O" \
  -o "$OUT_DIR/instrument_all.bin"
"$CLANGXX" -O2 -g "$SEL_LL" "$SHIM_O" "$STUBS_O" \
  -o "$OUT_DIR/selective.bin"

python3 - "$OUT_DIR" "$ROUNDS" "$HOT_ITERS" <<'PY'
import json
import os
import re
import statistics
import subprocess
import sys
import time

out_dir = sys.argv[1]
rounds = int(sys.argv[2])
hot_iters = int(sys.argv[3])
modes = ["baseline", "instrument_all", "selective"]

for mode in modes:
    subprocess.run(
        [os.path.join(out_dir, f"{mode}.bin")],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        check=False,
    )

rows = {}
for mode in modes:
    times = []
    enters = []
    exits = []
    for _ in range(rounds):
        start = time.perf_counter()
        proc = subprocess.run(
            [os.path.join(out_dir, f"{mode}.bin")],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
            check=False,
        )
        elapsed = time.perf_counter() - start
        times.append(elapsed)
        final = re.search(r"RTSAN_COUNTS final enter=(\d+) exit=(\d+)",
                          proc.stdout)
        if final:
            enters.append(int(final.group(1)))
            exits.append(int(final.group(2)))
        else:
            enters.append(None)
            exits.append(None)

    rows[mode] = {
        "times": times,
        "mean": statistics.mean(times),
        "median": statistics.median(times),
        "stdev": statistics.stdev(times) if len(times) > 1 else 0.0,
        "min": min(times),
        "max": max(times),
        "enter": enters[-1],
        "exit": exits[-1],
    }

baseline = rows["baseline"]["median"]
instrument_all = rows["instrument_all"]["median"]
selective = rows["selective"]["median"]

print(f"HOT_ITERS={hot_iters} ROUNDS={rounds}")
print("mode,median_s,mean_s,stdev_s,min_s,max_s,final_enter,final_exit")
for mode in modes:
    row = rows[mode]
    print(
        f"{mode},{row['median']:.6f},{row['mean']:.6f},"
        f"{row['stdev']:.6f},{row['min']:.6f},{row['max']:.6f},"
        f"{row['enter']},{row['exit']}"
    )

print(f"instrument_all_vs_baseline_overhead="
      f"{(instrument_all / baseline - 1) * 100:.2f}%")
print(f"selective_vs_baseline_overhead="
      f"{(selective / baseline - 1) * 100:.2f}%")
print(f"selective_vs_instrument_all_speedup="
      f"{(instrument_all / selective - 1) * 100:.2f}%")
print(f"selective_vs_instrument_all_time_reduction="
      f"{(1 - selective / instrument_all) * 100:.2f}%")
print("raw_json=" + json.dumps(rows, sort_keys=True))
PY
