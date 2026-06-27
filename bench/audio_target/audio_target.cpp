// Audio靶场: 基于 miniaudio + dr_wav 的实时音频处理测试用例。
//
// 做两件事:
//   1. 编译到 LLVM bitcode → 用 RT-Effect 分析检测违规
//   2. 可选: 链接真实 miniaudio/dr_wav 做运行时 Demo
//
// 预期检测到的违规 (在 audio_callback 的冷路径中):
//   A) malloc/free  → NormalHeap allocation   → 违反 nonallocating
//   B) printf       → blocking I/O             → 违反 nonblocking
//   C) pthread_mutex_lock → lock               → 违反 nolock (若标注)
//
// 热路径 (apply_gain / lowpass / clamp) → ProvenSafe

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#ifndef M_PI
#define M_PI 3.13159265358979323846
#endif

// ─── rtsan_shim 计数接口 (弱引用,可与本地 shim 或 compiler-rt 真 RTSan 共存) ──
extern "C" {
extern void    __rtsan_realtime_reset_counts(void) __attribute__((weak));
extern uint64_t __rtsan_realtime_enter_count(void)   __attribute__((weak));
extern uint64_t __rtsan_realtime_exit_count(void)    __attribute__((weak));
}

static void print_rtsan_counts(const char *label) {
    uint64_t enter = 0, exit = 0;
    if (__rtsan_realtime_enter_count) enter = __rtsan_realtime_enter_count();
    if (__rtsan_realtime_exit_count)  exit  = __rtsan_realtime_exit_count();
    std::printf("RTSAN_COUNTS %s enter=%llu exit=%llu\n",
                label,
                static_cast<unsigned long long>(enter),
                static_cast<unsigned long long>(exit));
}

// ─── miniaudio / dr_wav API 声明 (不用引入真实头文件) ────────────────

#define MA_UINT32 unsigned
#define MA_UINT64 unsigned long long

typedef void (*ma_device_callback)(void*, void*, const void*, MA_UINT32);

extern "C" {
int ma_device_init(void*, void*, void*);
int ma_device_start(void*);
int ma_device_stop(void*);
void ma_device_uninit(void*);
int ma_engine_init(void*, void*);
void ma_engine_uninit(void*);

int drwav_open_file(const char*, void*);
int drwav_read_pcm_frames_f32(void*, MA_UINT64, float*);
void drwav_close(void*);
}

// ─── 实时安全 DSP 辅助函数 ──────────────────────────────────────────

// 纯计算: 增益控制。应被证明为 ProvenSafe。
__attribute__((noinline))
static void apply_gain(float *buf, MA_UINT32 n, float gain) {
    for (MA_UINT32 i = 0; i < n; ++i)
        buf[i] *= gain;
}

// 纯计算: 一阶低通 (IIR)。应被证明为 ProvenSafe。
__attribute__((noinline))
static void lowpass_filter(float *buf, MA_UINT32 n,
                           float *state, float alpha) {
    for (MA_UINT32 i = 0; i < n; ++i) {
        *state += alpha * (buf[i] - *state);
        buf[i] = *state;
    }
}

// 纯计算: 硬限幅。应被证明为 ProvenSafe。
__attribute__((noinline))
static void clamp(float *buf, MA_UINT32 n, float lo, float hi) {
    for (MA_UINT32 i = 0; i < n; ++i) {
        if (buf[i] > hi)      buf[i] = hi;
        else if (buf[i] < lo) buf[i] = lo;
    }
}

// ─── 蓄意违规辅助 ──────────────────────────────────────────────────

// 分配临时缓冲区并写入 (NormalHeap → 违反 nonallocating)。
__attribute__((noinline))
static void scratch_alloc_and_copy(float *dst,
                                   const float *src,
                                   MA_UINT32 n) {
    float *tmp = (float *)malloc((unsigned long)n * sizeof(float));
    for (MA_UINT32 i = 0; i < n; ++i)
        tmp[i] = src[i] * 0.5f;
    for (MA_UINT32 i = 0; i < n; ++i)
        dst[i] += tmp[i];
    free(tmp);
}

// 打印诊断 (printf → blocking → 违反 nonblocking)。
__attribute__((noinline))
static void log_diagnostics(MA_UINT32 frameCount, float peak) {
    printf("[audio] frame=%u peak=%.3f\n",
           (unsigned)frameCount, (double)peak);
}

