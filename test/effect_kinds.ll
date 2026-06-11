; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare void @__cxa_throw(ptr, ptr, ptr)
declare ptr @__cxa_allocate_exception(i64)
declare i32 @pthread_mutex_lock(ptr)

define void @throws_directly() {
  %p = call ptr @__cxa_allocate_exception(i64 8)
  call void @__cxa_throw(ptr %p, ptr null, ptr null)
  unreachable
}
; CHECK: throws_directly: {{.*}}may_throw=1{{.*}}[throw via __cxa_allocate_exception]

define void @throw_via_invoke() personality ptr @__personality {
entry:
  invoke void @user_throw()
          to label %ok unwind label %lpad
ok:
  ret void
lpad:
  %lp = landingpad { ptr, i32 }
          cleanup
  resume { ptr, i32 } %lp
}
declare void @user_throw()
declare i32 @__personality(...)
; CHECK: throw_via_invoke: {{.*}}may_throw=1

define void @lock_then_unlock(ptr %m) {
  call i32 @pthread_mutex_lock(ptr %m)
  ret void
}
; CHECK: lock_then_unlock: {{.*}}may_lock=1{{.*}}[lock via pthread_mutex_lock]
