#!/usr/bin/env bash
set -euo pipefail

# Three-mode audio target evaluation.
#   1. check          — analyse + count violations (inference + constraint-check)
#   2. instrument-all — instrument every RT function
#   3. selective      — instrument only violating call sites
#
# Prints a Markdown results table to stdout.

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
TARGET="$ROOT/bench/audio_target/audio_target.cpp"
OUT_DIR="${OUT_DIR:-$ROOT/build/eval_audio}"
PLUGIN="$BUILD/libRTEffect.so"
YAML="$ROOT/external_funcs.yaml"

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

mkdir -p "$OUT_DIR"

SRC_BC="$OUT_DIR/audio_target.bc"
CHECK_LOG="$OUT_DIR/check.log"
JSON_DIAG="$OUT_DIR/diagnostics.jsonl"
ALL_LL="$OUT_DIR/instrument_all.ll"
SEL_LL="$OUT_DIR/selective.ll"

# ── 1. Compile to bitcode ──────────────────────────────────────────
echo "[eval] compiling audio target to bitcode..."
"$CLANGXX" -std=c++17 -O0 -g -emit-llvm -c "$TARGET" -o "$SRC_BC"

# ── 2. check mode ──────────────────────────────────────────────────
echo "[eval] check mode: inference + constraint-check..."
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
RTEFFECT_JSON_DIAGNOSTICS="$JSON_DIAG" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-constraint-check" \
    -S "$SRC_BC" -o /dev/null 2>"$CHECK_LOG"

VIOLATION_COUNT=$(grep -c '^\[RT-FEA\].*violation in' "$CHECK_LOG" || true)
UNKNOWN_COUNT=$(grep -c 'Unknown violation' "$CHECK_LOG" || true)
SAFE_COUNT=$(grep -c '^\[RT-FEA\] SAFE' "$CHECK_LOG" || true)

# Count hook insertions in instrument modes via log grep.
count_hooks() {
  local log="$1"
  grep -oP 'Instrumented \K[0-9]+' "$log" | head -1 || echo "0"
}

# ── 3. instrument-all mode ──────────────────────────────────────────
echo "[eval] instrument-all mode..."
ALL_LOG="$OUT_DIR/instrument_all.log"
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-san-place-all" \
    -S "$SRC_BC" -o "$ALL_LL" 2>"$ALL_LOG"
ALL_HOOKS=$(grep -c 'call void @__rtsan_realtime_enter' "$ALL_LL" || true)

# ── 4. selective mode ───────────────────────────────────────────────
echo "[eval] selective mode..."
SEL_LOG="$OUT_DIR/selective.log"
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
    -S "$SRC_BC" -o "$SEL_LL" 2>"$SEL_LOG"
SEL_HOOKS=$(grep -c 'call void @__rtsan_realtime_enter' "$SEL_LL" || true)

HOOK_REDUCTION="N/A"
if [[ "$ALL_HOOKS" -gt 0 ]]; then
  HOOK_REDUCTION=$(python3 -c \
    "print(f'{(1 - $SEL_HOOKS/$ALL_HOOKS)*100:.0f}%')")
fi

# ── 5. Results table ────────────────────────────────────────────────
cat <<TABLE

## Audio Target Evaluation Results

| Metric                          | Value                |
|---------------------------------|----------------------|
| Violations found                | ${VIOLATION_COUNT}   |
| Unknown entries                 | ${UNKNOWN_COUNT}     |
| ProvenSafe functions            | ${SAFE_COUNT}        |
| Instrument-all enter hooks      | ${ALL_HOOKS}         |
| Selective enter hooks           | ${SEL_HOOKS}         |
| Hook reduction                  | ${HOOK_REDUCTION}    |

### Detected violations

\`\`\`
$(grep '^\[RT-FEA\].*violation in' "$CHECK_LOG" || echo "(none)")
\`\`\`

### Safe functions

\`\`\`
$(grep '^\[RT-FEA\] SAFE' "$CHECK_LOG" || echo "(none)")
\`\`\`

Diagnostics JSON: \`$JSON_DIAG\`
Instrumented IR: \`$ALL_LL\` / \`$SEL_LL\`
TABLE
