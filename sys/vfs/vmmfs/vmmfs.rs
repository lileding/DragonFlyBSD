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

#[panic_handler]
fn panic(_info: &PanicInfo<'_>) -> ! {
    loop {
        core::hint::spin_loop();
    }
}
