# S-007 `config-file` (INI configuration store)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `util/config-file.c`, `config-file.h` (+ platform helpers `os_dtostr`, `os_strtod`, `os_fread_utf8`, `os_safe_replace`) |
| Target crate | `crates/obs-util` (module `config`) |
| Depends on | S-001, S-006; platform helpers (below) |
| Conformance | `test/conformance/scripts/config/` (11 scripts) |

## 1. Scope

The INI-style store behind `global.ini`, `user.ini`, every profile's `basic.ini`, and many
plugin settings. Files are user data and must round-trip bit-exactly across versions, so the
reader, the writer, and the typed conversions are all normative.

## 2. Interface

```c
typedef struct config_data config_t;                       /* opaque */
#define CONFIG_SUCCESS 0   #define CONFIG_FILENOTFOUND -1   #define CONFIG_ERROR -2
enum config_open_type { CONFIG_OPEN_EXISTING, CONFIG_OPEN_ALWAYS };

config_t *config_create(const char *file);
int  config_open(config_t **config, const char *file, enum config_open_type type);
int  config_open_string(config_t **config, const char *str);
int  config_open_defaults(config_t *config, const char *file);
int  config_save(config_t *config);
int  config_save_safe(config_t *config, const char *temp_ext, const char *backup_ext);
void config_close(config_t *config);
size_t config_num_sections(config_t*);          const char *config_get_section(config_t*, size_t idx);
void config_set_string/int/uint/bool/double(config_t*, const char *section, const char *name, T value);
void config_set_default_string/int/uint/bool/double(...);
const char *config_get_string(...);  int64_t config_get_int(...);  uint64_t config_get_uint(...);
bool config_get_bool(...);           double config_get_double(...);
const char *config_get_default_string(...) ; int64_t/uint64_t/bool/double config_get_default_*(...);
bool config_remove_value(config_t*, const char *section, const char *name);
bool config_has_user_value(...);  bool config_has_default_value(...);
```
Platform helpers the module relies on (specified here only as far as it uses them):
`os_fread_utf8(file, &str)`, `os_dtostr(double, buf, size)`, `os_strtod(str)`,
`os_safe_replace(target, from, backup)`.

## 3. Data model

A config has two ordered maps: **user** values and **defaults**; each is an ordered list of
sections, each an ordered list of items (name → string value). Both lists keep **insertion
order**, and `get` on a duplicated key returns the **most recently inserted** entry (Q-1).
Section and item names are compared byte-wise, case-sensitively. Everything is stored as a
string; typed getters convert on read.

## 4. File format

### 4.1 Reading (`config_open`, `config_open_string`, `config_open_defaults`)

Input bytes (after removing an optional leading UTF-8 BOM `EF BB BF`) are processed as follows.
A *newline character* is `\r` or `\n`.

