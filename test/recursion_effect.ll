; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)

define void @recurse_alloc(i32 %n) {
  %cmp = icmp eq i32 %n, 0
  br i1 %cmp, label %done, label %recurse

recurse:
  %next = sub i32 %n, 1
  call void @recurse_alloc(i32 %next)
  %p = call ptr @malloc(i64 16)
  br label %done

done:
  ret void
}
; CHECK: recurse_alloc: may_block=0 may_alloc=1 [via malloc]

define void @mutual_a(i32 %n) {
  %cmp = icmp eq i32 %n, 0
  br i1 %cmp, label %call_malloc, label %call_b
call_malloc:
  %p = call ptr @malloc(i64 16)
  ret void
call_b:
  %next = sub i32 %n, 1
  call void @mutual_b(i32 %next)
  ret void
}

define void @mutual_b(i32 %n) {
  %next = sub i32 %n, 1
  call void @mutual_a(i32 %next)
  ret void
}
; CHECK: mutual_a: may_block=0 may_alloc=1
; CHECK: mutual_b: may_block=0 may_alloc=1
