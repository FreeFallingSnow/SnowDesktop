# Layout system

Read this before creating or changing geometry, text sizing, lists, forms or
auxiliary surfaces. Use [layout-patterns.md](layout-patterns.md) for concrete
Lua structures and [visual-authoring.md](visual-authoring.md) for materials,
foreground themes, glyphs and preview mechanics.

## Authority and overrides

Distinguish three kinds of guidance:

- **Host contracts:** supported nodes and properties, actual content bounds,
  semantic metrics, clipping, scrolling and input ownership. Follow the installed
  CLI contracts; a layout preference cannot override them.
- **Product defaults:** the information-layout roles below and the standard
  structures. Use them for ordinary lists, searches, forms and management tools
  so equivalent controls have consistent density across components.
- **Authored composition:** artwork, charts, instruments and intentional visual
  treatments may choose their own geometry. Keep their chosen scaling invariant
  and the host contracts.

Preserve an existing widget's identity when refining it. Override a default for
a concrete content, hierarchy, accessibility or user requirement. Name the role
being changed and apply the override consistently on that surface; do not scatter
unrelated constants or redefine the host row unit. A user font preference is a
multiplier over the derived font, not a replacement scale for the whole layout.

## Plan each surface

For the surface being edited, identify its primary task, leading information,
related groups, intended density and smallest useful interaction set. Decide
what stays fixed, wraps, reflows or scrolls when space decreases. Use real content
and relevant empty/loading/error states to make these decisions.

Choose the model per surface, including auxiliary interfaces of a canvas widget:

| Surface and purpose | Model | Invariant |
| --- | --- | --- |
| Artwork, clock face, media composition, custom instrument | Proportional visual | Equal-aspect surfaces scale all visible geometry uniformly |
| Desktop agenda, search, list, calendar, RSS | Proportional information | Controls use the shared semantic row; extra room primarily exposes content |
| Panel/dialog/popover containing a search, list, form or commands | Proportional information, popup density | Controls use that callback's baseline row, independent of the source widget span |
| Status-card matrix intentionally aligned to host grid units | Grid density | Cards follow the declared grid relationship |

A visually composed desktop does not make its management form proportional
artwork. An auxiliary artwork surface can still choose the visual model. Mix
models only for an intentional overlay with explicit boundaries.

### Proportional visual

Read `layout.contentWidth()` and `layout.contentHeight()` in the current callback.
Derive text, icons, gaps, radii and strokes from these axes, `vw`, `vh`, `vmin`,
or `layout.rpxX`, `layout.rpxY` and isotropic `layout.rpx`. Ratios such as
`math.min(width, height) * 0.08` preserve uniform scaling.

If equal-aspect surfaces differ by a scale factor, every visible length follows
that factor. Avoid fixed pixel floors, capped global scales, `sizeClass`, grid
span breakpoints and `cu`/`fontCu` in the main composition. Use an aspect-ratio
branch only when the content relationship needs it; do not invent branches just
because the widget resizes. Center the complete group of currently visible parts
and recalculate it when optional parts disappear.

### Proportional information

Require `ui.semanticMetrics.rowUnit`. Read `ui.metrics().layoutRowHeight` inside
the current `view` or `render` callback. Never cache surface-dependent metrics
in `setup` or reuse desktop-derived fonts in a popup.

The host row is the content height of an ordinary input, button or single-line
item; surrounding insets and gaps are additional. One- and two-row desktop
widgets use the page baseline; taller spans grow the unit slowly. Width does
not change it. Panel, dialog and popover callbacks use the page baseline without
desktop-span growth. Page row settings and system text scaling still apply.
Do not multiply these values by another `rpxY`, desktop span or text scale.

Use these authoring defaults for equivalent information controls. Ratios use a
28-unit design row; they are not fixed pixels or new host theme tokens.

| Role | Default | Placement |
| --- | ---: | --- |
| Ordinary control / single-line item height | `row` | Content box, excluding sibling gaps |
| Body / control label | `row * 12 / 28` | Main text |
| Caption / supporting text | `row * 10 / 28` | Secondary text |
| Section heading | `row * 14 / 28` | Hierarchy within a tool surface |
| Tight gap | `row * 4 / 28` | Closely related parts |
| Regular gap | `row * 8 / 28` | Repeated rows or ordinary groups |
| Section separation | `row * 12 / 28` | Distinct task groups |
| Desktop content inset | `row * 12 / 28` | Main content inside host bounds |
| Popup content inset | `row * 6 / 28` | Compact auxiliary content inside host bounds |
| Control horizontal text inset | `row * 9 / 28` | Label inside an input or button |
| Ordinary control icon | `row * 14 / 28` | Glyph inside its row-height hit target |
| Control corner radius | `row * 6 / 28` | Internal inputs/buttons, not the host outer surface |
| Control stroke | `row / 28` | Internal control outline |

Use deliberate row multiples for multi-line content; budget the actual line
count and gap. Information density is structural: show more rows, adjust optional
metadata or reflow controls before shrinking text. Section separation should
express a stronger grouping boundary than a tight intra-group gap.

These local names illustrate the defaults; they are not a host module:

```lua
local function informationMetrics(surfaceKind)
    local row = ui.metrics().layoutRowHeight
    return {
        row = row,
        body = row * 12 / 28,
        caption = row * 10 / 28,
        heading = row * 14 / 28,
        gapSmall = row * 4 / 28,
        gap = row * 8 / 28,
        sectionGap = row * 12 / 28,
        inset = row * (surfaceKind == "popup" and 6 or 12) / 28,
        textInset = row * 9 / 28,
        icon = row * 14 / 28,
        radius = row * 6 / 28,
        stroke = row / 28,
    }
end
```

