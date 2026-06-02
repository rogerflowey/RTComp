; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare i32 @pthread_mutex_lock(ptr)
declare i32 @pthread_mutex_unlock(ptr)
declare i32 @sleep(i32)
declare i32 @usleep(i32)
declare i64 @read(i32, ptr, i64)
declare i64 @write(i32, ptr, i64)

define void @direct_lock(ptr %m) {
  call i32 @pthread_mutex_lock(ptr %m)
  ret void
}
; CHECK: direct_lock: may_block=1 may_alloc=0 [via pthread_mutex_lock]

define void @direct_sleep() {
  call i32 @sleep(i32 1)
  ret void
}

define void @direct_io(i32 %fd, ptr %buf, i64 %n) {
  call i64 @read(i32 %fd, ptr %buf, i64 %n)
  ret void
}

define void @safe_loop() {
  br label %loop
loop:
  %i = phi i32 [ 0, %0 ], [ %next, %loop ]
  %next = add i32 %i, 1
  %cmp = icmp slt i32 %next, 10
  br i1 %cmp, label %loop, label %exit
exit:
  ret void
}
; CHECK: safe_loop: may_block=0 may_alloc=0
