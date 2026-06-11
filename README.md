# RT-Effect IR

Realtime Function Effect Analysis and Selective RealtimeSanitizer Instrumentation on LLVM IR.

## Overview

An out-of-tree LLVM pass plugin, currently tested with LLVM 22, that:

1. **Infers** `may_block` / `may_alloc` / `may_throw` / `may_lock` / `may_recurse`
   / `may_signal_unsafe` / `unknown` effects on LLVM IR via inter-procedural
   fixpoint analysis, plus a bounded stack-depth via DAG longest-path.
2. **Tracks provenance** as call chains with instruction text and debug locations,
   stored in `!rt.effect`. Witness call instructions are tagged with `!rt.witness`.
3. **Resolves indirect calls** when callee types match an address-taken function in
   the module, and **infers effect-polymorphism** for callbacks that simply forward
   one of their function-typed parameters.
4. **Honors region scopes** (`__rt_region_enter` / `__rt_region_exit` markers) so an
   RT-region within a larger function can be analysed in isolation.
5. **Checks** real-time constraints on annotated functions, emitting text
   diagnostics, JSON Lines records, and SARIF 2.1.0 reports.
6. **Guides** selective RTSan instrumentation: statically proven-safe RT functions
   skip hooks, while violating witnesses are wrapped per-call-site (or whole-function
   when the witness is opaque).

## Build

Requires LLVM development libraries and tools. The test environment currently
uses LLVM 22. The code keeps a compatibility include for LLVM 18-style plugin
headers.

```bash
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build -j$(nproc)
```

This produces `build/libRTEffect.so`.

## One-Command Runner

`scripts/run_rt_effect.sh` builds the plugin, compiles an input to LLVM IR when
needed, runs the RT-Effect pass pipeline, writes JSON diagnostics, and can link
the result with either the real compiler-rt RTSan runtime or the local counting
shim.

Run the built-in smoke example:

```bash
scripts/run_rt_effect.sh
```

Check a source file and emit diagnostics:

```bash
scripts/run_rt_effect.sh --mode check test/Inputs/rt_helper_chain.c
```

Run selective placement and link with real RTSan:

```bash
scripts/run_rt_effect.sh --mode selective --real-rtsan path/to/program.cpp
```

Run the instrument-all baseline with the local counting shim:

```bash
scripts/run_rt_effect.sh --mode all --shim path/to/program.c
```

Inputs may be `.c`, `.cc`, `.cpp`, `.cxx`, `.ll`, or `.bc`. Linking modes
require the input module to define `main`. Generated files are written under
`build/rt-effect-run` by default.

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
- `"nonblocking"` / `"nonallocating"` / `"nothrow"` / `"nolock"` /
  `"norecurse"` / `"async_signal_safe"` function attributes
- `"rt_stack_bound"="<N>"` function attribute (or `rt_stack_bound=<N>`
  annotation) to require `MaxStackDepth <= N`
- `__attribute__((annotate("rt_<constraint>")))` for the same names

With the bundled Clang plugin (`build/libRTAttrPlugin.so`), C++ code can
use the standard attribute syntax:

```cpp
[[rt::nonblocking]] void process();
[[rt::nonallocating]] void update();
[[rt::nothrow]] void notify();
[[rt::nolock]] void poll();
[[rt::norecurse]] void compute();
[[rt::async_signal_safe]] void on_signal(int);
__attribute__((rt_stack_bound(64))) void deep();
```

Load the plugin via `clang -fplugin=build/libRTAttrPlugin.so` (only the
GNU spelling is available for `rt_stack_bound` because Clang's plugin
infrastructure does not run the expression parser for C++11-attribute
argument lists).

Annotation fallback example:
```c
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
void my_rt_handler(void) { ... }
```

Set `RTEFFECT_JSON_DIAGNOSTICS` to emit one JSON object per diagnostic, or
`RTEFFECT_SARIF_DIAGNOSTICS` to emit a single SARIF 2.1.0 run that can be
uploaded to GitHub Code Scanning, opened in VS Code, or fed to any
standard static-analysis pipeline:

