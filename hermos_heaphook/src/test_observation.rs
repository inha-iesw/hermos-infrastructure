//! Allocation-free instrumentation enabled only for system contract tests.

use std::{
    os::raw::c_void,
    ptr,
    sync::atomic::{AtomicPtr, Ordering},
};

/// Observer ABI: event, pointer, requested bytes, shared-pool routing.
pub type Observer = unsafe extern "C" fn(u32, *mut c_void, usize, bool) -> bool;

static OBSERVER: AtomicPtr<c_void> = AtomicPtr::new(ptr::null_mut());

/// Installs a process-wide test observer, or disables observation with NULL.
///
/// Event 0 precedes allocation; returning true injects ENOMEM without allocating
/// or releasing a realloc input. Its pointer is NULL except for realloc.
/// Event 1 reports a successful allocation and its requested size. Event 2
/// precedes an actual free and has size zero. Return values for events 1 and 2
/// are ignored. Realloc's internal relocation does not emit a separate event 2.
/// Requests rejected by argument validation bypass the observer.
///
/// # Safety
///
/// The callback must not allocate, log, unwind, change errno, or call allocator
/// hooks recursively. It may run concurrently on any allocating thread. Its code
/// must remain loaded until all in-flight calls have finished, including after
/// replacement or disabling. The caller owns that synchronization.
#[no_mangle]
pub unsafe extern "C" fn hermos_heaphook_test_set_observer(observer: Option<Observer>) {
    OBSERVER.store(
        observer.map_or(ptr::null_mut(), |callback| callback as *mut c_void),
        Ordering::Release,
    );
}

/// Returns whether an address is inside the initialized shared pool.
///
/// Does not dereference the address or initialize the allocator. NULL and any
/// address outside the exact half-open pool range return false.
#[no_mangle]
pub extern "C" fn hermos_heaphook_test_is_shared(ptr: *const c_void) -> bool {
    super::is_shared(ptr.cast())
}

#[inline]
pub(super) fn notify(event: u32, ptr: *mut c_void, size: usize, shared: bool) -> bool {
    let observer = OBSERVER.load(Ordering::Acquire);
    if observer.is_null() {
        return false;
    }
    let observer: Observer = unsafe { std::mem::transmute(observer) };
    unsafe { observer(event, ptr, size, shared) }
}

