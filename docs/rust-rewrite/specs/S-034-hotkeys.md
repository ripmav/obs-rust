# S-034 Hotkeys: registration, bindings, polling and dispatch, pairs, persistence, translations

| | |
|---|---|
| Status | Draft (no Rust sufficiency check performed, see section 11) |
| Spec version | initial |
| Reference | libobs 33.0.1: `obs-hotkey.c` (all of it), `obs-hotkey.h` (public API, types), the hotkey structs of `obs-internal.h`; the key list `obs-hotkeys.h` and the name lookup are specified in S-024 |
| Target crate | `crates/obs-hotkeys` |
| Depends on | S-014 (obs_data, obs_data_array), S-004 (signals: the `key` pointer parameter becomes a hotkey handle), S-024 (key table, `obs_key_to_name`, `obs_key_from_name`), S-006 (dstr behaviour for translations) |
| Conformance | `test/conformance/scripts/hotkeys/` (15 scripts), runner `gen/hotkeys_runner.c` |

## 1. Scope

The hotkey registry of the OBS core: hotkeys registered by the frontend, sources, outputs, encoders and
services; key bindings (key plus modifier mask) that trigger them; the platform-independent polling and
dispatch logic that decides when a binding is pressed or released; hotkey pairs (start/stop style);
saving and loading bindings as `obs_data`; callback re-routing; key-name translations.

Out of scope: the platform backends (`obs_hotkeys_platform_*`: reading real keyboard state, key code
tables, X11/Win32/Cocoa), the key table and name lookup (S-024), the frontend UI, the signal handler
implementation (S-004) and the weak-reference objects of sources/outputs/encoders/services.

**Fake platform.** Everything in this spec that depends on the keyboard is expressed against an
abstract backend `is_pressed(key) -> bool` (section 3.5). The conformance runner scripts this backend
(`down`/`up`) and calls the polling step synchronously (`poll`). In the product the polling step is run
by a dedicated thread every 25 ms (section 7).

## 2. Terminology and conventions

RFC 2119 words. `id` = hotkey id, `pid` = pair id; both are `size_t` (W bytes; 8 in the vectors);
`INVALID = ~0` (all bits set, `0xFFFF...`) for both. Vectors print it as `INVALID`.
"Core absent" = the global core object does not exist (before `obs_startup`, after shutdown).
The "hotkey lock" is the single recursive mutex of the hotkey subsystem (section 7).
Callback = the `obs_hotkey_func` supplied at registration.

Modifier mask bits (`INTERACT_*`, shared with the interaction API):

| name | value | persisted as | reported by polling |
|---|---|---|---|
| caps | 0x1 | no | no |
| shift | 0x2 | `"shift"` | yes (key `OBS_KEY_SHIFT`) |
| control | 0x4 | `"control"` | yes (`OBS_KEY_CONTROL`) |
| alt | 0x8 | `"alt"` | yes (`OBS_KEY_ALT`) |
| mouse left/middle/right | 0x10 / 0x20 / 0x40 | no | no |
| command | 0x80 | `"command"` | yes (`OBS_KEY_META`) |
| numlock | 0x100, keypad 0x200, left 0x400, right 0x800 | no | no |

Key ids are `enum obs_key` values from S-024 (`OBS_KEY_NONE = 0`, `OBS_KEY_LAST_VALUE = 636`).

## 3. Interface

### 3.1 Types

```
obs_key_combination { u32 modifiers; obs_key key; }      // 8 bytes, modifiers at 0, key at 4
enum registerer_type { FRONTEND=0, SOURCE=1, OUTPUT=2, ENCODER=3, SERVICE=4 }
callback       void  (*)(void *data, id, hotkey*, bool pressed)
pair callback  bool  (*)(void *data, pid, hotkey*, bool pressed)      // "accepted"
router         void  (*)(void *data, id, bool pressed)
```

Internal records (needed for the observable behaviour):

