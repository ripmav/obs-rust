# S-010 Serializers, bit reader and text lookup

| | |
|---|---|
| Status | Ready |
| Spec version | working tree on `docs/rust-rewrite-plan` (uncommitted) |
| Reference | C implementation, libobs 33.0.1 (`util/serializer.h`, `array-serializer.*`, `file-serializer.*`, `buffered-file-serializer.*`, `bitstream.*`, `text-lookup.*`) |
| Target crate | `crates/obs-util` (modules `serializer`, `bitstream`, `text_lookup`) |
| Depends on | S-001 (allocation, logging), S-005 (deque is internal only) |
| Conformance | `test/conformance/scripts/serial/` (runner described in `test/conformance/README.md`, "serial runner") |

## 1. Scope

Three small facilities used by libobs, the frontend and plugins:

1. **Serializer**: a callback table (`struct serializer`) with read / write / seek / get_pos,
   plus header-inline helpers that plugins compile in (`s_w8`, `s_wl32`, `serialize`, ...).
   Four implementations: in-memory growable array, file input, file output (optionally
   "safe" via temp file), and an asynchronous buffered file writer.
2. **Bit reader** (`bitstream_reader_*`): MSB-first reader over a byte slice.
3. **Text lookup** (`text_lookup_*`): loads `Key="Value"` locale files (`.ini` style) into a
   case-sensitive string table.

Absent from this version and MUST NOT be invented: read helpers (`s_r*`, `s_rb*`, `s_rl*`),
exp-Golomb readers, bit *writers*, `text_lookup_add_sub`, `$1`-style substitution in lookup
values (substitution lives elsewhere), a locale fallback chain (done by callers with several
`text_lookup_add` calls).

## 2. Terminology and conventions

RFC 2119 words. "LE"/"BE" = little/big endian. Integer arguments are truncated to the
parameter type of the helper (C implicit conversion). `pos` = the position a serializer would
write to next. Sizes are `size_t` (W = 8); positions and seek results are `int64_t`.
Quirk classes: CORE / QUIRK (SHOULD match) / DEFECT (required behaviour stated).

## 3. Interface

### 3.1 `struct serializer` (W = 8)

| Offset | Field | Type |
|---|---|---|
| 0 | `data` | `void *` (opaque, passed first to every callback) |
| 8 | `read` | `size_t (*)(void *data, void *buf, size_t size)` |
| 16 | `write` | `size_t (*)(void *data, const void *buf, size_t size)` |
| 24 | `seek` | `int64_t (*)(void *data, int64_t offset, enum serialize_seek_type)` |
| 32 | `get_pos` | `int64_t (*)(void *data)` |

`enum serialize_seek_type { SERIALIZE_SEEK_START = 0, SERIALIZE_SEEK_CURRENT = 1, SERIALIZE_SEEK_END = 2 }`.
A callback may be NULL ("not supported").
A Rust port keeps this layout `#[repr(C)]` if plugins are to call it; internally a trait
(`read`, `write`, `seek`, `pos`, each defaulting to "unsupported") is acceptable provided the
helper behaviour below is kept.

### 3.2 Header-inline helpers

All take `struct serializer *s` which MAY be NULL.

| Helper | Behaviour |
|---|---|
| `size_t s_read(s, buf, size)` | see R01 |
| `size_t s_write(s, buf, size)` | see R02 |
| `size_t serialize(s, buf, len)` | see R03 |
| `int64_t serializer_seek(s, offset, type)` | see R04 |
| `int64_t serializer_get_pos(s)` | see R05 |
| `void s_w8(s, u8)` | one byte |
| `s_wl16 s_wl24 s_wl32 s_wl64`, `s_wlf`, `s_wld` | LE integers (24-bit takes a `u32` and emits its low 3 bytes), `float`, `double` |
| `s_wb16 s_wb24 s_wb32 s_wb64`, `s_wbf`, `s_wbd` | BE counterparts |

Each multi-byte helper is composed of one-byte `s_w8` calls (see R06), so a serializer sees
up to 8 separate 1-byte writes per helper call; their order is observable.

