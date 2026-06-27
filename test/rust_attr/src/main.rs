// Smoke crate for the Rust RT annotation pathway.
//
// Expected after the RT-Effect pipeline runs on the emitted bitcode:
//   - rt_safe_compute    -> ProvenSafe (no allocs, no blocking, no throw)
//   - rt_alloc_violate   -> nonallocating violation via libc malloc
//                           (heap_kind=normal_heap)
//   - rt_block_violate   -> nonblocking violation via libc printf
//   - rt_lock_violate    -> nolock violation via libc pthread_mutex_lock
//   - rt_bounded         -> stack_depth exceeds bound 2 (underflow if applied)

extern "C" {
    fn malloc(n: usize) -> *mut u8;
    fn free(p: *mut u8);
    fn printf(fmt: *const u8, ...) -> i32;
    fn pthread_mutex_lock(m: *mut u8) -> i32;
}

// Pure compute — should be SAFE under both constraints.
#[rt::nonblocking]
#[rt::nonallocating]
extern "C" fn rt_safe_compute(x: i32) -> i32 {
    x * 3 + 7
}

// Allocates via libc malloc — should VIOLATE nonallocating (NormalHeap).
#[rt::nonblocking]
#[rt::nonallocating]
extern "C" fn rt_alloc_violate(n: usize) -> *mut u8 {
    unsafe { malloc(n) }
}

// Calls printf (blocking I/O) — should VIOLATE nonblocking.
#[rt::nonblocking]
extern "C" fn rt_block_violate(frame: u32) {
    let fmt = b"[audio] frame=%u\n\0";
    unsafe {
        printf(fmt.as_ptr(), frame);
    }
}

// Acquires a mutex — should VIOLATE nolock.
#[rt::nonblocking]
#[rt::nolock]
extern "C" fn rt_lock_violate(m: *mut u8) {
    unsafe {
        pthread_mutex_lock(m);
    }
}

// stack_bound = 2 is chosen to be tight enough that a single-call-deeper
// helper triggers a stack_depth violation report.
#[rt::nonblocking]
#[rt::stack_bound(2)]
extern "C" fn rt_bounded(x: i32) -> i32 {
    rt_safe_compute(x)
}

fn main() {
    let _ = rt_safe_compute(7);
    let _ = unsafe { rt_alloc_violate(64) };
    rt_block_violate(1);
    let mut mtx: u8 = 0u8;
    rt_lock_violate(&mut mtx as *mut _);
    let _ = rt_bounded(7);
}