#[inline]
pub(super) fn allocate(
    ptr: *mut c_void,
    size: usize,
    shared: bool,
    allocate: impl FnOnce() -> *mut c_void,
) -> *mut c_void {
    if notify(0, ptr, size, shared) {
        unsafe { *libc::__errno_location() = libc::ENOMEM };
        return ptr::null_mut();
    }
    let result = allocate();
    if !result.is_null() {
        notify(1, result, size, shared);
    }
    result
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::{
        atomic::{AtomicBool, AtomicI64, AtomicUsize},
        Mutex, MutexGuard,
    };

    static LOCK: Mutex<()> = Mutex::new(());
    static THREAD: AtomicI64 = AtomicI64::new(0);
    static FAIL_NEXT: AtomicBool = AtomicBool::new(false);
    static COUNTS: [AtomicUsize; 3] = [
        AtomicUsize::new(0),
        AtomicUsize::new(0),
        AtomicUsize::new(0),
    ];
    static POINTERS: [AtomicUsize; 3] = [
        AtomicUsize::new(0),
        AtomicUsize::new(0),
        AtomicUsize::new(0),
    ];
    static SIZE: AtomicUsize = AtomicUsize::new(0);
    static SHARED: AtomicBool = AtomicBool::new(false);

    unsafe extern "C" fn observer(event: u32, ptr: *mut c_void, size: usize, shared: bool) -> bool {
        if THREAD.load(Ordering::Relaxed) != libc::syscall(libc::SYS_gettid) {
            return false;
        }
        COUNTS[event as usize].fetch_add(1, Ordering::Relaxed);
        POINTERS[event as usize].store(ptr as usize, Ordering::Relaxed);
        SIZE.store(size, Ordering::Relaxed);
        SHARED.store(shared, Ordering::Relaxed);
        event != 0 || FAIL_NEXT.swap(false, Ordering::Relaxed)
    }

    struct Observation(MutexGuard<'static, ()>);

    impl Observation {
        fn new() -> Self {
            let observation = Self(LOCK.lock().unwrap());
            for value in COUNTS.iter().chain(POINTERS.iter()) {
                value.store(0, Ordering::Relaxed);
            }
            FAIL_NEXT.store(false, Ordering::Relaxed);
            unsafe {
                hermos_heaphook_test_set_observer(Some(observer));
                THREAD.store(libc::syscall(libc::SYS_gettid), Ordering::Relaxed);
            }
            observation
        }
    }

    impl Drop for Observation {
        fn drop(&mut self) {
            THREAD.store(0, Ordering::Relaxed);
            unsafe { hermos_heaphook_test_set_observer(None) };
        }
    }

    const ALLOCATORS: [fn() -> *mut c_void; 6] = [
        || crate::malloc(64),
        || crate::calloc(4, 16),
        || unsafe { crate::realloc(ptr::null_mut(), 64) },
        || crate::aligned_alloc(32, 64),
        || crate::memalign(32, 64),
        || {
            let mut ptr = ptr::null_mut();
            crate::posix_memalign(&mut ptr, 32, 64);
            ptr
        },
    ];

    #[test]
    fn every_allocator_reports_one_attempt_result_and_free() {
        for allocator in ALLOCATORS {
            let _observation = Observation::new();
            let ptr = allocator();
            assert!(!ptr.is_null());
            assert_eq!(COUNTS[0].load(Ordering::Relaxed), 1);
            assert_eq!(COUNTS[1].load(Ordering::Relaxed), 1);
            assert_eq!(POINTERS[0].load(Ordering::Relaxed), 0);
            assert_eq!(POINTERS[1].load(Ordering::Relaxed), ptr as usize);
            assert_eq!(SIZE.load(Ordering::Relaxed), 64);
            assert!(SHARED.load(Ordering::Relaxed));
            unsafe { crate::free(ptr) };
            assert_eq!(COUNTS[2].load(Ordering::Relaxed), 1);
            assert_eq!(POINTERS[2].load(Ordering::Relaxed), ptr as usize);
            assert_eq!(SIZE.load(Ordering::Relaxed), 0);
            assert!(SHARED.load(Ordering::Relaxed));
        }
    }

    #[test]
    fn injected_failure_is_one_shot_and_next_allocation_succeeds() {
        for (index, allocator) in ALLOCATORS.into_iter().enumerate() {
            let _observation = Observation::new();
            unsafe { *libc::__errno_location() = 0 };
            FAIL_NEXT.store(true, Ordering::Relaxed);
            let failed = allocator();
            assert!(failed.is_null());
            let expected_errno = if index == 5 { 0 } else { libc::ENOMEM };
            assert_eq!(unsafe { *libc::__errno_location() }, expected_errno);
            assert_eq!(COUNTS[0].load(Ordering::Relaxed), 1);
            assert_eq!(COUNTS[1].load(Ordering::Relaxed), 0);
            assert_eq!(COUNTS[2].load(Ordering::Relaxed), 0);
            let ptr = allocator();
            assert!(!ptr.is_null());
            assert_eq!(COUNTS[0].load(Ordering::Relaxed), 2);
            assert_eq!(COUNTS[1].load(Ordering::Relaxed), 1);
            unsafe { crate::free(ptr) };
        }
    }

    #[test]
    fn realloc_injection_preserves_shared_and_heap_inputs() {
        for shared in [true, false] {
            let ptr = if shared {
                crate::malloc(64)
            } else {
                unsafe { (*crate::ORIGINAL_MALLOC.get_or_init(crate::init_original_malloc))(64) }
            };
            assert!(!ptr.is_null());
            unsafe { ptr::write_bytes(ptr.cast::<u8>(), 0xa5, 64) };
            let _observation = Observation::new();
            FAIL_NEXT.store(true, Ordering::Relaxed);
            let result = unsafe { crate::realloc(ptr, 128) };
            assert!(result.is_null());
            assert_eq!(unsafe { *libc::__errno_location() }, libc::ENOMEM);
            assert_eq!(POINTERS[0].load(Ordering::Relaxed), ptr as usize);
            assert_eq!(COUNTS[1].load(Ordering::Relaxed), 0);
            assert_eq!(COUNTS[2].load(Ordering::Relaxed), 0);
            assert_eq!(SHARED.load(Ordering::Relaxed), shared);
            for offset in 0..64 {
                assert_eq!(unsafe { *ptr.cast::<u8>().add(offset) }, 0xa5);
            }
            let resized = unsafe { crate::realloc(ptr, 128) };
            assert!(!resized.is_null());
            assert_eq!(SHARED.load(Ordering::Relaxed), shared);
            unsafe { crate::free(resized) };
            assert_eq!(COUNTS[2].load(Ordering::Relaxed), 1);
            assert_eq!(SHARED.load(Ordering::Relaxed), shared);
        }
    }

    #[test]
    fn posix_failure_preserves_output_and_errno_and_invalid_alignment_wins() {
        let _observation = Observation::new();
        let mut output = 0x1234usize as *mut c_void;
        unsafe { *libc::__errno_location() = libc::EINTR };
        FAIL_NEXT.store(true, Ordering::Relaxed);
        let invalid = crate::posix_memalign(&mut output, 3, 64);
        let pending = FAIL_NEXT.load(Ordering::Relaxed);
        let failed = crate::posix_memalign(&mut output, 32, 64);
        assert_eq!(invalid, libc::EINVAL);
        assert!(pending);
        assert_eq!(failed, libc::ENOMEM);
        assert_eq!(output as usize, 0x1234);
        assert_eq!(unsafe { *libc::__errno_location() }, libc::EINTR);
        assert_eq!(COUNTS[0].load(Ordering::Relaxed), 1);
        assert_eq!(COUNTS[1].load(Ordering::Relaxed), 0);
    }

    #[test]
    fn invalid_shared_requests_do_not_consume_injection() {
        let _observation = Observation::new();
        FAIL_NEXT.store(true, Ordering::Relaxed);
        let overflow = crate::calloc(usize::MAX, 2);
        let alignment = crate::aligned_alloc(3, 64);
        let layout = crate::malloc(usize::MAX);
        let pending = FAIL_NEXT.swap(false, Ordering::Relaxed);
        assert!(overflow.is_null() && alignment.is_null() && layout.is_null());
        assert!(pending);
        assert_eq!(COUNTS[0].load(Ordering::Relaxed), 0);
    }

    #[test]
    fn disabled_observer_leaves_allocator_and_exact_pool_query_usable() {
        let _observation = Observation::new();
        unsafe { hermos_heaphook_test_set_observer(None) };
        let ptr = crate::malloc(64);
        assert!(!ptr.is_null());
        assert!(hermos_heaphook_test_is_shared(ptr));
        let pool = crate::HERMOS_SHARED_MEMORY.get().unwrap();
        assert!(!hermos_heaphook_test_is_shared(ptr::null()));
        assert!(!hermos_heaphook_test_is_shared(
            (pool.start - 1) as *const c_void
        ));
        assert!(hermos_heaphook_test_is_shared(
            pool.start as *const c_void
        ));
        assert!(hermos_heaphook_test_is_shared(
            (pool.end - 1) as *const c_void
        ));
        assert!(!hermos_heaphook_test_is_shared(pool.end as *const c_void));
        unsafe { crate::free(ptr) };
        assert_eq!(COUNTS[0].load(Ordering::Relaxed), 0);
        assert_eq!(COUNTS[1].load(Ordering::Relaxed), 0);
        assert_eq!(COUNTS[2].load(Ordering::Relaxed), 0);
    }
}