### 3.3 Implementations (exported functions)

```
array_output_serializer_init(s, data)   array_output_serializer_free(data)   array_output_serializer_reset(data)
file_input_serializer_init(s, path) -> bool        file_input_serializer_free(s)
file_output_serializer_init(s, path) -> bool       file_output_serializer_init_safe(s, path, temp_ext) -> bool
file_output_serializer_free(s)
buffered_file_serializer_init_defaults(s, path) -> bool
buffered_file_serializer_init(s, path, max_bufsize, chunk_size) -> bool     buffered_file_serializer_free(s)
bitstream_reader_init(r, data, len)   bitstream_reader_read_bits(r, bits) -> u8
bitstream_reader_r8(r) -> u8          bitstream_reader_r16(r) -> u16
bitstream_reader_read_bit(r) -> u8    (linkable but NOT declared in the header)
text_lookup_create(path) -> lookup*   text_lookup_add(lookup, path) -> bool
text_lookup_destroy(lookup)           text_lookup_getstr(lookup, key, &out) -> bool
```

`struct array_output_data { DARRAY(u8) bytes /* array ptr, num, capacity */; size_t cur_pos; }`
(layout: `bytes.array` @0, `bytes.num` @8, `bytes.capacity` @16, `cur_pos` @24).
`struct bitstream_reader { u8 pos @0; u8 subPos @1; u8 *buf @8; size_t len @16; }`.
`lookup_t` is opaque.

## 4. Data formats

### 4.1 Worked byte examples (vector `helpers_endian`, `helpers_floats`)

| Call | Bytes written |
|---|---|
| `s_wl16(0x1234)` | `34 12` |
| `s_wb16(0x1234)` | `12 34` |
| `s_wl24(0xaa123456)` | `56 34 12` (top byte dropped) |
| `s_wb24(0xaa123456)` | `12 34 56` |
| `s_wl32(0x01020304)` / `s_wb32` | `04 03 02 01` / `01 02 03 04` |
| `s_wl64(0x0102030405060708)` / `s_wb64` | `08 07 .. 01` / `01 02 .. 08` |
| `s_wlf(1.5f)` / `s_wbf` | `00 00 c0 3f` / `3f c0 00 00` |
| `s_wld(-2.0)` / `s_wbd` | `00 00 00 00 00 00 00 c0` / `c0 00 .. 00` |

Floats are written as their IEEE-754 bit pattern byte-for-byte (NaN payloads and -0 are
preserved; no canonicalisation).

### 4.2 Text lookup file

UTF-8 text (optional BOM `EF BB BF`), LF or CRLF, lines `Name="Value"`; see 5.6.

## 5. Behaviour (numbered rules)

### 5.1 Serializer helpers

- **S-010-R01** `s_read(s, buf, size)`: if `s` non-NULL, `s->read` non-NULL, `buf` non-NULL and
  `size != 0`, return `s->read(s->data, buf, size)` unchanged (no looping on short reads);
  otherwise 0 without calling anything.
- **S-010-R02** `s_write(s, buf, size)`: same guards with `s->write`. A zero size or NULL buffer
  returns 0 and calls nothing.
- **S-010-R03** `serialize(s, buf, len)`: NULL `s` -> 0. If `s->write` exists call it (even when
  `read` also exists); else if `s->read` exists call it; else 0. No buffer/size guards: the
  callback sees exactly the arguments. (QUIRK Q-2: on a serializer with both callbacks, or with
  only `write`, "reading" through `serialize` writes.)
