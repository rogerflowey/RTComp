; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)
declare i32 @pthread_mutex_lock(ptr)

define void @rt_safe_math() #0 {
  %x = add i32 1, 2
  ret void
}
attributes #0 = { "nonblocking" "nonallocating" }

define void @rt_alloc_violation() #1 {
  %p = call ptr @malloc(i64 16)
  ret void
}
attributes #1 = { "nonallocating" }

define void @rt_block_violation(ptr %m) #2 {
  call i32 @pthread_mutex_lock(ptr %m)
  ret void
}
attributes #2 = { "nonblocking" }

define void @rt_both_violation(ptr %m) #3 {
  %p = call ptr @malloc(i64 16)
  call i32 @pthread_mutex_lock(ptr %m)
  ret void
}
attributes #3 = { "nonblocking" "nonallocating" }

; CHECK: SAFE: 'rt_safe_math'
; CHECK: VIOLATION in 'rt_alloc_violation': allocates
; CHECK: VIOLATION in 'rt_block_violation': blocks
; CHECK: VIOLATION in 'rt_both_violation'
