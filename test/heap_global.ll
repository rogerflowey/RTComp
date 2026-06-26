; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

; Accessing a global variable should produce heap_kind=global.
; CHECK: global_writer: may_block=0 may_alloc=0{{.*}}heap_kind=global

@my_global = global i32 0

define void @global_writer() {
  store i32 42, ptr @my_global
  ret void
}

define void @global_reader() {
  %v = load i32, ptr @my_global
  ret void
}
; CHECK: global_reader: may_block=0 may_alloc=0{{.*}}heap_kind=global
