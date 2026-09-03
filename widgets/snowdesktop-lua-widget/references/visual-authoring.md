# Visual authoring and preview review

Use this reference when a SnowDesktop widget needs custom style values,
foreground colors, internal surfaces, image composition or final catalog
previews.

## Surface ownership

SnowDesktop composites the outer widget surface. The descriptor supplies its
fallback material through `bg`, `border`, `alpha`, `borderAlpha`,
`gradientEndA` and `glassEnabled`; the host applies the final rounded shape,
material and outer border.

When the concept needs decorative imagery or composition above the material
tint and below foreground content,
declare the required `widget.backgroundLayer` feature and add
`backgroundLayer={render=...,opacity=...,blurRadius=...}`. The callback uses
the immediate drawing API over the full desktop surface. It is clipped to the
host shape and cannot own interactions or accessibility semantics. Omitted
`blurRadius` inherits the active glass blur and resolves to zero without glass;
an explicit value from 0 to 48 overrides either case. The host material tint is
drawn first, so an opaque component background keeps its authored colors while
transparent pixels reveal the resolved theme. Acrylic noise, the outer border,
selected state and edge highlight remain host-owned.

Keep content rendering inside that host surface:

- Do not draw another rectangle or image over the full foreground content
  bounds merely to recreate the outer background; use `backgroundLayer` when
  a real background composition is intended.
- Use internal surfaces only to group content, separate hierarchy or implement
  a control whose body is part of the widget's information design.
- Avoid nested cards when spacing, a divider or typography hierarchy is enough.
- A canvas-led visual may cover the surface when the full canvas is the actual
  content, for example an analog clock face or deliberate artwork. It must still
  respect the host clip and resolved foreground theme. Add the exact source
  comment `-- snowwidget: allow-full-surface-content` only for this intentional
  case so the quality gate records the exception explicitly.

Set `followPersonalizationDefault = true` for ordinary components. Fallback
style values remain useful when personalization is disabled. A deliberately
independent sticky-note material or artwork can default to false when the
product concept requires it.

## Foreground theme

The resolved foreground theme is independent of the material. A light glass or
acrylic material may request light foregrounds, and a dark material may request
dark foregrounds. Never derive text, icons, strokes or control colors from:

- `widget.theme().bg` or another RGB luminance calculation;
- wallpaper pixels;
- background alpha;
- `normal`, `glass` or `acrylic` material names.

The built-in preset defaults are deliberately not a suffix-based dark/light
pairing:

| Host appearance | Default `contentTheme` | Foreground |
| --- | ---: | --- |
| `dark` | `0` | light/white |
| `light` | `1` | dark/black |
| `glass-dark` | `0` | light/white |
| `glass-light` | `0` | light/white |
| `acrylic-dark` | `0` | light/white |
| `acrylic-light` | `1` | dark/black |

`glass-light` therefore uses light text by default. Always consume semantic
tokens or the resolved `contentTheme`; never turn a `-light` suffix directly
into dark text.

For declarative views, require `view.theme.tokens` and prefer
`textPrimary`, `textSecondary`, `textDisabled`, `border` and related semantic
foreground tokens. `surface` and `surfaceVariant` are internal surface tokens;
they are not guaranteed to contrast with every independently selected
foreground. When necessary, select an explicit internal-surface palette from
the resolved `contentTheme` and verify it in the preview matrix.

For immediate drawing, map the host result explicitly:

```lua
local function foregroundPalette()
    local theme = widget.theme()
    local darkForeground = theme and theme.contentTheme == 1
    if darkForeground then
        return {
            primary = 0x111827,
            secondary = 0x4B5563,
            disabled = 0x9CA3AF,
            border = 0x6B7280,
        }
    end
    return {
        primary = 0xFFFFFF,
        secondary = 0xD1D5DB,
        disabled = 0x9CA3AF,
        border = 0x9CA3AF,
    }
end
```

The equivalent context mapping is `context.theme.mode == "dark"` for light
foregrounds and `"light"` for dark foregrounds. These names describe the host
color scheme, not the material brightness.

If the widget exposes its own foreground setting, use it only while the widget
is not following personalization. `__contentTheme` is a host-owned authoring
preview override. Pass it to the preview CLI and never read or persist it from
component Lua.

## Real preview contract

`snowwidget preview` launches the installed SnowDesktop renderer out of process
and writes a real API v2/D2D PNG. It does not emulate the view tree. The output
is opaque and contains the chosen background, resolved host material and widget
content.

Use `--appearance` with `dark`, `light`, `glass-dark`, `glass-light`,
`acrylic-dark` or `acrylic-light`. The legacy `--theme dark|light` shorthand
cannot be combined with `--appearance`. Use `--background <image-file>` for the
final catalog composition. The source background is not included in the package
unless the manifest separately declares it as a resource.

Preview JSON reports `theme` for the light or dark stage palette and reports the
resolved `contentTheme`/`foregroundTheme` separately. Use the latter fields when
checking text and icon colors; `glass-light` reports `contentTheme: 0` and a
light foreground even though its stage palette is light.

For an ordinary component that follows personalization, exercise each supported
host appearance with `followPersonalization=1`; include both `glass-light` and
`acrylic-light` so the two different default foregrounds remain visible. Only
force `__contentTheme=0` and `1` with `followPersonalization=0` when the
component exposes an independent foreground selector. For every materially
different state:

1. Render the actual supported span, DPI, locale, appearance, foreground theme
   and data state.
2. Open the PNG at native size.
3. Check that the purpose and hierarchy are immediately recognizable.
4. Check text, icons, strokes, controls, disabled content and focus states for
   contrast and clipping.
5. Check that the host outer surface remains visible and is not duplicated by a
   widget-drawn full-size card.
6. For a proportional visual or information component, compare small and large previews with
   the same aspect ratio: structure must stay the same and every part must
   scale uniformly. Check each intentional aspect-ratio branch, or verify that
   a single-structure composition remains centered across its supported range.
   Center the complete group of currently visible parts. Agendas, calendars,
   lists, search and RSS should use `ui.metrics().layoutRowHeight` as a shared
   row unit. A top control row normally occupies `1x` this value, and body rows
   use intentional multiples or the returned body-font and spacing metrics.
   Begin content at the row boundary without another title-area gap. Compare
   components with the same row span to confirm equal row units, and verify
   that changing only width does not resize them. Taller spans grow the unit
   and all derived semantic metrics slowly. Do not apply a second `rpxY` scale
   to those values, and never let width determine vertical controls or rows.
   Reserve grid-density
   checks for components that explicitly align
   information units to host grid metrics, such as a system-status card matrix.
7. Fix the smallest failing area, render again and repeat.

Do not treat CLI success as visual acceptance. Preview time is deterministic;
use manifest preview data and `--data-state` rather than waiting for real
schedules. Exercise ready, empty, loading, error, stale and permission-denied
only when those states apply to the widget.

Generate the final package preview before packing. Save it inside the package,
set the manifest `preview` field to that relative file, validate, and then pack.
For a square Workshop image, keep the component's real `--columns` and `--rows`
and add `--canvas-size 512 --padding 48`; the host preserves the widget aspect
ratio and center-crops the supplied background.
