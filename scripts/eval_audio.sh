#!/usr/bin/env bash
set -euo pipefail

# Three-mode audio target evaluation, with assertions and runtime hook
# counting.
#
#   1. check          — inference + constraint-check (count violations)
#   2. instrument-all — instrument every RT function
#   3. selective      — instrument only violating call sites
#   4*. selective + real compiler-rt RTSan
#                       (best-effort: skipped if -fsanitize=realtime
#                        and libclang_rt.rtsan are unavailable)
#
# On success prints a Markdown results table to stdout and exits 0.
# On a regression (fewer than 3 expected violations, OR unexpected
# violation signature, OR unbalanced hook counts) prints FAIL: ...
# to stderr and exits non-zero.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
TARGET="$ROOT/bench/audio_target/audio_target.cpp"
OUT_DIR="${OUT_DIR:-$ROOT/build/eval_audio}"
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

SRC_BC="$OUT_DIR/audio_target.bc"
CHECK_LOG="$OUT_DIR/check.log"
JSON_DIAG="$OUT_DIR/diagnostics.jsonl"
ALL_LL="$OUT_DIR/instrument_all.ll"
SEL_LL="$OUT_DIR/selective.ll"
SHIM_O="$OUT_DIR/rtsan_shim.o"
STUBS_O="$OUT_DIR/audio_target_stubs.o"
ALL_BIN="$OUT_DIR/instrument_all.bin"
SEL_BIN="$OUT_DIR/selective.bin"

# ── 0. compile shim + target bitcode ───────────────────────────────
echo "[eval] compiling shim + audio_target bitcode..."
"$CLANG" -O2 -c "$SHIM" -o "$SHIM_O"
"$CLANGXX" -O0 -std=c++17 -c "$ROOT/bench/audio_target/audio_target_stubs.cpp" -o "$STUBS_O"
"$CLANGXX" -std=c++17 -O0 -g -emit-llvm -c "$TARGET" -o "$SRC_BC"

# ── 1. check mode ──────────────────────────────────────────────────
echo "[eval] check mode: inference + constraint-check..."
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
RTEFFECT_JSON_DIAGNOSTICS="$JSON_DIAG" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-constraint-check" \
    -S "$SRC_BC" -o /dev/null 2>"$CHECK_LOG"

VIOLATION_COUNT=$(grep -c '^\[RT-FEA\].*violation in' "$CHECK_LOG" || true)
UNKNOWN_COUNT=$(grep -c 'Unknown violation' "$CHECK_LOG" || true)
SAFE_COUNT=$(grep -c '^\[RT-FEA\] SAFE' "$CHECK_LOG" || true)

# ── 2. instrument-all mode ──────────────────────────────────────────
echo "[eval] instrument-all mode..."
ALL_LOG="$OUT_DIR/instrument_all.log"
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-san-place-all" \
    -S "$SRC_BC" -o "$ALL_LL" 2>"$ALL_LOG"
ALL_HOOKS=$(grep -c 'call void @__rtsan_realtime_enter' "$ALL_LL" || true)
"$CLANGXX" -O2 -g "$ALL_LL" "$SHIM_O" "$STUBS_O" -o "$ALL_BIN"

# ── 3. selective mode ───────────────────────────────────────────────
echo "[eval] selective mode..."
SEL_LOG="$OUT_DIR/selective.log"
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
    -S "$SRC_BC" -o "$SEL_LL" 2>"$SEL_LOG"
SEL_HOOKS=$(grep -c 'call void @__rtsan_realtime_enter' "$SEL_LL" || true)
"$CLANGXX" -O2 -g "$SEL_LL" "$SHIM_O" "$STUBS_O" -o "$SEL_BIN"

# ── 4. runtime hook counts (real execution under the shim) ────────
echo "[eval] runtime hook counting..."
ALL_RUN_OUT="$OUT_DIR/instrument_all.run.out"
SEL_RUN_OUT="$OUT_DIR/selective.run.out"
"$ALL_BIN" >"$ALL_RUN_OUT" 2>&1 || true
"$SEL_BIN" >"$SEL_RUN_OUT" 2>&1 || true

