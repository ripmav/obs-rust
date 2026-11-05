# S-072 Frontend: dialogs, source pickers, toolbars & tree widgets

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `frontend/components/*`, `frontend/dialogs/*`, `frontend/utility/*` (Qt widgets) |
| Target crate | `crates/frontend` (GUI layer) |
| Depends on | S-048 (frontend API catalog, config), S-033 (scene item model), S-034 (undo/redo via `obs_source` settings), S-069 (main window hosts these widgets) |
| Conformance | planned; none yet (Qt UI; no executable vectors) |

## 1. Scope

In scope: the sources/scene tree widgets (view, model, per-row item
widget), the source-select thumbnail button, the scene list widget (grid
mode), the per-source property toolbars (combo device pickers, color,
text, game, device capture), the advanced-audio per-source control row,
the alignment selector (9-cell) + its a11y interfaces, the
missing-files model/dialog/delegates, the extra-browser-docks
model/dialog/delegate, and the small dialogs (name, about, log viewer,
log upload, permissions, update, dev playground) plus the shared
flow/label/button widgets.

Out of scope: scene-item canvas interaction (S-067), the advanced audio
*dialog shell* (`OBSBasicAdvAudio` — S-069), property *views* for filters
(`OBSBasicProperties` — S-069), thumbnail rendering pipeline internals
(`App()->thumbnails()` service).

**Reference files** (all 47):

- source tree: `frontend/components/SourceTree.cpp`, `frontend/components/SourceTree.hpp`, `frontend/components/SourceTreeModel.cpp`, `frontend/components/SourceTreeItem.cpp`, `frontend/components/SourceTreeDelegate.cpp`
- trees/lists: `frontend/components/SceneTree.cpp`, `frontend/components/SourceSelectButton.cpp`, `frontend/components/FocusList.cpp`, `frontend/components/VisibilityItemWidget.cpp`, `frontend/components/VisibilityItemDelegate.cpp`
- alignment: `frontend/components/AlignmentSelector.cpp`, `frontend/components/AccessibleAlignmentSelector.cpp`, `frontend/components/AccessibleAlignmentCell.cpp`
- source toolbars: `frontend/components/SourceToolbar.cpp`, `frontend/components/ComboSelectToolbar.cpp`, `frontend/components/AudioCaptureToolbar.cpp`, `frontend/components/ApplicationAudioCaptureToolbar.cpp`, `frontend/components/DisplayCaptureToolbar.cpp`, `frontend/components/WindowCaptureToolbar.cpp`, `frontend/components/GameCaptureToolbar.cpp`, `frontend/components/DeviceCaptureToolbar.cpp`
- source toolbars (cont.): `frontend/components/ColorSourceToolbar.cpp`, `frontend/components/TextSourceToolbar.cpp`, `frontend/components/ImageSourceToolbar.cpp`, `frontend/components/BrowserToolbar.cpp`
- audio: `frontend/components/OBSAdvAudioCtrl.cpp`
- missing files: `frontend/utility/MissingFilesModel.cpp`, `frontend/utility/MissingFilesPathItemDelegate.cpp`, `frontend/dialogs/OBSMissingFiles.cpp`
- extra browsers: `frontend/utility/ExtraBrowsersModel.cpp`, `frontend/utility/ExtraBrowsersModel.hpp`, `frontend/utility/ExtraBrowsersDelegate.cpp`, `frontend/dialogs/OBSExtraBrowsers.cpp`
- misc widgets: `frontend/components/FlowLayout.cpp`, `frontend/components/FlowFrame.cpp`, `frontend/components/MenuButton.cpp`, `frontend/components/UrlPushButton.cpp`, `frontend/components/ClickableLabel.hpp`, `frontend/components/OBSSourceLabel.cpp`, `frontend/utility/SceneRenameDelegate.cpp`
- dialogs: `frontend/dialogs/NameDialog.cpp`, `frontend/dialogs/OBSAbout.cpp`, `frontend/dialogs/OBSLogViewer.cpp`, `frontend/dialogs/LogUploadDialog.cpp`, `frontend/dialogs/OBSPermissions.cpp`, `frontend/dialogs/OBSUpdate.cpp`, `frontend/dialogs/OBSIdianPlayground.cpp`

## 2. Conventions

RFC 2119 words. All widgets are GUI-thread; obs-thread signals are queued
(`QMetaObject::invokeMethod`, `Qt::QueuedConnection`) before touching
widgets. `QTStr(...)` = translated string; `main` = `OBSBasic::Get()`
(S-069). Undo/redo actions go through `main->undo_s` (S-034-style action
stack).

