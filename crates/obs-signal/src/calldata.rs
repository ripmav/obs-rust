//! `calldata` — the typed-less parameter stack (S-002;
//! `libobs/callback/calldata.{c,h}`).
//!
//! An ordered collection of named byte payloads in one contiguous
//! `stack`: `record* terminator`, where
//! `record = name_size : size_t, name : name_size (incl. NUL),
//! data_size : size_t, data : data_size`. All `size_t` fields are stored
//! in native byte order, **unaligned** (memcpy-style access; never
//! pointer-cast). **There is no type tag** (Q-1): a value's "type" is
//! only its byte count, so `get_int`/`get_float`/`get_ptr` (W = 8)
//! succeed on each other's payloads.
//!
//! The three exported C functions are [`calldata_get_data`],
//! [`calldata_set_data`] and [`calldata_get_string`]; the
//! header-inline behaviour (`init`, `init_fixed`, `clear`, `free`,
//! typed accessors) is reproduced here because Rust callers must
//! produce identical bytes. The stack is allocated with the S-001
//! allocator (C code frees it with `bfree` and vice versa).
//!
//! Not in this WP (documented, deferred): `cd_getparam` cursor access
//! (S-004 proc handlers) and `cd_serialize_*` (format pinned in S-010,
//! not yet written).

use obs_util::bmem::{bfree_raw, bmalloc_raw, brealloc_raw};

/// `W` = `sizeof(size_t)` (S-002 §2).
pub const W: usize = std::mem::size_of::<usize>();

/// S-002 §3: the C `struct calldata` (4W).
#[repr(C)]
#[derive(Debug)]
pub struct Calldata {
    /// Byte stack (S-001 allocator) or, when `fixed`, caller-owned.
    pub stack: *mut u8,
    /// Used bytes including the terminator (0 if never written).
    pub size: usize,
    /// Allocated bytes.
    pub capacity: usize,
    /// Caller-provided buffer, never reallocated.
    pub fixed: bool,
}

impl Calldata {
    /// S-002 §3: `calldata_init` — all zero.
    #[must_use]
    pub const fn init() -> Self {
        Self {
            stack: std::ptr::null_mut(),
            size: 0,
            capacity: 0,
            fixed: false,
        }
    }
}

impl Default for Calldata {
    fn default() -> Self {
        Self::init()
    }
}

impl Drop for Calldata {
    fn drop(&mut self) {
        // Q-2/R22: the C `calldata_free` does not reset the fields; the
        // Rust wrapper MUST reset if it reuses the struct — Drop does
        // the free + reset in one place.
        if !self.fixed && !self.stack.is_null() {
            unsafe { bfree_raw(self.stack) };
        }
        self.stack = std::ptr::null_mut();
        self.size = 0;
        self.capacity = 0;
        self.fixed = false;
    }
}

fn read_size(stack: &[u8], pos: usize) -> usize {
    let mut buf = [0u8; W];
    buf.copy_from_slice(&stack[pos..pos + W]);
    usize::from_ne_bytes(buf)
}

fn write_size(stack: &mut [u8], pos: usize, v: usize) {
    stack[pos..pos + W].copy_from_slice(&v.to_ne_bytes());
}

// ── Lifecycle (inline surface) ───────────────────────────────────────────

/// S-002 §3: `calldata_init(d)` — all zero.
pub fn calldata_init(d: &mut Calldata) {
    *d = Calldata::init();
}

/// S-002 §3: `calldata_init_fixed(d, buf, n)` — caller-owned buffer;
/// cleared to size W (the empty terminator).
pub fn calldata_init_fixed(d: &mut Calldata, buf: *mut u8, n: usize) {
    d.stack = buf;
    d.capacity = n;
    d.fixed = true;
    d.size = 0;
    calldata_clear(d);
}

/// S-002 §3: `calldata_clear(d)` — `size = W`, first W bytes zeroed
/// (an empty stack is one zero terminator); NULL stack: nothing.
pub fn calldata_clear(d: &mut Calldata) {
    if d.stack.is_null() {
        return;
    }
    d.size = W;
    let stack = unsafe { std::slice::from_raw_parts_mut(d.stack, W) };
    stack.fill(0);
}

