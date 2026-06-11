; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

; CHECK: may_block=0 may_alloc=1 unknown=0 [via malloc]
; CHECK: [via _Znwm]

declare ptr @malloc(i64)
declare ptr @_Znwm(i64)
declare ptr @_Znam(i64)
declare i32 @printf(ptr, ...)

define void @direct_malloc() {
  %ptr = call ptr @malloc(i64 42)
  ret void
}

define void @direct_new() {
  %ptr = call ptr @_Znwm(i64 32)
  ret void
}

define void @direct_printf() {
  call i32 (ptr, ...) @printf(ptr null)
  ret void
}

define void @safe_arithmetic(i32 %x) {
  %y = add i32 %x, 1
  ret void
}
; CHECK: safe_arithmetic: may_block=0 may_alloc=0
