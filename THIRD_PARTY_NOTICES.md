# Third-Party Notices

SnowDesktop includes or redistributes the following third-party software and
assets. These notices apply only to the named components. The SnowDesktop core
is licensed under the GNU General Public License v3.0.

| Component | Version | License | Copyright / source |
| --- | --- | --- | --- |
| Dear ImGui | 1.92.5 WIP | MIT | Copyright (c) 2014-2025 Omar Cornut and contributors; <https://github.com/ocornut/imgui> |
| Lua | 5.4.7 | MIT | Copyright (C) 1994-2024 Lua.org, PUC-Rio; <https://www.lua.org> |
| Everything SDK client | bundled source | MIT | Copyright (C) 2022 David Carpenter; <https://www.voidtools.com/support/everything/sdk/> |
| pinyin-data | bundled data | MIT | Copyright (c) 2016 mozillazg; <https://github.com/mozillazg/pinyin-data> |
| Font Awesome 6 Free Solid | 6.5.2 font | SIL Open Font License 1.1 | Copyright (c) Font Awesome; <https://fontawesome.com/license/free> |
| Fluent System Icons Regular | upstream commit `21d5d02f724be2aaf586564775fff73a18a76eb6` | MIT | Copyright (c) 2020 Microsoft Corporation; <https://github.com/microsoft/fluentui-system-icons> |
| TranslucentTB-derived portions | upstream commit `322e2b7395a51975150126276308b415970e080b` | GPL-3.0-only | Copyright (c) TranslucentTB contributors; <https://github.com/TranslucentTB/TranslucentTB/tree/322e2b7395a51975150126276308b415970e080b> |

The complete MIT notices are retained in the corresponding bundled source
headers and in `third_party/pinyin-data/LICENSE`. The Font Awesome font is
distributed under the SIL Open Font License 1.1; the license text is available
from <https://openfontlicense.org/open-font-license-official-text/> and the
upstream Font Awesome license page linked above. The complete Microsoft Fluent
System Icons MIT license is retained in
`third_party/fluentui-system-icons/LICENSE` and is copied into release packages.

## TranslucentTB-derived portions

SnowDesktop contains modified portions derived from TranslucentTB at upstream
commit `322e2b7395a51975150126276308b415970e080b`. The incorporated material is
copyright TranslucentTB contributors and remains licensed under GPL-3.0-only,
the same license used by the SnowDesktop core. SnowDesktop modified the material
in 2026 for its own taskbar integration and desktop backdrop implementation.

The affected files are:

- `src/taskbar_dynamic/ShellViewCoordinator.idl`
- `src/taskbar_hook/taskview_visibility.h`
- portions of `src/taskbar_hook/taskbar_hook.cpp`
- portions of `src/app/desktop_backdrop_compositor.cpp`

The complete GPL v3 license text is retained in the repository root `LICENSE`.
The upstream source and history remain available at the pinned TranslucentTB
commit linked above.
