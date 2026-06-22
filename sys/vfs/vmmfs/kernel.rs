//! Safe wrappers over the DragonFly kernel C APIs that vmmfs uses.
//!
//! All raw FFI and `unsafe` is confined to this module.  The rest of the crate
//! (parsing, the lifecycle state machine) is plain safe Rust; the `#[no_mangle]`
//! entry points are the only other place that touches the C ABI, and they do so
//! only by calling the wrappers here.

use core::ffi::{c_char, c_int};

extern "C" {
    fn kprintf(fmt: *const c_char, ...) -> c_int;
    fn vmmfs_kalloc(size: usize) -> *mut u8;
    fn vmmfs_kfree(ptr: *mut u8);
}

/// Print a NUL-terminated message to the kernel console.  `msg` is used as the
/// kprintf format string, so callers must pass a fixed message with no `%`.
pub fn kputs(msg: &[u8]) {
    // SAFETY: msg is a NUL-terminated byte string used as the format string,
    // with no variadic arguments.
    unsafe {
        kprintf(msg.as_ptr().cast());
    }
}

/// A kmalloc-backed owning box for a fixed-size value.  Drops and frees its
/// contents on `Drop`.
pub struct KBox<T> {
    ptr: *mut T,
}

impl<T> KBox<T> {
    /// Move `value` onto the kernel heap.  `None` only on allocation failure
    /// (kmalloc uses M_WAITOK, so that is effectively impossible).
    pub fn new(value: T) -> Option<KBox<T>> {
        // SAFETY: vmmfs_kalloc returns a block of the requested size, aligned
        // adequately for T (whose alignment is <= 16), or NULL.
        let ptr = unsafe { vmmfs_kalloc(core::mem::size_of::<T>()) } as *mut T;
        if ptr.is_null() {
            return None;
        }
        // SAFETY: ptr is freshly allocated, sized for T, and uninitialized.
        unsafe {
            core::ptr::write(ptr, value);
        }
        Some(KBox { ptr })
    }

    /// Release ownership, yielding the raw pointer to hand to C.  The caller
    /// becomes responsible for eventually passing it back to `from_raw`.
    pub fn into_raw(self) -> *mut T {
        let ptr = self.ptr;
        core::mem::forget(self);
        ptr
    }

    /// Reclaim a box from a pointer previously produced by `into_raw`.
    ///
    /// # Safety
    /// `ptr` must come from this type's `into_raw` and not already be reclaimed.
    pub unsafe fn from_raw(ptr: *mut T) -> KBox<T> {
        KBox { ptr }
    }
}

impl<T> Drop for KBox<T> {
    fn drop(&mut self) {
        // SAFETY: ptr was produced by KBox::new and is still initialized.
        unsafe {
            core::ptr::drop_in_place(self.ptr);
            vmmfs_kfree(self.ptr as *mut u8);
        }
    }
}

/// Borrow a live raw handle (from `KBox::into_raw`) as a mutable reference for
/// the duration of a call.
///
/// # Safety
/// `ptr` must point to a live `T` that outlives `'a`.
pub unsafe fn handle_mut<'a, T>(ptr: *mut T) -> &'a mut T {
    &mut *ptr
}

/// View a C-provided `(ptr, len)` as a byte slice for the duration of a call.
///
/// # Safety
/// `ptr` must point to `len` readable bytes outliving `'a` (NULL yields `&[]`).
pub unsafe fn bytes<'a>(ptr: *const u8, len: usize) -> &'a [u8] {
    if ptr.is_null() {
        &[]
    } else {
        core::slice::from_raw_parts(ptr, len)
    }
}

/// View a C-provided `(ptr, len)` as a mutable byte slice for a call.
///
/// # Safety
/// `ptr` must point to `len` writable bytes outliving `'a` (NULL yields `&mut []`).
pub unsafe fn bytes_mut<'a>(ptr: *mut u8, len: usize) -> &'a mut [u8] {
    if ptr.is_null() {
        &mut []
    } else {
        core::slice::from_raw_parts_mut(ptr, len)
    }
}

/// Write `value` through a C-provided out-pointer.
///
/// # Safety
/// `ptr` must be a valid, writable, suitably aligned `*mut T`.
pub unsafe fn write_out<T>(ptr: *mut T, value: T) {
    core::ptr::write(ptr, value);
}
