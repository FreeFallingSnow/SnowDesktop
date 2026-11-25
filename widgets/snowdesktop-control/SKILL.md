---
name: snowdesktop-control
description: Use SnowDesktop on Windows to set up or organize a desktop, improve how apps and files are reached, adapt the desktop's look or interaction style, configure its Dock, status bar, navigation and widgets, or capture and replay SnowDesktop interactions. Controls a running installation through its local IPC.
---

# SnowDesktop control

Use the bundled PowerShell client for SnowDesktop's application control IPC. It needs Windows PowerShell 5.1 or PowerShell 7; no extra packages are required. SnowDesktop must already be running in the same Windows user, session and integrity level.

## Understand SnowDesktop

- **Desktop storage** organizes shortcuts with collections and tabbed collection groups. Desktop Files, folder mappings and file groups give access to working files, with source tabs, file-type filters and search where supported.
- **Dock, status bar and Windows taskbar** give different ways to launch apps, switch tasks and reach files, time, tray and system controls. Layout presets coordinate the bars. Position, visibility, work area and motion can also be tuned independently.
- **Quick Navigation** finds apps, desktop items and files. Use it when the goal is to reach something faster or reduce visible clutter. Indexed file search depends on the configured integration.
- **Appearance and widgets** include icon beautification, shared and per-surface materials, and built-in or installed components. They can make wallpaper and text easier to see, create recognizable entry points, or keep useful information at hand.

## Work with the user

1. Read the current setup and identify what the user wants to find, see or do differently. Start the discussion with a concrete observation and a recommended direction in everyday terms. Treat a reference to another desktop or style as a clue about behaviors such as where apps open, when controls appear, or how windows move.
2. Resolve open decisions through the [question rules](#ask-progressively). Reuse what the user has already said. Make routine and reversible implementation choices as part of your proposal.
3. Discover supported controls. Use `desktop.inspect` for the layout and native component catalog. Use `settings.search` with short English phrases for behaviors and `settings.catalog` on matching targets for availability, choices and side effects. Base the proposal on what the running app advertises and mark unverified details as such.
4. Preview spatial changes and explain consequential effects. Apply the requested changes through semantic operations once the decisions that affect the result are settled. Verify the new state and show how to find or use what changed.

### Ask progressively

- Assume the user is new to SnowDesktop. Explain what's possible through something they can picture doing, such as finding today's files or opening the apps they use most. Introduce a product term when it helps them recognize a control, with a brief explanation of what it does.
- Ask one question about one decision per turn, including when using a choice card. Start with the most consequential unknown in the user's routine or desired result. Wait for the answer before asking the next question, and ask it only if it still changes the plan. When the goal gives you enough to proceed, state your assumption and continue.
- Phrase questions in everyday terms. If options help, use a plain label and a visible sentence for each choice about what the user would see, do, or trade for it. Use the choice card's description field when available, or put the effect in the displayed option text. For example, seeing today's files when you return to the desktop takes some desktop space. Keep exact sizes, placements and component settings in your plan. Offer optional additions in their own turn when relevant.

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
