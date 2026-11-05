# S-023 Platform layer and UTF-8 conversion

| | |
|---|---|
| Status | Draft |
| Spec version | initial |
| Reference | C implementation, libobs 33.0.1: `util/utf8.c`, `util/platform.c`, Linux half of `util/platform-nix.c` (file names for the spec author only) |
| Target crate | `crates/platform` (module `utf8`, module `fs`, module `path`, module `text`) |
| Depends on | S-001 (allocation, logging, crash handler), S-006 (`dstr` is only an implementation detail of the reference) |
| Conformance | `test/conformance/scripts/platform/` (38 scripts), runner section "platform runner" in `test/conformance/README.md` |

## 1. Scope

The operating-system abstraction every other component sits on: UTF-8 / wide-character conversion,
text-file helpers, file and directory primitives, directory listing and globbing, path helpers, locale
independent number formatting and parsing, the file-name template expander used for recordings, and a handful of
machine queries. Callers: `config-file` (S-007: `os_fread_utf8`, `os_dtostr`, `os_strtod`, `os_safe_replace`),
`obs-data` (S-014: `os_quick_*`, `os_rename`, `os_file_exists`), the serializers (S-010: `os_fopen`,
`os_fseeki64`, `os_ftelli64`, `os_unlink`, `os_rename`), `dstr` (S-006: wide/multibyte conversion), plugins
(everything in `platform.h`), and the front end (file names, config paths).

In scope: **Linux** behaviour of everything above. Out of scope (listed so nobody searches for them): sleep
inhibition (`os_inhibit_sleep_*`, desktop-bus integration), the Windows and macOS halves of the platform files,
`os_stat` (a macro for `stat` on POSIX), `os_dlopen` of plug-in metadata (`get_plugin_info`), the process
(`pipe`) helpers. Windows/macOS differences that are visible in the reference source are listed in section 9.

## 2. Terminology and conventions

RFC 2119 words apply. `wchar` is the reference's `wchar_t`: **32 bits, signed** on Linux (committed vectors).
"Code point" below means a `wchar` value; values are not limited to Unicode scalar values (section 5.1).
"Bytes" are 8 bit; text is not validated unless a rule says so. "The runner" is the conformance runner
described in the README; paths in rules are relative to the process's current directory unless absolute.
`rc` means the integer returned. POSIX call names (`rename`, `glob`, `realpath`, `strftime`, ...) mean the
POSIX/glibc behaviour of the C library; where this spec only says "as `rename`" an implementation MAY call the
OS function directly, and the spec gives the reference-visible consequences.
All functions are thread-safe in the sense that they keep no mutable state of their own, except where §7 says
otherwise.

## 3. Interface

Names are the C names; a Rust crate exposes safe equivalents plus, where an ABI is required, `extern "C"`
functions with exactly these names and types (`size_t`, `int64_t`, `bool`, `FILE *`, `char *` ...).

### 3.1 utf8

```
#define UTF8_IGNORE_ERROR 0x01
#define UTF8_SKIP_BOM     0x02
size_t utf8_to_wchar(const char *in, size_t insize, wchar_t *out, size_t outsize, int flags);
size_t wchar_to_utf8(const wchar_t *in, size_t insize, char *out, size_t outsize, int flags);
```

### 3.2 conversions with termination (`platform.h`)

```
size_t os_mbs_to_wcs (const char *str,    size_t len, wchar_t *dst, size_t dst_size);
size_t os_utf8_to_wcs(const char *str,    size_t len, wchar_t *dst, size_t dst_size);
size_t os_wcs_to_mbs (const wchar_t *str, size_t len, char *dst,    size_t dst_size);
size_t os_wcs_to_utf8(const wchar_t *str, size_t len, char *dst,    size_t dst_size);
size_t os_mbs_to_wcs_ptr (const char *str,    size_t len, wchar_t **pstr);
size_t os_utf8_to_wcs_ptr(const char *str,    size_t len, wchar_t **pstr);
size_t os_wcs_to_mbs_ptr (const wchar_t *str, size_t len, char **pstr);
size_t os_wcs_to_utf8_ptr(const wchar_t *str, size_t len, char **pstr);
size_t os_utf8_to_mbs_ptr(const char *str,    size_t len, char **pstr);
size_t os_mbs_to_utf8_ptr(const char *str,    size_t len, char **pstr);
double os_strtod(const char *str);
int    os_dtostr(double value, char *dst, size_t size);
```
`_ptr` results are allocated with the S-001 allocator (`bmalloc`) and freed by the caller with `bfree`.

### 3.3 files

```
FILE   *os_wfopen(const wchar_t *path, const char *mode);   FILE *os_fopen(const char *path, const char *mode);
int64_t os_fgetsize(FILE *file);
int     os_fseeki64(FILE *file, int64_t offset, int origin);  int64_t os_ftelli64(FILE *file);
size_t  os_fread_mbs(FILE *file, char **pstr);                size_t os_fread_utf8(FILE *file, char **pstr);
char   *os_quick_read_utf8_file(const char *path);            char *os_quick_read_mbs_file(const char *path);
bool    os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker);
bool    os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker,
                                      const char *temp_ext, const char *backup_ext);
bool    os_quick_write_mbs_file(const char *path, const char *str, size_t len);
int64_t os_get_file_size(const char *path);
bool    os_file_exists(const char *path);
int os_mkdir(const char *path);  int os_mkdirs(const char *path);   /* MKDIR_EXISTS 1, MKDIR_SUCCESS 0, MKDIR_ERROR -1 */
int os_rename(const char *old_path, const char *new_path);  int os_copyfile(const char *in, const char *out);
int os_unlink(const char *path); int os_rmdir(const char *path);
int os_safe_replace(const char *target, const char *from, const char *backup);
char *os_getcwd(char *path, size_t size);  int os_chdir(const char *path);
int64_t os_get_free_space(const char *path);  uint64_t os_get_free_disk_space(const char *dir);
```

### 3.4 directories, globbing, paths

```
struct os_dirent { char d_name[256]; bool directory; };           typedef struct os_dir os_dir_t;
os_dir_t *os_opendir(const char *path);  struct os_dirent *os_readdir(os_dir_t *dir);  void os_closedir(os_dir_t *dir);
struct os_globent { char *path; bool directory; };
struct os_glob_info { size_t gl_pathc; struct os_globent *gl_pathv; };   typedef struct os_glob_info os_glob_t;
int os_glob(const char *pattern, int flags, os_glob_t **pglob);  void os_globfree(os_glob_t *pglob);
const char *os_get_path_extension(const char *path);
size_t os_get_abs_path(const char *path, char *abspath, size_t size);   char *os_get_abs_path_ptr(const char *path);
int os_get_config_path(char *dst, size_t size, const char *name);       char *os_get_config_path_ptr(const char *name);
int os_get_program_data_path(char *dst, size_t size, const char *name); char *os_get_program_data_path_ptr(const char *name);
char *os_get_executable_path_ptr(const char *name);
char *os_generate_formatted_filename(const char *extension, bool space, const char *format);
```
Layout (x86-64): `struct os_dirent` is 256 bytes of name followed by one `bool` (size 257, align 1);
`struct os_globent` is `{ptr, bool}` (16 bytes); `struct os_glob_info` is `{size_t, ptr}` (16 bytes).

### 3.5 time and machine queries

