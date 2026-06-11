// Test: safe real-time function
// RUN: %clang -S -emit-llvm -O0 %s -o /tmp/test_rt_safe.ll && %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S /tmp/test_rt_safe.ll -o /dev/null 2>&1 | %FileCheck %s

__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
int rt_safe_compute(int x) {
  return x * 2 + 1;
}
// CHECK: SAFE: 'rt_safe_compute'
