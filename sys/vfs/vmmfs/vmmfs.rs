#![no_std]

mod intrinsics;
mod kernel;
mod machine;
mod parse;

use core::ffi::c_int;
use core::panic::PanicInfo;
use kernel::KBox;
use machine::MachineState;

const EINVAL: c_int = 22;

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
// C boundary shims.  These are the only places besides kernel.rs that touch the
// C ABI; they delegate to the safe logic in parse.rs / machine.rs and to the
// wrappers in kernel.rs.
// ---------------------------------------------------------------------------

/// # Safety
/// `buf` covers `len` readable bytes (or is NULL); `out` is a writable `*mut u32`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_parse_vcpu(buf: *const u8, len: usize, out: *mut u32) -> c_int {
    if out.is_null() {
        return -EINVAL;
    }
    match parse::parse_vcpu(kernel::bytes(buf, len)) {
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
    match parse::parse_mem(kernel::bytes(buf, len)) {
        Some(v) => {
            kernel::write_out(out, v);
            0
        }
        None => -EINVAL,
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

/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_is_deleting(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).is_deleting() as c_int
}

/// Open the lease.  Returns 1 on success, 0 if the machine is being deleted.
///
/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_lease_open(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).lease_open() as c_int
}

/// Close the lease.  Returns 1 if the machine must now be destroyed, else 0.
///
/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_lease_close(m: *mut MachineState) -> c_int {
    match kernel::handle_mut(m).lease_close() {
        machine::CloseAction::Delete => 1,
        machine::CloseAction::None => 0,
    }
}

/// Begin deletion (rmdir/unmount).  Returns 1 if newly started, 0 if already.
///
/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_begin_delete(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).begin_delete() as c_int
}

/// # Safety
/// `m` is a live handle from `vmmfs_machine_new`.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_events_pending(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).events_pending() as c_int
}

/// Drain queued events into `(buf, cap)`, returning the number of bytes written.
///
/// # Safety
/// `m` is a live handle; `buf` covers `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_read_events(
    m: *mut MachineState,
    buf: *mut u8,
    cap: usize,
) -> usize {
    kernel::handle_mut(m).read_events(kernel::bytes_mut(buf, cap))
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