/// S-002 §3/Q-2: `calldata_free(d)` — frees the stack when not fixed.
/// **Does not reset the fields** (the caller — or the Rust `Drop` —
/// must reset before reuse).
pub fn calldata_free(d: &mut Calldata) {
    if !d.fixed && !d.stack.is_null() {
        unsafe { bfree_raw(d.stack) };
    }
}

/// The exact error text of S-002-R14 (MUST). Routed to stderr until the
/// obs-log crate lands.
pub(crate) fn fixed_overflow_log() {
    eprintln!("Tried to go above fixed calldata stack size!");
}

// ── Search (S-002 §6) ───────────────────────────────────────────────────

/// S-002 §6/R02: find the first record named `name`; returns the offset
/// of that record's `data_size` field. Precondition `d.size != 0`.
fn find_record(d: &Calldata, name: &[u8]) -> Option<usize> {
    if d.size == 0 || name.is_empty() {
        return None;
    }
    let stack = unsafe { std::slice::from_raw_parts(d.stack, d.size) };
    let mut pos = 0usize;
    loop {
        let name_size = read_size(stack, pos);
        if name_size == 0 {
            return None; // terminator
        }
        let name_at = pos + W;
        let stored = &stack[name_at..name_at + name_size - 1]; // name w/o NUL
        if stored == name {
            return Some(name_at + name_size); // the data_size field
        }
        let data_size_at = name_at + name_size;
        let data_size = read_size(stack, data_size_at);
        pos = data_size_at + W + data_size;
    }
}

// ── Capacity (R14) ──────────────────────────────────────────────────────

/// S-002-R14: `ensure(new_size)` — ok when `new_size < capacity`
/// (**strict**: equality overflows); fixed → log the R14 text and
/// report failure (the caller must abandon **without** having changed
/// anything); else grow to `max(2 * capacity, new_size)`.
fn ensure(d: &mut Calldata, new_size: usize) -> bool {
    if new_size < d.capacity {
        return true;
    }
    if d.fixed {
        fixed_overflow_log();
        return false;
    }
    let new_capacity = (d.capacity * 2).max(new_size);
    d.stack = unsafe { brealloc_raw(d.stack, new_capacity) };
    d.capacity = new_capacity;
    true
}

// ── The three exported functions ────────────────────────────────────────

/// S-002-R01–R03: `calldata_get_data(d, name, out, size)` — first
/// record with a byte-equal name; copies the payload **only when
/// `data_size == out.len()`** exactly, otherwise no write and `false`.
/// `false` for an empty name or `d.size == 0`.
#[must_use]
pub fn calldata_get_data(d: &Calldata, name: &[u8], out: &mut [u8]) -> bool {
    if name.is_empty() || d.size == 0 {
        return false;
    }
    let Some(pos) = find_record(d, name) else {
        return false;
    };
    let stack = unsafe { std::slice::from_raw_parts(d.stack, d.size) };
    let data_size = read_size(stack, pos);
    if data_size != out.len() {
        return false; // Q-1: type is only the byte count
    }
    out.copy_from_slice(&stack[pos + W..pos + W + data_size]);
    true
}

