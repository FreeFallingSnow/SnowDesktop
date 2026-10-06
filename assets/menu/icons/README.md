# Context-menu icons

Approved menu preview 06 (2026-09-21), promoted to the desktop background menu
and the built-in entries of its add-widget submenu. The settings resources are
independent. These are project-colored Fluent icons, not artwork exported from
Windows. A subsequent review removes the blue accents from Pin and Add Page.

All geometry comes from Microsoft Fluent System Icons revision
`21d5d02f724be2aaf586564775fff73a18a76eb6`, matching the embedded Regular font.
The upstream project is https://github.com/microsoft/fluentui-system-icons.
See `LICENSE-Fluent.txt` and `third_party/fluentui-system-icons/LICENSE` for MIT
attribution. No deprecated Color variants or Filled font are distributed.

| Asset stem | Official 24 px icon | Treatment |
| --- | --- | --- |
| sort-native-* | Arrow Sort Regular | Entire right down arrow blue; up arrow neutral |
| display-native-* | Options Regular | Both circular outlines blue; hollow centers and neutral rails |
| widgets-native-* | Apps Add In Regular | Complete plus blue |
| pin-native-* | Pin Regular | Neutral head and needle; hollow interior |
| add-page-native-* | Add Regular | Neutral plus |
| settings-native-* | Settings Regular | Inner ring blue; hollow center and neutral outer gear |
| paste-native-* | Clipboard Filled + Regular, Clipboard Paste Regular | Blue paper outline; rear board, paper and clip surfaces white in light menus, graphite in dark menus; neutral clipboard outline |
| new-item-native-* | Add Circle Regular | Blue plus; white circle surface in light menus, graphite in dark menus; neutral circle outline |
| refresh-native-* | Arrow Clockwise Regular | Neutral |
| collection | App Folder Filled + Regular | Yellow/orange |
| collection-group | Collections Empty Filled + Regular | Yellow/orange |
| file-group | Folder Multiple Filled + Regular | Yellow/orange |
| desktop-files | Folder Filled + Regular | Yellow/orange |
| folder-mapping | Folder Link Filled + Regular | Yellow/orange |
| search | Search Filled + Regular | Same source artwork as settings search |
| workshop | Globe Filled + Regular | Blue |

Primary icons have light (`#30343B`, `#0078D4`) and dark (`#E4E6EA`, `#60CDFF`)
variants. Full original Regular paths remain intact. Additional painted paths
are complete original closed subpaths; selection masks only recolor the approved
contours. Paste and New Item also paint their complete original Regular inner
contours as independent surfaces (`#FFFFFF` light, `#3B3B3B` dark), with a softer
`#595959` light outline. Paste also places the complete Clipboard Filled and
Regular paths behind these layers, translated -1 on x to align with the original
Clipboard Paste rear board; the foreground paper covers the overlapping board
outline. Their surface is independent of the menu background;
disabled items retain the renderer's reduced opacity. Secondary icons layer the
original Filled and Regular geometry.
Each SVG contains provenance metadata; `sources.json` records asset hashes.

The 128 × 128 PNG counterparts are embedded as RCDATA by `src/resources/resource.rc`.
WIC area downsampling produces premultiplied bitmaps at the menu's physical
icon size (18 DIP, or 16 DIP in compact menus). Runtime uses no filesystem
assets or preview-directory paths. The renderer caches a bounded number of
bitmaps, retains the Fluent glyph on load failure or in high contrast, and
preserves checked and disabled states. Package-provided images take precedence.
Paste and New Item semantic actions also resolve to this shared artwork when
their menu model does not explicitly bind a BuiltinIcon, covering component,
folder-popup and text-input menus as well as ordinary rows in compact menus.
An explicit built-in resource retains priority over semantic selection; high
contrast and missing resources retain the Fluent glyph fallback.

To regenerate PNGs, install Sharp 0.35.4 in a maintenance environment and run
`node scripts/render_menu_icons.cjs [path-to-sharp-module]` from the repository.
Normal builds use the checked-in PNGs and do not require Node or Sharp.
The renderer regression test exercises the embedded resources, designated blue
and neutral regions, hollow centers, themes, DPI, compact menus and fallbacks.
Desktop visual acceptance remains a separate manual check.
