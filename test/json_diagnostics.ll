; RUN: env RTEFFECT_JSON_DIAGNOSTICS=%t.json %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null 2>&1 | %FileCheck %s
; RUN: %jq -e '.function == "rt_entry" and .kind == "transitive" and .constraint == "nonallocating" and .chain[1].callee == "malloc"' %t.json > /dev/null
; RUN: python3 %S/check_json_diagnostics.py %t.json

declare ptr @malloc(i64)

define void @leaf_alloc() {
  %p = call ptr @malloc(i64 16)
  ret void
}

define void @rt_entry() #0 {
  call void @leaf_alloc()
  ret void
}

attributes #0 = { "nonallocating" }

; CHECK: [RT-FEA] nonallocating transitive violation in rt_entry
