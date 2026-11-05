# S-005 Dynamic array (`darray`) and byte ring buffer (`deque`)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `util/darray.h`, `util/deque.h` (header-only) |
| Target crate | `crates/obs-util` (modules `darray`, `deque`) |
| Depends on | S-001 |
| Conformance | `test/conformance/scripts/darray/` (4), `scripts/deque/` (5) |

## 1. Scope

Both containers are **header-only C code**: every function is `static inline` and is
compiled into libobs, every plugin and the frontend. They stay in the C headers. A Rust
port therefore does **not** replace them; it provides *layout-compatible Rust types* with
identical behaviour so that Rust code and C code can operate on the same struct (for
example a `DARRAY(struct foo)` field of a struct shared with a plugin, or a `struct deque`
filled by C and drained by Rust). This spec defines the layouts, the invariants C code relies
on, and the exact growth/ordering behaviour a Rust type must reproduce.

Memory is always obtained from and returned to the S-001 allocator (`bmalloc`/`bfree`,
`brealloc`), with two exceptions noted in R-D40.

## 2. Conventions

`W` = `sizeof(size_t)`. `es` = element size in bytes. Indices are zero-based. "Precondition"
means the reference guards it with `assert()` (aborting in debug builds; undefined behaviour
in release builds); conforming callers never violate it, and a Rust type SHOULD debug-assert it.

## 3. Interface — darray

```c
#define DARRAY_INVALID ((size_t)-1)
struct darray { void *array; size_t num; size_t capacity; };   /* sizeof = 3W; capacity/num count ELEMENTS */

/* DARRAY(T): a union providing both views at the same address */
#define DARRAY(T) union { struct darray da; struct { T *array; size_t num; size_t capacity; }; }
```

Functions (all `static inline`, element size passed explicitly): `darray_init`, `darray_free`,
`darray_alloc_size`, `darray_item`, `darray_end`, `darray_reserve`, `darray_ensure_capacity`,
`darray_clear`, `darray_resize`, `darray_copy`, `darray_copy_array`, `darray_move`,
`darray_find`, `darray_push_back`, `darray_push_back_new`, `darray_push_back_array`,
`darray_push_back_darray`, `darray_insert`, `darray_insert_new`, `darray_insert_array`,
`darray_insert_darray`, `darray_erase`, `darray_erase_item`, `darray_erase_range`,
`darray_pop_front`, `darray_pop_back`, `darray_join`, `darray_split`, `darray_move_item`,
`darray_swap`; plus the `da_*` convenience macros that pass `sizeof(*(v).array)`.

## 4. Behaviour — darray

