#!/usr/bin/env bash
set -euo pipefail

# RT-Effect Week 4 Demo: audio靶场分析演示
# 展示: 源码标注 → 违规检测 → 调用链溯源 → 选择性插桩

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD="${BUILD_DIR:-$ROOT/build}"
TARGET="$ROOT/bench/audio_target/audio_target.cpp"
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

SRC_BC="/tmp/rtcomp_demo.bc"
OUT_LL="/tmp/rtcomp_demo_selective.ll"
DIAG_JSON="/tmp/rtcomp_demo_diag.jsonl"

echo "══════════════════════════════════════════════════════"
echo "  RT-Effect 实时安全分析 — 音频靶场 Demo"
echo "══════════════════════════════════════════════════════"
echo

# 1. Show source annotations
echo "── 1. 标注的实时函数 ──"
grep -n 'rt_nonblocking\|rt_nonallocating\|rt_nolock\|audio_callback' "$TARGET" | head -8
echo

# 2. Compile + run analysis
echo "── 2. 编译并分析 ──"
"$CLANGXX" -std=c++17 -O0 -g -emit-llvm -c "$TARGET" -o "$SRC_BC"
echo "   源码 → LLVM bitcode 编译完成"
echo

# 3. Inference + constraint-check + selectve instrumentation
echo "── 3. Effect 推理 + 约束检查 + 选择性插桩 ──"
RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
RTEFFECT_JSON_DIAGNOSTICS="$DIAG_JSON" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
    -S "$SRC_BC" -o "$OUT_LL" 2>&1 | grep -E '\[RT-FEA\]|Instrumented|Skipping'
echo

# 4. Show violations with provenance chains
echo "── 4. 违规详情 (调用链溯源) ──"
cat "$DIAG_JSON" | python3 -c "
import json, sys
for line in sys.stdin:
    d = json.loads(line.strip())
    hk = d.get('heap_kind','')
    print(f\"  [{d['constraint']}] {d['function']} — {d['kind']}\")
    print(f\"    效果: {d['effect']}\" + (f' (heap: {hk})' if hk else ''))
    print(f\"    调用链:\")
    for i, f in enumerate(d['chain']):
        print(f\"      #{i} {f['function']} -> {f['callee']} [{f['kind']}]\")
    print(f\"    建议: {d['suggestion']}\")
"
echo

# 5. Show hook placement (compare)
echo "── 5. 插桩对比 ──"
ALL_HOOKS=$(RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
    -passes="rt-effect-infer,rt-san-place-all" \
    -S "$SRC_BC" -o - 2>/dev/null | grep -c 'call void @__rtsan_realtime_enter' || echo 0)
SEL_HOOKS=$(grep -c 'call void @__rtsan_realtime_enter' "$OUT_LL" || echo 0)
echo "   instrument-all: ${ALL_HOOKS} enter hooks"
echo "   selective:      ${SEL_HOOKS} enter hooks"
echo
echo "══════════════════════════════════════════════════════"
echo "  Demo 完成. 详细日志:\n    $DIAG_JSON"
echo "══════════════════════════════════════════════════════"
