; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

; CHECK: heap_malloc: may_block=0 may_alloc=1{{.*}}heap_kind=normal_heap{{.*}}[via malloc]
; CHECK: heap_free: may_block=1 may_alloc=0{{.*}}heap_kind=none

declare ptr @malloc(i64)
declare void @free(ptr)

define void @heap_malloc() {
  %p = call ptr @malloc(i64 42)
  ret void
}

define void @heap_free() {
  call void @free(ptr null)
  ret void
}