- **S-005-R-A01** `init`: `array = NULL, num = 0, capacity = 0`. `free`: `bfree(array)` then all three fields zero.
- **S-005-R-A02** `ensure_capacity(n)` (internal to most operations): if `n <= capacity` do nothing. Otherwise `new_cap = (capacity == 0) ? n : capacity * 2`; if `n > new_cap` then `new_cap = n`; allocate `new_cap * es` bytes, copy the first `capacity` elements, free the old block, set `capacity = new_cap`. **`num` is not changed.** (Growth sequence for repeated `push_back` from empty: 1, 2, 4, 8, 16 …; vector `darray/01`.)
- **S-005-R-A03** `reserve(c)`: if `c == 0 || c <= capacity` nothing; otherwise allocate exactly `c * es`, copy `num` elements, free old, `capacity = c` (exact, no doubling).
- **S-005-R-A04** `clear`: `num = 0`, capacity unchanged.
- **S-005-R-A05** `resize(n)`: `n == num` → nothing; `n == 0` → `num = 0` (capacity kept); else ensure_capacity(n), `num = n`, and if growing, **zero-fill** the new elements.
- **S-005-R-A06** `push_back(item)`: ensure_capacity(num + 1), `num += 1`, copy `item` to the last slot; returns the new element's index. `push_back_new`: same but the slot is zeroed and a pointer to it is returned.
- **S-005-R-A07** `push_back_array(a, n)`: `dst == NULL` → returns 0; `a == NULL || n == 0` → returns `num` (nothing added); else `old = num; resize(num + n);` copy; returns `old`. `push_back_darray(other)` = `push_back_array(other.array, other.num)`.
- **S-005-R-A08** `insert(idx, item)` (precondition `idx <= num`): `idx == num` → push_back; else ensure_capacity(num + 1), `num += 1`, move `[idx, old_num)` up by one, copy `item` to `idx`. `insert_new(idx)` likewise with a zeroed slot, returning a pointer. `insert_array(idx, a, n)` (preconditions `a != NULL`, `n != 0`, `idx <= num`): resize(num + n), move the tail up by `n`, copy.
- **S-005-R-A09** `erase(idx)` (precondition `idx < num`): `num -= 1`; if elements remain after `idx`, move `[idx + 1, old_num)` down by one. Capacity unchanged. `erase_item(item)`: erase the first element byte-equal (`memcmp`) to `item`, if any. `erase_range(start, end)` (preconditions `start <= num, end <= num, end > start`): remove `end - start` elements, closing the gap; if the range is the whole array `num = 0`; a range of one element behaves as `erase(start)`.
- **S-005-R-A10** `find(item, from)` (precondition `from <= num`): the first index `i >= from` with `memcmp(array[i], item, es) == 0`, else `DARRAY_INVALID`. Comparison is **byte-wise over the whole element** (padding included).
- **S-005-R-A11** `copy(src)`: `src.num == 0` → `free(dst)` (capacity becomes 0); else `resize(dst, src.num)` and copy. `copy_array(a, n)` = `resize(n)` + copy. `move(dst, src)`: `free(dst)`, `dst = src` (struct copy), `src` = empty (all zero).
- **S-005-R-A12** `pop_front` = `erase(0)`, `pop_back` = `erase(num - 1)`; precondition `num != 0`. `join(dst, da)`: push all of `da` onto `dst`, then free `da`. `split(dst1, dst2, da, idx)` (preconditions `da.num >= idx`, `dst1 != dst2`): copies `da` to a temporary, frees both destinations, then: if `da.num != 0`: `idx != 0` → `dst1` = copy of the **whole** temporary (note: all `num` elements, not just `[0, idx)` — see Q-A1); `idx < num - 1` → `dst2` = copy of `[idx, num)`.
- **S-005-R-A13** `move_item(from, to)` / `swap(a, b)`: standard element move/swap; use plain `malloc` for a temporary (`bcrash` on failure); `a == b` or `from == to` → nothing.
- **S-005-R-A14** `end()`: pointer to the last element, or NULL if `num == 0`. `alloc_size()` = `es * num`.

## 5. Interface and behaviour — deque

```c
struct deque { void *data; size_t size; size_t start_pos; size_t end_pos; size_t capacity; };  /* all counts in BYTES; sizeof = 5W */
```

A deque is a **byte** FIFO/LIFO over a ring buffer. `size` = number of live bytes; `capacity` =
allocated bytes; live data occupies `size` bytes starting at `start_pos`, wrapping at
`capacity`; `end_pos` is the position just after the last live byte (it may equal `capacity`
instead of 0 when the data ends exactly at the end of the buffer).

