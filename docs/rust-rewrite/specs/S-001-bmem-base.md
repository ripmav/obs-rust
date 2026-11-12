# S-001 Allocator, logging and crash base (`bmem`, `base`)

| | |
|---|---|
| Status | Ready for review (second-author check pending) |
| Reference | libobs 33.0.1 `util/bmem.*`, `util/base.*` |
| Target crate | `crates/obs-util` (modules `bmem`, `base`) |
| Depends on | – |
| Conformance | `test/conformance/scripts/bmem/` |

## 1. Scope

The process-wide allocator used by libobs and by every plugin, plus the logging and
fatal-error ("crash") hooks. Everything else in libobs allocates through this layer, and
plugins free memory that libobs allocated (and vice versa), so **this component defines
which pointers may be passed to which function**.

## 2. Conventions

`ALIGNMENT` = 32. Platform differences are noted where they exist; unless stated, the
committed vectors describe the POSIX reference.

## 3. Interface

```c
void *bmalloc(size_t size);
void *brealloc(void *ptr, size_t size);
void  bfree(void *ptr);
int   base_get_alignment(void);          /* returns 32 */
long  bnum_allocs(void);
void *bmemdup(const void *ptr, size_t size);

/* header-inline helpers (NOT exported; they live in the C header and are compiled into plugins;
   their behaviour is part of the contract because the Rust side must provide the same ones
   for Rust callers) */
void    *bzalloc(size_t size);
char    *bstrdup(const char *str);
char    *bstrdup_n(const char *str, size_t n);
wchar_t *bwstrdup(const wchar_t *str);
wchar_t *bwstrdup_n(const wchar_t *str, size_t n);

enum { LOG_ERROR = 100, LOG_WARNING = 200, LOG_INFO = 300, LOG_DEBUG = 400 };
typedef void (*log_handler_t)(int lvl, const char *msg, va_list args, void *p);

void base_get_log_handler(log_handler_t *handler, void **param);
void base_set_log_handler(log_handler_t handler, void *param);
void base_set_crash_handler(void (*handler)(const char *, va_list, void *), void *param);
void blogva(int log_level, const char *format, va_list args);
void blog(int log_level, const char *format, ...);       /* printf-style */
void bcrash(const char *format, ...);                    /* never returns */
```

`blog`, `bcrash` are variadic and the handlers receive a `va_list`: a Rust crate cannot
implement these entry points in stable Rust; the Rust crate MUST provide them through a
small C shim (see section 7) while exposing a non-variadic Rust API for Rust callers.

There is **no exported function to replace the allocator**: `struct base_allocator`
exists in the header but nothing installs one. (An earlier planning document mentioned
`base_set_allocator`; it does not exist.)

## 4. Behaviour

### 4.1 Allocation

