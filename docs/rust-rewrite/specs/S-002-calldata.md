# S-002 `calldata` (typed-less parameter stack)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `callback/calldata.c`, `calldata.h` |
| Target crate | `crates/obs-signal` (module `calldata`) |
| Depends on | S-001 |
| Conformance | `test/conformance/scripts/calldata/` (12 scripts) |

## 1. Scope

`calldata` is the parameter block passed to signal callbacks and procedure handlers
(S-004): an ordered collection of named values stored in one contiguous byte stack. It is
**binary-visible ABI**: the typed accessors (`calldata_get_int`, `calldata_set_string`,
…) are `static inline` functions compiled into plugins; they call the two exported
functions `calldata_get_data` / `calldata_set_data` and `calldata_get_string`, and also
touch the struct fields directly. A Rust implementation provides those three exported
functions and MUST keep the struct layout and the stack byte format exactly.

## 2. Conventions

`W` = `sizeof(size_t)`; vectors are for W = 8, little-endian. All `size_t` fields inside
the stack are stored in **native byte order, unaligned** (they are read and written with
`memcpy`; never cast the stack pointer to `*const usize`).

## 3. Interface

```c
struct calldata {
    uint8_t *stack;      /* offset 0  */
    size_t   size;       /* offset W   : number of used bytes in stack, including the terminator */
    size_t   capacity;   /* offset 2W  : allocated bytes */
    bool     fixed;      /* offset 3W  : caller-provided buffer, never reallocated */
};                       /* sizeof = 4W (padding after the bool) */

typedef struct calldata calldata_t;

bool calldata_get_data(const calldata_t *data, const char *name, void *out, size_t size);
void calldata_set_data(calldata_t *data, const char *name, const void *in, size_t size);
bool calldata_get_string(const calldata_t *data, const char *name, const char **str);
```

Header-inline API (stays in the C header; documented because Rust callers need the same
behaviour and the stack must be consistent with it):

| Function | Behaviour |
|---|---|
| `calldata_init(d)` | `memset(d, 0, sizeof *d)` |
| `calldata_init_fixed(d, buf, n)` | `d->stack = buf; d->capacity = n; d->fixed = true; d->size = 0; calldata_clear(d)` |
| `calldata_clear(d)` | if `d->stack != NULL`: `d->size = W` and the first `W` bytes of the stack are set to 0 (an empty stack = one zero terminator). If `stack == NULL`: nothing happens |
| `calldata_free(d)` | if not fixed: `bfree(d->stack)`. **Does not reset the fields.** |
| `calldata_create()` / `calldata_destroy(cd)` | `bzalloc(sizeof)` / `calldata_free` then `bfree(cd)` |
| `calldata_get_int(d, name, &v)` | `calldata_get_data(d, name, v, 8)` (`long long`) |
| `calldata_get_float` | size 8 (`double`) |
| `calldata_get_bool` | size 1 (`bool`) |
| `calldata_get_ptr(d, name, &p)` | size `sizeof(void*)` |
| `calldata_set_int/float/bool/ptr` | `calldata_set_data(d, name, &v, sizeof v)` |
| `calldata_set_string(d, name, s)` | `s != NULL`: `set_data(…, s, strlen(s)+1)`; `s == NULL`: `set_data(…, NULL, 0)` |
| `calldata_int/float/bool/ptr/string(d, name)` | the corresponding `get_*` with a zero (NULL) default when the read fails |

## 4. Data format

The stack is a sequence of **records** followed by a **terminator**:

```
record     := name_size : size_t        (> 0; counts the NUL)
              name      : name_size bytes (the name, including its NUL terminator)
              data_size : size_t        (may be 0)
              data      : data_size bytes
terminator := size_t 0
```

- `size` (the field) = total bytes from the start of the stack through the terminator,
  i.e. `Σ(W + name_size + W + data_size) + W`.
- An empty calldata that has an allocation has `size == W` and a zero terminator; one
  that has never been written has `stack == NULL` and `size == 0`.
- **There is no type tag.** The "type" of a value is only its byte count.
- Strings are stored with their NUL (`data_size = strlen + 1`); the empty string has
  `data_size = 1`; a NULL string has `data_size = 0`.

