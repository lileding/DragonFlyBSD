//! Pure machine model for vmmfs: config parsing, the desired-state config, the
//! lifecycle state machine, lease reference counting, and the event ring.
//!
//! No kernel dependencies, so it compiles both as a `mod` of the `#![no_std]`
//! kernel crate and standalone for host unit tests: `rustc --test machine.rs`.
//! Only inlinable, non-panicking ops (`.get()` / `.get_mut()` / iterators, no
//! `[i]` indexing) are used so the kernel object references no core panic
//! helpers.
//!
//! Config files behave like hardware registers: a reader sees the current
//! desired value serialized as text; the C side buffers writes and, on close,
//! commits the whole buffer atomically via `commit_*` (parse + update if valid,
//! else leave unchanged).  The values are DESIRED state — the system strives
//! toward them but may never reach them; the observed state is states.tar.gz.

// --------------------------------------------------------------------------
// Config value parsing (pure).
// --------------------------------------------------------------------------

const VMMFS_VCPU_MAX: u64 = 256;
/// Guest-memory alignment requirement (2 MiB, large-page granularity).
const VMMFS_MEM_ALIGN: u64 = 2 * 1024 * 1024;
const LOADER_MAX: usize = 256;

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

/// Parse the `vcpu` value: decimal integer, 1..=VMMFS_VCPU_MAX.
pub fn parse_vcpu(s: &[u8]) -> Option<u32> {
    match parse_decimal_u64(trim_ascii(s)) {
        Some(n) if (1..=VMMFS_VCPU_MAX).contains(&n) => Some(n as u32),
        _ => None,
    }
}

/// Parse the `mem` value: `number[KkMmGg]`, > 0, VMMFS_MEM_ALIGN-aligned.
pub fn parse_mem(s: &[u8]) -> Option<u64> {
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

// --------------------------------------------------------------------------
// Event ring (lossy, shared one-shot, text materialized at read).
// --------------------------------------------------------------------------

const EVENT_CAP: usize = 32;
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

struct EventRing {
    codes: [u8; EVENT_CAP],
    tail: usize,
    count: usize,
}

impl EventRing {
    #[cfg(test)]
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
            self.tail = (self.tail + 1) % EVENT_CAP;
        }
    }

    fn has_events(&self) -> bool {
        self.count > 0
    }

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
                None => break,
            }
            n = end;
            self.tail = (self.tail + 1) % EVENT_CAP;
            self.count -= 1;
        }
        n
    }
}

/// Write a decimal u64 to `out` followed by '\n'; returns bytes written (0 if
/// it does not fit).  Avoids any formatting machinery / panics.
fn write_decimal(mut v: u64, out: &mut [u8]) -> usize {
    let mut tmp = [0u8; 20];
    let mut i = tmp.len();
    loop {
        i -= 1;
        if let Some(d) = tmp.get_mut(i) {
            *d = b'0' + (v % 10) as u8;
        }
        v /= 10;
        if v == 0 {
            break;
        }
    }
    let digits = tmp.get(i..).unwrap_or(&[]);
    let need = digits.len() + 1;
    match out.get_mut(..need) {
        Some(dst) => {
            for (d, s) in dst.iter_mut().zip(digits.iter()) {
                *d = *s;
            }
            if let Some(nl) = out.get_mut(need - 1) {
                *nl = b'\n';
            }
            need
        }
        None => 0,
    }
}

// --------------------------------------------------------------------------
// Machine state.
// --------------------------------------------------------------------------

pub struct MachineState {
    stopped: bool,
    vcpu: u32, // 0 = unset
    mem: u64,  // 0 = unset
    loader: [u8; LOADER_MAX],
    loader_len: usize, // 0 = unset
    lease_count: u32,
    armed: bool,
    deleting: bool,
    events: EventRing,
}

impl MachineState {
    /// Initialize a freshly zeroed machine (mkdir) in place: stopped, no config.
    ///
    /// The backing memory is already zeroed (KBox::new_zeroed), giving every
    /// false/0 default including an empty event ring; we only raise the stopped
    /// flag and push the creation events.  In-place init avoids moving the
    /// (large, 256-byte loader) MachineState through a stack copy, which LLVM
    /// would lower to a memcpy the kernel loader cannot relocate.
    pub fn init(&mut self) {
        self.stopped = true;
        self.events.push(EV_CREATED);
        self.events.push(EV_STOPPED);
    }

    /// Test-only constructor (host std build): build a zeroed instance and init.
    #[cfg(test)]
    pub fn new() -> MachineState {
        let mut m = MachineState {
            stopped: false,
            vcpu: 0,
            mem: 0,
            loader: [0u8; LOADER_MAX],
            loader_len: 0,
            lease_count: 0,
            armed: false,
            deleting: false,
            events: EventRing::new(),
        };
        m.init();
        m
    }

    // ---- desired config: commit (parse+update) and read-back ----

