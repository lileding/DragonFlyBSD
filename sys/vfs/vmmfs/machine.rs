//! Pure machine lifecycle state machine for vmmfs.
//!
//! No kernel dependencies, so it compiles both as a `mod` of the `#![no_std]`
//! kernel crate and standalone for host unit tests: `rustc --test machine.rs`.
//!
//! The control surface exposes only the stable desired state {Running, Stopped}
//! with no intermediate state; the loader is stubbed so transitions are instant.
//! The async stopping transition for a real guest is a later addition.

/// What the caller (C) must do after a lease close.
#[derive(PartialEq, Debug)]
pub enum CloseAction {
    /// Nothing further.
    None,
    /// This was the last lease of an armed machine: destroy it (source 3).
    Delete,
}

pub struct MachineState {
    stopped: bool,
    lease_count: u32,
    armed: bool,    // the lease has been opened at least once
    deleting: bool, // in the deletion flow; the lease can no longer be opened
}

impl MachineState {
    pub fn new(stopped: bool) -> MachineState {
        MachineState {
            stopped,
            lease_count: 0,
            armed: false,
            deleting: false,
        }
    }

    pub fn is_stopped(&self) -> bool {
        self.stopped
    }

    pub fn is_deleting(&self) -> bool {
        self.deleting
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

    /// Open the lease.  Returns false if the machine is already being deleted
    /// (the lease can no longer be opened); otherwise arms it and bumps the
    /// reference count.
    pub fn lease_open(&mut self) -> bool {
        if self.deleting {
            return false;
        }
        self.lease_count += 1;
        self.armed = true;
        true
    }

    /// Close the lease.  Returns `Delete` when the last lease of an armed
    /// machine is released (source 3: refcount -> 0 triggers deletion).
    pub fn lease_close(&mut self) -> CloseAction {
        if self.lease_count > 0 {
            self.lease_count -= 1;
        }
        if self.armed && self.lease_count == 0 && !self.deleting {
            self.deleting = true;
            CloseAction::Delete
        } else {
            CloseAction::None
        }
    }

    /// Begin deletion from rmdir or unmount (sources 1/2), regardless of lease
    /// state.  Returns false if deletion was already in progress.
    pub fn begin_delete(&mut self) -> bool {
        if self.deleting {
            return false;
        }
        self.deleting = true;
        true
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

    #[test]
    fn lease_single_open_close_deletes() {
        let mut m = MachineState::new(false);
        assert!(m.lease_open());
        assert_eq!(m.lease_close(), CloseAction::Delete);
        assert!(m.is_deleting());
    }

    #[test]
    fn lease_refcount() {
        let mut m = MachineState::new(false);
        assert!(m.lease_open());
        assert!(m.lease_open());
        assert!(m.lease_open());
        assert_eq!(m.lease_close(), CloseAction::None);
        assert_eq!(m.lease_close(), CloseAction::None);
        assert_eq!(m.lease_close(), CloseAction::Delete);
    }

    #[test]
    fn lease_never_opened_does_not_delete() {
        // A machine that was never leased must not self-delete.
        let mut m = MachineState::new(false);
        assert_eq!(m.lease_close(), CloseAction::None);
        assert!(!m.is_deleting());
    }

    #[test]
    fn lease_open_fails_while_deleting() {
        let mut m = MachineState::new(true);
        assert!(m.begin_delete());
        assert!(!m.lease_open()); // cannot open during deletion
    }

    #[test]
    fn begin_delete_then_lease_close_no_double() {
        let mut m = MachineState::new(false);
        assert!(m.lease_open());
        assert!(m.begin_delete()); // e.g. rmdir while leased
        // the lease's eventual close must not trigger a second deletion
        assert_eq!(m.lease_close(), CloseAction::None);
    }

    #[test]
    fn begin_delete_idempotent() {
        let mut m = MachineState::new(true);
        assert!(m.begin_delete());
        assert!(!m.begin_delete());
    }
}
