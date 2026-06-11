; RUN: env RTEFFECT_SARIF_DIAGNOSTICS=%t.sarif %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S %s -o /dev/null
; RUN: %jq -e '.version == "2.1.0" and .runs[0].tool.driver.name == "RTEffect"' %t.sarif > /dev/null
; RUN: %jq -e '.runs[0].results[0].ruleId == "RT-nonallocating"' %t.sarif > /dev/null
; RUN: %jq -e '.runs[0].results[0].codeFlows[0].threadFlows[0].locations | length >= 2' %t.sarif > /dev/null

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