## 3. Interface (widget surface)

- `SourceTree : QListView` — model = `SourceTreeModel`; per-row index
  widgets = `SourceTreeItem`; API: `SelectItem`, `MultipleBaseSelected`,
  `GroupsSelected`, `GroupedItemsSelected`, `UpdateIcons`,
  `SetIconsVisible`, `Clear/Add/Get/GetNewGroupName`, slots
  `ReorderItems`, `RefreshItems`, `Remove`, `GroupSelectedItems`,
  `UngroupSelectedGroups`, `AddGroup`, `Edit(idx)`, `NewGroupEdit(idx)`;
  state: `ignoreReorder`, `textNoSources`/`iconNoSources`,
  `undoSceneData`.
- `SourceTreeModel : QAbstractListModel` — `items`
  (`QVector<OBSSceneItem>`, the *current scene's* base items incl. group
  sub-items in scene order), `groups` tracking, `nameList`; emits
  `dataChanged`/`rowsMoved`/`rowsInserted`/`rowsRemoved`;
  `OBSFrontendEvent` hook.
- `SourceTreeItem : QWidget` — per-row layout `[icon][label][vis][lock]`
  (+ group-collapse checkbox); types Item/Group/SubItem.
- `SourceToolbar : QWidget` — base: `weakSource`, `props`
  (`obs_source_properties(source)`), `oldData` (undo snapshot);
  `SaveOldProperties`, `SetUndoProperties`.
- `ComboSelectToolbar : SourceToolbar` — device/window combo +
  activate-button layout; `FillPropertyCombo`,
  `UpdateSourceComboToolbarProperties` (shared helpers).
- `OBSAdvAudioCtrl` — one advanced-audio row for a source.
- `AlignmentSelector : QWidget` — 3×3 grid; `value()/setAlignment`,
  `currentIndex()/setCurrentIndex`; signals `valueChanged(Qt::Alignment)`,
  `currentIndexChanged(int)`.
- `SceneTree : QListWidget` (scene list, optional grid mode);
  `SourceSelectButton : QAbstractButton` (scene-item thumbnail button);
  `MissingFilesModel : QAbstractTableModel` (columns
  State/OriginalPath/NewPath) + `OBSMissingFiles` dialog +
  `MissingFilesPathItemDelegate` (line edit + browse + clear);
  `ExtraBrowsersModel : QAbstractTableModel` (Title/Url/Delete) +
  `OBSExtraBrowsers` dialog + `ExtraBrowsersDelegate`; small widgets
  `FlowLayout`, `FlowFrame`, `FocusList`, `MenuButton`, `UrlPushButton`,
  `ClickableLabel`, `OBSSourceLabel`, `VisibilityItemWidget`,
  `VisibilityItemDelegate`, `SceneRenameDelegate`, `SourceTreeDelegate`.

## 4. Data formats

- `SourceToolbar::oldData`: full copy of the source's settings plus key
  `"undo_suuid"` = source UUID; stored as JSON in the undo action.
- `MissingFilesModel` states: `Missing`, `Found`, `Replaced`, `Cleared`
  (`MissingFilesState`); row = (originalPath, sourceName, newPath).
- `ExtraBrowsersModel` items: `{title, url, prevIdx}` — `prevIdx == -1`
  marks a newly added row.
- `OBSAdvAudioCtrl` constants: `MIN_DB = -96.0`, `MAX_DB = 26.0` (dB
  spinbox range).

## 5. Behaviour (numbered rules)

### 5.1 Source tree model (`SourceTreeModel.cpp`, `SourceTree.hpp`)