- **hotkey**: `id`, `name` (owned copy), `description` (owned copy, may be NULL), callback + data,
  `pressed` (count of currently pressed bindings of this hotkey, int), registerer type, registerer weak
  reference (FRONTEND: none), `pair_partner_id` (INVALID unless in a pair).
- **pair**: `pid`, `id[2]`, `func[2]`, `data[2]`, `pressed0`, `pressed1`.
- **binding**: `key` (combination), `pressed`, `modifiers_match`, `hotkey_id`, hotkey reference.
  All bindings of all hotkeys live in ONE ordered array (creation order; removal preserves the order of the rest).
- **core state**: map id->hotkey (iteration order = registration order), map pid->pair, `next_id` and
  `next_pair_id` (both start at 0), binding array, flags `strict_modifiers` (**true** by default in the product),
  `thread_disable_press`, `reroute_hotkeys`, router + data, per-key translation strings, six audio and two
  scene-item description templates.
- **context** (a source/output/encoder/service): list of its hotkey ids, list of its pair ids, `hotkey_data`
  (an obs_data object or NULL).

### 3.2 Functions (all `EXPORT`, `obs_` prefix)

Registration: `hotkey_register_frontend(name, desc, func, data)`, `hotkey_register_{encoder,output,service,source}(obj, name, desc, func, data)`;
`hotkey_pair_register_{frontend,encoder,output,service,source}([obj,] name0, desc0, name1, desc1, func0, func1, data0, data1)`;
`hotkey_unregister(id)`, `hotkey_pair_unregister(pid)`; `hotkeys_context_release(ctx)` (internal).
Getters: `hotkey_get_id`, `_get_name`, `_get_description`, `_get_registerer_type`, `_get_registerer`, `_get_pair_partner_id`;
`hotkey_binding_get_key_combination`, `_get_hotkey_id`, `_get_hotkey`.
Setters: `hotkey_set_name`, `hotkey_set_description`, `hotkey_pair_set_names`, `hotkey_pair_set_descriptions`.
Enumeration: `enum_hotkeys(func, data)`, `enum_hotkey_bindings(func, data)`.
Persistence: `hotkey_load_bindings(id, combos, n)`, `hotkey_load(id, array)`, `hotkey_pair_load(pid, a0, a1)`,
`hotkeys_load_{encoder,output,service,source}(obj, data)`, `hotkey_save(id) -> array`, `hotkey_pair_save(pid, &a0, &a1)`,
`hotkeys_save_{encoder,output,service,source}(obj) -> data`.
Input: `hotkey_inject_event(combo, pressed)`, `hotkey_enable_background_press(bool)`, polling step (internal, `hotkey_thread`).
Routing: `hotkey_set_callback_routing_func(func, data)`, `hotkey_enable_callback_rerouting(bool)`, `hotkey_trigger_routed_callback(id, pressed)`.
Misc: `hotkey_update_atomic(func, data)`, `hotkeys_free()` (internal), translations (section 5.12).

### 3.3 Signals

All emitted on the core's hotkey signal handler (S-004) with one pointer parameter named `key` holding the
hotkey handle. The runner prints `signal NAME id=ID`.

| signal | when |
|---|---|
| `hotkey_register` | after a hotkey was added (after its bindings were loaded) |
| `hotkey_unregister` | after the hotkey left the table, before it is destroyed |
| `hotkey_bindings_changed` | see R14-R19 |

Signals are emitted while the hotkey lock is held, synchronously.

### 3.4 Logging

One WARNING `obs-hotkey: Available hotkey ids exhausted` and one WARNING
`obs-hotkey: Available hotkey pair ids exhausted` (R03); text is informational, level MUST match.

### 3.5 Platform backend (abstract)

`is_pressed(key) -> bool`. Used only by the polling step.

## 4. Data formats

### 4.1 Binding persistence (obs_data)

One hotkey is saved as an obs_data array; each element is an obs_data object:

```
{ "shift": true, "control": true, "alt": true, "command": true, "key": "OBS_KEY_F1" }
```

