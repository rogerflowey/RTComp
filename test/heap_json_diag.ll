; RUN: rm -f %t.json && env RTEFFECT_JSON_DIAGNOSTICS=%t.json %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null 2>&1 | %FileCheck %s --dump-input=fail
; RUN: %jq -e -s '.[0].heap_kind == "normal_heap"' %t.json
; RUN: %jq -e -s '.[0].function == "rt_entry"' %t.json

; CHECK: [RT-FEA] nonallocating transitive violation in rt_entry
; CHECK-NOT: [RT-FEA] nonallocating transitive violation in rt_rt_entry

declare ptr @malloc(i64)
declare ptr @rt_malloc(i64)

define void @leaf_alloc() {
  %p = call ptr @malloc(i64 42)
  ret void
}

define void @leaf_rt_alloc() {
  %p = call ptr @rt_malloc(i64 64)
  ret void
}

define void @rt_entry()
    #0 {
  call void @leaf_alloc()
  ret void
}

define void @rt_rt_entry()
    #0 {
  call void @leaf_rt_alloc()
  ret void
}

attributes #0 = { "nonallocating" }
