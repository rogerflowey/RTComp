// Test: Clang [[rt::...]] attribute plugin lowers to the same annotation
// that the analyzer already understands. Verify by piping through the
// effect inference pass and observing that constraint check fires.
//
// RUN: %clang -fplugin=%plugin_dir/libRTAttrPlugin.so -S -emit-llvm -O0 -x c++ %s -o /tmp/rt_attr_plugin_test.ll
// RUN: %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S /tmp/rt_attr_plugin_test.ll -o /dev/null 2>&1 | %FileCheck %s

#include <stdlib.h>

extern "C" {

[[rt::nonallocating]]
int rt_safe(int x) {
  return x * 2 + 1;
}

[[rt::nonallocating]]
void rt_violator() {
  void *p = malloc(16);
  (void)p;
}

__attribute__((rt_stack_bound(2)))
void rt_bounded() {
  // empty
}

}

// CHECK: SAFE: 'rt_safe'
// CHECK: [RT-FEA] nonallocating direct violation in rt_violator
// CHECK: SAFE: 'rt_bounded'