- **S-010-R04** `serializer_seek`: NULL `s` or NULL `seek` -> -1, else the callback's result.
- **S-010-R05** `serializer_get_pos`: NULL `s` or NULL `get_pos` -> -1, else the callback's result.
- **S-010-R06** `s_w8(s, v)` = `s_write(s, &v, 1)`; the result is ignored by all `s_w*` helpers
  (void). Decompositions, each step a separate 1-byte write in this order:
  `wl16(v)`: `v`, `v>>8`. `wl24(v)`: `v`, then `wl16(v>>8)`. `wl32(v)`: `wl16(v)`, `wl16(v>>16)`.
  `wl64(v)`: `wl32(v)`, `wl32(v>>32)`. `wb16(v)`: `v>>8`, `v`. `wb24(v)`: `wb16(v>>8)`, `v`.
  `wb32(v)`: `wb16(v>>16)`, `wb16(v)`. `wb64(v)`: `wb32(v>>32)`, `wb32(v)`.
  Every written byte is truncated to 8 bits.
- **S-010-R07** `s_wlf/s_wbf` = `s_wl32/s_wb32` of the float's bit pattern; `s_wld/s_wbd` likewise with
  the double's 64-bit pattern.
- **S-010-R08** Results of helpers on a NULL serializer or one without callbacks: nothing happens,
  `s_read/s_write/serialize` return 0, `seek`/`get_pos` return -1.

### 5.2 Array serializer

State: byte vector `bytes` (length `num`) and `cur_pos` (<= num by construction).

- **S-010-R10** `init` zeroes both structures, installs `write`, `seek`, `get_pos`; **no `read`**.
  The vector is empty, `cur_pos = 0`.
- **S-010-R11** `write(size)` when `cur_pos < num`: overwrite in place; if `cur_pos+size > num` the
  vector grows (length becomes `cur_pos+size`); `cur_pos += size`. When `cur_pos >= num`: append
  and `cur_pos += size`. Always returns `size`. Capacity growth policy is not observable and
  unspecified.
- **S-010-R12** `seek`: target = `offset` (START), `cur_pos + offset` (CURRENT), `num - offset`
  (END, i.e. the offset counts **backwards from the end**). Arithmetic is unsigned 64-bit; a
  target `> num` (including any negative target, via wrap-around) returns -1 and changes
  nothing. Otherwise `cur_pos = target` and the target is returned. Seeking to exactly `num`
  is legal. Seeking never changes the data.
- **S-010-R13** `get_pos` returns `num` (the data length), NOT `cur_pos` (QUIRK Q-1).
- **S-010-R14** `reset`: length 0, `cur_pos = 0`, storage kept. `free`: releases storage; the
  structure is empty (num 0) afterwards.

### 5.3 File input serializer

- **S-010-R20** `init(s, path)`: opens `path` for binary reading. Failure (missing, unreadable)
  returns false and leaves the serializer untouched. Success installs `read`, `seek`, `get_pos`
  and sets `write = NULL`.
- **S-010-R21** `read` returns the number of bytes actually read (C `fread` of 1-byte items):
  short counts at end of file, 0 at/after the end.
- **S-010-R22** `seek` uses START/CURRENT/END with ordinary file semantics (END: offset **added** to
  the file size, so `-2` is two bytes before the end; positions beyond the end are allowed).
  If the underlying seek fails (e.g. resulting position negative) return -1 and the position
  is unchanged; else return the new absolute position. `get_pos` returns the absolute position.
- **S-010-R23** `free`: closes the file if open. The serializer keeps stale pointers (not reusable).

### 5.4 File output serializer

- **S-010-R30** `init(s, path)`: opens `path` for binary writing, **creating or truncating** it. Failure
  (e.g. missing directory) returns false. Installs `write`, `seek`, `get_pos`; `read = NULL`.
- **S-010-R31** `write` returns the number of bytes written (normally `size`). Seek and positions follow R22
  (END adds the offset to the current file size; holes created by seeking beyond the end and
  writing read back as zero bytes). `get_pos` is the logical position including buffered data.
- **S-010-R32** File content is guaranteed visible only after `free` (stdio buffering): implementations
  MUST flush/close in `free`; scripts never inspect an open file.
- **S-010-R33** `init_safe(s, path, ext)`: NULL or empty `ext` -> false. Temporary name = `path` +
  (`.` unless `ext` starts with `.`) + `ext`; it is opened like R30 (truncating), the target path
  is **not touched** until `free`. Open failure -> false.
