//! Dynamic array (spec S-005, `libobs/util/darray.h`).
//!
//! `#[repr(C)]` layout-compatible with the C `DARRAY(T)` union's `da` member
//! — C inline code and Rust code operate on the same struct
//! (`{ T *array; size_t num; size_t capacity; }`, counts in **elements**).
//! Memory goes through and returns to the S-001 allocator (P1-01).
//!
//! Growth/ordering behaviour reproduces the C inlines exactly (R-A01…R-A14),
//! including the quirks: `find` compares whole-element bytes including
//! padding (Q-A2), `split` copies the whole source into `dst1` (Q-A1).

use std::mem;
use std::ptr;

use crate::bmem;

/// `struct darray` — C field order; `num`/`capacity` count elements
/// (S-005 §3). Fields are public for FFI layout sharing; use the methods,
/// never the raw fields, from Rust code.
#[repr(C)]
pub struct DArray<T>
where
    T: Copy,
{
    pub array: *mut T,
    pub num: usize,
    pub capacity: usize,
}

impl<T: Copy> DArray<T> {
    /// R-A01: init zeroes the struct.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            array: ptr::null_mut(),
            num: 0,
            capacity: 0,
        }
    }

    /// R-A01: `bfree(array)` and zero the struct. Idempotent.
    pub fn free(&mut self) {
        let array = mem::replace(&mut self.array, ptr::null_mut());
        self.num = 0;
        self.capacity = 0;
        if !array.is_null() {
            let bytes = array.cast::<u8>();
            unsafe { bmem::bfree_raw(bytes) };
        }
    }

    /// Element count.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.num
    }

    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.num == 0
    }

    /// R-A14: allocated bytes (`es * num`).
    #[must_use]
    pub const fn alloc_size(&self) -> usize {
        mem::size_of::<T>() * self.num
    }

    /// R-A14: pointer to the last element, or `None`.
    #[must_use]
    pub fn end(&self) -> Option<&T> {
        if self.num == 0 {
            None
        } else {
            Some(unsafe { &*self.array.add(self.num - 1) })
        }
    }

    /// Live elements as a slice.
    #[must_use]
    pub fn as_slice(&self) -> &[T] {
        if self.num == 0 {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.array, self.num) }
        }
    }

    /// Live elements as a mutable slice.
    #[must_use]
    pub fn as_mut_slice(&mut self) -> &mut [T] {
        if self.num == 0 {
            &mut []
        } else {
            unsafe { std::slice::from_raw_parts_mut(self.array, self.num) }
        }
    }

    /// R-A02 — internal to most operations; `num` is not changed.
    pub fn ensure_capacity(&mut self, n: usize) {
        if n <= self.capacity {
            return;
        }
        let new_cap = if self.capacity == 0 {
            n
        } else {
            self.capacity * 2
        };
        let new_cap = new_cap.max(n);
        let es = mem::size_of::<T>();
        let bytes = if self.array.is_null() {
            unsafe { bmem::bmalloc_raw(new_cap * es) }
        } else {
            unsafe { bmem::brealloc_raw(self.array.cast::<u8>(), new_cap * es) }
        };
        self.array = bytes.cast::<T>();
        self.capacity = new_cap;
    }

    /// R-A03: exact growth (no doubling); `c == 0` or a shrink is a no-op.
    pub fn reserve(&mut self, c: usize) {
        if c == 0 || c <= self.capacity {
            return;
        }
        let es = mem::size_of::<T>();
        let bytes = if self.array.is_null() {
            unsafe { bmem::bmalloc_raw(c * es) }
        } else {
            unsafe { bmem::brealloc_raw(self.array.cast::<u8>(), c * es) }
        };
        self.array = bytes.cast::<T>();
        self.capacity = c;
    }

    /// R-A04: `num = 0`, capacity unchanged.
    pub fn clear(&mut self) {
        self.num = 0;
    }

    /// R-A05: grow zero-fills the new elements; shrink keeps capacity.
    pub fn resize(&mut self, n: usize) {
        if n == self.num {
            return;
        }
        if n == 0 {
            self.num = 0;
            return;
        }
        self.ensure_capacity(n);
        if n > self.num {
            let es = mem::size_of::<T>();
            unsafe {
                self.array
                    .add(self.num)
                    .cast::<u8>()
                    .write_bytes(0, (n - self.num) * es);
            }
        }
        self.num = n;
    }

    /// R-A06: returns the new element's index.
    pub fn push_back(&mut self, item: &T) -> usize {
        self.ensure_capacity(self.num + 1);
        let idx = self.num;
        unsafe {
            self.array.add(idx).copy_from_nonoverlapping(item, 1);
        }
        self.num += 1;
        idx
    }

    /// R-A06: zeroed slot, pointer to it.
    pub fn push_back_new(&mut self) -> *mut T {
        self.ensure_capacity(self.num + 1);
        let es = mem::size_of::<T>();
        unsafe { self.array.add(self.num).cast::<u8>().write_bytes(0, es) };
        let ptr = unsafe { self.array.add(self.num) };
        self.num += 1;
        ptr
    }

    /// R-A07: returns the old element count (`0` when `a` is empty —
    /// the C `dst == NULL` case cannot occur through `&mut self`).
    pub fn push_back_array(&mut self, a: &[T]) -> usize {
        if a.is_empty() {
            return self.num;
        }
        let old = self.num;
        self.resize(self.num + a.len());
        unsafe {
            self.array
                .add(old)
                .copy_from_nonoverlapping(a.as_ptr(), a.len());
        }
        old
    }

    /// R-A07: append all of `other`.
    pub fn push_back_darray(&mut self, other: &DArray<T>) -> usize {
        self.push_back_array(other.as_slice())
    }

    /// R-A08 (precondition `idx <= num`): insert one element.
    pub fn insert(&mut self, idx: usize, item: &T) {
        debug_assert!(idx <= self.num);
        if idx == self.num {
            self.push_back(item);
            return;
        }
        let old = self.num;
        self.ensure_capacity(old + 1);
        unsafe {
            ptr::copy(self.array.add(idx), self.array.add(idx + 1), old - idx);
            self.array.add(idx).copy_from_nonoverlapping(item, 1);
        }
        self.num = old + 1;
    }

    /// R-A08: zeroed slot at `idx`, pointer to it.
    pub fn insert_new(&mut self, idx: usize) -> *mut T {
        debug_assert!(idx <= self.num);
        if idx == self.num {
            return self.push_back_new();
        }
        let old = self.num;
        self.ensure_capacity(old + 1);
        unsafe {
            ptr::copy(self.array.add(idx), self.array.add(idx + 1), old - idx);
            let es = mem::size_of::<T>();
            self.array.add(idx).cast::<u8>().write_bytes(0, es);
        }
        self.num = old + 1;
        unsafe { self.array.add(idx) }
    }

    /// R-A08 (Q-A3: `a` empty is an assert in C debug builds): insert a
    /// byte range at `idx`.
    pub fn insert_array(&mut self, idx: usize, a: &[T]) {
        debug_assert!(!a.is_empty());
        debug_assert!(idx <= self.num);
        let n = a.len();
        let old = self.num;
        self.resize(old + n);
        unsafe {
            ptr::copy(self.array.add(idx), self.array.add(idx + n), old - idx);
            self.array.add(idx).copy_from_nonoverlapping(a.as_ptr(), n);
        }
    }

    /// R-A09 (precondition `idx < num`): capacity unchanged.
    pub fn erase(&mut self, idx: usize) {
        debug_assert!(idx < self.num);
        let old = self.num;
        self.num = old - 1;
        if idx < old - 1 {
            unsafe {
                ptr::copy(self.array.add(idx + 1), self.array.add(idx), old - idx - 1);
            }
        }
    }

    /// R-A09: erase the first byte-equal element (whole-element compare,
    /// Q-A2); `true` when found.
    pub fn erase_item(&mut self, item: &T) -> bool {
        match self.find(item, 0) {
            Some(idx) => {
                self.erase(idx);
                true
            }
            None => false,
        }
    }

    /// R-A09 (preconditions `start <= num, end <= num, end > start`).
    pub fn erase_range(&mut self, start: usize, end: usize) {
        debug_assert!(start <= self.num);
        debug_assert!(end <= self.num);
        debug_assert!(end > start);
        let n = end - start;
        let old = self.num;
        self.num = old - n;
        if end < old {
            unsafe {
                ptr::copy(self.array.add(end), self.array.add(start), old - end);
            }
        }
    }

    /// R-A10 (precondition `from <= num`): first index `i >= from`
    /// byte-equal to `item` over the **whole element** including padding
    /// (Q-A2); `None` when absent (C: `DARRAY_INVALID`).
    #[must_use]
    pub fn find(&self, item: &T, from: usize) -> Option<usize> {
        debug_assert!(from <= self.num);
        let es = mem::size_of::<T>();
        let item_bytes = unsafe { std::slice::from_raw_parts(item as *const T as *const u8, es) };
        (from..self.num).find(|&i| {
            let elem = unsafe { std::slice::from_raw_parts(self.array.add(i) as *const u8, es) };
            elem == item_bytes
        })
    }

    /// R-A11: copy all of `src` (`src` empty → this array freed, capacity 0).
    pub fn copy_from(&mut self, src: &DArray<T>) {
        if src.num == 0 {
            self.free();
            return;
        }
        self.resize(src.num);
        unsafe {
            self.array.copy_from_nonoverlapping(src.array, src.num);
        }
    }

    /// R-A11: `resize(n)` + copy.
    pub fn copy_array(&mut self, a: &[T]) {
        self.resize(a.len());
        if !a.is_empty() {
            unsafe {
                self.array.copy_from_nonoverlapping(a.as_ptr(), a.len());
            }
        }
    }

    /// R-A11: take ownership of `src`'s contents; `src` becomes empty.
    pub fn move_from(&mut self, src: &mut DArray<T>) {
        self.free();
        self.array = src.array;
        self.num = src.num;
        self.capacity = src.capacity;
        // Clear the fields before overwriting the value: dropping the old
        // `*src` would free the pointer we just took (double free).
        src.array = ptr::null_mut();
        src.num = 0;
        src.capacity = 0;
    }

    /// R-A12: `erase(0)` (precondition `num != 0`).
    pub fn pop_front(&mut self) -> T {
        let item = unsafe { *self.array };
        self.erase(0);
        item
    }

    /// R-A12: `erase(num - 1)` (precondition `num != 0`).
    pub fn pop_back(&mut self) -> T {
        let item = unsafe { *self.array.add(self.num - 1) };
        self.erase(self.num - 1);
        item
    }

    /// R-A12: append all of `other`, then free it.
    pub fn join_from(&mut self, other: &mut DArray<T>) {
        self.push_back_darray(other);
        other.free();
    }

    /// R-A12 / Q-A1: split `self` at `idx` into `dst1`/`dst2`. `dst1`
    /// receives the **whole** source when `idx != 0` (the C quirk,
    /// reproduced deliberately); `dst2` receives `[idx, num)` when
    /// `idx < num - 1`. Both destinations are freed first.
    pub fn split_into(&mut self, dst1: &mut DArray<T>, dst2: &mut DArray<T>, idx: usize) {
        debug_assert!(self.num >= idx);
        debug_assert!(!ptr::eq(dst1, dst2));
        dst1.free();
        dst2.free();
        if self.num != 0 {
            if idx != 0 {
                // Q-A1: the whole source, not [0, idx)
                dst1.copy_from(self);
            }
            if idx < self.num - 1 {
                dst2.resize(self.num - idx);
                unsafe {
                    dst2.array
                        .copy_from_nonoverlapping(self.array.add(idx), self.num - idx);
                }
            }
        }
    }

    /// R-A13: move the element at `from` to `to`, shifting the in-between
    /// elements. `from == to` → nothing.
    pub fn move_item(&mut self, from: usize, to: usize) {
        debug_assert!(from < self.num);
        debug_assert!(to < self.num);
        if from == to {
            return;
        }
        let item = unsafe { *self.array.add(from) };
        if from < to {
            unsafe { ptr::copy(self.array.add(from + 1), self.array.add(from), to - from) };
        } else {
            unsafe { ptr::copy(self.array.add(to), self.array.add(to + 1), from - to) };
        }
        unsafe { *self.array.add(to) = item };
    }

    /// R-A13: byte-swap the elements at `a` and `b`. `a == b` → nothing.
    pub fn swap_items(&mut self, a: usize, b: usize) {
        debug_assert!(a < self.num);
        debug_assert!(b < self.num);
        if a == b {
            return;
        }
        let es = mem::size_of::<T>();
        let tmp = unsafe { bmem::bmalloc_raw(es) };
        unsafe {
            ptr::copy_nonoverlapping(self.array.add(a) as *const u8, tmp, es);
            ptr::copy_nonoverlapping(
                self.array.add(b) as *const u8,
                self.array.add(a) as *mut u8,
                es,
            );
            ptr::copy_nonoverlapping(tmp, self.array.add(b) as *mut u8, es);
            bmem::bfree_raw(tmp);
        }
    }
}

