#!/usr/bin/env bash
set -euo pipefail

# Full audio target evaluation — links against the REAL miniaudio and
# dr_wav single-header libraries (fetched by
# scripts/fetch_audio_headers.sh) and runs the RTEffect pipeline over
# the actual library source.
#
# Uses a subset of miniaudio features (MA_NO_DEVICE_IO /
# MA_NO_DECODING / MA_NO_ENCODING / MA_NO_RESOURCE_MANAGER /
# MA_NO_NODE_GRAPH / MA_NO_ENGINE) for two reasons:
#   - We do not exercise any device I/O, decoding or encoding (the
#     benchmark only touches WAV read + DSP callbacks).
#   - The full miniaudio source at -O1 is ~1700 functions, which
#     causes our pass scanner (which is linear-in-IR with several
#     O(indirect_call × address_taken) sub-passes) to take several
#     minutes. With the subset above the module drops to ~800
#     functions and analysis completes in under a second. The
#     remaining coverage is still genuinely "real library source"
#     rather than opaque YAML stubs.

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
TARGET="$ROOT/bench/audio_target/audio_target_full.cpp"
VENDOR="$ROOT/bench/audio_target/vendor"
OUT_DIR="${OUT_DIR:-$ROOT/build/eval_audio_full}"
PLUGIN="$BUILD/libRTEffect.so"
YAML="$ROOT/external_funcs.yaml"
SHIM="$ROOT/rtsan_runtime/rtsan_shim.c"

need_tool() {
  local var="$1" name="$2"
  local cur="${!var:-}"
  [[ -n "$cur" ]] && return
  cur="$(command -v "$name" || true)"
  [[ -n "$cur" ]] || { echo "error: $name not found" >&2; exit 1; }
  printf -v "$var" '%s' "$cur"
}
need_tool CLANGXX clang++
need_tool OPT opt
need_tool CLANG clang

mkdir -p "$OUT_DIR"

# Make sure the headers actually exist — if not, instruct the user to
# run scripts/fetch_audio_headers.sh once.
for f in "$VENDOR"/miniaudio.h "$VENDOR"/dr_wav.h; do
  if [[ ! -s "$f" ]]; then
    echo "error: $f missing — run scripts/fetch_audio_headers.sh first." >&2
    exit 1
  fi
done

SRC_BC="$OUT_DIR/audio_target_full.bc"
SHIM_O="$OUT_DIR/rtsan_shim.o"
CHECK_LOG="$OUT_DIR/check.log"
DIS_LL="$OUT_DIR/audio_target_full.dis.ll"

# ── build ──────────────────────────────────────────────────────────
echo "[full] compiling shim..."
"$CLANG" -O2 -c "$SHIM" -o "$SHIM_O"

echo "[full] compiling real miniaudio + dr_wav (subset features)..."
MA_DEFS=(
  -DMA_NO_DEVICE_IO=1
  -DMA_NO_DECODING=1
  -DMA_NO_ENCODING=1
  -DMA_NO_RESOURCE_MANAGER=1
  -DMA_NO_NODE_GRAPH=1
  -DMA_NO_ENGINE=1
)
"$CLANGXX" -std=c++17 -O1 -g -emit-llvm -c -I"$ROOT/bench/audio_target" \
  "${MA_DEFS[@]}" "$TARGET" -o "$SRC_BC"

llvm-dis "$SRC_BC" -o "$DIS_LL"
FUNC_COUNT=$(grep -c '^define' "$DIS_LL")
echo "[full] module contains $FUNC_COUNT functions"

# ── analysis ──────────────────────────────────────────────────────
echo "[full] running rt-effect-infer + rt-constraint-check over theulled module..."
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-constraint-check" \
    -disable-output "$SRC_BC" 2>"$CHECK_LOG"

# ── summary table ─────────────────────────────────────────────────
VIOL_COUNT=$(grep -c '^\[RT-FEA\].*violation in' "$CHECK_LOG" || true)
SAFE_COUNT=$(grep -c '^\[RT-FEA\] SAFE' "$CHECK_LOG" || true)

