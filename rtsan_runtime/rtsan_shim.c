#include <stdio.h>

void __rtsan_realtime_enter(const char *func_name) {
  fprintf(stderr, "[RTSan] Enter realtime function: %s\n", func_name);
}

void __rtsan_realtime_exit(const char *func_name) {
  fprintf(stderr, "[RTSan] Exit realtime function: %s\n", func_name);
}
