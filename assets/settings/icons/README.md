# Settings icon assets

The navigation SVG files listed in the first table are composed from Microsoft Fluent
System Icons Regular and Filled paths. They use the same upstream revision as
SnowDesktop's embedded Regular font:
`21d5d02f724be2aaf586564775fff73a18a76eb6`.

- Upstream: <https://github.com/microsoft/fluentui-system-icons>
- License: MIT; see `third_party/fluentui-system-icons/LICENSE`.
- The deprecated upstream Color variants are not used.
- Each asset layers the Filled and Regular paths of one semantic icon; it does
  not add decorative badges or unrelated secondary symbols.
- The upstream SVG paths are embedded directly; no Filled font is redistributed.

| Asset | Fluent glyph composition |
| --- | --- |
| `search.svg` | Search 24 Regular + Filled (onboarding quick navigation) |
| `context-menu.svg` | Text Bullet List Square 24 Regular + Filled |
| `calendar.svg` | Calendar 24 Regular + Filled |
| `general.svg` | Settings 24 Regular + Filled |
| `animation-performance.svg` | Filmstrip Play 24 Regular + Filled |
| `appearance.svg` | Paint Brush 24 Regular + Filled |
| `appearance-theme.svg` | Dark Theme 24 Regular + Filled |
| `appearance-widgets.svg` | Window Apps 24 Regular + Filled |
| `appearance-desktop-icons.svg` | Icons 24 Regular + Filled |
| `appearance-icon-beautification.svg` | Paint Brush Sparkle 24 Regular + Filled |
| `desktop.svg` | Desktop 24 Regular + Filled |
| `desktop-style.svg` | Desktop Edit 24 Regular + Filled |
| `pages.svg` | Table Multiple 24 Regular + Filled |
| `categories.svg` | Collections 24 Regular + Filled |
| `dock.svg` | Dock Row 24 Regular + Filled |
| `taskbar.svg` | Panel Bottom Contract 20 Regular + Filled |
| `status-bar.svg` | Panel Top Contract 20 Regular + Filled |
| `widgets.svg` | Apps 24 Regular + Filled |
| `widget-behavior.svg` | Cursor Hover 24 Regular + Filled |
| `backup.svg` | Cloud Arrow Up 24 Regular + Filled |
| `about.svg` | Info 24 Regular + Filled |
| `developer.svg` | Window Dev Tools 24 Regular + Filled |
| `debug.svg` | Bug 24 Regular + Filled |

## Desktop style preview artwork

The following assets are used only inside the desktop layout illustration, not
as navigation or page-heading icons. Each preserves the original 24 px Filled
and Regular paths from the revision above. Application glyphs sit on a 32 px
tile; folder and trash keep their native 24 px viewBox for balanced ink sizes.
The application backplates are SnowDesktop artwork; they are not an upstream
Fluent Color variant. Collections are composed in the presenter from four of
these application icons using the actual Dock collection layout rules.

| Asset | Fluent path source |
| --- | --- |
| `preview-browser.svg` | Globe 24 Filled + Regular |
| `preview-mail.svg` | Mail 24 Filled + Regular |
| `preview-terminal.svg` | Code 24 Filled + Regular |
| `preview-media.svg` | Play Circle 24 Filled + Regular |
| `preview-folder.svg` | Folder 24 Filled + Regular |
| `preview-trash.svg` | Delete 24 Filled + Regular |
