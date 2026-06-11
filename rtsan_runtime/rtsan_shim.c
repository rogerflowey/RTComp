#include <stdatomic.h>
#include <stdint.h>

static atomic_uint_fast64_t rtsan_enter_count;
static atomic_uint_fast64_t rtsan_exit_count;

void __rtsan_realtime_enter(void) {
  atomic_fetch_add_explicit(&rtsan_enter_count, 1, memory_order_relaxed);
}

void __rtsan_realtime_exit(void) {
  atomic_fetch_add_explicit(&rtsan_exit_count, 1, memory_order_relaxed);
}

uint64_t __rtsan_realtime_enter_count(void) {
  return atomic_load_explicit(&rtsan_enter_count, memory_order_relaxed);
}

uint64_t __rtsan_realtime_exit_count(void) {
  return atomic_load_explicit(&rtsan_exit_count, memory_order_relaxed);
}

void __rtsan_realtime_reset_counts(void) {
  atomic_store_explicit(&rtsan_enter_count, 0, memory_order_relaxed);
  atomic_store_explicit(&rtsan_exit_count, 0, memory_order_relaxed);
}