1. **Top level.** Skip whitespace (space, tab, newline characters). If the next non-white character is not `[`, the rest of that line (up to and including the newline character) is ignored. Otherwise read a **section header**: all characters after `[` up to the next `]` become the section name, **verbatim, including spaces**. If a newline character comes before `]`, the header ends there (the name read so far is used). **If the resulting name is empty (`[]`), parsing stops entirely** and everything after is ignored (R-D3).
2. **Section body.** Lines are read until the next line whose first non-white character is `[` (start of the next section) or end of input. For each line: skip leading whitespace (space, tab); a line starting with `#` is a comment; otherwise the **name** is every character up to the first `=` (trailing whitespace before the `=` **is kept** in the name), and the **value** is every character after the `=` up to the end of the line (leading and trailing whitespace **kept**). A line with no `=` before its end, or whose name would start with `=`, is ignored. A name may contain spaces.
3. **Unescape.** In the value only, `\\` → `\`, `\r` → CR, `\n` → LF. Any other backslash sequence (e.g. `\x`) is kept literally including the backslash. A trailing lone backslash is kept.
4. An empty value is stored as `""`.
5. Line endings: `\n`, `\r\n` and `\r` all end a line.
6. Sections with the same name create separate section records (both listed by `config_num_sections`); lookups find the **later** one (Q-1). The same holds for repeated names inside a section.

`config_open_string(str)` parses `str` exactly as above (no BOM handling needed); the resulting
config has **no file**, so `config_save` fails with `CONFIG_ERROR`.

### 4.2 Writing (`config_save`)

For each user section in order: if it is not the first, write one `\n`; then `[` name `]\n`; then for each item `name=value\n` where value has `\` → `\\`, CR → `\r`, LF → `\n` applied **in that order** (backslashes first). No quoting, no trailing blank line. A NULL/empty value writes `name=`. The file is written in binary mode, truncated first. **Windows only:** the content is preceded by the BOM `EF BB BF`.

## 5. Behaviour

- **S-007-R01** `config_create(file)`: creates/truncates `file` (fails → NULL), returns an empty config bound to `file`.
- **S-007-R02** `config_open(&c, file, EXISTING)`: file missing → `CONFIG_FILENOTFOUND` and `*c = NULL`. `ALWAYS`: creates the file if missing. A file that exists but is empty (or only a BOM) opens with zero sections. `c == NULL` → `CONFIG_ERROR`.
- **S-007-R03** `config_open_defaults(c, file)`: parses `file` into the **defaults** map (adding to what is there; repeated calls accumulate and the later definition of a key wins). Missing file → `CONFIG_FILENOTFOUND`. It does **not** create user values. `c == NULL` → `CONFIG_ERROR`.
- **S-007-R04** `config_save`: `CONFIG_ERROR` if `c` or its file is NULL; `CONFIG_FILENOTFOUND` if the file cannot be opened for writing; `CONFIG_SUCCESS` after writing. **An empty config cannot be saved successfully**: the file is truncated to zero bytes and `CONFIG_ERROR` is returned (the write of a zero-length buffer is treated as a failure) (Q-2).
- **S-007-R05** `config_save_safe(c, temp_ext, backup_ext)`: if `temp_ext` is NULL or empty → log `LOG_ERROR "config_save_safe: invalid temporary extension specified"` and return `CONFIG_ERROR`. The temporary path is `file` + `.` + `temp_ext` (a leading `.` in the extension is not doubled). Write it with `config_save` (failure → log `LOG_ERROR "config_save_safe: failed to write to <temp>"`, return its error, leave the target untouched). If `backup_ext` is non-empty the backup path is `file` + `.` + `backup_ext` (same dot rule). Then `os_safe_replace(file, temp, backup)`: on POSIX, if a backup path is given and the target exists, **rename target → backup** (failure → error), then **rename temp → target**; non-zero → `CONFIG_ERROR`. After success the temp file no longer exists; the backup holds the previous contents.
- **S-007-R06** `config_num_sections` counts user sections; `config_get_section(idx)` returns the idx-th name in insertion order, or NULL if out of range (including after all items are removed — sections are never removed).
- **S-007-R07** Setters create the section (appended) and the item (appended) if absent; otherwise replace the value in place (position unchanged). `set_string(..., NULL)` stores `""`.
- **S-007-R08** Value formats: `set_int` → decimal `%lld`; `set_uint` → decimal `%llu`; `set_bool` → `"true"`/`"false"`; `set_double` → the string left in the buffer by `os_dtostr` (S-023): `printf("%.17g")`, `".0"` appended when the text has neither `.` nor `e`, then an exponent "shortening" step. **That step is defective (S-023 D-2):** it removes the `+` sign and leading zeros of the exponent but does not move the terminator, so the stored text keeps stale characters. Consequences observed in the reference (vector `04-typed-values`): `2` → `"2.0"`, `0.1` → `"0.10000000000000001"`, `-0.0` → `"-0.0"`, but `1e21` → `"1e211"` (read back as 1e211), `0.00001` → `"1.0000000000000001e-55"`, `1.5e300` → corrupt likewise. **Required: reproduce this stored text exactly** (existing user files already contain such values; a "fixed" writer would make old and new builds disagree) — *or* implement the correct text and treat the reference text as the QUIRK; the project decision is recorded as open question 2.
- **S-007-R09** `config_set_default_*` store the default; **if there is no user value for that (section, name) they also create a user value with the same text**. Consequently `has_user_value` becomes true. If a user value exists it is left unchanged. `set_default_string(NULL)` stores `""`. `set_default_int`/`uint` use the same decimal formats; `set_default_bool` the same words; **`set_default_double` stores `printf("%g")` — six significant digits** (`1234567.891` → `"1.23457e+06"`), unlike `set_double` (Q-3).
- **S-007-R10** `config_get_string`: the user value if present, else the default, else NULL. The pointer is valid until the item changes, is removed, or the config is closed. `config_get_default_*` read only the defaults map.
- **S-007-R11** Integer conversion (`get_int`, `get_uint`, defaults): NULL/empty → 0; text starting with the two characters `0x` (lowercase x only) → `strtoll/strtoull(text + 2, base 16)`; otherwise `strtoll/strtoull(text, base 10)`, which skips leading whitespace, accepts a sign, and stops at the first invalid character (`"12abc"` → 12, `"abc"` → 0, `"0X1F"` → 0). Unsigned conversion of a negative text wraps (`"-5"` → 2⁶⁴−5).
- **S-007-R12** `get_bool`: true iff the text equals `true` case-insensitively (ASCII) **or** `get_uint(text)` is non-zero (`"1"`, `"2"`, `"0x1"` true; `"yes"`, `"0"`, `"false"`, `""` false). Missing → false.
- **S-007-R13** `get_double`: `os_strtod(text)`: at most 63 characters considered, locale-independent `strtod` (accepts `inf`, `nan`, hexadecimal floats such as `0x10` → 16); missing → 0.0; unparsable → 0.0.
- **S-007-R14** `config_remove_value`: removes the **user** item (not the default) and returns true; false if the section or item is missing. The section stays (empty).
- **S-007-R15** `has_user_value` / `has_default_value` test presence in the respective map. Missing section/name never faults.
- **S-007-R16** `config_close(NULL)` is a no-op; otherwise frees everything.

## 6. Concurrency

Every operation takes the config's recursive mutex; pointers returned by `get_string` are not protected after the call returns (same as the reference).

## 7. Reference quirks

- **Q-1 (duplicates).** Duplicate section or item names are accepted by the reader and both stay in the list (and are both written by `save`); lookups return the one inserted last. Required: reproduce.
- **Q-2 (empty save).** See R04. Required: reproduce the return value and the truncation.
- **Q-3 (`%g` defaults).** See R09. Required: reproduce (callers store defaults for doubles such as `0.1`).
- **Q-4 (whitespace in names/values).** Names keep trailing whitespace and values keep all surrounding whitespace (vector `10`). Required: reproduce.
- **Q-5 (bad header).** A header missing `]` still creates a section from the text read (`"[A\n"` → section `A`). Required: reproduce.
- **Q-6 (hex prefix).** Only lowercase `0x` is recognised.

## 8. Implementation notes (non-normative)

- Keep sections/items in `Vec`s with a lookup that scans from the **end** (gives the "last wins" rule without a hash map), or use an index map that overwrites on insert while the list keeps all entries.
- The reader needs only a character scanner; the reference uses the generic base-token lexer, whose token boundaries never change the rules above.
- `os_dtostr` / `os_strtod` should live in the platform crate (S-0xx) and be reused by `obs_data`.
- Atomic replace on Windows uses a different primitive (`ReplaceFile`-style); semantics (R05) are the same.

## 9. Conformance coverage

| Rule | Scripts |
|---|---|
| §4.1, R02, R06 | `01-parse-basic`, `02-parse-edge-cases`, `09-open-file`, `10-whitespace-canonical`, `11-line-endings-and-escapes` |
| §4.2, R04 | `03-save-format`, `04-typed-values`, `07-remove-and-sections` |
| R05 | `08-save-safe-and-files` |
| R07, R08 | `03`, `04` |
| R09, R10, R03 | `06-defaults`, `09-open-file` |
| R11–R13 | `04`, `05-value-conversions` |
| R14, R15 | `07-remove-and-sections`, `06` |
| Q-1…Q-6 | `02`, `03`, `05`, `06`, `10` |

## 10. Open questions

2. Whether the Rust `set_double` should reproduce the corrupt exponent text (compatibility with files written by the C build) or write correct text. The reference text for exponent doubles is wrong (S-023 D-2); the vectors currently record the reference.

- Behaviour when the file contains NUL bytes or invalid UTF-8 is not characterised (the reader treats data as bytes; invalid sequences are passed through).
- Windows file-replacement details beyond R05 are in the platform spec.
