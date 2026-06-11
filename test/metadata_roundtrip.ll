; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o - 2>&1 | %FileCheck %s

declare ptr @malloc(i64)

define void @leaf_alloc() {
  %p = call ptr @malloc(i64 16)
  ret void
}

define void @rt_entry() #0 {
  call void @leaf_alloc()
  ret void
}

attributes #0 = { "nonallocating" }

; CHECK: rt_entry: may_block=0 may_alloc=1 unknown=0 [via leaf_alloc -> malloc]
; CHECK: [RT-FEA] nonallocating transitive violation in rt_entry: may_alloc via leaf_alloc -> malloc
; CHECK: #0 rt_entry -> leaf_alloc [call]
; CHECK: #1 leaf_alloc -> malloc [external]
; CHECK: !rt.effect
; CHECK: leaf_alloc -> malloc
