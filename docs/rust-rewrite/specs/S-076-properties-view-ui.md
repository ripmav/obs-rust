# S-076 Properties view & Qt widget kit (properties-view, idian, spinboxes, wrappers)

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 (`shared/properties-view/`, `shared/qt/`) |
| Target crate | `crates/frontend` (widget-kit module; consumed by `crates/frontend` dialogs and by plugin UIs) |
| Depends on | S-015 (obs_properties: type set, `OBS_PROPERTIES_DEFER_UPDATE`, `obs_property_modified`), S-077 (qt-wrappers used app-wide) |
| Conformance | planned, none yet |

## 1. Scope

In scope: `OBSPropertiesView` (the generic properties → Qt widget renderer), the idian
widget kit (rows, toggles, spinboxes, collapsibles), the OBS spinbox/double-slider
behaviour, the wheel-ignore variants, and the `shared/qt/wrappers` helpers. Out of scope:
the property *definitions* (S-015) and the dialogs that embed `OBSPropertiesView`
(S-072/S-068/S-087).

### Reference files (27, full paths relative to `obs-studio/`)

- `shared/properties-view/properties-view.cpp` — widget factory + change routing
- `shared/properties-view/properties-view.hpp` — view contract, `WidgetInfo`, flags
- `shared/properties-view/double-slider.cpp` — int-mapped double `QSlider`
- `shared/properties-view/spinbox-ignorewheel.cpp` — no-focus wheel ignore
- `shared/qt/wrappers/qt-wrappers.cpp` — message boxes, safe-thread exec, line-edit events, dir picker
- `shared/qt/idian/components/ToggleSwitch.cpp` — animated toggle
- `shared/qt/idian/components/ComboBox.cpp` — popup reopen guard
- `shared/qt/idian/components/ExpandButton.cpp` — checkable inline button
- `shared/qt/idian/components/RowInfo.cpp` — title/description label pair
- `shared/qt/idian/components/RowSpinBox.cpp` — −/spinbox/+ row control
- `shared/qt/idian/components/RowDoubleSpinBox.cpp` — double variant
- `shared/qt/idian/widgets/Row.cpp` — row frame + buddy/cursor
- `shared/qt/idian/widgets/RowList.cpp` — row container, ownership
- `shared/qt/idian/widgets/CollapsibleGroup.cpp` — header row + hidden list
- `shared/qt/idian/widgets/ListHeader.cpp` — list header labels
- `shared/qt/idian/include/Idian/Row.hpp` — `Row` contract
- `shared/qt/idian/include/Idian/Utils.cpp` — class manipulation + polish
- `shared/qt/idian/include/Idian/Utils.hpp` — `addClass`/`toggleClass` declarations
- `shared/qt/idian/include/Idian/StateEventFilter.cpp` — focus/toggled state classes
- `shared/qt/idian/include/Idian/ToggleSwitch.hpp` — toggle contract
- `shared/qt/obs-spinbox/SpinBox.cpp` — validated int spinbox
- `shared/qt/obs-spinbox/DoubleSpinBox.cpp` — validated double spinbox
- `shared/qt/obs-spinbox/AutoSelectLineEdit.cpp` — double-click select-all
- `shared/qt/slider-ignorewheel/slider-ignorewheel.cpp` — wheel/click-ignore slider
- `shared/qt/icon-label/IconLabel.cpp` — resizable icon label
- `shared/qt/plain-text-edit/plain-text-edit.cpp` — `white-space: pre` editor
- `shared/qt/vertical-scroll-area/vertical-scroll-area.cpp` — max-width-on-resize scroll area

## 2. Conventions

RFC 2119 words. "The view" = `OBSPropertiesView`. `WidgetInfo` = per-widget state
(property ref, signal connection, `recently_updated`, `old_settings_cache`). QSS class
names are stylesheet selectors (idian theme).

## 3. Behaviour

