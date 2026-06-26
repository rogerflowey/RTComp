#!/usr/bin/env bash
set -euo pipefail

# Cross-language (Rust + C++) RT-Effect analysis pipeline.
# Compiles Rust and C++ to LLVM bitcode, links them, and runs analysis.

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT_DIR/build}"
TEST_DIR="$ROOT_DIR/test/cross_lang"
OUT_DIR="${OUT_DIR:-$ROOT_DIR/build/cross_lang_out}"

RUST_SRC="${1:-$TEST_DIR/rust_lib.rs}"
CPP_SRC="${2:-$TEST_DIR/cpp_rt_caller.cpp}"
MODE="${3:-check}"

need_tool() {
  local var_name="$1" tool_name="$2"
  local current="${!var_name:-}"
  [[ -n "$current" ]] && return
  current="$(command -v "$tool_name" || true)"
  [[ -n "$current" ]] || { echo "SKIP: $tool_name not found"; exit 0; }
  printf -v "$var_name" '%s' "$current"
}

need_tool RUSTC rustc
need_tool LLVM_LINK llvm-link
need_tool CLANGXX clang++
need_tool OPT opt

mkdir -p "$OUT_DIR"

RUST_BC="$OUT_DIR/rust_lib.bc"
CPP_BC="$OUT_DIR/cpp_caller.bc"
MERGED_BC="$OUT_DIR/combined.bc"

# Compile Rust
echo "[cross-lang] compiling Rust -> bitcode"
rustc --emit=llvm-bc --crate-type=lib -o "$RUST_BC" "$RUST_SRC"

# Compile C++
echo "[cross-lang] compiling C++ -> bitcode"
clang++ -O0 -g -emit-llvm -c "$CPP_SRC" -o "$CPP_BC"

# Link bitcode
echo "[cross-lang] linking Rust + C++ bitcode"
llvm-link -o "$MERGED_BC" "$RUST_BC" "$CPP_BC"

# Run analysis
echo "[cross-lang] running RT-Effect inference + constraint check"
PLUGIN="$BUILD_DIR/libRTEffect.so"
YAML="$ROOT_DIR/external_funcs.yaml"

RTEFFECT_EXTERNAL_FUNC_FILE="$YAML" \
  "$OPT" -load-pass-plugin="$PLUGIN" \
  -passes="rt-effect-infer,rt-constraint-check" \
  -S "$MERGED_BC" -o /dev/null 2>&1

echo "[cross-lang] done"