    /// Commit the `vcpu` buffer at close: update the desired value iff valid.
    /// Returns whether the value was updated.
    pub fn commit_vcpu(&mut self, text: &[u8]) -> bool {
        match parse_vcpu(text) {
            Some(v) => {
                self.vcpu = v;
                true
            }
            None => false,
        }
    }

    pub fn commit_mem(&mut self, text: &[u8]) -> bool {
        match parse_mem(text) {
            Some(v) => {
                self.mem = v;
                true
            }
            None => false,
        }
    }

    /// Commit the `loader` buffer: store the trimmed path iff non-empty and it
    /// fits.  (Path existence/executability is checked at start time.)
    pub fn commit_loader(&mut self, text: &[u8]) -> bool {
        let p = trim_ascii(text);
        if p.is_empty() || p.len() > LOADER_MAX {
            return false;
        }
        if let Some(dst) = self.loader.get_mut(..p.len()) {
            for (d, s) in dst.iter_mut().zip(p.iter()) {
                *d = *s;
            }
            self.loader_len = p.len();
            true
        } else {
            false
        }
    }

    /// Serialize the current desired `vcpu` ("N\n", or empty if unset).
    pub fn vcpu_text(&self, out: &mut [u8]) -> usize {
        if self.vcpu == 0 {
            0
        } else {
            write_decimal(self.vcpu as u64, out)
        }
    }

    pub fn mem_text(&self, out: &mut [u8]) -> usize {
        if self.mem == 0 {
            0
        } else {
            write_decimal(self.mem, out)
        }
    }

    pub fn loader_text(&self, out: &mut [u8]) -> usize {
        let path = self.loader.get(..self.loader_len).unwrap_or(&[]);
        let need = self.loader_len + 1;
        match out.get_mut(..need) {
            Some(dst) => {
                for (d, s) in dst.iter_mut().zip(path.iter()) {
                    *d = *s;
                }
                if let Some(nl) = out.get_mut(self.loader_len) {
                    *nl = b'\n';
                }
                if self.loader_len == 0 {
                    0
                } else {
                    need
                }
            }
            None => 0,
        }
    }

    /// Copy the loader path (no trailing newline) into `out`; returns its length
    /// (0 if unset).  Used by the start path to resolve the loader.
    pub fn loader_path(&self, out: &mut [u8]) -> usize {
        let path = self.loader.get(..self.loader_len).unwrap_or(&[]);
        if let Some(dst) = out.get_mut(..self.loader_len) {
            for (d, s) in dst.iter_mut().zip(path.iter()) {
                *d = *s;
            }
        }
        self.loader_len
    }

    /// All required desired config is set (vcpu, mem, loader).
    pub fn config_complete(&self) -> bool {
        self.vcpu != 0 && self.mem != 0 && self.loader_len != 0
    }

    // ---- lifecycle ----

    pub fn is_stopped(&self) -> bool {
        self.stopped
    }

    pub fn is_deleting(&self) -> bool {
        self.deleting
    }

    pub fn stop(&mut self, _force: bool) {
        if !self.stopped {
            self.stopped = true;
            self.events.push(EV_STOPPED);
        }
    }

    pub fn start(&mut self) {
        if self.stopped {
            self.stopped = false;
            self.events.push(EV_STARTED);
        }
    }

    // ---- lease ----

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

    pub fn begin_delete(&mut self) -> bool {
        if self.deleting {
            return false;
        }
        self.deleting = true;
        self.events.push(EV_DELETED);
        true
    }

    // ---- events ----

    pub fn events_pending(&self) -> bool {
        self.events.has_events()
    }

    pub fn read_events(&mut self, out: &mut [u8]) -> usize {
        self.events.read(out)
    }
}

/// What the caller (C) must do after a lease close.
#[derive(PartialEq, Debug)]
pub enum CloseAction {
    None,
    Delete,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn drain(m: &mut MachineState) -> String {
        let mut buf = [0u8; 1024];
        let n = m.read_events(&mut buf);
        String::from_utf8_lossy(&buf[..n]).into_owned()
    }

    fn vcpu_str(m: &MachineState) -> String {
        let mut b = [0u8; 32];
        let n = m.vcpu_text(&mut b);
        String::from_utf8_lossy(&b[..n]).into_owned()
    }
    fn mem_str(m: &MachineState) -> String {
        let mut b = [0u8; 32];
        let n = m.mem_text(&mut b);
        String::from_utf8_lossy(&b[..n]).into_owned()
    }
    fn loader_str(m: &MachineState) -> String {
        let mut b = [0u8; 512];
        let n = m.loader_text(&mut b);
        String::from_utf8_lossy(&b[..n]).into_owned()
    }

    // --- parsing ---
    #[test]
    fn parse_vcpu_basic() {
        assert_eq!(parse_vcpu(b"4\n"), Some(4));
        assert_eq!(parse_vcpu(b"0"), None);
        assert_eq!(parse_vcpu(b"256"), Some(256));
        assert_eq!(parse_vcpu(b"257"), None);
        assert_eq!(parse_vcpu(b"x"), None);
    }