// 持锁拷贝 (pthread_mutex_lock → lock → 违反 nolock)。
// 实际使用 trylock 做 fallback，但 lock 路径是违规的。
// pthread.h 已 include 在头文件区，pthread_mutex_* 直接用真实原型。

// 提供给 main 真实运行的 mutex — 一个 process-local 真实例，避免
// audio_callback 解引用 NULL mtx。RT-Effect 分析不会因此改动 —
// analysis 在函数级时仅看 pthread_mutex_lock 的调用站点，而
// 不在意运行时 mtx 是否有效、是否已经初始化。
static pthread_mutex_t g_demo_mtx = PTHREAD_MUTEX_INITIALIZER;

__attribute__((noinline))
static void locked_buffer_copy(float *dst, const float *src,
                               MA_UINT32 n, void *mtx) {
    // 蓄意使用 lock (阻塞变体), 不是 trylock
    pthread_mutex_lock(static_cast<pthread_mutex_t *>(mtx));   // ← 违反 nolock
    for (MA_UINT32 i = 0; i < n; ++i)
        dst[i] = src[i];
    pthread_mutex_unlock(static_cast<pthread_mutex_t *>(mtx));
}

// ─── 音频回调: 实时路径 ─────────────────────────────────────────────

// 标注为:
//    nonblocking  — 不得阻塞 (sleep/IO/mutex)
//    nonallocating — 不得分配 NormalHeap (malloc/new)
//
// rare=1 触发冷路径中的违规调用。分析器应在这些 call site 打 witness。

extern "C" __attribute__((used)) __attribute__((noinline))
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
__attribute__((annotate("rt_nolock")))
void audio_callback(void * /*pDevice*/, void *pOutput,
                    const void * /*pInput*/, MA_UINT32 frameCount,
                    int rare, void *mtx) {
    static float lp_state = 0.0f;
    float *out = (float *)pOutput;

    // 热路径: 纯 DSP, 无违规
    apply_gain(out, frameCount, 0.85f);
    lowpass_filter(out, frameCount, &lp_state, 0.15f);
    clamp(out, frameCount, -1.0f, 1.0f);

    // 冷路径: 蓄意违规 — 只在 rare 时触发
    if (rare) {
        // 违规 A: NormalHeap 分配
        float backup[256];
        scratch_alloc_and_copy(backup, out,
                               frameCount < 256 ? frameCount : 256);

        // 违规 B: 阻塞 I/O
        float peak = 0.0f;
        for (MA_UINT32 i = 0; i < frameCount; ++i)
            if (out[i] > peak) peak = out[i];
        log_diagnostics(frameCount, peak);

        // 违规 C: 持锁
        locked_buffer_copy(out, backup,
                           frameCount < 256 ? frameCount : 256, mtx);
    }
}

// ─── 主函数: 初始化路径 (非实时) ───────────────────────────────────

int main(int /*argc*/, char ** /*argv*/) {
    // 模拟 init 流程: 打开 WAV + 初始化设备 (外部 API, 非 RT)
    void *wav = nullptr;
    drwav_open_file("test.wav", &wav);

    void *device = nullptr;
    ma_device_init(nullptr, nullptr, &device);

    // Reset shim counters AFTER init so the init path (which calls into
    // external stubs ma_device_init / drwav_open_file) doesn't pollute
    // the RT-hook accounting.   reset_counts is weak — absent under real
    // compiler-rt RTSan, in which case the count fields stay zero
    // (and print_rtsan_counts reports that, which is fine).
    if (__rtsan_realtime_reset_counts)
        __rtsan_realtime_reset_counts();

    // 模拟运行几帧 (rare=0 → 纯热路径, 无违规; 不会被选择性插桩 hook 包围)
    float buf[256];
    for (int i = 0; i < 100; ++i) {
        for (MA_UINT32 j = 0; j < 256; ++j)
            buf[j] = 0.0f;
        audio_callback(nullptr, buf, nullptr, 256, /*rare=*/0, &g_demo_mtx);
    }

    print_rtsan_counts("hotpath");

    // 模拟一帧冷路径 (rare=1 → 触发违规;若被 hook 包围会触发)
    audio_callback(nullptr, buf, nullptr, 256, /*rare=*/1, &g_demo_mtx);

    print_rtsan_counts("final");

    ma_device_uninit(device);
    drwav_close(wav);
    return 0;
}
