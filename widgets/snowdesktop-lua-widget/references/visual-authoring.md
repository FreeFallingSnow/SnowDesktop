# Visual authoring and preview review

Use this reference for materials, foreground colors, internal surfaces, glyphs,
image composition and real preview review. For geometry, density, lists, forms,
scrolling and popup budgets, first read [layout-system.md](layout-system.md);
use [layout-patterns.md](layout-patterns.md) for concrete view structures.

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
- For immediate-mode artwork that must have rounded corners, declare and probe
  `draw.imageFit.roundedClip` and pass the radius to `draw.imageFit`; a rounded
  stroke around an image does not clip its rectangular bitmap corners.
- A canvas-led visual may cover the surface when the full canvas is the actual
  content, for example an analog clock face or deliberate artwork. It must still
  respect the host clip and resolved foreground theme. Add the exact source
  comment `-- snowwidget: allow-full-surface-content` only for this intentional
  case so the quality gate records the exception explicitly.

Set `useCustomStyle = true` when supplying fallback style values and
`followPersonalizationDefault = true` for ordinary components. Fallback
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
| `glass-transparent` | `0` | light/white |
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
is not following personalization. `__contentTheme` is a host-owned appearance
value: an appearance preset may declare `__contentTheme = 0` for a light/white
foreground or `1` for a dark/black foreground so host chrome follows that
preset. Pass it directly to the preview CLI for authoring overrides, and never
read or persist it from component Lua.

## Icon meaning and glyph verification

Choose icons whose established meaning matches the command or information they
represent. Do not use an unrelated symbol merely because it looks attractive,
fills the slot or resembles the intended icon. Check the action label and icon
together and confirm that they describe the same action or state.

For font icons, verify the named glyph and code point against the mapping for
the actual font family and version used by `iconFont`. Never guess a private-use
code point or copy one from another icon font, including a different Fluent
family. A valid, nonempty glyph is not evidence of the correct meaning. If no
appropriate host glyph exists, use a semantically appropriate declared resource
image when supported instead of substituting an unrelated icon.

Inspect the actual glyph rendered with that font at the intended UI size,
alongside its label, before accepting the change. Confirm that it is the chosen
symbol, remains legible and has appropriate visual weight. Code review or a
check that `icon` is nonempty cannot replace this visual check.

## Real preview contract

`snowwidget preview` launches the installed SnowDesktop renderer out of process
and writes a real API v2/D2D PNG. It does not emulate the view tree. The output
is opaque and contains the chosen background, resolved host material and widget
content.

Use `--appearance` with `dark`, `light`, `glass-dark`, `glass-light`, `glass-transparent`,
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
component exposes an independent foreground selector. For example:

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

Repeat the applicable cases for supported dark materials. Select materially
different states and affected dimensions rather than every unrelated
combination; reuse valid evidence when inputs relevant to it are unchanged.

1. Render the actual supported span, DPI, locale, appearance, foreground theme
   and data state.
2. Open the PNG at native size.
3. Check that the purpose and hierarchy are immediately recognizable.
4. Check text, icons, strokes, controls, disabled content and focus states for
   contrast and clipping. Inspect label and icon placement inside each changed
   control: horizontal and vertical alignment, baseline, padding and balance
   must match the design intent. Do not assume framework defaults match that
   intent or treat centering as a universal rule.
5. Check that the host outer surface remains visible and is not duplicated by a
   widget-drawn full-size card.
6. Apply the selected model's invariants and the affected
   [layout acceptance cases](layout-system.md#layout-acceptance). Inspect the
   actual overflow, long text, constrained popup bounds and whole-form fallback
   when those structures changed. Geometry and density defaults live in that
   reference, not in a second preview-specific sizing system.
7. Fix the smallest failing area, render again and repeat.

Do not treat CLI success as visual acceptance. Preview time is deterministic;
use manifest preview data and `--data-state` rather than waiting for real
schedules. Exercise ready, empty, loading, error, stale and permission-denied
only when those states apply to the widget.

Review each changed surface, including panels and other auxiliary interfaces;
a desktop or catalog preview does not verify a different surface. If the preview
CLI cannot render the required surface directly, an isolated temporary package
may route its actual view callback to a previewable surface to inspect parsing
and layout. Match the target surface's content bounds, semantic UI metrics and
theme; an unrelated preview scale cannot establish runtime text readability.
Record that adaptation and its limits: it does not verify native
surface routing, focus, keyboard interaction or runtime permission prompts.
Keep temporary inspection changes out of the delivered package, fix visible
defects, and inspect the new render before claiming visual acceptance.

Generate the final package preview before packing. Save it inside the package,
set the manifest `preview` field to that relative file, validate, and then pack.
For a square Workshop image, keep the component's real `--columns` and `--rows`
and add `--canvas-size 512 --padding 48`; the host preserves the widget aspect
ratio and center-crops the supplied background.

`glass-transparent` uses light/white content (`contentTheme=0`), neutral low-opacity tint,
blur and edge reflections. Use the tool bundled with a supporting host; older
tools reject this appearance. Lua API and schema versions remain unchanged.
