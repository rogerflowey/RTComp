; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null 2>&1 | %FileCheck %s

; rt_malloc is annotated heap_kind: rtheap in external_funcs.yaml.
; nonallocating constraint + RTHeap allocation -> No violation (RTHeap is RT-safe).
; CHECK: rt_heap_user: may_block=0 may_alloc=1{{.*}}heap_kind=rtheap{{.*}}[via rt_malloc]
; CHECK: SAFE: 'rt_heap_user'

declare ptr @rt_malloc(i64)

define void @rt_heap_user()
    #0 {
  %p = call ptr @rt_malloc(i64 64)
  ret void
}

attributes #0 = { "nonallocating" }
