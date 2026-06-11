; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null 2>&1 | %FileCheck %s

define void @leaf() {
  ret void
}

define void @middle() {
  call void @leaf()
  ret void
}

define void @outer() {
  call void @middle()
  ret void
}

; A bounded chain: outer -> middle -> leaf has depth 3.
; CHECK: outer: {{.*}}stack_depth=3

define void @recursive(i32 %n) {
  %cmp = icmp eq i32 %n, 0
  br i1 %cmp, label %done, label %rec
rec:
  %next = sub i32 %n, 1
  call void @recursive(i32 %next)
  br label %done
done:
  ret void
}
; CHECK: recursive: {{.*}}stack_depth=-1

define void @bounded_caller() #0 {
  call void @outer()
  ret void
}
attributes #0 = { "rt_stack_bound"="2" }
; CHECK: [RT-FEA] stackbound direct violation in bounded_caller: stack_depth via <stack_depth=4>