Worked example (W = 8) — `calldata_init; set_int("a", 1)` gives 34 bytes (vector
`calldata/01-basic`, first dump):

```
02 00 00 00 00 00 00 00      name_size = 2
61 00                        "a\0"
08 00 00 00 00 00 00 00      data_size = 8
01 00 00 00 00 00 00 00      value 1 (little-endian i64)
00 00 00 00 00 00 00 00      terminator
```

## 5. Behaviour

### Reads

- **S-002-R01** `calldata_get_data(d, name, out, size)` returns `false` if `d` is NULL, `name` is NULL, or `*name == 0`. It also returns `false` if `d->size == 0` (even if `stack` is non-NULL).
- **S-002-R02** Otherwise it searches records from the start for the **first** record whose name equals `name` (byte-wise `strcmp`, case-sensitive). If none exists → `false`.
- **S-002-R03** If found, it returns `true` and copies the payload to `out` **only if `data_size == size`** exactly; otherwise it returns `false` and does not write to `out`. Consequently `get_int`, `get_float` and `get_ptr` (on W = 8) succeed on each other's values, and `get_bool` only on 1-byte payloads. `size == 0` succeeds only on a zero-length payload (copying nothing).
- **S-002-R04** `calldata_get_string(d, name, &s)`: same argument checks and search as R01–R02, then returns `true` for **any** record regardless of size, storing in `*s` a pointer *into the stack* at the payload start, or `NULL` if `data_size == 0`. It does not verify that a NUL terminator exists inside the payload.
- **S-002-R05** The pointer returned by `get_string` is valid until the calldata is modified or freed.

### Writes

