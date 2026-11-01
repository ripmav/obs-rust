# S-015 Properties (obs_properties / obs_property)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `obs-properties.c`, `obs-properties.h` (and the `media_frames_per_second` struct of `media-io/frame-rate.h`) |
| Target crate | `crates/obs-properties` |
| Depends on | S-001 (allocation, `blog` levels), the `obs_data` settings object (specified separately; only used as an opaque handle that is passed to callbacks) |
| Conformance | `test/conformance/scripts/props/` (25 scripts); runner section in `test/conformance/README.md` |

## 1. Scope

A *properties object* describes, as plain data, the controls a user interface should show
for a source, encoder, output or service: an ordered list of typed *properties* (bool, int,
float, text, path, list, colour, button, font, editable list, frame rate, group). It holds
no values: the values live in an `obs_data` settings object that the UI passes to the
callbacks. Plugins build the object in their `get_properties` callbacks; the frontend walks
it (`first`/`next`), reads every attribute through the getters, and calls back
(`modified`, `button_clicked`) when the user acts. Scripting and obs-websocket use the same API.

In scope: everything in `obs-properties.h`. Out of scope: how a UI renders a property, and
`obs_data` itself (callbacks receive an opaque settings handle; this component never reads it).

## 2. Terminology and conventions

RFC 2119 words apply. "Container" = an `obs_properties_t`. "Property" = an `obs_property_t`.
"Tree" = a container together with every container nested in group properties below it.
"Top-most container" of a container = follow *parent* links (§3.2) until none is left.
A *NULL handle* is a NULL container or NULL property argument. All string arguments are
NUL-terminated byte strings compared byte-wise (case-sensitive, no normalisation).
`W` = pointer size (8 for the committed vectors).

Properties are **not thread-safe**: no operation locks anything; callers serialise access
(the frontend uses the UI thread).

## 3. Interface

### 3.1 Enumerations (numeric values are ABI)

| Enum | Values |
|---|---|
| `obs_property_type` | INVALID 0, BOOL 1, INT 2, FLOAT 3, TEXT 4, PATH 5, LIST 6, COLOR 7, BUTTON 8, FONT 9, EDITABLE_LIST 10, FRAME_RATE 11, GROUP 12, COLOR_ALPHA 13 |
| `obs_combo_format` | INVALID 0, INT 1, FLOAT 2, STRING 3, BOOL 4 |
| `obs_combo_type` | INVALID 0, EDITABLE 1, LIST 2, RADIO 3 |
| `obs_editable_list_type` | STRINGS 0, FILES 1, FILES_AND_URLS 2 |
| `obs_path_type` | FILE 0, FILE_SAVE 1, DIRECTORY 2 |
| `obs_text_type` | DEFAULT 0, PASSWORD 1, MULTILINE 2, INFO 3 |
| `obs_text_info_type` | NORMAL 0, WARNING 1, ERROR 2 |
| `obs_number_type` | SCROLLER 0, SLIDER 1 |
| `obs_group_type` | INVALID 0 (named `OBS_COMBO_INVALID` in the header), NORMAL 1, CHECKABLE 2 |
| `obs_button_type` | DEFAULT 0, URL 1 |

Flag constant: `OBS_PROPERTIES_DEFER_UPDATE = 1<<0` (stored/returned like any flag; the
library never interprets flags). Font flag constants `OBS_FONT_BOLD 1, ITALIC 2, UNDERLINE 4,
STRIKEOUT 8` are defined in the header but not used by this component.

**Enum arguments are never validated.** Any integer passed as an enum is stored and returned
unchanged (vectors `07`, `10`, `25` use out-of-range values; they print as `?N`). The only
semantic checks are the ones stated in §5.

### 3.2 Functions

