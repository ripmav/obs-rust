# S-014 `obs-data` (settings objects and their JSON form)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `obs-data.c`, `obs-data.h`; JSON text is produced and parsed by jansson 2.15.1 (behaviour of jansson that matters is specified in §6, because a Rust port cannot link it); file helpers `os_quick_read_utf8_file`, `os_quick_write_utf8_file(_safe)`, `os_rename`, `os_file_exists` |
| Target crate | `crates/obs-data` |
| Depends on | S-001 (allocation), S-006 (`dstr`, only for path building), S-008 (`vec2/3/4`, `quat` field layout), platform file helpers (§3.4) |
| Conformance | `test/conformance/scripts/obsdata/` (20 scripts), runner language in `test/conformance/README.md` ("obsdata runner") |

## 1. Scope

`obs_data_t` is the settings object of every source, filter, encoder, output and service. It is a
string-keyed, ordered set of typed *items*; each item can carry three independent values
(*layers*: user, default, autoselect). Scene collections, profiles' service/encoder settings and
many plugin files are the JSON serialisation of these objects, so the **JSON reader and writer,
the key order and the number typing are all user-visible file format**.

Included: the data model, every `obs_data_*`, `obs_data_item_*` and `obs_data_array_*` function,
the vec/quat and frames-per-second helpers, JSON read/write (including jansson behaviour), file
helpers. Excluded: `obs_properties` (S-0xx), source settings handling.

## 2. Terminology and conventions

RFC 2119 words. Committed vectors are for a 64-bit little-endian target. "NULL" is the absent
pointer; in a Rust API it is `Option::None`. *Object* = `obs_data_t`, *array* = `obs_data_array_t`,
*item* = `obs_data_item_t`. Classes: CORE (must match), QUIRK (SHOULD match; deviation documented),
DEFECT (reference crashes or corrupts memory; required behaviour stated).

## 3. Interface

### 3.1 Types

```c
enum obs_data_type        { OBS_DATA_NULL=0, OBS_DATA_STRING=1, OBS_DATA_NUMBER=2,
                            OBS_DATA_BOOLEAN=3, OBS_DATA_OBJECT=4, OBS_DATA_ARRAY=5 };
enum obs_data_number_type { OBS_DATA_NUM_INVALID=0, OBS_DATA_NUM_INT=1, OBS_DATA_NUM_DOUBLE=2 };
struct media_frames_per_second { uint32_t numerator, denominator; };   /* both non-zero = valid */
```
`obs_data_t`, `obs_data_item_t`, `obs_data_array_t` are opaque, reference counted (§5.1).

### 3.2 Functions

All names below are exported with C linkage and the signatures of `obs-data.h`; `D` is an
`obs_data_t *`, `name` a `const char *` (byte string, never NULL, see D2).

- Lifetime: `obs_data_create()`, `obs_data_addref(D)`, `obs_data_release(D)`, inline
  `obs_data_newref(D)` (addref and return `D`, or create a new object if `D` is NULL).
- JSON/text: `obs_data_create_from_json(str)`, `…_from_json_file(path)`,
  `…_from_json_file_safe(path, backup_ext)`, `obs_data_get_json(D)`, `…_with_defaults`, `…_pretty`,
  `…_pretty_with_defaults`, `obs_data_get_last_json(D)`, `obs_data_save_json(D, path)`,
  `…_save_json_safe(D, path, temp_ext, backup_ext)`, `…_save_json_pretty_safe(…)`.
- Whole-object: `obs_data_apply(target, src)`, `obs_data_erase(D, name)`, `obs_data_clear(D)`,
  `obs_data_get_defaults(D)`.
- Per-name setters for each layer L in {user (no infix), `default_`, `autoselect_`} and each kind K in
  {`string` (const char*), `int` (long long), `double`, `bool`, `obj` (obs_data_t*), `array`
  (obs_data_array_t*)}: `obs_data_set_[L_]K(D, name, value)`; getters `obs_data_get_[L_]K(D, name)`.
  Also `has_user_value`, `has_default_value`, `has_autoselect_value`, `unset_user_value`,
  `unset_default_value`, `unset_autoselect_value` (all `(D, name)`).
- Vec helpers `obs_data_set_[L_]{vec2,vec3,vec4,quat}(D, name, const struct *)` and
  `obs_data_get_[L_]{…}(D, name, struct *)`; frames-per-second helpers
  `obs_data_set_[L_]frames_per_second(D, name, fps, option)` and
  `bool obs_data_get_[L_]frames_per_second(D, name, fps *, const char **option)`.