- **S-002-R10** `calldata_set_data(d, name, in, size)` does nothing if `d` is NULL, `name` is NULL or `*name == 0`.
- **S-002-R11** *First write to a growable calldata.* If `!d->fixed && d->stack == NULL`: allocate a stack of `max(128, 3W + name_len + size)` bytes where `name_len = strlen(name) + 1`, write the first record and the terminator, set `d->size = 3W + name_len + size`, `d->capacity` = the allocation.
- **S-002-R12** *Existing name* (found by R02's search): the payload is replaced. If the new size is larger by `k` bytes: ensure capacity for `d->size + k` (R14), shift everything after the old payload up by `k`, `d->size += k`. If smaller by `k`: shift the tail down by `k`, `d->size -= k`. Then write the new `data_size` and payload in place. Record **order is preserved**; the name is not rewritten.
- **S-002-R13** *New name*: append at the end (before the terminator): ensure capacity for `d->size + (name_len + size + 2W)`, write `name_size`, name, `data_size`, payload, new terminator; `d->size += name_len + size + 2W`.
- **S-002-R14** *Capacity rule* (`ensure(new_size)`): if `new_size < d->capacity` → ok (note the **strict** `<`: reaching exactly `capacity` triggers growth/failure). Else if `d->fixed` → log `LOG_ERROR "Tried to go above fixed calldata stack size!"` (text MUST) and the write is abandoned: **no change at all** to the calldata. Else `new_capacity = max(2 * capacity, new_size)`, reallocate (contents preserved).
- **S-002-R15** When writing `size == 0` with `in == NULL` no bytes are read from `in`; a zero-length payload is stored (this is how NULL strings are represented).
- **S-002-R16** For a fixed calldata the very first write takes the "existing stack" path (R12/R13), because `init_fixed` already cleared it to size `W`; R11 applies only to growable calldata with `stack == NULL`. (A fixed calldata with a NULL buffer is a misuse and undefined.)

### Interaction rules

- **S-002-R20** Names are unique per calldata (a write with an existing name replaces, R12). Case differs → different names.
- **S-002-R21** After `calldata_clear`, all previous parameters are gone; capacity is unchanged; the next `set_data` appends at the terminator (it goes through R13, not R11).
- **S-002-R22** `calldata_free` on a growable calldata releases the stack but leaves the fields stale; Rust wrappers MUST reset the struct themselves after freeing if they reuse it.

## 6. Algorithms

Search (shared by get and set; precondition `d->size != 0`):

```
pos = stack
loop:
  name_size = read_size(pos); pos += W
  if name_size == 0: return NOT_FOUND(pos - W)      # pos at the terminator
  if strcmp(pos, name) == 0: return FOUND(pos + name_size)   # pos at the data_size field
  pos += name_size
  data_size = read_size(pos); pos += W + data_size
```

Set (existing record, `pos` at the record's `data_size` field, `cur = read_size(pos)`):

```
if cur < size:  k = size - cur; ensure(d->size + k); move [pos .. end) up by k; d->size += k
elif cur > size: k = cur - size; move [pos + k .. end) down to pos; d->size -= k
write_size(pos, size); copy payload (if size != 0)
```

(The reference's move lengths are derived from `d->size`; any correct block move gives the same bytes.)

## 7. Concurrency

None. A calldata is not thread-safe; signal emission (S-004) passes one block to all callbacks sequentially.

## 8. Reference defects and quirks

- **Q-1 (type by size).** A read succeeds whenever the stored byte count equals the requested size, even if the value was written as a different type (vector `calldata/02`). Required: reproduce.
- **Q-2 (`calldata_free` does not clear).** See R22.
- **Q-3 (`get_string` on non-strings).** Succeeds on any record and returns a pointer to whatever bytes are there (vector `02`: `get_string i ok ""`). Required: reproduce (return pointer to payload start).
- **Q-4 (strict capacity check).** `new_size == capacity` is treated as overflow (fixed) or grows (growable). Vector `12-fixed-boundary`. Required: reproduce.
- **Q-5 (growth leaves unwritten bytes).** Newly allocated memory is not zeroed; bytes beyond `size` are unspecified. Vectors dump only `[0, size)`.

## 9. Implementation notes (non-normative)

- Implement the three exported functions on a `#[repr(C)] struct Calldata { stack: *mut u8, size: usize, capacity: usize, fixed: bool }`.
- Use `ptr::copy`/`copy_nonoverlapping` and `read_unaligned`/`write_unaligned` for `size_t` fields.
- Allocate with the S-001 allocator (`bmalloc`/`brealloc`), because C code frees the stack with `bfree` and vice-versa.
- A safe Rust-side wrapper (`CallData`) can offer typed accessors but must produce the same bytes.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01–R03 | `01-basic`, `02-type-is-only-size`, `05-invalid-names-and-empty-stack`, `09-raw-data` |
| R04, R05 | `01-basic`, `04-null-and-empty-strings`, `11-long-names-and-binary-strings` |
| R10 | `05-invalid-names-and-empty-stack` |
| R11 | `01-basic`, `06-capacity-growth` (capacity), `11` |
| R12 | `03-overwrite-resize`, `10-order-and-duplicates`, `09-raw-data` |
| R13, R20 | `01-basic`, `10-order-and-duplicates`, `11` |
| R14 | `06-capacity-growth`, `07-fixed-stack`, `12-fixed-boundary` |
| R15 | `04-null-and-empty-strings`, `09-raw-data` |
| R16, R21 | `07-fixed-stack`, `08-clear-and-reuse` |
| R22 | not scriptable (unit test in the crate) |

CORE: all scripts. QUIRK vectors are embedded in `02` and `12`.

## 11. Open questions

None. 32-bit (`W = 4`) layouts follow from the formulas in section 4; vectors for W = 4 must be generated from a 32-bit 

## 12. Round 9: `calldata.c` uncited functions (added)

### Reference files (all cited)

- `libobs/callback/calldata.c`

- **S-002-R23** `calldata_get_data`/`calldata_set_data`: the typed accessors over the param array (get: type check with the default 0/empty-string on mismatch per the earlier sections; set: replaces in place, string params are copied).
- **S-002-R24** `calldata_get_string`/`cd_getparam`: raw param access without type checking — used by proc handlers; `cd_getparam` returns the param at the handler's current cursor.
- **S-002-R25** Copy/serialize internals: `cd_copy_string` (duplicate a param string), `cd_copy_data` (deep copy of the whole calldata — every param, strings duplicated), `cd_ensure_capacity`/`cd_set_first_param` (capacity/cursor management), `cd_serialize_size`/`cd_serialize_string` (the calldata half of the binary serialization format pinned in S-010: size computation, then byte writing).build of the reference.