/// S-002-R10–R16: `calldata_set_data(d, name, data)` — no-op for an
/// empty name. First write to a growable calldata allocates
/// `max(128, 3W + name_len + size)` (R11); an existing name is replaced
/// in place with a tail shift (R12, order preserved); a new name is
/// appended before the terminator (R13). `in == NULL && size == 0`
/// reads nothing (R15).
pub fn calldata_set_data(d: &mut Calldata, name: &[u8], data: &[u8]) {
    if name.is_empty() {
        return;
    }
    let name_len = name.len() + 1; // incl. NUL

    // R11: first write to a growable calldata.
    if !d.fixed && d.stack.is_null() {
        let alloc = 128usize.max(3 * W + name_len + data.len());
        d.stack = unsafe { bmalloc_raw(alloc) };
        d.capacity = alloc;
        // record at 0, terminator right after
        let stack = unsafe { std::slice::from_raw_parts_mut(d.stack, alloc) };
        write_size(stack, 0, name_len);
        stack[W..W + name.len()].copy_from_slice(name);
        stack[W + name.len()] = 0;
        write_size(stack, W + name_len, data.len());
        if !data.is_empty() {
            stack[W + name_len + W..W + name_len + W + data.len()].copy_from_slice(data);
        }
        let total = 3 * W + name_len + data.len();
        stack[total - W..total].fill(0); // terminator
        d.size = total;
        return;
    }

    // R12/R13 (and R16: a fixed calldata's first write goes here).
    if let Some(pos) = find_record(d, name) {
        // existing record: replace the payload (R12)
        let stack = unsafe { std::slice::from_raw_parts_mut(d.stack, d.size) };
        let cur = read_size(stack, pos);
        if cur < data.len() {
            let k = data.len() - cur;
            if !ensure(d, d.size + k) {
                return; // R14: abandoned, no change
            }
            unsafe {
                std::ptr::copy(
                    d.stack.add(pos + W + cur),
                    d.stack.add(pos + W + cur + k),
                    d.size - (pos + W + cur) + 1,
                );
            }
            d.size += k;
        } else if cur > data.len() {
            let k = cur - data.len();
            unsafe {
                std::ptr::copy(
                    d.stack.add(pos + W + cur),
                    d.stack.add(pos + W + cur - k),
                    d.size - (pos + W + cur) + 1,
                );
            }
            d.size -= k;
        }
        let stack = unsafe { std::slice::from_raw_parts_mut(d.stack, d.size) };
        write_size(stack, pos, data.len());
        if !data.is_empty() {
            stack[pos + W..pos + W + data.len()].copy_from_slice(data);
        }
    } else {
        // new record: append before the terminator (R13)
        let add = name_len + data.len() + 2 * W;
        if !ensure(d, d.size + add) {
            return; // R14: abandoned, no change
        }
        let at = d.size - W; // before the terminator
        unsafe {
            std::ptr::copy(d.stack.add(at), d.stack.add(at + add), W);
        }
        let stack = unsafe { std::slice::from_raw_parts_mut(d.stack, d.size + add) };
        write_size(stack, at, name_len);
        stack[at + W..at + W + name.len()].copy_from_slice(name);
        stack[at + W + name.len()] = 0;
        write_size(stack, at + W + name_len, data.len());
        if !data.is_empty() {
            stack[at + W + name_len + W..at + W + name_len + W + data.len()].copy_from_slice(data);
        }
        // terminator already shifted up by the copy above
        d.size += add;
    }
}

/// S-002-R04/R05/Q-3: `calldata_get_string(d, name, &s)` — succeeds for
/// **any** record regardless of payload size; `Some(payload)` points
/// into the stack (valid until the next modification), `None` when the
/// name is missing or `data_size == 0` (NULL). No NUL check is made.
#[must_use]
pub fn calldata_get_string<'a>(d: &'a Calldata, name: &[u8]) -> Option<&'a [u8]> {
    if name.is_empty() || d.size == 0 {
        return None;
    }
    let pos = find_record(d, name)?;
    let stack = unsafe { std::slice::from_raw_parts(d.stack, d.size) };
    let data_size = read_size(stack, pos);
    if data_size == 0 {
        return None;
    }
    Some(&stack[pos + W..pos + W + data_size])
}

// ── Typed accessors (header-inline surface) ─────────────────────────────

/// `calldata_set_int` — `set_data(name, &v, 8)`.
pub fn calldata_set_int(d: &mut Calldata, name: &[u8], v: i64) {
    calldata_set_data(d, name, &v.to_ne_bytes());
}

/// `calldata_set_float` — `set_data(name, &v, 8)` (double).
pub fn calldata_set_float(d: &mut Calldata, name: &[u8], v: f64) {
    calldata_set_data(d, name, &v.to_bits().to_ne_bytes());
}

/// `calldata_set_bool` — `set_data(name, &v, 1)`.
pub fn calldata_set_bool(d: &mut Calldata, name: &[u8], v: bool) {
    calldata_set_data(d, name, &[v as u8]);
}

/// `calldata_set_ptr` — `set_data(name, &v, W)`.
pub fn calldata_set_ptr(d: &mut Calldata, name: &[u8], v: *mut u8) {
    calldata_set_data(d, name, &(v as usize).to_ne_bytes());
}

/// `calldata_set_string` — `Some(s)`: payload `s` + NUL; `None`:
/// zero-length payload (R15).
pub fn calldata_set_string(d: &mut Calldata, name: &[u8], s: Option<&[u8]>) {
    match s {
        Some(s) => {
            let mut buf = s.to_vec();
            buf.push(0);
            calldata_set_data(d, name, &buf);
        }
        None => calldata_set_data(d, name, &[]),
    }
}