- **S-010-R34** `free` on a safe serializer: close the temp file, delete `path` (ignore errors), rename
  temp to `path` (ignore errors). Afterwards the temp name no longer exists and `path` holds the
  temp file's content (an empty file if nothing was written). On a plain serializer: close only.

### 5.5 Buffered file serializer

Observable model: the serializer records a sequence of *writes* `(offset, bytes)`; `free`
guarantees that all recorded writes have been applied **in call order** to the (initially
truncated) file, which is then closed. Offsets come from the logical position `next_pos`.

- **S-010-R40** `init_defaults(s,path)` = `init(s,path,0,0)`. `init`: opens `path` "wb" (create/truncate);
  failure -> false (nothing leaks). `max_bufsize` 0 -> 256 MiB, `chunk_size` 0 -> 1 MiB.
  Installs `write`, `seek`, `get_pos`; **no `read`**. Starts a background writer; `next_pos = 0`.
- **S-010-R41** `write(size)`: size 0 -> 0. Otherwise the data is recorded in pieces of at most
  `chunk_size`, each tagged with the current `next_pos`, and `next_pos += piece`. Returns `size` on
  success, 0 if the writer thread has failed (earlier I/O error). May block while the internal queue is
  full ("back pressure"); then logs `LOG_DEBUG` "Waiting for I/O thread..." (text informational,
  occurrence timing dependent, hence not in vectors).
- **S-010-R42** `seek`: if the writer has failed return -1. START: `next_pos = offset`. CURRENT:
  `next_pos += offset`. END: `next_pos -= offset` (offset counts **backwards from the current
  write position**, not from the file size; QUIRK Q-9). Unsigned 64-bit arithmetic, no range check.
  Returns `next_pos` as `int64` (a negative target thus returns a negative value, e.g. -1).
  A seek by itself never changes the file; seeking beyond the end and writing zero-fills the
  gap; seeking back overwrites.
- **S-010-R43** `get_pos` returns `next_pos` (or -1 after a writer failure). It is the logical position,
  unrelated to bytes already flushed.
- **S-010-R44** `free`: requests shutdown, waits until every queued write has reached the file, closes the
  file, releases all memory. It logs exactly one `LOG_DEBUG` "Final buffer capacity: N KiB" per
  free of an initialised serializer (MUST: one debug-level line; the text/number is informational).
- **S-010-R45** Writer failure (short `fwrite`): logs `LOG_ERROR` "Error writing to '<file>': ..." once,
  the writer stops; subsequent `write` return 0, `seek`/`get_pos` return -1.
- **S-010-R46** Flushing policy (informational): the writer batches into one chunk buffer and writes it
  when it reaches `chunk_size`, when the position becomes discontinuous, when at least 64 KiB are
  pending, or at shutdown; smaller pending data may stay unwritten until `free`.
  Whether data is on disk before `free` is not specified.
- **S-010-R47** Preconditions: `chunk_size + 16` must not exceed the queue capacity,
  `max(1 MiB, max_bufsize)`, otherwise a write can wait forever (reference deadlock; callers MUST
  NOT do this). `max_bufsize` values below 1 MiB have no effect because the queue starts at 1 MiB.
  The 16 bytes are the per-piece header (two u64: offset, length).

### 5.6 Bit reader

- **S-010-R50** `init(r, data, len)`: zeroes the struct; `buf = data`, `len = len`, `pos = 0`, `subPos = 0x80`.
- **S-010-R51** `read_bit`: if `pos >= len` return 0 and change nothing. Else the bit is
  `(buf[pos] & subPos) != 0`; then `subPos >>= 1`; if it became 0, `subPos = 0x80` and `pos += 1`.
  (`pos` is an unsigned 8-bit field: see Q-3.)
- **S-010-R52** `read_bits(r, n)`: starts with an 8-bit accumulator 0, repeats `n` times
  `acc = (acc << 1) | read_bit()` (8-bit arithmetic) and returns `acc`. `n <= 0` reads nothing and
  returns 0. For `n > 8` all `n` bits are consumed but only the last 8 are retained (Q-4). Bits
  past the end read as 0 and are not consumed.