- **S-005-R-D01** `init` zeroes the struct. `free`: `bfree(data)` and zero the struct.
- **S-005-R-D02** `ensure_capacity` (called after `size` has been increased): if `size <= capacity` nothing; else `new_cap = capacity * 2`, `new_cap = max(new_cap, size)`; `data = brealloc(data, new_cap)`; **reorder** (R-D03); `capacity = new_cap`.
- **S-005-R-D03** `reorder(new_cap)` (data already reallocated, `capacity` still the old value): if `size == 0 || start_pos == 0 || end_pos > start_pos` (data not wrapped) → nothing. Otherwise (wrapped, or full): `difference = new_cap - capacity`; move the bytes `[start_pos, capacity)` up by `difference`; `start_pos += difference`.
- **S-005-R-D04** `reserve(c)`: if `c <= capacity` nothing; else `data = brealloc(data, c)`, reorder (R-D03), `capacity = c`.
- **S-005-R-D05** `push_back(p, n)`: `new_end = end_pos + n; size += n; ensure_capacity`. If `new_end > capacity`: copy `capacity - end_pos` bytes (if any) to `data + end_pos`, the remainder to `data[0..]`, and `new_end -= capacity`; else copy to `data + end_pos`. `end_pos = new_end`. (Note: when `new_end == capacity` it is *not* wrapped to 0.) **`capacity` is read after `ensure_capacity`.**
- **S-005-R-D06** `push_front(p, n)`: `size += n; ensure_capacity`. If the deque was empty (`size == n`): `start_pos = 0; end_pos = n;` copy to `data[0..]`. Else if `start_pos < n` (not enough room in front, so the new bytes wrap): see algorithm 6.2; otherwise `start_pos -= n` and the bytes are copied to `data + start_pos`. `end_pos` is unchanged except in the empty case.
- **S-005-R-D07** `push_back_zero(n)` / `push_front_zero(n)`: as push_back/push_front with zero bytes instead of a source buffer.
- **S-005-R-D08** `peek_front(out, n)` (precondition `n <= size`): if `out != NULL`, copy `n` bytes starting at `start_pos`, wrapping at `capacity`. `peek_back(out, n)`: copy the **last** `n` bytes in logical order (`back_size = end_pos ? end_pos : capacity`; if `back_size < n` the first part comes from the end of the buffer, see 6.3).
- **S-005-R-D09** `pop_front(out, n)`: `peek_front(out, n)`, `size -= n`; if `size == 0` then `start_pos = end_pos = 0`; else `start_pos += n`, minus `capacity` if it reached or passed it. `pop_back(out, n)`: `peek_back`, `size -= n`; if `size == 0` both positions 0; else if `end_pos <= n` then `end_pos = capacity - (n - end_pos)` else `end_pos -= n`. `out` may be NULL (discard).
- **S-005-R-D10** `place(pos, p, n)`: overwrite `n` bytes at logical offset `pos` (relative to the front). If `pos + n > size`, first `upsize(pos + n)`. Writes wrap at `capacity`.
- **S-005-R-D11** `upsize(s)`: if `s <= size` nothing. Else `add = s - size; new_end = end_pos + add; size = s; ensure_capacity;` zero-fill the added `add` bytes at the logical end (wrapping), set `end_pos` to the new end (wrapped when it **exceeds** `capacity`).
- **S-005-R-D12** `deque_data(idx)`: pointer to byte `idx` from the front (`start_pos + idx`, wrapped), or NULL if `idx >= size`.
- **S-005-R-D13** Bytes outside the live ring are unspecified; conformance dumps hide them (`..`).

## 6. Algorithms

### 6.1 `push_back` (R-D05)
```
new_end = end_pos + n; size += n; ensure_capacity()
if new_end > capacity:
    back = capacity - end_pos
    if back > 0: copy(data + end_pos, src, back)
    copy(data, src + back, n - back)
    new_end -= capacity
else:
    copy(data + end_pos, src, n)
end_pos = new_end
```

### 6.2 `push_front` (R-D06)
```
size += n; ensure_capacity()
if size == n:                       # was empty
    start_pos = 0; end_pos = n; copy(data, src, n)
elif start_pos < n:                 # not enough room before the start: wrap
    back = n - start_pos            # bytes that go to the END of the buffer
    if start_pos > 0: copy(data, src + back, start_pos)     # the tail of src goes to [0, start_pos)
    start_pos = capacity - back
    copy(data + start_pos, src, back)                       # the head of src goes to the end
else:
    start_pos -= n; copy(data + start_pos, src, n)
```

### 6.3 `peek_back` (R-D08)
```
back_size = (end_pos != 0) ? end_pos : capacity
if back_size < n:                    # wraps backwards across offset 0
    front_size = n - back_size
    copy(out + front_size, data, back_size)                 # newest bytes: [0, back_size)
    copy(out, data + capacity - front_size, front_size)     # older bytes from the end of the buffer
else:
    copy(out, data + end_pos - n, n)
```
(`end_pos == 0` with `size > 0` is believed unreachable through the public operations — pushes leave `end_pos` in `[1, capacity]` and `pop_back` maps to `capacity` — so the `end_pos ? end_pos : capacity` expression is defensive. No vector exercises it; a Rust port SHOULD debug-assert it.)

## 7. Concurrency

None; both are plain data structures. Callers synchronise.

## 8. Reference defects and quirks

