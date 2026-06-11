; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-san-place-all" -S %s -o - 2>&1 | %FileCheck %s

declare void @may_throw()
declare i32 @__gxx_personality_v0(...)

define void @rt_unwind() #0 personality ptr @__gxx_personality_v0 {
entry:
  invoke void @may_throw()
          to label %ok unwind label %lpad

ok:
  ret void

lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  resume { ptr, i32 } %lp
}

attributes #0 = { "nonblocking" }

; CHECK: Instrumenting (whole): rt_unwind
; CHECK-LABEL: define void @rt_unwind
; CHECK: call void @__rtsan_realtime_enter
; CHECK: ok:
; CHECK-NEXT: call void @__rtsan_realtime_exit
; CHECK-NEXT: ret void
; CHECK: lpad:
; CHECK: landingpad
; CHECK: call void @__rtsan_realtime_exit
; CHECK-NEXT: resume
