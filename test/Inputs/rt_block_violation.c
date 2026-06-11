// Test: real-time function that blocks (using pthread mutex)
// RUN: %clang -S -emit-llvm -O0 %s -o /tmp/test_block.ll && %opt -load-pass-plugin=%plugin_dir/libRTEffect.so -passes="rt-effect-infer,rt-constraint-check" -S /tmp/test_block.ll -o /dev/null 2>&1 | %FileCheck %s

#include <pthread.h>

__attribute__((annotate("rt_nonblocking")))
void rt_block_violation(pthread_mutex_t *m) {
  pthread_mutex_lock(m);
}
// CHECK: VIOLATION in 'rt_block_violation'
// CHECK: blocks
