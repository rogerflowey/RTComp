// C++ real-time entry points that call across into Rust helpers.
// The Rust functions (compiled to bitcode) are linked via llvm-link.
//
// Build + analyze via scripts/run_cross_lang.sh.
//
// Expected results:
//   rt_cpp_safe    -> rust_safe_compute    -> SAFE (no effects)
//   rt_cpp_violate -> rust_ffi_malloc      -> Violation (NormalHeap via malloc)

#ifdef __cplusplus
extern "C" {
#endif

int rust_safe_compute(int x);
void *rust_ffi_malloc(unsigned long n);
void rust_ffi_malloc_free(unsigned long n);

#ifdef __cplusplus
}
#endif

// RT-annotated function calling safe Rust code -> should be SAFE.
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
int rt_cpp_safe(int x) {
    return rust_safe_compute(x) * 2;
}

// RT-annotated function calling allocator via Rust -> should VIOLATE.
__attribute__((annotate("rt_nonblocking")))
__attribute__((annotate("rt_nonallocating")))
void *rt_cpp_violate(unsigned long n) {
    return rust_ffi_malloc(n);
}

int main(void) {
    int v = rt_cpp_safe(5);
    // guard: only alloc when env var is set (for benchmark comparison)
    if (v > 0) {
        // cold path
    }
    return v == 22 ? 0 : 1;
}
