//! P1-03.2 (WP-32): the C helpers behind the `da_push_back`, `da_insert`,
//! `da_erase` and `da_move_item` macros (spec S-005,
//! `libobs/util/darray.h`).
//!
//! The `da_*` macros stay in the C header; the `darray_*` functions they
//! call are provided here as linkable `extern "C"` symbols operating on
//! the C `struct darray` (this mirror is named `darray_c` because the
//! cf-lexer bridge already owns the crate-level name `darray` for its
//! generic DARRAY view) in C memory through the C allocator (the P1-03.3
//! recipe swaps the header's `static inline` bodies for declarations —
//! until then the inlines and these exports coexist: the inlines are
//! `static`, these are global).
//!
//! Elements are raw bytes: every copy/move is byte-wise over the whole
//! element **including padding** (S-005 Q-A2 — the C `memcpy`/`memmove`
//! semantics).

use crate::abi_common::{bfree, bmalloc};
use std::os::raw::c_void;

unsafe extern "C" {
    /// The C shim's `bcrash(format, ...)` (base-logging.c) — R-A13 OOM path.
    fn bcrash(format: *const std::ffi::c_char);
}

/// C `struct darray` (S-005 §2): `array` @0, `num` @8, `capacity` @16;
/// the `DARRAY(type)` union field view aliases these three fields. Named
/// `darray_c` (see the module docs).
#[repr(C)]
pub struct darray_c {
    pub array: *mut u8,
    pub num: usize,
    pub capacity: usize,
}

/// S-005-R-A02: grow to at least `new_size` *elements* of `element_size`
/// bytes (the C form copies `element_size * capacity` old bytes; `num` is
/// untouched).
fn ensure_capacity(dst: &mut darray_c, element_size: usize, new_size: usize) {
    if new_size <= dst.capacity {
        return;
    }
    let new_cap = if dst.capacity == 0 {
        new_size
    } else {
        dst.capacity * 2
    };
    let new_cap = new_cap.max(new_size);
    // SAFETY: C-ABI — S-001 allocation; the fresh block is valid for new_cap * es bytes.
    let ptr = unsafe { bmalloc(new_cap * element_size) };
    if !dst.array.is_null() && dst.capacity > 0 {
        // SAFETY: old block valid for es * capacity bytes; fresh for new_cap * es ≥ es * capacity.
        unsafe {
            std::ptr::copy_nonoverlapping(dst.array, ptr, dst.capacity * element_size);
            bfree(dst.array);
        };
    }
    dst.array = ptr;
    dst.capacity = new_cap;
}

/// C `static inline size_t darray_push_back(const size_t element_size,
/// struct darray *dst, const void *item)`: ensure `num + 1`, copy the item
/// (whole element, padding included — Q-A2) to the new last slot, return
/// the new index.
///
/// # Safety
/// `dst` must be a live C `struct darray`; `item` valid for `element_size`
/// bytes for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn darray_push_back(
    element_size: usize,
    dst: *mut darray_c,
    item: *const c_void,
) -> usize {
    // SAFETY: caller contract — live C struct; the growth and the store
    // stay within the ensured capacity (checked against it below).
    unsafe {
        let dst = &mut *dst;
        dst.num += 1;
        ensure_capacity(dst, element_size, dst.num);
        // SAFETY: `array` holds num ≥ 1 elements (ensured above); `item` valid for es bytes.
        std::ptr::copy_nonoverlapping(
            item.cast::<u8>(),
            dst.array.add((dst.num - 1) * element_size),
            element_size,
        );
        dst.num - 1
    }
}

/// C `static inline void darray_insert(const size_t element_size,
/// struct darray *dst, const size_t idx, const void *item)`: append when
/// `idx == num`, else grow by one, shift the tail right by one element
/// (memmove over whole elements — Q-A2) and copy the item into the gap.
///
/// # Safety
/// As [`darray_push_back`]; `idx <= num` (the C `assert`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn darray_insert(
    element_size: usize,
    dst: *mut darray_c,
    idx: usize,
    item: *const c_void,
) {
    // SAFETY: caller contract — live C struct, `idx <= num`, `item` valid for es bytes.
    unsafe {
        let dst = &mut *dst;
        if idx == dst.num {
            darray_push_back(element_size, dst, item);
            return;
        }
        let move_count = dst.num - idx;
        dst.num += 1;
        ensure_capacity(dst, element_size, dst.num);
        // SAFETY: tail [idx, idx+move_count) and [idx+1, idx+1+move_count) both fit in the ensured block.
        // src first, dst second (std::ptr::copy convention): the tail
        // [idx, idx+move_count) moves one element to the right.
        std::ptr::copy(
            dst.array.add(idx * element_size),
            dst.array.add((idx + 1) * element_size),
            move_count * element_size,
        );
        // SAFETY: the gap [idx, idx+es) is inside the ensured block.
        std::ptr::copy_nonoverlapping(
            item.cast::<u8>(),
            dst.array.add(idx * element_size),
            element_size,
        );
    };
}