```
uint64_t os_gettime_ns(void);  bool os_sleepto_ns(uint64_t t);  bool os_sleepto_ns_fast(uint64_t t);  void os_sleep_ms(uint32_t ms);
struct timespec *os_nstime_to_timespec(uint64_t timestamp, struct timespec *storage);
os_cpu_usage_info_t *os_cpu_usage_info_start(void); double os_cpu_usage_info_query(os_cpu_usage_info_t *); void os_cpu_usage_info_destroy(os_cpu_usage_info_t *);
int os_get_physical_cores(void); int os_get_logical_cores(void);
uint64_t os_get_sys_free_size(void); uint64_t os_get_sys_total_size(void);
struct os_proc_memory_usage { uint64_t resident_size; uint64_t virtual_size; };
bool os_get_proc_memory_usage(struct os_proc_memory_usage *); uint64_t os_get_proc_resident_size(void); uint64_t os_get_proc_virtual_size(void);
char *os_generate_uuid(void);                       /* UUID_STR_LENGTH 36 */
void *os_dlopen(const char *path); void *os_dlsym(void *module, const char *func); void os_dlclose(void *module); bool os_is_obs_plugin(const char *path);
bool os_get_emulation_status(void); os_performance_token_t *os_request_high_performance(const char *reason); void os_end_high_performance(os_performance_token_t *);
void os_breakpoint(void); void os_oom(void);
```

## 4. Data formats

### 4.1 UTF-8 as understood by the reference (the "extended" form)

A sequence is a lead byte followed by continuation bytes (`10xxxxxx`):

| Lead byte | length n | payload bits of the lead |
|---|---|---|
| `00..7F` | 1 | 7 |
| `C0..DF` | 2 | 5 (`& 0x1F`) |
| `E0..EF` | 3 | 4 (`& 0x0F`) |
| `F0..F7` | 4 | 3 (`& 0x07`) |
| `F8..FB` | 5 | 2 (`& 0x03`) |
| `FC..FD` | 6 | 1 (`& 0x01`) |
| `80..BF`, `FE`, `FF` | none (invalid) | |

The value is the lead payload followed by the 6 low bits of each continuation byte, most significant first,
so values up to `0x7FFFFFFF` are representable. The encoder chooses the shortest form for a value:
`<=7F`:1, `<=7FF`:2, `<=FFFF`:3, `<=1FFFFF`:4, `<=3FFFFFF`:5, otherwise 6 bytes.

Worked examples (all in vectors): `C3 A9` = `E9`; `E2 82 AC` = `20AC`; `F0 9F 98 80` = `1F600`;
`F4 8F BF BF` = `10FFFF`; `F4 90 80 80` = `110000` (decoder accepts, encoder produces the same bytes);
`FD BF BF BF BF BF` = `7FFFFFFF`; `E0 80 80` (overlong NUL) decodes to `0` (accepted); `ED A0 80` would be `D800`
(a surrogate: rejected).

### 4.2 Text file conventions

`os_quick_write_utf8_file` / `os_fread_utf8`: a file is a byte sequence; a UTF-8 byte order mark `EF BB BF`
at offset 0 is the only marker understood (UTF-16 BOMs are not). Reading returns the bytes after the mark.

### 4.3 `os_dtostr` text

`%.17g` of the C library (17 significant digits, shortest of fixed/exponential per `%g`, C locale `.`), then the
adjustments of rule R40. Worked: `0.1` -> `0.10000000000000001`; `2` -> `2.0`; `-0.0` -> `-0.0`;
`1e21` -> `1e21`; `1.5e-7` -> `1.4999999999999999e-7`; `inf` -> `inf.0`.

### 4.4 Formatted file name tokens

Table T1 (order is significant; a token matches when the text at the current position starts with it):

| # | Token | strftime format used | Result (C locale) |
|---|---|---|---|
| 1 | `%CCYY` | `%Y` | four digit year |
| 2 | `%YY` | `%y` | two digit year |
| 3 | `%MM` | `%m` | month 01-12 |
| 4 | `%DD` | `%d` | day 01-31 |
| 5 | `%hh` | `%H` | hour 00-23 |
| 6 | `%mm` | `%M` | minute 00-59 |
| 7 | `%ss` | `%S` | second 00-60 |
| 8 | `%%` | `%%` | `%` |
| 9-23 | `%a %A %b %B %d %H %I %m %M %p %S %y %Y %z %Z` | the token itself | abbreviated/full weekday (`Tue`/`Tuesday`), abbreviated/full month (`Jan`/`January`), day, hour 00-23, hour 01-12, minute `%M`, `AM`/`PM`, second, 2-digit year, year, `+hhmm`, zone abbreviation |

Table T2 (checked only when no T1 token matched): `%FPS`, `%CRES`, `%ORES`, `%VF`, `%s`.

## 5. Behaviour (numbered rules)

### 5.1 `utf8_to_wchar`

- **S-023-R01** Returns 0 if `in` is NULL, or if `out` is non-NULL and `outsize` is 0. `out == NULL` selects
  the *sizing mode* (R12); `outsize` is then ignored.
- **S-023-R02** Input length: `insize > 0` -> exactly `insize` bytes are examined and a byte `00` is an ordinary
  character (U+0000, counted and written). `insize == 0` -> input ends at the first `00` byte (not counted);
  an empty input gives 0.
- **S-023-R03** Each step examines the byte at the cursor. Unless `UTF8_IGNORE_ERROR` is set, a byte equal to
  `C0`, `C1`, `F5` or `FF` is an error (returns 0). With the flag set this test is **not applied** (Q-2): `C0`, `C1`,
  `F5` are then ordinary lead bytes (table in 4.1) and `FF` falls under R04.
- **S-023-R04** A byte that is no lead byte (`80..BF`, `FE`, `FF`) is an error (rc 0); with `UTF8_IGNORE_ERROR` it
  is skipped (cursor +1, nothing produced).
- **S-023-R05** *Truncation.* In explicit-length mode, if fewer than `n` bytes remain from the lead byte on, it is an
  error (skip: cursor +1). In terminator mode this check is not made; a truncated sequence is caught by R06 because
  the terminating `00` is not a continuation byte.
- **S-023-R06** The `n-1` bytes after the lead must all be of the form `10xxxxxx`; otherwise error (skip: cursor +1 only —
  the bytes after the bad lead are examined again as new characters, so they are typically skipped too by R04).
- **S-023-R07** *Value.* Computed as in 4.1. No overlong check (`E0 80 80` -> 0, `F0 80 80 AF` -> `2F`), no upper
  bound (`F7 BF BF BF` -> `1FFFFF`, `FD BF BF BF BF BF` -> `7FFFFFFF`). A value in `D800..DFFF` is an error
  (skip: the whole sequence is dropped and not counted).
- **S-023-R08** The value `FEFF` is an ordinary character, except with `UTF8_SKIP_BOM`: it is then dropped (not
  counted).
- **S-023-R09** *Error return.* Without `UTF8_IGNORE_ERROR` the first error ends the call with 0 (output already written is
  unspecified). With the flag, problems are skipped as stated in R04-R07; if nothing is left the result is 0 (the
  same value as for an error).
- **S-023-R10** *Output capacity* (writing mode). Every valid sequence that reaches the value stage first needs a free
  slot (`written < outsize`), else the call returns 0 — **even if the character is about to be dropped** (surrogate with
  IGNORE, BOM with SKIP_BOM, Q-5). A dropped character leaves the output unchanged. No terminator is written.
