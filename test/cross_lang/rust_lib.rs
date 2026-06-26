// Rust helper functions for cross-language real-time analysis.
// Compiled to LLVM bitcode and linked with C++ via llvm-link.

// Safe computation: no allocation, no blocking. Must be ProvenSafe.
#[no_mangle]
pub extern "C" fn rust_safe_compute(x: i32) -> i32 {
    x * 3 + 7
}

// FFI call to C's malloc — annotated in external_funcs.yaml as NormalHeap.
extern "C" {
    fn malloc(size: usize) -> *mut u8;
    fn free(ptr: *mut u8);
}

#[no_mangle]
pub extern "C" fn rust_ffi_malloc(n: usize) -> *mut u8 {
    unsafe { malloc(n) }
}

#[no_mangle]
pub extern "C" fn rust_ffi_malloc_free(n: usize) {
    let p = unsafe { malloc(n) };
    unsafe { free(p); }
}
