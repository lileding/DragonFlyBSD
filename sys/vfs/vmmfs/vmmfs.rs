#![no_std]

mod intrinsics;
mod kernel;
mod machine;

use core::ffi::c_int;
use core::panic::PanicInfo;
use kernel::KBox;
use machine::MachineState;

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
// C boundary shims.  These and kernel.rs are the only places that touch the C
// ABI; they delegate to the safe logic in machine.rs and to kernel.rs.
// ---------------------------------------------------------------------------

/// Create a machine (mkdir): stopped, no config yet.  Allocates zeroed and
/// initializes in place to avoid moving the large MachineState (see new_zeroed).
#[no_mangle]
pub extern "C" fn vmmfs_machine_new() -> *mut MachineState {
    // SAFETY: MachineState is POD (valid all-zero); init() sets it up in place.
    match unsafe { KBox::<MachineState>::new_zeroed() } {
        Some(b) => {
            let ptr = b.into_raw();
            // SAFETY: ptr is a freshly allocated, zeroed MachineState.
            unsafe { kernel::handle_mut(ptr).init() };
            ptr
        }
        None => core::ptr::null_mut(),
    }
}

/// # Safety: `m` is a handle from `vmmfs_machine_new`, not yet freed.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_free(m: *mut MachineState) {
    if !m.is_null() {
        drop(KBox::from_raw(m));
    }
}

// ---- desired config registers: commit (parse+update) and read-back ----

/// # Safety: `m` live; `buf` covers `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_commit_vcpu(
    m: *mut MachineState,
    buf: *const u8,
    len: usize,
) -> c_int {
    kernel::handle_mut(m).commit_vcpu(kernel::bytes(buf, len)) as c_int
}

/// # Safety: `m` live; `buf` covers `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_commit_mem(
    m: *mut MachineState,
    buf: *const u8,
    len: usize,
) -> c_int {
    kernel::handle_mut(m).commit_mem(kernel::bytes(buf, len)) as c_int
}

/// # Safety: `m` live; `buf` covers `len` bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_commit_loader(
    m: *mut MachineState,
    buf: *const u8,
    len: usize,
) -> c_int {
    kernel::handle_mut(m).commit_loader(kernel::bytes(buf, len)) as c_int
}

/// # Safety: `m` live; `buf` covers `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_vcpu_text(
    m: *mut MachineState,
    buf: *mut u8,
    cap: usize,
) -> usize {
    kernel::handle_mut(m).vcpu_text(kernel::bytes_mut(buf, cap))
}

/// # Safety: `m` live; `buf` covers `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_mem_text(
    m: *mut MachineState,
    buf: *mut u8,
    cap: usize,
) -> usize {
    kernel::handle_mut(m).mem_text(kernel::bytes_mut(buf, cap))
}

/// # Safety: `m` live; `buf` covers `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_loader_text(
    m: *mut MachineState,
    buf: *mut u8,
    cap: usize,
) -> usize {
    kernel::handle_mut(m).loader_text(kernel::bytes_mut(buf, cap))
}

/// Copy the loader path (no trailing newline) for start-time resolution.
/// # Safety: `m` live; `buf` covers `cap` writable bytes.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_loader_path(
    m: *mut MachineState,
    buf: *mut u8,
    cap: usize,
) -> usize {
    kernel::handle_mut(m).loader_path(kernel::bytes_mut(buf, cap))
}

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_config_complete(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).config_complete() as c_int
}

// ---- lifecycle ----

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_is_stopped(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).is_stopped() as c_int
}

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_stop(m: *mut MachineState, force: c_int) {
    kernel::handle_mut(m).stop(force != 0);
}

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_start(m: *mut MachineState) {
    kernel::handle_mut(m).start();
}

// ---- lease ----

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_is_deleting(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).is_deleting() as c_int
}

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_lease_open(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).lease_open() as c_int
}

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_lease_close(m: *mut MachineState) -> c_int {
    match kernel::handle_mut(m).lease_close() {
        machine::CloseAction::Delete => 1,
        machine::CloseAction::None => 0,
    }
}

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_begin_delete(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).begin_delete() as c_int
}

// ---- events ----

/// # Safety: `m` is a live handle.
#[no_mangle]
pub unsafe extern "C" fn vmmfs_machine_events_pending(m: *mut MachineState) -> c_int {
    kernel::handle_mut(m).events_pending() as c_int
}

/// # Safety: `m` live; `buf` covers `cap` writable bytes.
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
