#![no_main]
//! P1-02.4 (WP-32): random op sequences for `dstr`, Rust vs C.
//!
//! Each fuzz input is parsed as an op stream; the same sequence is applied
//! to the Rust `obs_util::dstr` implementation and to the pinned C
//! `libobs/util/dstr.c` (compiled into this harness by `build.rs`). After
//! every op the two states must agree byte-for-byte: `len`, `capacity`,
//! content, and the NUL terminator. `Dstr` is `#[repr(C)]` with the exact
//! `struct dstr` layout (S-006), so the same type drives both.
//!
//! The C side is freed through the C `dstr_free` (its allocations come
//! from the harness shim's system allocator); `ManuallyDrop` keeps the
//! Rust `Drop` (Rust-side bmem) off that buffer.
//!
//! `cat_ch(0)`/`insert_ch(0)` put a real NUL byte into the content, so
//! NUL-embedded states are driven. The C reference's `strlen`-based ops
//! (`depad`) see the truncated view — the port mirrors that (S-006-R22).
//!
//! The non-fast-path C `dstr_insert_ch` (Q-8) shifts one byte past the
//! guaranteed tail when the buffer has no slack, so the harness gives
//! the C block headroom for those ops and normalises the observable
//! capacity afterwards — the stray byte lands past the NUL and is
//! unobservable.
//!
//! `init` is driven only on an unallocated dstr (the C caller contract):
//! the C `dstr_init` zeroes the fields without freeing, so re-initing an
//! allocated dstr leaks in C but not in the port (the Rust `dstr_init`
//! frees first — a deliberate safe deviation, S-006-R04). Driven in that
//! state the two observably diverge (a leak on the C side only), which is
//! a caller bug, not a port defect.
//!
//! The `insert*` ops are driven with `idx <= len` only (the C caller
//! contract): with `idx > len` the C shift count `len - idx + 1` double-
//! wraps to 0, so C keeps an uninitialised byte and never moves the NUL
//! (silent corruption); the port zero-fills the fresh allocation instead
//! — a defensive deviation visible only in the contract-violating state.
//!
//! `resize` is driven with `n <= len` only (shrink/no-op): an extending
//! resize leaves `[old_len, n)` uninitialised in C (S-006-R03) while the
//! port zero-fills — a documented defensive deviation, so an extended
//! state would poison every later byte comparison. Growth through
//! `ensure` (op 11) still exercises the same capacity arithmetic.
//!
//! A bare `reserve`/`ensure` on an unallocated dstr likewise leaves the
//! buffer uninitialised (no NUL is written until content is appended), so
//! the NUL-terminator assertion only applies once `len > 0`.
//!
//! The case ops (`to_upper`/`to_lower`) are driven only on **NUL-free
//! ASCII** content. S-006-R24 specifies ASCII behaviour ("non-ASCII
//! mapping is locale-dependent in the reference", Q-7), and the C
//! round-trip additionally truncates at the first embedded NUL (`while
//! (\*temp)` + `wchar_to_utf8` stop at the NUL wide char) — an
//! NUL-bearing state is off-domain for the byte-wise port, as is any
//! non-ASCII byte (the C `utf8_to_wchar` conversion is leniency-
//! dependent there). Off-domain, the harness skips the op.

use std::mem::ManuallyDrop;

use obs_util::dstr::{self, Dstr};

/// The C reference (shim + `libobs/util/dstr.c`, see `build.rs`).
#[allow(non_snake_case, dead_code)]
mod c {
    use obs_util::dstr::Dstr;

    unsafe extern "C" {
        // dstr.h `static inline` functions, through the shim wrappers
        pub fn fz_dstr_init(dst: &mut Dstr);
        pub fn fz_dstr_free(dst: &mut Dstr);
        pub fn fz_dstr_reserve(dst: &mut Dstr, capacity: usize);
        pub fn fz_dstr_resize(dst: &mut Dstr, num: usize);
        pub fn fz_dstr_ensure_capacity(dst: &mut Dstr, new_size: usize);
        pub fn fz_dstr_cat(dst: &mut Dstr, array: *const u8);
        pub fn fz_dstr_cat_ch(dst: &mut Dstr, ch: i8);
        // non-inline functions compiled into `dstr.c`
        pub fn dstr_copy(dst: &mut Dstr, array: *const u8);
        pub fn dstr_ncopy(dst: &mut Dstr, array: *const u8, len: usize);
        pub fn dstr_cat_dstr(dst: &mut Dstr, src: &Dstr);
        pub fn dstr_ncat(dst: &mut Dstr, array: *const u8, len: usize);
        pub fn dstr_ncat_dstr(dst: &mut Dstr, src: &Dstr, len: usize);
        pub fn dstr_insert(dst: &mut Dstr, idx: usize, array: *const u8);
        pub fn dstr_insert_dstr(dst: &mut Dstr, idx: usize, src: &Dstr);
        pub fn dstr_insert_ch(dst: &mut Dstr, idx: usize, ch: i8);
        pub fn dstr_replace(str_: &mut Dstr, find: *const u8, replace: *const u8);
        pub fn dstr_depad(dst: &mut Dstr);
        pub fn dstr_to_upper(str_: &mut Dstr);
        pub fn dstr_to_lower(str_: &mut Dstr);
    }
}