### Grid density

Use `layout.cu()` and `layout.fontCu()` when content intentionally tracks host
grid metrics, such as a system-status card matrix. Grid metrics and size classes
may determine those card boundaries. They do not grow linearly with the whole
surface and do not replace the other models' invariants.

## Space ownership

Allocate space in this order:

1. Start with the actual host content bounds. Host chrome, material, border,
   rounded clipping and outer scrollbar placement remain host-owned.
2. Choose which content scrolls. Its viewport occupies its allocated region;
   an outer popup viewport reaches the host content edge.
3. Apply the primary content inset inside that viewport. Fixed header/footer
   content receives the same horizontal inset independently.
4. Apply label padding inside individual controls, then sibling gaps between
   controls. An outer inset is not a substitute for a control's text inset.

Use one primary inset for each content region. Additional nesting needs a real
grouping purpose; repeated padded columns accumulate empty space. Prefer `gap`
for sibling rhythm. Align the search field, list text and fixed actions to
deliberate common edges, accounting for each control's internal text padding.

### Lists and scrollbars

For an outer scrollable auxiliary surface, keep `view.scroll` at
`width="fill"`, `height="fill"`, with no content padding on the viewport.
Put padding on its child column/list. The viewport then reaches the host content
boundary and the host can place the track in its outer gutter. A padded wrapper
around the viewport moves the track inward. Local scrolling areas retain tracks
at their own viewport edges; they are not outer popups.

Give selectable row labels explicit horizontal text padding, particularly
start-aligned buttons. Set `padding={horizontal=metrics.textInset, vertical=0}`
on the control rather than uniform padding that consumes its row height.
Keep trailing commands from shrinking and let the label take the remaining
width. Use stable item IDs for keys and routed actions.

A list that already owns scrolling, including a virtual list, should not be
wrapped in another scroll container for the same axis. Choose one scroll owner.
Do not draw imitation scrollbars or shift pointer coordinates to repair a track;
the host owns rendering, clipping, wheel routing and thumb dragging.

### Forms and popup sizing

Use ordinary host settings for preferences. A separate tool form is appropriate
for an explicitly requested workflow or a task that needs its own interaction.
Use one consistent control vocabulary and preserve the primary action's route.

Derive a popup request from its content: the widest useful row, fixed regions,
desired visible item count, insets and gaps. Keep comparable tools at comparable
control density. Do not multiply the requested dimensions or font sizes by the
desktop widget span, and do not use popup width as a vertical scale.

Treat the requested size as a preference. Work-area constraints and positioning
can produce different actual content bounds. Recompute from those actual bounds
inside the callback; a popup near an edge must not keep the original request's
layout budget.

For a fixed header + scrolling body + fixed footer, subtract the fixed content
heights, outer vertical inset and inter-region gaps first. The remaining viewport
must have positive content width and height after padding. Reserve enough room
for at least one useful body row. If that budget fails, switch to one outer
scroll containing the complete form so required controls remain reachable.
See the concrete calculation in [layout-patterns.md](layout-patterns.md).

Do not manufacture a one-pixel inner viewport with `math.max(1, remaining)` or
squeeze every control to keep an unsuitable fixed structure. Reduce redundant
insets, reflow or change the scroll owner. Even an empty list needs a meaningful
content node rather than a zero-size body.

## Text, alignment and adaptation

Declarative text defaults to `textAlign="start"` and
`verticalAlign="center"`. Choose alignment according to the role: row labels
usually start-align, while an ordinary command button may center-align. Check
label, icon, baseline and padding together; centering every element is not a
layout system.

Let ordinary names and localized labels wrap or ellipsize intentionally. Reserve
the measured line budget for multi-line labels. For a short critical timer,
amount or counter, use `overflowText="clip"`, `width="auto"`, a calculated
`minWidth` and `flexShrink=0`; preserve that minimum on a shrinking parent column
and take the width from adjacent flexible content. A minimum cannot enlarge the
surface. If the group still cannot fit, change its structure or value font.

Choose the smallest structural adaptation that preserves the task: reposition,
resize the content region, reflow, move optional detail behind an accessible
route, or change the layout model. Keep required actions available and visual
order consistent with keyboard/accessibility order. Use the actual component
container, not monitor resolution, to make the decision.

## Layout acceptance

Select the cases affected by the change; do not run a Cartesian product of
unrelated states. Review every changed surface at its own bounds and metrics.

| Case | Observable result |
| --- | --- |
| Short/empty list and representative long name | Text has deliberate insets; empty state offers a useful next step; commands remain visible |
| Overflowing list | Last item is reachable; track does not obscure content; wheel and thumb work at the actual edge |
| Same-aspect visual sizes | All visible geometry scales uniformly; the complete composition remains centered |
| Information widths / taller desktop spans | Width does not change row or font scale; additional room primarily reveals content |
| Tool opened from small and large desktop spans | Popup controls retain the same baseline scale |
| Popup constrained by work area or larger text | Uses actual bounds; positive scroll content; fixed regions or whole-form fallback keep required actions reachable |
| Relevant locale/text scale and applicable async states | Grouping, line budgets, focus order and actions survive the content change |

Report a violation with its surface, file/location, failed expectation and
rendered or runtime evidence. A parser/lint pass cannot establish spacing,
readability or pointer behavior. A routed offscreen callback can establish
content parsing/layout only when bounds, metrics and theme match; it cannot
establish native popup placement, focus or gutter hit testing. Follow
[visual-authoring.md](visual-authoring.md#real-preview-contract) for rendering
and the applicable repository/runtime rules for interaction acceptance.