    #[test]
    fn parse_mem_basic() {
        assert_eq!(parse_mem(b"512M\n"), Some(512 * 1024 * 1024));
        assert_eq!(parse_mem(b"1G"), Some(1024 * 1024 * 1024));
        assert_eq!(parse_mem(b"1M"), None); // not 2 MiB aligned
        assert_eq!(parse_mem(b"512Q"), None);
        assert_eq!(parse_mem(b"0"), None);
    }

    // --- config registers: commit + read-back ---
    #[test]
    fn config_unset_reads_empty() {
        let m = MachineState::new();
        assert_eq!(vcpu_str(&m), "");
        assert_eq!(mem_str(&m), "");
        assert_eq!(loader_str(&m), "");
        assert!(!m.config_complete());
    }

    #[test]
    fn config_commit_and_readback() {
        let mut m = MachineState::new();
        assert!(m.commit_vcpu(b"4\n"));
        assert_eq!(vcpu_str(&m), "4\n");
        assert!(m.commit_mem(b"512M"));
        assert_eq!(mem_str(&m), "536870912\n");
        assert!(m.commit_loader(b"/bin/sh\n"));
        assert_eq!(loader_str(&m), "/bin/sh\n");
        assert!(m.config_complete());
    }

    #[test]
    fn config_invalid_commit_keeps_old() {
        let mut m = MachineState::new();
        assert!(m.commit_vcpu(b"4"));
        assert!(!m.commit_vcpu(b"0")); // invalid: rejected
        assert_eq!(vcpu_str(&m), "4\n"); // unchanged
        assert!(!m.commit_mem(b"3M")); // not aligned
        assert_eq!(mem_str(&m), ""); // never set
        assert!(!m.commit_loader(b"   ")); // empty path
    }

    #[test]
    fn config_complete_needs_all_three() {
        let mut m = MachineState::new();
        m.commit_vcpu(b"2");
        assert!(!m.config_complete());
        m.commit_mem(b"2M");
        assert!(!m.config_complete());
        m.commit_loader(b"/x");
        assert!(m.config_complete());
    }

    // --- lifecycle ---
    #[test]
    fn new_is_stopped() {
        assert!(MachineState::new().is_stopped());
    }

    #[test]
    fn stop_then_start() {
        let mut m = MachineState::new();
        m.start();
        assert!(!m.is_stopped());
        m.stop(false);
        assert!(m.is_stopped());
    }

    #[test]
    fn start_stop_idempotent() {
        let mut m = MachineState::new();
        m.start();
        m.start();
        assert!(!m.is_stopped());
        m.stop(false);
        m.stop(true);
        assert!(m.is_stopped());
    }

    // --- lease ---
    #[test]
    fn lease_single_open_close_deletes() {
        let mut m = MachineState::new();
        assert!(m.lease_open());
        assert_eq!(m.lease_close(), CloseAction::Delete);
        assert!(m.is_deleting());
    }

    #[test]
    fn lease_refcount() {
        let mut m = MachineState::new();
        assert!(m.lease_open());
        assert!(m.lease_open());
        assert_eq!(m.lease_close(), CloseAction::None);
        assert_eq!(m.lease_close(), CloseAction::Delete);
    }

    #[test]
    fn lease_never_opened_no_delete() {
        let mut m = MachineState::new();
        assert_eq!(m.lease_close(), CloseAction::None);
        assert!(!m.is_deleting());
    }

    #[test]
    fn lease_open_fails_while_deleting() {
        let mut m = MachineState::new();
        assert!(m.begin_delete());
        assert!(!m.lease_open());
    }

    #[test]
    fn begin_delete_then_lease_close_no_double() {
        let mut m = MachineState::new();
        assert!(m.lease_open());
        assert!(m.begin_delete());
        assert_eq!(m.lease_close(), CloseAction::None);
    }

    // --- events ---
    #[test]
    fn events_on_create() {
        let mut m = MachineState::new();
        assert_eq!(drain(&mut m), "created\nstopped\n");
    }

    #[test]
    fn events_lifecycle_oneshot() {
        let mut m = MachineState::new();
        let _ = drain(&mut m);
        assert!(!m.events_pending());
        m.start();
        m.stop(false);
        assert_eq!(drain(&mut m), "started\nstopped\n");
    }

    #[test]
    fn events_idempotent_no_dup() {
        let mut m = MachineState::new();
        let _ = drain(&mut m);
        m.stop(false); // already stopped
        assert!(!m.events_pending());
    }

    #[test]
    fn events_delete() {
        let mut m = MachineState::new();
        let _ = drain(&mut m);
        m.begin_delete();
        assert_eq!(drain(&mut m), "deleted\n");
    }

    #[test]
    fn events_lossy_overflow() {
        let mut m = MachineState::new();
        let _ = drain(&mut m);
        for _ in 0..100 {
            m.start();
            m.stop(false);
        }
        let mut buf = [0u8; 4096];
        let n = m.read_events(&mut buf);
        let s = String::from_utf8_lossy(&buf[..n]).into_owned();
        let c = s.matches('\n').count();
        assert!(c > 0 && c <= EVENT_CAP);
    }
}