- **S-072-R01** The model mirrors the **current scene only**: `SceneChanged()` (on `OBS_FRONTEND_EVENT_SCENE_CHANGED` and any item add/remove/reorder via `OBSFrontendEvent`) re-enumerates scene items (`obs_scene_enum_items`) into `items`, updates group tracking; `SourceTree::ResetWidgets` creates a `SourceTreeItem` per row (`UpdateWidgets(force)` reuses existing widgets when possible). An empty scene shows the built-in "no sources" message (icon + static text).
- **S-072-R02** `ReorderItems()` (scene item order changed externally, e.g. via the canvas): if the item count differs → full `SceneChanged()` reset. Otherwise find the first index where old/new differ (`idx1Old`), the index in the new list where that old item reappears (`idx1New`), and the maximal run length `count` of consecutive matches `old[idx1Old+i] == new[idx1New+i]`; then `beginMoveRows(∅, idx1Old, idx1Old+count-1 → idx1New+count)`, shift the `items` vector (`MoveItem`), `endMoveRows`; repeat until no differences. If the moved item is not found later, fall back to full reset.
- **S-072-R03** `Add(item)`: `beginInsertRows` at the item's scene position, push into `items`, `endInsertRows`, `UpdateWidget`; `Remove(item)` is the reverse, plus group-state update when a group leaves.
- **S-072-R04** `data()` returns only `AccessibleTextRole` (source name); visuals come from the index widget. `flags()`: root row `ItemIsDropEnabled`; item rows `ItemIsEditable | ItemIsDragEnabled | (is_group ? ItemIsDropEnabled : 0)` — **only groups accept drops**; `supportedDropActions` = MoveAction.
- **S-072-R05** `GetNewGroupName()`: `QTStr("Group")`; if a source of that name exists, `QTStr("Basic.Main.Group").arg(n)` with n = 2,3,4… until free. `AddGroup()` creates `obs_scene_add_group` at position 0, `beginInsertRows(0,0)`, updates the widget, then **queued** `Edit(0)` (enter rename mode).

### 5.2 Source tree view + drop (`SourceTree.cpp`)

- **S-072-R06** `dropEvent`: foreign sources delegate to `QListView`. Self-drops: compute drop row/indicator; empty drop → row = last, indicator = BelowItem. Destination group: the item under the cursor if it is a group, else its parent group — except drops *above* a group (no group) and empty drops (no group). Remember `dropOnCollapsed` from the group's private setting `"collapsed"`. Row is incremented for Below/On/OnViewport indicators. If any selected item is a group and the item below the drop point is not in the same group, the drop-group is cleared; **dropping a group onto another group is invalid** (delegate to base, no-op).
- **S-072-R07** If groups are selected, all their sub-items are added to the index list (groups move as a block) and sub-items belonging to *other* scenes are dropped from the list. Undo snapshot = `main->BackupScene(scene, &sources-of-items-in-other-scenes + dropGroup)`.
- **S-072-R08** Model move: sort the selected indices; for each index in order: `from = row`, `to = r` (running destination), `itemTo = to > from ? to-1 : to`; if `itemTo != from` move the row (`beginMoveRows`/`MoveItem`/`endMoveRows`); `r = from + 1`.
- **S-072-R09** Back-end reorder: build the `obs_sceneitem_order_info` list from the new `items` order — moved non-group items take `group = dropGroup` (when no groups were involved and the index is within the moved range); crossing group boundaries inserts the previous group first (groups stay above their sub-items); a *collapsed* destination group re-inserts its current sub-items before the inserted ones (they stay hidden). Applied as `obs_scene_reorder_items2` **inside `obs_scene_atomic_update`** with `ignoreReorder = true` (the resulting `ReorderItems` signal is suppressed — no model churn).
- **S-072-R10** After reorder: redo snapshot `BackupScene`, push undo/redo action `QTStr("Undo.ReorderSources")` (scene name); if `dropOnCollapsed`, the moved rows are removed from the model (hidden under the collapsed group); `UpdateWidgets(true)`; event accepted with `Qt::CopyAction`.
- **S-072-R11** `selectionChanged`: skipped when both selection and deselection are empty (startup circular-call guard); otherwise `obs_sceneitem_select(item, true/false)` per row (signals blocked on the tree), then `UpdateContextBarDeferred()` + `UpdateEditMenu()`.
- **S-072-R12** Double-click: on an item → `Edit(row)` (rename); on empty area → `AddGroup()`. `GroupSelectedItems()` inserts a group containing the selected base items (`obs_sceneinsert_group` with the selection in reverse order), snapshots the scene first (the group insert itself generates the undo), deselects members, selects the new group, and **queued** `NewGroupEdit(newIdx)`; the group's `item_add`-driven model refresh supplies the row.
- **S-072-R13** `Edit(row)` delegates to the row's `SourceTreeItem::EnterEditMode`. `NewGroupEdit(row)`: if the edit fails to start (should never happen — a warning log with Lain's apology is written and `undo_s.pop_disabled()` runs), the group rename is abandoned.

### 5.3 Source tree row widget (`SourceTreeItem.cpp`)

