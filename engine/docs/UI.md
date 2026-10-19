# UI system (`ui` module)

A small retained-mode widget toolkit for menus, the HUD, the inventory and debug tools. It
depends on `core` only. Drawing, the clipboard and script calls are injected through interfaces,
and input comes in as device-independent `UiEvent`s. That means every widget can be tested
headlessly, and the module never touches GL, Vulkan, GLFW or AngelScript.

```
game ── HudModel / callbacks ──> GameUi ──> ui::UiRoot (widget tree, layers, focus)
  │                                          │ paint(IUiRenderer&)
  │  InputEvents ─ translateUiEvents ─> UiEvent        └──> HudUiRenderer (render) ─> HudLayer::draws
  │                                                                  └─> buildHudVertices ─> gl33 / Vulkan
  └─ ScriptEngine <── UiScriptBindings (ui::IScriptHost + ui_* script API)
```

## Pieces

| Header | Contents |
|---|---|
| `geometry.h` | `Vec2`, `Rect`, `Edges` (padding / margins / insets), `Color` |
| `events.h` | `UiEvent` (pointer move / down / up, wheel, key down incl. repeat, text), `Key`, modifiers |
| `ui_renderer.h` | `IUiRenderer` (fillRect, drawText, drawImage, push / popClip), `IClipboard`, `MemoryClipboard` |
| `theme.h` | `Style` per widget class, `Theme` (JSON parse, font metrics, scale) |
| `painter.h` | `Painter` (renderer + theme + `ClipScope`) |
| `widget.h` | `Widget` (tree, `Layout`, events, measure / arrange / paint / input hooks), `anchoredRect` |
| `ui_root.h` | `UiRoot` (root, popups, modals, focus, hover, capture, keyboard navigation, tooltips), `IScriptHost` |
| `controls.h` | Panel, Label, Button, Image, Checkbox (toggle style), ProgressBar, Slider, NumberField, Slot |
| `containers.h` | StackPanel, GridPanel, ScrollBar, ScrollView, Splitter, TabView |
| `text_buffer.h` | `TextBuffer`: the text model (positions, selection, editing, undo / redo) |
| `text_edit.h` | TextBox (single line), TextEditor (multi-line, line numbers) |
| `lists.h` | ListView, TreeView, Dropdown |
| `menus.h` | MenuItem, Menu, MenuBar, ContextMenu |
| `dialog.h` | Dialog (modal), Tooltip |
| `widget_factory.h` | `buildWidget(json)`: widget trees from JSON |
| `ui_selftest.h` | headless selftests |

## Widget tree and layout
Each `Widget` owns its children (`std::unique_ptr`), so `add<T>(args...)` constructs a child in
place and returns a reference to it. Each widget has a **style class** (its theme entry), an
optional `id` (for `find`, scripts and JSON), `visible` / `enabled`, a tooltip, and a `Layout`:

- **Anchors** (the default parent behaviour): on each axis, if `anchorMin == anchorMax`, the
  widget is a box of `size` (or its measured size) whose `pivot` sits on that fraction of the
  parent's content rect, moved by `offset`. Otherwise it stretches between the two anchors,
  inset by `insets`. `Layout::fill(insets)` and `Layout::at(anchor, pivot, offset, size)` cover
  the common cases. Example: `at({0.5, 1}, {0.5, 1}, {0, -6})` is bottom centre, 6 px up.
- **StackPanel** (vertical / horizontal) puts its children one after another with `spacing`.
  Along the stack axis, each child gets its size plus a `flex` share of the leftover space.
  Across the axis it stretches (or keeps its size with `setStretch(false)`). `margin` is
  honoured.
- **GridPanel** lays out fixed-size cells in `columns` columns, row-major, with `spacing`.
- **Padding** comes from the theme style (`Style::padding`) and shrinks the content rect that
  children are placed in.

`measure()` returns a widget's preferred outer size and `arrange()` places its children. UiRoot
lays out the whole tree, then the modal and popup layers, from `layout(viewport)`. Layers are
anchored to the screen and kept on it.

## Theme (JSON through the PackStack)
`ResourceManager` reads `ui/theme.json` from the pack stack (the highest pack wins), and F9
reloads it. LithoKraft's file is `LithoKraft/assets/ui/theme.json` (game pack). If the file is missing or invalid, the
built-in `Theme::defaults()` is used (with a warning).

