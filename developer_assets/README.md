# Developer-only assets

This directory contains resources used only by local development and debugging
workflows. Release and Steam packaging must not copy it.

`demo_icons/` supplies the fictional application icons used by Demo Mode on the
hidden Debug settings page. SnowDesktop loads these PNG files at runtime; they
are not embedded in `SnowDesktop.exe`.

Each icon filename starts with a stable three-digit visual index (`000_` through
`114_`). Keep those prefixes unique and contiguous. Developers can override the
directory with the `SNOWDESKTOP_DEMO_ICON_DIR` environment variable.