- **S-001-R01** `bmalloc(size)` with `size > 0` returns a pointer `p` with `(uintptr_t)p % 32 == 0`, to at least `size` writable bytes. Contents are unspecified (not zeroed).
- **S-001-R02** `bmalloc(0)` is a fatal error: first the debug hook `os_breakpoint()` is invoked (on Linux this raises `SIGTRAP`; under a debugger it breaks, otherwise the default action terminates the process *before* `bcrash` runs unless a handler is installed — see S-023; MAY be a no-op in a Rust port), then `bcrash("bmalloc: Allocating 0 bytes is broken behavior, please fix your code!")`. The text is MUST (it is part of crash reports).
- **S-001-R03** If the underlying allocation fails, `os_oom()` is invoked (hook; MAY be a no-op) and `bcrash("Out of memory while trying to allocate %lu bytes", (unsigned long)size)` follows. `bmalloc` therefore never returns NULL.
- **S-001-R04** Every successful `bmalloc` increments the live-allocation counter by one.
- **S-001-R05** `bfree(NULL)` has no effect. `bfree(p)` for `p != NULL` decrements the counter by one and releases `p`. Passing a pointer not obtained from `bmalloc`/`brealloc`/`bmemdup`/`bzalloc`/`bstrdup*` is undefined.
- **S-001-R06** `brealloc(NULL, size)` behaves like `bmalloc(size)` for the returned pointer and **increments the counter** (it increments *before* the zero-size check of R07).
- **S-001-R07** `brealloc(p, 0)` — including `p == NULL` — is fatal with message `"brealloc: Allocating 0 bytes is broken behavior, please fix your code!"` (hook `os_breakpoint()` first).
- **S-001-R08** `brealloc(p, size)` with `p != NULL` and `size > 0` returns a 32-byte-aligned pointer whose first `min(old_size, size)` bytes equal the old contents; the counter is **unchanged**. If it fails: R03 text. (The old pointer is invalid after success.)
- **S-001-R09** `bnum_allocs()` returns the counter (number of live blocks as defined by R04–R08). It is an atomic quantity; reads need not be synchronised with concurrent updates.
- **S-001-R10** `base_get_alignment()` returns 32 on every platform.
- **S-001-R11** `bmemdup(p, size)` = `bmalloc(size)` then, if `size != 0`, copy `size` bytes from `p`. Consequently `bmemdup(p, 0)` is fatal (R02 message with `bmalloc`).
- **S-001-R12** `bzalloc(size)` = `bmalloc(size)` followed by zero-filling `size` bytes; `bzalloc(0)` is fatal as `bmalloc(0)`.
- **S-001-R13** `bstrdup_n(str, n)`: if `str == NULL` returns NULL; otherwise `d = bmemdup(str, n + 1)`, sets `d[n] = 0`, returns `d`. It reads `n + 1` bytes from `str` (the caller guarantees `n <= strlen(str)`). `bstrdup(str)`: NULL → NULL, else `bstrdup_n(str, strlen(str))`. The wide variants are identical with `wchar_t` units and `(n + 1) * sizeof(wchar_t)` bytes.

### 4.2 Logging

- **S-001-R20** Level constants have the values in section 3. Lower numbers are more severe.
- **S-001-R21** `blog(level, fmt, ...)` ≡ `blogva(level, fmt, ap)`; `blogva` calls the installed handler `handler(level, fmt, ap, param)` with the **unformatted** format string and the argument list. Formatting is the handler's job.
- **S-001-R22** Initially the handler is the *default handler*. `base_set_log_handler(h, p)` installs `h` and `p`; **`h == NULL` reinstalls the default handler** (and stores `p`). `base_get_log_handler(&h, &p)` stores the current handler and parameter into the non-NULL out pointers; either may be NULL.
- **S-001-R23** Default handler: formats with `vsnprintf` into a 8192-byte buffer (longer messages are truncated), then writes one line, flushing after the write:
  - `LOG_DEBUG`  → stdout `"debug: <msg>\n"`
  - `LOG_INFO`   → stdout `"info: <msg>\n"`
  - `LOG_WARNING`→ stdout `"warning: <msg>\n"`
  - `LOG_ERROR`  → stderr `"error: <msg>\n"`
  - any other level value → nothing is written.
- **S-001-R24** `bcrash(fmt, ...)`: if a crash is already in progress (a crash handler itself called `bcrash`) write `"Crashed in the crash handler"` to stderr and `exit(2)`. Otherwise mark crashing, call the crash handler `h(fmt, ap, param)`, and — if the handler returns — `exit(0)`. The default crash handler writes the formatted message to stderr **without a trailing newline** and calls `exit(0)`. `bcrash` never returns.
- **S-001-R25** `base_set_crash_handler(h, p)` can succeed **at most once per process**: the first call stores `h`/`p`; every later call logs `LOG_WARNING "Tried to set a crash handler when one already exists."` and is ignored. (The flag is global and atomic; the default handler does not count as "set".)

### 4.3 Platform notes

- Windows uses `_aligned_malloc/_aligned_realloc/_aligned_free(…, 32)`; POSIX uses an alignment shim over `malloc` (a 1-byte offset header placed before the returned pointer). **How alignment is achieved is implementation-defined**; observable requirements are R01, R08, R10 and that `realloc` preserves alignment (non-trivial with plain `posix_memalign`, which has no aligned realloc — implement realloc as allocate-copy-free if necessary).
- The `os_breakpoint`/`os_oom` hooks come from the platform layer (S-0xx, not yet specified); a conforming crate may omit them.