- Each modifier member is present (as boolean `true`) only if all bits of the corresponding flag are set; it is
  never written as `false`. Members are set in the order shift, control, alt, command, key (matters only for
  textual JSON output, S-014).
- `"key"` is always written; its value is `obs_key_to_name(key)` (S-024) -- `"OBS_KEY_NONE"` for modifier-only bindings.
- A context (source, output, encoder, service) saves its hotkeys as one object: member name = hotkey NAME, value = the array above
  (in order of the context's hotkey list = registration order; a hotkey without bindings gives `[]`).
  A context with no hotkeys saves `NULL` (not an empty object).

Worked example (vector `10-context-data`): a source with hotkey `Mute` bound to Control+M and `NoEntry` to
Alt+Q saves `{"Mute":[{"control":true,"key":"OBS_KEY_M"}],"Empty":[],"NoEntry":[{"alt":true,"key":"OBS_KEY_Q"}]}`.

### 4.2 Binding loading

Each array element is read as: for each of `shift`, `control`, `alt`, `command`: `obs_data_get_bool` (so a missing
member, `false`, or a non-boolean such as the integer `1` all mean "not set" -- S-014 conversion rules apply);
`key` = `obs_key_from_name(obs_data_get_string(item,"key"))` (missing/unknown string -> `OBS_KEY_NONE`, no error).
Only the four listed bits can come from loading; other bits only arise from `hotkey_load_bindings`.

## 5. Behaviour

### 5.1 Core absent and locking

- **S-034-R01** Every operation that takes the hotkey lock does nothing when the core is absent and returns its
  failure value (`INVALID` for registrations, no result/NULL for saves/enumeration). This covers all register, unregister, load,
  save, enum, inject, routing, atomic-update and background-press functions. Not locking (and requiring a present core):
  all getters, `set_name`, `set_description`, `pair_set_*` (see D-2), translation setters/getter.
  Scripts: 02-register-contexts, 11-lifecycle.

### 5.2 Ids and registration

- **S-034-R02** `next_id` and `next_pair_id` start at 0; every registration consumes one id of the respective counter; ids are
  never reused (also not after unregister); the two counters are independent. A pair consumes one pair id and two hotkey ids
  (first half first). Scripts: 01, 07.
- **S-034-R03** If `next_id + 1 == INVALID` at registration time a WARNING is logged (ids exhausted); the registration still
  succeeds with `id = next_id`, and `next_id` increments. Hence the registration after the last valid id succeeds and RETURNS
  `INVALID` as the id of a real, table-resident hotkey (Q-7). Same for pairs (pair counter; the pair returns `INVALID` while
  its two hotkeys get ordinary ids). Script: 11-lifecycle.
- **S-034-R04** Registration algorithm (`register_internal`), in this order: (1) allocate the hotkey with copies of name and
  description (description may be NULL); `pair_partner_id = INVALID`; (2) insert into the table; (3) if a context is
  given: take array `hotkey_data[name]` (absent or `hotkey_data == NULL` -> none), load its bindings (R15), append the id to the
  context's hotkey list; (4) emit `hotkey_register`; (5) return the id. Consequently for a context hotkey with a non-empty
  stored array `hotkey_bindings_changed` is emitted BEFORE `hotkey_register`. Script: 10-context-data.
- **S-034-R05** Registerer variants: frontend (type 0, no context, no weak reference); source (1), output (2), encoder (3),
  service (4) each store a NEW weak reference of the object per hotkey (a pair therefore holds two) and register against the
  object's context. A NULL object returns `INVALID` with no side effect (no id consumed, no signal). A source whose context is
  private (`private` flag) also returns `INVALID`. Script: 02.
- **S-034-R06** `pair_register_*`: `INVALID` pair id (no side effect) if the object is NULL or the core is absent. Otherwise: create the pair
  (new pair id; the pair id is appended to the context's pair list if a context is given), register hotkey 0 (name0/desc0,
  internal callback "first") then hotkey 1 (internal callback "second"), set each half's `pair_partner_id` to the other half's id.
  Both halves load their bindings from the context data like any hotkey (R04). Scripts: 07, 10.

### 5.3 Getters, setters, enumeration

- **S-034-R07** Getters return the stored fields. `get_registerer` returns the weak reference (NULL for frontend). Binding getters return the binding's combination, hotkey id and hotkey.
- **S-034-R08** `set_name(id, s)` / `set_description(id, s)` replace the stored copy; unknown id (including `INVALID`): no-op.
  No signal is emitted. `pair_set_names(pid, n0, n1)` applies `set_name` to the two halves (n0 to half 0); unknown pair: no-op;
  likewise `pair_set_descriptions`. NULL name is outside the contract (the context save would use it as a key). Script: 01, 07.
- **S-034-R09** `enum_hotkeys(f, d)` calls `f(d, id, hotkey)` for every registered hotkey in registration order
  (unregistered ones are gone), stops when `f` returns false. `enum_hotkey_bindings` calls `f(d, idx, binding)` over the global binding
  array in order, same stop rule. Both hold the lock during the calls. The callback MUST NOT add or remove bindings or hotkeys
  (the enumeration iterates a snapshot of count and storage in the reference; that is not defined behaviour to rely on).
  Scripts: 01, 03.

### 5.4 Unregistering

- **S-034-R10** `unregister(id)`: ignored if `id >= next_id` (this includes `INVALID`) or not registered. Otherwise in this order:
  remove from the table; emit `hotkey_unregister` (hotkey still valid in the handler); release the weak reference (source: a
  second release of a NULL reference follows in the reference, harmless, Q-8); destroy the hotkey; remove all of its bindings
  (R12). The id remains in the owning context's list (stale; never matched again). Scripts: 01, 02, 13.
- **S-034-R11** `pair_unregister(pid)`: ignored if `pid >= next_pair_id` or unknown; otherwise unregister half 0 then half 1 (each as
  R10, so two `hotkey_unregister` signals) and remove the pair. Script: 07.
- **S-034-R12** `remove_bindings(id)` (internal): repeatedly find the FIRST binding with that hotkey id in array order; if it is pressed
  release it (R22) first; erase it, preserving the order of the other bindings; returns whether any was removed.
  Releasing may invoke the hotkey callback with `pressed=false` (R22), BEFORE the erase. Scripts: 14, 13.
- **S-034-R13** `hotkeys_context_release(ctx)` (core absent: nothing at all): unregister (R10) every id in the context's hotkey list in
  list order, then every pair in its pair list (R11; the halves are already gone, so only the pair record is dropped and no further
  signals), then release `hotkey_data`, then empty both lists. Script: 02, 10.
- **S-034-R13b** `hotkeys_free()` (core shutdown): drops all hotkeys, pairs, bindings and translation strings WITHOUT signals and
  without invoking callbacks; releases each hotkey's weak reference. Counters (`next_id`) are not reset. Script: 11, 09.

### 5.5 Loading bindings

- **S-034-R14** `hotkey_load_bindings(id, combos, n)`: unknown id: nothing. Otherwise remove the hotkey's bindings (R12, may release
  callbacks), append one binding per combination in order (key and modifiers stored verbatim, any modifier bits), then emit
  `hotkey_bindings_changed` iff `n > 0` or at least one binding was removed. Script: 03, 14, 15.
