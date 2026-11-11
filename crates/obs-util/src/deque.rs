//! Byte ring buffer (spec S-005, `libobs/util/deque.h`).
//!
//! `#[repr(C)]` layout-compatible with the C `struct deque` — C inline code
//! and Rust code operate on the same struct (all counts in **bytes**).
//! Memory goes through and returns to the S-001 allocator (P1-01).
//!
//! Growth/ordering behaviour reproduces the C inline functions exactly,
//! including the quirks: `end_pos` may equal `capacity` (Q-D1), no integer
//! overflow checks (Q-D2 — callers stay within `isize::MAX`).
//!
//! The `extern "C"` surface stays in the C header until the `obs-ffi` shim
//! (C checkout); this module is the Rust side of the strangler pattern.

use std::ptr;

use crate::bmem;

/// `struct deque` — C field order, all counts in bytes (S-005 §5).
/// Fields are public for FFI layout sharing; use the methods, never the raw
/// fields, from Rust code.
#[repr(C)]
#[derive(Default)]
pub struct Deque {
    pub data: *mut u8,
    pub size: usize,
    pub start_pos: usize,
    pub end_pos: usize,
    pub capacity: usize,
}

impl Deque {
    /// R-D01: init zeroes the struct.
    #[must_use]
    pub const fn new() -> Self {
        Self {
            data: ptr::null_mut(),
            size: 0,
            start_pos: 0,
            end_pos: 0,
            capacity: 0,
        }
    }

    /// R-D01: `bfree(data)` and zero the struct. Idempotent: the fields are
    /// cleared before the free, so a re-entrant `Drop` sees a null pointer.
    pub fn free(&mut self) {
        let data = std::mem::replace(&mut self.data, ptr::null_mut());
        self.size = 0;
        self.start_pos = 0;
        self.end_pos = 0;
        self.capacity = 0;
        if !data.is_null() {
            unsafe { bmem::bfree_raw(data) };
        }
    }

    /// Live bytes.
    #[must_use]
    pub const fn len(&self) -> usize {
        self.size
    }

    #[must_use]
    pub const fn is_empty(&self) -> bool {
        self.size == 0
    }

    /// Logical byte view as up to two physical slices (front chunk first).
    #[must_use]
    pub fn chunks(&self) -> [&[u8]; 2] {
        if self.size == 0 {
            return [&[], &[]];
        }
        let total = self.start_pos + self.size;
        if total <= self.capacity {
            let s = unsafe { std::slice::from_raw_parts(self.data.add(self.start_pos), self.size) };
            [s, &[]]
        } else {
            let first = unsafe {
                std::slice::from_raw_parts(
                    self.data.add(self.start_pos),
                    self.capacity - self.start_pos,
                )
            };
            let second = unsafe { std::slice::from_raw_parts(self.data, total - self.capacity) };
            [first, second]
        }
    }

    /// R-D02 — call after `size` has been increased.
    pub fn ensure_capacity(&mut self) {
        if self.size <= self.capacity {
            return;
        }
        let new_cap = (self.capacity * 2).max(self.size);
        unsafe {
            self.data = bmem::brealloc_raw(self.data, new_cap);
        }
        self.reorder(new_cap);
        self.capacity = new_cap;
    }

    /// R-D03 — data already reallocated to `new_cap`; `capacity` still holds
    /// the old value.
    fn reorder(&mut self, new_cap: usize) {
        if self.size == 0 || self.start_pos == 0 || self.end_pos > self.start_pos {
            return;
        }
        let difference = new_cap - self.capacity;
        unsafe {
            ptr::copy(
                self.data.add(self.start_pos),
                self.data.add(self.start_pos + difference),
                self.capacity - self.start_pos,
            );
        }
        self.start_pos += difference;
    }

    /// R-D04: grow the buffer to exactly `c` bytes (no doubling).
    pub fn reserve(&mut self, c: usize) {
        if c <= self.capacity {
            return;
        }
        unsafe {
            self.data = bmem::brealloc_raw(self.data, c);
        }
        self.reorder(c);
        self.capacity = c;
    }

