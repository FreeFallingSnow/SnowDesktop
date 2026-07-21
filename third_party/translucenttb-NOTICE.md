# TranslucentTB implementation notice

SnowDesktop's Windows 11 taskbar backdrop hook is an independent, reduced
implementation of the technique used by TranslucentTB's `ExplorerTAP`:

- XAML Diagnostics is used to observe Explorer's taskbar visual tree.
- `Taskbar.TaskbarFrame` and its `BackgroundFill` shape are located.
- The shape's XAML brush is replaced with a fully transparent brush, and its
  original brush is retained for restoration. SnowDesktop then renders the
  visible material on its own desktop composition layer.

The XAML visual-tree matching and composition effect structure were adapted
from TranslucentTB:

https://github.com/TranslucentTB/TranslucentTB

TranslucentTB is copyright its contributors and licensed under the GNU General
Public License, version 3. SnowDesktop is distributed under the same license.
