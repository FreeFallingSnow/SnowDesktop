---
name: snowdesktop-lua-widget
description: Create, modify, debug, visually review, validate, package, and publish SnowDesktop API v2 Lua desktop widget packages. Use for folders containing widget.json and main.lua, including callable performance evaluation, responsive layout, host-owned materials, foreground themes, localization, resources, settings, data, interactions, previews, and the snowwidget CLI.
---

# SnowDesktop Lua Widget

Build one validated package directory, never a loose Lua file and manifest:

```text
my-widget/
├── widget.json
├── main.lua
├── assets/
├── modules/
├── tests/
└── LICENSE
```

Only add optional directories when the widget needs them. Built-in widgets live
under the executable `widgets` directory. Installed and development packages
live under `data\widgets\installed` and `data\widgets\dev`. Layouts retain the
immutable package UUID, so never change an existing package ID.

## Confirm the development workspace

For ordinary user-authored widget work, confirm before writing that the target
package is inside the development widget directory the user opened from
SnowDesktop. A random current directory, downloaded package, installed package
or application directory is not an authorized development workspace merely
because it contains `widget.json`.

If the current workspace is not the development widget directory, or that
relationship cannot be established, do not create or modify the package there.
Tell the user to open **SnowDesktop Settings → Widgets → Component Development
Tools → Open Development Widget Directory** (`设置 → 组件 → 组件开发工具 →
打开开发组件目录`), then add or open that directory in the coding client and
authorize it for the workspace. Resume after the development directory is
available. Never edit `data\widgets\installed` as a substitute.

An explicit SnowDesktop source-repository maintenance task is the exception:
built-in packages and this distributed Skill may be edited in an already
authorized repository workspace.

## Start with the tool contract

Resolve this Skill directory and run:

```powershell
bin\snowwidget.exe capabilities
```

Treat its JSON as the available authoring contract. Only versions listed under
`executableSchemaVersions` and `executableApiVersions` can run. Use the bundled
CLI beside this Skill; a copied CLI may need `--host <SnowDesktop.exe>` or
`SNOWDESKTOP_HOST` for previews.

Do not load the complete API manual by default. Select the smallest relevant
source of truth:

| Need | Use |
| --- | --- |
| Callable Lua functions and permissions | `snowwidget api-contract` |
| Declarative nodes, properties, events and limits | `snowwidget view-contract` |
| Data topics, tasks and their typed arguments | `snowwidget system-contract` |
| Function signatures for the editor | `library/snowdesktop-v2.lua` |
| Detailed behavior for one selected feature | Search the matching heading in `references/api-v2.md` |
| Any new or changed layout, text sizing, list, form or popup | Read `references/layout-system.md` before editing |
| List rows, search/results, settings forms or management popups | Select the matching structure in `references/layout-patterns.md` |
| Materials, foreground themes, glyphs and preview mechanics | `references/visual-authoring.md` |
| Evaluate component CPU, allocations, timers, rendering or idle cost | `references/performance-evaluation.md`; check `snowwidget capabilities` first |
| Steam Workshop planning and publishing | `references/workshop-publishing.md` |
| Suspected host/API defect or contract mismatch | `references/api-troubleshooting.md`, then the matching implementation in the official source |

## Choose the workflow

| Task | Required focus |
| --- | --- |
| Create a component or add a surface | Establish its task/content brief, choose a model per surface, then use the initializer or matching layout pattern |
| Refine an existing interface | Inspect its current structure and representative content; preserve identity and behavior outside the requested change |
| Fix a list, form, sizing or overflow defect | Reproduce the failing surface/bounds; identify scale, space ownership and scroll owner before changing values |
| Review an interface | Apply the layout acceptance cases and visual reference; report each issue with surface, location and observable evidence |
| Diagnose an API/runtime failure | Use the installed contracts and `references/api-troubleshooting.md`; distinguish host behavior from component layout |
| Package or publish an unchanged design | Verify package identity and artifacts; follow the publishing reference only when publication is requested |

## Plan the changed surfaces

For a new widget or changed surface, infer a compact internal brief before code:

- the user's primary task and the information that must be visible first;
- recommended starting spans, and whether the content has a real functional
  reason to restrict minimum or maximum spans;
- ready, empty, loading, error and permission-denied states that are relevant;
- the smallest useful interaction set and any trusted-gesture requirements;
- data sources, permissions, persistent settings and supported locales;
- a recognizable visual direction that still respects the host surface.

Choose one rendering path:

- Prefer a declarative `view` for text, controls, lists, forms, ordinary layout,
  accessibility and host-rendered interaction states.
- Use immediate `render` for clocks, custom charts, freeform geometry and other
  visuals whose main value depends on a canvas.
- Do not mix the two desktop rendering paths. A descriptor must expose exactly
  one local `view` or `render` function.

Read [the layout system](references/layout-system.md) before writing geometry.
Choose a model for each actual surface: proportional visual for a composition,
proportional information for lists/forms, and grid density only for intentional
grid-aligned content. A canvas widget's management popup normally uses the
information model independently of its desktop artwork.

Use the layout system's public-contract boundaries, default information roles,
space budgets and override rules. Select a [layout pattern](references/layout-patterns.md)
when composing lists, search or management tools. Keep layout rules in those
references rather than inventing another scale for each component.

## Create a package

1. For a new widget, use the bundled deterministic initializer rather than
   rewriting the starter by hand:

   ```powershell
   & scripts\init_widget.ps1 `
     -Destination <directory> -Slug <lowercase-hyphenated-slug> `
     -Name <name> -Description <description> -Author <author>
   ```

   It refuses an existing destination, generates a new UUID, replaces the
   localization prefix and retains one honest `en-US` starter locale. Add only
   translations you can supply reliably. Do not use it when editing an existing
   package; preserve that package's UUID.
2. Replace all starter content, preview copy and example identity that remain.
3. Keep `schemaVersion` and `apiVersion` at `2`. Use SemVer for `version` and a
   positive `dataVersion`.
4. Replace starter content with a coherent first product slice. Do not leave
   example names, placeholder copy, empty starter cards or unused settings.
5. Keep only locales the package actually supports with reliable translations.
   Put every visible string behind a literal `l10n.tr("key")`, and include every
   used key in every declared locale. Component text belongs in `widget.json`,
   never in the host `lang/` directory.
6. Add only features and permissions actually used. Put degradable features and
   permissions in the optional arrays and probe before use.
7. Declare package images and fonts under `resources`. Create handles at entry
   scope and pass handles to v2 APIs. Load modules only with
   `module.require("modules/name.lua")` while the entry script loads.
8. Use `state` for VM-lifetime JSON-like values and `storage` for persistent
   values. Write persistent state only when it changes and never from a render
   or view callback.
9. When a host settings change affects an active subscription or a
   descriptor-level visual such as `backgroundLayer.blurRadius`, require
   `settings.changeEvent` and reconcile it from
   `event.kind == "settings.changed"`. The event covers ordinary values, host
   appearance, passwords, filesystem handles and entity references, and
   supplies sorted affected `keys`. `preview == true` identifies an
   uncommitted live preview; immediate writes and preview commit or cancellation
   emit the current persisted or restored state with `preview == false`.
   Calls to `storage.set` from the component do not emit this host-settings
   event.
   Keep render and view callbacks free of subscription creation or descriptor
   mutation.

## Surface ownership and theme contract

The host owns the outer shape, material, border and selected state. Set
`useCustomStyle=true` for authored fallback values and normally keep
`followPersonalizationDefault=true`. Draw content and intentional internal
groups inside that surface; use the documented decorative `backgroundLayer`
when a background composition is needed.

Material and foreground are independent. Require `view.theme.tokens` for
declarative semantic colors; immediate drawing consumes the resolved
`contentTheme`. Read [visual authoring](references/visual-authoring.md) before
implementing materials, foreground choices, imagery, glyphs or preview images.
That reference owns the exact theme mappings and surface exceptions.

## Entry callbacks