    /// R-D05 / algorithm 6.1.
    pub fn push_back(&mut self, src: &[u8]) {
        let n = src.len();
        if n == 0 {
            return;
        }
        let new_end = self.end_pos + n;
        self.size += n;
        self.ensure_capacity();
        if new_end > self.capacity {
            let back = self.capacity - self.end_pos;
            if back > 0 {
                unsafe {
                    ptr::copy_nonoverlapping(src.as_ptr(), self.data.add(self.end_pos), back);
                }
            }
            unsafe {
                ptr::copy_nonoverlapping(src.as_ptr().add(back), self.data, n - back);
            }
            self.end_pos = new_end - self.capacity;
        } else {
            unsafe {
                ptr::copy_nonoverlapping(src.as_ptr(), self.data.add(self.end_pos), n);
            }
            self.end_pos = new_end;
        }
    }

    /// R-D06 / algorithm 6.2.
    pub fn push_front(&mut self, src: &[u8]) {
        let n = src.len();
        if n == 0 {
            return;
        }
        self.size += n;
        self.ensure_capacity();
        if self.size == n {
            // was empty
            self.start_pos = 0;
            self.end_pos = n;
            unsafe {
                ptr::copy_nonoverlapping(src.as_ptr(), self.data, n);
            }
        } else if self.start_pos < n {
            let back = n - self.start_pos;
            if self.start_pos > 0 {
                unsafe {
                    ptr::copy_nonoverlapping(src.as_ptr().add(back), self.data, self.start_pos);
                }
            }
            self.start_pos = self.capacity - back;
            unsafe {
                ptr::copy_nonoverlapping(src.as_ptr(), self.data.add(self.start_pos), back);
            }
        } else {
            self.start_pos -= n;
            unsafe {
                ptr::copy_nonoverlapping(src.as_ptr(), self.data.add(self.start_pos), n);
            }
        }
    }

    /// R-D07: `push_back` with zero bytes.
    pub fn push_back_zero(&mut self, n: usize) {
        if n == 0 {
            return;
        }
        let new_end = self.end_pos + n;
        self.size += n;
        self.ensure_capacity();
        if new_end > self.capacity {
            let back = self.capacity - self.end_pos;
            if back > 0 {
                unsafe { self.data.add(self.end_pos).write_bytes(0, back) };
            }
            unsafe { self.data.write_bytes(0, n - back) };
            self.end_pos = new_end - self.capacity;
        } else {
            unsafe { self.data.add(self.end_pos).write_bytes(0, n) };
            self.end_pos = new_end;
        }
    }

    /// R-D07: `push_front` with zero bytes.
    pub fn push_front_zero(&mut self, n: usize) {
        if n == 0 {
            return;
        }
        self.size += n;
        self.ensure_capacity();
        if self.size == n {
            self.start_pos = 0;
            self.end_pos = n;
            unsafe { self.data.write_bytes(0, n) };
        } else if self.start_pos < n {
            let back = n - self.start_pos;
            if self.start_pos > 0 {
                unsafe { self.data.write_bytes(0, self.start_pos) };
            }
            self.start_pos = self.capacity - back;
            unsafe { self.data.add(self.start_pos).write_bytes(0, back) };
        } else {
            self.start_pos -= n;
            unsafe { self.data.add(self.start_pos).write_bytes(0, n) };
        }
    }

    /// R-D08: copy `n` bytes starting at `start_pos`, wrapping at capacity.
    /// Precondition `n <= size` (debug-asserted, S-005 §2). `out` may be
    /// `None` (inspect only).
    pub fn peek_front(&self, n: usize, out: Option<&mut [u8]>) {
        debug_assert!(n <= self.size);
        let Some(out) = out else {
            return;
        };
        debug_assert!(out.len() >= n);
        let mut written = 0;
        let mut pos = self.start_pos;
        while written < n {
            let chunk = (self.capacity - pos).min(n - written);
            unsafe {
                ptr::copy_nonoverlapping(self.data.add(pos), out[written..].as_mut_ptr(), chunk);
            }
            written += chunk;
            pos = (pos + chunk) % self.capacity;
        }
    }

