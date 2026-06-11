; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)

; This callee allocates: it should be discovered through the indirect-call
; resolver because its address escapes (stored in a global).
define void @leaf_alloc() {
  %p = call ptr @malloc(i64 8)
  ret void
}

@table = global ptr @leaf_alloc

define void @dispatch() {
  %fp = load ptr, ptr @table
  call void %fp()
  ret void
}

; @leaf_alloc is address-taken (used in @table, not as a call target)
; so the indirect call should resolve to it. The caller @dispatch should
; report may_alloc=1, not unknown=1.
; CHECK: leaf_alloc: {{.*}}may_alloc=1
; CHECK: dispatch: may_block=0 may_alloc=1
; CHECK-NOT: dispatch: {{.*}}unknown=1
