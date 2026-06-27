; RUN: %clang -std=c++17 -O0 -g -emit-llvm -c %S/../bench/audio_target/audio_target.cpp -o %t.bc
; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %t.bc -o /dev/null 2>&1 | %FileCheck %s

; Function summary order depends on clang's module function iteration,
; which can shift when helper functions are added to audio_target.cpp
; (e.g. rtsan count-printer for the runtime-hook experiment). Use
; CHECK-DAG so the audit remains order-independent.
; CHECK-DAG:      audio_callback: {{.*}}may_block=1{{.*}}may_alloc=1{{.*}}may_lock=1{{.*}}heap_kind=normal_heap
; CHECK-DAG:      {{.*}}apply_gain{{.*}}: {{.*}}may_block=0 may_alloc=0{{.*}}heap_kind=stack
; CHECK-DAG:      {{.*}}lowpass_filter{{.*}}: {{.*}}may_block=0 may_alloc=0{{.*}}heap_kind=stack
; CHECK-DAG:      {{.*}}clamp{{.*}}: {{.*}}may_block=0 may_alloc=0{{.*}}heap_kind=stack

; CHECK-DAG:      [RT-FEA] nonblocking {{.*}}violation in audio_callback: may_block
; CHECK-DAG:      [RT-FEA] nonallocating {{.*}}violation in audio_callback: may_alloc{{.*}}[heap: normal_heap]
; CHECK-DAG:      [RT-FEA] nolock {{.*}}violation in audio_callback: may_lock
; CHECK-NOT:  [RT-FEA] {{.*}}violation in{{.*}}apply_gain
; CHECK-NOT:  [RT-FEA] {{.*}}violation in{{.*}}lowpass_filter
; CHECK-NOT:  [RT-FEA] {{.*}}violation in{{.*}}clamp