- **S-023-R11** *Result.* The number of `wchar`s produced (= kept characters).
- **S-023-R12** *Sizing mode* (`out == NULL`): the sequence checks R03-R06 and the invalid-lead handling apply, but the
  value-stage checks do not: surrogates (R07) and U+FEFF (R08) are **counted**, with or without the flags (Q-4). The
  result can therefore be larger than what the writing mode returns for the same input and flags; in strict mode the
  sizing call can succeed where the writing call returns 0.

### 5.2 `wchar_to_utf8`

- **S-023-R13** Returns 0 if `in` is NULL, or `out` non-NULL and `outsize == 0`. `out == NULL` is the sizing mode (it applies the same
  rules as the writing mode, except for capacity).
- **S-023-R14** `insize > 0`: that many `wchar`s (a `0` is an ordinary character and is encoded as the single byte `00`);
  `insize == 0`: up to the first `0` element (not encoded).
- **S-023-R15** For each element in order: (a) a value in `D800..DFFF` -> error (rc 0), or skipped with
  `UTF8_IGNORE_ERROR`; (b) `FEFF` with `UTF8_SKIP_BOM` -> skipped; (c) a negative value -> error, or skipped with
  `UTF8_IGNORE_ERROR`; (d) otherwise encoded in the shortest extended form of 4.1 (values >`10FFFF` are encoded, Q-6).
  Checks (a) and (b) come before (c), so with only `UTF8_SKIP_BOM` set a surrogate is still an error.
- **S-023-R16** Capacity (writing mode): an element of `n` bytes needs `n` free bytes (`written + n <= outsize`), else the call
  returns 0 (so there is no partial result). No terminator is written. Skipped elements need no space.
- **S-023-R17** Result: number of bytes produced; 0 on error and for empty / fully skipped input.

### 5.3 Conversions with termination

- **S-023-R18** `os_utf8_to_wcs(str, len, dst, dst_size)`: NULL `str` -> 0 (nothing written). `in_len = len ? len : strlen(str)` (so
  with `len == 0` an embedded `00` ends the input; with `len > 0` it is converted). Sizing (`dst == NULL`): returns the `utf8_to_wchar` size
  of `str` with flags 0 (so 0 on invalid input). Writing: `dst_size == 0` -> 0 (nothing written). If `dst_size == 1` nothing is converted (`n = 0`). Otherwise the text is converted with flags 0 into `dst` with a capacity of `dst_size` characters (not `dst_size - 1`) giving `n` characters
  (`n = 0` if the conversion failed, e.g. invalid input or more characters than `dst_size`). Then `dst[n] = 0` and `n` is returned. Hence invalid input yields the empty string and 0.
  Precondition (D-1): `dst_size` must be **greater than the number of characters the conversion needs**; a text of exactly `dst_size` characters overflows the buffer in the reference.
- **S-023-R19** `os_wcs_to_utf8(str, len, dst, dst_size)`: same structure; `in_len = len ? len : wcslen(str)`; the
  converter gets room for `dst_size - 1` bytes; `dst[n] = 0`. A result needing `dst_size` or more bytes -> the empty string and 0. This function does not
  have D-1.
- **S-023-R20** `_ptr` forms (`os_utf8_to_wcs_ptr`, `os_wcs_to_utf8_ptr`, `os_mbs_to_wcs_ptr`, `os_wcs_to_mbs_ptr`): NULL input -> `*pstr = NULL`,
  return 0. Otherwise the size is determined with the sizing call, a buffer of exactly `size + 1` units is allocated,
  filled by the writing call, and the length returned. Invalid input therefore yields a valid empty string (not NULL).
- **S-023-R21** `os_utf8_to_mbs_ptr(str, len, &out)`: NULL -> `*out = NULL`, 0; otherwise `str` is converted to wide (R20) and then to the multibyte
  encoding (`os_wcs_to_mbs_ptr`); the second step receives the *wide length* (ignored by it). `os_mbs_to_utf8_ptr` is the mirror
  (multibyte -> wide -> UTF-8). `len` selects the input bytes only in the first step (`len == 0` = whole string).
- **S-023-R22** `os_mbs_to_wcs`/`os_wcs_to_mbs` (and `_ptr`) use the C library's `mbstowcs`/`wcstombs` of the current locale; the `len`
  argument is **ignored** (conversion always runs to the terminator, so `os_mbs_to_utf8_ptr(str, 3)` converts the whole string, Q-14). In a UTF-8
  locale this is the identity on valid UTF-8; the conformance suite runs in the "C" locale with ASCII data only.
  Structure of the writing form as R18, including the `dst_size == 0` and `== 1` cases, but with `mbstowcs`/`wcstombs` limited to `dst_size` units: a text longer than the buffer is *truncated* to `dst_size` units and then (D-1) terminated one unit past the buffer; only
  buffers that are large enough (or of size 0 or 1) have defined results. Invalid / unrepresentable input is D-6.
- **S-023-R23** (Windows only, informative) `utf8_to_wchar`/`wchar_to_utf8` use `MultiByteToWideChar`/`WideCharToMultiByte` (UTF-16, flags
  ignored except that a leading BOM is removed on input when at least 3 bytes are given); the Linux rules above do not apply there.

### 5.4 Numbers

- **S-023-R40** `os_dtostr(value, dst, size)`: (1) print `value` with `%.17g` into `dst[0..size)` (C locale; `inf`, `-inf`, `nan`). If
  the printed length is `>= size` return -1. (2) If the text contains neither `.` nor `e`, append `.0`; if
  `length + 3 >= size` instead return -1 (so for a short integer text `"5"` the buffer must hold at least 5 bytes, not 4: Q-8). `inf`/`nan` contain
  neither and become `inf.0`, `-inf.0`, `nan.0` (Q-7). (3) If the text contains `e`: remove a `+` sign after the `e` and remove leading zeros of the
  exponent digits (`e+21` -> `e21`, `e-05` -> `e-5`, `e+100` -> `e100`, `e-100` unchanged); a `-` is kept. The size check of step 1 is made on the
  text *before* this shortening. (4) Return the final text length (a value >= 0). The text MUST be correctly terminated after that length
  (D-2: the reference does not terminate it; see §8). On -1 the buffer content is unspecified.