```json
{"glyph": [8, 16], "scale": 1,
 "styles": {"default": {"background": [0.12, 0.12, 0.14, 0.92], "text": "#ebebeb", "padding": 4},
            "button":  {"hover": "#3a3a44", "border_width": 1},
            "fancy":   {"accent": "#00ff0080"}}}
```

Style keys: `background hover pressed disabled text text_disabled accent border` (colours as
`[r,g,b]`, `[r,g,b,a]` or `#rrggbb[aa]`), `border_width`, `padding` (a number or
`[l,t,r,b]`), `text_scale`. Built-in classes keep their own look unless a pack overrides them;
new classes start from the (possibly overridden) `default`. Unknown keys and bad values are
errors, so typos don't fail silently. Classes in use: `default panel hud label button
checkbox slider progress textbox editor list menu tooltip slot scrollbar tabs` plus the
transparent layout classes `none root stack scrollview splitter image`. The font is the
monospaced 8x16 HUD font. The game sets `Theme::scale` to 2 for windows 900 px or taller.

## Rendering
Widgets draw through `Painter` into an injected `IUiRenderer`. The engine implements it once:
`litholattice::HudUiRenderer` (render module) turns draws into ordered `HudDraw`s on the frame's
`HudLayer`, and clips them on the CPU (rects are cut, glyphs are cut with their UVs
interpolated). Both backends already draw the HUD layer through the shared
`buildHudVertices` (overlay geometry), so **gl33 and Vulkan render the UI with no
backend-specific code**. UI draws come after the legacy rect / text lists, in paint order, so
overlapping windows stack correctly. `drawImage(name)` is resolved by a callback: the game maps
`block:<id>` to the block's material swatch, and unknown names draw magenta. The HUD has no
texture path yet, so icons are swatches; see "Not done yet". The Vulkan overlay buffer is
4 MB per frame (about 130k HUD vertices, enough for a full-screen editor).

## Input, focus and navigation
The game turns each frame's raw `InputEvent`s into `UiEvent`s (`game/ui_input_bridge`), and
key repeat becomes repeated `KeyDown`s. `UiRoot::handleEvent` returns whether the UI consumed
the event:

- **Hover** is the deepest interactive widget under the pointer (top layer first).
- **Capture**: the widget that accepts a `PointerDown` gets every move until `PointerUp`
  (sliders, scroll bars, splitters, text selection and dialog dragging keep working outside
  the widget).
- **Focus**: a click focuses the nearest focusable ancestor of the target. Keys go to the
  focused widget and bubble up through its parents until one handles them.
- **Keyboard navigation** for unhandled keys: Tab / Shift+Tab cycle through the focusable
  widgets of the active layer in tree order. The arrow keys move to the nearest focusable
  widget in that direction (for grids of slots, for example). Escape closes the top popup, else
  offers itself to the top modal (a dialog closes), else drops focus. Disabled and hidden
  widgets are skipped, and hiding a widget clears focus, hover and capture inside it.
- **Popups** (menus, dropdown lists) close on an outside click (which is not passed on).
  **Modals** (dialogs) block input below them and dim the screen. Focus stays inside the top
  modal.
- **Tooltips** show for the hovered widget after 0.6 s.
- `wantsKeyboard()` is true while a modal or popup is open or something has focus.

Game integration: input contexts gain `UiText` (opaque, wants text). The game pushes it while
the script editor is open or a text field has focus, so letters never move the player or open
the inventory. `Debug` stays on top, so F-keys still work. The `Inventory` context now only
keeps Inventory and Pause: clicks reach the inventory as UI events instead of a Break action.
With the mouse captured for play, only keyboard events reach the UI.

## Events and scripts
`widget.on("click", fn)` adds a C++ handler. `widget.onScript("click", "void onPlay(const
string &in)")` names an AngelScript function; it is called with the widget id through
`UiRoot`'s `IScriptHost`. Events: Button / Slot `click` (Slot also `rightclick`); Checkbox,
Slider, NumberField, Dropdown, TabView, TextBox and TextEditor `change`; TextBox and NumberField
`submit` (Enter); ListView and TreeView `select` / `activate`; TextEditor `save` (Ctrl+S);
Dialog `close`. Menu items run a C++ `action` and / or a `script` handler (called with the item
label).

`game/UiScriptBindings` implements `IScriptHost` on `ScriptEngine::callWithString`, and
registers this sandboxed API (widgets by id, nothing else):

