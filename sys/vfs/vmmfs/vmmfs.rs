#![no_std]

mod kernel;

use core::ffi::c_int;
use core::panic::PanicInfo;
use kernel::KBox;

const EINVAL: c_int = 22;

const VMMFS_VCPU_MAX: u64 = 256;
/// Guest-memory alignment requirement (2 MiB, large-page granularity).
const VMMFS_MEM_ALIGN: u64 = 2 * 1024 * 1024;

const LOAD_MESSAGE: &[u8] = b"vmmfs: Rust module loaded\n\0";
const UNLOAD_MESSAGE: &[u8] = b"vmmfs: Rust module unloaded\n\0";

#[no_mangle]
pub extern "C" fn vmmfs_rust_init() -> c_int {
    kernel::kputs(LOAD_MESSAGE);
    0
}

#[no_mangle]
pub extern "C" fn vmmfs_rust_fini() {
    kernel::kputs(UNLOAD_MESSAGE);
}

// ---------------------------------------------------------------------------
// Config parsing: pure safe logic.  Written with only inlinable, non-panicking
// slice ops (`.get()` / `.iter()` / `split_last`) so the object references no
// core panic helpers that a bare kernel module cannot resolve.
// ---------------------------------------------------------------------------

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
fn parse_vcpu(s: &[u8]) -> Option<u32> {
    match parse_decimal_u64(trim_ascii(s)) {
        Some(n) if (1..=VMMFS_VCPU_MAX).contains(&n) => Some(n as u32),
        _ => None,
    }
}

/// Parse the `mem` file: `number[KkMmGg]`, > 0, VMMFS_MEM_ALIGN-aligned.
fn parse_mem(s: &[u8]) -> Option<u64> {
    let s = trim_ascii(s);
    let (digits, mult): (&[u8], u64) = match s.split_last() {
        Some((&c, rest)) if c == b'K' || c == b'k' => (rest, 1024),
        Some((&c, rest)) if c == b'M' || c == b'm' => (rest, 1024 * 1024),
        Some((&c, rest)) if c == b'G' || c == b'g' => (rest, 1024 * 1024 * 1024),
        Some((&c, _)) if c.is_ascii_digit() => (s, 1),
        _ => return None,
    };
    let total = parse_decimal_u64(digits)?.checked_mul(mult)?;
    if total == 0 || total % VMMFS_MEM_ALIGN != 0 {
        return None;
    }
    Some(total)
}

/// # Safety
/// `buf` covers `len` readable bytes (or is NULL); `out` is a writable `*mut u32`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_parse_vcpu(buf: *const u8, len: usize, out: *mut u32) -> c_int {
    if out.is_null() {
        return -EINVAL;
    }
    match parse_vcpu(kernel::bytes(buf, len)) {
        Some(v) => {
            kernel::write_out(out, v);
            0
        }
        None => -EINVAL,
    }
}

/// # Safety
/// `buf` covers `len` readable bytes (or is NULL); `out` is a writable `*mut u64`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_parse_mem(buf: *const u8, len: usize, out: *mut u64) -> c_int {
    if out.is_null() {
        return -EINVAL;
    }
    match parse_mem(kernel::bytes(buf, len)) {
        Some(v) => {
            kernel::write_out(out, v);
            0
        }
        None => -EINVAL,
    }
}

// ---------------------------------------------------------------------------
// Machine lifecycle state machine: pure safe logic.
//
// The control surface exposes only the stable desired state {Running, Stopped}
// (no intermediate state); the loader is stubbed so transitions are instant.
// The async stopping transition for a real guest is a later addition.
// ---------------------------------------------------------------------------

pub struct MachineState {
    stopped: bool,
}

impl MachineState {
    fn new(stopped: bool) -> MachineState {
        MachineState { stopped }
    }

    fn is_stopped(&self) -> bool {
        self.stopped
    }

    /// Stop (apic graceful or force).  Idempotent; `force` selects the method
    /// for the future real-guest path.
    fn stop(&mut self, _force: bool) {
        self.stopped = true;
    }

    /// Start.  Idempotent.
    fn start(&mut self) {
        self.stopped = false;
    }
}

#[no_mangle]
pub extern "C" fn vmmfs_machine_new(stopped: c_int) -> *mut MachineState {
    match KBox::new(MachineState::new(stopped != 0)) {
        Some(b) => b.into_raw(),
        None => core::ptr::null_mut(),
    }
}

/// # Safety
/// `m` is a handle from `vmmfs_machine_new` that has not yet been freed.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_free(m: *mut MachineState) {
    if !m.is_null() {
        drop(KBox::from_raw(m));
    }
}

/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_is_stopped(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).is_stopped() as c_int
}

/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_stop(m: *mut MachineState, force: c_int) {
    kernel::handle_mut(m).stop(force != 0);
}

/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_start(m: *mut MachineState) {
    kernel::handle_mut(m).start();
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