/// A C string view: the slice plus its NUL-terminated copy.
struct CStr<'a> {
    /// The bytes (without the NUL) — the Rust-side argument.
    bytes: &'a [u8],
    /// NUL-terminated copy — the C-side argument.
    null_terminated: Vec<u8>,
}

impl<'a> CStr<'a> {
    fn new(data: &'a [u8], off: usize, max_len: usize) -> Self {
        let off = off % (data.len().saturating_add(1));
        let end = data.len().min(off + max_len);
        let bytes = &data[off..end];
        let mut null_terminated = bytes.to_vec();
        null_terminated.push(0);
        Self { bytes, null_terminated }
    }
}

/// The bytes of a `Dstr` (len-bounded; the invariant under test).
fn bytes(d: &Dstr) -> &[u8] {
    if d.array.is_null() {
        &[]
    } else {
        // SAFETY: Dstr invariant — `array` holds at least `len` bytes.
        unsafe { std::slice::from_raw_parts(d.array, d.len) }
    }
}

/// The C-string view of a fuzz slice: up to the first NUL. The C
/// reference sees `strlen` bytes; the Rust side must see the same prefix
/// (the future C bridge will hand it `strlen(array)`).
fn cstr_len(bytes: &[u8]) -> usize {
    bytes.iter().position(|&b| b == 0).unwrap_or(bytes.len())
}

/// After every op the Rust and C states must be byte-identical. The
/// harness keeps both states fully defined (see the module docs on the
/// C-uninitialised regions), so the whole `len`-bounded view is compared.
/// The NUL assertion applies once `len > 0` (a bare `reserve` on an
/// unallocated dstr leaves `array[0]` unspecified).
fn check(c: &Dstr, r: &Dstr) {
    assert_eq!(c.len, r.len, "len mismatch");
    assert_eq!(c.capacity, r.capacity, "capacity mismatch");
    assert_eq!(bytes(c), bytes(r), "content mismatch");
    for d in [&*c, &r] {
        if !d.array.is_null() && d.len > 0 {
            // SAFETY: Dstr invariant — `array[len]` is the terminator.
            assert_eq!(unsafe { *d.array.add(d.len) }, 0, "missing NUL terminator");
        }
    }
}

