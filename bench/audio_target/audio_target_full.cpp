// Audio target with REAL miniaudio + dr_wav single-header libraries.
//
// Contrast with audio_target.cpp, which uses `extern "C"` stubs only
// for compile/link portability. This version pulls the real libraries
// in so the RT-Effect analysis pipeline runs over genuine library
// source — extracting real transitive effects (`malloc` /
// `pthread_mutex_lock` / blocking I/O) inside the init path rather
// than relying on a hand-maintained YAML table.
//
// Build workflow:
//   scripts/fetch_audio_headers.sh                 # one-time header fetch
//   scripts/eval_audio_full.sh                     # build + analyse
//
// Strategy:
//   - Init path (non-RT) calls real `drwav_open_file_and_read_pcm_frames_f32`
//     (loads a tiny 8-sample WAV synthesized on the fly into /tmp).
//   - Real-time path uses plain DSP helpers (apply_gain / lowpass /
//     clamp) identical to the stub audio_target.cpp.
//   - Cold path: same three intentional violations (malloc+free,
//     printf, pthread_mutex_lock).

#define MINIAUDIO_IMPLEMENTATION
#include "vendor/miniaudio.h"
#define DR_WAV_IMPLEMENTATION
#include "vendor/dr_wav.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <pthread.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

// ─── rtsan_shim 计数接口 (供 runtime count 实验) ─────────────────
extern "C" {
extern void __rtsan_realtime_reset_counts(void) __attribute__((weak));
extern uint64_t __rtsan_realtime_enter_count(void)  __attribute__((weak));
extern uint64_t __rtsan_realtime_exit_count(void)   __attribute__((weak));
}

static void print_rtsan_counts(const char *label) {
    uint64_t enter = 0, exit = 0;
    if (__rtsan_realtime_enter_count) enter = __rtsan_realtime_enter_count();
    if (__rtsan_realtime_exit_count)  exit  = __rtsan_realtime_exit_count();
    std::printf("RTSAN_COUNTS %s enter=%llu exit=%llu\n", label,
                static_cast<unsigned long long>(enter),
                static_cast<unsigned long long>(exit));
}

// ─── 实时安全 DSP 辅助函数 ──────────────────────────────────────────

__attribute__((noinline))
static void apply_gain(float *buf, ma_uint32 n, float gain) {
    for (ma_uint32 i = 0; i < n; ++i)
        buf[i] *= gain;
}

__attribute__((noinline))
static void lowpass_filter(float *buf, ma_uint32 n, float *state, float alpha) {
    for (ma_uint32 i = 0; i < n; ++i) {
        *state += alpha * (buf[i] - *state);
        buf[i] = *state;
    }
}

__attribute__((noinline))
static void clamp(float *buf, ma_uint32 n, float lo, float hi) {
    for (ma_uint32 i = 0; i < n; ++i) {
        if (buf[i] > hi)      buf[i] = hi;
        else if (buf[i] < lo) buf[i] = lo;
    }
}

// ─── 蓄意违规辅助 (cold path) ───────────────────────────────────────
__attribute__((noinline))
static void scratch_alloc_and_copy(float *dst, const float *src, ma_uint32 n) {
    float *tmp = (float *)malloc((unsigned long)n * sizeof(float));
    for (ma_uint32 i = 0; i < n; ++i)
        tmp[i] = src[i] * 0.5f;
    for (ma_uint32 i = 0; i < n; ++i)
        dst[i] += tmp[i];
    free(tmp);
}

__attribute__((noinline))
static void log_diagnostics(ma_uint32 frameCount, float peak) {
    std::printf("[audio] frame=%u peak=%.3f\n",
                (unsigned)frameCount, (double)peak);
}

static pthread_mutex_t g_demo_mtx = PTHREAD_MUTEX_INITIALIZER;

__attribute__((noinline))
static void locked_buffer_copy(float *dst, const float *src,
                               ma_uint32 n, void *mtx) {
    pthread_mutex_lock(static_cast<pthread_mutex_t *>(mtx));
    for (ma_uint32 i = 0; i < n; ++i)
        dst[i] = src[i];
    pthread_mutex_unlock(static_cast<pthread_mutex_t *>(mtx));
}

// ─── 音频回调: 实时路径 ─────────────────────────────────────────────
extern "C" __attribute__((used)) __attribute__((noinline))
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
__attribute__((annotate("rt_nolock")))
void audio_callback(void * /*pDevice*/, void *pOutput,
                     const void * /*pInput*/, ma_uint32 frameCount,
                     int rare, void *mtx) {
    static float lp_state = 0.0f;
    float *out = (float *)pOutput;

    apply_gain(out, frameCount, 0.85f);
    lowpass_filter(out, frameCount, &lp_state, 0.15f);
    clamp(out, frameCount, -1.0f, 1.0f);

    if (rare) {
        float backup[256];
        scratch_alloc_and_copy(backup, out,
                               frameCount < 256 ? frameCount : 256);
        float peak = 0.0f;
        for (ma_uint32 i = 0; i < frameCount; ++i)
            if (out[i] > peak) peak = out[i];
        log_diagnostics(frameCount, peak);
        locked_buffer_copy(out, backup,
                            frameCount < 256 ? frameCount : 256, mtx);
    }
}

// ─── 合成一个 16-sample / 44100-Hz / float32 WAV 文件到 /tmp ───────
static void synth_test_wav(const char *path) {
    drwav_data_format fmt;
    fmt.container = drwav_container_riff;
    fmt.format    = DR_WAVE_FORMAT_IEEE_FLOAT;
    fmt.channels  = 1;
    fmt.sampleRate = 44100;
    fmt.bitsPerSample = 32;

    drwav wav;
    if (!drwav_init_file_write(&wav, path, &fmt, nullptr)) {
        std::fprintf(stderr, "drwav_init_file_write failed for %s\n", path);
        return;
    }
    float samples[16];
    for (int i = 0; i < 16; ++i)
        samples[i] = 0.7f * std::sin(2 * M_PI * 440.0f * i / 44100.0);
    drwav_write_pcm_frames(&wav, 16, samples);
    drwav_uninit(&wav);
}

int main(int /*argc*/, char ** /*argv*/) {
    const char *wav_path = "/tmp/rteffect_audio_full.wav";
    synth_test_wav(wav_path);

    // Real WAV loading via dr_wav. Shows that analysis can attribute
    // effects to library-internal code (drwav_open_file opens FILE,
    // allocates buffers, etc).
    float *frames = nullptr;
    unsigned channels = 0;
    unsigned sample_rate = 0;
    ma_uint64 num_frames = 0;
    frames = drwav_open_file_and_read_pcm_frames_f32(wav_path, &channels,
            &sample_rate, &num_frames, nullptr);
    std::printf("[full] loaded %llu frames (%u ch, %u Hz) from %s\n",
                static_cast<unsigned long long>(num_frames),
                channels, sample_rate, wav_path);
    drwav_free(frames, nullptr);

    if (__rtsan_realtime_reset_counts)
        __rtsan_realtime_reset_counts();

    float buf[256];
    for (int i = 0; i < 100; ++i) {
        for (ma_uint32 j = 0; j < 256; ++j)
            buf[j] = static_cast<float>(j) / 32.0f;
        audio_callback(nullptr, buf, nullptr, 256, /*rare=*/0, &g_demo_mtx);
    }
    print_rtsan_counts("hotpath");

    audio_callback(nullptr, buf, nullptr, 256, /*rare=*/1, &g_demo_mtx);
    print_rtsan_counts("final");

    return 0;
}