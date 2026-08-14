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
4. Keep `schemaVersion` and `apiVersion` at `2`. Implement a local `render`
   function and return `widget.define({ render = render, ... })` from `main.lua`.
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
`getContextMenu`, `imguiRender`, or `onHttpResponse`. The v2 descriptor reserves
`view` until that contract is implemented. The current host
supports optional `setup(context)` and `dispose(context, model, reason)`;
`setup` runs once and its return value is passed to `render`, `event`, and
`dispose`. Optional `event(context, model, event)` receives host surface events;
immediate-mode elements use `interaction.region`, while declarative elements
remain unavailable. Optional `menu(context, model, request)` builds a region's
synchronous native context menu.

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

- Treat `render` as a hot path. Do not write storage, create resource handles,
  load modules, or perform future data queries during every render.
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
- Start `media.toggle`, `media.next`, and `media.previous` with `task.start`
  only inside a direct trusted gesture callback, then match the returned ID
  in `event.kind == "task.complete"`. Never loop media actions from the
  completion event; it intentionally has no trusted-gesture activation.
- Search applications with the bounded `task.start("app.search", { query,
  limit, offset })` task and retain only its opaque `ref` values. Launch one
  with `task.start("app.launch", { ref = item.ref })` inside the direct click
  action. Never persist or invent refs, and never substitute a path, command
  line, or working directory.
- Post background completion notices only with
  `task.start("notification.show", { title, message })`, declare
  `notification.post` as optional when the widget can keep working without it,
  and handle the matching `task.complete` result. Do not loop notifications or
  fall back to the API v1 `system.notify` call.
- Create `resource.image/font` handles at entry scope. Use `resource.status`
  when diagnostics are needed.
- Use `draw.measureText`, clipping, explicit `maxWidth`, and separate opacity.
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
  implemented controls use `task`. The absence of `desktop`, `http`, `sys`,
  legacy `ui` controls and other action libraries is intentional until the
  corresponding v2 capability is implemented. `ui.menu` is the only current
  v2 `ui` entry and is valid only as the result of the descriptor menu callback.

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
