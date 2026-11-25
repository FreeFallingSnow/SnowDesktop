# Desktop settings guide

See [Understand SnowDesktop](../CAPABILITIES.md) for product capabilities and when each component is useful.

## Components, pages and appearance

Use the [component property query commands](desktop-editing.md#query-component-properties) to see supported presentations and current settings. See [batch layout](desktop-editing.md#batch-layout) for placement and membership changes.

Use `settings.search` to find behaviors such as page and display settings, icon appearance, themes, privacy or hover behavior. Query the matching `settings.catalog` target for availability and writable fields.

## Bar layout presets and motion

Use the `runtime-services` catalog to compare what presets do to Dock, the status bar, the Windows taskbar and the work area. Use `apply-desktop-style` to apply a matching preset. For example, `island` pairs a bottom island Dock with a top status bar and requests that the Windows taskbar be hidden. Use fields such as `general.dockEnabled`, `general.statusBar` and the `dock` target for focused changes. Read the usable grid again after a bar change before placing components.

`apply-style-animations` applies the Dock hover and launch animations associated with a preset. The Genie window minimize and restore effect is controlled separately through `dock.windowEffect`. Check its availability on the running version.
