; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-san-place-all" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)

define void @rt_safe() #0 {
  %x = add i32 1, 2
  ret void
}

define void @rt_unsafe() #1 {
  %p = call ptr @malloc(i64 16)
  ret void
}

define void @plain_func() {
  ret void
}

attributes #0 = { "nonblocking" "nonallocating" }
attributes #1 = { "nonallocating" }

; CHECK: instrument-all mode
; CHECK: Instrumenting: rt_safe
; CHECK: Instrumenting: rt_unsafe
; CHECK: Instrumenting: plain_func
; CHECK: Instrumented 3