/// `calldata_get_int` — 8-byte payload; `None` when absent or mis-sized.
#[must_use]
pub fn calldata_get_int(d: &Calldata, name: &[u8]) -> Option<i64> {
    let mut buf = [0u8; 8];
    if calldata_get_data(d, name, &mut buf) {
        Some(i64::from_ne_bytes(buf))
    } else {
        None
    }
}

/// `calldata_get_float` — 8-byte payload (double).
#[must_use]
pub fn calldata_get_float(d: &Calldata, name: &[u8]) -> Option<f64> {
    let mut buf = [0u8; 8];
    if calldata_get_data(d, name, &mut buf) {
        Some(f64::from_bits(u64::from_ne_bytes(buf)))
    } else {
        None
    }
}

/// `calldata_get_bool` — 1-byte payload.
#[must_use]
pub fn calldata_get_bool(d: &Calldata, name: &[u8]) -> Option<bool> {
    let mut buf = [0u8; 1];
    if calldata_get_data(d, name, &mut buf) {
        Some(buf[0] != 0)
    } else {
        None
    }
}

/// `calldata_get_ptr` — W-byte payload.
#[must_use]
pub fn calldata_get_ptr(d: &Calldata, name: &[u8]) -> Option<*mut u8> {
    let mut buf = [0u8; W];
    if calldata_get_data(d, name, &mut buf) {
        Some(usize::from_ne_bytes(buf) as *mut u8)
    } else {
        None
    }
}

// ── Copy (R25) ──────────────────────────────────────────────────────────