- **S-010-R53** `r8` = `read_bits(8)`. `r16` = `(read_bits(8) << 8) | read_bits(8)` (big endian, the
  two reads happen in that order). Neither fails: past the end they return 0 (partial values keep
  the already-read leading bytes: `r16` with one byte left returns `byte << 8`).
- **S-010-R54** The reader never reads outside `buf[0..len)`; `len` may be smaller than the buffer; `len = 0`
  is valid.

### 5.7 Text lookup

- **S-010-R60** `create(path)`: new empty table, then `add(path)`; if `add` fails, the table is freed and
  NULL is returned. A file that contains entries or only blank/comment lines succeeds.
- **S-010-R61** `add(lookup, path)`: `lookup` MUST be non-NULL (reference dereferences it). Returns false if the
  file cannot be opened, is empty, or contains only a BOM (the table is unchanged). Otherwise the
  file is processed per 6.3 and true is returned (even if zero entries were found). Existing keys
  are replaced by later definitions (**last wins**, also within one file).
- **S-010-R62** `getstr(lookup, key, &out)`: NULL `lookup` -> false. Otherwise exact, case-sensitive byte
  comparison of `key` (non-NULL) with stored keys; hit -> `*out` = stored value (valid until
  `destroy` or until a later `add` replaces that key) and true; miss -> false and `*out` untouched.
  The key may be any stored string including `" "` or `"="`.
- **S-010-R63** `destroy(NULL)` is a no-op; otherwise frees all entries.
- **S-010-R64** Preprocessing of file content: strip a leading UTF-8 BOM (3 bytes); if nothing remains, fail (R61);
  replace every CR by a space; the remaining text is parsed as in 6.3.
- **S-010-R65** Value unescaping (after the value token is cut): sequentially over the whole value, replace
  every `\n` (backslash, `n`) by LF, then every `\t` by TAB, then every `\r` by CR, then every `\"`
  by `"`. Each pass is a left-to-right non-overlapping scan of the result of the previous pass.
  Other backslash sequences (including `\\`) stay unchanged; `\\n` therefore becomes backslash + LF
  (the first pass sees `\n` at the second backslash).
- **S-010-R66** Keys are stored without unescaping. There is no `$` processing and no inheritance.
- **S-010-R67** No concurrency guarantees; a table is not thread-safe for concurrent add/get.

## 6. Algorithms

### 6.1 Base tokens

Character classes: whitespace = space, TAB, LF (CR was replaced by space); digit = `0-9`;
alpha = ASCII letters; other = everything else (including every byte >= 0x80, `_`, `.`, `-`, `"`, `=`, `#`).
`base_token(p)`: skip nothing; take the char at `p`; if it is digit or alpha the token is the
maximal run of the same class; otherwise it is that single char. No token at end of text.

### 6.2 `gettoken()` (returns a slice, may be "empty" = failure)

```
result = none
loop:
  t = base_token(); if none: break
  if result is none:
      if t starts '#':  skip forward to (not including) the next LF or end;  continue
      if t is whitespace: result = t; break           # a one-character token (" ", TAB or LF)
      result = t
      if t starts '"':  extend as string (below); break
      if t starts '=':  break                          # the token "="
      # otherwise it is the start of a word: keep looping
  else:                                                # extending a word
      if t is whitespace or t starts '=':  push t back; break
      if t starts '#':  push back one char; break
      result.len += t.len                              # words are contiguous in the text
return result.len != 0
```