```
string ui_text(const string &in id)            void ui_setText(const string &in id, const string &in text)
bool   ui_checked(const string &in id)         void ui_setChecked(const string &in id, bool on)
float  ui_value(const string &in id)           void ui_setValue(const string &in id, float v)
void   ui_setVisible(const string &in id, bool) void ui_setEnabled(const string &in id, bool)
bool   ui_show(const string &in layoutJson)    void ui_close(const string &in id)
```

`ui_show` builds a JSON layout (below) and adds it to the screen. Its `"on"` handlers name
script functions. A handler may close its own widget (`ui_close` defers destruction until no
widget code is running).

## JSON layouts (`buildWidget`)
```json
{"type": "panel", "id": "menu", "layout": {"anchor_min": [0.5, 0.5], "anchor_max": [0.5, 0.5], "pivot": [0.5, 0.5]},
 "children": [
   {"type": "stack", "spacing": 6, "children": [
     {"type": "label", "text": "Settings"},
     {"type": "checkbox", "id": "fly", "text": "Flight", "toggle": true, "checked": true},
     {"type": "number", "id": "signal", "min": 0, "max": 64, "value": 15},
     {"type": "button", "id": "ok", "text": "OK", "on": {"click": "void onOk(const string &in)"}}]}]}
```
Common keys: `type id style tooltip visible enabled layout on children`. The `layout` keys are
`anchor_min anchor_max insets offset pivot size min_size margin flex`. Types and their own
keys: `widget`, `panel`, `label {text}`, `button {text}`, `image {image, size}`,
`checkbox {text, checked, toggle}`, `progress {value}`, `slider {min, max, value, step}`,
`number {min, max, value}`, `slot {size, image}`, `stack {orientation, spacing}`,
`grid {columns, cell, spacing}`, `textbox {text, placeholder}`, `editor {text, line_numbers}`,
`list {items, selected}`, `dropdown {items, selected}`, `scroll` (exactly one child),
`splitter {orientation, ratio}` (two children), `tabs {titles}` (one title per child). Unknown
types and keys are errors, and the error message includes the JSON path (`ui/panel[0]: unknown
key 'txt' for type 'label'`).

## Controls
- **Panel**: a background, border and padding around anchored children.
- **Label**: multi-line text. Optional colour and centring.
- **Button**: clicks on release inside the button, or on Enter / Space.
- **Image**: a named image.
- **Slot**: an inventory cell with an icon, caption, selection ring and an int payload.
- **Checkbox / Toggle**: click or Enter / Space flips it.
- **Slider**: drag (captured), wheel, arrow keys, Home / End. Optional step.
- **NumberField**: an integer field with - / + buttons. You can type digits (clamped while
  typing; Enter or losing focus commits). Up / Down step by 1 (Shift: 8), PageUp / PageDown jump
  to max / min, and the wheel steps too. The game uses 0..64 for signal strengths.
- **ProgressBar**.
- **ScrollBar** and **ScrollView**: vertical, using the wheel, a draggable thumb, page clicks and
  `ensureVisible`. Content is clipped.
- **TextBox**: a single line that scrolls horizontally. Enter emits `submit`. Optional
  placeholder.
- **TextEditor**: multi-line with a line-number gutter and a highlighted current line number.
  It scrolls in both directions. Tab inserts spaces to the next multiple of 4 (Shift+Tab still
  navigates out). Enter keeps the line's indentation. PageUp / PageDown, Ctrl+Home / End, and
  Ctrl+S emits `save`.
- Shared text keys: the arrows (Shift selects, Ctrl moves by word), Home (toggles between the
  first non-blank character and column 0), End, Backspace / Delete, and Ctrl+A / C / X / V / Z /
  Y. A mouse click places the caret, Shift+click extends the selection, and dragging selects.
  Consecutive typing or deleting is one undo step.
- Text position API: `TextBuffer::offsetOf / posAt / clamp / caret / selection / select`, and
  `TextEditBase::positionAt(point)` / `caretRect()`.
- **Dropdown**: opens a popup Menu of its items. Enter / Space opens it; Up / Down change the
  selection directly.
- **TabView**: a header of tabs over one visible page. Click or Left / Right switch tabs.
- **Tooltip**: drawn by UiRoot for `tooltip()`; also usable standalone.
- **ListView**: rows with a selection. Keyboard: Up / Down / PageUp / PageDown / Home / End.
  Enter, or a click on the selected row, activates.
