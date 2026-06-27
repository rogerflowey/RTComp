// Stub implementations for the test/audio_target so that the
// shim-linked benchmark binary can be linked without pulling in the
// real miniaudio/dr_wav libraries.   These are NOT real
// implementations: they just satisfy the linker — the benchmark only
// exercises the real-time callback path; the init-time calls
// `drwav_open_file` / `ma_device_init` etc. are deliberately inert.
//
// The actual real-library pathway is exercised separately via
// scripts/eval_audio_full.sh (see bench/audio_target_full.cpp).

extern "C" {

int drwav_open_file(const char *path, void *pWav)  { (void)path;  (void)pWav; return 0; }
int drwav_read_pcm_frames_f32(void *pWav, unsigned long long n, float *pOut)
{ (void)pWav; (void)n; (void)pOut; return 0; }
void drwav_close(void *pWav) { (void)pWav; }

int  ma_device_init(void *pContext, void *pConfig, void *pDevice)
{ (void)pContext; (void)pConfig; (void)pDevice; return 0; }
int  ma_device_start(void *pDevice)  { (void)pDevice; return 0; }
int  ma_device_stop(void *pDevice)   { (void)pDevice; return 0; }
void ma_device_uninit(void *pDevice) { (void)pDevice; }
int  ma_engine_init(void *pConfig, void *pEngine)
{ (void)pConfig; (void)pEngine; return 0; }
void ma_engine_uninit(void *pEngine) { (void)pEngine; }

} // extern "C"