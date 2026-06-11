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

; CHECK: [RT-FEA] SAFE: 'rt_safe_math'
; CHECK: [RT-FEA] nonallocating direct violation in rt_alloc_violation: may_alloc via malloc
; CHECK: #0 rt_alloc_violation -> malloc [external]
; CHECK: suggestion: Preallocate memory
; CHECK: [RT-FEA] nonblocking direct violation in rt_block_violation: may_block via pthread_mutex_lock
; CHECK: #0 rt_block_violation -> pthread_mutex_lock [external]
; CHECK: [RT-FEA] nonblocking direct violation in rt_both_violation
; CHECK: [RT-FEA] nonallocating direct violation in rt_both_violation
