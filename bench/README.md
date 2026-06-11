# RT-Effect benchmark

A minimal audio-DSP-style real-time loop used to compare instrumentation
strategies. Run it with:

```bash
cd ..
cmake -S . -B build -DLLVM_DIR="$(llvm-config --cmakedir)"
cmake --build build -j
bench/run_benchmark.sh
```

The driver compiles the workload to LLVM IR once and then re-runs four
pipelines:

| Mode              | Pipeline                                                              |
|-------------------|-----------------------------------------------------------------------|
| `baseline`        | no analysis, no hooks                                                 |
| `instrument_all`  | `rt-effect-infer,rt-san-place-all` (every RT function wrapped)        |
| `selective_whole` | `rt-effect-infer,rt-constraint-check,rt-san-place-whole`              |
| `selective`       | `rt-effect-infer,rt-constraint-check,rt-san-place`|

The workload's `audio_callback` is `nonallocating`. Most of the body is
proven safe; one cold-path call to `heavy_alloc()` is the only witness.
With `argc == 1` the cold path is never executed, so:

```
[baseline]         rtsan_enter=0       rtsan_exit=0
[instrument_all]   rtsan_enter=200000  rtsan_exit=200000
[selective_whole]  rtsan_enter=200000  rtsan_exit=200000
[selective]        rtsan_enter=0       rtsan_exit=0   <-- per-callsite wins
```

Selective per-callsite instrumentation pays zero hook overhead in the
common case while still catching the violation if the cold path is hit
(`bench/run_benchmark.sh` may be edited to invoke the binary with an
argument to exercise the witness).