- Arrays: `obs_data_array_create/addref/release/count/item/push_back/insert/push_back_array/erase/enum`.
- Items: `obs_data_first(D)`, `obs_data_item_byname(D, name)`, `bool obs_data_item_next(item **)`,
  `obs_data_item_release(item **)`, `obs_data_item_remove(item **)`, `obs_data_item_gettype`,
  `obs_data_item_numtype`, `obs_data_item_get_name`, `obs_data_item_has_{user,default,autoselect}_value`,
  `obs_data_item_unset_{user,default,autoselect}_value`, `obs_data_item_set_[L_]K(item **, value)`,
  `obs_data_item_get_[L_]K(item)`, `obs_data_item_set_[L_]frames_per_second(item **, …)`,
  `obs_data_item_get_[L_]frames_per_second(item, …)`.

The `autoselect` family is marked deprecated in the header; it is still part of the contract.

### 3.3 Item-handle contract

`obs_data_first`, `obs_data_item_byname` and `obs_data_item_next` return/produce an **item handle that
owns one reference to the item**; it must be released with `obs_data_item_release`. An item handle
is a view of the item in the object it came from; it stays usable after the item is erased from the
object (§5.8).

### 3.4 Platform file helpers (specified only as far as used)

- `os_quick_read_utf8_file(path)`: NULL if the file cannot be opened. Otherwise the file content as a
  string with a leading UTF-8 BOM (`EF BB BF`) removed; **NULL if the file is empty or contains only a
  BOM**. (A NUL byte in the file ends the C string early.)
- `os_quick_write_utf8_file(path, str, len, marker=false)`: create/truncate (binary), write `len`
  bytes, no BOM. false if the file cannot be created.
- `os_quick_write_utf8_file_safe(path, str, len, false, temp_ext, backup_ext)`: if `temp_ext` is NULL or
  empty: log ERROR, return false. temp path = `path` + `.` + `temp_ext`, backup path = `path` + `.` +
  `backup_ext` (a leading `.` in the extension is not doubled; empty/NULL `backup_ext` = no backup).
  Write the temp file (failure: log ERROR, false); then `os_safe_replace(path, temp, backup)`:
  if a backup path is given and `path` exists, rename `path` to the backup (failure aborts); then rename
  temp to `path`; success iff both are 0.
- `os_rename(old, new)`: POSIX `rename` (overwrites an existing target).
- `os_file_exists(path)`.

## 4. Data model

### 4.1 Items and layers

An object is an ordered list of items, at most one per name (byte-exact, case-sensitive, any length
including the empty name). An item has: a name; a **type** (string, number, boolean, object, array);
and up to three **layers** — *user*, *default*, *autoselect* — each either absent or holding a value
of the item's type. A **number** value is either an **int** (`i64`) or a **double** (`f64`); the
int/double *number type* is part of each stored number value, not of the item.
An **object value** and an **array value** may be NULL (absent object) — a NULL value is still a
present layer (§5.4). An **array** holds zero or more objects (never NULL).

An item with no layer present (a *shell*) can exist; it keeps its name and type (§5.5).

### 4.2 Effective value

