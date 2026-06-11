; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)

define void @safe_callback() {
  ret void
}

define void @alloc_callback() {
  %p = call ptr @malloc(i64 8)
  ret void
}

; @apply forwards its function-typed argument; should be detected
; effect-polymorphic in argument 0.
define void @apply(ptr %fp) {
  call void %fp()
  ret void
}
; CHECK: apply: {{.*}}poly_args=[0]

; Calling @apply with a safe callback should not introduce any effect
; in the caller.
define void @use_safe() {
  call void @apply(ptr @safe_callback)
  ret void
}
; CHECK: use_safe: may_block=0 may_alloc=0{{.*}}unknown=0

; Calling @apply with the allocating callback should make the caller
; may_alloc=1 (poly-resolved), not unknown=1.
define void @use_alloc() {
  call void @apply(ptr @alloc_callback)
  ret void
}
; CHECK: use_alloc: may_block=0 may_alloc=1
; CHECK-NOT: use_alloc: {{.*}}unknown=1