- **S-034-R15** `load_bindings(hotkey, array)` (shared by R16-R18): for each element in order read a combination (4.2) and append a binding for it (Q-1:
  every element yields a binding, including `{}`); afterwards emit `hotkey_bindings_changed` iff the array had at least one element.
  A NULL array counts as zero elements. Scripts: 03, 12.
- **S-034-R16** `hotkey_load(id, array)`: unknown id: nothing. Otherwise remove all bindings of the hotkey (R12) then R15. NOTE the
  asymmetry with R14: loading an empty or NULL array removes bindings but emits no signal (Q-5). Script: 03, 14.
- **S-034-R17** `hotkey_pair_load(pid, a0, a1)`: nothing if both arrays are NULL or the pair is unknown; otherwise for half 0 then half 1
  (each if it still exists): remove its bindings, R15 with its array. A NULL array for one half therefore clears that half. Script: 07.
- **S-034-R18** `hotkeys_load_{kind}(obj, data)`: nothing if `obj` or `data` is NULL. For each hotkey in the context's list (list order, hotkeys no
  longer registered skipped): if `data` has an array member named like the hotkey, R15 on it WITHOUT removing existing bindings (the
  stored bindings are APPENDED, Q-3); members for unknown names are ignored; hotkeys without a member are untouched (no signal).
  Script: 10.