## 5. Concurrency

All functions in section 3 are thread-safe, except that `base_set_log_handler`, `base_set_crash_handler` and reads of the handler are not synchronised with each other (handlers are installed during start-up). The counter is updated atomically.

## 6. Reference defects and quirks

- **Q-1** `bnum_allocs()` reads a plain `long` that other threads update atomically; implementations SHOULD use an atomic.
- **Q-2** `bcrash`'s "crashing" flag is not atomic; an implementation MAY use an atomic.
- **Q-3** A default log handler silently drops messages whose level is not one of the four constants.

## 7. Implementation notes (non-normative)

- Allocation: `std::alloc::dealloc` needs the original `Layout`, but `bfree(ptr)` receives only the pointer, so plain `std::alloc` with `Layout::from_size_align(size, 32)` is not enough unless you store the size in a header. Simplest approach: `libc::malloc(size + 32)`, round the pointer up to a multiple of 32 (leaving at least one byte before it), and store the offset in the byte immediately before the returned pointer; `bfree` and `brealloc` recover the original block from that byte. Any scheme works if `bfree`/`brealloc` can recover the block from the user pointer alone.
- Variadics: a ~30-line C file calling `vsnprintf` into a buffer and then calling `obs_ffi_log(level, const char *msg)` covers `blog`/`blogva`/`bcrash`; the log handler API (which hands `va_list` to the handler) must remain C-callable, so the dispatch to a *registered C handler* happens in the shim. Rust code calls `obs_ffi_log`.
- Keep the counter in a `static AtomicIsize`; `bnum_allocs()` returns it as `long`.

## 8. Conformance coverage

| Rule | Scripts |
|---|---|
| R01, R04, R05, R09, R10 | `bmem/01-alignment-and-count` |
| R06, R08 | `bmem/02-realloc` |
| R11, R13 | `bmem/03-dup` |
| R02, R03(partially: OOM not scripted), R07, R11 (size 0), R12 (size 0) | `bmem/04-fatal-paths` |
| R21 (format + handler dispatch) | `bmem/05-logging` |
| R20, R22–R25 | not scriptable with the current runner; unit tests required in the crate (default-handler output, `base_set_log_handler(NULL)`, crash handler once-only) |

## 9. Open questions

- None known. (OOM paths are not exercised by vectors.)

## 10. Round 9: core glue — allocation wrappers, base log/crash handlers, C++ RAII (added)

### Reference files (all cited)

- `libobs/util/bmem.c`, `libobs/util/bmem.h`, `libobs/util/base.c`, `libobs/util/util.hpp`

- **S-001-R26** `bmem.c`: the allocation functions pinned earlier (`bmalloc`/`brealloc`/`bfree`/`bmemdup`) route through private `a_malloc`/`a_realloc`/`a_free` wrappers which maintain a process-wide allocation counter (`num_allocs`); the earlier OOM rules apply unchanged.
- **S-001-R27** `bmem.h`: the zeroing/string-dup inlines over `bmalloc`: `bzalloc` (zeroed memory), `bstrdup`/`bstrdup_n` and the wide variants `bwstrdup`/`bwstrdup_n` (`_n` = length-bounded).
- **S-001-R28** `base.c`: `blog(level, fmt, …)` dispatches through the current log handler, swapped by `base_get_log_handler`/`base_set_log_handler`; the default `def_log_handler` formats into an 8192-byte buffer (`vsnprintf`) and prints to stdout with a per-level prefix (`"debug: "`, `"info: "`, …). `bcrash(msg)` calls the current crash handler (default in the same file; swapped by `base_set_crash_handler`).
- **S-001-R29** `util.hpp`: the C++ convenience wrappers: `BPtr<T>` — move-only RAII pointer freeing via `bfree` (`Get`, `operator T*`, `operator&`, `operator!`, comparisons); `ConfigFile` — move-only RAII over S-007's `config_t` (copy deleted; `Create` → `config_create`, `Close`/destructor → `config_close`, plus `Swap`, `Open`/`OpenString`, `GetString`) and a `TextLookup` accessor over S-010's text lookup.

