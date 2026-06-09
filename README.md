# RT-Effect IR

Realtime Function Effect Analysis and Selective RealtimeSanitizer Instrumentation on LLVM IR.

## Overview

An out-of-tree LLVM 18 pass plugin that:

1. **Infers** `may_block` / `may_alloc` effects on LLVM IR via inter-procedural fixpoint analysis
2. **Checks** real-time constraints on annotated functions, emitting diagnostics with witness call chains
3. **Guides** selective RTSan instrumentation — statically proven-safe RT functions skip hooks, reducing runtime overhead

## Build

Requires LLVM 18 development libraries.

```bash
cmake -S . -B build -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm
cmake --build build -j$(nproc)
```

This produces `build/libRTEffect.so`.

## Usage

The plugin registers three passes in the LLVM new pass manager pipeline:

### Effect Inference

```bash
opt -load-pass-plugin=build/libRTEffect.so \
    -passes="rt-effect-infer" \
    -disable-output input.ll
```

Scans all instructions for direct allocation/blocking calls, builds a call graph,
and runs a worklist fixpoint to propagate effects transitively. Writes
`FunctionEffectSummary` metadata on each function.

Set the external function YAML table path via environment variable:
```bash
RTEFFECT_EXTERNAL_FUNC_FILE=external_funcs.yaml
```

### Constraint Checking

```bash
opt -load-pass-plugin=build/libRTEffect.so \
    -passes="rt-effect-infer,rt-constraint-check" \
    -disable-output input.ll
```

Reads function effect summaries and compares against real-time annotations
(`__attribute__((annotate("rt_nonblocking")))` / `__attribute__((annotate("rt_nonallocating")))`).
Emits diagnostics with witness call chains for violations.

Function annotations supplement standard Clang attributes:
```c
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
void my_rt_handler(void) { ... }
```

### Selective Instrumentation (default)

```bash
opt -load-pass-plugin=build/libRTEffect.so \
    -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
    -S input.ll
```

Inserts `__rtsan_realtime_enter` / `__rtsan_realtime_exit` hooks:
- **ProvenSafe** RT functions → **no hooks** (skipped)
- **Violating** or **Unknown** RT functions → **hooks inserted**
- Non-RT functions → skipped

### Instrument-All Baseline

```bash
opt -load-pass-plugin=build/libRTEffect.so \
    -passes="rt-effect-infer,rt-san-place-all" \
    -S input.ll
```

Instruments **all** functions regardless of analysis results. Use for overhead
comparison against selective mode.

## External Function Table

`external_funcs.yaml` maps known library functions to their real-time effects:

```yaml
- name: malloc
  may_block: false
  may_alloc: true
- name: pthread_mutex_lock
  may_block: true
  may_alloc: false
- name: printf
  may_block: true
  may_alloc: false
```

Unknown external callees and indirect calls are treated conservatively
(`may_block=true`, `may_alloc=true`).

## Architecture

```
[Annotated C/C++ source]
        │  Clang
        ▼
[LLVM IR with annotations]
        │
        ▼  RTEffectInferPass
[FunctionEffectSummary per function]
  { may_block, may_alloc, status, reason_block, reason_alloc }
        │
        ▼  RTConstraintCheckPass
[Diagnostics for violations]
        │
        ▼  RTSanPlacementPass
[LLVM IR with selective RTSan hooks]
```

### Data Structures

```cpp
struct FunctionEffectSummary {
    bool MayBlock;
    bool MayAlloc;
    enum Status { ProvenSafe=0, Violating=1, Unknown=2 } status;
    std::string ReasonBlockFn;  // witness call chain
    std::string ReasonAllocFn;
};
```

Summaries are stored as LLVM metadata (`!rt.effect`) on each function for
inter-pass communication.

## Testing

```bash
pip install lit
RTEFFECT_PLUGIN_DIR=build lit -v test/
```

The test suite includes:
- **IR-level tests** (`.ll`): direct allocation, blocking, helper chains,
  recursion/mutual recursion, indirect calls, constraint checks, selective
  and instrument-all modes
- **Source-level tests** (`test/Inputs/*.c`): end-to-end from annotated C
  through Clang to pass pipeline, verified with FileCheck

## Project Structure

```
rtcomp/
├── CMakeLists.txt              # Build configuration
├── external_funcs.yaml         # External function effect table
├── include/RTEffect/
│   ├── EffectSummary.h         # Summary data structure + metadata I/O
│   ├── ExternalFuncTable.h     # YAML-based external function table
│   └── Passes.h                # Pass declarations
├── lib/
│   ├── EffectSummary.cpp
│   ├── ExternalFuncTable.cpp
│   ├── RTEffectInferPass.cpp   # Effect inference (direct scan + fixpoint)
│   ├── RTConstraintCheckPass.cpp # Constraint checking + diagnostics
│   ├── RTSanPlacementPass.cpp  # Selective RTSan instrumentation
│   └── RTEffectPlugin.cpp      # Pass plugin registration entry point
├── rtsan_runtime/
│   └── rtsan_shim.c            # Minimal RTSan-compatible runtime hooks
├── test/
│   ├── lit.cfg                 # lit test runner configuration
│   ├── *.ll                    # IR-level FileCheck tests
│   └── Inputs/*.c              # Source-level end-to-end tests

```