```bash
RTEFFECT_JSON_DIAGNOSTICS=diag.jsonl \
RTEFFECT_SARIF_DIAGNOSTICS=diag.sarif \
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
- **Violating** RT functions → **per-call-site hooks** around each
  `!rt.witness` call; falls back to whole-function wrapping when no
  local witness is available
- **Unknown** RT functions → whole-function hooks (we cannot pinpoint
  which call is problematic)
- Non-RT functions → skipped

Use `-passes="...,rt-san-place-whole"` to force the legacy whole-function
behaviour for comparison.

Exit hooks are inserted before normal returns and unwind exits such as `resume`,
`cleanupret`, and `catchret`.

The inserted hooks use the real compiler-rt RTSan ABI:

```llvm
declare void @__rtsan_realtime_enter()
declare void @__rtsan_realtime_exit()
```

To use the real RTSan runtime, compile/link the instrumented IR with Clang's
RTSan runtime enabled:

```bash
clang++ -O2 -g -c build/app.rtsan.ll -o build/app.rtsan.o
clang++ -O2 -g -fsanitize=realtime build/app.rtsan.o -o build/app.rtsan
```

For the selective pipeline, do not compile the original source files with
`-fsanitize=realtime`, because Clang will insert its own hooks before this pass
can decide which RT functions are proven safe. Use `-fsanitize=realtime` at the
final link/compile-from-instrumented-IR step to pull in compiler-rt RTSan.

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

`rtsan_runtime/rtsan_shim.c` is not a replacement for compiler-rt RTSan. It
provides standalone hook definitions for tests and hook-count experiments when
the real runtime is not linked:

```c
void __rtsan_realtime_enter(void);
void __rtsan_realtime_exit(void);
uint64_t __rtsan_realtime_enter_count(void);
uint64_t __rtsan_realtime_exit_count(void);
void __rtsan_realtime_reset_counts(void);
```

Do not link the shim and `-fsanitize=realtime` into the same binary; both define
the RTSan entry/exit hooks.

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
    bool MayThrow;
    bool MayLock;
    bool MayRecurse;
    bool MaySignalUnsafe;
    bool HasUnknown;
    int  MaxStackDepth;          // -1 = unbounded
    bool IsEffectPolymorphic;
    std::vector<unsigned> PolyArgs;
    enum Status { ProvenSafe=0, Violating=1, Unknown=2 } status;
    // Per-effect reason strings + provenance chains
    std::string ReasonBlockFn, ReasonAllocFn, ReasonThrowFn,
                ReasonLockFn,  ReasonRecurseFn, ReasonSignalUnsafeFn,
                ReasonUnknown;
    std::vector<RTProvenanceFrame> BlockChain, AllocChain,
        ThrowChain, LockChain, RecurseChain, SignalUnsafeChain,
        UnknownChain;
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
│   ├── RTEffectInferPass.cpp   # Effect inference (direct + fixpoint + polymorphism)
│   ├── RTConstraintCheckPass.cpp # Constraint checking, JSON + SARIF diagnostics
│   ├── RTSanPlacementPass.cpp  # Selective per-call-site RTSan instrumentation
│   └── RTEffectPlugin.cpp      # Pass plugin registration entry point
├── scripts/
│   └── run_rt_effect.sh        # Build/run helper for analysis and RTSan modes
├── clang_plugin/
│   ├── RTAttrPlugin.cpp        # [[rt::*]] attribute plugin
│   └── CMakeLists.txt
├── rtsan_runtime/
│   └── rtsan_shim.c            # Minimal RTSan-compatible runtime hooks
├── bench/                      # Hook-overhead benchmark harness
│   ├── audio_dsp.cpp
│   ├── run_benchmark.sh
│   └── README.md
└── test/
    ├── lit.cfg                 # lit test runner configuration
    ├── *.ll                    # IR-level FileCheck tests
    └── Inputs/*.c              # Source-level end-to-end tests
```