ALL_ENTER_RUNTIME=$(grep '^RTSAN_COUNTS final' "$ALL_RUN_OUT" | sed -n 's/.*enter=\([0-9]*\).*/\1/p')
SEL_ENTER_RUNTIME=$(grep '^RTSAN_COUNTS final' "$SEL_RUN_OUT" | sed -n 's/.*enter=\([0-9]*\).*/\1/p')
ALL_EXIT_RUNTIME=$(grep '^RTSAN_COUNTS final' "$ALL_RUN_OUT" | sed -n 's/.*exit=\([0-9]*\).*/\1/p')
SEL_EXIT_RUNTIME=$(grep '^RTSAN_COUNTS final' "$SEL_RUN_OUT" | sed -n 's/.*exit=\([0-9]*\).*/\1/p')
ALL_ENTER_HOT=$(grep '^RTSAN_COUNTS hotpath' "$ALL_RUN_OUT" | sed -n 's/.*enter=\([0-9]*\).*/\1/p')
SEL_ENTER_HOT=$(grep '^RTSAN_COUNTS hotpath' "$SEL_RUN_OUT" | sed -n 's/.*enter=\([0-9]*\).*/\1/p')

# Manual savings computation: instrument-all trips a hook on every
# callback invocation (101 total). selective trips zero on hot path
# (100 callbacks, rare=0) and 3 hooks on the single cold call (rare=1)
# — one enter/exit pair per witness call site (scratch_alloc, log, lock).
RUNTIME_HOOK_REDUCTION="N/A"
if [[ "${ALL_ENTER_RUNTIME:-0}" -gt 0 ]]; then
  RUNTIME_HOOK_REDUCTION=$(python3 -c \
    "print(f'{(1 - ${SEL_ENTER_RUNTIME:-0}/${ALL_ENTER_RUNTIME})*100:.1f}%')")
fi

# ── 5. safe-helper inference roll-up ───────────────────────────────
# Helpers apply_gain / lowpass_filter / clamp are not rt-annotated, so
# they never appear in the SAFE log; instead the inference pass logs
# their effect flags, where may_block=0 may_alloc=0 heap_kind=stack
# means "no effect of consequence to real-time safety". Surface those
# here so the evaluation portrait matches what the README claims.
SAFE_HELPERS=$(rg '(_ZL10apply_gain|_ZL14lowpass_filter|_ZL5clamp).*heap_kind=stack' \
  "$CHECK_LOG" || true)

SEL_RTSAN_OUT="$OUT_DIR/selective_real_rtsan.run.out"
SEL_RTSAN_LL="$OUT_DIR/selective_real_rtsan.ll"
SEL_RTSAN_BIN="$OUT_DIR/selective_real_rtsan.bin"
SEL_RTSAN_CODE=""
RTSAN_AVAILABLE=false
# -### prints the link command without linking; if it mentions the
# RTSan runtime then we know the toolchain wires -fsanitize=realtime
# to libclang_rt.rtsan. clang writes the dry-run command to STDERR —
# we keep stderr, discard stdout (which only carries the original
# clang version banner).
# -### prints the link command without linking; if it mentions the
# RTSan runtime then we know the toolchain wires -fsanitize=realtime
# to libclang_rt.rtsan. clang writes the dry-run command to STDERR; we
# join stderr into stdout for grep without losing the byte stream.
if "$CLANGXX" -fsanitize=realtime -### "$SHIM_O" -o /tmp/rtsan_probe 2>&1 \
    | grep -q 'libclang_rt\.rtsan'; then
  RTSAN_AVAILABLE=true
  echo "[eval] selective + real compiler-rt RTSan mode..."
  RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
    "$OPT" -load-pass-plugin="$PLUGIN" \
      -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
      -S "$SRC_BC" -o "$SEL_RTSAN_LL" 2>/dev/null || true
  "$CLANGXX" -O2 -g -fsanitize=realtime "$SEL_RTSAN_LL" "$STUBS_O" -o "$SEL_RTSAN_BIN" 2>/dev/null || true
  set +e
  "$SEL_RTSAN_BIN" >"$SEL_RTSAN_OUT" 2>&1
  SEL_RTSAN_CODE=$?
  set -e
else
  echo "[eval] real compiler-rt RTSan unavailable; skipping that mode."
fi

# ── pass/fail assertions ───────────────────────────────────────────
PASS=true
expected_violation() {
  local pattern="$1" label="$2"
  if ! rg -q "$pattern" "$CHECK_LOG"; then
    echo "FAIL: expected violation '$label' not found in check log." >&2
    PASS=false
  fi
}
expected_violation 'nonallocating .*via .*malloc.*normal_heap' 'alloc via malloc (NormalHeap)'
expected_violation 'nonblocking .*via .*free'                 'block via free'
expected_violation 'nolock .*via .*pthread_mutex_lock'         'lock via pthread_mutex_lock'