### 5.6 Saving bindings

- **S-034-R19** `hotkey_save(id)`: NULL for unknown id or core absent; otherwise a new array with one object (4.1) per binding of that
  hotkey, in global array order. The caller owns the array. A hotkey without bindings saves an empty array (not NULL). Scripts: 03, 07.
- **S-034-R20** Modifier members: written iff `(mods & flag) == flag`; bits outside shift/control/alt/command are lost. Script: 03, 15.
- **S-034-R21** `hotkey_pair_save(pid, p0, p1)`: nothing if both out-pointers are NULL or the pair is unknown; otherwise `*p0` / `*p1` (each
  only if its pointer is non-NULL and its half exists) are set to `hotkey_save` results. Out-values are NOT initialised when the pair is unknown:
  callers must pre-initialise to NULL. Script: 07.
- **S-034-R22b** `hotkeys_save_{kind}(obj)`: 4.1; NULL result if the context's hotkey list is empty (even if previously used) or the core is absent.
  An obs_data containing every listed hotkey, including those without bindings. Script: 10.

### 5.7 Press and release of a binding

- **S-034-R22** `press(binding)`: `binding.pressed = true`; `hotkey.pressed += 1`; if the counter was 0 before: dispatch `(hotkey, true)`.
  `release(binding)`: `binding.pressed = false`; `hotkey.pressed -= 1`; if the counter is now 0: dispatch `(hotkey, false)`.
  So a hotkey with several bindings fires once on the first press and once after the last release (Script 04).
- **S-034-R23** `dispatch(hotkey, pressed)`: if rerouting is OFF call `hotkey.func(data, id, hotkey, pressed)`; if rerouting is ON call
  `router(router_data, id, pressed)` if a router is set, else drop the event. Script: 08.

### 5.8 Modifier matching

- **S-034-R24** `modifiers_match(binding, m)`: strict mode: `binding.mods == m`; non-strict: `(binding.mods & m) == binding.mods`
  (all bits required by the binding are present). Strict mode is a core flag (`true` in a running product; the runner command `strict`). Script: 05, 06.

### 5.9 Polling step

- **S-034-R25** One polling step (runs under the lock; skipped when the core is absent): `m` = OR of `0x2` if `is_pressed(SHIFT)`, `0x4` if `CONTROL`,
  `0x8` if `ALT`, `0x80` if `META`; then for every binding in array order `handle_binding(b, m, no_press = thread_disable_press, strict)`.
- **S-034-R26** `handle_binding` (see 6.1 for the pseudocode): decides press/reset per binding, as specified in section 6.1.
  Observable consequences, each with a vector (04, 05): (a) a key-only binding presses on the first poll that sees its key down *provided the binding's
  `modifiers_match` latch is set*; in strict mode the latch is initially false, so the very first press of a fresh binding is delayed by one
  poll (Q-9); (b) with modifiers, the modifiers must be satisfied before the key goes down -- pressing the key first and the modifiers
  afterwards presses one poll AFTER the modifiers matched; (c) release happens on the first poll that sees the key up or the modifiers
  no longer matching; (d) modifier-only bindings (`key == NONE`, `mods != 0`) press as soon as the modifier set matches; (e) bindings with
  `key == NONE` and no modifiers are never pressed by polling; (f) any binding with bits other than the four polled modifier bits never
  matches in strict mode or non-strict mode (the polled mask never contains them): they are injectable only (Script 15).
