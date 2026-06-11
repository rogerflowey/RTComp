// RUN: %clang -std=c11 -c %S/../rtsan_runtime/rtsan_shim.c -o %t.shim.o
// RUN: %clang -std=c11 %s %t.shim.o -o %t
// RUN: %t

extern void __rtsan_realtime_enter(void);
extern void __rtsan_realtime_exit(void);
extern unsigned long long __rtsan_realtime_enter_count(void);
extern unsigned long long __rtsan_realtime_exit_count(void);
extern void __rtsan_realtime_reset_counts(void);

int main(void) {
  __rtsan_realtime_reset_counts();
  __rtsan_realtime_enter();
  __rtsan_realtime_exit();
  return __rtsan_realtime_enter_count() == 1 &&
                 __rtsan_realtime_exit_count() == 1
             ? 0
             : 1;
}