/// C `static inline void darray_erase(const size_t element_size,
/// struct darray *dst, const size_t idx)`: `idx >= num` (or the last
/// element) → nothing (after the `assert`, the C body's own guard); else
/// decrement `num` first and shift the tail left (memmove — Q-A2).
///
/// # Safety
/// As [`darray_push_back`]; `idx < num` (the C `assert`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn darray_erase(element_size: usize, dst: *mut darray_c, idx: usize) {
    // SAFETY: caller contract — live C struct, `idx < num`.
    let dst = unsafe { &mut *dst };
    // C: `if (idx >= dst->num || !--dst->num) return;` — short-circuit:
    // out of range leaves `num` untouched; otherwise `num` is decremented
    // first and a result of 0 (the last element) skips the move.
    if idx >= dst.num {
        return;
    }
    dst.num -= 1;
    if dst.num == 0 {
        return;
    }
    // SAFETY: tail [idx+1, idx+1+(num-idx)) fits in the (unchanged) block.
    unsafe {
        // src first, dst second: the tail [idx+1, idx+1+(num-idx)) moves one
        // element to the left.
        std::ptr::copy(
            dst.array.add((idx + 1) * element_size),
            dst.array.add(idx * element_size),
            (dst.num - idx) * element_size,
        );
    };
}

/// C `static inline void darray_move_item(const size_t element_size,
/// struct darray *dst, const size_t from, const size_t to)`: `from == to`
/// → nothing; else copy the element to a plain-`malloc` temp (`bcrash` on
/// OOM — R-A13), shift the range one element (memmove), copy back, free
/// the temp.
///
/// # Safety
/// As [`darray_push_back`]; `from` and `to` inside `num` (the caller
/// contract of the C inline).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn darray_move_item(
    element_size: usize,
    dst: *mut darray_c,
    from: usize,
    to: usize,
) {
    // SAFETY: caller contract — live C struct; `from`/`to` inside `num`.
    let dst = unsafe { &mut *dst };
    if from == to {
        return;
    }
    // R-A13: plain malloc (NOT the S-001 allocator — the C inline does
    // `malloc`/`free`); OOM → bcrash through the C shim.
    // SAFETY: C-ABI — plain malloc/free pair on the same pointer.
    let temp = unsafe {
        std::alloc::alloc(std::alloc::Layout::from_size_align_unchecked(
            element_size,
            1,
        ))
    };
    if temp.is_null() {
        // SAFETY: C shim crash path; the literal is NUL-terminated for the call.
        unsafe { bcrash(c"darray_move_item: out of memory".as_ptr()) };
        return;
    }
    // SAFETY: `temp` valid for es bytes (allocated above); the two element slots fit in the block.
    unsafe {
        std::ptr::copy_nonoverlapping(dst.array.add(from * element_size), temp, element_size);
        if to < from {
            std::ptr::copy(
                dst.array.add(to * element_size),
                dst.array.add((to + 1) * element_size),
                (from - to) * element_size,
            );
        } else {
            std::ptr::copy(
                dst.array.add((from + 1) * element_size),
                dst.array.add(from * element_size),
                (to - from) * element_size,
            );
        }
        std::ptr::copy_nonoverlapping(temp, dst.array.add(to * element_size), element_size);
        std::alloc::dealloc(
            temp,
            std::alloc::Layout::from_size_align_unchecked(element_size, 1),
        );
    };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;

    /// A DARRAY(uint32_t)-shaped element with a deliberate padding byte:
    /// the element copy must move the padding too (Q-A2).
    #[test]
    fn q_a2_padding_bytes_moved() {
        // SAFETY: test allocator stand-ins are live (crate test build).
        let mut da = darray_c {
            array: ptr::null_mut(),
            num: 0,
            capacity: 0,
        };
        // element_size 4: each "element" is value byte + 3 bytes kept as
        // padding markers (0xEE) — the copies must move all 4 bytes (Q-A2).
        let mk = |v: u8| -> [u8; 4] { [v, 0, 0, 0xEE] };
        // SAFETY: stand-in allocator; fresh blocks.
        unsafe {
            darray_push_back(4, &mut da, mk(1).as_ptr() as *const c_void);
            darray_push_back(4, &mut da, mk(2).as_ptr() as *const c_void);
            // insert between: the tail (incl. its padding byte) shifts.
            darray_insert(4, &mut da, 1, mk(3).as_ptr() as *const c_void);
            assert_eq!(da.num, 3);
            assert_eq!(
                std::slice::from_raw_parts(da.array, 12),
                &[1, 0, 0, 0xEE, 3, 0, 0, 0xEE, 2, 0, 0, 0xEE]
            );
            // erase middle: elem 2 (with padding) moves down.
            darray_erase(4, &mut da, 1);
            assert_eq!(da.num, 2);
            assert_eq!(
                std::slice::from_raw_parts(da.array, 8),
                &[1, 0, 0, 0xEE, 2, 0, 0, 0xEE]
            );
            // move_item 0 -> 1: whole elements swap order.
            darray_move_item(4, &mut da, 0, 1);
            assert_eq!(
                std::slice::from_raw_parts(da.array, 8),
                &[2, 0, 0, 0xEE, 1, 0, 0, 0xEE]
            );
            bfree(da.array);
        }
    }

    #[test]
    fn growth_sequence_and_noop_move() {
        // SAFETY: test allocator stand-ins are live.
        let mut da = darray_c {
            array: ptr::null_mut(),
            num: 0,
            capacity: 0,
        };
        // SAFETY: stand-in allocator.
        unsafe {
            for _ in 0..5 {
                darray_push_back(1, &mut da, &1u8 as *const u8 as *const c_void);
            }
            assert_eq!(da.capacity, 8);
            darray_move_item(1, &mut da, 2, 2);
            assert_eq!((da.num, da.capacity), (5, 8));
            // insert at the end == push_back
            darray_insert(1, &mut da, da.num, &9u8 as *const u8 as *const c_void);
            assert_eq!(da.num, 6);
            assert_eq!(*da.array.add(5), 9);
            // erase the last element: nothing moves
            darray_erase(1, &mut da, 5);
            assert_eq!(da.num, 5);
            bfree(da.array);
        }
    }
}