# Real library coverage markers: count functions whose name suggests
# a known real lib API (drwav_* / ma_*) and which got transitive
# effects propagated (MayBlock or MayAlloc set); flip side is count
# of them being reported as SAFE under no constraint (none, since
# they are not rt-annotated).
REAL_FN_BLOCKING=$(rg -c '^  (drwav|ma)_.*: may_block=1' "$CHECK_LOG" || true)
REAL_FN_ALLOC=$(rg -c '^  (drwav|ma)_.*: .*may_alloc=1' "$CHECK_LOG" || true)
MINIAUDIO_IR_CALLS=$(rg 'call .*@ma_audio_buffer_(alloc_and_init|read_pcm_frames|uninit_and_free)' "$DIS_LL" || true)
MINIAUDIO_HIGHLIGHTS=$(grep -E '^  ma_audio_buffer_(alloc_and_init|uninit_and_free):|^  _ZL28exercise_miniaudio_init_pathv:' "$CHECK_LOG" || true)
MINIAUDIO_DSP_IR_CALLS=$(rg 'call .*@ma_gainer_process_pcm_frames' "$DIS_LL" || true)
MINIAUDIO_DSP_HIGHLIGHTS=$(grep -E '^  ma_gainer_(init|uninit|process_pcm_frames):|^  audio_callback: .*ma_gainer_process_pcm_frames' "$CHECK_LOG" || true)

cat <<TABLE

## Audio Target — FULL real-library evaluation

| Metric                            | Value                              |
|-----------------------------------|------------------------------------|
| Module functions (post -O1)       | ${FUNC_COUNT}                       |
| Real drwav/ma fns flagged may_block | ${REAL_FN_BLOCKING}             |
| Real drwav/ma fns flagged may_alloc | ${REAL_FN_ALLOC}             |
| Violations detected in audio_callback | ${VIOL_COUNT}                  |
| ProvenSafe functions (rt-annotated only) | ${SAFE_COUNT}               |

### Detected violations (must match the basic target's expected 3)

\`\`\`
$(grep '^\[RT-FEA\].*violation in' "$CHECK_LOG" || echo "(none)")
\`\`\`

### Real-lib inference highlights (top 5 drwav_init summaries)

\`\`\`
$(rg -m 5 '^  drwav_init.*may_block=1|^  drwav_init.*may_alloc=1' "$CHECK_LOG" || true)
\`\`\`

### Actual miniaudio init-path call chain

\`\`\`
${MINIAUDIO_HIGHLIGHTS:-(none)}
${MINIAUDIO_IR_CALLS:-(IR did not include direct miniaudio calls)}
\`\`\`

### Actual miniaudio realtime DSP call

\`\`\`
${MINIAUDIO_DSP_HIGHLIGHTS:-(none)}
${MINIAUDIO_DSP_IR_CALLS:-(IR did not include ma_gainer_process_pcm_frames)}
\`\`\`

Tight-set inspection log: \`$CHECK_LOG\`

TABLE

# pass/fail assertions: the same 3 RT violations expected by the stub
# target must be present even when real library code is in the module
PASS=true
for sig in \
  'nonblocking .*via .*scratch_alloc.*free' \
  'nonallocating .*via .*scratch_alloc.*malloc.*normal_heap' \
  'nolock .*via .*locked_buffer_copy.*pthread_mutex_lock'; do
  if ! rg -q "$sig" "$CHECK_LOG"; then
    echo "FAIL: expected violation $sig not found." >&2
    PASS=false
  fi
done
if [[ "$VIOL_COUNT" -ne 3 ]]; then
  echo "FAIL: violation_count=$VIOL_COUNT, expected 3." >&2
  PASS=false
fi
if [[ "${REAL_FN_BLOCKING:-0}" -lt 5 ]]; then
  echo "FAIL: REAL_FN_BLOCKING=${REAL_FN_BLOCKING:-0}, expected 5+ real-lib functions flagged blocking." >&2
  PASS=false
fi
if ! rg -q '@ma_audio_buffer_alloc_and_init' "$DIS_LL"; then
  echo "FAIL: full target IR did not call ma_audio_buffer_alloc_and_init." >&2
  PASS=false
fi
if ! rg -q '^  ma_audio_buffer_alloc_and_init: may_block=1 may_alloc=1' "$CHECK_LOG"; then
  echo "FAIL: miniaudio allocation API was not inferred as may_block/may_alloc." >&2
  PASS=false
fi
if ! rg -q '@ma_gainer_process_pcm_frames' "$DIS_LL"; then
  echo "FAIL: full target IR did not call ma_gainer_process_pcm_frames." >&2
  PASS=false
fi
if ! rg -q '^  ma_gainer_process_pcm_frames: may_block=0 may_alloc=0 .*unknown=0' "$CHECK_LOG"; then
  echo "FAIL: miniaudio realtime DSP API was not inferred as safe." >&2
  PASS=false
fi
if [[ "$PASS" != "true" ]]; then
  echo "FAIL: full eval assertions failed." >&2
  exit 1
fi
echo "PASS: full eval assertions all green."
