; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check,rt-san-place" -S %s -o - 2>&1 | %FileCheck %s

declare ptr @malloc(i64)

define i32 @safe_helper() {
  ret i32 0
}

define void @rt_violator() #0 {
  %r = call i32 @safe_helper()
  %p = call ptr @malloc(i64 16)
  ret void
}

attributes #0 = { "nonallocating" }

; Per-call-site mode: only the malloc call site should be wrapped, not
; the whole function. We expect the enter hook immediately above the
; malloc (not before safe_helper) and the exit hook immediately after.
; CHECK: Instrumenting (per-callsite x1): rt_violator
; CHECK-LABEL: define void @rt_violator
; CHECK-NEXT: %r = call i32 @safe_helper()
; CHECK-NEXT: call void @__rtsan_realtime_enter()
; CHECK-NEXT: %p = call ptr @malloc
; CHECK-NEXT: call void @__rtsan_realtime_exit()
; CHECK-NEXT: ret void
