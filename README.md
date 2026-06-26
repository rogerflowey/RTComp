# RT-Effect IR

Realtime Function Effect Analysis and Selective RealtimeSanitizer Instrumentation on LLVM IR.

## Overview

An out-of-tree LLVM pass plugin, currently tested with LLVM 22, that:

1. **Infers** `may_block` / `may_alloc` / `may_throw` / `may_lock` / `may_recurse`
   / `may_signal_unsafe` / `unknown` effects on LLVM IR via inter-procedural
   fixpoint analysis, plus a bounded stack-depth via DAG longest-path.
2. **Classifies heap-kind** (`Stack` / `Global` / `RTHeap` / `NormalHeap`) for
   every memory operation and propagates the most restrictive kind through the
   call graph. RT-safe heap (`RTHeap`) allocations do **not** violate
   `nonallocating`, while `NormalHeap` allocations do.
3. **Tracks provenance** as call chains with instruction text and debug locations,
   stored in `!rt.effect`. Witness call instructions are tagged with `!rt.witness`.
4. **Resolves indirect calls** when callee types match an address-taken function in
   the module, and **infers effect-polymorphism** for callbacks that simply forward
   one of their function-typed parameters.
5. **Honors region scopes** (`__rt_region_enter` / `__rt_region_exit` markers) so an
   RT-region within a larger function can be analysed in isolation.
6. **Checks** real-time constraints on annotated functions, emitting text
   diagnostics, JSON Lines records, and SARIF 2.1.0 reports (including `heap_kind`).
7. **Guides** selective RTSan instrumentation: statically proven-safe RT functions
   skip hooks, while violating witnesses are wrapped per-call-site (or whole-function
   when the witness is opaque).
8. **Exports** inferred function effect summaries as YAML via
   `RTEFFECT_EXPORT_YAML`, producing a table compatible with
   `external_funcs.yaml` for cross-module analysis.
9. **Cross-language analysis** via Rust + C++ bitcode linking: compile both
   languages to `.bc` with `rustc --emit=llvm-bc` / `clang++ -emit-llvm`,
   merge with `llvm-link`, and run the full analysis pipeline on the
   combined module.

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

## Audio Target & Evaluation

`bench/audio_target/audio_target.cpp` is a realistic audio processing example
built around miniaudio (real-time callback) and dr_wav (WAV file I/O) APIs.
The audio callback is annotated `nonblocking` / `nonallocating` / `nolock` and
contains three intentional violations in a cold path:

| Violation | Call chain | Constraint |
|-----------|-----------|------------|
| `scratch_alloc_and_copy` → `malloc` | NormalHeap allocation | `nonallocating` |
| `scratch_alloc_and_copy` → `free` | Blocking deallocation | `nonblocking` |
| `locked_buffer_copy` → `pthread_mutex_lock` | Lock acquisition | `nolock` |

Safe hot-path helpers (`apply_gain`, `lowpass_filter`, `clamp`) are correctly
classified as ProvenSafe with `heap_kind=stack`.

### Demo (one command)

```bash
scripts/demo.sh
```

Displays annotated source, runs inference + constraint-check + selective
instrumentation, prints violation provenance chains, and compares
instrument-all vs selective hook counts.

### Evaluation (three-mode)

```bash
scripts/eval_audio.sh
```

Runs three modes and produces a Markdown results table:
- **check**: inference + constraint-check (violation count)
- **instrument-all**: every RT function wrapped (baseline overhead)
- **selective**: only violating call-sites wrapped (optimized)

## External Function Table

`external_funcs.yaml` maps known library functions to their real-time effects:

```yaml
- name: malloc
  may_block: false
  may_alloc: true
  heap_kind: normal_heap
- name: pthread_mutex_lock
  may_block: true
  may_alloc: false
- name: rt_malloc
  may_block: false
  may_alloc: true
  heap_kind: rtheap
```

The optional `heap_kind` field (one of `none`, `stack`, `global`, `rtheap`,
`normal_heap`) tells the constraint checker whether an allocation is RT-safe:
`nonallocating` functions that only reach `RTHeap` allocators (e.g. custom
pre-allocated pool allocators) are still ProvenSafe; only `NormalHeap`
allocations trigger a violation.

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
enum HeapKind { HK_None=0, HK_Stack, HK_Global, HK_RTHeap, HK_NormalHeap };

struct FunctionEffectSummary {
    bool MayBlock, MayAlloc, MayThrow, MayLock, MayRecurse, MaySignalUnsafe;
    bool HasUnknown;
    int  MaxStackDepth;          // -1 = unbounded
    HeapKind MayAllocHeapKind;   // where does the allocation come from?
    std::vector<RTProvenanceFrame> AllocHeapChain;
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
  diagnostics, unwind instrumentation, **heap-kind classification**
  (normal / stack / RT-safe / global / transitive / JSON `heap_kind`),
  **YAML export**, and **cross-language (Rust + C++) effect propagation**
- **Source-level tests** (`test/Inputs/*.c`): end-to-end from annotated C
  through Clang to pass pipeline, verified with FileCheck, including debug
  source-location chains

## Project Structure

```
rtcomp/
├── CMakeLists.txt              # Build configuration
├── external_funcs.yaml         # External function effect table (with heap_kind)
├── include/RTEffect/
│   ├── EffectSummary.h         # Summary data structure (HeapKind + EffectKind) + metadata I/O
│   ├── ExternalFuncTable.h     # YAML-based external function table (AllocHeap field)
│   └── Passes.h                # Pass declarations
├── lib/
│   ├── EffectSummary.cpp
│   ├── ExternalFuncTable.cpp
│   ├── RTEffectInferPass.cpp   # Effect inference, heap-kind detection & propagation, YAML export
│   ├── RTConstraintCheckPass.cpp # Constraint checking, heap policy, JSON + SARIF diagnostics
│   ├── RTSanPlacementPass.cpp  # Selective per-call-site RTSan instrumentation
│   └── RTEffectPlugin.cpp      # Pass plugin registration entry point
├── scripts/
│   ├── run_rt_effect.sh        # Build/run helper for analysis and RTSan modes
│   └── run_cross_lang.sh       # Rust + C++ joint bitcode analysis pipeline
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
    ├── cross_lang/             # Rust + C++ cross-language test inputs
    ├── *.ll                    # IR-level FileCheck tests (heap, export, cross-lang)
    └── Inputs/*.c              # Source-level end-to-end tests
```
