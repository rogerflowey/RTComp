// Test: real-time function with helper chain
// RUN: %clang -S -emit-llvm -O0 %s -o /tmp/test_hlp_chain.ll && %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S /tmp/test_hlp_chain.ll -o /dev/null 2>&1 | %FileCheck %s

#include <stdlib.h>

void inner_alloc() {
  malloc(16);
}

void middle_alloc() {
  inner_alloc();
}

__attribute__((annotate("rt_nonallocating")))
void rt_outer() {
  middle_alloc();
}
// CHECK: VIOLATION in 'rt_outer'
// CHECK: inner_alloc
// CHECK: malloc