/// S-002-R25: `cd_copy_data(dst, src)` — deep copy of the whole
/// calldata (every param, strings duplicated). `dst` must be a
/// growable calldata that is reset to empty first.
pub fn cd_copy_data(dst: &mut Calldata, src: &Calldata) {
    if src.stack.is_null() || src.size == 0 {
        calldata_init(dst);
        return;
    }
    calldata_init(dst);
    let stack = unsafe { std::slice::from_raw_parts(src.stack, src.size) };
    let mut pos = 0usize;
    while pos < src.size {
        let name_size = read_size(stack, pos);
        if name_size == 0 {
            break;
        }
        let name_at = pos + W;
        let name = &stack[name_at..name_at + name_size - 1];
        let data_size_at = name_at + name_size;
        let data_size = read_size(stack, data_size_at);
        let data = &stack[data_size_at + W..data_size_at + W + data_size];
        pos = data_size_at + W + data_size;
        calldata_set_data(dst, name, data);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dump(d: &Calldata) -> Vec<u8> {
        unsafe { std::slice::from_raw_parts(d.stack, d.size) }.to_vec()
    }

    #[test]
    fn layout_is_c_compatible() {
        assert_eq!(std::mem::size_of::<Calldata>(), 4 * W);
        assert_eq!(std::mem::offset_of!(Calldata, stack), 0);
        assert_eq!(std::mem::offset_of!(Calldata, size), W);
        assert_eq!(std::mem::offset_of!(Calldata, capacity), 2 * W);
        assert_eq!(std::mem::offset_of!(Calldata, fixed), 3 * W);
    }

    #[test]
    fn vec01_basic_worked_example() {
        let mut d = Calldata::init();
        assert!(d.stack.is_null());
        assert_eq!(d.size, 0);
        calldata_set_int(&mut d, b"a", 1);
        assert_eq!(d.size, 34, "worked example: 34 bytes");
        let mut exp: Vec<u8> = vec![
            2, 0, 0, 0, 0, 0, 0, 0, b'a', 0, 8, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0,
        ];
        exp.extend_from_slice(&[0u8; W]);
        assert_eq!(dump(&d), exp, "exact byte dump");
        assert_eq!(d.capacity, 128, "R11: max(128, ...)");
        assert_eq!(calldata_get_int(&d, b"a"), Some(1));
        assert_eq!(calldata_get_int(&d, b"b"), None);
    }

    #[test]
    fn vec02_type_is_only_size() {
        let mut d = Calldata::init();
        calldata_set_int(&mut d, b"i", 1);
        // Q-1: an 8-byte read succeeds on the int value
        assert_eq!(calldata_get_float(&d, b"i"), Some(f64::from_bits(1)));
        assert_eq!(calldata_get_ptr(&d, b"i").map(|p| p as usize), Some(1));
        // bool wants 1 byte -> fails on 8
        assert_eq!(calldata_get_bool(&d, b"i"), None);
        // Q-3: get_string succeeds on any record; the int's bytes are
        // not a valid C string (first byte 0x01, then NUL) -> prints ""
        let s = calldata_get_string(&d, b"i").unwrap();
        assert_eq!(s.len(), 8);
        assert_eq!(s[0], 1);
        assert_eq!(s[1], 0);
        assert_eq!(
            calldata_get_string(&d, b"i").unwrap(),
            &d.as_bytes()[W + 2 + W..][..8]
        );
    }

    #[test]
    fn vec03_overwrite_resizes() {
        let mut d = Calldata::init();
        calldata_set_int(&mut d, b"a", 1); // 8-byte payload
        calldata_set_int(&mut d, b"b", 2); // second record
        // shrink "a" to a 6-byte payload
        calldata_set_data(&mut d, b"a", b"hello\0");
        assert!(calldata_get_data(&d, b"a", &mut [0u8; 6]));
        assert_eq!(&d.as_bytes()[W + 2 + W..W + 2 + W + 6], b"hello\0");
        // grow "a" to 12 bytes
        calldata_set_data(&mut d, b"a", &[7u8; 12]);
        assert!(calldata_get_data(&d, b"a", &mut [0u8; 12]));
        // order preserved: a, b
        let names: Vec<&[u8]> = vec![b"a", b"b"];
        let mut pos = 0usize;
        let stack = d.as_bytes();
        for n in names {
            let name_size = read_size(stack, pos);
            assert_eq!(&stack[pos + W..pos + W + name_size - 1], n);
            pos += W + name_size + W + read_size(stack, pos + W + name_size);
        }
    }

    #[test]
    fn vec04_null_and_empty_strings() {
        let mut d = Calldata::init();
        calldata_set_string(&mut d, b"n", None); // data_size 0
        assert_eq!(calldata_get_string(&d, b"n"), None, "NULL payload -> NULL");
        calldata_set_string(&mut d, b"e", Some(b"")); // data_size 1
        assert_eq!(calldata_get_string(&d, b"e"), Some(b"\0".as_ref()));
        calldata_set_string(&mut d, b"s", Some(b"hi"));
        assert_eq!(calldata_get_string(&d, b"s"), Some(b"hi\0".as_ref()));
        // R15: the size-0 write read nothing from in
        let mut e2 = Calldata::init();
        calldata_set_data(&mut e2, b"z", &[]);
        assert!(calldata_get_data(&e2, b"z", &mut []));
        assert!(!calldata_get_data(&e2, b"z", &mut [0]));
    }

    #[test]
    fn vec05_invalid_names_and_empty_stack() {
        let mut d = Calldata::init();
        calldata_set_data(&mut d, b"", b"v"); // no-op
        assert!(d.stack.is_null(), "empty name: no allocation");
        assert!(!calldata_get_data(&d, b"", &mut [0]));
        // size == 0 (never written) -> reads fail even though stack may
        // exist; here: a cleared-but-never-grown calldata
        let mut c = Calldata::init();
        calldata_clear(&mut c);
        assert!(!calldata_get_data(&c, b"a", &mut [0]));
        // set_data on a stack-less calldata allocates (R11) — sanity
        calldata_set_data(&mut d, b"a", b"1");
        assert_eq!(d.size, 3 * W + 2 + 1);
    }

    #[test]
    fn vec06_capacity_growth() {
        let mut d = Calldata::init();
        calldata_set_int(&mut d, b"a", 1);
        assert_eq!(d.capacity, 128);
        // force several doublings: each append must keep size < capacity
        let mut i = 0u32;
        loop {
            let name = format!("n{i:04}").into_bytes();
            calldata_set_data(&mut d, &name, &[0u8; 64]);
            assert!(d.size < d.capacity, "R14 strict invariant");
            i += 1;
            if i == 40 {
                break;
            }
        }
        assert!(d.capacity > 128);
    }

    #[test]
    fn vec07_fixed_stack() {
        let mut buf = vec![0u8; 64];
        let p = buf.as_mut_ptr();
        let mut d = Calldata::init();
        calldata_init_fixed(&mut d, p, 64);
        assert!(d.fixed);
        assert_eq!(d.size, W);
        // R16: first write goes through the append path (R13)
        calldata_set_int(&mut d, b"a", 1);
        assert_eq!(d.size, 3 * W + 2 + 8);
        assert_eq!(calldata_get_int(&d, b"a"), Some(1));
        // now fill near the edge: 64 - (3*8+2+8) = 34 left;
        // a record needs name_len + size + 2W >= 2 + 8 + 16 = 26 + ...
        calldata_set_int(&mut d, b"b", 2); // size 26 -> total 60 < 64 ok
        assert_eq!(d.size, 60);
        // next write: new_size = 60 + 26 = 86 >= 64 -> overflow
        let size_before = d.size;
        calldata_set_int(&mut d, b"c", 3);
        assert_eq!(d.size, size_before, "R14: abandoned, no change");
        assert_eq!(d.capacity, 64, "fixed: no realloc");
        // existing-name replace that overflows is also abandoned
        // (payload 8 -> 64: size 60 + 56 = 116 >= 64)
        calldata_set_data(&mut d, b"a", &[9u8; 64]);
        assert_eq!(d.size, size_before, "R12+R14: abandoned, no change");
        assert_eq!(calldata_get_int(&d, b"a"), Some(1), "old value kept");
        // shrinking on a fixed stack is fine
        calldata_set_data(&mut d, b"a", b"x\0");
        assert!(calldata_get_data(&d, b"a", &mut [0; 2]));
        d.fixed = false;
        d.stack = std::ptr::null_mut(); // do not free the test vec
        d.capacity = 0;
    }

    #[test]
    fn vec08_clear_and_reuse() {
        let mut d = Calldata::init();
        calldata_set_int(&mut d, b"a", 1);
        calldata_set_int(&mut d, b"b", 2);
        let cap = d.capacity;
        calldata_clear(&mut d);
        assert_eq!(d.size, W);
        assert_eq!(d.capacity, cap, "R21: capacity unchanged");
        assert_eq!(dump(&d), vec![0u8; W]);
        // R21: next set_data appends (R13), not R11
        calldata_set_int(&mut d, b"c", 3);
        assert_eq!(d.size, 2 * W + 2 + 8 + W);
        assert_eq!(calldata_get_int(&d, b"c"), Some(3));
        assert_eq!(calldata_get_int(&d, b"a"), None, "old params gone");
    }

    #[test]
    fn vec09_raw_data() {
        let mut d = Calldata::init();
        let raw = [0xde, 0xad, 0x00, 0xbe, 0xef];
        calldata_set_data(&mut d, b"raw", &raw);
        let mut out = [0u8; 5];
        assert!(calldata_get_data(&d, b"raw", &mut out));
        assert_eq!(out, raw, "binary payload with embedded NUL");
        // zero-length payload
        calldata_set_data(&mut d, b"z", &[]);
        assert!(calldata_get_data(&d, b"z", &mut []));
    }

    #[test]
    fn vec10_order_and_duplicates() {
        let mut d = Calldata::init();
        calldata_set_int(&mut d, b"a", 1);
        calldata_set_int(&mut d, b"b", 2);
        calldata_set_int(&mut d, b"c", 3);
        // overwrite middle: order a,b,c preserved
        calldata_set_int(&mut d, b"b", 22);
        let names = order_of(&d);
        assert_eq!(names, vec![b"a", b"b", b"c"]);
        assert_eq!(calldata_get_int(&d, b"b"), Some(22));
        // case differs -> different name
        calldata_set_int(&mut d, b"A", 9);
        let names = order_of(&d);
        assert_eq!(names, vec![b"a", b"b", b"c", b"A"]);
        // duplicate write = replace, not append
        calldata_set_int(&mut d, b"c", 33);
        let names = order_of(&d);
        assert_eq!(names, vec![b"a", b"b", b"c", b"A"]);
        assert_eq!(calldata_get_int(&d, b"c"), Some(33));
    }

    fn order_of(d: &Calldata) -> Vec<&'static [u8]> {
        let stack = d.as_bytes();
        let mut names = vec![];
        let mut pos = 0usize;
        while pos < d.size {
            let name_size = read_size(stack, pos);
            if name_size == 0 {
                break;
            }
            let name_at = pos + W;
            let n = &stack[name_at..name_at + name_size - 1];
            names.push(if n == b"a" {
                &b"a"[..]
            } else if n == b"b" {
                &b"b"[..]
            } else if n == b"c" {
                &b"c"[..]
            } else if n == b"A" {
                &b"A"[..]
            } else {
                &b"?"[..]
            });
            let data_size_at = name_at + name_size;
            let data_size = read_size(stack, data_size_at);
            pos = data_size_at + W + data_size;
        }
        names
    }

    #[test]
    fn vec11_long_names_and_binary_strings() {
        let mut d = Calldata::init();
        let long: Vec<u8> = std::iter::repeat_n(b'x', 100).collect();
        calldata_set_string(&mut d, &long, Some(b"v"));
        assert_eq!(calldata_get_string(&d, &long), Some(b"v\0".as_ref()));
        // binary "string" with embedded NULs
        let bin = [0u8, 1, 2, 0, 4];
        calldata_set_string(&mut d, b"bin", Some(&bin));
        assert_eq!(
            calldata_get_string(&d, b"bin"),
            Some(b"\x00\x01\x02\x00\x04\x00".as_ref())
        );
    }

    #[test]
    fn vec12_fixed_boundary_q4() {
        // Q-4: new_size == capacity is treated as overflow (strict <).
        // Craft a fixed stack whose current size + one minimal record
        // (name "a\0" = 2, empty payload, 2W) lands exactly on capacity.
        // record cost = 2 + 0 + 2W = 18; pick capacity = W + 18 = 26.
        let mut buf1 = vec![0u8; 26];
        let mut d = Calldata::init();
        calldata_init_fixed(&mut d, buf1.as_mut_ptr(), 26);
        // size = W = 8; appending the minimal record needs 8 + 18 = 26
        // == capacity -> overflow (strict)
        calldata_set_data(&mut d, b"a", &[]);
        assert_eq!(d.size, W, "Q-4: exactly-at-capacity write abandoned");
        // fresh 27-byte buffer: the same write now fits (26 < 27)
        d.fixed = false;
        d.stack = std::ptr::null_mut();
        d.capacity = 0;
        let mut buf2 = vec![0u8; 27];
        calldata_init_fixed(&mut d, buf2.as_mut_ptr(), 27);
        calldata_set_data(&mut d, b"a", &[]);
        assert_eq!(d.size, W + 18, "fits when new_size < capacity");
        assert!(calldata_get_data(&d, b"a", &mut []));
        d.fixed = false;
        d.stack = std::ptr::null_mut();
        d.capacity = 0;
    }

    #[test]
    fn r22_free_leaves_stale_fields() {
        let mut d = Calldata::init();
        calldata_set_int(&mut d, b"a", 1);
        let stack = d.stack;
        assert!(!stack.is_null());
        calldata_free(&mut d);
        // Q-2: fields are stale after free
        assert_eq!(d.stack, stack);
        assert_eq!(d.size, 34);
        // the Rust wrapper resets for reuse:
        d.stack = std::ptr::null_mut();
        d.size = 0;
        d.capacity = 0;
        // and Drop frees + resets:
        let mut d2 = Calldata::init();
        calldata_set_int(&mut d2, b"a", 1);
        drop(d2);
    }

    #[test]
    fn r25_cd_copy_data_deep() {
        let mut src = Calldata::init();
        calldata_set_int(&mut src, b"a", 1);
        calldata_set_string(&mut src, b"s", Some(b"hello"));
        calldata_set_float(&mut src, b"f", 1.5);
        let mut dst = Calldata::init();
        cd_copy_data(&mut dst, &src);
        assert_eq!(calldata_get_int(&dst, b"a"), Some(1));
        assert_eq!(calldata_get_string(&dst, b"s"), Some(b"hello\0".as_ref()));
        assert_eq!(calldata_get_float(&dst, b"f"), Some(1.5));
        assert_eq!(dump(&dst), dump(&src), "byte-identical copy");
        // deep: dst owns its stack
        assert_ne!(dst.stack, src.stack);
        // empty source -> reset dst
        let empty = Calldata::init();
        let mut dst2 = Calldata::init();
        calldata_set_int(&mut dst2, b"x", 1);
        cd_copy_data(&mut dst2, &empty);
        assert!(dst2.stack.is_null());
        assert_eq!(dst2.size, 0);
    }

    // helper used by tests: the used stack bytes
    impl Calldata {
        fn as_bytes(&self) -> &[u8] {
            if self.stack.is_null() {
                &[]
            } else {
                unsafe { std::slice::from_raw_parts(self.stack, self.size) }
            }
        }
    }
}
