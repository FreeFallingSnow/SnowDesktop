---
name: snowdesktop-lua-widget
description: Create, modify, debug, validate, and package SnowDesktop API v2 Lua desktop widget folders with widget.json and main.lua, package resources, localization, storage, drawing, context, and the snowwidget CLI.
---

# SnowDesktop Lua Widget

Create widgets against the sandboxed API v2 contract. A runnable widget is one
validated package directory, never a loose `.lua + .widget.json` pair:

```text
my-widget/
├── widget.json
├── main.lua
├── assets/
├── modules/
└── LICENSE
```

Built-ins live under the executable `widgets` directory. Installed and
development packages live under `data\widgets\installed` and
`data\widgets\dev`. Layouts store the immutable package UUID, not a path.

## Workflow

1. Resolve this Skill directory and run `bin\snowwidget.exe capabilities`.
   Treat the returned JSON as the available CLI contract. If that bundled CLI
   predates API v2, use the repository/runtime v2 contract and refresh the CLI
   before distributing the Skill.
2. Copy `assets/widget-template` as a complete package directory.
3. Generate a new UUID for `id`, choose a lowercase hyphenated `slug`, and keep
   the UUID across all versions and channels.
4. Keep `schemaVersion` and `apiVersion` at `2`. Implement exactly one local
   `render` or `view` function and return it through `widget.define(...)` from
   `main.lua`. Require `view.tree.core` when using the declarative subset.
5. Put every user-visible string behind a literal `l10n.tr("key")`. Add every
   key to every locale catalog in `widget.json`; never put component strings in
   the host `lang/` directory.
6. Derive geometry from `layout.width/height`, `layout.cu/fontCu`, and
   `widget.context()`. Test multiple spans, DPI values and preview mode.
7. Declare package images/fonts in `resources`, create their handles while the
   entry script loads, and pass only handles to v2 draw functions.
8. Load package modules only with `module.require("modules/name.lua")` while the
   entry script is loading.
9. Add only features and permissions used by the component. Basic time,
   context, drawing, localization and package resources require no high-risk
   permission.
10. Use `state` for JSON-like VM-lifetime values and `storage` for persistent
    strings. Write persistent values only when they change.
11. Run `snowwidget validate <directory>` and
    `snowwidget pack <directory> <name.snowwidget>`.
12. In the repository, also run `scripts\test.bat`, the standard Release build,
    and `scripts\widget-dev.bat <directory>` for transactional hot reload.

Read `references/api-v2.md` completely before implementing API calls, features,
resources or troubleshooting. Use `library/snowdesktop-v2.lua` as the LuaLS
library. Read `references/package-v1.md` only when diagnosing or migrating an
old schema/API v1 package; do not create new v1 packages.

## Required entry

```lua
local function render()
    local padding = layout.cu(12)
    draw.text(padding, padding,
        l10n.tr("lua_widget.my_widget.hello"),
        layout.fontCu(15), 0xFFFFFF,
        layout.width() - padding * 2)
end

return widget.define({
    name = l10n.tr("lua_widget.my_widget.name"),
    render = render,
})
```

