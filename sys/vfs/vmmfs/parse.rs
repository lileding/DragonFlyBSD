//! Pure config-file parsing for vmmfs (`vcpu`, `mem`).
//!
//! No kernel dependencies, so this compiles both as a `mod` of the `#![no_std]`
//! kernel crate and standalone for host unit tests: `rustc --test parse.rs`.
//! Only inlinable, non-panicking slice ops are used (`.get()`, `.iter()`,
//! `split_last`) so the kernel object references no core panic helpers.

const VMMFS_VCPU_MAX: u64 = 256;
/// Guest-memory alignment requirement (2 MiB, large-page granularity).
const VMMFS_MEM_ALIGN: u64 = 2 * 1024 * 1024;

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
pub fn parse_vcpu(s: &[u8]) -> Option<u32> {
    match parse_decimal_u64(trim_ascii(s)) {
        Some(n) if (1..=VMMFS_VCPU_MAX).contains(&n) => Some(n as u32),
        _ => None,
    }
}

/// Parse the `mem` file: `number[KkMmGg]`, > 0, VMMFS_MEM_ALIGN-aligned.
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

#[cfg(test)]
mod tests {
    use super::*;

    const M: u64 = 1024 * 1024;
    const G: u64 = 1024 * 1024 * 1024;

    #[test]
    fn vcpu_basic() {
        assert_eq!(parse_vcpu(b"4"), Some(4));
        assert_eq!(parse_vcpu(b"4\n"), Some(4));
        assert_eq!(parse_vcpu(b"  4  \n"), Some(4));
        assert_eq!(parse_vcpu(b"1"), Some(1));
    }

    #[test]
    fn vcpu_bounds() {
        assert_eq!(parse_vcpu(b"0"), None);
        assert_eq!(parse_vcpu(b"256"), Some(256));
        assert_eq!(parse_vcpu(b"257"), None);
    }

    #[test]
    fn vcpu_garbage() {
        assert_eq!(parse_vcpu(b""), None);
        assert_eq!(parse_vcpu(b"  "), None);
        assert_eq!(parse_vcpu(b"4x"), None);
        assert_eq!(parse_vcpu(b"x4"), None);
        assert_eq!(parse_vcpu(b"-1"), None);
    }

    #[test]
    fn mem_units() {
        assert_eq!(parse_mem(b"2097152"), Some(2 * M)); // raw bytes
        assert_eq!(parse_mem(b"2M"), Some(2 * M));
        assert_eq!(parse_mem(b"512M\n"), Some(512 * M));
        assert_eq!(parse_mem(b"1G"), Some(G));
        assert_eq!(parse_mem(b"1048576K"), Some(1048576 * 1024));
        assert_eq!(parse_mem(b"2m"), Some(2 * M));
        assert_eq!(parse_mem(b"1g"), Some(G));
    }

    #[test]
    fn mem_alignment() {
        assert_eq!(parse_mem(b"1M"), None); // 1 MiB not 2 MiB-aligned
        assert_eq!(parse_mem(b"1024K"), None); // 1 MiB
        assert_eq!(parse_mem(b"1048576"), None); // 1 MiB in bytes
    }

    #[test]
    fn mem_garbage() {
        assert_eq!(parse_mem(b""), None);
        assert_eq!(parse_mem(b"0"), None);
        assert_eq!(parse_mem(b"0M"), None);
        assert_eq!(parse_mem(b"512Q"), None);
        assert_eq!(parse_mem(b"M"), None);
        assert_eq!(parse_mem(b"xM"), None);
    }

    #[test]
    fn mem_no_overflow() {
        // would overflow u64 when scaled
        assert_eq!(parse_mem(b"99999999999999999999G"), None);
    }
}
