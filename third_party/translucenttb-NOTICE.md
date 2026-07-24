# TranslucentTB implementation notice

SnowDesktop's Windows 11 taskbar backdrop hook and dynamic appearance tracker
are independent, reduced implementations of techniques used by
TranslucentTB commit `322e2b7395a51975150126276308b415970e080b`:

- XAML Diagnostics is used to observe Explorer's taskbar visual tree.
- `Taskbar.TaskbarFrame` and its `BackgroundFill` shape are located.
- The shape's XAML brush is replaced with a solid, composition blur, or
  `AcrylicBrush` using `AcrylicBackgroundSource::Backdrop`, and its original
  brush is retained for restoration.
- Window visibility and maximized state are tracked per monitor.
- Start, search, and Task View visibility techniques are used to select a
  higher-priority taskbar appearance.

The XAML visual-tree matching, composition effect structure, acrylic brush
selection, window filtering, dynamic-state precedence, and minimal private
Shell interface declarations were adapted from TranslucentTB:

https://github.com/TranslucentTB/TranslucentTB

TranslucentTB is copyright its contributors and licensed under the GNU General
Public License, version 3. SnowDesktop is distributed under the same license.