- **S-072-R14** Construction: reads the scene item's private setting `color-preset` (1 → solid background from the `"color"` string; >1 → `bgColor` property preset-1; else none). Icon: scene icon for scene sources, group icon for groups, `GetSourceIcon(id)` otherwise (16×16, class `source-icon`, muted when invisible). Label = `OBSSourceLabel`; if `obs_source_load_state(unversioned_id)` is disabled/missing the label gets class `text-danger`. Visibility + lock checkboxes (accessible names/descriptions per source).
- **S-072-R15** `Update(force)` classifies the row: Group (is group), SubItem (item's scene ≠ current scene), else Item; when the type changes (or forced) the layout is rebuilt — group rows insert the collapse checkbox (checked iff the group's private `"collapsed"` is true — the checkbox tracks the *collapsed* state; `ExpandClicked` writes it back and drives the model's Collapse/ExpandGroup).
- **S-072-R16** Per-item signal subscriptions: `rename` → update label; `remove`/`destroyed` → clear the widget; visibility signal → `VisibilityChanged` (muted icon/label + checkbox, blocked from re-entry); locked signal → `LockedChanged` (checkbox + `UpdateEditMenu`). The visibility checkbox toggles `obs_sceneitem_set_visible` with an undo/redo action (`QTStr("Undo.ShowSceneItem"/"Undo.HideSceneItem")`, bound to scene-item UUID/id, redo flips it back); the lock checkbox sets `obs_sceneitem_set_locked`.
- **S-072-R17** Edit mode: `EnterEditMode` swaps the label for an event-filtered `QLineEdit`: Escape → queued `ExitEditMode(false)` (revert, line-edit `undo`); Enter/changed → queued `ExitEditMode(true)`. On save, a non-empty changed name applies `obs_source_set_name` with an undo/redo rename action; the label is restored (queued, so the widget isn't destroyed mid-signal).
- **S-072-R18** Hover: `enterEvent`/`leaveEvent` set a hover class and a tooltip (source id + group info); double-click on the row also enters edit mode. `DisconnectSignals`/`ReconnectSignals` manage the obs subscriptions on type changes.

### 5.4 Scene list (`SceneTree.cpp`) and select button (`SourceSelectButton.cpp`)

- **S-072-R19** `SceneTree` is a `QListWidget` of scenes. Grid mode (`SetGridMode`, persisted; `SetGridItemWidth/Height` + getters): in grid mode `resizeEvent` lays items out in a grid (columns = width/item width, `maxWidth`-based), `RepositionGrid` previews drag targets (unselected items shift around the dragged row with the documented offset formula), `dropEvent` computes the target cell (`x = point.x / wid * ceil(wid/maxWidth)`, `y = (point.y + firstItemY) / itemHeight`, `r = x + y * cols`), takes and re-inserts the selected item, toggles the scrollbar policy, re-fires `resizeEvent`, and emits `scenesReordered()` via a **100 ms** `QTimer::singleShot` (Q-2).
- **S-072-R20** `SourceSelectButton`: checkable button = 160×90 `ThumbnailView` (from `App()->thumbnails()` for the source) + name label; pressed → records drag start; a left-button move beyond `startDragDistance() * 3` starts a drag of the source (dropping onto the canvas is S-067). Source `destroy` → queued `sourceRemoved()` signal; `rename` → queued label text update.

### 5.5 Alignment selector (`AlignmentSelector.cpp`, a11y files)

- **S-072-R21** 3×3 grid over a square `gridRect` (side = min(width,height), positioned per the *current* alignment flags). Cell index → alignment: col 0/1/2 = Left/HCenter/Right; row 0/1/2 = Top/VCenter/Bottom (`cellAlignment`). Mouse move tracks `hoveredCell` (highlight); click `selectCell(cell)` → `setAlignment(cellAlignment(cell))` + emit `valueChanged` and `currentIndexChanged`.
- **S-072-R22** Keyboard: arrows move the focused cell within the grid; Space/Return/Enter selects the focused cell; other keys fall through. `setAlignment`/`setCurrentIndex` are the programmatic setters (they recompute the focused cell from the flags).
- **S-072-R23** Accessibility: `AccessibleAlignmentSelector` wraps the widget (role `QAccessible::Grouping`; name/value = the current alignment text; 9 children); `AccessibleAlignmentCell` represents one cell (rect from `cellRect`, name/value = the translated alignment name, e.g. `Basic.TransformWindow.Alignment.TopLeft`; selected state for the active cell).

### 5.6 Source toolbars (`SourceToolbar.cpp` + the per-source toolbars)

- **S-072-R24** `SourceToolbar` base holds a weak source ref and the source's properties. `SaveOldProperties(source)`: snapshots current settings + `"undo_suuid"` = source UUID into `oldData`. `SetUndoProperties(source, repeatable)`: with a snapshot present, pushes an undo/redo action on the current scene's undo stack whose undo/redo callbacks JSON-parse the snapshot, look the source up by UUID, and `obs_source_reset_settings` it; without a snapshot an error is logged (should not happen).
- **S-072-R25** `ComboSelectToolbar` helpers: `FillPropertyCombo(combo, prop, cur_id, is_int)` fills name + id-data entries, returns the current value's index (-1 if absent). `UpdateSourceComboToolbarProperties`: blocks signals, refills from the property, preserves the current index; if absent → inserts `QTStr("Basic.Settings.Audio.UnknownAudioDevice")` at 0 and selects it (disabled); a property-disabled current item is disabled in the combo.
- **S-072-R26** Platform device toolbars all derive `ComboSelectToolbar` and in `Init()` delete the activate button, then set the label from the platform capture module's locale text and the property to drive: `AudioCaptureToolbar` (module `win-wasapi`/`mac-capture`/`linux-pulseaudio`, prop `device_id`), `DisplayCaptureToolbar` (`win-capture`/`mac-capture`/`linux-capture`), `WindowCaptureToolbar` (same modules; Linux-only extras for the window-list source), `ApplicationAudioCaptureToolbar` (`win-wasapi` only). Combo change → `SaveOldProperties`, update the `device_id` setting, `SetUndoProperties`.
- **S-072-R27** `DeviceCaptureToolbar` (video capture): no device combo — reads `active` from settings, button text Activate/Deactivate from the `win-dshow` module locale; `on_activateButton_clicked` flips the `active` setting (with undo snapshot).
- **S-072-R28** `GameCaptureToolbar`: `capture_mode` combo (window/monitor) + `window` combo; the window row is visible only when mode = `window` (`UpdateWindowVisibility`); mode/window changes go through `SaveOldProperties` → `obs_source_update` → `SetUndoProperties`; a disabled current window entry is shown disabled.
- **S-072-R29** `ColorSourceToolbar`: reads `color` (int) via `color_from_int` (0xAARRGGBB ↔ QColor helpers shared with the text toolbar); the colour swatch shows the hex name + preview background; "Choose" opens a `QColorDialog` (title from the property description) and writes `color` back with undo.
- **S-072-R30** `TextSourceToolbar`: detects freetype (`text_ft2_source`) vs GDI text — freetype uses settings keys `from_file`/`color1`, GDI `read_from_file`/`color`. Single-line mode (no newline and not read-from-file) uses a line edit, otherwise a text area. Font dialog (`on_selectFont_clicked`, writes the `font` object), colour dialog (`on_selectColor_clicked`), and text edits update the source with the undo snapshot/restore.
- **S-072-R31** `ImageSourceToolbar`: "Browse" picks an image file (filter from the property), writes `file` with undo. `BrowserToolbar`: "Refresh" re-applies the source settings (reload) with undo.

### 5.7 Advanced audio control (`OBSAdvAudioCtrl.cpp`)

- **S-072-R32** Per-source row, constructed from the source: 11 signal subscriptions (`activate`, `deactivate`, `audio_activate`, `audio_deactivate`, `volume`, `audio_sync`, `update_flags`, `monitor`, `audio_mixers`, `audio_balance`, `rename`). Widgets: icon + name + Active/Inactive label (danger style when active); dB spinbox (MIN_DB − 0.1 … MAX_DB, step 0.1, suffix " dB", special text "-inf dB" below MIN_DB); percent spinbox (0…2000, suffix "%"); the two are a stacked pair switched by user config `[BasicWindow] AdvAudioVolumeType` (dB vs %); mono checkbox (`OBS_SOURCE_FLAG_FORCE_MONO`); balance slider (0…100, tick at 50; disabled when channel setup is Mono or the source's speaker layout ≠ stereo; double-click resets to center); sync-offset spinbox (−950…20000 ms); monitoring checkbox (enabled iff `obs_audio_monitoring_available()`); six track checkboxes (bits 0…5 of `obs_source_get_audio_mixers`).
- **S-072-R33** Signal handlers refresh the matching widgets (volume → both spinboxes; sync → spinbox; flags → mono checkbox; monitor → checkbox; mixers → the six checkboxes; balance → slider; activate/deactivate → status label; rename → name label), each guarded against re-entrancy from their own slots.
- **S-072-R34** Slots: dB → `obs_source_set_volume(obs_db_to_mul(v))`; percent → `set_volume(v/100)`; mono → set/clear the force-mono flag via `update_flags`; balance → `obs_source_set_balance_value(v/100)`, double-click → reset to 0; sync → `obs_source_set_sync_offset(ms)`; monitoring → `obs_source_set_monitoring_type`; track i → `setMixer(source, i, checked)` (updates the `audio_mixers` bit). `ShowAudioControl(grid)` appends the row's widgets to the caller's grid layout.

### 5.8 Missing files (`MissingFilesModel.cpp`, `MissingFilesPathItemDelegate.cpp`, `OBSMissingFiles.cpp`)

- **S-072-R35** Model rows = `(originalPath, sourceName, newPath)` with state Missing/Found/Replaced/Cleared; `found()` counts non-Missing rows; `data()` renders state text/icon per state; a row is added via `setData` with `NewPathsToProcessRole` (value = [path, source] list, inserted after the current row; backslashes normalised to `/`).
- **S-072-R36** `setData` on the NewPath column: empty path → Missing; `QTStr("MissingFiles.Clear")` → Cleared; same file name as original → Found; different name → Replaced; then — only when not already inside a search (`recursionLevel == 0`) — re-runs the automatic search from the new path.
- **S-072-R37** Automatic search (`findAllFilesInPath`/`fileCheckLoop`): for the directory of the new path, every still-Missing row whose file name exists in that directory is auto-assigned; the first such hit (when `skipPrompt` is false) prompts `QTStr("MissingFiles.AutoSearch")` once (No aborts the auto-assignment). Subdirectories are recursed (prompting disabled) only while the current no-match depth ≤ 2 — the search descends at most a few levels through directories that contain no matches. All rows found → `allFilesFound` (search stops).
- **S-072-R38** Editor delegate: row editor = line edit + "..." browse button (file dialog, sets the path) + clear button (close icon, sets the special Clear value). The `OBSMissingFiles` dialog wraps the model table with buttons: Browse (folder search, `browseFolders`), Done (`saveFiles` — writes each resolved path back to its source's settings and closes), Cancel; a warning icon is shown while files are missing (`dataChanged` drives it).

### 5.9 Extra browser docks (`ExtraBrowsersModel.cpp/.hpp`, `ExtraBrowsersDelegate.cpp`, `OBSExtraBrowsers.cpp`)

- **S-072-R39** Model: `Reset()` mirrors `main->extraBrowserDocks` (title/url, `prevIdx` = original index); `Init()` adds a delete button per row; `CheckToAdd()` auto-inserts an empty row when needed; `TabSelection(forward)` moves focus Title↔Url with row wrap; `DeleteItem` records the row in `deleted` and clears it.
- **S-072-R40** `Apply()`: existing rows (`prevIdx != -1`) update title/url in place; new rows are added with a generated UUID (QUuid with dashes/braces stripped); deleted rows are removed from the three `main->extraBrowser*` vectors in reverse index order; if no docks remain the menu separator is cleared; then `deleted.clear()` + `Reset()`.
- **S-072-R41** Delegate: title/url editors with `ValidName` validation (non-empty; title rules); Escape reverts, Enter commits; the delete column renders the trash button. Dialog: `WA_DeleteOnClose`, columns stretched except the fixed delete column, edit on focus change; **Apply runs both on the Apply button and on close** (R40 semantics).

### 5.10 Small widgets (`FlowLayout.cpp`, `FlowFrame.cpp`, `FocusList.cpp`, `MenuButton.cpp`, `UrlPushButton.cpp`, `ClickableLabel.hpp`, `OBSSourceLabel.cpp`, `VisibilityItemWidget.cpp`, `VisibilityItemDelegate.cpp`, `SceneRenameDelegate.cpp`, `SourceTreeDelegate.cpp`)

- **S-072-R42** `FlowLayout`: standard Qt flow layout — items wrap at the layout width honouring margins/spacing; `hasHeightForWidth` = true; `sizeHint` = `minimumSize` (max of item minimums + margins); `doLayout(rect, testOnly)` computes row placement. `FlowFrame` = QFrame hosting a FlowLayout plus keyboard focus navigation: arrow keys move focus between child widgets (focus-proxy aware) in flow order.
- **S-072-R43** `FocusList` (QListWidget): emits `GotFocus` on focus-in; drag-move is ignored when dragging onto the row directly below the current row or past the last row (prevents wrap-around reorders, Q-4), otherwise default list behaviour.
- **S-072-R44** `MenuButton` (QPushButton with menu): Enter/Return emit `clicked`; Down/Space open the menu; a mouse press in the rightmost 30 px (arrow zone) opens the menu directly. `UrlPushButton`: a button carrying a target `QUrl`; clicking opens it in the default browser. `ClickableLabel`: a `QLabel` whose left-mouse press emits `clicked()` (event accepted).
- **S-072-R45** `OBSSourceLabel`: a label that follows its source — `rename` → queued text update, `remove`/`destroyed` → queued hide/clear; a mouse press emits `clicked()` (button not filtered; the consumer selects the source's scene item when it is in a scene). `VisibilityItemWidget`: the small "eye" icon for list rows; subscribes to the source's `enable` signal (queued) and toggles the checkbox state (`vis->setChecked`); `SetColor` only handles the active/selected paint state, not the enabled flag. `VisibilityItemDelegate`: paints the icon with palette-correct highlight colours per platform (active/inactive palette group on Win/Mac; highlight text role differs on Win vs others).
- **S-072-R46** `SceneRenameDelegate` (scene list inline rename): editor text is pre-selected; Escape reverts (line edit `undo`); Tab/Backtab pass through for focus travel. `SourceTreeDelegate`: `sizeHint` delegates to the row's `SourceTreeItem` widget size hint (keeps row heights in sync with the custom widgets).

### 5.11 Small dialogs (`NameDialog.cpp`, `OBSAbout.cpp`, `OBSLogViewer.cpp`, `LogUploadDialog.cpp`, `OBSPermissions.cpp`, `OBSUpdate.cpp`, `OBSIdianPlayground.cpp`)

- **S-072-R47** `NameDialog`: window-modal, fixed width 555, label + line edit (+ optional checkbox). `AskForName(parent, title, text, &out, placeHolder, maxSize)`: `maxSize` clamped — ≤0 or >32767 → **170**; text pre-selected; on OK the input is trimmed of leading/trailing spaces/tabs (`CleanWhitespace`) and returned; on Cancel false. `AskForNameWithOption` adds the checkbox (extra return value).
- **S-072-R48** `OBSAbout`: About/Authors/License tabs. `ShowAbout` builds the top-patrons HTML from `main->patronJson` (JSON array of `{name, link, amount}`): patrons with `amount >= 5000` get the large top-tier style, the rest normal lines, links HTML-escaped; empty/missing patron JSON → no patrons section. Authors from the credits file, license from the bundled license.
- **S-072-R49** `OBSLogViewer`: loads the current session log (`<config>/obs-studio/logs/<GetCurrentLog()>`) into the text area (one block per line, scrolled to the bottom); live lines appended via `AddLine(type, str)` with HTML escaping and colour — warning `#c08000`, error `#c00000`, else default; the "show startup log" checkbox persists to user config `[LogViewer] ShowLogStartup`; "Open" opens the log folder.
- **S-072-R50** `LogUploadDialog`: starts an upload through the app (`startLogUpload`), watches `OBSApp::logUploadFinished` (shows the URL, enables copy/analyze) and `logUploadFailed` (shows the error, enables retry); copy puts the URL on the clipboard, analyze opens it in the browser.
- **S-072-R51** `OBSPermissions` (macOS only): four permission rows — screen recording, camera, microphone, input monitoring — plus an accessibility row shown only when input monitoring is not authorized. Per status: authorized → "Access granted" (button disabled); not-determined → "Request access"; otherwise → "Open preferences" (named pane); clicks request the permission or open System Settings; Continue closes.
- **S-072-R52** `OBSUpdate`: yes/no/skip buttons → dialog codes Yes/No/Skip (accept = Yes, reject = No). For a *manual* update check the skip button is removed and "No" relabelled "Cancel".
- **S-072-R53** `OBSIdianPlayground`: developer-only style-test dialog (row lists, info labels, dropdowns, toggle switches incl. a delayed toggle with a 1 s async status flip, checkboxes, list headers). No product behaviour; kept for theme development.

## 6. Algorithms

Source-tree drop reorder (R06–R10) — summary of the move computation:

```
row  = drop row (empty drop → last, indicator BelowItem)
group = item-under-cursor if group else its parent group
        (none if indicator==AboveItem on a group, or empty drop)
row++  for indicator in {Below, On, OnViewport}
if selected has groups:
    if itemBelow not in same group: group = none
    if group set: invalid drop → abort
expand selected groups into their sub-items (block move)
undo = BackupScene(scene, [sources of out-of-scene items] + group)
r = row
for idx in sorted(selected):
    to = r; itemTo = (to > idx) ? to - 1 : to
    if itemTo != idx: moveRow(idx → to) in items
    r = idx + 1
orderList = rebuild from items (moved range takes group;
            group boundaries inserted first; collapsed group's
            existing sub-items re-inserted ahead of the moved ones)
with ignoreReorder: atomic_update(scene, reorder_items2(orderList))
redo = BackupScene(...); push undo/redo "Reorder sources"
if dropOnCollapsed: remove moved rows from model
UpdateWidgets(force); accept event
```

## 7. Concurrency

- All widgets GUI-thread. Obs signals (source rename/remove/destroy,
  scene changes, frontend events) arrive on obs threads and are queued
  (`QMetaObject::invokeMethod`, QueuedConnection) before any widget
  mutation — used uniformly (R16/R17/R20/R45).
- Scene mutations are wrapped: reorder in `obs_scene_atomic_update`
  (R09), group inserts via the scene's own atomic item-add path; the
  `ignoreReorder` flag decouples model echo from user-initiated moves.
- `MissingFilesModel` recursion is single-threaded (GUI thread);
  `recursionLevel` guards re-entrant auto-search (R36–R37).

## 8. Reference defects and quirks

- **Q-1** `SourceTree::NewGroupEdit` contains a warning log describing an "impossible" state (edit not processing) — preserve the log line and the `undo_s.pop_disabled()` compensation; the state *can* occur (comment says so).
- **Q-2** `SceneTree::dropEvent` emits `scenesReordered()` 100 ms after the drop (single-shot timer) — consumers must treat the signal as asynchronous.
- **Q-3** `OBSAbout`'s top-patron tier threshold is a hard-coded `amount < 5000` (currency units); patrons exactly at 5000 stay in the top tier.
- **Q-4** `FocusList` only special-cases dragging onto *the row directly below* the current row (and past-the-end); other wrap positions use default QListWidget behaviour.

## 9. Implementation notes (non-normative)

- The per-row custom widgets (index widgets) are the reference's answer
  to "custom QListView rows"; in a Rust port: model = ordered scene
  items + group-collapsed filtering, view = virtualised list with
  checkbox/lock/label cells.
- Keep the `ignoreReorder`/atomic-update pairing exactly (R09) — without
  it the model reorders itself on the echo of the user's own move.
- `SourceToolbar` undo snapshots (R24) are the *only* undo path for the
  toolbars; don't add direct `obs_source_update` without a snapshot.
- Toolbars are source-id-keyed (created per source id by the main
  window); this spec pins their behaviour, not their instantiation table
  (S-069).

## 10. Conformance coverage

| Rule(s) | Scripts |
|---|---|
| all S-072-Rxx | none yet (Qt UI; no executable vectors) |

## 11. Open questions

1. `SourceTreeModel::data()` roles beyond `AccessibleTextRole` (icon/tooltip are served by the row widget; the model may expose more in edge paths) not fully enumerated.
2. `OBSMissingFiles::saveFiles`' exact per-source setting key writes (which sources accept `file` updates, how group/filter paths are rewritten) not transcribed line by line.
3. `LogUploadDialog`'s upload transport (endpoint, auth) lives in `OBSApp` (S-074 territory); only the dialog's state machine is pinned.
4. `TextSourceToolbar` font-dialog handling of the `font` data object (which fields are offered per freetype version) not detailed.
5. `DeviceCaptureToolbar`'s activate path assumes the `win-dshow` module; the exact settings keys toggled beyond `active` (device re-init parameters) not read in full.
6. Round 10 second-author check (2026-11-05): corrected R15 (collapse checkbox is checked iff private `collapsed` is true — it tracks the collapsed state, was inverted), R17 (rename API is `obs_source_set_name`, not `obs_source_set_source_name`), R23 (a11y role is `QAccessible::Grouping`, not Slider; cell text is the translated alignment name, not "Align <flags>"), R45 (`OBSSourceLabel` emits `clicked()` on any mouse press; `VisibilityItemWidget`'s enable signal toggles the checkbox — `SetColor` is paint state only).
