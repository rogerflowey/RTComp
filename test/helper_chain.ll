; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)
declare i32 @pthread_mutex_lock(ptr)

define void @inner_alloc() {
  %p = call ptr @malloc(i64 16)
  ret void
}

define void @middle_alloc() {
  call void @inner_alloc()
  ret void
}

define void @outer_alloc() {
  call void @middle_alloc()
  ret void
}

define void @inner_lock(ptr %m) {
  call i32 @pthread_mutex_lock(ptr %m)
  ret void
}

define void @middle_lock(ptr %m) {
  call void @inner_lock(ptr %m)
  ret void
}

define void @outer_lock(ptr %m) {
  call void @middle_lock(ptr %m)
  ret void
}

define void @mixed_outer(ptr %m) {
  call void @inner_alloc()
  call void @inner_lock(ptr %m)
  ret void
}

; CHECK: outer_alloc: may_block=0 may_alloc=1
; CHECK: outer_lock: may_block=1 may_alloc=0
; CHECK: mixed_outer: may_block=1 may_alloc=1
