# Context-menu icons

Approved menu preview 06 (2026-09-21), promoted to the desktop background menu
and the built-in entries of its add-widget submenu. The settings resources are
independent. These are project-colored Fluent icons, not artwork exported from
Windows.

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
| pin-native-* | Pin Regular | Non-needle head outline blue; hollow interior and neutral needle |
| add-page-native-* | Add Regular | Complete plus blue |
| settings-native-* | Settings Regular | Inner ring blue; hollow center and neutral outer gear |
| paste-native-* | Clipboard Paste Regular | Lower-right rectangle outline blue; hollow interior |
| new-item-native-* | Add Circle Regular | Plus blue; circle neutral |
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
contours. Secondary icons layer the original Filled and Regular geometry.
Each SVG contains provenance metadata; `sources.json` records asset hashes.

The 128 × 128 PNG counterparts are embedded as RCDATA by `src/resource.rc`.
WIC area downsampling produces premultiplied bitmaps at the menu's physical
icon size (18 DIP, or 16 DIP in compact menus). Runtime uses no filesystem
assets or preview-directory paths. The renderer caches a bounded number of
bitmaps, retains the Fluent glyph on load failure or in high contrast, and
preserves checked and disabled states. Package-provided images take precedence.

To regenerate PNGs, install Sharp 0.35.4 in a maintenance environment and run
`node scripts/render_menu_icons.cjs [path-to-sharp-module]` from the repository.
Normal builds use the checked-in PNGs and do not require Node or Sharp.
The renderer regression test exercises the embedded resources, designated blue
and neutral regions, hollow centers, themes, DPI, compact menus and fallbacks.
Desktop visual acceptance remains a separate manual check.