fn run(data: &[u8]) {
    let mut c = ManuallyDrop::new(Dstr::init());
    let mut r = Dstr::init();
    // Secondary pair, for the `*_dstr` ops.
    let mut oc = ManuallyDrop::new(Dstr::init());
    let mut or = Dstr::init();

    // SAFETY: `c`/`oc` are zeroed (the dstr_init state) and live for the
    // call; every C fn mirrors the C contract on that state.
    unsafe {
        c::fz_dstr_init(&mut c);
        c::fz_dstr_init(&mut oc);
    }

    let mut i = 0;
    while i + 2 < data.len() {
        let op = data[i];
        let p1 = data[i + 1] as usize;
        let p2 = data[i + 2] as usize;
        i += 3;
        let s = CStr::new(data, p1, p2);
        let s2 = CStr::new(data, p2, p1);

        match op % 16 {
            0 => {
                // C caller contract: `init` on an unallocated dstr only (see
                // the module docs — re-init diverges: C leaks, Rust frees).
                if c.array.is_null() {
                    dstr::dstr_init(&mut r);
                    unsafe { c::fz_dstr_init(&mut c) };
                }
            }
            1 => {
                dstr::dstr_free(&mut r);
                unsafe { c::fz_dstr_free(&mut c) };
            }
            2 => {
                // C-string op: the Rust side sees the strlen prefix.
                let cs = &s.bytes[..cstr_len(s.bytes)];
                dstr::dstr_copy(&mut r, cs);
                unsafe { c::dstr_copy(&mut c, s.null_terminated.as_ptr()) };
            }
            3 => {
                // C reads exactly `n` bytes (the caller must provide them),
                // so clamp to the view length — the in-contract use.
                let n = p2.min(s.bytes.len());
                dstr::dstr_ncopy(&mut r, s.bytes, n);
                unsafe { c::dstr_ncopy(&mut c, s.null_terminated.as_ptr(), n) };
            }
            4 => {
                let cs = &s.bytes[..cstr_len(s.bytes)];
                dstr::dstr_cat(&mut r, cs);
                unsafe { c::fz_dstr_cat(&mut c, s.null_terminated.as_ptr()) };
            }
            5 => {
                let n = p2.min(s.bytes.len());
                dstr::dstr_ncat(&mut r, s.bytes, n);
                unsafe { c::dstr_ncat(&mut c, s.null_terminated.as_ptr(), n) };
            }
            6 => {
                // C caller contract: idx <= len (see the module docs).
                let idx = p1 % (r.len + 1);
                let cs = &s.bytes[..cstr_len(s.bytes)];
                dstr::dstr_insert(&mut r, idx, cs);
                unsafe { c::dstr_insert(&mut c, idx, s.null_terminated.as_ptr()) };
            }
            7 => {
                let idx = p1 % (r.len + 1);
                let ch = s.bytes.first().copied().unwrap_or(0);
                dstr::dstr_insert_ch(&mut r, idx, ch);
                if idx < c.len {
                    // C Q-8 defect: the non-fast-path `dstr_insert_ch`
                    // shifts one byte past the guaranteed tail (1-byte
                    // heap OOB) when the buffer has no slack. Grow C's
                    // block so the stray byte lands inside the
                    // allocation (past the NUL — unobservable), then
                    // normalise the observable capacity.
                    let slack = c.capacity + 1;
                    unsafe { c::fz_dstr_ensure_capacity(&mut c, slack) };
                    unsafe { c::dstr_insert_ch(&mut c, idx, ch as i8) };
                    c.capacity = r.capacity;
                } else {
                    unsafe { c::dstr_insert_ch(&mut c, idx, ch as i8) };
                }
            }
            8 => {
                // Grow the secondary pair (C-string op), then insert it.
                let cs = &s2.bytes[..cstr_len(s2.bytes)];
                dstr::dstr_cat(&mut or, cs);
                unsafe { c::fz_dstr_cat(&mut oc, s2.null_terminated.as_ptr()) };
                let idx = p1 % (r.len + 1);
                dstr::dstr_insert_dstr(&mut r, idx, &or);
                unsafe { c::dstr_insert_dstr(&mut c, idx, &oc) };
            }
            9 => {
                dstr::dstr_reserve(&mut r, p2);
                unsafe { c::fz_dstr_reserve(&mut c, p2) };
            }
            10 => {
                // Shrink/no-op only — an extending resize leaves C bytes
                // in [old_len, n) uninitialised (see the module docs).
                if p2 <= r.len {
                    dstr::dstr_resize(&mut r, p2);
                    unsafe { c::fz_dstr_resize(&mut c, p2) };
                }
            }
            11 => {
                dstr::dstr_ensure_capacity(&mut r, p2);
                unsafe { c::fz_dstr_ensure_capacity(&mut c, p2) };
            }
            12 => {
                let ch = s.bytes.first().copied().unwrap_or(0);
                dstr::dstr_cat_ch(&mut r, ch);
                unsafe { c::fz_dstr_cat_ch(&mut c, ch as i8) };
            }
            13 => {
                // C `dstr_depad` runs `strlen` over the buffer — UB on a
                // reserved-but-empty (uninitialised) state; skip it there.
                if c.len > 0 || c.array.is_null() {
                    dstr::dstr_depad(&mut r);
                    unsafe { c::dstr_depad(&mut c) };
                }
            }
            14 | 15 => {
                // NUL-free ASCII only — see the module docs (Q-7):
                // off-domain (non-ASCII, NUL-bearing) case folding is
                // locale/leniency-dependent or truncating in the C
                // reference and not specified for the byte-wise port.
                if bytes(&c).iter().all(|&b| (0x01..=0x7f).contains(&b)) {
                    if op % 16 == 14 {
                        dstr::dstr_to_upper(&mut r);
                        unsafe { c::dstr_to_upper(&mut c) };
                    } else {
                        dstr::dstr_to_lower(&mut r);
                        unsafe { c::dstr_to_lower(&mut c) };
                    }
                }
            }
            _ => unreachable!("op % 16"),
        }

        check(&c, &r);
        check(&*oc, &or);
    }

    // SAFETY: C-side buffers were allocated by the shim's system allocator
    // and are freed by the C free (never by the Rust Drop).
    unsafe {
        c::fz_dstr_free(&mut c);
        c::fz_dstr_free(&mut oc);
    }
}

libfuzzer_sys::fuzz_target!(|data: &[u8]| {
    run(data);
});
