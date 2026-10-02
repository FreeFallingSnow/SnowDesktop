# Quick navigation action icons

These assets layer the unmodified 24 px Filled and Regular paths of one semantic
Microsoft Fluent System Icon. They use the same fixed upstream revision as the
embedded Regular font: `21d5d02f724be2aaf586564775fff73a18a76eb6`.
Only fill colors change; there are no decorative badges, hand-drawn outlines,
deprecated Color variants, or redistributed Filled fonts.

| Asset | Official icon | Purpose |
| --- | --- | --- |
| all.svg | Search 24 Filled + Regular | Combined search |
| app.svg | Apps 24 Filled + Regular | Applications |
| web.svg | Globe 24 Filled + Regular | Web and engine prefixes |
| settings.svg | Settings 24 Filled + Regular | Settings search |
| run.svg | Code 24 Filled + Regular | Run command |
| calculator.svg | Calculator 24 Filled + Regular | Calculator |

`sources.json` records original source URLs and SHA-256 hashes, plus the hashes of
each composed SVG and its 128 px PNG. Native rendering scales the embedded PNGs
to 24 or 28 DIP and caches them per Direct2D context. High contrast uses the
existing system-color glyph fallback. These icons never replace user app icons.

Upstream: <https://github.com/microsoft/fluentui-system-icons>

License: MIT; see [the retained upstream license](../../../third_party/fluentui-system-icons/LICENSE).
