# RT-Effect IR

Realtime Function Effect Analysis and Selective RealtimeSanitizer Instrumentation on LLVM IR.

## Overview

An out-of-tree LLVM pass plugin, currently tested with LLVM 22, that:

1. **Infers** `may_block` / `may_alloc` / `unknown` effects on LLVM IR via inter-procedural fixpoint analysis
2. **Tracks provenance** as call chains with instruction text and debug locations, stored in `!rt.effect`
3. **Checks** real-time constraints on annotated functions, emitting text diagnostics and JSON Lines records
4. **Guides** selective RTSan instrumentation: statically proven-safe RT functions skip hooks, while violating or unknown RT functions are instrumented

## Build

Requires LLVM development libraries and tools. The test environment currently
uses LLVM 22. The code keeps a compatibility include for LLVM 18-style plugin
headers.

```bash
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
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

Scans call instructions for direct allocation/blocking operations, indirect
calls, and external declarations. It builds a call graph, runs a worklist
fixpoint to propagate summaries transitively, marks recursive SCC provenance,
and writes `FunctionEffectSummary` metadata on each function.

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

Reads function effect summaries and compares against real-time annotations.
Diagnostics distinguish direct violations, transitive violations, and Unknown
entries caused by indirect calls or missing external summaries. Each diagnostic
prints the call chain, per-frame source location when `!dbg` is available, the
leaf witness instruction, and a repair suggestion.

Supported IR/function markers are:
- `"nonblocking"` / `"nonallocating"` function attributes
- `__attribute__((annotate("rt_nonblocking")))`
- `__attribute__((annotate("rt_nonallocating")))`

Annotation fallback example:
```c
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
void my_rt_handler(void) { ... }
```

Set `RTEFFECT_JSON_DIAGNOSTICS` to emit one JSON object per diagnostic:

```bash
RTEFFECT_JSON_DIAGNOSTICS=diag.jsonl \
opt -load-pass-plugin=build/libRTEffect.so \
    -passes="rt-effect-infer,rt-constraint-check" \
    -disable-output input.ll
```

### Selective Instrumentation (default)

```bash
opt -load-pass-plugin=build/libRTEffect.so \
    -passes="rt-effect-infer,rt-constraint-check,rt-san-place" \
    -S input.ll
```

Inserts `__rtsan_realtime_enter` / `__rtsan_realtime_exit` hooks:
- **ProvenSafe** RT functions → **no hooks**, plus `!nosanitize_realtime`
- **Violating** or **Unknown** RT functions → **hooks inserted**
- Non-RT functions → skipped

Exit hooks are inserted before normal returns and unwind exits such as `resume`,
`cleanupret`, and `catchret`.

### Instrument-All Baseline

```bash
opt -load-pass-plugin=build/libRTEffect.so \
    -passes="rt-effect-infer,rt-san-place-all" \
    -S input.ll
```

Instruments **all RT functions** regardless of analysis results. Non-RT
functions remain untouched. Use this for overhead comparison against selective
mode.

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

Unknown external callees and indirect calls are tracked as `unknown`, not as
known `may_block` / `may_alloc` effects. Constraint checking reports them as
Unknown diagnostics and selective RTSan placement instruments the affected RT
functions.

## RTSan Shim

`rtsan_runtime/rtsan_shim.c` provides standalone hook definitions for tests and
experiments:

```c
void __rtsan_realtime_enter(const char *func_name);
void __rtsan_realtime_exit(const char *func_name);
uint64_t __rtsan_realtime_enter_count(void);
uint64_t __rtsan_realtime_exit_count(void);
void __rtsan_realtime_reset_counts(void);
```

## Architecture

```
[Annotated C/C++ source]
        │  Clang
        ▼
[LLVM IR with annotations]
        │
        ▼  RTEffectInferPass
[FunctionEffectSummary per function]
  { may_block, may_alloc, unknown, status, provenance chains }
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
    bool HasUnknown;
    enum Status { ProvenSafe=0, Violating=1, Unknown=2 } status;
    std::string ReasonBlockFn;
    std::string ReasonAllocFn;
    std::string ReasonUnknown;
    std::vector<RTProvenanceFrame> BlockChain;
    std::vector<RTProvenanceFrame> AllocChain;
    std::vector<RTProvenanceFrame> UnknownChain;
};
```

Summaries are stored as LLVM metadata (`!rt.effect`) on each function for
inter-pass communication.

## Testing

```bash
pip install lit
lit -v -j1 test/
```

The test suite includes:
- **IR-level tests** (`.ll`): direct allocation, blocking, helper chains,
  recursion/mutual recursion, indirect calls, constraint checks, selective
  and instrument-all modes, JSON diagnostics, metadata round-trip, Unknown
  diagnostics, and unwind instrumentation
- **Source-level tests** (`test/Inputs/*.c`): end-to-end from annotated C
  through Clang to pass pipeline, verified with FileCheck, including debug
  source-location chains

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