- **TreeView**: expand / collapse with + / -. Right expands or enters a node, Left collapses or
  goes to the parent. The selection is a path of child indices.
- **Menu / MenuBar / ContextMenu**: separators, shortcut text, check marks, disabled items and
  submenus (Right opens one, Left / Escape closes it). Keyboard: Up / Down / Enter. In a bar,
  hovering another title or pressing Left / Right switches menus.
  `ContextMenu::open(ui, pointer, items)` opens a menu from a `rightclick` handler.
- **Dialog**: modal, with a title bar (drag to move), a body stack and a button row. Escape
  closes it unless `setCloseGuard` says not yet. `Dialog::show(ui, title)`.
- **Splitter**: two panes with a draggable divider (horizontal or vertical).

## In the game
`game/GameUi` owns the `UiRoot` and the game screens. Each frame the game hands it a
`HudModel` (hotbar contents, debug lines, controls lines, the keyboard-capture banner) and
reacts to callbacks (slot selected, inventory block picked, inventory closed):

- **HUD**: the F3 stats panel (top left), the H controls panel (bottom left), the keyboard
  capture banner, the hotbar (9 Slots, bottom centre; click a slot to select it while the mouse
  is free) and the selected block's name. Everything is on the UI now: the game no longer
  builds HUD rects by hand.
- **Inventory (E)**: a modal Dialog with a scrollable GridPanel of Slots for every block.
  Click puts the block in the selected slot, Shift+click puts it in the first free slot. The
  hovered block's name and palette info appear below the grid. E or Escape closes it.
- **Script editor (F10, debug)**: `game/ScriptEditor`. It is a Dialog with a MenuBar (File:
  Save + reload, Reload from disk, Close), a Splitter with a ListView of `scripts/*.as` and a
  TextEditor, and a status line (file, modified mark, line / column). Ctrl+S writes the file
  and calls `ScriptEngine::reload()`. The new scripts are built into a staging module and
  swapped in only if they compile, then `onLoad()` runs again. On a compile error the old
  scripts stay active, the first error is shown and the caret jumps to its line. Switching
  files or closing with unsaved changes asks first, in a nested modal. The editor only opens
  and writes files it listed itself from the scripts folder; it never takes a path from the
  user. It edits the `scripts/` folder the game loaded: in dev builds the source tree's
  `LithoKraft/scripts`, in shipping builds the copy next to the exe (`--scripts` overrides; see
  ARCHITECTURE 0b).

## Tests
`--selftest` runs the ui suite headlessly (`ui:` lines): anchors, stacks (flex, margins),
grids, padding, label measuring; theme parsing (inheritance, hex colours, and rejecting
unknown keys and bad values); Tab / Shift+Tab, skipping disabled widgets, spatial arrows,
click-to-focus, hide-clears-focus, Escape; text editing (positions, insert, undo coalescing,
redo, joining lines, word selection, multi-line replace, auto-indent, Home, preferred column,
single-line buffers); TextEditor (Tab, select all / copy / paste, undo, Ctrl+S, `positionAt`
round trip, cut, line numbers, balanced clip) and TextBox; events (button click vs release
outside, Enter, script handler ids, checkbox, slider capture, NumberField clamping, list, tree,
dropdown); layers (menu bar, submenu, switching menus, outside click, context-menu script
item, modal blocking, Tab confined to the dialog, button / Escape close, tooltip delay); and
the JSON factory (properties, handlers, errors with paths).

The `game ui:` lines cover the game side: HudUiRenderer ordering and clipping, swatch / magenta
images, script bindings (a handler that reads and writes widgets, `ui_show` / `ui_close`), the
script editor (open, Ctrl+S save + hot reload, a compile error keeping the old scripts and
jumping to the error, Escape asking about unsaved changes), and the HUD / hotbar / inventory
screens.

## Not done yet
- Images are solid swatches (`block:<id>`). Real texture icons need a textured quad path in the
  HUD shaders of both backends.
- Text is ASCII only: the baked font has no Unicode, and other characters are swallowed.
- No gamepad cursor or focus-by-gamepad yet (the D-pad could drive `focusDirection`).
- No layout or animation transitions, and no per-widget fonts.
- A widget-level "rightclick → ContextMenu" is wired by hand per widget. There is no global
  context-menu event yet.