- **S-034-R27** Background press disabled (`hotkey_enable_background_press(false)` sets `thread_disable_press`): the polling step never presses a
  binding (`no_press`), but still releases and still maintains the latch. Re-enabling restores pressing. Script: 05.

### 5.10 Injection

- **S-034-R28** `hotkey_inject_event(combo, pressed)` (lock held; uses the current strict flag): for every binding in order: if
  `modifiers_match(binding, combo.modifiers)`: let `p = (binding.key == combo.key) && pressed`; if `binding.key == NONE` then `p = true`
  (regardless of `pressed` and of `combo.key`); if `p`: set `binding.modifiers_match = true` and, if the binding is not pressed, press it (R22).
  Nothing else happens: in particular an event with `pressed = false` never releases (Q-2); injected presses are released by a later
  polling step (key up), by binding replacement, or by unregistering. Injection is not affected by background-press. Script: 06.

### 5.11 Pairs

- **S-034-R29** Pair "first" internal callback `(hotkey, pressed)`: if `pressed1` return. If `pressed0 && !pressed`: set `pressed0 = false` and do NOT call the user
  callback. Else call `func0(data0, pid, hotkey, pressed)`; if it returns true set `pressed0 = pressed`. "Second" is symmetric
  (`pressed0` blocks, `func1`, `pressed1`). A rejected press (return false) leaves the state unchanged, so the later release is still
  delivered to the user callback (with `pressed=false`). Script: 07.
- **S-034-R30** Since blocking is decided in the pair callback only, a binding of the blocked side still becomes "pressed" in the binding layer;
  when it is released later while the pair state of that side is "not pressed", the user callback IS called with `pressed=false` (Q-4). Script: 07.

### 5.12 Rerouting, atomic update, translations

- **S-034-R31** `enable_callback_rerouting`, `set_callback_routing_func` store the flag/function (lock held). `trigger_routed_callback(id, pressed)`: if rerouting is on
  and the hotkey exists call its callback `(data, id, hotkey, pressed)` directly; otherwise nothing. Script: 08.
- **S-034-R32** `hotkey_update_atomic(f, d)`: with the core present calls `f(d)` while holding the lock; absent core: `f` is not called. Script: 11.
- **S-034-R33** `set_translations_s(t, size)`: NULL `t` -> nothing. Copy `min(size, sizeof(struct))` bytes into a zeroed struct (a shorter old-ABI
  struct leaves the rest NULL). For each of these fields, if non-NULL (an empty string counts) store a copy as the translation of the key: `insert->INSERT, del->DELETE,
  home->HOME, end->END, page_up->PAGEUP, page_down->PAGEDOWN, num_lock->NUMLOCK, scroll_lock->SCROLLLOCK, caps_lock->CAPSLOCK, backspace->BACKSPACE, tab->TAB,
  print->PRINT, pause->PAUSE, shift->SHIFT, alt->ALT, control->CONTROL, meta->META, menu->MENU, space->SPACE, escape->ESCAPE`; and on non-Apple builds
  `numpad_divide->NUMSLASH, numpad_multiply->NUMASTERISK, numpad_minus->NUMMINUS, numpad_plus->NUMPLUS, numpad_decimal->NUMPERIOD`
  (Apple builds use the `apple_keypad_*` fields instead and additionally `apple_keypad_equal->NUMEQUAL`; not testable on Linux).
  The fields `left`, `right`, `up`, `down` are never applied (Q-6). Previously stored translations of keys whose field is NULL are kept.
  Script: 09.
- **S-034-R34** Templates: `numpad_num` (Apple: `apple_keypad_num`) if non-NULL: strip leading and trailing whitespace (as `dstr_depad`, S-006), if the text does not contain `%1`
  append `" %1"`, then for digits 0..9 store the text with every `%1` replaced by the digit as translation of `NUM0..NUM9`. `mouse_num`: same, for `MOUSE1..MOUSE29`.
  Replaced translations free the old string. Script: 09.