- **S-023-R41** `os_strtod(str)`: copy at most 63 bytes of `str` (a longer string is cut, the rest ignored), then `strtod` of the copy in the C
  locale and return its value: leading blanks skipped; optional sign; decimal digits with optional `.` and exponent; hexadecimal floats (`0x10` = 16,
  `0x1p4`, `0x.8`); `inf`/`infinity`/`nan` in any letter case; overflow -> +-infinity; underflow -> 0 (or denormal); trailing garbage ignored; no
  conversion (`abc`, `""`, `.`, `e5`) -> 0.0. A `,` is not a decimal point. (In a locale whose decimal separator is not `.` the first `.` is replaced by
  the locale's separator before parsing, so the result does not depend on the locale.)

### 5.5 Files: handles and whole-file helpers

- **S-023-R50** `os_fopen(path, mode)`: NULL `path` -> NULL; otherwise the C library `fopen(path, mode)` on the raw bytes of `path`
  (modes `r w a` with optional `+` and `b`; missing file with `r`/`r+` -> NULL; `w` creates/truncates, `a` appends; permissions `0666 & ~umask`; a directory
  cannot be opened for writing -> NULL; a missing parent directory -> NULL).
- **S-023-R51** `os_wfopen(wpath, mode)`: NULL -> NULL; else the wide path is converted to UTF-8 with `os_wcs_to_utf8_ptr` (R20) and opened like R50. A
  path containing a surrogate converts to the empty string and the call returns NULL.
- **S-023-R52** `os_fseeki64(f, off, origin)` is `fseeko` with 64 bit offsets (`SEEK_SET=0, SEEK_CUR=1, SEEK_END=2`), returning 0 or -1; a negative
  resulting position fails (-1) and leaves the position unchanged; seeking beyond the end is allowed on a readable file (no growth); offsets above 2^32 work.
  `os_ftelli64` returns the position (`ftello`), -1 on error.
- **S-023-R53** `os_fgetsize(f)`: remember the position, seek to the end, read the position as the size, seek back to the remembered position, return the size (position restored,
  pending buffered writes included in the size). If the seek to the end fails return -1 (the position is then unspecified). A failing `ftell` gives -1 as the size.
- **S-023-R54** `os_get_file_size(path)`: -1 if the file cannot be opened (also NULL path), else `os_fgetsize` of it (0 for an empty file).
- **S-023-R55** `os_fread_utf8(f, &str)`: `*str = NULL` first. Determine the file size with seek-to-end (regardless of the current position). If the size is 0 -> return 0 (str NULL).
  Otherwise seek to 0 and read up to 3 bytes; if they are `EF BB BF` the data starts at offset 3, else at 0 (a file shorter than 3 bytes is never a BOM). If
  nothing is left (BOM-only file) -> return 0, str NULL (the position is 3). Otherwise read the remainder (`size - offset` bytes) from the data start; a read of 0 bytes
  frees and gives NULL; else `*str` is a newly allocated buffer holding the bytes read followed by `00`, and the position is the end of the file. The data is returned byte for byte
  (no validation, line endings unchanged, embedded `00` kept; a C consumer sees the text up to the first `00`). **The return value is always 0** (D-3).
- **S-023-R56** `os_fread_mbs(f, &str)`: `*str = NULL`; size by seek-to-end; if 0 -> 0. Otherwise read the whole file from offset 0, append `00`, convert with
  `os_mbs_to_utf8_ptr(buf, size)`, and return the converted length (so a `00` inside the file ends the converted text, R22). No BOM handling. Short read of 0 bytes -> NULL, 0. The position ends at the end of the file.
- **S-023-R57** `os_quick_read_utf8_file(path)` / `os_quick_read_mbs_file(path)`: NULL if the file cannot be opened; otherwise the string produced by the matching
  `os_fread_*` (NULL for an empty file, and for a BOM-only file in the UTF-8 form) and the file is closed.
- **S-023-R58** `os_quick_write_utf8_file(path, str, len, marker)`: open for writing in binary mode (create/truncate), failure -> false. If `marker`, write
  `EF BB BF` (failure -> close, false). If `len > 0` write exactly `len` bytes from `str` (failure -> close, false); `len == 0` writes nothing (the text is not
  inspected, `str` may be NULL — there is **no** `strlen` fallback). Flush and close; result true. The result of the final flush/close is not checked (Q-13). The bytes are written unchanged
  (no validation, no newline conversion).
- **S-023-R59** `os_quick_write_mbs_file(path, str, len)`: open for writing (failure -> false); convert with `os_utf8_to_mbs_ptr(str, len)` (here `len == 0` **does** mean `strlen`); write the
  converted bytes if the converted length is non-zero; flush, close, return true (also when the conversion produced nothing: an empty file remains).
- **S-023-R60** `os_quick_write_utf8_file_safe(path, str, len, marker, temp_ext, backup_ext)`: if `temp_ext` is NULL or empty -> `LOG_ERROR`
  `"os_quick_write_utf8_file_safe: invalid temporary extension specified"` and false. Temp path = `path` + (`.` unless `temp_ext` starts with `.`) + `temp_ext`.
  Write it with R58; failure -> `LOG_ERROR` `"os_quick_write_utf8_file_safe: failed to write to <temp path>"`, false (the target is untouched). If `backup_ext` is non-NULL
  and non-empty the backup path is built the same way, else there is no backup (NULL). Then `os_safe_replace(path, temp, backup)`; 0 -> true, else false (the temp file stays).
- **S-023-R61** `os_safe_replace(target, from, backup)` (POSIX): if `backup` is **non-NULL** and `target` exists (`os_file_exists`), `rename(target, backup)`; if that fails return -1.
  Then return `rename(from, target)` (0 or -1). No rollback: when the second rename fails, the target has already been moved to `backup` (Q-9). A non-NULL empty `backup` string is used
  as a path (the rename fails with ENOENT -> -1 if the target exists; nothing happens if it does not). `rename` replaces an existing file target atomically.
- **S-023-R62** `os_file_exists(path)` is true iff the path can be accessed (`access(path, F_OK)`: symbolic links are followed, so a dangling link does not exist; directories count).

### 5.6 Directories and file system primitives

- **S-023-R70** `os_mkdir(path)`: `mkdir(path, 0755)` (before umask). Result 0 (`MKDIR_SUCCESS`) if created; 1 (`MKDIR_EXISTS`) if the OS reports that the name exists (`EEXIST`: a directory, a regular file, a link, even a dangling link); -1 (`MKDIR_ERROR`) for every other failure (missing parent, parent
  not a directory, empty path ...). Backslashes are ordinary name characters. A trailing `/` is accepted.
- **S-023-R71** `os_mkdirs(path)`: replace every `\` by `/` in a copy, then create recursively: try `os_mkdir(p)`; if the result is not -1 return it. Else, if `p` has no `/` return -1. Else
  `parent = p` up to the last `/`; `r = os_mkdirs_rec(parent)`; if `r == -1` return -1; otherwise return `os_mkdir(p)`. Consequences: the result describes only the final component
  (0 created, 1 already existing — also when it is a file); `a//b` and `a/b/` work; when a parent is a regular file the result is -1; a path whose first-level `mkdir` fails for another reason than `EEXIST` (e.g. `/x` without permission) ends up asking for the empty parent and returns -1. `"."`, `".."` and `"./"` return 1. The empty string is D-4.
- **S-023-R72** `os_rename` = `rename(2)`; `os_unlink` = `unlink(2)` (files and links, not directories: -1); `os_rmdir` = `rmdir(2)` (empty directories only; -1 on a file, link, non-empty directory, missing). All return 0 or -1.
  `rename` onto an existing file replaces it; a directory onto an empty directory replaces it; a file onto a directory, a directory onto a file, a directory onto a non-empty directory fail (-1); renaming a file onto itself succeeds.
- **S-023-R73** `os_copyfile(in, out)`: if `out` exists (`os_file_exists`, links followed) return -1 (nothing touched). Open `in` for reading in binary (`fopen`; failure -> -1, `out` not created). Open `out` for append-binary
  (creating it; failure -> -1). Copy in blocks of 4096 bytes (`read block; write what was read; continue while the block was full`). Result 0 if the input reached end-of-file, else -1. Consequences: sizes that are a multiple of 4096 work; an empty
  input produces an empty output and 0; the copy of a directory as `in` returns -1 *after* creating an empty `out` (Q-10); links as `in` are followed. The new file's permissions are `0666 & ~umask` (not copied from the source).
- **S-023-R74** `os_getcwd(buf, size)` = `getcwd` (NULL when the buffer is too small); `os_chdir(path)` = `chdir` (0 / -1).
- **S-023-R75** `os_get_free_space(path)`: `statvfs`; -1 on failure, else `f_bsize * f_bfree` (as `int64_t`). `os_get_free_disk_space(dir)`: 0 on failure, else `f_frsize * f_bavail`.
- **S-023-R76** `os_opendir(path)`: NULL if the directory cannot be opened (missing, not a directory). The handle **keeps the caller's `path` pointer** (not a copy); the string must stay valid until `os_closedir` (D-5). `os_readdir(dir)`: NULL for a NULL handle; otherwise the next entry of the OS enumeration (`readdir`),
  *including `.` and `..`*, in file-system order (unspecified — callers must not rely on it); NULL after the last entry and on every later call; NULL also if a name is 256 bytes or longer (cannot happen on Linux file systems). The result points into the handle and is valid until the next call. `d_name` is the entry name (not a path). `directory` is true iff
  `stat(path + "/" + name)` succeeds and the file is a directory (links are followed); if `stat` fails the flag is false and `LOG_DEBUG` `"is_dir: stat for <path/name> failed, errno: <n>"` is logged (n = 2 for a dangling link). `os_closedir(NULL)` is a no-op.
- **S-023-R77** `os_glob(pattern, flags, &out)`: `flags` is ignored (the reference passes 0 to `glob`). Calls `glob(3)` without flags: sorted ascending (by `strcoll`; bytewise in the C locale: `A1 < a.txt < b.txt < b1 < b10 < b2`), no `GLOB_BRACE`, no `GLOB_TILDE`
  (`{a,b}` and `~` are literal), no `GLOB_NOCHECK` (no match is an error), no `GLOB_PERIOD`. Pattern syntax (fnmatch): `*` any run (also empty) of characters other than `/`; `?` one character other than `/`; `[abc]`, `[a-z]` ranges, `[!x]` and `[^x]` negation; `\x` matches `x` literally; a leading `.` of a path component is
  matched only by a `.` in the pattern (`*` does not match `.hid`; `.*` matches `.`, `..` and `.hid`); wildcards never match `/`; components without wildcards must exist; a trailing `/` in the pattern restricts matches to directories and is kept in the result path (`g/*/` -> `g/sub/`).
  Result: 0 and `*out` allocated with `gl_pathc` entries, each `path` the matched path spelled relative/absolute like the pattern (not normalised: `g/../g/a.*` -> `g/../g/a.txt`), and `directory` computed with `stat` (links followed; a dangling link: false and the R76 debug message). Failure: the `glob` error code (3 = no match; 1 = out of memory, 2 = read error), `*out = NULL`.
  `os_globfree(NULL)` is a no-op; the entry paths belong to the object and die with it.
- **S-023-R78** `os_get_path_extension(path)`: scan from the end towards the start; return a pointer to the first `.` found; but stop with NULL when a `/` or `\` is met first (the last path component has no dot). The result therefore starts with `.` and includes everything after it:
  `a/b.c` -> `.c`; `file.tar.gz` -> `.gz`; `.hidden` -> `.hidden`; `x.` -> `.`; `..` -> `.` (the last one); `dir.d/file` -> NULL; `a\b.c` -> `.c`. Empty path -> NULL. NULL path is invalid.
- **S-023-R79** `os_get_abs_path(path, abspath, size)`: 0 if `abspath` is NULL; `realpath(path)` (0 if it fails: missing file, empty path, NULL path); the resolved path is copied with `snprintf(abspath, min(size, PATH_MAX), "%s")`; the
  return value is the length of the *full* resolved path (it may be `>= size`, the buffer is then truncated and NUL terminated; with `size == 0` nothing is written). Symbolic links are resolved; `.` and `..` removed; no trailing slash.
  `os_get_abs_path_ptr(path)`: allocates 512 bytes and uses the above with size 512; NULL if it returned 0; a result of 512 bytes or more is silently truncated to 511 (Q-15).
- **S-023-R80** `os_get_config_path(dst, size, name)`: if the environment variable `XDG_CONFIG_HOME` is set (an empty value counts as set): `"<XDG>"` when `name` is NULL or empty, else `"<XDG>/<name>"`.
  Otherwise `HOME` is used: `"<HOME>/.config"` or `"<HOME>/.config/<name>"`; if `HOME` is not set the process terminates with the fatal error `bcrash("Could not get $HOME\n")` (S-001). Returns the `snprintf` result (the length of the complete text, also when truncated; `dst`
  may be NULL with `size` 0 to measure). `os_get_config_path_ptr(name)` returns an allocated string built the same way **but without the NULL/empty-name case**: it always appends `"/"` (XDG) or `"/.config/"` (HOME) before `name`, so a NULL or empty `name` leaves a trailing slash
  (`/xdg/`, `/home/u/.config/`) (Q-11).
- **S-023-R81** `os_get_program_data_path(dst, size, name)` = `snprintf(dst, size, "/usr/local/share/%s", name ? name : "")` (same return semantics); `_ptr` returns an allocated copy.
- **S-023-R82** `os_get_executable_path_ptr(name)`: directory of the running executable (`readlink /proc/self/exe`, then `dirname`) + `"/"` + `name` (when `name` is non-NULL and non-empty); NULL if the executable path cannot be determined. Machine dependent.

### 5.7 Formatted file names

- **S-023-R90** `os_generate_formatted_filename(ext, space, fmt)` returns a newly allocated string or NULL (R96). Inputs: the current local time (`localtime(time(0))`, honouring `TZ`, DST) and the current video settings (fps numerator `fps_num`, denominator `fps_den`, canvas size `base_width x base_height`, output size
  `output_width x output_height`, `output_format`). A NULL `fmt` behaves like the empty string.
- **S-023-R91** *Scan.* Work on a copy `s` of `fmt` with a cursor `pos = 0`. While `pos < len(s)`: let `rest = s[pos..]`. **Step A**: go through table T1 in order and stop at the first token that is a prefix of `rest`; `conv` = `strftime` (C locale, 127 bytes at most) of the token's strftime format (T1 column 3).
  If `conv` is non-empty and contains a character other than a space, replace the token in `s` by `conv`. (If `conv` is empty the scan continues with the next T1 entries; if `conv` is non-empty but consists only of spaces, `s` is left unchanged but `conv` stays set — not reachable with C-locale
  output.) **Step B** (only if `conv` is still empty): in this order — `rest` starts with `%FPS`: `conv` = `fps_num` in decimal when `fps_den <= 1`, else `fps_num / fps_den` (floating point) with two decimals (`%.2f`: `30000/1001` -> `29.97`, `5/2` -> `2.50`); replace the 4 characters.
  `%CRES`: `"<base_width>x<base_height>"` (5 chars). `%ORES`: `"<output_width>x<output_height>"` (5 chars). `%VF`: name of `output_format` (table T3), 3 chars. `%s`: the epoch seconds of the current time in decimal (2 chars). **Step C**: if `conv` is non-empty advance
  `pos` by `len(conv)` (so replaced text is never rescanned, and the `%` produced by `%%` is plain text) and clear it; else if `s[pos] == '%'` delete that character (the following characters stay and are scanned normally — an unknown token `%q` becomes `q`, a trailing `%` disappears); else advance `pos` by one.
- **S-023-R92** Consequences of the order (tested): `%YY` is the 2-digit year, `%Y` the 4-digit one, `%YYYY` = 2-digit year + the text `YY`; `%mm` is the minute but `%m` the month (a month directly followed by the letter `m` cannot be written); `%MM` is the month but `%M` the minute;
  `%ss` is the second but `%s` the epoch (so epoch followed by `s` cannot be written); `%DD` and `%d` both give the day; `%hh` is the hour while `%h` is unknown (-> `h`); tokens are case sensitive (`%yy`, `%fps` are unknown); `%e %j %c %x %X %w %k %l %n %t %u %U %V %W %C %D %F %G %g %r %R %T %h`
  and flag forms (`%-d`, `%5d`, `%Ed`, `%CC`, `%FP`) are unknown tokens: only the `%` is dropped.
- **S-023-R93** After the scan: if `space` is false every space character of `s` becomes `_`. Then, if `ext` is non-NULL and non-empty, `"." + ext` is appended (the extension keeps its spaces, a leading dot in `ext` is not removed: `".mkv"` -> `..mkv`).
- **S-023-R94** Finally, if the length exceeds 255 *bytes* the string is cut to its first 255 bytes (mid-character if necessary; the extension may be cut off).
- **S-023-R95** T3 `output_format` names: `NONE`->`None`, `I420 NV12 I422 I210 YVYU YUY2 UYVY RGBA BGRA BGRX I444 I412 Y800 BGR3 I40A I42A YUVA YA2L AYUV I010 P010 P216 P416` -> the same text; `V210` -> `v210`; `R10L` -> `R10l`; any other value -> `None`.
- **S-023-R96** The result is NULL when the final string is empty (empty/NULL `fmt` and no extension, or a format consisting only of unknown `%` characters), because an empty string is never allocated by the reference. With a non-empty extension the result is at least `"." + ext`.

### 5.8 Time and machine queries

- **S-023-R100** `os_gettime_ns()`: nanoseconds of the monotonic clock (`CLOCK_MONOTONIC`); non-zero and never decreasing. `os_sleep_ms(ms)` sleeps at least `ms` milliseconds (the product `ms*1000` is computed in 32 bits, so very long values wrap, Q-16).
- **S-023-R101** `os_sleepto_ns(t)`: if `t < os_gettime_ns()` return false at once; else sleep (resuming after interruptions) until the monotonic clock has reached `t` and return true. `os_sleepto_ns_fast(t)`: same contract, implemented as repeated sleeps of at most 999999 microseconds with the remaining time rounded up to whole microseconds.
- **S-023-R102** `os_nstime_to_timespec(ts, storage)`: NULL if `storage` is NULL or the wall clock could not be sampled. At the first call (process wide, once) one wall clock reading (`timespec_get(TIME_UTC)`) is paired with one `os_gettime_ns()` reading (the *anchor*). The result is the anchor wall time plus (`ts` - anchor monotonic time) in nanoseconds, signed, with `tv_nsec` normalised into `[0, 10^9)` (reference
  defect: a result with exactly `10^9` ns is not normalised, D-7). It returns `storage`. Differences between results are exact: `(t2-t1)` in ns for any two timestamps, earlier or later than the anchor.
- **S-023-R103** `os_get_physical_cores()` / `os_get_logical_cores()`: cached after the first call. Logical = `sysconf(_SC_NPROCESSORS_ONLN)`. Physical: from `/proc/cpuinfo`, sum the `cpu cores` values once per distinct `physical id`; if the sum is 0 the logical count is used; if `/proc/cpuinfo` cannot be opened the physical count stays **0** (Q-17). Contract: logical >= 1, physical <= logical when available.
- **S-023-R104** `os_get_sys_free_size()` = `(freeram + bufferram) * mem_unit` of `sysinfo` (0 on failure); `os_get_sys_total_size()` = `totalram * mem_unit`, cached. `os_get_proc_memory_usage(u)` (and the two single-value functions): from `/proc/self/statm`: `resident_size` = resident pages * page size (bytes); `virtual_size` = the **raw page count** (not multiplied, Q-12); false / 0 if the file cannot be parsed.
- **S-023-R105** `os_cpu_usage_info_start()` samples `times()` and the online core count; `os_cpu_usage_info_query(info)` returns `100 * (d_user + d_sys) / d_wall / cores` since the previous sample and moves the sample forward; 0.0 for a NULL `info`, when no wall time has passed, or when a counter went backwards; `..._destroy(NULL)` is a no-op.
- **S-023-R106** `os_generate_uuid()` returns a newly allocated 36 character lowercase hexadecimal random (version 4) UUID `xxxxxxxx-xxxx-4xxx-[89ab]xxx-xxxxxxxxxxxx` (NUL terminated, buffer 37).
- **S-023-R107** `os_dlopen(path)`: NULL for NULL `path` (no log). Otherwise if `path` contains the substring `.so` anywhere it is used as is, else `.so` is appended; `dlopen(RTLD_NOW)`. On failure `LOG_ERROR` `"os_dlopen(<path>-><used name>): <dlerror text>\n"`. `os_dlsym` = `dlsym`; `os_dlclose(NULL)` is a no-op.
  `os_is_obs_plugin(path)` is always true, `os_get_emulation_status()` always false, `os_request_high_performance()` returns NULL and `os_end_high_performance` does nothing (Linux). `os_breakpoint()` / `os_oom()` raise `SIGTRAP` on Linux (they are the hooks called by S-001 before it terminates).

## 6. Algorithms

`os_dtostr` in pseudocode (the *required* behaviour, D-2 fixed):

```
text = format("%.17g", value);  n = len(text)
if n >= size: return -1
if '.' not in text and 'e' not in text:
    if n + 3 >= size: return -1
    text += ".0";  n += 2
if let Some(e) = index of 'e' in text:
    sign = text[e+1] is '+' or '-' ? text[e+1] : none ; digits = text after the sign
    text = text[..e+1] + (if sign == '-' then "-" else "") + digits with leading '0's removed
    n = len(text)
write text and a NUL into dst; return n
```
(The exponent of `%.17g` never has fewer than two digits and never all zeros, so "digits with leading zeros removed" is never empty.)

`utf8_to_wchar` (writing mode, flags F; `lim` = end of input, or the first NUL in terminator mode):

```
count = 0; i = 0
while i < lim:
    b = in[i]
    if terminator_mode and b == 0: break
    if forbidden(b) and not F.ignore: return 0                      # C0 C1 F5 FF
    (n, high) = lead_class(b)                                        # 4.1; none -> error handling (skip 1 or return 0)
    if explicit_mode and lim - i < n: error: (skip 1 | return 0)
    if any continuation of in[i+1 .. i+n) is not 10xxxxxx: error: (skip 1 | return 0)
    value = combine(high, continuations)
    count += 1
    if written == outsize: return 0                                  # even if dropped below
    if surrogate(value): if not F.ignore: return 0 else: count -= 1; i += n; continue
    if value == FEFF and F.skip_bom: count -= 1; i += n; continue
    out[written++] = value; i += n
return count
```
(In sizing mode `written == outsize`, the surrogate test and the BOM test are omitted.) "skip 1" means `i += 1`.

## 7. Concurrency

No function holds a lock. `os_get_physical_cores`/`logical`/`os_get_sys_total_size` populate a cache without synchronisation (benign: idempotent values). `os_nstime_to_timespec` initialises its anchor exactly once per process (`pthread_once`).
`os_glob`/`os_readdir` results are not shared. `os_readdir` returns a pointer into the handle that is overwritten by the next call on the *same handle*.

## 8. Reference defects and quirks

| ID | Kind | What | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | `utf8_to_wchar` accepts overlong forms (`E0 80 80` = U+0000, `F0 80 80 AF`), 5/6 byte forms and values above U+10FFFF; it rejects only lead bytes `C0 C1 F5 FF` and surrogates | MUST match (R07) | `03-utf8-lenient-forms` |
| Q-2 | QUIRK | With `UTF8_IGNORE_ERROR` the lead bytes `C0 C1 F5` are no longer rejected but decoded (`61 C0 80` -> `61, 0`) | MUST match (R03) | `04-utf8-decode-flags` |
| Q-3 | QUIRK | After a bad sequence with IGNORE only one byte is skipped, so continuation bytes are looked at again (and dropped as stray bytes) | MUST match (R06) | `04-utf8-decode-flags` |
| Q-4 | QUIRK | Sizing call (`out == NULL`) counts surrogates and BOM that the real conversion would drop (and succeeds for surrogates in strict mode) | MUST match (R12) | `05-utf8-sizing-quirks` |
| Q-5 | QUIRK | A character that is dropped later still needs a free output slot when it is reached (`61 ED A0 80` with IGNORE and capacity 1 -> 0) | MUST match (R10) | `05-utf8-sizing-quirks` |
| Q-6 | QUIRK | `wchar_to_utf8` encodes values above U+10FFFF as 4-6 byte forms instead of failing | MUST match (R15) | `06-utf8-encode` |
| Q-7 | QUIRK | `os_dtostr` of `inf`/`nan` yields `inf.0`, `-inf.0`, `nan.0` (sign of a NaN as printed by `printf`) | SHOULD match | `09-dtostr` |
| Q-8 | QUIRK | `os_dtostr` needs `length + 4` bytes for an integral value (buffer 4 fails for `5`, 5 works), and the size check precedes exponent shortening (`1e21` needs size > 5) | MUST match (R40) | `09-dtostr` |
| Q-9 | QUIRK | `os_safe_replace`: when the final rename fails, the target has already been renamed to the backup (no rollback) | MUST match (R61) | `17-safe-replace` |
| Q-10 | QUIRK | `os_copyfile` of a directory returns -1 but leaves an empty destination file | MUST match (R73) | `21-copyfile` |
| Q-11 | QUIRK | `os_get_config_path_ptr(NULL or "")` ends with `/` while the buffer form does not | MUST match (R80) | `26-config-paths` |
| Q-12 | QUIRK | `virtual_size` of the process memory query is in pages on Linux, `resident_size` in bytes | SHOULD match (a port MAY return bytes; no consumer depends on it); only "non-zero" is tested | `33-system-contracts` |
| Q-13 | QUIRK | `os_quick_write_*` ignore the result of the final flush/close (disk-full is not reported) | SHOULD match (a port MAY report it) | not testable |
| Q-14 | QUIRK | the `len` argument of `os_mbs_to_wcs*`/`os_wcs_to_mbs*` is ignored (`os_mbs_to_utf8_ptr("hello", 3)` -> `hello`) | MUST match (R22) | `38-mbs-conversions` |
| Q-15 | QUIRK | `os_get_abs_path_ptr` truncates silently at 511 bytes | SHOULD match | not tested |
| Q-16 | QUIRK | `os_sleep_ms` computes `ms * 1000` in 32 bits | MAY differ (a port should sleep the full time) | not tested |
| Q-17 | QUIRK | physical core count is 0 when `/proc/cpuinfo` cannot be read | SHOULD match | not testable |
| D-1 | DEFECT | `os_utf8_to_wcs`, `os_mbs_to_wcs`, `os_wcs_to_mbs` write the terminating NUL one unit **past** `dst` when the converted text exactly fills `dst_size` (heap buffer overflow, found with AddressSanitizer) | MUST NOT write outside `dst`; for `os_utf8_to_wcs` treat `dst_size <= needed` as failure: return 0 and store the empty string when `dst_size >= 1`; for the two multibyte functions any memory-safe result is acceptable when the buffer is too small | none (undefined in the reference) |
| D-2 | DEFECT | `os_dtostr` shortens an exponent with a move that is one byte too short, so the buffer keeps stale trailing characters: `1e21` -> buffer `1e211` while the return value says 4; `config_set_double` stores the buffer, so values with an exponent are **corrupted** in config files (`1e-5` -> `1.0000000000000001e-55`) | return value and text MUST be consistent: the text is exactly the first `rc` bytes (`1e21`) and is NUL terminated there | `09-dtostr` (the stale buffer is on `info_dtostr_raw` lines, not compared) |
| D-3 | DEFECT | `os_fread_utf8` always returns 0 (the length is never assigned; ABI-visible bug). The requirement keeps the bug on purpose: nothing consumes the value and an ABI shim that returned the length could change a caller that compares it | The C ABI function MUST return 0; a native Rust API may return the length | `13-fread-utf8` |
| D-4 | DEFECT | `os_mkdirs("")` dereferences NULL (crash) | MUST NOT crash; return -1 (`MKDIR_ERROR`) | `37-platform-defects` |
| D-5 | DEFECT | `os_opendir` keeps the caller's `path` pointer; passing a temporary string that is freed before `os_readdir` is a use-after-free | A port MUST copy the path | not testable |
| D-6 | DEFECT | `os_mbs_to_wcs(_ptr)` / `os_wcs_to_mbs(_ptr)` on input the locale cannot convert get `(size_t)-1` from the C library and then allocate 0 bytes (fatal error) or index out of range | MUST return 0 and the empty string | not tested |
| D-7 | DEFECT | `os_nstime_to_timespec` may return `tv_nsec == 1000000000` (condition is `>` instead of `>=`) | MUST return a normalised timespec (`tv_nsec < 10^9`) | not reachable deterministically |

## 9. Implementation notes (non-normative)

- Use `u32` for `wchar` and `i32` when a signed view is needed (the encoder rejects negatives). A native API: `utf8_to_wchar(input: &[u8], flags, out: Option<&mut [u32]>) -> usize`.
  On Windows the reference uses UTF-16 (`u16`) and the OS conversion functions; only the Linux behaviour is specified here.
- Paths: use `OsStr`/byte strings, not `String`; the reference never validates UTF-8 in path arguments. `os_fopen` returns a C `FILE*`; a Rust port can expose `File` plus the sizing/seek helpers and provide `FILE*` only at the C ABI.
- `os_mkdirs`, `os_copyfile`, `os_safe_replace`, `os_quick_write_*`: reproduce the exact call sequences (create target `0755` directories; `ab+` open of the copy target; rename order) because their failure behaviour (Q-9, Q-10) is observable.
- `glob`: either call `glob(3)` through FFI with flags 0 or reimplement fnmatch; if reimplementing, sort bytewise, never match `/` with wildcards, hide leading dots, and keep `.`/`..` for `.*`.
- `os_generate_formatted_filename`: the pure algorithm (R91-R96) takes a broken-down time, the epoch value and the video settings as inputs so that it can be tested without clocks; the `strftime` fields in the C locale are fully determined (`%a` = `Sun..Sat`, `%b` = `Jan..Dec`, `%p` = `AM/PM`, `%I` zero padded). The time zone abbreviation and offset are the C library's (`TZ`).
- `os_dtostr`: format with a correct `%.17g` (Rust's `{:e}` is not enough; use `libc::snprintf` or a `%g` implementation), then post-process as in section 6.
- Windows: `os_fopen` converts the UTF-8 path to UTF-16 (`_wfopen`), `os_stat` is a UTF-8 wrapper over `_wstat`, rename replace semantics and `os_safe_replace` use `MoveFileEx`/`ReplaceFile` (not verified here), `os_get_config_path` uses the roaming AppData folder; macOS has its own `os_gettime_ns` (mach time) and config path (`~/Library/Application Support`).

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01-R05 | `01-utf8-decode-valid`, `02-utf8-decode-invalid` |
| R06-R09 | `02-utf8-decode-invalid`, `03-utf8-lenient-forms`, `04-utf8-decode-flags` |
| R10-R12 | `04-utf8-decode-flags`, `05-utf8-sizing-quirks`, `01-utf8-decode-valid` |
| R13-R17 | `06-utf8-encode`, `07-utf8-encode-flags` |
| R18-R22 | `08-os-wcs-utf8`, `38-mbs-conversions`, `36-null-arguments` |
| R23 | (Windows only, not tested) |
| R40 | `09-dtostr` |
| R41 | `10-strtod` |
| R50-R51 | `11-fopen-modes`, `36-null-arguments` |
| R52-R54 | `12-file-seek-size` |
| R55-R57 | `13-fread-utf8`, `14-fread-mbs` |
| R58-R59 | `15-quick-write` |
| R60 | `16-quick-write-safe` |
| R61 | `17-safe-replace`, `16-quick-write-safe` |
| R62 | `11-fopen-modes` (via `exists` in `15`, `16`, `20`) |
| R70 | `18-mkdir` |
| R71 | `19-mkdirs`, `37-platform-defects` |
| R72 | `20-rename-unlink-rmdir` |
| R73 | `21-copyfile` |
| R74 | `25-abs-path-cwd` |
| R75 | `33-system-contracts` |
| R76 | `22-dir-iteration` |
| R77 | `23-glob` |
| R78 | `24-path-extension` |
| R79 | `25-abs-path-cwd` |
| R80-R81 | `26-config-paths` |
| R82 | `33-system-contracts` |
| R90-R96 | `27-formatted-filename`, `28-fname-time-tokens`, `29-fname-timezones`, `30-fname-video-tokens`, `31-fname-edge-cases`, `32-fname-truncation` |
| R100-R101 | `33-system-contracts` |
| R102 | `34-nstime-to-timespec` |
| R103-R106 | `33-system-contracts` |
| R107 | `35-dl-and-misc`, `33-system-contracts` |

## 11. Open questions

1. Comparison of pointer-returning results with the *machine-dependent* functions (`system`, `cpu_usage`, `free_space`, `exe_path`, `uuid`) is by property only; absolute values are not specified.
2. `os_glob` ordering uses `strcoll` of the process locale; the vectors are bytewise (C locale). Whether the front end sets a locale that changes the order is not known to the author.
3. `os_inhibit_sleep_*` (desktop-bus based, absent without GIO) are unspecified.
4. Behaviour of `os_fopen` with an invalid `mode` string is the C library's and is not specified.
5. **Conflict with S-007.** The S-007 conformance runner links a stand-in `os_dtostr` (in `gen/stubs.c`) that lacks exponent shortening and returns 0 on success: S-007-R08 (`1e21` -> `1e+21`) and the vector `config/04-typed-values` (`d4=1e+21`, `get_double T d4 1e+21`) do not describe the real function. The real function (this spec, R40/D-2) gives the text `1e21` (the reference buffer actually holds `1e211`, so `config_set_double` stores a corrupted value). S-007 must be revisited together with this spec. The other helpers S-007 assumes (`os_fread_utf8`, `os_strtod` in the C locale, `os_safe_replace`, `os_file_exists`) are consistent with S-023.
6. `os_fgets`-style helpers and `os_extract_path_*` do not exist in the 33.0.1 platform files and are therefore not specified.

> **Resolution of the S-007 conflict (open question 5):** `test/conformance/gen/stubs.c` now contains the exact reference `os_dtostr`, `S-007-R08` and the `config/04-typed-values` vectors were corrected to the real (defective) behaviour, and the choice between reproducing and fixing it is recorded as S-007 open question 2.

## 12. Round 9: the `platform.c` / `platform-windows.c` implementations, utf8 predicates, Windows helpers (added)

### Reference files (all cited)

- `libobs/util/platform.c`, `libobs/util/platform-windows.c`, `libobs/util/utf8.c`, `libobs/util/windows/obfuscate.c`, `libobs/util/windows/WinHandle.hpp`, `libobs/util/windows/device-enum.c`, `libobs/util/windows/win-version.h`

- **S-023-R108** `platform.c`: the cross-platform `os_*` file/IO layer — `os_wfopen`/`os_fopen`, `os_fgetsize`/`os_stat`/`os_fseeki64`/`os_ftelli64`, `os_fread_mbs`/`os_fread_utf8` (whole-file reads into arrays), `os_quick_read_mbs_file`/`os_quick_read_utf8_file`, `os_quick_write_mbs_file`/`os_quick_write_utf8_file` and `os_quick_write_utf8_file_safe` (atomic temp-file + rename), `os_get_file_size`, and the wide-conversion wrappers `os_mbs_to_wcs`/`os_utf8_to_wcs` (+ their `_free`/count variants) — the cross-platform contract is pinned in the earlier sections; this file is the shared implementation.
- **S-023-R109** `platform-windows.c`: the Win32 implementation of that contract — `get_clockfreq`/`get_winver` (cached system/build info), module access `os_dlopen`/`os_dlsym`/`has_obs_export`/`get_plugin_info`/`os_is_obs_plugin`, CPU usage `os_cpu_usage_info_start`/`os_cpu_usage_info_query`/`os_cpu_usage_info_destroy` (process + kernel/user times), `os_sleepto_ns`/`os_sleepto_ns_fast`/`os_sleep_ms`, `os_gettime_ns` (QPC), the `os_get_path_*` family (`os_get_path_internal`/`os_get_path_ptr_internal`), plus the remaining `os_*` entries (environment, process spawn/kill, window helpers, admin check) per the contract pinned earlier.
- **S-023-R110** `utf8.c`: `has_utf8_bom`, `utf8_to_wchar`/`wchar_to_utf8` (conversion with the error rules pinned earlier), `wchar_forbidden`/`utf8_forbidden` (forbidden-character predicates used by the path sanitiser).
- **S-023-R111** `windows/obfuscate.c`: `deobfuscate_str` (XOR unscramble at first use) and `ms_get_obfuscated_func` (resolve a CRT function name from the obfuscated table, so the binary does not reference it by plain name).
- **S-023-R112** Small Windows helpers: `windows/WinHandle.hpp` (`Clear` — RAII handle closer), `windows/device-enum.c` (`enum_graphics_device_luids` — graphics-device LUIDs via EnumDisplayDevices), `windows/win-version.h` (`win_version_compare` — build-version comparison).