Do not define API v1 globals such as top-level `render`, `onClick`,
`getContextMenu`, `imguiRender`, or `onHttpResponse`. The current host
supports optional `setup(context)` and `dispose(context, model, reason)`;
`setup` runs once and its return value is passed to `render` or `view`, `event`, and
`dispose`. Optional `event(context, model, event)` receives host surface events;
immediate-mode elements use `interaction.region`. The transitional
`view.tree.core` subset supports box/row/column/grid/flow/stack/text/image/
button/icon/iconButton/shape/progressBar/progressRing/spacer nodes, stable element actions,
package resource handles, hover/pressed styles, and per-element context-menu
bindings. Probe `view.dataSeries` for bounded sparkline/lineChart/barChart/
waveform/spectrum nodes; keep each series within 512 finite samples and the
whole tree within 4096, and always provide `accessibility.label`. The subset
also publishes `view.statusVisuals` for badge/divider/meter; give every meter
an `accessibility.label` and use meter only for a current reading, not task
progress. Probe `view.selectionControls` for controlled toggle/checkbox nodes:
always pass an explicit `checked` value and handle their `change` action by
updating component-owned state; never bind `click` or assume the host persists
the proposed value.
Probe `view.actionControls` for host-rendered link/radioGroup/slider nodes.
Treat radioGroup and slider as controlled: update component-owned state from
`selection` or `controlValue`, then invalidate. Radio options use generated
`<group-key>/<option-key>` targets for independent hover, press, semantics, and
context menus; slider changes are emitted during captured left-button drag.
Do not assume keyboard or UI Automation support yet.
Probe `view.inputControls` for declarative textInput/textArea/searchBox/
numberInput/select nodes. Treat every value and select expansion as controlled:
write `text`, valid `controlValue`, `selection`, or `expanded` proposals into the
component model, then invalidate. Inputs reuse host keyboard, selection,
clipboard proxy, and IME behavior and may emit focus/blur/submit; they still do
not expose clipboard data or native handles. Select requires both click (toggle
proposal) and change (option proposal), and its bounded popup is clipped by the
widget/parent scroll surface. Do not confuse these nodes with the storage-bound
immediate `control.textInput/textArea` compatibility calls.
Probe `view.styledText.basic` for 1-64 bounded text spans with per-span color,
size, bold, italic, underline, and strikethrough. This basic feature does not
yet include inline icons or actionable spans. Probe `view.monthCalendar` for a
host-rendered six-week Gregorian grid: provide seven localized weekday labels
in Sunday-first order, keep `selectedDate` controlled, and apply the proposed
ISO date from the action event's `selection` field. Date cells have stable
`<calendar-key>/<YYYY-MM-DD>` targets and independent hover/context menus.
Probe `slots.model` and `view.logicalSlots` before using manifest-declared
logical bindings or collections. Open them with `slots.binding(id)` or
`slots.collection(id)`, render the exact host snapshot with
`view.slotSurface/slotItem`, and only call bind/add/clear/remove/move from the
current trusted action event. These methods persist references; they do not
grant file contents, expose paths, or move source objects. Probe
`view.referenceIcon` to render a bound/search result's opaque reference as a
host-resolved icon without requiring a package image or exposing its target;
this visual node does not grant launch, open, reveal, or file-content access.
Probe
`slots.nativeDrop` when a committed slotSurface should accept one native
desktop/Application/Explorer object with host insertion preview. Probe
`slots.pointerReorder` when collection slotItem nodes should support direct
same-surface dragging; the host owns the drag threshold, insertion indicator,
atomic move, undo record, and `slot.changed` event. Probe
`slots.nativeContextMenu` for host-owned per-item move/remove menus, and handle
`slot.changed` only after probing `slots.event.changed`; re-read the slot model
instead of trusting event data as writable state. Probe `slots.hostPicker` and
call a binding/collection handle's `pick()` only from the current trusted action
to open the manifest-filtered host picker; cancellation does not mutate the slot
or grant file-content access. Multi-object native ingress and native item
drag-out are not available yet. A
source reference used by Lua mutation must still come from a bounded host
search or explicit file-reference task.
Probe `slots.history` to expose explicit component actions for the bounded
per-instance undo/redo history. Call undo/redo only from the current trusted
action and use canUndo/canRedo while rendering; history is not restored after
reload or restart. Probe `slots.hostHistory` when documenting that a selected
widget also receives host Ctrl+Z, Ctrl+Shift+Z, and Ctrl+Y for this history.
Probe `view.scroll` for a host-owned vertical or horizontal viewport. Give it
exactly one child, keep that child visible, and keep the same key so the host
retains its clamped offset; never offset descendants yourself. Probe
`view.collection.basic` for
non-virtual `list/gridList/listItem`: collection children must be listItem,
each item needs exactly one visible child, a globally stable key, and
`accessibility.label`.
Keep a tree within 256 list items and use per-item actions/context menus. These
basic collections do not provide keyboard/UIA support. For larger data, probe
`view.collection.virtual`, call `view.virtualRange` with the stable collection
key and actual content-viewport height, create only its inclusive 1-based
window, then submit `virtualList` or `virtualGrid` with matching fixed extent,
row gap, columns, overscan, firstIndex, and contiguous listItem children. Keep
the materialized window within 128 items; do not emulate variable-height rows,
horizontal virtualization, sticky headers, or programmatic scrolling.
Probe `view.grid.uniform` before using `view.grid`; it is a bounded row-major
equal-column layout with 1–64 columns and optional `columnGap`/`rowGap`, not
the future track/span/virtual-grid contract.
Probe `view.flow.wrap` before using `view.flow`; it wraps fixed/auto-width
children horizontally, skips hidden children, and supports per-line
`columnGap`/`rowGap`, but it is not a scrolling or virtualized collection.
Outside declarative host inputs, the subset does not yet provide keyboard focus;
it still does not provide UI Automation, variable-height
virtualization, or the complete `view.tree` contract. Optional
`menu(context, model, request)` builds an element's synchronous native context
menu.

## Manifest rules

- `schemaVersion: 2` and `apiVersion: 2` must match.
- `id` is an immutable UUID; `version` is SemVer; `dataVersion` is positive.
- `name` and `description` are English fallbacks; localized values use
  `nameKey`, `descriptionKey`, and manifest `locales`.
- `requiredFeatures` must be supported for activation. Put degradable feature
  IDs in `optionalFeatures` and probe them before use.