- **S-034-R35** `get_hotkey_translation(key, def)`: `key == NONE` -> NULL; else the stored translation, else `def` (which may be NULL). Out-of-range key is outside the contract. Script: 09.
- **S-034-R36** `set_audio_hotkeys_translations(mute, unmute, push_to_mute, push_to_talk, monitor_on, monitor_off)` and
  `set_sceneitem_hotkeys_translations(show, hide)` replace the stored template strings with copies (NULL stores NULL); no lock; no other effect. The core initialises
  them to `"Mute"`, `"Unmute"`, `"Push-to-mute"`, `"Push-to-talk"`, and sceneitem `"Show '%1'"` / `"Hide '%1'"` (initial values of the monitor templates are not specified here). Script: 09.

## 6. Algorithms

### 6.1 handle_binding (polling), pseudocode

```
fn handle(b, m, no_press, strict):
    mm        = modifiers_match(b, m, strict)
    only_mods = (b.key.key == NONE)
    if !strict && b.key.mods == 0:  b.modifiers_match = true          // latch is forced on
    do_reset = false
    if only_mods && b.key.mods == 0:                       do_reset = true
    else if (!b.modifiers_match && !only_mods) || !mm:     do_reset = true
    else if !only_mods && !is_pressed(b.key.key):          do_reset = true
    if do_reset:
        b.modifiers_match = mm
        if b.pressed: release(b)
        return
    if b.pressed || no_press: return
    press(b)
```

Notes: the latch `modifiers_match` records "the modifiers matched on the previous poll (or were forced on)". It is rewritten by every reset to the
CURRENT match; it is only ever set by reset/inject. Newly created bindings start with `pressed=false, modifiers_match=false`.
Worked trace (vector 05, strict, binding Shift+A): key A down first, Shift later: polls: [A] reset (mm false -> latch false),
[A+Shift] reset (latch := true, no press), next poll: press. With Shift first: [Shift] reset (latch := true), [Shift+A] press at once.

### 6.2 remove / press interplay
`remove_bindings` and `release` as in R12/R22; the binding array is modified between callbacks, so callbacks that call into the binding API are out of contract (R09).

## 7. Concurrency

- One recursive mutex ("hotkey lock") protects the whole subsystem; callbacks, router, enum callbacks and signal handlers run with it held, so they may
  re-enter any locking API on the same thread (vector 11 `reenter`: a callback calls `hotkey_save` and `set_description`). They must not mutate
  the binding array while an enumeration/polling step is running (R09).
- The product runs the polling step on a dedicated thread: loop { wait on a stop event up to 25 ms; on timeout (not on stop) take the lock (skip if the core is absent),
  run the polling step (R25), unlock }. Callbacks therefore run on that thread unless rerouting is on. The thread name/profiler entry
  (`obs_hotkey_thread(25 ms)`) are informational. The loop is not covered by vectors (timing).
- Platform backends may call `inject_event` / the lock from other threads.
- Getters and the unlocked setters (R01, D-2) race with registration; a Rust port SHOULD lock them.

## 8. Reference defects and quirks

