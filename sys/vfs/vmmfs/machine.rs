//! Pure machine lifecycle state machine for vmmfs.
//!
//! No kernel dependencies, so it compiles both as a `mod` of the `#![no_std]`
//! kernel crate and standalone for host unit tests: `rustc --test machine.rs`.
//! Only inlinable, non-panicking ops (`.get()` / `.get_mut()` / iterators, no
//! `[i]` indexing) are used so the kernel object references no core panic
//! helpers.
//!
//! The control surface exposes only the stable desired state {Running, Stopped}
//! with no intermediate state; the loader is stubbed so transitions are instant.

/// What the caller (C) must do after a lease close.
#[derive(PartialEq, Debug)]
pub enum CloseAction {
    None,
    /// Last lease of an armed machine: destroy it (source 3).
    Delete,
}

const EVENT_CAP: usize = 32; // ring capacity in events

// Event codes.  The ring stores codes (tiny) and the text is materialized at
// read time; a large char array would make LLVM emit memcpy/memset via GOT,
// which the kernel object loader cannot relocate.
const EV_CREATED: u8 = 1;
const EV_STARTED: u8 = 2;
const EV_STOPPED: u8 = 3;
const EV_DELETED: u8 = 4;

fn event_text(code: u8) -> &'static [u8] {
    match code {
        EV_CREATED => b"created\n",
        EV_STARTED => b"started\n",
        EV_STOPPED => b"stopped\n",
        EV_DELETED => b"deleted\n",
        _ => b"",
    }
}

/// A fixed-size, lossy, shared one-shot ring of event codes.  Producers append;
/// a read drains and consumes queued events as text lines (shared cursor —
/// competing readers race).  On overflow the oldest unconsumed event is
/// dropped; keeping up is the reader's responsibility.
struct EventRing {
    codes: [u8; EVENT_CAP],
    tail: usize,
    count: usize,
}

impl EventRing {
    fn new() -> EventRing {
        EventRing {
            codes: [0u8; EVENT_CAP],
            tail: 0,
            count: 0,
        }
    }

    fn push(&mut self, code: u8) {
        let slot = (self.tail + self.count) % EVENT_CAP;
        if let Some(c) = self.codes.get_mut(slot) {
            *c = code;
        }
        if self.count < EVENT_CAP {
            self.count += 1;
        } else {
            // ring full: drop the oldest (lossy)
            self.tail = (self.tail + 1) % EVENT_CAP;
        }
    }

    fn has_events(&self) -> bool {
        self.count > 0
    }

    /// Drain queued events as text lines into `out`, consuming them, up to what
    /// fits.  Returns the number of bytes written.
    fn read(&mut self, out: &mut [u8]) -> usize {
        let mut n = 0usize;
        while self.count > 0 {
            let code = self.codes.get(self.tail).copied().unwrap_or(0);
            let text = event_text(code);
            let end = n + text.len();
            match out.get_mut(n..end) {
                Some(dst) => {
                    for (d, s) in dst.iter_mut().zip(text.iter()) {
                        *d = *s;
                    }
                }
                // does not fit: leave it for the next read
                None => break,
            }
            n = end;
            self.tail = (self.tail + 1) % EVENT_CAP;
            self.count -= 1;
        }
        n
    }
}

pub struct MachineState {
    stopped: bool,
    lease_count: u32,
    armed: bool,
    deleting: bool,
    events: EventRing,
}

impl MachineState {
    pub fn new(stopped: bool) -> MachineState {
        let mut m = MachineState {
            stopped,
            lease_count: 0,
            armed: false,
            deleting: false,
            events: EventRing::new(),
        };
        m.events.push(EV_CREATED);
        m.events.push(if stopped { EV_STOPPED } else { EV_STARTED });
        m
    }

    pub fn is_stopped(&self) -> bool {
        self.stopped
    }

    pub fn is_deleting(&self) -> bool {
        self.deleting
    }

    /// Stop (apic graceful or force).  Idempotent: emits an event only on an
    /// actual transition.
    pub fn stop(&mut self, _force: bool) {
        if !self.stopped {
            self.stopped = true;
            self.events.push(EV_STOPPED);
        }
    }

