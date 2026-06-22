//! Pure machine lifecycle state machine for vmmfs.
//!
//! No kernel dependencies, so it compiles both as a `mod` of the `#![no_std]`
//! kernel crate and standalone for host unit tests: `rustc --test machine.rs`.
//!
//! The control surface exposes only the stable desired state {Running, Stopped}
//! with no intermediate state; the loader is stubbed so transitions are instant.
//! The async stopping transition for a real guest is a later addition.

pub struct MachineState {
    stopped: bool,
}

impl MachineState {
    pub fn new(stopped: bool) -> MachineState {
        MachineState { stopped }
    }

    pub fn is_stopped(&self) -> bool {
        self.stopped
    }

    /// Stop (apic graceful or force).  Idempotent; `force` selects the method
    /// for the future real-guest path.
    pub fn stop(&mut self, _force: bool) {
        self.stopped = true;
    }

    /// Start.  Idempotent.
    pub fn start(&mut self) {
        self.stopped = false;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn import_running() {
        let m = MachineState::new(false);
        assert!(!m.is_stopped());
    }

    #[test]
    fn import_stopped() {
        let m = MachineState::new(true);
        assert!(m.is_stopped());
    }

    #[test]
    fn stop_then_start() {
        let mut m = MachineState::new(false);
        m.stop(false);
        assert!(m.is_stopped());
        m.start();
        assert!(!m.is_stopped());
    }

    #[test]
    fn stop_idempotent() {
        let mut m = MachineState::new(false);
        m.stop(false);
        m.stop(true); // force, already stopped
        assert!(m.is_stopped());
    }

    #[test]
    fn start_idempotent() {
        let mut m = MachineState::new(true);
        m.start();
        m.start();
        assert!(!m.is_stopped());
    }
}
