// Test: real-time function that calls malloc (violation)
// RUN: %clang -S -emit-llvm -O0 %s -o /tmp/test_alloc_viol.ll && %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S /tmp/test_alloc_viol.ll -o /dev/null 2>&1 | %FileCheck %s

#include <stdlib.h>

__attribute__((annotate("rt_nonallocating")))
int rt_alloc_violation(int n) {
  int *p = (int *)malloc(n * sizeof(int));
  return p ? p[0] : -1;
}
// CHECK: nonallocating direct violation in rt_alloc_violation
// CHECK: malloc