```c
typedef bool (*obs_property_clicked_t)(obs_properties_t *props, obs_property_t *property, void *data);
typedef bool (*obs_property_modified_t)(obs_properties_t *props, obs_property_t *property, obs_data_t *settings);
typedef bool (*obs_property_modified2_t)(void *priv, obs_properties_t *props, obs_property_t *property, obs_data_t *settings);
struct media_frames_per_second { uint32_t numerator; uint32_t denominator; };

/* container */
obs_properties_t *obs_properties_create(void);
obs_properties_t *obs_properties_create_param(void *param, void (*destroy)(void *param));
void      obs_properties_destroy(obs_properties_t *);
void      obs_properties_set_flags(obs_properties_t *, uint32_t);   uint32_t obs_properties_get_flags(obs_properties_t *);
void      obs_properties_set_param(obs_properties_t *, void *param, void (*destroy)(void *));
void     *obs_properties_get_param(obs_properties_t *);
obs_property_t   *obs_properties_first(obs_properties_t *);
obs_property_t   *obs_properties_get(obs_properties_t *, const char *name);
obs_properties_t *obs_properties_get_parent(obs_properties_t *);
void      obs_properties_remove_by_name(obs_properties_t *, const char *name);
void      obs_properties_apply_settings(obs_properties_t *, obs_data_t *settings);

/* adding (all return the new property, or NULL on refusal) */
obs_property_t *obs_properties_add_bool(props, name, desc);
obs_property_t *obs_properties_add_int(props, name, desc, int min, int max, int step);
obs_property_t *obs_properties_add_int_slider(props, name, desc, int min, int max, int step);
obs_property_t *obs_properties_add_float(props, name, desc, double min, double max, double step);
obs_property_t *obs_properties_add_float_slider(props, name, desc, double min, double max, double step);
obs_property_t *obs_properties_add_text(props, name, desc, enum obs_text_type);
obs_property_t *obs_properties_add_path(props, name, desc, enum obs_path_type, const char *filter, const char *default_path);
obs_property_t *obs_properties_add_list(props, name, desc, enum obs_combo_type, enum obs_combo_format);
obs_property_t *obs_properties_add_color(props, name, desc);
obs_property_t *obs_properties_add_color_alpha(props, name, desc);
obs_property_t *obs_properties_add_button(props, name, text, obs_property_clicked_t);            /* deprecated */
obs_property_t *obs_properties_add_button2(props, name, text, obs_property_clicked_t, void *priv);
obs_property_t *obs_properties_add_font(props, name, desc);
obs_property_t *obs_properties_add_editable_list(props, name, desc, enum obs_editable_list_type, const char *filter, const char *default_path);
obs_property_t *obs_properties_add_frame_rate(props, name, desc);
obs_property_t *obs_properties_add_group(props, name, desc, enum obs_group_type, obs_properties_t *group);

/* generic property API */
bool  obs_property_next(obs_property_t **p);
void  obs_property_set_modified_callback(p, obs_property_modified_t);
void  obs_property_set_modified_callback2(p, obs_property_modified2_t, void *priv);
bool  obs_property_modified(p, obs_data_t *settings);
bool  obs_property_button_clicked(p, void *obj);
void  obs_property_set_visible(p, bool);   void obs_property_set_enabled(p, bool);
void  obs_property_set_description(p, const char *);  void obs_property_set_long_description(p, const char *);
const char *obs_property_name(p), *obs_property_description(p), *obs_property_long_description(p);
enum obs_property_type obs_property_get_type(p);   bool obs_property_enabled(p), obs_property_visible(p);
/* kind specific: see §5.4 .. §5.13 */
```

There is **no** `obs_property_get_parent` in this version; the parent link exists only on
containers (via `obs_properties_get_parent`).

`obj` in `obs_property_button_clicked` is an opaque pointer to an object whose first fields
are `{ char *name; const char *uuid; void *data; … }` (libobs's `obs_context_data`, which
every source, output, encoder and service starts with). Only the `data` field (offset `2*W`)
is read. A re-implementation exposes either this layout through its C ABI or the equivalent
"object's private data pointer".

### 3.3 Ownership and lifetime

- Every string passed in (name, descriptions, suffix, filter, default path, list item names
  and string values, option names/descriptions, URL) is **copied**. A NULL string stays NULL.
  An empty string stays empty at add time (see Q-9 for the setters).
- Returned `const char *` point into the property and stay valid until that same attribute is
  changed, removed or the property is destroyed. A property pointer stays valid until the
  property is removed or its container destroyed. Properties are never moved by later adds
  (pointer stability).
