; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s

; Heap kind should propagate transitively through the call chain.
; leaf does malloc (NormalHeap) -> mid calls leaf -> top calls mid.
; top should inherit NormalHeap.

declare ptr @malloc(i64)

define void @leaf_heap() {
  %p = call ptr @malloc(i64 42)
  ret void
}
; CHECK: leaf_heap: may_block=0 may_alloc=1{{.*}}heap_kind=normal_heap{{.*}}[via malloc]

define void @mid_heap() {
  call void @leaf_heap()
  ret void
}
; CHECK: mid_heap: {{.*}}may_alloc=1{{.*}}heap_kind=normal_heap{{.*}}[via leaf_heap{{.*}}malloc]

define void @top_heap() {
  call void @mid_heap()
  ret void
}
; CHECK: top_heap: {{.*}}may_alloc=1{{.*}}heap_kind=normal_heap{{.*}}[via mid_heap{{.*}}leaf_heap{{.*}}malloc]

; Now the transitive case for rt_malloc (RTHeap).
declare ptr @rt_malloc(i64)

define void @leaf_rt() {
  %p = call ptr @rt_malloc(i64 64)
  ret void
}
; CHECK: leaf_rt: {{.*}}may_alloc=1{{.*}}heap_kind=rtheap{{.*}}[via rt_malloc]

define void @top_rt() {
  call void @leaf_rt()
  ret void
}
; CHECK: top_rt: {{.*}}may_alloc=1{{.*}}heap_kind=rtheap{{.*}}[via leaf_rt{{.*}}rt_malloc]