- Use `resources` for package images and fonts. Keep resource names stable;
  use package-relative files and include font license metadata.
- Keep `permissions` empty unless a currently documented guarded v2 call needs
  one. Reserved permission vocabulary does not make an API available.
- Keep size dimensions from 1 through 8; a max dimension of `0` means
  unrestricted where the manifest schema permits it.
- Never include DLLs, executables, absolute paths, parent traversal, symlinks,
  junctions, reparse points or files outside the package.

## Implementation rules

- Treat `render` and `view` as hot paths. Do not write storage, create resource
  handles, load modules, or perform future data queries during every frame.
- `state.set` deep-copies JSON-like data and requests another frame only when
  the value changes. Do not use it as persistent storage.
- Group related persistent string writes with `storage.transaction`; access
  storage only through its `tx` argument until the callback returns. A callback
  error or final quota failure rolls back the complete change. Never write
  persistent storage from `render`.
- Use `schedule.every/after/cancel` for v2 timers and handle
  `event.kind == "schedule"`; do not add new `widget.setTimer` usage.
- Set `whenHidden` deliberately: prefer `pause` for purely visual clocks and
  animation, `throttle` for low-frequency freshness, and `continue` only when
  deadlines must remain active while the component is hidden.
- Create `data.subscribe` handles once in `setup` or module scope, read their
  immutable envelopes during render, and declare `system.performance.read`
  `system.power.read`, or `system.network.read` as optional when system data
  can degrade gracefully.
- For local calendars, subscribe to `calendar.events` and
  `calendar.selectedDate` under `calendar.read`; use permission-free
  `calendar.dateInfo/addDays/selectDate` for date math and SnowDesktop's shared
  selection. Rebuild range subscriptions from `event.kind == "data.change"`,
  and do not request `calendar.write` unless event records are mutated.
- Mutate local calendar records through
  `task.start("calendar.create"|"calendar.update"|"calendar.remove", args)`
  and match `task.complete`. Preserve event `id/revision`, handle `conflict`,
  and start remove only from a direct trusted action or menu command.
- Fetch public HTTPS data with `task.start("network.request", args)` and declare
  `network.internet`. Leave `networkDomains` absent when a user setting may
  point at arbitrary public HTTPS hosts; add exact hostnames only when the
  package intentionally narrows its own network scope. Keep requests as
  bounded GETs, match the returned task ID, handle stable failure codes, and
  cancel outstanding work in `dispose`; never restore v1 `http` in an API v2
  widget.
- Open an article or other external public HTTPS URL only with
  `task.start("shell.openUri", { url = value })` from a direct trusted action
  or menu command. Declare `shell.launch` as optional when opening links is not
  the widget's core function, and do not accept file, command, or custom-scheme
  targets.
- Start `media.play/pause/toggle/stop/next/previous/seek/setRate/setShuffle/setRepeat`
  with `task.start` only inside a direct trusted gesture callback. Pass the
  opaque `sessionId` from `media.sessions/current` when the widget displays or
  controls a specific session; omit it to target the current Windows session.
  Match the returned ID in `event.kind == "task.complete"`. Never loop media
  actions from the completion event; it intentionally has no trusted-gesture
  activation.
- Read the current session cover through `media.artwork`. Pass its temporary
  `image` handle directly to `draw.image` or `view.image.source`; never expect
  encoded bytes or a cache path, never persist the handle after unsubscribing,
  and treat `notPresent` as a normal no-cover state.
- Change only the current default audio endpoint with
  `audio.output.setVolume` or `audio.output.setMute` from a direct trusted
  gesture. Declare `audio.output.control`, treat `rateLimited` as a normal
  rejection, and never emulate per-process or non-default-device control.
- Open Windows Settings only with `system.openSettings` and one documented
  page name. Declare `shell.launch`, start it from a direct trusted gesture,
  and never accept or construct a raw `ms-settings:` URI in widget code.
- Read clipboard `text`, `image`, or `file-reference` only through
  `clipboard.read` from a direct trusted gesture. Probe
  `task.clipboard.image` or `task.clipboard.fileReference` before using the
  latter formats. Treat returned image handles and item refs as temporary and
  instance-scoped; never infer a path or file-content grant from a file ref.
  Clipboard write and clear remain text-only through
  `clipboard.write/clear`; declare `clipboard.read` separately from
  `clipboard.write`, keep text within 256 KiB, and do not claim clipboard
  history access.
