#![no_std]

mod intrinsics;
mod kernel;
mod machine;

use core::ffi::c_int;
use core::panic::PanicInfo;
use kernel::KBox;
use machine::MachineState;

const LOAD_MESSAGE: &[u8] = b"vmm: core loaded\n\0";
const UNLOAD_MESSAGE: &[u8] = b"vmm: core unloaded\n\0";

#[no_mangle]
pub extern "C" fn vmm_init() -> c_int {
    kernel::kputs(LOAD_MESSAGE);
    0
}

#[no_mangle]
pub extern "C" fn vmm_fini() {
    kernel::kputs(UNLOAD_MESSAGE);
}

// ===========================================================================
// C ABI for the VMM core.
//
// These are the only entry points the vmmfs control plane (the C VFS layer)
// calls into the vmm core.  All machine logic lives in `machine.rs` as plain
// safe Rust; the unsafe raw-pointer primitives are confined to `kernel.rs`.
// Each shim below is a thin adapter: it converts the C-provided handle/buffers
// to safe Rust via one audited `kernel::` helper, then calls a safe method.
//
// Shared SAFETY contract for every `m: *mut MachineState` below: `m` is a live
// handle returned by `vmm_machine_new` and not yet freed, and any `(buf, len)`
// names readable/writable memory of that length.  The C caller upholds this.
// ===========================================================================

/// Create a machine (mkdir): stopped, no config yet.
#[no_mangle]
pub extern "C" fn vmm_machine_new() -> *mut MachineState {
    // SAFETY: MachineState is POD (valid all-zero); we initialize it in place
    // before handing the pointer back, so it is never observed uninitialized.
    unsafe {
        match KBox::<MachineState>::new_zeroed() {
            Some(b) => {
                let ptr = b.into_raw();
                kernel::handle_mut(ptr).init();
                ptr
            }
            None => core::ptr::null_mut(),
        }
    }
}

#[no_mangle]
pub extern "C" fn vmm_machine_free(m: *mut MachineState) {
    if !m.is_null() {
        // SAFETY: m came from vmm_machine_new and is freed exactly once.
        unsafe { drop(KBox::from_raw(m)) };
    }
}

// ---- desired-config registers: commit (parse+update) and read-back ----

#[no_mangle]
pub extern "C" fn vmm_machine_commit_vcpu(m: *mut MachineState, buf: *const u8, len: usize) -> c_int {
    // SAFETY: see the shared contract above.
    let (st, text) = unsafe { (kernel::handle_mut(m), kernel::bytes(buf, len)) };
    st.commit_vcpu(text) as c_int
}

#[no_mangle]
pub extern "C" fn vmm_machine_commit_mem(m: *mut MachineState, buf: *const u8, len: usize) -> c_int {
    let (st, text) = unsafe { (kernel::handle_mut(m), kernel::bytes(buf, len)) };
    st.commit_mem(text) as c_int
}

#[no_mangle]
pub extern "C" fn vmm_machine_commit_loader(m: *mut MachineState, buf: *const u8, len: usize) -> c_int {
    let (st, text) = unsafe { (kernel::handle_mut(m), kernel::bytes(buf, len)) };
    st.commit_loader(text) as c_int
}

#[no_mangle]
pub extern "C" fn vmm_machine_vcpu_text(m: *mut MachineState, buf: *mut u8, cap: usize) -> usize {
    let (st, out) = unsafe { (kernel::handle_mut(m), kernel::bytes_mut(buf, cap)) };
    st.vcpu_text(out)
}

#[no_mangle]
pub extern "C" fn vmm_machine_mem_text(m: *mut MachineState, buf: *mut u8, cap: usize) -> usize {
    let (st, out) = unsafe { (kernel::handle_mut(m), kernel::bytes_mut(buf, cap)) };
    st.mem_text(out)
}

#[no_mangle]
pub extern "C" fn vmm_machine_loader_text(m: *mut MachineState, buf: *mut u8, cap: usize) -> usize {
    let (st, out) = unsafe { (kernel::handle_mut(m), kernel::bytes_mut(buf, cap)) };
    st.loader_text(out)
}

/// Copy the loader path (no trailing newline) for start-time resolution.
#[no_mangle]
pub extern "C" fn vmm_machine_loader_path(m: *mut MachineState, buf: *mut u8, cap: usize) -> usize {
    let (st, out) = unsafe { (kernel::handle_mut(m), kernel::bytes_mut(buf, cap)) };
    st.loader_path(out)
}

#[no_mangle]
pub extern "C" fn vmm_machine_config_complete(m: *mut MachineState) -> c_int {
    unsafe { kernel::handle_mut(m) }.config_complete() as c_int
}

// ---- lifecycle ----

#[no_mangle]
pub extern "C" fn vmm_machine_is_stopped(m: *mut MachineState) -> c_int {
    unsafe { kernel::handle_mut(m) }.is_stopped() as c_int
}

#[no_mangle]
pub extern "C" fn vmm_machine_stop(m: *mut MachineState, force: c_int) {
    unsafe { kernel::handle_mut(m) }.stop(force != 0);
}

#[no_mangle]
pub extern "C" fn vmm_machine_start(m: *mut MachineState) {
    unsafe { kernel::handle_mut(m) }.start();
}

// ---- lease ----

#[no_mangle]
pub extern "C" fn vmm_machine_is_deleting(m: *mut MachineState) -> c_int {
    unsafe { kernel::handle_mut(m) }.is_deleting() as c_int
}

#[no_mangle]
pub extern "C" fn vmm_machine_lease_open(m: *mut MachineState) -> c_int {
    unsafe { kernel::handle_mut(m) }.lease_open() as c_int
}

#[no_mangle]
pub extern "C" fn vmm_machine_lease_close(m: *mut MachineState) -> c_int {
    match unsafe { kernel::handle_mut(m) }.lease_close() {
        machine::CloseAction::Delete => 1,
        machine::CloseAction::None => 0,
    }
}

#[no_mangle]
pub extern "C" fn vmm_machine_begin_delete(m: *mut MachineState) -> c_int {
    unsafe { kernel::handle_mut(m) }.begin_delete() as c_int
}

// ---- events ----

#[no_mangle]
pub extern "C" fn vmm_machine_events_pending(m: *mut MachineState) -> c_int {
    unsafe { kernel::handle_mut(m) }.events_pending() as c_int
}

#[no_mangle]
pub extern "C" fn vmm_machine_read_events(m: *mut MachineState, buf: *mut u8, cap: usize) -> usize {
    let (st, out) = unsafe { (kernel::handle_mut(m), kernel::bytes_mut(buf, cap)) };
    st.read_events(out)
}

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