String extension, starting right after the opening quote: scan forward; `escaped=false`; stop at NUL or LF
(not consumed); on `\` when not escaped set `escaped`; otherwise clear `escaped`; on `"` when not
escaped consume it and stop. Let `raw` = text from the opening quote to the stop point (the
opening quote included, a consumed closing quote included). Token content = `raw` without its first
char, and without its last char if that char is `"` (closing quote; or an escaped quote at
the end of an unterminated line, e.g. `"abc\"` -> content `abc\`).
Consequences: `""` has content length 0 and `gettoken` returns **failure** (Q-7). A raw token of
exactly `"` (opening quote directly followed by LF or end of text) has computed length -1 (D-1).

### 6.3 File parsing

```
while gettoken -> name:
    if name.first == LF: continue                       # blank line / end of previous line
    got_eq = false
    repeat:
        if gettoken fails: stop parsing (name discarded)
        if value.first == LF: go to next while-iteration  # name without value: discarded
        if value.first == '=' and not got_eq: got_eq = true; repeat   # skip the first separator
    store name -> unescape(value)    (replace an existing entry)
    skip tokens until an LF token has been read; if end of text reached first: stop parsing
```

`first` is the first character of the token content (after quote stripping). Anything after the value
on the same line is ignored (including further `Name=Value` pairs). Consequences, all QUIRKs
(vectors `textlookup_quirk_*`): whitespace tokens are returned as names/values (Q-5); a
second `=` becomes the value (Q-6); `""` stops parsing (Q-7).

A lone-quote token (D-1): content length -1; its `first` char is the char following it (LF or end of text).
As a name followed by LF it is treated like a blank line; as a value followed by LF the name is
discarded and parsing continues; at end of text it behaves like the empty token (stop).

## 7. Concurrency

Serializers are single-owner objects; callbacks run on the caller's thread. The buffered
serializer owns a background writer thread: `write`, `seek`, `get_pos` and `free` MUST be called
from one thread at a time (internally mutex + two events synchronise producer and writer; the writer
error flag is atomic). `free` joins the writer; after `free` the serializer MUST NOT be used.
Bit reader and text lookup have no internal synchronisation; a populated table may be read
concurrently by several threads when nobody calls `add`.

## 8. Reference defects and quirks

| ID | Class | Description | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | Array serializer `get_pos` returns the length, not the position set by seek. | SHOULD match (callers call `get_pos` only when appending). | `array_quirk_getpos` |
| Q-2 | QUIRK | `serialize` prefers `write`; on the array serializer `serialize_r` writes the caller's buffer. | SHOULD match. | `array_quirk_serialize`, `helpers_callbacks` |
| Q-3 | QUIRK | `bitstream_reader.pos` is 8 bits: for `len > 256` reading wraps to byte 0 after byte 255 and never reaches the end; for `len = 256` it also wraps; `len = 255` ends correctly. | SHOULD match (reproduce with `pos: u8`, `wrapping_add`). | `bitstream_quirk_pos_wrap` |
| Q-4 | QUIRK | `read_bits(n > 8)` consumes n bits, returns last 8. | SHOULD match. | `bitstream_quirk_wide` |
| Q-5 | QUIRK | Whitespace is a token: `K = "v"` stores `K -> " "`; leading blanks make `" "` a name. | SHOULD match; real locale files never contain spaces around `=`. | `textlookup_quirk_whitespace` |
| Q-6 | QUIRK | `=` handling: name without value dropped; `A==b` stores `A -> "="`; `=x` stores `"=" -> "x"`. | SHOULD match. | `textlookup_quirk_equals` |
| Q-7 | QUIRK | `""` (empty quoted token) ends parsing silently; later lines are lost. | SHOULD match. | `textlookup_quirk_empty_string` |
| Q-8 | QUIRK | Three different SEEK_END conventions: array subtracts from the length, files add to the file size, buffered subtracts from the current position. | MUST match (CORE rules R12, R22, R42). | `array_seek`, `file_input`, `file_output`, `buffered_seek` |
| Q-9 | QUIRK | Buffered `seek` has no range check; a negative START returns a negative number; the next write then lands at an unspecified place (reference: the physical file position after the last flushed write because the failed seek is ignored). | Return value MUST match; behaviour of a later write is UNSPECIFIED (not in vectors). | `buffered_seek` (last case, no write) |
| D-1 | DEFECT | Value token that is a lone `"` as the last byte of the file (`B="` EOF) makes the reference compute length `SIZE_MAX`, call the allocator with size 0 (which aborts in some configurations or corrupts memory by writing one byte before the allocation). | MUST NOT crash or corrupt memory: treat like `B=""` (stop parsing, earlier entries kept, `create` succeeds). | `textlookup_defect_lone_quote` (`tl_defect`) |

Platform notes: on Windows `fopen`/seek use UTF-8 -> wide conversion and `_fseeki64`; unchanged
behaviour. `os_fread_utf8` also strips a BOM only (no UTF-16 handling).

## 9. Implementation notes (non-normative)

- Rust: a `trait Serializer { fn read(&mut self, &mut [u8]) -> usize; fn write(&mut self, &[u8]) -> usize; fn seek(&mut self, i64, SeekFrom3) -> i64; fn pos(&self) -> i64; }`
  with default `0` / `-1` bodies reproduces R01-R08; `Option<&mut dyn Serializer>` models the NULL pointer.
  Provide a `#[repr(C)]` vtable adapter if C plugins need to call `s_*`.
- Array serializer: `Vec<u8>` + `cur_pos: usize`. Keep `get_pos == len`.
- File serializers: wrap `std::fs::File`; use a small `BufWriter` for output but flush/close in `free`; seek must flush first.
  Negative resulting positions must yield -1 (Rust's `SeekFrom::Start` takes u64: check manually).
- Buffered writer: a `Vec<(u64, Vec<u8>)>` queue plus a writer thread, or simply a synchronous
  implementation that applies the same offset logic with a `BufWriter` (the observable contract is the file after `free`);
  keep the one-debug-log-per-free requirement.
- Text lookup: write the tokenizer exactly as 6.2, on bytes (not `char`s): keys and values may hold invalid UTF-8.
  Use `HashMap<Vec<u8>, Vec<u8>>`.
- Hex-printing for byte data in the runner: lowercase.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01, R02 | helpers_callbacks, helpers_null_empty, file_input, buffered_basic |
| R03 | helpers_callbacks, array_quirk_serialize, file_input, helpers_null_empty |
| R04, R05, R08 | helpers_callbacks, helpers_null_empty |
| R06 | helpers_endian, helpers_callbacks |
| R07 | helpers_floats |
| R10 | array_basic, array_reset, helpers_endian |
| R11 | array_basic |
| R12 | array_seek |
| R13 | array_quirk_getpos |
| R14 | array_reset |
| R20, R23 | file_input |
| R21, R22 | file_input |
| R30, R31 | file_output |
| R32 | file_output (content only read after close) |
| R33, R34 | file_output_safe |
| R40 | buffered_basic, buffered_seek |
| R41 | buffered_basic, buffered_large |
| R42, R43 | buffered_seek, buffered_basic |
| R44 | every buffered script (debug log line) |
| R45 | not testable without fault injection (see 11) |
| R46, R47 | buffered_large (informational / precondition) |
| R50, R51 | bitstream_basic |
| R52 | bitstream_basic, bitstream_quirk_wide |
| R53 | bitstream_basic, bitstream_eof |
| R54 | bitstream_eof |
| R60, R61 | textlookup_files, textlookup_basic |
| R62, R63 | textlookup_basic, textlookup_files |
| R64 | textlookup_tokens, textlookup_files |
| R65 | textlookup_escapes |
| R66 | textlookup_basic (keys with `.`/`-`), textlookup_tokens |
| 6.2 / 6.3 algorithm | textlookup_tokens, textlookup_unterminated, textlookup_quirk_* , textlookup_defect_lone_quote |
| Q-1..Q-9, D-1 | see section 8 |

## 11. Open questions

- R45 (writer I/O error path): cannot be reproduced portably in the suite (needs a full disk);
  behaviour taken from the source only.
- Buffered negative-seek write (Q-9) and writes after a failed `seek` in the file serializer are unspecified.
- Windows: `os_fopen` accepts UTF-8 paths only through conversion; not verified here.
- R47: the 1 MiB / `chunk_size` deadlock is a reference limitation, not exercised.
