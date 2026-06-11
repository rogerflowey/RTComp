// Test: instrument-all baseline
// RUN: %clang -S -emit-llvm -O0 %s -o /tmp/test_baseline.ll && %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-san-place-all" -S /tmp/test_baseline.ll -o /dev/null 2>&1 | %FileCheck %s

__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
int rt_func(int x) {
  return x + 1;
}

int normal_func(void) {
  return 42;
}
// CHECK: instrument-all mode
// CHECK: Instrumenting: rt_func
// CHECK: Instrumenting: normal_func
// CHECK: Instrumented 2