`effective(item)` = the user layer if present, else the default layer if present, else the
autoselect layer if present, else *none*. Plain getters (`get_K`, `item_get_K`) return the typed
reading of the effective value (or the type's zero when none, §5.3). `default`/`autoselect` getters
read only their own layer.

### 4.3 Reference counts

Every object, array and item has a reference count that is observable through lifetime only (there
is no API to read it; the conformance runner peeks into the reference memory — an implementation using
`Rc`/`Arc` reports `strong_count`). Counting rules are in §5.1.

## 5. Behaviour

### 5.1 Reference counting (CORE)

- **S-014-R01** `create` returns an object with count 1. `addref(NULL)` and `release(NULL)` are
  no-ops. When `release` drops the count to 0 the object is destroyed: it releases one reference on
  every object/array value held in any of its layers (user, default, autoselect).
  The same holds for arrays (they release their elements) and items.
- **S-014-R02** Every getter that returns an object or array — `get_obj`, `get_default_obj`,
  `get_autoselect_obj`, `get_array`…, `item_get_*_obj/array`, `obs_data_array_item` — returns
  **a new reference** (count + 1) or NULL; the caller must release it. The same object is returned
  every time (identity, not a copy).
- **S-014-R03** Every setter of an object/array value (`set_obj`, `set_default_obj`,
  `set_autoselect_obj`, array variants, item variants) **takes its own reference** (count + 1); the
  caller keeps and releases its own. Replacing a layer's value releases the reference of the old value.
  Storing NULL stores a present NULL value and releases the old one.
- **S-014-R04** `erase`, `clear` (user layers only), and `unset_*` release the references of the values they
  drop. `obs_data_array_push_back/insert/push_back_array` take one reference per element added;
  `obs_data_array_erase` releases the element.
- **S-014-R05** Vector helpers and `frames_per_second` setters create a fresh sub-object, store it with
  the usual setter (R03) and drop their own reference; the net count of the sub-object is 1.

Exception: DEFECT D1 (§8) for default object/array.

### 5.2 Setters

- **S-014-R10** `set_K(D, name, v)` (user layer): if `D` is NULL: no-op. If no item has `name`, a new item
  is **appended** at the end of the list with that type and only that layer. Otherwise the item's
  **type becomes K** and its user layer is replaced (the old user value's references are released).
  `set_string(…, NULL)` stores the empty string. `int` and `double` store a value with the
  respective number type; `bool` stores a boolean.
- **S-014-R11** `set_default_K(D, name, v)`: NULL `D`: no-op. No item: a new item is appended holding only a
  default layer. Existing item whose type differs from K: **ignored, no change** (including not
  creating anything). Same type: the default layer is replaced.
- **S-014-R12** `set_autoselect_K(D, name, v)`: like R10 for the autoselect layer: it creates the item if
  missing and **overwrites the item's type** without checking (see D3 for mixed types).
- **S-014-R13** The item-level setters `obs_data_item_set_[L_]K(&item, v)` have exactly the semantics of the
  name-based setters applied to the item the handle refers to (R10–R12, including R11's type check),
  except that no item is ever created. A NULL item pointer or NULL `*item` is a no-op. If the item has to
  grow (§6.1) the handle follows the item; it is still the same item and now last in the order.

### 5.3 Getters

- **S-014-R20** `get_K(D, name)` (user-or-fallback getter) uses `effective(item)`. A getter returns the **zero
  value of K** when: `D` is NULL, the item does not exist, the item's type differs from K, or there is
  no effective value. Zero values: `""` (a valid empty C string, never NULL), `0`, `0.0`, `false`, NULL
  object, NULL array.
- **S-014-R21** `get_default_K` reads only the default layer, `get_autoselect_K` only the autoselect layer;
  absent layer or type mismatch gives the zero value.
- **S-014-R22** Number conversion: `get_int` of a double value truncates toward zero (`-2.9` → `-2`,
  `3000000000.75` → `3000000000`); `get_double` of an int converts (`9007199254740993` →
  `9007199254740992.0`, round to nearest). Converting a double outside the `i64` range or NaN to int
  is undefined in the reference (x86-64 yields `i64::MIN`); an implementation SHOULD saturate and
  scripts do not depend on it.
- **S-014-R23** `get_string` for a string item returns the stored bytes; the pointer is valid until the
  value is replaced or the object destroyed.
- **S-014-R24** `has_user_value(D, name)` / `has_default_value` / `has_autoselect_value` are true iff the
  object is non-NULL, the item exists and the layer is present (a present NULL object value counts).
  The item forms test the handle's item; NULL item gives false.
- **S-014-R25** `obs_data_item_gettype(item)` is the item's type; NULL item → `OBS_DATA_NULL`.
  `obs_data_item_numtype` is `INVALID` for NULL, for non-number items and for number items without any
  layer; otherwise the number type (`INT`/`DOUBLE`) of the **effective** value.
  `obs_data_item_get_name` returns the name (NULL for a NULL item).

### 5.4 NULL objects and arrays

- **S-014-R26** `set_obj(D, n, NULL)` creates/overwrites a user layer holding NULL: `has_user_value` is true,
  `get_obj` returns NULL, JSON writes `null` (§6.3). `set_array(D, n, NULL)` likewise; the JSON form is `[]`.
  The plain getter does **not** fall back to a default when a user NULL value exists.

### 5.5 Unset, erase, clear

- **S-014-R30** `unset_user_value/unset_default_value/unset_autoselect_value` (and item forms) remove just that
  layer (releasing references). Missing item, absent layer or NULL object: no-op. The item stays in the
  list even if no layer is left (shell), keeping its type and position; a shell reads as zero values.
- **S-014-R31** `obs_data_erase(D, name)` removes the item (missing item or NULL `D`: no-op). Handles to it stay
  valid (§5.8). `obs_data_clear(D)` removes the **user** layer of every item and keeps the items (with their
  default/autoselect layers, or as shells); NULL `D`: no-op.

### 5.6 `obs_data_apply`

- **S-014-R32** `apply(target, src)`: no-op if either is NULL or they are the same object. Otherwise, for
  each item of `src` in list order that has a **user layer**, with that user value `v`:
  string/number/bool: `set_K(target, name, v)` (the number keeps its int/double type);
  object: if `v` is NULL, **skip**; else `set_obj(target, name, deepcopy(v))`;
  array: if `v` is NULL, skip; else `set_array(target, name, a)` where `a` has, for each element `e`,
  a *new object* `deepcopy(e)` in the same order. `deepcopy(x)` = new empty object, then `apply(new, x)` —
  so only user layers are copied recursively, defaults/autoselect values and NULL object members
  are dropped, and nothing is shared with the source. An existing same-named target value is
  replaced, never merged. Default and autoselect layers of `src` are not copied. The item type of
  the target follows R10.

### 5.7 `obs_data_get_defaults`

- **S-014-R33** Returns a new object (count 1). For NULL `D` it is empty. Otherwise, for each item of `D` in list
  order, one **user** value is set on the result (in the same order): string → `get_default_string`;
  bool → `get_default_bool`; number → by the item's *effective* number type: DOUBLE → `get_default_double`,
  INT → `get_default_int`, INVALID (shell) → **no member is created**; object → a new object that is
  `get_defaults(default object)` (an empty object if there is no default object); array → a new array
  holding `get_defaults(e)` for every element `e` of the default array (empty array if none). Missing default layers read as zero values, so
  items without a default appear with `""`/`0`/`false`.

### 5.8 Iteration

- **S-014-R40** `first(D)` returns a handle to the first item (list order) with +1 reference, NULL when `D` is NULL or empty.
  `byname(D, name)` returns +1 reference to the item or NULL. `next(&it)`: if `it` is NULL (or the pointer
  is NULL) returns false without effect. Otherwise `it` becomes the successor, with one reference acquired
  on it, and the reference on the old item is released; returns true iff a successor exists (when false,
  `*it` is NULL).
- **S-014-R41** Order is the list order of §6.1. Items **appended during a walk are visited**.
  Erasing (or `item_remove`) an item, including the one a handle points to, detaches it; a handle
  to a detached item remains valid (its reference count drops by one) and `next` from it yields the item that
  followed it **at the time it was detached** (the walk continues). `item_remove(&it)` detaches the item and
  releases the handle's reference; `*it` is NULL afterwards (it also works for a NULL handle: no-op).
- **S-014-R42** An item handle's reference count = 1 (held by its object while attached) + one per live handle.
  A detached item is destroyed with its last handle (its values' references are released then).
- **S-014-R43** `item_release(&it)`: decrements; at zero destroys; sets `*it = NULL` only on destruction in the
  reference, so callers always treat the handle as dead after release (a port clears it always).
- Growing an item that is held by **more than one** handle and then using the other handles is undefined in the
  reference (D4); do not rely on it.

### 5.9 Arrays

- **S-014-R50** `array_create()` count 1, empty. `count(NULL)` = 0.
- **S-014-R51** `push_back(array, obj)` appends, takes a reference and returns the index of the new element;
  if `array` or `obj` is NULL it returns `0` and does nothing. `insert(array, idx, obj)`: NULL `array` or `obj` →
  no-op; otherwise inserts at `idx` (`0 ≤ idx ≤ count`; `idx = count` appends); idx > count is outside the contract.
- **S-014-R52** `item(array, idx)` returns +1 reference to the element, or NULL when `array` is NULL or `idx ≥ count`.
  `erase(array, idx)` removes element `idx` and releases it (`idx < count` is a precondition; NULL array: no-op).
  `push_back_array(a, b)`: for each element of `b` in order, a reference is added and it is appended to `a`;
  NULL `a` or `b`: no-op; `a == b` is outside the contract.
- **S-014-R53** `enum(array, cb, param)` calls `cb(element, param)` for each element in order **without** adding
  references; no-op if `array` or `cb` is NULL.
- Elements are shared (`item` returns the same object): changing it changes the array's content.

### 5.10 Vec / quat helpers

- **S-014-R60** `set_vec2(D, n, v)` stores (via the corresponding layer setter) a **new object value** with
  **double** members `x`, `y` (in this order); vec3 `x,y,z`; vec4 and quat `x,y,z,w`. Floats widen exactly to doubles.
- **S-014-R61** `get_vec2/3/4/quat` read the object value of the layer (user-or-fallback for the plain getter,
  own layer for `default_`/`autoselect_`), and for each member `get_double(member)` narrowed to `f32` (round to
  nearest). **If there is no object value (missing item, other type, NULL object) the output struct is not
  touched.** Missing members read as `0`; int members convert.

### 5.11 Frames-per-second helpers

- **S-014-R65** `set_frames_per_second(D, n, fps, option)` stores a new object value: if `option` is NULL,
  `{numerator: double, denominator: double}` (in this order, as **doubles**); otherwise
  `{option: string}` (fps ignored). Layer variants exist; the item variants take the item.
- **S-014-R66** `get_frames_per_second(D, n, fps*, option**)`, with `obj` the object value of the layer (NULL ⇒
  return false, outputs untouched): if `option` is non-NULL and `obj` has a member `option` (any type: its
  `get_string`, `""` for a non-string), then `*option` receives that string pointer and the result is **true**
  (`fps` untouched). Else if `fps` is NULL: false. Else if `numerator` or `denominator` is missing as a member: false,
  untouched. Else numerator and denominator are read with `get_int` (truncating doubles), **clamped to
  `[0, 4294967295]`**, stored into `*fps`, and the result is `fps.numerator != 0 && fps.denominator != 0`
  (the clamped values are stored even when the result is false). The member-existence check does not look
  at the member type.

## 6. Algorithms

### 6.1 List order and storage growth (QUIRK Q1)

The item list is in insertion order **except** that an item moves to the end of the list whenever its
(modelled) storage must grow. This changes JSON key order and iteration order, which is visible in saved
files. A conforming implementation SHOULD reproduce it; the model below is exact for 64-bit builds.

Per item keep six integers: `ds` (user size), `dl` (user region), `fs` (default size), `fl` (default region),
`as` (autoselect size), `cap` (capacity). Layer sizes (`n`): string = byte length + 1; number = 16; boolean = 1;
object or array = 8. `A(n)` = `n` rounded up to a multiple of 32. All start at 0.

```text
ensure():  need = dl + fl + as
           if cap < need { cap = need; move this item to the end of the list }

create item with layer L of size n:   cap = n;  (user: ds = dl = n) (default: fs = fl = n) (autoselect: as = n)
set user n:      ds = n;  dl = (fs > 0 or as > 0) ? A(n) : n;                                  ensure()
set default n:   fs = n;  fl = (as > 0) ? A(n) : n;  dl = (ds > 0) ? A(ds) : 0;               ensure()
set autoselect n: as = n; dl = (ds > 0) ? A(ds) : 0;  fl = (fs > 0) ? A(fs) : 0;              ensure()
unset user / clear:   ds = 0; dl = 0          (clear: only when dl > 0 — same effect)
unset default:        fs = 0; fl = 0
unset autoselect:     as = 0
```
`cap` never decreases; the item's name length does not matter. A `set` that changes nothing in
size (e.g. int over int with no other layer) never moves the item; replacing a string by a longer one,
adding a default to an item with a user value (the user region becomes aligned to 32) or the reverse
order, move it. A default that is ignored by R11 changes nothing. `apply` is a sequence of sets and
follows the same model; `erase` followed by `set` re-creates the item at the end.

### 6.2 JSON writer

`json(D, pretty, with_defaults)` (NULL `D` → NULL; the returned buffer replaces the previous one stored in `D`
(`get_last_json` returns the most recently produced buffer, or NULL if none was produced; the buffer is freed with the object):

```text
write_obj(D, pretty, defaults): for each item in list order:
    if not defaults and not has_user(item): skip
    value = effective(item)                        # user, else default, else autoselect (§4.2)
    match item.type:
      string  -> JSON string of get_string(item)   # "" if none
      number  -> numtype = numtype(item)           # INVALID (shell) is treated as DOUBLE with value 0.0
                 INT -> JSON integer get_int; else JSON real get_double
      bool    -> true / false
      object  -> NULL object -> `null`, else write_obj(object, pretty, defaults)   # same flags recursively
      array   -> `[` write_obj(element) for each, `]`    # NULL/no array = `[]`
    **omit the member entirely** (no error) if:
          the key is not valid UTF-8, or a string value is not valid UTF-8, or the real is NaN or ±infinity
```
Valid UTF-8 means well-formed per RFC 3629: no overlongs (`C0`,`C1`, `E0 80..9F`, `F0 80..8F`), no surrogates
(`ED A0..BF`), nothing above U+10FFFF (`F4 90..` and `F5..`).

**Layout.** Compact: no whitespace at all (`{"a":1,"b":[{"c":2}]}`). Pretty: an object is `{`, newline, members each on its line indented by
`4 × depth` spaces as `"key": value` separated by `,` + newline, newline, indent of the parent, `}`; arrays likewise
with elements on lines; **empty object `{}`, empty array `[]`**; no trailing newline; top-level empty object is `{}`.
Pretty and compact output contain the same tokens.

**Strings.** Output `"` + escapes + `"`. Escapes: `"` → `\"`, `\` → `\\`, `\b` (08) `\f` (0C) `\n` `\r` `\t`; every other byte `< 0x20` →
`\u00XX` with **upper-case** hex digits; everything else (including `/`, `0x7F`, U+2028/2029 and all non-ASCII)
is written raw as UTF-8.

**Integers**: decimal, `-` for negatives, no `+`. `i64::MIN` allowed.

**Reals** (always distinguishable from integers): let `digits` be the shortest decimal digit string `d1…dn`
that round-trips the double (as produced by shortest-round-trip printing, e.g. Rust `{:e}`), `x` its decimal exponent
(`value = d1.d2…dn × 10^x`).
- `0` → `0.0`, `-0` → `-0.0`.
- If `-4 ≤ x < 16`, positional: for `x ≥ 0` the integer part is the first `x+1` digits (padded with zeros), then `.`, then the remaining
  digits or `0` if none; for `x < 0` it is `0.` followed by `-x-1` zeros and all digits. (`100.0`, `1000000000000000.0`, `0.0001`, `0.30000000000000004`, `1234567.125`.)
- Otherwise scientific: `d1`, then `.` + `d2…dn` only when `n > 1`, then `e`, then `x` in decimal with `-` if negative, **no `+`, no leading zeros**
  (`1e16`, `1e21`, `1.5e300`, `1e-5`, `5e-324`, `1.2345678901234568e17`, `1.7976931348623157e308`).
  Negative values get a leading `-`.

### 6.3 JSON reader (`create_from_json`)

Input: C string `s`. If `s` is NULL: failure (error logged). The text is parsed as **one JSON text** with these rules (these are
jansson's, which are RFC 8259 with the following specifics):

1. Whitespace = space, `\t`, `\n`, `\r` only. A UTF-8 BOM is **not** skipped (it is an error), neither are comments.
2. The top-level value must be an **object or an array** (`5`, `"x"`, `true`, `null`, empty text: error). Any non-whitespace after the value is an error.
3. Object keys must be strings; **duplicate keys within one object are an error** (byte-wise comparison, at any depth,
   also inside arrays and inside arrays that will later be ignored).
4. Numbers: `-? (0 | [1-9][0-9]*) (\. [0-9]+)? ([eE] [+-]? [0-9]+)?` — a number with `.`, `e` or `E` is a *real*, otherwise an
   *integer*. A leading `+`, `.5`, `1.`, `01`, `-`, `1e`, hex, `NaN`, `Infinity` are errors. An integer that does not fit `i64`
   (`9223372036854775808`, `-9223372036854775809`) is an **error** (not converted to real). A real whose value overflows
   (`1e400`) is an error; underflow (`1e-400`) yields `0.0` (sign kept: `-1e-400` → `-0.0`). Reals are converted with correct rounding. `-0` is the integer 0; `-0.0` the real −0.
5. Literals `true`, `false`, `null` exactly lower-case, delimited like any token (`nul`, `True`, `truex` are errors).
6. Strings: control characters `< 0x20` (including raw newline and tab) are errors; escapes `\" \\ \/ \b \f \n \r \t \uXXXX` (hex any case);
   any other escape is an error; `\u0000` is an **error** (also in keys); `\u` surrogates must come as a valid high+low pair, a lone or
   misordered surrogate is an error; the raw bytes must be well-formed UTF-8 (same rules as §6.2) else error.
7. Nesting: containers may be nested to depth **2047**; 2048 or more levels are an error (the innermost scalar of a 2048-deep
   chain counts: `[[…[1]…]]` with 2048 pairs fails, with 2047 succeeds).

On failure the function logs **one LOG_ERROR line** (message text informational) and returns NULL. On success the root's members, in
document order, are converted (`add_item`) into the new object:

| JSON | stored as user value |
|---|---|
| object | `set_obj` of a new object built recursively from its members |
| array | `set_array` of a new array: for each element that **is an object**, a new object (members converted as above) appended; **all other elements (numbers, strings, bool, null, arrays) are silently dropped** (QUIRK Q3) |
| string | `set_string` |
| integer | `set_int` |
| real | `set_double` |
| `true`/`false` | `set_bool` |
| `null` | `set_obj(NULL)` — an object item with a present NULL value |

If the root is an **array** the result is a valid **empty** object (QUIRK Q2). Conversion uses the ordinary setters, so §6.1 applies
(no member is ever replaced because duplicates are rejected, hence the order equals document order).

### 6.4 Files

- **S-014-R70** `create_from_json_file(path)`: `os_quick_read_utf8_file`; NULL result (unreadable, empty) → NULL with **no** log; else `create_from_json` (so invalid
  text logs the R-error and returns NULL).
- **S-014-R71** `create_from_json_file_safe(path, ext)`: `r = create_from_json_file(path)`. If `r` is NULL and `ext` is non-NULL and non-empty:
  `backup = path + "." + ext` (no extra dot if `ext` starts with `.`); if the backup exists: log LOG_WARNING, **rename backup over `path`**
  (so the backup file no longer exists afterwards, even if it is corrupt; works also when `path` is missing), and return `create_from_json_file(path)`.
  Otherwise NULL.
- **S-014-R72** `save_json(D, path)`: false if `D` is NULL; else writes the compact JSON (no BOM) via `os_quick_write_utf8_file`; result = write success.
  `save_json_safe` / `save_json_pretty_safe` write compact / pretty text with the safe write (§3.4); `{}` (a non-empty string) is written for an empty
  object.

### 6.5 Rules for JSON output entry points

- **S-014-R80** `get_json` = compact, user values only; `get_json_with_defaults` = compact, all items; `get_json_pretty`, `get_json_pretty_with_defaults` likewise
  (§6.2). The returned pointer belongs to `D` and is valid until the next `get_json*` call on `D` or the destruction of `D`.
- **S-014-R81** `get_last_json(D)` returns the pointer last produced by any `get_json*` (NULL if none yet, or if `D` is NULL).
- **S-014-R82** The with-defaults forms write the **effective** value: for an item with a default but no user value that is the default
  (for nested objects the default object, recursively written with defaults); a number is written with the number type of that value;
  shells are written as `""`, `0.0`, `false`, `null`, `[]` by item type.
- **S-014-R83** A re-read of `get_json` output reproduces all user values (int/double typing preserved: `2.0` stays double) except
  arrays of non-objects (Q3), non-finite doubles and invalid UTF-8 (omitted at write time), and NULL arrays (re-read as empty arrays).

## 7. Concurrency

Reference counts are manipulated with atomic operations (so addref/release from several threads on the same object is
safe), but nothing else is synchronised: concurrent mutation or JSON generation (which writes `D`'s internal buffer) on the
same object, or mutating while another thread holds a string pointer, is a race. A Rust port should make the
handle types `Send + Sync` only if the interior state is protected (e.g. `Arc<Mutex<…>>`), keeping the observable counts
identical.

## 8. Reference defects and quirks

- **Q1 (order, QUIRK)** Growth moves an item to the end (§6.1). Demonstrated by `03-order-growth` (and the key orders in `12`, `13`, `14`, `19`).
- **Q2 (top-level array, QUIRK)** `create_from_json("[1,2]")` succeeds with an empty object (`15-json-read`).
- **Q3 (array elements, QUIRK)** Only object elements of a JSON array are kept; the writer cannot produce arrays of scalars (`15`, `19`).
- **Q4 (get_json buffer, QUIRK)** The pointer returned by a json call is invalidated by the next call; a port that returns owned `String`s may do so but must keep `last_json`.
- **Q5 (JSON members vanish silently, QUIRK)** NaN/inf doubles and invalid UTF-8 strings or keys are dropped from the text without any log (`12-json-write`).
- **Q6 (safe loader consumes backup, QUIRK)** The backup is moved over the primary even when the primary merely does not exist, and a corrupt backup is consumed too (`17-json-files`).
- **Q7 (default getter fall-through)** `get_K` returning the default/autoselect value when no user value exists is what makes `obs_data_get_*` usable
  for settings with defaults (CORE, listed so it is not "fixed"); autoselect is the last fallback (`02-layers`).
- **D1 (DEFECT, refcount of a re-set default object/array)** `set_default_obj` / `set_default_array` (and `item_` forms, `set_default_vec*`, `set_default_frames_per_second`) on an
  *existing item that has no user layer* release the old default but **do not add a reference for the new one** (the count is one too low; destroying
  the owner later frees an object the caller still owns). Creating the item with the default, or setting the default while a user layer
  exists, counts correctly. **Required:** always take the reference (count + 1). `05-defect-default-ref` prints the reference's wrong counts on `info_refs`/`info_a_refs` lines (not compared).
- **D2 (DEFECT, NULL name)** A NULL `name` crashes every name-taking function (`get_*`, `set_*`, `has_*`, `unset_*`, `erase`, `byname`), even on an empty object.
  **Required:** no crash; reads return the zero value, `has_*` false, writers/erase no-ops, `byname` NULL (`18-null-name-crash`, `try_quiet`: `try X crash` = `try X ok`; the conforming zero-value results are specified here but not printed).
- **D3 (DEFECT, type change under other layers)** `set_K` (R10) or `set_autoselect_K` (R12) on an item whose default/autoselect layer holds a value of a *different type* reinterprets those
  bytes as the new type: wrong strings, out-of-bounds reads, or releasing garbage as an object. Number→number (int/double) is fine and legitimate.
  **Required:** the layers that no longer match the new type are discarded (released). Not covered by vectors (undefined bytes).
- **D4 (DEFECT, stale item handles after growth)** If an item is grown (`item_set_*` or a set by name) while a second handle to it exists, the reference reallocates the storage and the second handle dangles.
  **Required:** all handles keep referring to the same live item. Not covered.
- **D5 (DEFECT, unspecified conversions)** `get_int` of a double that does not fit `i64` (or NaN) is UB; saturate (Rust `as`) and do not rely on it.
- **D6 (DEFECT, index preconditions)** `obs_data_array_erase` with `idx ≥ count`, `insert` with `idx > count` and `push_back_array(a, a)` are out of contract; a port SHOULD ignore/clamp rather than corrupt.

## 9. Implementation notes (non-normative)

- Object: `Rc<RefCell<Data>>`/`Arc<Mutex<…>>` with `items: Vec<Rc<Item>>` (list order); a map from name to index is an optimisation only — keep the Vec as the
  source of order and move an item to the end exactly when §6.1 says so.
- Item: `{ name: Vec<u8>, ty, user: Option<Value>, default: Option<Value>, autoselect: Option<Value>, sizes… }`; `Value` = `Str(Vec<u8>) | Int(i64) | Dbl(f64) | Bool | Obj(Option<Obj>) | Arr(Option<Arr>)`.
  Names and strings are **byte strings** (they may be invalid UTF-8 until written to JSON).
- Reference counts = `Rc::strong_count`/`Arc::strong_count`; clone exactly where §5.1 says. The arrays' elements, items' layers and handles all hold one clone each.
- Detached item walk (R41): store `next_at_detach` on the item when it is removed from the list.
- JSON writer: shortest round-trip digits are available from `format!("{:e}", f)`; re-assemble per §6.2. Parser: write a recursive-descent parser with an explicit depth counter
  (≤ 2047) and the duplicate-key check; use `str::parse::<f64>` (correctly rounded) and reject non-finite results.
- The `Option<&str>` / byte-string API: getters returning `&str` should return `&[u8]`/`Cow` if invalid UTF-8 can be stored (it can, via `set_string` from FFI).
- FFI: the C ABI of `obs_data_item_next(obs_data_item_t **)` and friends passes pointers to handles; keep the ABI.

## 10. Conformance coverage

| Rule / item | Scripts |
|---|---|
| R01–R05 (reference counts) | `04-refcounts`, `06-apply-clear-erase`, `09-arrays`, `08-item-api` |
| R10–R13 setters | `01-scalars`, `02-layers`, `08-item-api`, `20-misc-quirks` |
| R20–R25 getters, has, types | `01-scalars`, `02-layers`, `08-item-api`, `14-json-with-defaults` |
| R26 NULL values | `04-refcounts`, `08-item-api`, `15-json-read`, `20-misc-quirks` |
| R30, R31 unset/erase/clear | `02-layers`, `06-apply-clear-erase`, `07-iteration` |
| R32 apply | `06-apply-clear-erase`, `19-roundtrip`, `20-misc-quirks` |
| R33 get_defaults | `06-apply-clear-erase`, `20-misc-quirks` |
| R40–R43 iteration | `07-iteration`, `08-item-api` |
| R50–R53 arrays | `09-arrays`, `04-refcounts` |
| R60, R61 vec/quat | `10-vec-quat` |
| R65, R66 fps | `11-frames-per-second` |
| §6.1 growth / Q1 | `03-order-growth` (+ key order in `12`,`13`,`14`,`19`) |
| §6.2 writer, R80–R82 | `12-json-write`, `13-json-pretty`, `14-json-with-defaults`, `19-roundtrip` |
| §6.3 reader | `15-json-read`, `16-json-read-errors`, `19-roundtrip` |
| R70–R72, §6.4 files, Q6 | `17-json-files` |
| R81, R83, Q4 | `13-json-pretty`, `12-json-write`, `19-roundtrip` |
| Q2, Q3, Q5 | `15`, `16`, `12` |
| D1 | `05-defect-default-ref` |
| D2 | `18-null-name-crash` |
| D3, D4, D5, D6 | not testable (undefined bytes / UB); stated here |

Comparison rules: all output lines byte-identical except (README "obsdata runner"): log message text, `info_*` lines, and `try X crash` (= `try X ok`). Doubles are compared as exact printed text (`%.17g`).

## 11. Open questions

- jansson version: the shortest-round-trip real format and the depth limit 2048 are those of jansson 2.15.1; older jansson (before 2.13?) prints reals with `%.17g` (`0.1` → `0.10000000000000001`). OBS ships its own build; the project that rewrites it must decide whether files written by old versions matter for byte-exact comparison (they re-read identically).
- Exact threshold behaviour of the real formatter was inferred from the values in `12-json-write` (every decimal exponent from −5 to 22 near the boundaries); the authority is the vectors.
- Windows differences not tested: `long` width in the item header, `os_rename` replace semantics, BOM when writing (the code passes `marker=false`).
- Behaviour on 32-bit targets of the §6.1 sizes (pointer-sized object layers are 4 bytes; the number layer is 16 on Win32, 12 on Linux i386); only 64-bit is specified.
- Binary data: strings containing NUL cannot be stored through the C API (strlen), so the port's `set_string` should not accept them either.
