---
name: snowdesktop-control
description: Use SnowDesktop on Windows to set up or organize a desktop, improve how apps and files are reached, adapt the desktop's look or interaction style, configure its Dock, status bar, navigation and widgets, or capture and replay SnowDesktop interactions. Controls a running installation through its local IPC.
---

# SnowDesktop control

Use the bundled PowerShell client for SnowDesktop's application control IPC. It needs Windows PowerShell 5.1 or PowerShell 7; no extra packages are required. SnowDesktop must already be running in the same Windows user, session and integrity level.

## Understand SnowDesktop

- **Desktop storage** organizes shortcuts with collections and tabbed collection groups. Desktop Files, folder mappings and file groups give access to working files, with source tabs, file-type filters and search where supported. Storage can live in a desktop panel, behind a one-cell icon, or in a mix of both.
- **Dock, status bar and Windows taskbar** give different ways to launch apps, switch tasks and reach files, time, tray and system controls. Layout presets coordinate the bars. Position, visibility, work area and motion can also be tuned independently.
- **Quick Navigation** finds apps, desktop items and files. Use it when the goal is to reach something faster or reduce visible clutter. Indexed file search depends on the configured integration.
- **Appearance and widgets** include icon beautification, shared and per-surface materials, and built-in or installed components. They can make wallpaper and text easier to see, create recognizable entry points, or keep useful information at hand.

## Work from the user's intent

1. Identify what the user wants to find, see or do differently. For references to another desktop or style, infer candidate behaviors such as placement, launch and switching habits, visibility, or motion. Reuse explicit preferences and the current setup. When clarification would change the result, first share what you found and a recommendation in everyday terms. Ask about the user's routine or desired outcome so they can answer without knowing SnowDesktop's component names or ways of arranging them.
2. Discover the closest supported controls. Use `desktop.inspect` for the layout and native component catalog. Use `settings.search` with short English phrases for behaviors and `settings.catalog` on matching targets for availability, choices and side effects. Look at adjacent capabilities when they help the same goal.
3. Choose the smallest coherent set of changes. Preview spatial changes and explain any consequential side effects before applying. For a clear execution request, carry out the requested behavior through semantic operations. Offer optional add-ons separately when they change a preference the user has yet to express.
4. Verify what changed through fresh state and, when relevant and available, a whole-screen visual check. Explain how to use any new entry points or controls.

## Connect and discover

Run `scripts/snowdesktop-control.ps1 -Method api.describe`, then `-Method app.getState`.
Paths below are relative to this Skill's installed directory; invoke the script by its absolute path from other working directories. The installed `connection.json` binds the client to the SnowDesktop executable that installed the Skill. The client verifies the named-pipe server process path before sending a request. If SnowDesktop was moved, reinstall/update this Skill from the intended installation, or supply its verified absolute path with `-ExecutablePath`. Do not accept an unexpected server or silently connect to another installation.

```powershell
& '<skill-directory>/scripts/snowdesktop-control.ps1' -Method settings.catalog
& '<skill-directory>/scripts/snowdesktop-control.ps1' -Method settings.get -ParametersJson '{"target":{"kind":"dock"}}'
```

Requests accept named JSON objects through `-ParametersJson` or UTF-8 `-ParametersFile`. Results are JSON-RPC envelopes on stdout; errors exit nonzero and are never automatically retried. Discover methods/capabilities at runtime, as older builds can expose fewer operations. Check `runtimeMode`, `dataDirectory`, `desktopDirectory` and `debugProfile` before changing a desktop or recording it.

## Settings and layout

Prefer semantic operations for ordinary configuration. Read `settings.catalog` for a concrete target to learn writable fields, constraints, defaults and named actions. Read fresh state immediately before mutation and pass its opaque `revision` as `ifRevision`. Respect read-only fields, current availability, Steam unlocks, permissions, and any target-specific confirmation arguments. Execute only changes within the user's request. Secret values are not returned by settings reads; use the catalog's explicit secret and picker actions where applicable.

For arranging or changing how the desktop works, read [desktop workflows](references/desktop-workflows.md). Use **first-time desktop setup** for an initial arrangement, **existing desktop management** for a scoped edit, and **interaction preferences** when the goal centers on how apps, files or controls are reached.

Use the [batch editing guide](references/desktop-editing.md) to create and place components, assign collection items, and move icons. On hosts with `desktopBatchEdits:1`, use `desktop.inspect`, `desktop.previewEdits` and `desktop.applyEdits` with the returned identities and grid coordinates. `desktopNativeComponents:1` adds direct group and folder-path controls. `desktopComponentCatalog:1` describes each native component before it exists and lets the layout batch configure icon entrances, file filters and search visibility.

For an individual existing component, `desktop.getLayout` and `widgets.place` remain available. Their layout revision comes from `desktop.getLayout`. Placement follows the application's displacement rules. Close storage popups and Quick Navigation before layout changes. `persistence:"queued"` acknowledges the in-memory layout.

`widgets.open/close`, `navigation.open/close`, `dock.show/hide` and `settings.open` expose existing presentation actions. An `accepted` response is not completion: observe `app.getState` for readiness and animation settlement. Component creation is available through the desktop editing workflow on compatible hosts. This endpoint provides no general component deletion, arbitrary program execution, generic file operation or Workshop publication.

## Interaction and recording

Read [references/interaction.md](references/interaction.md) when the task requires visible input, screenshots, recordings or fixed-data demo exports. Query `ui.snapshot` to discover current geometry and coverage; do not treat advertised affordances as executable element commands. Settings UI private input is unsupported while `settingsPrivateInput` is zero. Use semantic settings calls for configuration rather than inventing a fallback input path.

On `stale_settings` or `stale_layout`, reread current state and reassess the intended patch. On `busy`, finish the relevant interaction and wait within a bounded deadline. A timeout/disconnect after execution begins has an uncertain outcome: query state before retrying a mutation. Report unavailable capabilities and incomplete persistence/background operations accurately.
