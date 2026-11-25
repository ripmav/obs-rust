//! P1-04.2 (WP-32): layout sharing for `struct deque` (spec S-005 §5).
//!
//! The C inline code (`libobs/util/deque.h`) touches the struct's fields
//! directly, so the Rust side owns a `#[repr(C)]`-compatible
//! [`obs_util::deque::Deque`] with the exact C field set
//! (`data`, `size`, `start_pos`, `end_pos`, `capacity`) and hands it to C
//! code unboxed: the conformance deque runner drives a Rust-owned deque
//! with the C inlines and must dump identically to the C-owned reference
//! (Q-D1: `end_pos` may equal `capacity`).
//!
//! The backing buffer goes through the S-001 allocator on both sides: the
//! Rust `Deque` allocates with the same system allocator the C inlines'
//! `bmalloc`/`brealloc`/`bfree` use (on unix `bmem.c` is plain
//! `malloc`/`free`). On Windows the C `bmalloc` uses `_aligned_malloc`
//! while Rust std uses the system allocator — the C-driven flow is
//! unix-verified here (CI conformance is unix); documented, not hidden.
//!
//! `obs_ffi_deque_new`/`obs_ffi_deque_free` are test-only symbols (like
//! the link stand-ins): excluded from the cbindgen header, declared
//! locally by the runner.

use obs_util::deque::Deque;

/// Allocate a zeroed, Rust-owned `struct deque` (R-D01 shape: all fields
/// 0, `data` NULL) and hand the pointer to C code.
///
/// # Safety
/// The returned pointer must be passed back to [`obs_ffi_deque_free`]
/// exactly once, and C code must treat it as the C `struct deque` (the
/// layouts are identical — `#[repr(C)]`, same field order and types).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn obs_ffi_deque_new() -> *mut Deque {
    Box::into_raw(Box::new(Deque::new()))
}

/// Free a deque allocated by [`obs_ffi_deque_new`]. The C inlines own the
/// backing buffer (a C `deque_free` already bfree'd it), so this drops the
/// box; the `Drop` impl sees the zeroed fields and frees nothing.
///
/// # Safety
/// `q` must be NULL (ignored) or the return of a matching
/// [`obs_ffi_deque_new`], not yet freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn obs_ffi_deque_free(q: *mut Deque) {
    if !q.is_null() {
        // SAFETY: caller contract — the matching new, freed once.
        unsafe {
            drop(Box::from_raw(q));
        };
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;

    /// The layout-sharing contract: a C-shaped consumer (the deque
    /// runner's `dump`) reads the five fields through the C layout and
    /// sees the Rust-owned state; the C inline `deque_push_back` shape
    /// (brealloc + byte store) works on the same memory.
    #[test]
    fn c_layout_fields_visible() {
        // SAFETY: box is freed at the end of the test.
        let q = unsafe { obs_ffi_deque_new() };
        // SAFETY: fresh box; field reads through the C field order.
        unsafe {
            assert_eq!((*q).data, ptr::null_mut());
            assert_eq!((*q).size, 0);
            assert_eq!((*q).start_pos, 0);
            assert_eq!((*q).end_pos, 0);
            assert_eq!((*q).capacity, 0);
        }
        // Drive it with the Rust ops (the C inlines do the same field
        // updates — S-005 R-D04); then the C field view must match.
        // SAFETY: `q` is a fresh, live box (allocated above, not yet freed).
        let dq = unsafe { &mut *q };
        dq.reserve(8);
        // SAFETY: invariant of `reserve` — the buffer is valid for capacity bytes.
        unsafe {
            std::ptr::write_bytes(dq.data, 0x42, 8);
        }
        // SAFETY: `q` is still live (freed once at the end of the test).
        let dq = unsafe { &mut *q };
        dq.push_back(b"AB");
        // SAFETY: field reads through the C field order.
        let (size, start, end, cap) =
            unsafe { ((*q).size, (*q).start_pos, (*q).end_pos, (*q).capacity) };
        assert_eq!((size, start, end, cap), (2, 0, 2, 8));
        // SAFETY: buffer valid for capacity bytes.
        assert_eq!(unsafe { std::slice::from_raw_parts((*q).data, 2) }, b"AB");
        // SAFETY: matching new, freed once.
        unsafe { obs_ffi_deque_free(q) };
    }
}