Return one `widget.define` descriptor whose desktop entry is a local `view` or
`render` function. The initializer contains a complete declarative entry;
[layout patterns](references/layout-patterns.md) show reusable view builders.
For immediate artwork, use the visual model and the foreground palette in
[visual authoring](references/visual-authoring.md#foreground-theme).

Do not define removed globals such as top-level `render`, `onClick`,
`getContextMenu`, `imguiRender` or `onHttpResponse`. Optional `setup(context)`
returns the model passed to `view`/`render`, `event` and `dispose`. Keep rendering
callbacks hot-path safe: do not create resources, load modules, start future
queries, write storage or rebuild invariant data on every frame.

## Settings, menus and appearance presets

- Put ordinary component preferences in `widget.define({settings=...})` and use
  the host's component-settings entry. Do not add another settings button,
  context-menu command or configuration panel unless the user explicitly asks
  for a separate workflow.
- Give each actionable context-menu command a semantic host glyph through
  `icon` and `iconFont`, or a declared resource image when that feature is
  available. Reuse the host's Fluent icon vocabulary for actions such as
  refresh; separators and informational rows do not need command icons. Follow
  [Icon meaning and glyph verification](references/visual-authoring.md#icon-meaning-and-glyph-verification)
  to check the symbol's meaning, actual font mapping and rendered result;
  unrelated substitute icons are prohibited.
- Expose a selectable component appearance, such as a transparent theme,
  through `settings.presets` with a stable `id`, localized `label` and grouped
  `values`. Mark the intended initial choice with `default=true`; put its
  material, opacity and foreground choices in that preset. Descriptor style
  values remain fallbacks, not a replacement for a reselectable preset. Preserve
  saved user choices, and do not apply a preset repeatedly from rendering code.

## Manifest boundaries

- `id` is an immutable UUID; `version` is SemVer; `dataVersion` is positive.
- `name` and `description` are English fallbacks. Localized metadata uses
  `nameKey`, `descriptionKey` and manifest `locales`.
- `previewData.variants` may offer up to four user-visible starting sizes in
  the add-component preview. Its `storage` remains isolated preview data and
  is never copied into a new instance, so do not use it to imply a persistent
  mode or setting that the added component will not receive.
- Register initial content modes in manifest `creationPresets`, not in
  `settings.presets` (which are appearance themes). Require
  `widget.creationPresets`. Each preset has a stable `id`, an English `title`,
  optional `titleKey`, and `values` containing ordinary string-backed settings.
  The picker shows each preset and initializes the added instance with the
  selected values after validating them against its actual settings schema.
  At most 32 content presets are supported. Secrets, opaque references,
  host-owned appearance keys and typed range/multi-select settings are excluded.
  Existing preview-only `storage` is never copied into the new instance.
- Keep explicit default, preview and finite boundary dimensions from 1 through
  8. Omit `minSize` to use the host's 1 × 1 default. Omit `maxSize`, or use a
  schema-permitted maximum dimension of `0`, when the widget has no functional
  upper bound. Recommended preview variants do not need to restrict resizing.
- For screen-dependent starting proportions, require `widget.size.aspectRatio`
  and define `defaultSize` or a preview variant's `size` as
  `{"rows":2,"aspectRatio":1}` (fixed two rows, choose the closest square width),
  or use `columns` instead to derive rows. Supply exactly one axis; the ratio is
  outer width divided by outer height. A creation preset may override `size` in
  the same format or with an explicit grid size. The host resolves integer spans
  using the destination page's actual grid and spacing; rendering and saved
  spans remain unchanged. Publish the supporting host first; old hosts reject
  this required feature instead of guessing a starting size.
- Keep `permissions` empty unless a documented guarded API needs one. Reserved
  vocabulary does not make an API available.
- Native helpers are limited to declared, hash-matched `.exe` files with
  `native.execute` permission and a required `task.native.request` (one request)
  or `task.native.watch` (resident stream) feature.
  Provide all four localized consent explanations and require the host's strong
  user authorization before execution. Lua remains sandboxed; use only the fixed
  helper protocol described under "Native EXE helpers" in `references/api-v2.md`.
  A supporting host must be published before a component requires this feature.
- Native protocol output must use explicit UTF-8 bytes and flush each frame.
  Keep logs, configuration and caches outside the package (for example in
  LocalAppData or the system temporary directory); any package write can stop a
  watch and invalidate authorization. Read the language examples and optional
  `task.complete.diagnostic` metadata in `references/api-v2.md`.
- Never include component DLLs or undeclared executables. Do not use shell
  commands or scripts as native helpers.
- Never include absolute paths, parent traversal, symlinks, junctions, reparse
  points or files outside the package.

## Build, preview and verify

Use the lowest-cost checks first:

1. Run `snowwidget lint <directory>` and resolve every error. Review warnings;
   do not suppress them without understanding the resulting behavior.
2. Run `snowwidget permissions <directory>` when the package declares a
   permission, data topic or task.
3. Run `snowwidget test <directory>` only when the package contains meaningful
   pure Lua tests. Tests are for behavior and logic, not formatting or copied
   implementation details.
4. Run `snowwidget preview <directory> <output.png>` at the default span and at
   the changed surfaces' materially different sizes or density states. Select
   the affected cases from [layout acceptance](references/layout-system.md#layout-acceptance)
   and follow [the real preview contract](references/visual-authoring.md#real-preview-contract)
   for appearance, content fixtures and surface-specific evidence.
5. Open and visually inspect every PNG. A JSON result with `ok=true` proves only
   that rendering completed. It does not prove readable contrast, correct
   hierarchy, unclipped content or a useful design. Fix defects, render again
   and repeat until the images pass.
6. Generate the final catalog image inside the package, then declare its
   relative path as manifest `preview`.
7. Run `snowwidget quality <directory>`. It combines package validation and
   source lint and fails on unresolved warnings, including a missing preview,
   hard-coded UI text and suspicious full-surface background drawing.
8. Run `snowwidget pack <directory> <name.snowwidget>`, then validate the packed
   archive as well.

The first meaningful preview must already show the widget's purpose, intended
visual direction, representative content and primary affordance. Reject an
untouched starter, placeholder copy, blank surface or loading-only handoff.

Check each changed surface's parsing, layout and interaction separately. A
desktop/catalog image does not verify a panel, dialog or popover. Keep temporary
preview adapters and fixtures outside the delivered package and record their
limits. Reuse applicable existing evidence instead of expanding unrelated
combinations; an untested behavior remains unverified.

Do not claim pointer interaction, context menus, declarative events,
multi-monitor DPI or permission UX is verified from lint, validation or build
alone. Those require an observable runtime check or explicit user validation.

## Evaluate performance when requested

Use `snowwidget profile <directory> <new-output-directory>` for a user-requested
performance review or a demonstrated performance concern. Read
`references/performance-evaluation.md` for workload selection, artifacts and
metric boundaries. This runs the real isolated preview engine and native
Perfetto; it does not require a performance control in Settings. Check the
`performanceProfile` capability and the returned report quality before drawing
conclusions. Non-default module filters or input sampling also require the
corresponding capabilities and confirmation in the actual result. Ordinary
previews remain static and do not start tracing.

## Diagnose suspected API problems

The official SnowDesktop source at
<https://github.com/FreeFallingSnow/SnowDesktop> is an implementation reference.
Use it when the installed CLI contracts, bundled API documentation and observed
host behavior appear inconsistent. Match the source version to the user's
installed SnowDesktop version when possible; the default branch may describe a
newer implementation.

Read `references/api-troubleshooting.md` before inventing a workaround. If a
minimal reproduction shows that the documented, advertised and authorized API
still fails, tell the user that the evidence points to a SnowDesktop API issue
and prompt them to report it at
<https://github.com/FreeFallingSnow/SnowDesktop/issues>. Provide the version,
capability output, minimal package or code, reproduction steps, expected and
actual behavior, and relevant `snowwidget`/host errors. Do not silently replace
the API with undocumented globals, filesystem access or a misleading mock.

## Publish only on request

Package creation and external publishing are separate. When the user asks to
publish or update a Steam Workshop item, read
`references/workshop-publishing.md`, generate a plan first, show the exact plan,
and obtain approval immediately before the external mutation.
