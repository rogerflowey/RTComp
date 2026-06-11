; RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null 2>&1 | %FileCheck %s

declare ptr @malloc(i64)
declare void @__rt_region_enter()
declare void @__rt_region_exit()

; The malloc is OUTSIDE the RT region, so the function should be safe.
define void @region_excludes() #0 {
  call void @__rt_region_enter()
  call void @__rt_region_exit()
  %p = call ptr @malloc(i64 8)
  ret void
}
attributes #0 = { "nonallocating" }

; The malloc is INSIDE the RT region, so the function should violate.
define void @region_includes() #1 {
  call void @__rt_region_enter()
  %p = call ptr @malloc(i64 8)
  call void @__rt_region_exit()
  ret void
}
attributes #1 = { "nonallocating" }

; CHECK: region_excludes: may_block=0 may_alloc=0
; CHECK: region_includes: may_block=0 may_alloc=1
; CHECK: SAFE: 'region_excludes'
; CHECK: [RT-FEA] nonallocating direct violation in region_includes