impl<T: Copy> Default for DArray<T> {
    fn default() -> Self {
        Self::new()
    }
}

impl<T: Copy> Drop for DArray<T> {
    fn drop(&mut self) {
        self.free();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Padding-free `repr(C)` element: the whole element is four content
    /// bytes, so a whole-element compare is observable deterministically.
    #[repr(C)]
    #[derive(Copy, Clone, Debug, PartialEq)]
    struct Raw {
        x: [u8; 4],
    }

    /// Deterministic LCG (same scheme as the deque property test).
    struct Lcg(u64);
    impl Lcg {
        fn below(&mut self, n: usize) -> usize {
            self.0 = self
                .0
                .wrapping_mul(6364136223846793005)
                .wrapping_add(1442695040888963407);
            (self.0 % n as u64) as usize
        }
    }

    #[test]
    fn layout_matches_c_struct() {
        // S-005 §3: { T *array; size_t num; size_t capacity; } — 3W.
        let w = mem::size_of::<usize>();
        assert_eq!(mem::size_of::<DArray<u32>>(), 3 * w);
        assert_eq!(mem::offset_of!(DArray<u32>, array), 0);
        assert_eq!(mem::offset_of!(DArray<u32>, num), w);
        assert_eq!(mem::offset_of!(DArray<u32>, capacity), 2 * w);
    }

    #[test]
    fn init_and_free() {
        // R-A01
        let mut d = DArray::<u32>::new();
        assert!(d.array.is_null());
        assert_eq!((d.num, d.capacity), (0, 0));
        d.push_back(&1);
        d.free();
        assert!(d.array.is_null());
        assert_eq!((d.num, d.capacity), (0, 0));
    }

    #[test]
    fn growth_sequence() {
        // R-A02: 1, 2, 4, 8, 16 ...
        let mut d = DArray::<u32>::new();
        for i in 0..6u32 {
            d.push_back(&i);
            let expected_cap = [1usize, 2, 4, 4, 8, 8][i as usize];
            assert_eq!(d.capacity, expected_cap, "push {i}");
        }
        assert_eq!(d.as_slice(), &[0, 1, 2, 3, 4, 5]);
    }

    #[test]
    fn ensure_grows_to_n_when_larger_than_double() {
        // R-A02: new_cap = max(capacity * 2, n).
        let mut d = DArray::<u32>::new();
        d.push_back(&1); // capacity 1
        d.ensure_capacity(10);
        assert_eq!(d.capacity, 10);
        assert_eq!(d.num, 1); // num untouched
        d.ensure_capacity(16);
        assert_eq!(d.capacity, 20); // doubles to 20 before the max
        d.ensure_capacity(20);
        assert_eq!(d.capacity, 20); // no-op
    }

    #[test]
    fn reserve_exact() {
        // R-A03
        let mut d = DArray::<u32>::new();
        d.reserve(7);
        assert_eq!(d.capacity, 7); // exact, no doubling
        d.push_back(&9);
        d.reserve(3);
        assert_eq!(d.capacity, 7); // shrink: nothing
        d.reserve(0);
        assert_eq!(d.capacity, 7);
        assert_eq!(d.as_slice(), &[9]);
    }

    #[test]
    fn clear_keeps_capacity() {
        // R-A04
        let mut d = DArray::<u32>::new();
        for i in 0..5u32 {
            d.push_back(&i);
        }
        assert_eq!(d.capacity, 8);
        d.clear();
        assert_eq!((d.num, d.capacity), (0, 8));
        assert!(d.as_slice().is_empty());
    }

    #[test]
    fn resize_zero_fills_and_shrinks() {
        // R-A05
        let mut d = DArray::<u32>::new();
        for i in 0..3u32 {
            d.push_back(&i);
        }
        d.resize(6);
        assert_eq!(d.as_slice(), &[0, 1, 2, 0, 0, 0]);
        d.resize(2);
        assert_eq!((d.num, d.capacity), (2, 8)); // capacity kept
        d.resize(2); // no-op
        assert_eq!(d.as_slice(), &[0, 1]);
        d.resize(0);
        assert_eq!((d.num, d.capacity), (0, 8));
    }

    #[test]
    fn push_back_and_new() {
        // R-A06
        let mut d = DArray::<u32>::new();
        assert_eq!(d.push_back(&7), 0);
        assert_eq!(d.push_back(&8), 1);
        let p = d.push_back_new();
        assert_eq!(d.len(), 3);
        assert_eq!(unsafe { *p }, 0); // zeroed slot
        unsafe { *p = 42 };
        assert_eq!(d.as_slice(), &[7, 8, 42]);
    }

    #[test]
    fn push_back_array_and_darray() {
        // R-A07
        let mut d = DArray::<u32>::new();
        assert_eq!(d.push_back_array(&[1, 2, 3]), 0); // returns old num
        assert_eq!(d.push_back_array(&[]), 3); // empty: nothing, returns num
        let mut other = DArray::<u32>::new();
        other.push_back(&4);
        assert_eq!(d.push_back_darray(&other), 3);
        assert_eq!(d.as_slice(), &[1, 2, 3, 4]);
    }

    #[test]
    fn insert_variants() {
        // R-A08
        let mut d = DArray::<u32>::new();
        for i in 1..=4u32 {
            d.push_back(&i);
        }
        d.insert(1, &9);
        assert_eq!(d.as_slice(), &[1, 9, 2, 3, 4]);
        d.insert(0, &5);
        assert_eq!(d.as_slice(), &[5, 1, 9, 2, 3, 4]);
        d.insert(6, &6); // idx == num -> push_back
        assert_eq!(d.as_slice(), &[5, 1, 9, 2, 3, 4, 6]);
        let p = d.insert_new(0);
        assert_eq!(unsafe { *p }, 0);
        assert_eq!(d.len(), 8);
        d.insert_array(2, &[70, 71]);
        assert_eq!(d.as_slice(), &[0, 5, 70, 71, 1, 9, 2, 3, 4, 6]);
    }

    #[test]
    fn erase_variants() {
        // R-A09
        let mut d = DArray::<u32>::new();
        for i in 0..5u32 {
            d.push_back(&i);
        }
        let cap = d.capacity;
        d.erase(2);
        assert_eq!(d.as_slice(), &[0, 1, 3, 4]);
        assert_eq!(d.capacity, cap); // unchanged
        d.erase(0);
        assert_eq!(d.as_slice(), &[1, 3, 4]);
        d.erase(2); // last
        assert_eq!(d.as_slice(), &[1, 3]);
        assert!(d.erase_item(&3));
        assert!(!d.erase_item(&3));
        assert_eq!(d.as_slice(), &[1]);
        d.insert_array(1, &[10, 11, 12, 13]);
        d.erase_range(1, 3); // [10, 11]
        assert_eq!(d.as_slice(), &[1, 12, 13]);
        d.erase_range(0, 3); // whole
        assert_eq!(d.num, 0);
    }

    #[test]
    fn find_first_match_from_and_absent() {
        // R-A10: first i >= from, DARRAY_INVALID (None) when absent.
        let mut d = DArray::<u32>::new();
        for v in [3u32, 7, 3, 9, 7] {
            d.push_back(&v);
        }
        assert_eq!(d.find(&3, 0), Some(0));
        assert_eq!(d.find(&3, 1), Some(2)); // first match from `from`
        assert_eq!(d.find(&3, 3), None);
        assert_eq!(d.find(&7, 0), Some(1));
        assert_eq!(d.find(&7, 4), Some(4));
        assert_eq!(d.find(&42, 0), None);
        assert_eq!(d.find(&7, 5), None); // from == num
    }

    #[test]
    fn find_compares_whole_element_bytes() {
        // R-A10 + Q-A2: the compare covers the whole element, not just
        // "logical" fields. Padding participation (the C memcmp quirk) is
        // reproduced by construction — `es` bytes — but Rust padding bytes
        // are unspecified, so the quirk itself is pinned by the C
        // conformance substep, not here.
        let a = Raw { x: [1, 2, 3, 4] };
        let b = Raw { x: [1, 2, 3, 5] };
        let mut d = DArray::<Raw>::new();
        d.push_back(&a);
        d.push_back(&b);
        assert_eq!(d.find(&a, 0), Some(0));
        assert_eq!(d.find(&b, 0), Some(1)); // last byte differs -> distinct
        assert_eq!(d.find(&b, 1), Some(1));
        let absent = Raw { x: [9, 9, 9, 9] };
        assert_eq!(d.find(&absent, 0), None);
    }

    #[test]
    fn copy_move_and_arrays() {
        // R-A11
        let mut src = DArray::<u32>::new();
        for i in 0..4u32 {
            src.push_back(&i);
        }
        let mut dst = DArray::<u32>::new();
        dst.push_back(&99);
        dst.copy_from(&src);
        assert_eq!(dst.as_slice(), &[0, 1, 2, 3]);
        let mut empty = DArray::<u32>::new();
        empty.push_back(&1);
        empty.push_back(&2);
        let no_src = DArray::<u32>::new();
        empty.copy_from(&no_src); // src empty -> free, capacity 0
        assert_eq!((empty.num, empty.capacity), (0, 0));
        assert!(empty.array.is_null());

        let mut moved = DArray::<u32>::new();
        moved.copy_array(&[7, 8]);
        assert_eq!(moved.as_slice(), &[7, 8]);

        let mut a = DArray::<u32>::new();
        a.push_back(&1);
        let mut b = DArray::<u32>::new();
        b.move_from(&mut a);
        assert_eq!(b.as_slice(), &[1]);
        assert_eq!((a.num, a.capacity), (0, 0));
        assert!(a.array.is_null());
    }

    #[test]
    fn pop_join_and_split() {
        // R-A12 + Q-A1
        let mut d = DArray::<u32>::new();
        for i in 0..4u32 {
            d.push_back(&i);
        }
        assert_eq!(d.pop_front(), 0);
        assert_eq!(d.pop_back(), 3);
        assert_eq!(d.as_slice(), &[1, 2]);

        let mut other = DArray::<u32>::new();
        other.push_back(&10);
        d.join_from(&mut other);
        assert_eq!(d.as_slice(), &[1, 2, 10]);
        assert_eq!(other.num, 0);
        assert!(other.array.is_null());

        // split at idx 1 of [1, 2, 10]:
        // Q-A1: dst1 gets the WHOLE source (not [0,1));
        // dst2 gets [1, num) = [2, 10].
        let mut d1 = DArray::<u32>::new();
        let mut d2 = DArray::<u32>::new();
        d.split_into(&mut d1, &mut d2, 1);
        assert_eq!(d1.as_slice(), &[1, 2, 10]); // Q-A1
        assert_eq!(d2.as_slice(), &[2, 10]);
        // split at idx 0: dst1 stays empty, dst2 gets [0, num).
        let mut d3 = DArray::<u32>::new();
        let mut d4 = DArray::<u32>::new();
        d.split_into(&mut d3, &mut d4, 0);
        assert!(d3.is_empty());
        assert_eq!(d4.as_slice(), &[1, 2, 10]);
        // single element, idx 0: both empty (C defect, reproduced).
        let mut single = DArray::<u32>::new();
        single.push_back(&5);
        let mut d5 = DArray::<u32>::new();
        let mut d6 = DArray::<u32>::new();
        single.split_into(&mut d5, &mut d6, 0);
        assert!(d5.is_empty());
        assert!(d6.is_empty());
    }

    #[test]
    fn move_item_and_swap() {
        // R-A13
        let mut d = DArray::<u32>::new();
        for i in 0..5u32 {
            d.push_back(&i);
        }
        d.move_item(0, 3);
        assert_eq!(d.as_slice(), &[1, 2, 3, 0, 4]);
        d.move_item(3, 1);
        assert_eq!(d.as_slice(), &[1, 0, 2, 3, 4]);
        d.move_item(2, 2); // no-op
        assert_eq!(d.as_slice(), &[1, 0, 2, 3, 4]);
        d.swap_items(0, 4);
        assert_eq!(d.as_slice(), &[4, 0, 2, 3, 1]);
        d.swap_items(1, 1); // no-op
        assert_eq!(d.as_slice(), &[4, 0, 2, 3, 1]);
    }

    #[test]
    fn end_and_alloc_size() {
        // R-A14
        let mut d = DArray::<u32>::new();
        assert_eq!(d.end(), None);
        assert_eq!(d.alloc_size(), 0);
        d.push_back(&1);
        d.push_back(&2);
        assert_eq!(*d.end().unwrap(), 2);
        assert_eq!(d.alloc_size(), 8); // 2 * sizeof(u32)
    }

    #[test]
    fn random_ops_match_vec_model() {
        // Property test: random op sequences must keep the logical element
        // sequence equal to a Vec model (the C comparison is the blocked
        // C-checkout substep).
        let mut d = DArray::<u32>::new();
        let mut model: Vec<u32> = Vec::new();
        let mut rng = Lcg(0xd1a1_a1a1);
        for _ in 0..2000 {
            match rng.below(10) {
                0..=3 => {
                    d.push_back(&(rng.below(1000) as u32));
                    model.push(d.as_slice().last().copied().unwrap());
                }
                4 => {
                    let n = 1 + rng.below(4);
                    let bytes: Vec<u32> = (0..n).map(|_| rng.below(1000) as u32).collect();
                    d.push_back_array(&bytes);
                    model.extend(bytes);
                }
                5 => {
                    if !model.is_empty() {
                        let idx = rng.below(model.len() + 1);
                        let item = rng.below(1000) as u32;
                        d.insert(idx, &item);
                        model.insert(idx, item);
                    }
                }
                6 => {
                    if !model.is_empty() {
                        let idx = rng.below(model.len());
                        d.erase(idx);
                        model.remove(idx);
                    }
                }
                7 => {
                    if model.len() >= 2 {
                        let a = rng.below(model.len());
                        let b = rng.below(model.len());
                        d.swap_items(a, b);
                        model.swap(a, b);
                    } else if model.len() == 1 && rng.below(2) == 0 {
                        let _ = d.pop_back();
                        model.pop();
                    }
                }
                8 => {
                    let n = rng.below(12);
                    d.resize(n);
                    model.resize(n, 0);
                }
                _ => {
                    if model.len() >= 2 && rng.below(2) == 0 {
                        let start = rng.below(model.len());
                        let end = start + 1 + rng.below(model.len() - start);
                        d.erase_range(start, end);
                        model.drain(start..end);
                    } else {
                        let from = rng.below(model.len() + 1);
                        let probe = if model.is_empty() {
                            0
                        } else {
                            model[rng.below(model.len())]
                        };
                        let expect = model
                            .iter()
                            .enumerate()
                            .find(|(i, x)| *i >= from && **x == probe)
                            .map(|(i, _)| i);
                        assert_eq!(d.find(&probe, from), expect);
                    }
                }
            }
            assert_eq!(d.as_slice(), &model, "diverged");
        }
    }
}
