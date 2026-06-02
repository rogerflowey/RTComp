; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

@func_ptr = global ptr null

declare ptr @malloc(i64)

define void @indirect_call() {
  %fp = load ptr, ptr @func_ptr
  call void %fp()
  ret void
}
; CHECK: indirect_call: may_block=1 may_alloc=1
; CHECK: indirect call

define void @unknown_external() {
  call void @some_unknown_library_function()
  ret void
}
declare void @some_unknown_library_function()
; CHECK: unknown_external: may_block=1 may_alloc=1
