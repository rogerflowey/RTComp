; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null 2>&1 | %FileCheck %s

@func_ptr = global ptr null

define void @rt_indirect() #0 {
  %fp = load ptr, ptr @func_ptr
  call void %fp()
  ret void
}

define void @rt_unknown_external() #0 {
  call void @some_unknown_library_function()
  ret void
}

declare void @some_unknown_library_function()

attributes #0 = { "nonblocking" }

; CHECK: rt_indirect: may_block=0 may_alloc=0 unknown=1
; CHECK: rt_unknown_external: may_block=0 may_alloc=0 unknown=1
; CHECK: [RT-FEA] nonblocking Unknown violation in rt_indirect: unknown-call via <indirect call>
; CHECK: #0 rt_indirect -> <indirect call> [indirect-call]
; CHECK: suggestion: Add the callee to external_funcs.yaml
; CHECK: [RT-FEA] nonblocking Unknown violation in rt_unknown_external: unknown-call via some_unknown_library_function
; CHECK: #0 rt_unknown_external -> some_unknown_library_function [unknown-external]
