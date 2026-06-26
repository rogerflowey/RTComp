; RUN: rm -f %t.export.yaml
; RUN: env RTEFFECT_EXPORT_YAML=%t.export.yaml %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer" -S %s -o /dev/null 2>&1 | %FileCheck %s --check-prefix=CHECK-INFER
; RUN: test -s %t.export.yaml
; RUN: cat %t.export.yaml | %FileCheck %s --check-prefix=CHECK-YAML

; CHECK-INFER: leaf_alloc: may_block=0 may_alloc=1{{.*}}heap_kind=normal_heap{{.*}}[via malloc]
; CHECK-INFER: safe_pure: may_block=0 may_alloc=0{{.*}}heap_kind=none

; CHECK-YAML: - name: leaf_alloc
; CHECK-YAML:   may_alloc: true
; CHECK-YAML:   heap_kind: normal_heap
; CHECK-YAML: - name: safe_pure

declare ptr @malloc(i64)

define void @leaf_alloc() {
  %p = call ptr @malloc(i64 42)
  ret void
}

define void @safe_pure(i32 %x) {
  %y = add i32 %x, 1
  ret void
}
