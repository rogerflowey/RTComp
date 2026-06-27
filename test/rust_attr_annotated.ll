; RUN: bash -c 'command -v cargo >/dev/null 2>&1 || exit 0' || exit 0
; RUN: bash -c 'command -v rustc >/dev/null 2>&1 || exit 0' || exit 0
;
; Rust end-to-end test for the `#[rt::*]` annotation pathway.
;
; Verifies that the `rt_attr` proc-macro emits `#[used] #[no_mangle]` marker
; globals (`__rt_annot_<fn>_<constraint>`), that RTEffectInferPass lifts them
; into LLVM fn attributes, and that RTConstraintCheckPass triggers the
; expected violation / SAFE diagnostics on pure-Rust code paths.

; Build proc-macro deps + crate, emit LLVM bitcode into the crate's deps dir.
; RUN: cargo build --release --manifest-path %S/rust_attr/Cargo.toml 2>/dev/null
; RUN: cargo rustc --release --manifest-path %S/rust_attr/Cargo.toml -- --emit=llvm-bc 2>/dev/null

; Resolve the produced bitcode (cargo assigns a hash suffix), run analysis.
; RUN: bash -c 'BC=$(ls -t %S/rust_attr/target/release/deps/rt_attr_smoke-*.bc 2>/dev/null | head -1); RTEFFECT_EXTERNAL_FUNC_FILE=%yaml_table %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -disable-output "$BC" 2>&1' | %FileCheck %s

; ── Marker lift (order depends on module-global iteration; use DAG) ──
; CHECK-DAG: Applied Rust annotation 'nonblocking' to 'rt_safe_compute'
; CHECK-DAG: Applied Rust annotation 'nonallocating' to 'rt_safe_compute'
; CHECK-DAG: Applied Rust annotation 'nonblocking' to 'rt_alloc_violate'
; CHECK-DAG: Applied Rust annotation 'nonallocating' to 'rt_alloc_violate'
; CHECK-DAG: Applied Rust annotation 'nonblocking' to 'rt_block_violate'
; CHECK-DAG: Applied Rust annotation 'nolock' to 'rt_lock_violate'
; CHECK-DAG: Applied Rust annotation 'nonblocking' to 'rt_lock_violate'
; CHECK-DAG: Applied Rust annotation 'nonblocking' to 'rt_bounded'
; CHECK-DAG: Applied Rust annotation 'stack_bound=2' to 'rt_bounded'

; ── Inference results on the Rust functions themselves (order varies) ──
; CHECK-DAG: rt_safe_compute: may_block=0 may_alloc=0{{.*}}unknown=0{{.*}}heap_kind=none
; CHECK-DAG: rt_alloc_violate: may_block=0 may_alloc=1{{.*}}heap_kind=normal_heap
; CHECK-DAG: rt_block_violate: may_block=1{{.*}}unknown=0{{.*}}heap_kind=global
; CHECK-DAG: rt_lock_violate: may_block=1{{.*}}may_lock=1
; CHECK-DAG: rt_bounded: may_block=0 may_alloc=0{{.*}}stack_depth=2

; ── Constraint diagnostics (Rust is now a first-class RT source) ──
; CHECK-DAG: [RT-FEA] nonallocating direct violation in rt_alloc_violate: may_alloc via malloc [heap: normal_heap]
; CHECK-DAG: [RT-FEA] nonblocking direct violation in rt_block_violate: may_block via printf
; CHECK-DAG: [RT-FEA] nonblocking direct violation in rt_lock_violate: may_block via pthread_mutex_lock
; CHECK-DAG: [RT-FEA] nolock direct violation in rt_lock_violate: may_lock via pthread_mutex_lock
; CHECK-DAG: [RT-FEA] SAFE: 'rt_safe_compute' passes all real-time constraints
; CHECK-DAG: [RT-FEA] SAFE: 'rt_bounded' passes all real-time constraints