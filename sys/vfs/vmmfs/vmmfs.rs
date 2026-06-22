#![no_std]

use core::ffi::{c_char, c_int};
use core::panic::PanicInfo;

extern "C" {
    fn kprintf(fmt: *const c_char, ...) -> c_int;
}

const LOAD_MESSAGE: &[u8] = b"vmmfs: Rust module loaded\n\0";
const UNLOAD_MESSAGE: &[u8] = b"vmmfs: Rust module unloaded\n\0";

#[no_mangle]
pub extern "C" fn vmmfs_rust_init() -> c_int {
    unsafe {
        kprintf(LOAD_MESSAGE.as_ptr().cast());
    }

    0
}

#[no_mangle]
pub extern "C" fn vmmfs_rust_fini() {
    unsafe {
        kprintf(UNLOAD_MESSAGE.as_ptr().cast());
    }
}

// ---------------------------------------------------------------------------
// Config parsing (vmmfs object-model semantics; pure, no heap, no kernel APIs).
// C reads the raw bytes of the `vcpu` / `mem` config files and calls these.
// Returns 0 on success (writing *out), or a negative DragonFly errno.
// ---------------------------------------------------------------------------

const EINVAL: c_int = 22;

const VMMFS_VCPU_MAX: u64 = 256;
/// Guest-memory alignment requirement (2 MiB, large-page granularity).
const VMMFS_MEM_ALIGN: u64 = 2 * 1024 * 1024;

// NOTE: these are written with only inlinable, non-panicking operations
// (`.get()` / `.iter()` / `split_last`, no `[i]` indexing or `[a..b]` range
// slicing).  Indexing/range-slicing emit references to core panic helpers
// (e.g. slice_index_order_fail) that are undefined in a bare kernel module.
fn is_ws(c: u8) -> bool {
    matches!(c, b' ' | b'\t' | b'\r' | b'\n')
}

fn trim_ascii(b: &[u8]) -> &[u8] {
    let mut start = 0usize;
    let mut end = b.len();
    while start < end {
        match b.get(start) {
            Some(&c) if is_ws(c) => start += 1,
            _ => break,
        }
    }
    while end > start {
        match b.get(end - 1) {
            Some(&c) if is_ws(c) => end -= 1,
            _ => break,
        }
    }
    b.get(start..end).unwrap_or(&[])
}

fn parse_decimal_u64(b: &[u8]) -> Option<u64> {
    if b.is_empty() {
        return None;
    }
    let mut v: u64 = 0;
    for &c in b.iter() {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as u64)?;
    }
    Some(v)
}

/// Parse the `vcpu` file: decimal integer, 1..=VMMFS_VCPU_MAX.
///
/// # Safety
/// `buf` must point to `len` readable bytes; `out` must be a valid `*mut u32`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_parse_vcpu(buf: *const u8, len: usize, out: *mut u32) -> c_int {
    if buf.is_null() || out.is_null() {
        return -EINVAL;
    }
    let s = trim_ascii(core::slice::from_raw_parts(buf, len));
    match parse_decimal_u64(s) {
        Some(n) if (1..=VMMFS_VCPU_MAX).contains(&n) => {
            *out = n as u32;
            0
        }
        _ => -EINVAL,
    }
}

/// Parse the `mem` file: `number[KkMmGg]`, > 0, VMMFS_MEM_ALIGN-aligned.
///
/// # Safety
/// `buf` must point to `len` readable bytes; `out` must be a valid `*mut u64`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_parse_mem(buf: *const u8, len: usize, out: *mut u64) -> c_int {
    if buf.is_null() || out.is_null() {
        return -EINVAL;
    }
    let s = trim_ascii(core::slice::from_raw_parts(buf, len));
    if s.is_empty() {
        return -EINVAL;
    }

    let (digits, mult): (&[u8], u64) = match s.split_last() {
        Some((&c, rest)) if c == b'K' || c == b'k' => (rest, 1024),
        Some((&c, rest)) if c == b'M' || c == b'm' => (rest, 1024 * 1024),
        Some((&c, rest)) if c == b'G' || c == b'g' => (rest, 1024 * 1024 * 1024),
        Some((&c, _)) if c.is_ascii_digit() => (s, 1),
        _ => return -EINVAL,
    };

    let total = match parse_decimal_u64(digits).and_then(|n| n.checked_mul(mult)) {
        Some(t) => t,
        None => return -EINVAL,
    };
    if total == 0 || total % VMMFS_MEM_ALIGN != 0 {
        return -EINVAL;
    }
    *out = total;
    0
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