    /// Start.  Idempotent.
    pub fn start(&mut self) {
        if self.stopped {
            self.stopped = false;
            self.events.push(EV_STARTED);
        }
    }

    pub fn lease_open(&mut self) -> bool {
        if self.deleting {
            return false;
        }
        self.lease_count += 1;
        self.armed = true;
        true
    }

    pub fn lease_close(&mut self) -> CloseAction {
        if self.lease_count > 0 {
            self.lease_count -= 1;
        }
        if self.armed && self.lease_count == 0 && !self.deleting {
            self.deleting = true;
            self.events.push(EV_DELETED);
            CloseAction::Delete
        } else {
            CloseAction::None
        }
    }

    /// Begin deletion from rmdir or unmount (sources 1/2).  Returns false if
    /// deletion was already in progress.
    pub fn begin_delete(&mut self) -> bool {
        if self.deleting {
            return false;
        }
        self.deleting = true;
        self.events.push(EV_DELETED);
        true
    }

    pub fn events_pending(&self) -> bool {
        self.events.has_events()
    }

    /// Drain queued events into `out`; returns bytes written.
    pub fn read_events(&mut self, out: &mut [u8]) -> usize {
        self.events.read(out)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // The test build is a std crate (`rustc --test`), so String is in prelude;
    // the kernel build excludes this whole module via cfg(test).
    fn drain(m: &mut MachineState) -> String {
        let mut buf = [0u8; 1024];
        let n = m.read_events(&mut buf);
        String::from_utf8_lossy(&buf[..n]).into_owned()
    }

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
        m.stop(true);
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
        let mut m = MachineState::new(false);
        assert_eq!(m.lease_close(), CloseAction::None);
        assert!(!m.is_deleting());
    }

    #[test]
    fn lease_open_fails_while_deleting() {
        let mut m = MachineState::new(true);
        assert!(m.begin_delete());
        assert!(!m.lease_open());
    }

    #[test]
    fn begin_delete_then_lease_close_no_double() {
        let mut m = MachineState::new(false);
        assert!(m.lease_open());
        assert!(m.begin_delete());
        assert_eq!(m.lease_close(), CloseAction::None);
    }

    #[test]
    fn begin_delete_idempotent() {
        let mut m = MachineState::new(true);
        assert!(m.begin_delete());
        assert!(!m.begin_delete());
    }

    #[test]
    fn events_on_create_running() {
        let mut m = MachineState::new(false);
        assert_eq!(drain(&mut m), "created\nstarted\n");
    }

    #[test]
    fn events_on_create_stopped() {
        let mut m = MachineState::new(true);
        assert_eq!(drain(&mut m), "created\nstopped\n");
    }

    #[test]
    fn events_lifecycle_and_oneshot() {
        let mut m = MachineState::new(false);
        assert_eq!(drain(&mut m), "created\nstarted\n");
        // already drained: one-shot, so empty now
        assert!(!m.events_pending());
        m.stop(false);
        m.start();
        assert_eq!(drain(&mut m), "stopped\nstarted\n");
    }

    #[test]
    fn events_idempotent_no_dup() {
        let mut m = MachineState::new(true); // created\nstopped
        let _ = drain(&mut m);
        m.stop(false); // already stopped -> no event
        assert!(!m.events_pending());
    }

    #[test]
    fn events_delete() {
        let mut m = MachineState::new(true);
        let _ = drain(&mut m);
        m.begin_delete();
        assert_eq!(drain(&mut m), "deleted\n");
    }

    #[test]
    fn events_lossy_overflow() {
        let mut m = MachineState::new(false);
        let _ = drain(&mut m);
        // produce more than EVENT_CAP transitions; oldest dropped, newest kept
        for _ in 0..100 {
            m.stop(false);
            m.start();
        }
        let mut buf = [0u8; 4096];
        let n = m.read_events(&mut buf);
        // the ring holds at most EVENT_CAP events
        let s = String::from_utf8_lossy(&buf[..n]).into_owned();
        let count = s.matches('\n').count();
        assert!(count <= EVENT_CAP);
        assert!(count > 0);
    }
}