- A container passed to `add_group` is **adopted**: it is destroyed together with the group
  property (and when the owning container is destroyed). A container the call refused to
  adopt (NULL return) remains owned by the caller.
- `param`/`destroy` pair: the container owns `param` only through the `destroy` callback
  (§5.1).

## 4. State model

```
Container { param, destroy_cb, flags: u32, props: ordered list of Property (insertion order),
            group_count: number of GROUP properties directly in `props`,
            parent: the GROUP property whose content this container is, or none }
Property  { name, desc (nullable), long_desc (nullable), type, visible=true, enabled=true,
            owner: Container, modified_cb, modified2_cb, priv: opaque (ONE slot, see Q-4),
            kind data }
```

Kind data: INT `{min,max,step:i32, numtype, suffix?}`; FLOAT same with `f64`;
TEXT `{type, monospace=false, info_type=NORMAL, info_word_wrap=true}`;
PATH `{type, filter?, default_path?}`; LIST `{type, format, items[]}` with item
`{name?, disabled=false, value: string? | i64 | f64 | bool}`;
BUTTON `{callback?, type=DEFAULT, url?}`; EDITABLE_LIST `{type, filter?, default_path?}`;
FRAME_RATE `{options[]: (name?, description?), ranges[]: (min fps, max fps)}`;
GROUP `{type, content: Container}`; BOOL, COLOR, COLOR_ALPHA, FONT: none.

## 5. Behaviour

### 5.1 Container lifecycle, flags, param

- **S-015-R01** `create` returns an empty container: flags 0, no param, no destroy callback, no parent, no properties.
- **S-015-R02** `create_param(param, destroy)` is `create` followed by `set_param(param, destroy)`.
- **S-015-R03** `set_param(props, param, destroy)`: no-op for a NULL container. Otherwise, if the container currently has a param **and** a destroy callback (both non-NULL), call `old_destroy(old_param)` first; then store the new pair (a NULL `destroy` is allowed and means "never destroy"; a NULL `param` with a non-NULL `destroy` is stored too).
- **S-015-R04** `get_param`: the stored param; NULL for a NULL container.
- **S-015-R05** `set_flags` stores the 32-bit value verbatim (no masking); `get_flags` returns it. NULL container: set is a no-op, get returns 0.
- **S-015-R06** `destroy(NULL)` is a no-op. Otherwise: first, if destroy callback and param are both non-NULL, call `destroy(param)`; then destroy every property in insertion order, then free the container. Destroying a GROUP property destroys its content container with this same procedure (so a nested container's param callback runs when its group is reached, after the outer container's own param callback and after earlier properties). Vector `02`.

### 5.2 Iteration and ordering

- **S-015-R07** Properties are kept in **insertion order**. `first(props)` returns the first property or NULL (NULL container, or empty). Removal keeps the relative order of the rest; a property added afterwards goes to the end. Vectors `01`, `03`.
- **S-015-R08** `obs_property_next(&p)`: returns `false` and leaves `p` unchanged if the pointer-to-pointer is NULL or `*p` is NULL. Otherwise sets `*p` to the next property in the same container (NULL after the last) and returns whether the new `*p` is non-NULL. Iteration does not descend into groups (use `obs_property_group_content`). Vector `01`.

### 5.3 Names, lookup, parent, removal

