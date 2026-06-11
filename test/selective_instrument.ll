; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check,rt-san-place" -S %s -o - 2>&1 | %FileCheck %s

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
; CHECK: Instrumenting (per-callsite x1): rt_unsafe
; CHECK: define void @rt_safe() {{.*}}!nosanitize_realtime
; CHECK: call void @__rtsan_realtime_enter()
; CHECK: call void @__rtsan_realtime_exit()
; CHECK: declare void @__rtsan_realtime_enter()

define void @plain_func() {
  %p = call ptr @malloc(i64 16)
  ret void
}
; CHECK-NOT: Instrumenting{{.*}}plain_func