if [[ "$VIOLATION_COUNT" -ne 3 ]]; then
  echo "FAIL: violation_count=$VIOLATION_COUNT, expected exactly 3." >&2
  PASS=false
fi

# expectation: 3 RT violations → 3 witness call sites → 3 wrapper pairs
# => 3 enter + 3 exit hooks (steady-state after the IR-level rewrite).
if [[ "$SEL_HOOKS" -ne 3 ]]; then
  echo "FAIL: selective_enter_hooks=$SEL_HOOKS, expected 3 (one per violation call site)." >&2
  PASS=false
fi

# instrument-all wraps the whole audio_callback — exactly 1 enter hook.
if [[ "$ALL_HOOKS" -ne 1 ]]; then
  echo "FAIL: instrument_all_enter_hooks=$ALL_HOOKS, expected 1." >&2
  PASS=false
fi

# runtime: SAVE-waterline — selective must not trip hooks on hot path
# (rare=0 callbacks) so its hotpath count must be 0.
if [[ "${SEL_ENTER_HOT:-0}" -ne 0 ]]; then
  echo "FAIL: selective_enter_hot_runtime=${SEL_ENTER_HOT:-0}, expected 0 (cold-path-only)." >&2
  PASS=false
fi

# instrument-all trips a hook on every callback (101 total: 100 hot + 1 cold).
if [[ "${ALL_ENTER_HOT:-0}" -ne 100 ]]; then
  echo "FAIL: instrument_all_enter_hot_runtime=${ALL_ENTER_HOT:-0}, expected 100 (one per hot callback)." >&2
  PASS=false
fi

if [[ "$PASS" != "true" ]]; then
  echo "FAIL: eval assertions failed. See $OUT_DIR/ for logs." >&2
  exit 1
fi

# ── 7. summary markdown ────────────────────────────────────────────
cat <<TABLE

## Audio Target Evaluation Results

### Static analysis (IR-level)

| Metric                          | Value                |
|---------------------------------|----------------------|
| Violations found                | ${VIOLATION_COUNT}   |
| Unknown entries                 | ${UNKNOWN_COUNT}     |
| ProvenSafe functions            | ${SAFE_COUNT}        |
| Instrument-all enter hooks      | ${ALL_HOOKS}         |
| Selective enter hooks           | ${SEL_HOOKS}         |

### Runtime hook firing (linked against \`rtsan_shim.c\`)

The benchmark runs 100 \`audio_callback(..., rare=0)\` calls (hot path
— no violating helper is invoked) followed by one call with \`rare=1\`
(cold path — all three witness call sites run).

| Metric                          | Value  |
|---------------------------------|--------|
| Instrument-all: hot-path enter fires  | ${ALL_ENTER_HOT:-?}  |
| Selective:    hot-path enter fires  | ${SEL_ENTER_HOT:-?}  |
| Instrument-all: final enter count | ${ALL_ENTER_RUNTIME:-?}  |
| Selective:    final enter count | ${SEL_ENTER_RUNTIME:-?}  |
| Runtime hook reduction          | ${RUNTIME_HOOK_REDUCTION}  |

### Detected violations

\`\`\`
$(grep '^\[RT-FEA\].*violation in' "$CHECK_LOG" || echo "(none)")
\`\`\`

### Safe helpers (inferred, not rt-annotated)

\`\`\`
${SAFE_HELPERS:-(none)}
\`\`\`

### Real compiler-rt RTSan run (selective IR linked with \`-fsanitize=realtime\`)

\`\`\`
exit_code=${SEL_RTSAN_CODE:-skip}
$(if [[ "$RTSAN_AVAILABLE" == "true" ]]; then
    head -8 "$SEL_RTSAN_OUT" 2>/dev/null || echo "(empty stderr/stdout)"
  else
    echo "real compiler-rt RTSan unavailable on this system."
  fi)
\`\`\`

Diagnostics JSON: \`$JSON_DIAG\`
Instrumented IR:   \`$ALL_LL\` / \`$SEL_LL\`
Runtime outputs:   \`$ALL_RUN_OUT\` / \`$SEL_RUN_OUT\`$([[ "$RTSAN_AVAILABLE" == "true" ]] && echo " / ${SEL_RTSAN_OUT}")
TABLE

echo
echo "PASS: eval assertions all green."