### 3.1 `OBSPropertiesView` core (`properties-view.cpp/.hpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-076-R01 | `properties-view.cpp` | any ctor | Three constructors (obs_object weak ref / raw `void*` / type string) **all defer the initial load**: they store the args and `QMetaObject::invokeMethod(this, &ReloadProperties, Qt::QueuedConnection)` — the view is never populated in the constructor (widgets can be created before the event loop spins). | queued reload |
| S-076-R02 | `properties-view.cpp` | `ReloadProperties` | Fetch properties (obj or type), rebuild the whole widget tree; `deferUpdate = enableDefer && (flags & OBS_PROPERTIES_DEFER_UPDATE)`; default `enableDefer = true` (overridable via `SetDeferrable(false)` — S-087 R35 uses this for scripts). | `deferUpdate` |
| S-076-R03 | `properties-view.cpp` | `RefreshProperties` | Preserves vertical scroll position, deletes all existing widgets, rebuilds the `QFormLayout` with `setFieldGrowthPolicy(AllNonFixedFieldsGrow)`, clears `lastFocused`. Empty property set → single disabled label `Basic.PropertiesWindow.NoProperties`. Called queued from change routing (R05). | widget tree |
| S-076-R04 | `properties-view.cpp` | widget factory (`NewWidget` + `Add*` per type) | Every interactive widget is connected to the same `WidgetInfo::ControlChanged` slot: `checkStateChanged`, `textEdited` (not `textChanged` — programmatic sets don't fire it), `clicked`, `valueChanged`, `buttonClicked`, `currentIndexChanged`, `toggled`. Long descriptions get a theme-aware help `IconLabel` (help.svg / help_light.svg by dark mode) beside the control; tooltips always set from the long description. | `children` |
| S-076-R05 | `properties-view.cpp` | `WidgetInfo::ControlChanged` | (1) First change only: snapshot `old_settings_cache = obs_data_apply(settings)`. (2) Per-type writer: BOOL/INT/FLOAT/TEXT/LIST/EDITABLE_LIST/FRAME_RATE/GROUP/COLOR/COLOR_ALPHA/FONT/PATH; `OBS_PROPERTY_INVALID` and `OBS_PROPERTY_BUTTON` return early (button = side effect only). Writers that fail (e.g. invalid colour) abort. (3) **Debounce**: single-shot `QTimer` 500 ms, restarted on every change; on expiry: `callback(obj, old_settings_cache, settings)` — but only when `!deferUpdate` (deferred views never auto-apply; the dialog applies on OK). (4) `visUpdateCb(obj, settings)` **immediately** (when set and `!deferUpdate`) — drives live UI previews (e.g. transition preview). (5) `SignalChanged()` (Qt signal for the host). (6) `obs_property_modified(property, settings)` true → `lastFocused = name` + queued `RefreshProperties` (layout changed: e.g. list item visibility). | settings, layout |
| S-076-R06 | `properties-view.cpp` | float widgets | Decimals: `sane_limit` capped; when `step < 1`: `decimals = min(log10(1/step) + 0.99, sane_limit)`. `OBS_NUMBER_SLIDER` doubles get a synced `DoubleSlider` below the spinbox (slider ↔ spinbox two-way). | widget config |
| S-076-R07 | `properties-view.cpp` | `lastFocused` | After a refresh, the widget whose name == `lastFocused` re-grabs focus (keeps keyboard flow across dynamic relayouts). | focus |

### 3.2 Spinboxes (`SpinBox.cpp`, `DoubleSpinBox.cpp`, `AutoSelectLineEdit.cpp`, `spinbox-ignorewheel.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-076-R08 | `SpinBox.cpp` | `validate(text)` | Strip prefix/suffix → empty or `"-"` → `Intermediate`; locale-int parse fail → `Invalid`; canonical re-format `prefix + textFromValue(val) + suffix` differs from input → `Intermediate`; out of `[min,max]` → `Intermediate`; else `Acceptable`. (So a partially typed number is "intermediate", never "invalid" while empty.) | — |
| S-076-R09 | `SpinBox.cpp` | `fixup` | Parse (affix-stripped, locale); success → `qBound(min, val, max)` re-formatted; failure → base `QSpinBox::fixup`. | value |
| S-076-R10 | `SpinBox.cpp` | key press | Up/Down arrows: `stepBy(±1)`, **Shift → `stepBy(±10)`** (gated on `StepUp/DownEnabled`); other keys → base class (Return → `interpretText`). | value |
| S-076-R11 | `SpinBox.cpp` | wheel / mouse / cursor | Wheel events are **ignored unless the spinbox has focus** (`event->ignore()`); `mousePressEvent` forwards to the line edit with the correct drag offset (so dragging the arrows works); line edit is an `AutoSelectLineEdit` (double-click → `selectAll()`); `clampCursorPosition` keeps the cursor inside the number when prefix/suffix present (signal-blocked). | focus/value |
| S-076-R12 | `DoubleSpinBox.cpp` | same hooks | Mirrors R08–R11 with `locale().toDouble` and double range. | value |
| S-076-R13 | `spinbox-ignorewheel.cpp` | wheel | `SpinBoxIgnoreScroll` (used by idian rows): wheel ignored unless focused, else default. Strong focus policy set in ctor. | — |
| S-076-R14 | `AutoSelectLineEdit.cpp` | double-click | `selectAll()` + accept. Shared by both spinboxes. | selection |

### 3.3 Sliders (`double-slider.cpp`, `slider-ignorewheel.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-076-R15 | `double-slider.cpp` | construction / `setDoubleVal` | Integer mapping: `intMax = (max-min)/minStep`; range `[0, intMax]`, single step 1; `setDoubleVal(val) = setValue(lround((val - min) / minStep))`. No floating-point accumulation in the slider value itself. | value |
| S-076-R16 | `slider-ignorewheel.cpp` | wheel / click | `SliderIgnoreScroll`: wheel ignored unless focused. `SliderIgnoreClick`: additionally a mouse press **on the handle** forwards to `SliderIgnoreScroll::mousePressEvent` (i.e. starting a drag from the handle is allowed; clicking the groove is swallowed so clicking a row containing the slider doesn't jump the value). | value |

### 3.4 idian kit (`Row*`, `ToggleSwitch`, `Utils`, `StateEventFilter`, misc)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-076-R17 | `Row.cpp` | construction / `setBuddy` | `QFrame` + zero-margin HBox. First `setBuddy(w)`: `addClass(w, "row-buddy")` + `connectBuddyWidget` (buddy state follows row state). `setChangeCursor(true)` → `addClass(this,"cursor-pointer")` + hand cursor on enter (when enabled) + repolish buddy on enter/leave. | classes |
| S-076-R18 | `RowList.cpp` | `addHeader` / `addRow` | `addHeader` inserts **before** the rows layout (headers stay above rows). `addRow` **takes ownership** of the widget (deleted on `clear()`/destroy — documented in source); inserts a `RowListSpacer` between rows when count > 0; QSS `:first`/`:last` workarounds via added `first`/`last` classes. | rows |
| S-076-R19 | `CollapsibleGroup.cpp` | ctor / toggle | = `Row(RowInfo + ExpandButton)` (expand button is the row's buddy) over a `RowList` that starts **hidden**; button click → `toggleVisibility` (shows/hides the list, rotates state). `setCheckable` variant drives an internal `ToggleSwitch`. | visibility |
| S-076-R20 | `ListHeader.cpp` / `RowInfo.cpp` | labels | Both build a title label (QSS class `title`) + description label (class `description`, word-wrapped for ListHeader); labels start **hidden** and are shown only when their text is set. | labels |
| S-076-R21 | `ToggleSwitch.cpp` | state changes | Geometry: `offPos = width/2 - 18`, `onPos = width/2 + 18`, `margin = 3`. Two `QPropertyAnimation`s: `xpos` (knob travel) and `blend` (0..1 knob/background blend ratio, `blendColors` mix); `blend = (xPos - offset)/(onPos)` recomputed on animation frames. | animation |
| S-076-R22 | `Utils.cpp` | `addClass`/`toggleClass` | QSS class set on `objectName`/`property` + **style repolish** (deferred through a polish queue; `polishChildren` repolishes the subtree). Class name validated against regex `^[a-zA-Z][a-zA-Z0-9_-]*$` — invalid names rejected (prevents CSS injection from property names). | object name |
| S-076-R23 | `StateEventFilter.cpp` | events | Installed via `applyStateStylingEventFilter`: `StyleChange`/`ThemeChange` → repolish + repolish children; `FocusIn` → `addClass(widget,"focus")`, plus `keyFocus` **only** when focus reason is not Mouse/Popup (keyboard-driven styling); `FocusOut` → remove both; `QAbstractButton::toggled` → checked-state class updates. | classes |
| S-076-R24 | `ComboBox.cpp` | popup | After `hidePopup`, a short timer blocks `showPopup` (`allowOpeningPopup` flag) — prevents clicking a generic row with the dropdown open from closing-and-immediately-reopening it (source comment acknowledges this is a hack; keep the guard). | popup state |
| S-076-R25 | `RowSpinBox.cpp` / `RowDoubleSpinBox.cpp` | construction | Layout: `−` button (objectName `obsSpinBoxButton`) + spinbox with buttons removed (objectName `obsSpinBox`) + `+` button; `setFocusProxy` routes focus to the decr button; row-level state styling. Double variant mirrors with `QDoubleSpinBox`. | — |
| S-076-R26 | `ExpandButton.cpp` / `IconLabel.cpp` | misc | `ExpandButton`: checkable `InlineButton` with state-styling filter (rotation is QSS/state-driven). `IconLabel`: `setIcon` renders `icon.pixmap(m_iconSize)`; `setIconSize` updates the size for subsequent sets. | — |

### 3.5 Misc widgets + wrappers (`plain-text-edit`, `vertical-scroll-area`, `qt-wrappers.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-076-R27 | `plain-text-edit.cpp` | ctor | Document default stylesheet `font { white-space: pre; }` (tabs + multiple spaces render literally); optional system fixed font (family + point size) for the monospace variant. | — |
| S-076-R28 | `vertical-scroll-area.cpp` | resize | On resize, the contained widget is resized to the scroll area's **max width** (horizontal fit, vertical scroll) — no horizontal scrollbar. | layout |
| S-076-R29 | `qt-wrappers.cpp` | message boxes | `OBSMessageBox::question/information/warning/critical` variants (warning takes `enableRichText`); `OBSErrorBox` = `critical` with printf formatting; macOS role adjustments for question buttons (S-077 R29). | — |
| S-076-R30 | `qt-wrappers.cpp` | thread-safe exec | `ExecuteFuncSafeBlock(func)`: if on UI thread run inline, else hop. `ExecuteFuncSafeBlockMsgBox(func, title, text)`: same, with a modal "working" message box shown while off-thread. `ExecThreadedWithoutBlocking(func, title, text)`: runs `func` on a **separate thread** (never blocks the UI thread) with a non-modal progress message box; used by all remote fetches (S-074 R28). `CreateQThread` helper wraps a std::function in a `QThread`. | thread |
| S-076-R31 | `qt-wrappers.cpp` | line edits | `LineEditCanceled(event)`: true for Return/Escape **when the widget is a line edit** (consumed); `LineEditChanged(event)`: true for text-change key events — used by property views to commit on Enter without stealing other keys. | — |
| S-076-R32 | `qt-wrappers.cpp` | helpers | `TranslateQtKeyboardEventModifiers` → `INTERACT_*` flag set; `setClasses(widget, newClasses)` replaces the QSS class set (repolish); `SelectDirectory` = `getExistingDirectory` (dir-must-exist); `DeleteLayout` recursively deletes a layout + item widgets; `SetComboItemEnabled`; `QDataStream` operators for `OBSScene`/`OBSSource`/signal-vector (persist by **uuid string**, re-resolve on load). | — |

## 4. Reference defects and quirks

- **Q-1** (`properties-view.cpp`): the 500 ms debounce timer is a **single shared timer per
  WidgetInfo** that is stopped/restarted on every change — a rapid burst from *one* widget
  fires one callback; simultaneous edits from different widgets each have their own timer.
- **Q-2** (`properties-view.cpp`): `old_settings_cache` is only snapshotted on the *first*
  change (`!recently_updated`), so the callback's "old" side reflects pre-edit state, not the
  previous edit — the `recently_updated` flag resets on refresh.
- **Q-3** (`ComboBox.cpp`): the reopen guard is a timer hack (source admits it); do not remove
  it in a "cleanup" — row+combo click races are the reason it exists.
- **Q-4** (`Utils.cpp`): the class-name regex silently *drops* invalid class names rather
  than erroring — property-derived class names with dots/spaces never reach the stylesheet.
- **Q-5** (`SpinBox.cpp`): `validate` returns `Intermediate` (not `Invalid`) for out-of-range
  values, so the line edit never goes red for range violations; clamping happens in `fixup`.

## 5. Concurrency (summary)

- All widget code runs on the UI thread; the only cross-thread pieces are the deferred
  constructor reload (queued call) and `qt-wrappers` thread helpers (R30).
- `RefreshProperties` is always queued, never synchronous inside a change callback (re-entrancy guard).

## 6. Open questions

1. idian is a themeable widget kit: confirm the Rust frontend keeps a QSS-class-based theming
   model (class set + repolish) or switches to native styling — R22/R23 behaviour is the
   contract either way.
2. `OBSPropertiesView` supports `OBS_PROPERTY_EDITABLE_LIST` with an item dialog
   (`EditableItemDialog`, in-file); its full behaviour (item add/remove/rename, `lastFocused`
   interplay) is only pinned at the routing level here — expand when the editable-list
   conformance vectors are generated.
3. DoubleSpinBox R12 mirrors SpinBox — confirm the decimal `fixup` uses the same
   `qBound` + locale round-trip (verified structurally, not per-test).