- **S-015-R09** (uniqueness) Every `add_*` first checks whether a property with exactly this name exists anywhere in the *tree of the top-most container of `props`* (top-level properties of that container first, then recursively inside group contents). If so it logs `LOG_WARNING "Property '<name>' exists"` (text informational) and returns NULL. `add_*` with a NULL container returns NULL without logging. (The name argument must be non-NULL: D-4.) Vectors `03`, `14`.
- **S-015-R10** `get(props, name)`: NULL for a NULL container. Search the container's own properties by exact name; if found return it. Otherwise (only when the container has at least one direct GROUP property) search each group content in insertion order, recursively, and return the first match. No match → NULL. Vectors `03`, `14`, `21`.
- **S-015-R11** `obs_properties_get_parent(props)`: if `props` is the content of a group property, the container that holds that group property (not the group property itself); NULL otherwise (a top container, or a container not yet adopted or whose group was refused). A NULL argument is D-3. Vector `14`.
- **S-015-R12** `remove_by_name(props, name)`: no-op for a NULL container. If a direct property has this name: unlink it, destroy it (a GROUP property destroys its content, R06, and decrements the container's group count), return. Otherwise, if the container has direct groups, call `remove_by_name` recursively on **every** direct group's content, in order (names are normally unique, so at most one removes something; Q-1 shows how duplicates arise). A missing name is silent. Documented precondition (header): only call it from `get_properties` or from a modified callback that returns `true`. Vectors `03`, `16`, `21`.

### 5.4 Common property operations

- **S-015-R13** A new property is `enabled = true`, `visible = true`, long description NULL, no callbacks, `priv` NULL, owner = the container, and is appended. `name` and `desc` are copies; `desc` may be NULL.
- **S-015-R14** `name`, `description`, `long_description`: the stored string or NULL; NULL handle → NULL. `get_type`: the kind; NULL handle → INVALID. `enabled`, `visible`: the flag; NULL handle → `false`. `set_visible`/`set_enabled`: store; NULL handle no-op. Vectors `04`, `19`, `20`.
- **S-015-R15** `set_description(p, s)` and `set_long_description(p, s)` replace the text; if `s` is NULL **or empty** the stored value becomes NULL (Q-9). The name never changes. Vector `19`.

### 5.5 Callbacks: modified, clicked, apply

- **S-015-R16** `set_modified_callback(p, cb)` stores `cb` (NULL clears it); `set_modified_callback2(p, cb, priv)` stores `cb` and **overwrites the property's single `priv` slot** (also when `cb` or `priv` is NULL); NULL handle no-op. The two callbacks are independent slots; both can be set. Vectors `17`, `24`.
- **S-015-R17** `obs_property_modified(p, settings)`: NULL handle → `false`. If the v1 callback is set: let `top` = top-most container of the property's owner container and return `cb(top, p, settings)`. Else if the v2 callback is set: return `cb2(priv, top, p, settings)`. Else `false`. v1 therefore wins when both are set (Q-5). The return value means "the UI must rebuild/refresh the properties" (`true`) and is passed through unchanged. `settings` may be NULL and is passed through. Vector `17`.
- **S-015-R18** `obs_property_button_clicked(p, obj)`: `false` for a NULL handle, a property that is not a BUTTON, or a button without callback. Otherwise `top` as in R17; if `p`'s `priv` is non-NULL return `callback(top, p, priv)`, else return `callback(top, p, obj ? obj->data : NULL)`. (`add_button` sets no priv; `add_button2` sets `priv` — which a later `set_modified_callback2` can replace, Q-4.) The callback return value means "refresh UI". Vectors `11`, `24`.
- **S-015-R19** `apply_settings(props, settings)`: no-op for a NULL container. Otherwise run the algorithm of §6.2: collect, in a depth-first pre-gather order (a group's nested properties before the group property itself), every property that has a v1 **or** a v2 callback, then invoke them in **reverse** collection order. Each call passes **the container `apply_settings` was called on** (not the property's owner and not the top-most container) as `props`. The callback to run is chosen at call time: v1 if set, else v2 (with its priv). Return values are ignored. The process does not recurse into itself. Vector `18`.

### 5.6 BOOL, COLOR, COLOR_ALPHA, FONT

- **S-015-R20** `add_bool/color/color_alpha/font` (R09 checks, then R13) create properties of the corresponding kind with no extra data. Their kind-specific getters return the neutral defaults of §5.14. Vector `04`.

### 5.7 INT and FLOAT

- **S-015-R21** `add_int`/`add_float` store min, max, step verbatim (no ordering or sign validation, e.g. min > max is kept) with number type SCROLLER; `add_int_slider`/`add_float_slider` use SLIDER; suffix NULL. Vector `05`.
- **S-015-R22** Getters (`int_min/max/step/type/suffix`, `float_*`) on the matching kind return the stored values; on any other kind or NULL handle: `0` / `0.0`, type `SCROLLER`, suffix NULL.
- **S-015-R23** `int_set_limits(p,min,max,step)` / `float_set_limits` replace the three values on the matching kind only (other kinds: no-op, in particular an int's limits are not changed by `float_set_limits`). `int_set_suffix`/`float_set_suffix` replace the suffix (copy; NULL clears) on the matching kind only. Vector `05`.

### 5.8 TEXT

- **S-015-R24** `add_text(…, type)` stores the type, `monospace = false`, `info_type = NORMAL`, `info_word_wrap = true`. Setters `text_set_monospace`, `text_set_info_type`, `text_set_info_word_wrap` act on **any** TEXT property regardless of its text type (info type is not restricted to INFO texts) and are no-ops on other kinds. Getters `text_type/monospace/info_type/info_word_wrap` on non-TEXT or NULL: `DEFAULT`, `false`, `NORMAL`, **`true`**. Vector `06`.

### 5.9 PATH and EDITABLE_LIST

- **S-015-R25** `add_path(…, type, filter, default_path)`: the default path is stored (copied, NULL allowed) for every type. The filter is stored (copied) **only if `type` is FILE or FILE_SAVE**; for any other value (DIRECTORY or an out-of-range number) the filter is dropped and `path_filter` returns NULL. Getters on non-PATH: type `DIRECTORY`, strings NULL. Vector `07`.
- **S-015-R26** `add_editable_list(…, type, filter, default_path)` stores all three verbatim (filter is **not** dropped for any type). Getters on non-EDITABLE_LIST: type `STRINGS`, strings NULL. Vector `07`.

### 5.10 LIST

- **S-015-R27** `add_list(…, type, format)`: if `type == EDITABLE` and `format != STRING`: log `LOG_WARNING "List '<name>', error: Editable combo boxes must be of the 'string' type"` (text informational) and return NULL (after the R09 uniqueness check, which has priority). Every other combination — including INVALID and out-of-range values — is accepted and stored. The item array starts empty. Vectors `10`.
- **S-015-R28** Getters `list_type`, `list_format` on non-LIST or NULL: `INVALID`/`INVALID`. `list_item_count`: 0 for non-LIST.
- **S-015-R29** `list_add_string/int/float/bool(p, name, value)`: append an item and return its index **iff** `p` is a LIST whose format equals the function's format (string/int/float/bool). Otherwise do nothing and return **0** (indistinguishable from "stored at index 0", Q-3). The item is `{name copy (NULL ok), disabled=false, value}`; a string value is copied (NULL ok); the bool value is stored as `true` for any non-zero input. Vectors `08`, `09`, `10`.
- **S-015-R30** `list_insert_string/int/float/bool(p, idx, name, value)`: same acceptance rule as R29 (silent no-op on mismatch); inserts the item at `idx`, shifting later items up. Precondition `idx <= count` (D-5); `idx == count` appends. Vector `08`, `09`.
- **S-015-R31** `list_item_remove(p, idx)`: if `p` is a LIST and `idx < count` remove that item (later items shift down); otherwise no-op. `list_clear(p)`: remove all items of a LIST (type and format are kept). Vector `08`.
- **S-015-R32** `list_item_disable(p, idx, disabled)` sets the flag when `p` is a LIST and `idx < count`, else no-op. `list_item_disabled(p, idx)`: the flag; `false` if out of range / not a LIST. Disabling does not alter anything else. Vector `08`.
- **S-015-R33** `list_item_name(p, idx)`: the item's name (may be NULL); NULL if out of range or not a LIST. `list_item_string/int/float/bool(p, idx)`: the value iff `p` is a LIST of the matching format and `idx < count`; otherwise NULL / `0` / `0.0` / `false`. (A string item added with a NULL value reads back as NULL.) Vectors `08`, `09`.

### 5.11 BUTTON

- **S-015-R34** `add_button(props, name, text, cb)` / `add_button2(…, cb, priv)`: the *text* becomes the description; `callback` may be NULL; `priv` may be NULL; type DEFAULT; url NULL. Behaviour on click: R18. Vector `11`.
- **S-015-R35** `button_set_type` / `button_set_url(p, url)` apply to BUTTON properties only (copy; NULL clears the URL); other kinds: no-op. Getter `button_type`: `DEFAULT` on non-BUTTON. Getter `button_url`: the URL or NULL **on a BUTTON**, but the empty string `""` (not NULL) on any other kind or NULL handle (Q-6). The library does not open URLs; BUTTON with type URL tells the UI to treat the stored URL as a link. Vector `12`.

### 5.12 FRAME_RATE

- **S-015-R36** `add_frame_rate` creates a property with two empty arrays: *options* (each `{name?, description?}`) and *ranges* (each `{min, max}` of `media_frames_per_second`).
- **S-015-R37** `frame_rate_option_add(p, name, desc)` / `frame_rate_fps_range_add(p, min, max)`: on a FRAME_RATE append and return the new index; otherwise return `(size_t)-1` (`DARRAY_INVALID`) and do nothing. Values are not validated (zero denominators are stored).
- **S-015-R38** `frame_rate_option_insert(p, idx, name, desc)` / `frame_rate_fps_range_insert(p, idx, min, max)`: on a FRAME_RATE insert at `idx` (`idx <= count`, D-5; `idx == count` appends), shifting later entries; otherwise no-op.
- **S-015-R39** `frame_rate_options_count`, `frame_rate_fps_ranges_count`: sizes (0 for non-FRAME_RATE). `frame_rate_option_name/description(p, idx)`: the string (maybe NULL) or NULL when `idx >= count` or not a FRAME_RATE. `frame_rate_fps_range_min/max(p, idx)`: the stored value, or `{0, 0}` when out of range / wrong kind.
- **S-015-R40** `frame_rate_clear` empties both arrays, `frame_rate_options_clear` only the options, `frame_rate_fps_ranges_clear` only the ranges; no-ops on other kinds. Vector `13`.

### 5.13 GROUP

- **S-015-R41** `add_group(props, name, desc, type, group)` returns NULL, adopting nothing, if any of these holds (checked in this order): `props` NULL; R09 duplicate name (logs); `group` NULL; `group == props`; `group`'s tree already contains `props` as the content of one of its (transitive) groups ("would create a cycle", silent); any **direct** property of `group` has a name that already exists in the tree of `props`' top-most container (logs the R09 warning). Otherwise: create the GROUP property (appended), increment the container's group count, set `group`'s parent link to the new property, and store `type` and `group`. Vectors `14`, `15`, `21`.
- **S-015-R42** `group_type(p)`: stored type; `0` (INVALID) on non-GROUP or NULL. `group_content(p)`: the content container; NULL otherwise. Vectors `14`, `20`.
- **S-015-R43** Properties may be added to a group content container before or after the group is adopted; they obey R09 relative to the whole tree at the time of the add (uniqueness through the top-most container). Group contents may themselves contain groups (any depth). Adding a property to a content container makes it findable from the top (R10). Vectors `14`, `16`, `22`.

### 5.14 Wrong-kind and NULL defaults (summary)

Every kind-specific getter returns the defaults in this table when the property is NULL or of
another kind (vectors `20`, `12`, `19`, `13`, `06` print them with `probe`):

| Getter family | Default |
|---|---|
| `int_min/max/step`, `float_min/max/step`, `list_item_count`, `*_count` | 0 |
| `int_type`, `float_type` | SCROLLER |
| suffixes, `path_filter`, `path_default_path`, `editable_list_filter/default_path`, `list_item_name/string`, option name/description | NULL |
| `text_type` / `text_info_type` | DEFAULT / NORMAL |
| `text_monospace` | false |
| `text_info_word_wrap` | **true** |
| `path_type` | DIRECTORY |
| `list_type` / `list_format` | INVALID / INVALID |
| `list_item_int/float/bool/disabled` | 0 / 0.0 / false / false |
| `editable_list_type` | STRINGS |
| `fps_range_min/max` | {0,0} |
| `group_type` / `group_content` | 0 / NULL |
| `button_type` / `button_url` | DEFAULT / **""** |

All setters on a wrong kind or NULL handle are silent no-ops.

## 6. Algorithms

### 6.1 Lookup and uniqueness

```
find_direct(c, name)   = first property of c with name == name (byte compare), or none
tree_contains(c, name) =
    if find_direct(c, name): log warning "Property '<name>' exists"; return true
    if c.group_count == 0: return false
    for p in c.props where p.type == GROUP:
        if tree_contains(p.content, name): return true
    return false
has_prop(c, name) = tree_contains(topmost(c), name)

get(c, name):
    if c == NULL: return NULL
    p = find_direct(c, name); if p: return p
    if c.group_count == 0: return NULL
    for p in c.props where p.type == GROUP:
        r = get(p.content, name); if r: return r
    return NULL

topmost(c) = c while c.parent != none: c = c.parent.owner
```
Note `tree_contains` (used by `add`) logs on a hit at any level; `get` does not log.

### 6.2 apply_settings

```
gather(c, list):
    for p in c.props in order:
        if p.type == GROUP: gather(p.content, list)       # nested first
        if p.modified != none or p.modified2 != none: list.push(p)
apply_settings(root, settings):
    list = []; gather(root, list)
    while list not empty:
        p = list.pop()                                    # last pushed first
        if p.modified:   p.modified(root, p, settings)
        elif p.modified2: p.modified2(p.priv, root, p, settings)
```
Worked example (vector `18`): top = [first, grp[g1a, g1b, sub[g2a]], last] with callbacks on all;
gather order = first, g1a, g1b, g2a, sub, grp, last; execution order = last, grp, sub, g2a, g1b, g1a, first.
Mutations of the container by callbacks (adds, visibility, descriptions) are legal; removing
properties during `apply_settings` leaves dangling entries in the list (D-6) and is outside the contract.

## 7. Concurrency

None. The component has no locks or atomics. Callbacks run synchronously on the calling thread
and may call back into the same container (add, hide, describe, fill lists, remove other
properties when returning `true` from `modified`/`button` callbacks). A callback MUST NOT touch
a property it removed.

## 8. Reference defects and quirks

- **Q-1 (duplicate check depth).** `add_group` verifies only the group's *direct* children against the target tree, and a group may share its own name with a property inside its content. Names nested two levels deep, or a group named like its own child, can therefore duplicate others (vector `21`). `get` returns the first match in search order (top-level first), `remove_by_name` removes the first top-level match, else tries every group. Required: reproduce (SHOULD).
- **Q-2 (silent cycle rejection).** A cycle-forming `add_group` returns NULL without any log (only duplicate names log). The check looks only at the *descendants of the candidate group* for the target container (vector `15`). Reproduce.
- **Q-3 (add returns 0 on mismatch).** `list_add_*` on a wrong format returns 0 (vectors `09`, `10`). Required: reproduce.
- **Q-4 (single priv slot).** `add_button2`'s priv and `set_modified_callback2`'s priv are the same field: setting a modified2 callback on a button replaces what clicks receive, and a NULL priv sends clicks back to `obj->data` (vector `24`). Required: reproduce.
- **Q-5 (callback precedence and container argument).** v1 beats v2 when both are set; `apply_settings` passes its own root, `obs_property_modified` and `button_clicked` pass the top-most container (vectors `17`, `18`). Required: reproduce.
- **Q-6 (`button_url` default `""`).** NULL for a button without URL, `""` for other kinds. Reproduce (vector `12`).
- **Q-7 (group type default).** `group_type` of a non-group is 0, whose header name is `OBS_COMBO_INVALID`. Reproduce (vector `20`).
- **Q-8 (apply order).** Reverse of gathering order (vector `18`). Required: reproduce.
- **Q-9 (empty description).** `add_*` keeps `""`; `set_description`/`set_long_description` turn `""` into NULL (vector `19`). Required: reproduce.
- **Q-10 (path filter dropped).** For DIRECTORY and unknown path types (vector `07`); editable lists keep it. Reproduce.
- **Q-11 (bool list items).** The reference reads a bool item through a different union member (`double`); with its zero-initialised storage the result equals the stored bool. Required: a plain bool.
- **D-1 (leak).** `path_data_free` frees the filter only for type FILE, so FILE_SAVE leaks its filter. Required: free it (not observable; found by the leak sanitizer, script `07`).
- **D-2 (leak).** `button_set_url` does not free the previous URL (script `12`). Required: free it.
- **D-3 (NULL deref).** `obs_properties_get_parent(NULL)` dereferences NULL. Required: return NULL (not in vectors).
- **D-4 (NULL name).** `add_*`, `get`, `remove_by_name` with a NULL name crash in the hash lookup. Required: SHOULD NOT crash: `add_*` and `get` return NULL, `remove` is a no-op. Not in vectors.
- **D-5 (insert range).** `list_insert_*`, `frame_rate_*_insert` with `idx > count` trip a debug `assert` (undefined behaviour in release). Required: ignore the call (no vector).
- **D-6 (remove during apply).** Removing a property that has a callback from inside `apply_settings` leaves a dangling pointer in the pending list. Required: a Rust implementation SHOULD skip removed properties (no vector).
- **D-7 (double adoption).** Passing a container that is already a group's content to a second `add_group` (or adding a container to itself indirectly) corrupts the parent link and double-frees on destroy; it is outside the preconditions. Required: reject (return NULL) if the container already has a parent.

## 9. Implementation notes (non-normative)

- Model the tree with `Rc`/arena indices rather than raw pointers: a property needs a back-reference to its owner container, and a container a back-reference to its group property, for R17/R18 (`top` computation) and R11.
- Keep an `IndexMap`/`Vec` with a name index; order is insertion order. Do not use a `HashMap` iteration order.
- Preserve pointer stability for the C ABI (`Box` each property); returned `const char *` need a NUL-terminated copy owned by the property.
- Store the single `priv` as one field shared by button2 and modified2 (Q-4).
- Clone nothing in callbacks: take the callback out of the property (or copy the function pointer + priv) before calling so the callback can mutate the container.
- Floats are stored exactly; the vectors print `%.17g` and compare textually.

## 10. Conformance coverage

| Rule | Scripts (`scripts/props/`) |
|---|---|
| R01, R05, R07 | `01-container-basics` |
| R02–R04, R06 | `02-param-and-destroy-order`, `16-group-remove` |
| R08 | `01`, `22-add-int-float-from-group-content` |
| R09, R10, R12 | `03-lookup-duplicates-remove`, `14-groups`, `16`, `21-quirks`, `25-misc-edges` |
| R11 | `14` |
| R13, R14, R20 | `04-simple-kinds`, `19-string-ownership`, `20-wrong-kind-and-null` |
| R15 | `19` |
| R16, R17 | `17-modified-callbacks`, `24-priv-sharing-quirk` |
| R18, R34 | `11-button`, `24` |
| R19 | `18-apply-settings`, `23-callback-edits-layout` |
| R21–R23 | `05-int-float` |
| R24 | `06-text` |
| R25, R26 | `07-path-editable-list` |
| R27 | `10-list-creation-rules` |
| R28–R33 | `08-list-string`, `09-list-int-float-bool`, `10`, `25` |
| R35 | `12-button-url-type` |
| R36–R40 | `13-frame-rate` |
| R41–R43 | `14`, `15-group-rejections`, `16`, `22`, `25` |
| Section 5.14 defaults | `20`, `06`, `12`, `13`, `19` |
| Q-1 | `21-quirks`; Q-2 `15`; Q-3 `09`, `10`; Q-4 `24`; Q-5 `17`, `18`; Q-6 `12`; Q-7 `20`; Q-8 `18`; Q-9 `19`; Q-10 `07`; Q-11 `09` |
| D-1, D-2 | found with a leak sanitizer on `07`, `12`; not observable in vectors |
| D-3 – D-7 | no vectors (crash or UB in the reference) |

CORE: all scripts except `21-quirks` and `24-priv-sharing-quirk` (QUIRK).

## 11. Open questions

- Whether D-7 should reject (as specified) or keep the reference's undefined behaviour is for the crate owner; the frontend never does it.
- The text of the two logged messages is informational only (log-level `warning` is what the vectors compare).
- The actual visual semantics of `OBS_PROPERTIES_DEFER_UPDATE`, `visible`/`enabled`, and `info_type` belong to the frontend and are not specified here.
