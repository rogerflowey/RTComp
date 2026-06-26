; RUN: bash -c 'command -v rustc >/dev/null 2>&1 || exit 0' || exit 0
; RUN: bash -c 'command -v llvm-link >/dev/null 2>&1 || exit 0' || exit 0
;
; Compile Rust + C++ to bitcode, link, and run RT-Effect analysis.
; Verify cross-language effect propagation.
;
; RUN: rustc --emit=llvm-bc --crate-type=lib -o %t.rust.bc %S/cross_lang/rust_lib.rs
; RUN: %clang -O0 -g -emit-llvm -c %S/cross_lang/cpp_rt_caller.cpp -o %t.cpp.bc
; RUN: llvm-link -o %t.combined.bc %t.rust.bc %t.cpp.bc 2>/dev/null
; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %t.combined.bc -o /dev/null 2>&1 | %FileCheck %s

; CHECK: rust_ffi_malloc: {{.*}}may_alloc=1{{.*}}heap_kind=normal_heap
; CHECK: rust_safe_compute: {{.*}}may_alloc=0
; CHECK: rt_cpp_violate{{.*}}: {{.*}}may_alloc=1{{.*}}heap_kind=normal_heap{{.*}}via rust_ffi_malloc
; CHECK: nonallocating transitive violation in {{.*}}rt_cpp_violate
; CHECK-NOT: nonallocating {{.*}} violation in {{.*}}rt_cpp_safe
