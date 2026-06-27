; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check,rt-san-place" -S %s -o - 2>&1 | %FileCheck %s

; Regression test for the multi-witness fix: a single RT function that
; calls TWO different blocking helpers must wrap EACH contributing call
; site, not just the one whose chain happened to win setEffect. Before
; the fix, the per-callsite placement pass saw only one witness tag and
; silently dropped coverage of the other one — selective instrumentation
; under-counts violations accordingly.

declare void @sleep(i32) ; listed in external_funcs.yaml (may_block: true)

define void @helper_block_a() {
  call void @sleep(i32 1)
  ret void
}

define void @helper_block_b() {
  call void @sleep(i32 2)
  ret void
}

define void @multi_witness() #0 {
  call void @helper_block_a()
  call void @helper_block_b()
  ret void
}

attributes #0 = { "nonblocking" }

; The placement log reports BOTH contributing call sites, not just one.
; CHECK: Instrumenting (per-callsite x2): multi_witness

; The instrumentation inlines hooks around EACH call in original order.
; CHECK-LABEL: define void @multi_witness
; CHECK-NEXT: call void @__rtsan_realtime_enter()
; CHECK-NEXT: call void @helper_block_a()
; CHECK-NEXT: call void @__rtsan_realtime_exit()
; CHECK-NEXT: call void @__rtsan_realtime_enter()
; CHECK-NEXT: call void @helper_block_b()
; CHECK-NEXT: call void @__rtsan_realtime_exit()
; CHECK-NEXT: ret void