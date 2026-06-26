; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

; alloca produces stack-local memory -- no may_alloc flag, heap_kind=stack.
; CHECK: stack_user: may_block=0 may_alloc=0{{.*}}heap_kind=stack

define void @stack_user(i32 %n) {
  %buf = alloca i32, i32 10
  ret void
}

; no allocation at all
define void @no_alloc() {
  ret void
}
; CHECK: no_alloc: may_block=0 may_alloc=0{{.*}}may_throw=0{{.*}}heap_kind=none
