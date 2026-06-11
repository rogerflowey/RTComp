// Test: real-time function with helper chain
// RUN: %clang -S -emit-llvm -g -O0 %s -o /tmp/test_hlp_chain.ll && %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S /tmp/test_hlp_chain.ll -o /dev/null 2>&1 | %FileCheck %s

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
// CHECK: nonallocating transitive violation in rt_outer
// CHECK: #0 rt_outer -> middle_alloc [call] at {{.*}}rt_helper_chain.c:16:3
// CHECK: #1 middle_alloc -> inner_alloc [call] at {{.*}}rt_helper_chain.c:11:3
// CHECK: #2 inner_alloc -> malloc [external] at {{.*}}rt_helper_chain.c:7:3