- **Q-A1 (`darray_split` copies everything).** `dst1` receives all `num` elements, not `[0, idx)`. It is unused in-tree except possibly by plugins; a Rust type SHOULD reproduce it for layout-sharing code or omit `split`. No vector.
- **Q-A2 (`find` compares padding).** Uses `memcmp` over the whole element; elements containing padding may fail to match logically equal values. Callers (S-004 global callbacks) already depend on it; see S-004 Q-6. Rust types should compare bytes only when mirroring `da_find`.
- **Q-A3 (`insert_array` with `n == 0` or `array == NULL`)** asserts in debug builds; in release it is undefined.
- **Q-D1 (`end_pos` may equal `capacity`).** Data ending exactly at the buffer end leaves `end_pos == capacity` (not 0) after `push_back` and after `pop_back` that retreats to the buffer end (R-D09). Required: reproduce exactly (the struct is shared with C inline code; vectors `02-wraparound` and `05-growth` show `end=` values).
- **Q-D2 (no integer-overflow checks)** in `size` / `capacity` arithmetic; callers are expected to stay within `isize::MAX`.

## 9. Implementation notes (non-normative)

- `#[repr(C)] pub struct DArray<T> { array: *mut T, num: usize, capacity: usize }` is layout-compatible with `DARRAY(T)` (the union's `da` member). Do not use `Vec<T>` as the storage if C code may `bfree` or grow the array — use the S-001 allocator.
- Provide a safe wrapper that borrows as `&[T]`/`&mut [T]` for read access; mutate only through methods that implement R-A02 growth exactly.
- `Deque` needs `unsafe` pointer arithmetic only at the memcpy sites; keep the ring state in the same five fields so C inline functions can operate on it.
- Prefer fuzzing a Rust implementation against a tiny C driver (the reference runners are the model for that driver).

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R-A01, R-A02, R-A03, R-A04 | `darray/01-growth` |
| R-A05, R-A06, R-A14 | `darray/02-resize-and-new`, `04` |
| R-A07 | `darray/03-insert-erase`, `04-find-copy-move` |
| R-A08, R-A09 | `darray/03-insert-erase` |
| R-A10, R-A11 | `darray/04-find-copy-move` |
| R-A12, R-A13 | not covered (unit tests in the crate; join/split/move_item/swap) |
| R-D01…R-D05, R-D08, R-D09 | `deque/01-push-pop`, `02-wraparound` |
| R-D06, R-D07 | `deque/03-push-front` |
| R-D10, R-D11 | `deque/04-zero-place-upsize` |
| R-D02, R-D03, R-D04 | `deque/05-growth` |
| R-D12 | not covered (trivial; unit test) |

CORE: all scripts. Q-A1…Q-A3, Q-D1 have no QUIRK vectors.

## 11. Open questions

- None for the behaviours above. The `ENABLE_DARRAY_TYPE_TEST` macro variant and the remaining
  `da_*` convenience macros (`da_insert`, `da_erase`, `da_push_back_array`, …) are pure
  forwarding wrappers of the functions listed in section 3.

## 12. Round 9: full inline surface of `darray.h` / `deque.h` (added)

### Reference files (all cited)

- `libobs/util/darray.h`, `libobs/util/deque.h`

- **S-005-R-E01** `darray.h` — the complete inline surface over the struct pinned in section 2: lifecycle (`darray_init`, `darray_free`, `darray_end`), capacity (`darray_reserve`, `darray_ensure_capacity`, `darray_resize`, `darray_copy`, `darray_copy_array`, `darray_move`), mutation/lookup (`darray_push_back`, `darray_push_back_new`, `darray_push_back_array`, `darray_insert`, `darray_erase`, `darray_find`). Each is a thin wrapper applying the capacity/ensure rules of section 3 (R-A series) to typed element size.
- **S-005-R-E02** The `da_*` typed convenience macros forward to the inlines of R-E01; semantics are identical, element type supplied by the macro.
- **S-005-R-E03** `deque.h` — the byte ring-buffer inline surface: `deque_free`, `deque_reserve`, `deque_ensure_capacity`, `deque_upsize` (realloc + `deque_reorder_data` to fix the wrap point), `deque_place` (internal cursor management).
- **S-005-R-E04** Push/pop inlines: `deque_push_back`/`deque_push_front` (and the `_zero` variants), `deque_peek_front`/`deque_peek_back`, `deque_pop_front`/`deque_pop_back` (plus their array forms copying N bytes) — wrap-around handling per the rules pinned in the earlier sections.