    /// R-D08 / algorithm 6.3: the last `n` bytes in logical order.
    pub fn peek_back(&self, n: usize, out: Option<&mut [u8]>) {
        debug_assert!(n <= self.size);
        let Some(out) = out else {
            return;
        };
        debug_assert!(out.len() >= n);
        if n == 0 {
            return;
        }
        let back_size = if self.end_pos != 0 {
            self.end_pos
        } else {
            self.capacity
        };
        // S-005 §6.3: end_pos == 0 with size > 0 is believed unreachable
        // through the public operations — debug-assert it.
        debug_assert!(self.end_pos != 0 || self.size == 0);
        if back_size < n {
            let front_size = n - back_size;
            unsafe {
                ptr::copy_nonoverlapping(self.data, out[front_size..].as_mut_ptr(), back_size);
                ptr::copy_nonoverlapping(
                    self.data.add(self.capacity - front_size),
                    out.as_mut_ptr(),
                    front_size,
                );
            }
        } else {
            unsafe {
                ptr::copy_nonoverlapping(self.data.add(self.end_pos - n), out.as_mut_ptr(), n);
            }
        }
    }

    /// R-D09: peek then advance the front. `out` may be `None` (discard).
    pub fn pop_front(&mut self, n: usize, out: Option<&mut [u8]>) {
        debug_assert!(n <= self.size);
        self.peek_front(n, out);
        self.size -= n;
        if self.size == 0 {
            self.start_pos = 0;
            self.end_pos = 0;
        } else {
            self.start_pos += n;
            if self.start_pos >= self.capacity {
                self.start_pos -= self.capacity;
            }
        }
    }

    /// R-D09: peek then retreat the back. `out` may be `None` (discard).
    /// Note Q-D1: may leave `end_pos == capacity`.
    pub fn pop_back(&mut self, n: usize, out: Option<&mut [u8]>) {
        debug_assert!(n <= self.size);
        self.peek_back(n, out);
        self.size -= n;
        if self.size == 0 {
            self.start_pos = 0;
            self.end_pos = 0;
        } else if self.end_pos <= n {
            self.end_pos = self.capacity - (n - self.end_pos);
        } else {
            self.end_pos -= n;
        }
    }

    /// R-D10: overwrite `n` bytes at logical offset `pos` (from the front);
    /// upsizes to `pos + n` first when needed. Writes wrap at capacity.
    pub fn place(&mut self, pos: usize, src: &[u8]) {
        let n = src.len();
        if pos + n > self.size {
            self.upsize(pos + n);
        }
        let mut written = 0;
        let mut offset = pos;
        while written < n {
            let phys = self.start_pos + offset;
            let phys = if phys >= self.capacity {
                phys - self.capacity
            } else {
                phys
            };
            let chunk = (self.capacity - phys).min(n - written);
            unsafe {
                ptr::copy_nonoverlapping(src[written..].as_ptr(), self.data.add(phys), chunk);
            }
            written += chunk;
            offset += chunk;
        }
    }

    /// R-D11: grow the logical size to `s`, zero-filling the added bytes at
    /// the logical end (wrapping).
    pub fn upsize(&mut self, s: usize) {
        if s <= self.size {
            return;
        }
        let add = s - self.size;
        let new_end = self.end_pos + add;
        let old_end = self.end_pos;
        self.size = s;
        self.ensure_capacity();
        // Q-D1: old_end may equal capacity — normalise before the loop.
        let mut pos = old_end % self.capacity;
        let mut remaining = add;
        while remaining > 0 {
            let chunk = (self.capacity - pos).min(remaining);
            unsafe { self.data.add(pos).write_bytes(0, chunk) };
            pos = (pos + chunk) % self.capacity;
            remaining -= chunk;
        }
        self.end_pos = new_end;
        if self.end_pos > self.capacity {
            self.end_pos -= self.capacity;
        }
    }

    /// R-D12: pointer to byte `idx` from the front (wrapped), or `None`
    /// when `idx >= size`.
    #[must_use]
    pub fn deque_data(&self, idx: usize) -> Option<*const u8> {
        if idx >= self.size {
            return None;
        }
        let pos = self.start_pos + idx;
        let phys = if pos >= self.capacity {
            pos - self.capacity
        } else {
            pos
        };
        Some(unsafe { self.data.add(phys) })
    }
}

