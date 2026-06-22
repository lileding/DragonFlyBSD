//! Compiler intrinsics that LLVM emits (memcpy / memset / memmove / memcmp).
//!
//! LLVM's loop-idiom pass turns byte-copy/fill loops (e.g. the small copies in
//! the event ring, struct moves) into calls to these.  compiler_builtins is not
//! linked into the bare kernel module, so we must provide them.  Defining them
//! in-crate also keeps the references local (R_X86_64_PLT32) instead of an
//! R_X86_64_GOTPCREL against an external symbol, which the kernel ELF object
//! loader cannot relocate ("unexpected relocation type 9").
//!
//! The bodies use volatile accesses so loop-idiom does NOT rewrite them back
//! into a (recursive) memcpy/memset call.

use core::ffi::c_int;
use core::ptr::{read_volatile, write_volatile};

/// # Safety: `dst`/`src` cover `n` bytes and do not overlap.
#[no_mangle]
pub unsafe extern "C" fn memcpy(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    let mut i = 0;
    while i < n {
        write_volatile(dst.add(i), read_volatile(src.add(i)));
        i += 1;
    }
    dst
}

/// # Safety: `dst`/`src` cover `n` bytes (may overlap).
#[no_mangle]
pub unsafe extern "C" fn memmove(dst: *mut u8, src: *const u8, n: usize) -> *mut u8 {
    if (dst as usize) < (src as usize) {
        let mut i = 0;
        while i < n {
            write_volatile(dst.add(i), read_volatile(src.add(i)));
            i += 1;
        }
    } else {
        let mut i = n;
        while i > 0 {
            i -= 1;
            write_volatile(dst.add(i), read_volatile(src.add(i)));
        }
    }
    dst
}

/// # Safety: `dst` covers `n` bytes.
#[no_mangle]
pub unsafe extern "C" fn memset(dst: *mut u8, c: c_int, n: usize) -> *mut u8 {
    let b = c as u8;
    let mut i = 0;
    while i < n {
        write_volatile(dst.add(i), b);
        i += 1;
    }
    dst
}

/// # Safety: `a`/`b` cover `n` bytes.
#[no_mangle]
pub unsafe extern "C" fn memcmp(a: *const u8, b: *const u8, n: usize) -> c_int {
    let mut i = 0;
    while i < n {
        let x = read_volatile(a.add(i));
        let y = read_volatile(b.add(i));
        if x != y {
            return (x as c_int) - (y as c_int);
        }
        i += 1;
    }
    0
}