| ID | Kind | Reference behaviour | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | the load test `!modifiers && key==NONE...` takes the address of the local mask, so it is never true: `{}` / unknown key name / `key:NONE` without modifiers create a binding {mods 0, key NONE} (saved back as `{"key":"OBS_KEY_NONE"}`; never pressed by polling but pressed by ANY inject event that matches (R28)) | SHOULD match (saved profiles round-trip and inject behaviour depend on it) | 12 |
| Q-2 | QUIRK | `inject_event(pressed=false)` is a no-op for already pressed bindings | MUST match | 06 |
| Q-3 | QUIRK | context load appends instead of replacing | MUST match | 10 |
| Q-4 | QUIRK | blocked pair side delivers its release to the user callback | MUST match | 07 |
| Q-5 | QUIRK | `hotkey_load` with empty/NULL array removes bindings without signal; `load_bindings` (combos) signals when something was removed | MUST match | 03, 14 |
| Q-6 | QUIRK | translation fields left/right/up/down ignored | MUST match | 09 |
| Q-7 | QUIRK | after exhaustion a real hotkey is registered with id `INVALID`; callers cannot tell it from a failure | SHOULD match; MAY use u64 ids and never hit it | 11 |
| Q-8 | QUIRK | unregistering a source hotkey releases the weak ref twice, the second time with NULL (no-op) | n/a (ownership detail); a port MUST release exactly once | 02 (`weaks`) |
| Q-9 | QUIRK | strict-mode first press of a fresh binding is delayed one poll by the latch | SHOULD match | 04, 05 |
| D-1 | DEFECT | unregister / context release of a hotkey whose binding is pressed: the hotkey is freed and then the pressed binding is released through the dangling pointer (use-after-free; works by luck, passes a clobbered id to the callback) | exactly one callback `(pressed=false)` with the valid id and hotkey, delivered after `hotkey_unregister` and before the binding disappears; no other effect; the hotkey must stay alive until then | 13 (recorded with `quiet` callbacks; do not run under ASan) |
| D-2 | DEFECT | `set_name`, `set_description`, `pair_set_*` and the translation setters do not take the lock and dereference the core without checking | take the lock; core absent -> no-op | not testable |
| D-3 | DEFECT | `hotkey_pair_save` leaves out-values unset when the pair is unknown (caller reads garbage if not initialised) | SHOULD set both out-values to NULL when the pair is unknown | 07 |

## 9. Implementation notes (non-normative)

- Keep hotkeys in an insertion-ordered map (`IndexMap<u64, Hotkey>`), bindings in a `Vec<Binding>` storing hotkey id (not pointers); this also fixes D-1 by design if release is performed before removal.
- Use a `parking_lot::ReentrantMutex<RefCell<..>>`; take care not to hold a `RefCell` borrow across callbacks (callbacks re-enter).
- The signal handler `key` pointer maps to a hotkey handle/id; `calldata` is S-002.
- Translations: use `Vec<Option<String>>` of length 636.
- Depends on the exact JSON member names listed in 4.1; modifiers via `obs_data_get_bool` (so `1` is false, S-014).

## 10. Conformance coverage

| Rules | Scripts |
|---|---|
| R01 | 02-register-contexts, 11-lifecycle |
| R02-R05 | 01-register-basic, 02-register-contexts, 11-lifecycle |
| R06 | 07-pairs, 10-context-data |
| R07-R09 | 01, 03, 07 |
| R10-R13b | 01, 02, 07, 10, 11, 13-defect-unregister-pressed, 14-replace-while-pressed |
| R14-R18 | 03-bindings-load-save, 07, 10, 12-quirks-empty-bindings, 14, 15 |
| R19-R22b | 03, 07, 10, 15-extra-modifier-bits |
| R22-R24 | 04-poll-plain-keys, 05-poll-modifiers, 06-inject |
| R25-R27 | 04, 05, 15 |
| R28 | 06-inject, 12 |
| R29-R30 | 07-pairs |
| R31 | 08-routing |
| R32 | 11-lifecycle |
| R33-R36 | 09-translations |

## 11. Open questions

- **No Rust sufficiency check was performed** for this spec (cost control, by instruction). The pseudocode in 6.1 and the rules were validated only against the C reference through the vectors.
- The key table / name lookup (S-024) is taken as given. The initial values of the monitor templates and the stop-event/profiler details of the hotkey thread were not specified.
- The Windows / macOS / X11 backends (what `is_pressed` means for META, AltGr, left/right variants, `strict_modifiers` on other platforms) are not specified; macOS also uses the Apple translation field set (R33).
- Behaviour when callbacks mutate the binding array during polling/enumeration is deliberately left undefined.
