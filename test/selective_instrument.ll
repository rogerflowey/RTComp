; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check,rt-san-place" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)

define void @rt_safe() #0 {
  %x = add i32 1, 2
  ret void
}
attributes #0 = { "nonblocking" "nonallocating" }
; CHECK: Skipping (ProvenSafe RT): rt_safe

define void @rt_unsafe() #1 {
  %p = call ptr @malloc(i64 16)
  ret void
}
attributes #1 = { "nonallocating" }
; CHECK: Instrumenting: rt_unsafe

define void @plain_func() {
  %p = call ptr @malloc(i64 16)
  ret void
}
; CHECK-NOT: Instrumenting: plain_func
