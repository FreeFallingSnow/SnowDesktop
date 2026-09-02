---
name: snowdesktop-lua-widget
description: Create, modify, debug, visually review, validate, package, and publish SnowDesktop API v2 Lua desktop widget packages. Use for folders containing widget.json and main.lua, including responsive layout, host-owned materials, foreground themes, localization, resources, settings, data, interactions, previews, and the snowwidget CLI.
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
| Visual ownership, themes and preview review | `references/visual-authoring.md` |
| Steam Workshop planning and publishing | `references/workshop-publishing.md` |
| Suspected host/API defect or contract mismatch | `references/api-troubleshooting.md`, then the matching implementation in the official source |

## Classify before creating

For a new widget, infer a compact internal brief before writing code:

- the user's primary task and the information that must be visible first;
- default, minimum and maximum grid spans;
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

## Surface ownership and theme contract

The SnowDesktop host owns the outer shape, material, border and selected state.
By default:

- Let the host draw the component material, rounded shape, outer border,
  gradient, glass or acrylic through `widget.define` style fields.
- Set `useCustomStyle = true` when supplying fallback style values and set
  `followPersonalizationDefault = true` unless the user's concept explicitly
  requires an independent material.
- Use `backgroundLayer={render=...}` with required feature
  `widget.backgroundLayer` when a component deliberately needs images, color
  blocks, gradients or paths below the host material. The callback is
  decorative, desktop-only and cannot register interactions or native
  marquees. Full-surface drawing is expected inside this callback.
- Do not draw a full-surface `draw.rect`, `draw.gradientRect`, image or view
  background in the foreground callback to imitate the host material. Draw
  only content and intentional internal surfaces. Full-canvas foreground
  artwork remains an exception for a canvas-led widget such as a clock face;
  document it with `-- snowwidget: allow-full-surface-content`.
- Treat material/background and foreground theme as independent inputs. Never
  infer foreground colors from background RGB, luminance, wallpaper, alpha or
  a material name.
- In declarative views, require `view.theme.tokens` and use semantic foreground
  tokens such as `textPrimary`, `textSecondary`, `textDisabled` and `border`.
- In immediate drawing, derive the foreground palette from
  `widget.theme().contentTheme`: `0` selects light/white foregrounds and `1`
  selects dark/black foregrounds. `widget.context().theme.mode == "dark"`
  likewise means light foregrounds, while `"light"` means dark foregrounds.
- Preserve the host preset defaults exactly: `dark`, `glass-dark`,
  `glass-light` and `acrylic-dark` resolve to light/white foregrounds;
  `light` and `acrylic-light` resolve to dark/black foregrounds. In particular,
  the default `glass-light` pairing still uses light text. Do not infer this
  mapping from the appearance name suffix; consume the resolved theme.
- If the widget offers a custom foreground setting, use it only when the widget
  is not following personalization. Never read or persist the host-owned
  `__contentTheme` preview override from component Lua.

Read `references/visual-authoring.md` before implementing custom materials,
foreground choices, internal surfaces or final preview images.

## Required entry shape

This immediate-mode example deliberately lets the host own the outer surface
and resolves content colors independently:

```lua
local function foregroundPalette()
    local theme = widget.theme()
    local darkForeground = theme and theme.contentTheme == 1
    return {
        primary = darkForeground and 0x111827 or 0xFFFFFF,
        secondary = darkForeground and 0x4B5563 or 0xD1D5DB,
    }
end

local function render(_context, _model)
    local colors = foregroundPalette()
    local padding = layout.cu(12)
    draw.text(padding, padding,
        l10n.tr("lua_widget.my_widget.hello"),
        layout.fontCu(15), colors.primary,
        layout.contentWidth() - padding * 2)
end

return widget.define({
    name = l10n.tr("lua_widget.my_widget.name"),
    useCustomStyle = true,
    followPersonalizationDefault = true,
    bg = 0x18202A,
    border = 0xFFFFFF,
    alpha = 0.42,
    borderAlpha = 0.18,
    gradientEndA = 0.28,
    render = render,
})
```

Do not define removed globals such as top-level `render`, `onClick`,
`getContextMenu`, `imguiRender` or `onHttpResponse`. Optional `setup(context)`
returns the model passed to `view`/`render`, `event` and `dispose`. Keep rendering
callbacks hot-path safe: do not create resources, load modules, start future
queries, write storage or rebuild invariant data on every frame.

## Manifest boundaries

- `id` is an immutable UUID; `version` is SemVer; `dataVersion` is positive.
- `name` and `description` are English fallbacks. Localized metadata uses
  `nameKey`, `descriptionKey` and manifest `locales`.
- Keep size dimensions from 1 through 8. A maximum dimension of `0` means
  unrestricted only where the schema permits it.
- Keep `permissions` empty unless a documented guarded API needs one. Reserved
  vocabulary does not make an API available.
- Never include native binaries, executables, absolute paths, parent traversal,
  symlinks, junctions, reparse points or files outside the package.

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
   every materially different compact or expanded span. Exercise relevant DPI,
   locale, appearance, foreground theme, data state and storage values.
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

For an ordinary component that follows personalization, preview the supported
host appearances with `followPersonalization=1`. Include `glass-light` with its
default light foreground and `acrylic-light` with its default dark foreground;
they intentionally differ. If the component exposes an independent foreground
selector, also verify both foreground choices with personalization disabled:

```powershell
bin\snowwidget.exe preview <directory> <light-fg.png> `
  --appearance acrylic-light `
  --storage followPersonalization=0 `
  --storage __contentTheme=0

bin\snowwidget.exe preview <directory> <dark-fg.png> `
  --appearance acrylic-light `
  --storage followPersonalization=0 `
  --storage __contentTheme=1
```

Repeat the corresponding cases for dark material when the widget supports it.
Check primary, secondary, disabled, icon, stroke and control text. Use
`--data-state` for relevant ready, empty, loading, error, stale and
permission-denied envelopes. For a square Workshop image, preserve the real
component span and use `--canvas-size 512 --padding 48` rather than changing the
widget to a square layout.

In the SnowDesktop repository, a Lua-package-only change uses component-level
lint, tests, validation, preview and packaging. Do not build the native host
unless the change crosses the package boundary. Use
`scripts\widget-dev.bat <directory>` when an observable desktop hot-reload
check is required.

Do not claim pointer interaction, context menus, declarative events,
multi-monitor DPI or permission UX is verified from lint, validation or build
alone. Those require an observable runtime check or explicit user validation.

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