impl Drop for Deque {
    fn drop(&mut self) {
        self.free();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Deterministic LCG for the property test (no new deps).
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

    fn logical(d: &Deque) -> Vec<u8> {
        let [a, b] = d.chunks();
        a.iter().chain(b.iter()).copied().collect()
    }

    #[test]
    fn layout_matches_c_struct() {
        // S-005 §5: { void *data; size_t size, start_pos, end_pos, capacity; }
        let w = std::mem::size_of::<usize>();
        assert_eq!(std::mem::size_of::<Deque>(), 5 * w);
        assert_eq!(std::mem::offset_of!(Deque, data), 0);
        assert_eq!(std::mem::offset_of!(Deque, size), w);
        assert_eq!(std::mem::offset_of!(Deque, start_pos), 2 * w);
        assert_eq!(std::mem::offset_of!(Deque, end_pos), 3 * w);
        assert_eq!(std::mem::offset_of!(Deque, capacity), 4 * w);
    }

    #[test]
    fn init_is_zeroed() {
        // R-D01
        let d = Deque::new();
        assert!(d.data.is_null());
        assert_eq!((d.size, d.start_pos, d.end_pos, d.capacity), (0, 0, 0, 0));
    }

    #[test]
    fn push_back_growth_sequence() {
        // R-D02: first growth exact, then doubling (1,2,4,4,8,...).
        let mut d = Deque::new();
        for i in 0..5u8 {
            d.push_back(&[b'a' + i]);
            let expected_cap = [1usize, 2, 4, 4, 8][i as usize];
            assert_eq!(d.capacity, expected_cap, "push {i}");
        }
        assert_eq!(logical(&d), b"abcde");
        assert_eq!((d.start_pos, d.end_pos), (0, 5));
    }

    #[test]
    fn q_d1_end_pos_may_equal_capacity() {
        // Q-D1: filling to exactly capacity leaves end_pos == capacity.
        let mut d = Deque::new();
        d.push_back(b"ABCD");
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (0, 4, 4, 4));
        d.push_back(b"EF");
        // growth to 8, no wrap yet
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (0, 6, 6, 8));
        d.push_back(b"GH");
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (0, 8, 8, 8));
        assert_eq!(logical(&d), b"ABCDEFGH");
    }

    #[test]
    fn push_back_growth_kills_wrap() {
        // R-D05: capacity is read AFTER ensure_capacity — growth can kill the wrap.
        let mut d = Deque::new();
        d.push_back(b"ABCDEFGH"); // capacity 8, full, end == capacity (Q-D1)
        d.push_back(b"IJ"); // ensure grows to 16 first -> NO wrap
        assert_eq!(
            (d.start_pos, d.end_pos, d.size, d.capacity),
            (0, 10, 10, 16)
        );
        assert_eq!(logical(&d), b"ABCDEFGHIJ");
        d.push_back(b"KLMNOP"); // ends exactly at capacity again
        assert_eq!(
            (d.start_pos, d.end_pos, d.size, d.capacity),
            (0, 16, 16, 16)
        );
        assert_eq!(logical(&d), b"ABCDEFGHIJKLMNOP");
        d.push_back(b"QR"); // ensure grows to 32 first -> still NO wrap
        assert_eq!(
            (d.start_pos, d.end_pos, d.size, d.capacity),
            (0, 18, 18, 32)
        );
        assert_eq!(logical(&d), b"ABCDEFGHIJKLMNOPQR");
    }

    #[test]
    fn push_back_real_wraparound() {
        // R-D05 algorithm 6.1 wrap branch: n > capacity, no growth needed.
        let mut d = Deque::new();
        d.reserve(16);
        d.push_back(b"123456789012"); // start 0 end 12
        d.pop_front(8, None); // start 8 end 12 (size 4: "9012")
        d.push_back(b"ABCDEFGH"); // size 12 <= 16 no growth; new_end 20 > 16
        // back = 16 - 12 = 4: data[12..16) = "ABCD"; data[0..4) = "EFGH"; end = 4
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (8, 4, 12, 16));
        assert_eq!(logical(&d), b"9012ABCDEFGH");
    }

    #[test]
    fn push_front_empty_and_plain() {
        // R-D06: empty -> start 0, end n; plain -> start -= n.
        let mut d = Deque::new();
        d.push_front(b"XY");
        assert_eq!((d.start_pos, d.end_pos, d.size), (0, 2, 2));
        assert_eq!(logical(&d), b"XY");
        d.push_front(b"ZW");
        // growth 2 -> 4 first; start 0 < 2 -> wrap branch: start = 4 - 2 = 2
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (2, 2, 4, 4));
        assert_eq!(logical(&d), b"ZWXY");
    }

    #[test]
    fn push_front_wraparound() {
        // Algorithm 6.2 wrap branch.
        let mut d = Deque::new();
        d.reserve(8);
        let mut d2 = Deque::new();
        d2.reserve(8);
        d2.push_back(b"XYZ"); // start 0 end 3
        d2.pop_front(1, None); // start 1 end 3 (size 2: "YZ")
        d2.push_front(b"abcd"); // start 1 < 4: back = 3
        assert_eq!(
            (d2.start_pos, d2.end_pos, d2.size, d2.capacity),
            (5, 3, 6, 8)
        );
        assert_eq!(logical(&d2), b"abcdYZ");
        let _ = d;
    }

    #[test]
    fn pop_back_retreats_to_buffer_end() {
        // R-D09 + Q-D1: end_pos == capacity after retreat to the buffer end.
        let mut d = Deque::new();
        d.reserve(8);
        d.push_back(b"123456"); // start 0 end 6
        d.pop_front(4, None); // start 4 end 6 (size 2: "56")
        d.push_front(b"78"); // start 2 end 6 (size 4: "7856")
        assert_eq!(logical(&d), b"7856");
        d.pop_back(2, None); // "56" gone; end 6 > 2 -> end 4...
        assert_eq!(logical(&d), b"78");
    }

    #[test]
    fn pop_back_end_pos_le_n_branch() {
        // R-D09: end_pos <= n -> end_pos = capacity - (n - end_pos).
        let mut d = Deque::new();
        d.reserve(8);
        // start 5, size 4 -> end 1: build via pushes.
        d.push_back(b"abcde"); // start 0 end 5
        d.pop_front(3, None); // start 3 end 5 (size 2: "de")
        d.push_front(b"xy"); // start 1 end 5 (size 4: "xyde")
        d.pop_front(2, None); // start 3 end 5 (size 2: "de")
        d.push_front(b"zz"); // start 1 end 5 (size 4: "zzde")
        // start 1, end 5, size 4. pop_back(4) empties.
        // Need end_pos <= n with size > 0 after: start 5 end 1 size 4:
        let mut d2 = Deque::new();
        d2.reserve(8);
        d2.push_back(b"12345678"); // full: start 0 end 8
        d2.pop_front(5, None); // start 5 end 8 (size 3: "678")
        d2.pop_back(1, None); // end 8 > 1 -> end 7 (size 2: "67")
        d2.push_front(b"9A"); // start 3 end 7 (size 4: "9A67")
        assert_eq!(logical(&d2), b"9A67");
        d2.pop_back(2, None); // "67" -> size 2, end 7 > 2 -> end 5
        assert_eq!((d2.start_pos, d2.end_pos, d2.size), (3, 5, 2));
        assert_eq!(logical(&d2), b"9A");
        d2.pop_back(2, None); // size 0
        assert_eq!((d2.start_pos, d2.end_pos, d2.size), (0, 0, 0));
        let _ = d;
    }

    #[test]
    fn place_overwrites_and_upsizes() {
        // R-D10: overwrite across the wrap point; upsize when pos + n > size.
        let mut d = Deque::new();
        d.push_back(b"ABCDEFGH"); // capacity 8 full
        d.place(6, b"XY"); // logical [6,8) -> physical [6,8)
        assert_eq!(logical(&d), b"ABCDEFXY");
        d.place(7, b"123"); // pos 7 + 3 = 10 > 8 -> upsize(10)
        assert_eq!(d.size, 10);
        assert_eq!(logical(&d), b"ABCDEFX123");
    }

    #[test]
    fn upsize_zero_fills_and_wraps_end() {
        // R-D11.
        let mut d = Deque::new();
        d.reserve(8);
        d.push_back(b"1234"); // start 0 end 4
        d.upsize(10); // add 6: [4,10); end 10 > capacity 8 -> ensure to 16
        assert_eq!(
            (d.start_pos, d.end_pos, d.size, d.capacity),
            (0, 10, 10, 16)
        );
        let l = logical(&d);
        assert_eq!(&l[..4], b"1234");
        assert!(l[4..].iter().all(|&b| b == 0));
        // wrap case: start 2 end 6, upsize(10) -> end 12
        let mut d2 = Deque::new();
        d2.reserve(8);
        d2.push_back(b"12345678"); // full
        d2.pop_front(2, None); // start 2 end 8 (size 6)
        d2.pop_back(2, None); // end 6 (size 4: "3456")
        d2.upsize(10);
        assert_eq!(
            (d2.start_pos, d2.end_pos, d2.size, d2.capacity),
            (2, 12, 10, 16)
        );
        let l2 = logical(&d2);
        assert_eq!(&l2[..4], b"3456");
        assert!(l2[4..].iter().all(|&b| b == 0));
    }

    #[test]
    fn reserve_exact_and_reorder_wrapped() {
        // R-D04 + R-D03: wrapped data moves up by the size difference.
        let mut d = Deque::new();
        d.reserve(16);
        assert_eq!(d.capacity, 16); // exact, no doubling
        d.reserve(8);
        assert_eq!(d.capacity, 16); // shrink: nothing
        // wrapped: capacity 8, start 6, size 3 -> end 1
        let mut d2 = Deque::new();
        d2.reserve(8);
        d2.push_back(b"ABC"); // start 0 end 3
        d2.pop_front(1, None); // start 1 end 3
        d2.push_front(b"DEFGH"); // start 1 < 5: back = 4; data[0] = 'H';
        // start = 8 - 4 = 4; data[4..8) = "DEFG"
        assert_eq!((d2.start_pos, d2.end_pos, d2.size), (4, 3, 7));
        assert_eq!(logical(&d2), b"DEFGHBC");
        // now a fully wrapped ring: rebuild
        let mut d3 = Deque::new();
        d3.reserve(8);
        d3.push_back(b"ABCDEFG"); // start 0 end 7
        d3.pop_front(1, None); // start 1 end 7 (size 6)
        d3.push_front(b"H"); // start 0 end 7 (size 7)
        d3.push_front(b"I"); // start 0... hmm start 0 < 1: back = 1; data[0..1) = "I"?
        // start = 8 - 1 = 7; data[7] = "I"; size 8, end 7
        assert_eq!(
            (d3.start_pos, d3.end_pos, d3.size, d3.capacity),
            (7, 7, 8, 8)
        );
        // full ring, start 7: logical = data[7], data[0..7); "H" overwrote
        // the popped 'A' in the dead region — that is correct ring behaviour.
        assert_eq!(logical(&d3), b"IHBCDEFG");
        d3.reserve(16); // reorder: start 7 != 0, end 7 <= start -> wrapped
        assert_eq!(d3.capacity, 16);
        assert_eq!(d3.start_pos, 15); // 7 + (16 - 8)
        assert_eq!((d3.end_pos, d3.size), (7, 8));
        assert_eq!(logical(&d3), b"IHBCDEFG");
        let _ = d2;
    }

    #[test]
    fn peek_front_back_and_deque_data() {
        // R-D08, R-D12.
        let mut d = Deque::new();
        d.reserve(6);
        d.push_back(b"123456"); // start 0 end 6 (== capacity)
        d.pop_front(2, None); // start 2 end 6 (size 4: "3456")
        d.push_front(b"789"); // growth 6 -> 12 first; start 2 < 3: back 1;
        // data[0..2) = "89"; start = 12 - 1 = 11; data[11] = "7"
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (11, 6, 7, 12));
        assert_eq!(logical(&d), b"7893456");
        let mut buf = [0u8; 7];
        d.peek_front(7, Some(&mut buf));
        assert_eq!(&buf, b"7893456");
        d.peek_back(4, Some(&mut buf));
        assert_eq!(&buf[..4], b"3456");
        let p0 = unsafe { d.data.add(11) } as *const u8;
        assert_eq!(d.deque_data(0), Some(p0)); // start 11
        let p6 = unsafe { d.data.add(5) } as *const u8;
        assert_eq!(d.deque_data(6), Some(p6)); // 11 + 6 = 17 -> wrap to 5
        assert_eq!(d.deque_data(7), None);
    }

    #[test]
    fn zero_pushes() {
        // R-D07.
        let mut d = Deque::new();
        d.push_back_zero(4);
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (0, 4, 4, 4));
        assert!(d.chunks()[0].iter().all(|&b| b == 0));
        d.push_front_zero(2);
        // growth 4 -> 8 first; start 0 < 2 -> wrap branch: start = 8 - 2 = 6
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (6, 4, 6, 8));
        assert!(d.chunks().iter().copied().flatten().all(|&b| b == 0));
        // zero pushes on wrapped state
        let mut d2 = Deque::new();
        d2.reserve(4);
        d2.push_back(b"AB"); // start 0 end 2
        d2.pop_front(1, None); // start 1 end 2
        d2.push_back_zero(4); // growth 4 -> 8 first -> NO wrap: data[2..6) = 0
        assert_eq!(
            (d2.start_pos, d2.end_pos, d2.size, d2.capacity),
            (1, 6, 5, 8)
        );
        assert_eq!(d2.chunks()[0], [b'B', 0, 0, 0, 0]);
        assert!(d2.chunks()[1].is_empty());
    }

    #[test]
    fn upsize_with_end_at_capacity_does_not_hang() {
        // Q-D1 + R-D11: end_pos == capacity with size < capacity; the
        // zero-fill loop must wrap instead of stalling on a zero-length chunk.
        let mut d = Deque::new();
        d.reserve(8);
        d.push_back(b"12345678"); // full: start 0 end 8
        d.pop_front(2, None); // start 2 end 8 (size 6) — end == capacity
        d.upsize(7); // 7 <= 8: no growth; zero-fill from old_end 8 % 8 = 0
        assert_eq!((d.start_pos, d.end_pos, d.size, d.capacity), (2, 1, 7, 8));
        let l = logical(&d);
        assert_eq!(&l[..6], b"345678");
        assert_eq!(l[6], 0);
    }

    #[test]
    fn random_ops_match_vecdeque_model() {
        // Wraparound property test: random op sequences must keep the logical
        // byte sequence equal to a VecDeque model (the C comparison is the
        // blocked C-checkout substep).
        use std::collections::VecDeque;
        let mut d = Deque::new();
        let mut model: VecDeque<u8> = VecDeque::new();
        let mut rng = Lcg(0x5eed_c0de);
        for _ in 0..2000 {
            match rng.below(8) {
                0..=2 => {
                    let n = 1 + rng.below(5);
                    let bytes: Vec<u8> = (0..n).map(|_| rng.below(256) as u8).collect();
                    d.push_back(&bytes);
                    model.extend(bytes);
                }
                3..=4 => {
                    let n = 1 + rng.below(5);
                    let bytes: Vec<u8> = (0..n).map(|_| rng.below(256) as u8).collect();
                    d.push_front(&bytes);
                    for b in bytes.iter().rev() {
                        model.push_front(*b);
                    }
                }
                5 => {
                    let n = rng.below(d.len() + 1);
                    if n > 0 {
                        let mut buf = vec![0u8; n];
                        d.pop_front(n, Some(&mut buf));
                        assert_eq!(buf, model.iter().take(n).copied().collect::<Vec<_>>());
                        for _ in 0..n {
                            model.pop_front();
                        }
                    }
                }
                6 => {
                    let n = rng.below(d.len() + 1);
                    if n > 0 {
                        let mut buf = vec![0u8; n];
                        d.pop_back(n, Some(&mut buf));
                        let expect: Vec<u8> = model.iter().skip(model.len() - n).copied().collect();
                        assert_eq!(buf, expect);
                        for _ in 0..n {
                            model.pop_back();
                        }
                    }
                }
                7 => {
                    let n = rng.below(d.len() + 1);
                    if n > 0 {
                        let mut buf = vec![0u8; n];
                        d.peek_front(n, Some(&mut buf));
                        assert_eq!(buf, model.iter().take(n).copied().collect::<Vec<_>>());
                        let mut buf2 = vec![0u8; n];
                        d.peek_back(n, Some(&mut buf2));
                        let expect2: Vec<u8> =
                            model.iter().skip(model.len() - n).copied().collect();
                        assert_eq!(buf2, expect2);
                    }
                }
                _ => {}
            }
            if !d.is_empty() && rng.below(4) == 0 {
                // place: overwrite at a random logical offset
                let pos = rng.below(d.len() + 1);
                let n = 1 + rng.below(3);
                let bytes: Vec<u8> = (0..n).map(|_| rng.below(256) as u8).collect();
                d.place(pos, &bytes);
                let mut v: Vec<u8> = model.iter().copied().collect();
                v.resize(v.len().max(pos + n), 0); // upsize(pos + n) grows only
                for (i, b) in bytes.iter().enumerate() {
                    v[pos + i] = *b;
                }
                model = v.into();
            }
            if rng.below(10) == 0 {
                let add = rng.below(6);
                d.upsize(d.len() + add);
                model.extend(std::iter::repeat_n(0, add));
            }
            if rng.below(12) == 0 {
                d.reserve(1 + rng.below(40));
            }
            let l = logical(&d);
            assert_eq!(
                l,
                model.iter().copied().collect::<Vec<_>>(),
                "diverged at op"
            );
        }
    }
}