- Ask the user to grant a concrete file or folder only through
  `filesystem.pickOpen/pickSave/pickFolder` in a direct trusted gesture.
  Declare `filesystem.userSelected.read` and/or `.write` for the requested
  access, retain or persist only the returned opaque handle token, and show
  its display-only name. Probe `task.filesystem.access` before using bounded
  `filesystem.stat/list/read/write/release` tasks. Preserve revisions and pass
  `expectedRevision` when updating content. Never parse, log, or replace a
  handle with a filesystem path. For direct-child change notifications,
  declare `filesystem.userSelected.watch`, probe `data.filesystem.watch`, and
  subscribe with the selected folder handle. Treat `overflow=true` as a signal
  to run a fresh bounded `filesystem.list`; watching pauses while hidden and
  never recurses or follows reparse points.
- Search applications with the bounded `task.start("app.search", { query,
  limit, offset })` task and retain only its opaque `ref` values. Render one
  declaratively with `view.referenceIcon` after probing that feature. Launch one
  with `task.start("app.launch", { ref = item.ref })` inside the direct click
  action. Never persist or invent refs, and never substitute a path, command
  line, or working directory.
- Search SnowDesktop items or the local Everything index with bounded
  `desktop.search` / `everything.search` tasks. Render their opaque refs with
  `draw.icon`, and use `shell.openItem` / `shell.revealItem` only inside a
  direct trusted action. `desktop.refresh` is also gesture-gated. Never expose,
  persist, parse, or replace these refs with filesystem paths.
- Post background completion notices only with
  `task.start("notification.show", { title, message })`, declare
  `notification.post` as optional when the widget can keep working without it,
  and handle the matching `task.complete` result. Do not loop notifications or
  fall back to the API v1 `system.notify` call.
- Create `resource.image/font` handles at entry scope. Use `resource.status`
  when diagnostics are needed.
- Use `draw.measureText`, clipping, explicit `maxWidth`, and separate opacity.
- Probe `draw.advanced` before using `draw.arc/path/gradientRect/imageFit/shadow/
  sparkline`. Keep paths within 256 strict commands and sparklines within 512
  finite values. Pass only image handles to `imageFit`; its fit, alignment, and
  interpolation are host-controlled. Shadow blur stops at 64 and uses at most
  16 bounded falloff layers, so do not describe it as an arbitrary shader or
  unbounded Gaussian effect.
- Submit every immediate-mode hit target with a stable `interaction.region`
  key during render. Read `interaction.isHovered/isPressed` for visuals and
  handle serialized region actions in `event`; never synthesize click from raw
  down/up callbacks. Build an element menu only through `widget.define.menu`
  and `ui.menu`, keeping the callback synchronous and I/O-free.
- Register vertical immediate-mode overflow with `interaction.scroll`, translate
  content by its returned offset, and pair the viewport with
  `draw.pushClip/popClip`. Do not use the v1 `ui.scrollArea` compatibility API.
- Submit storage-bound text editors with `control.textInput/textArea` during
  render. Keep keys stable, set an explicit practical `maxBytes`, and call
  `control.focus` only inside a direct trusted action or menu callback. Ordinary
  editing does not require `ui.input`; Lua never receives clipboard contents.
- Put an auxiliary editor in the optional `widget.define.panel` callback and
  open it with `widget.openPanel`; its context surface is `panel`, it accepts
  the same storage-bound controls, and persistent writes still belong in
  events rather than the panel render callback.
- Keep colors in `0xRRGGBB`.
- Respect `widget.context().accessibility`, theme, DPI, visibility and preview
  state. Do not request permission for an ordinary pointer clock or static UI.
- Treat the bottom host bar as reserved movement/resize space.
- Use `widget.log("debug"|"info"|"warn"|"error", message)` for recoverable
  diagnostics.
- Never use `io`, `os`, `require`, `package`, `load`, arbitrary filesystem or
  process APIs; the sandbox does not expose them.
- Do not invent v2 APIs from old v1 documentation. The synchronous `media`
  library remains v1-only; v2 media reads use `data.subscribe` and the three
  implemented controls use `task`. API v2 intentionally omits the synchronous
  `desktop`, `everything`, `http`, `sys`, and legacy `ui` libraries; their
  implemented replacements are scoped data subscriptions and bounded tasks.
  `ui.menu` is the only current v2 `ui` entry and is valid only as the result
  of the descriptor menu callback.

## Verification

For every package change:

1. Validate the directory with `snowwidget validate` and resolve every error.
   API v1 migration warnings are expected only for legacy input, never for a
   new package.
2. Pack it with `snowwidget pack`, then validate the resulting `.snowwidget`.
3. Run the repository localization and contract tests.
4. Preview compact and expanded spans; check text clipping, theme, DPI and
   resource rendering.
5. Activate the development candidate and verify hot reload. A failed reload
   must keep the last-known-good VM.

Do not claim pointer interaction, context-menu interaction, declarative element events, resource visuals,
multi-monitor DPI or permission UX is verified from validation/build alone;
those require an observable desktop run.
