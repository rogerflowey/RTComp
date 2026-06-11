// Benchmark workload: a fixed-size audio-DSP-style real-time loop.
//
// The "audio_callback" is the RT entry point; the analyzer should mark
// the chain of safe helpers ProvenSafe and the rare misbehaving helper
// (alloc_path) Violating. Selective instrumentation should leave the hot
// path hook-free and only wrap the offending call sites.
//
// We deliberately mix:
//   - tight numeric loops (proven safe)
//   - a callback dispatch (poly-resolvable when the callback is known)
//   - a slow/leaky helper that allocates (violating)
//
// Build + run via bench/run_benchmark.sh.

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cstdio>

extern "C" {
extern void __rtsan_realtime_enter(void) __attribute__((weak));
extern void __rtsan_realtime_exit(void) __attribute__((weak));
extern uint64_t __rtsan_realtime_enter_count(void) __attribute__((weak));
extern uint64_t __rtsan_realtime_exit_count(void) __attribute__((weak));
extern void __rtsan_realtime_reset_counts(void) __attribute__((weak));
}

constexpr size_t kFrames = 256;
constexpr size_t kIters  = 200000;

static float gBuf[kFrames];

// Pure math helper: must come out ProvenSafe.
__attribute__((noinline))
static float mix(float a, float b, float w) {
  return a * (1.0f - w) + b * w;
}

// Tight loop, no allocation. Should be ProvenSafe.
__attribute__((noinline))
static void apply_gain(float *buf, size_t n, float g) {
  for (size_t i = 0; i < n; ++i) {
    buf[i] = mix(buf[i], buf[i] * g, 0.5f);
  }
}

// Effect-polymorphic: caller-provided unary op, applied to every sample.
// In the analyzer this should be detected polymorphic in arg 1.
__attribute__((noinline))
static void map_buffer(float *buf, void (*op)(float *)) {
  for (size_t i = 0; i < kFrames; ++i)
    op(&buf[i]);
}

// Safe per-sample callback.
static void clamp_sample(float *s) {
  if (*s > 1.0f) *s = 1.0f;
  if (*s < -1.0f) *s = -1.0f;
}

// One-in-a-million slow path that always allocates. The outer guard
// in audio_callback decides whether we ever enter this function — that
// guard is what selective per-call-site instrumentation exploits.
__attribute__((noinline))
static void heavy_alloc(void) {
  void *p = malloc(64);
  asm volatile("" : : "r"(p) : "memory");
  free(p);
}

// Real-time entry point. External linkage + `used` keep the inliner
// from substituting it into main; that's what we want when measuring
// per-call hook overhead.
extern "C" __attribute__((used)) __attribute__((noinline))
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
void audio_callback(int rare) {
  apply_gain(gBuf, kFrames, 0.7f);
  map_buffer(gBuf, clamp_sample);
  // The leaky path is the *only* witness in this function; selective
  // per-call-site instrumentation should wrap exactly this call site,
  // so when `rare` is false the hooks never fire at runtime.
  if (rare)
    heavy_alloc();
}

int main(int argc, char ** /*argv*/) {
  if (__rtsan_realtime_reset_counts)
    __rtsan_realtime_reset_counts();

  for (size_t i = 0; i < kFrames; ++i)
    gBuf[i] = static_cast<float>(i % 32) / 32.0f;

  // Inject allocation activity only when the user explicitly requests
  // it, so we can compare hot-loop overhead in the common case.
  int rare = (argc > 1) ? 1 : 0;

  for (size_t i = 0; i < kIters; ++i)
    audio_callback(rare && (i % 1000 == 0));

  uint64_t enters = 0, exits = 0;
  if (__rtsan_realtime_enter_count) enters = __rtsan_realtime_enter_count();
  if (__rtsan_realtime_exit_count)  exits  = __rtsan_realtime_exit_count();

  std::printf("rtsan_enter=%llu rtsan_exit=%llu iters=%zu\n",
              static_cast<unsigned long long>(enters),
              static_cast<unsigned long long>(exits),
              kIters);
  return 0;
}
