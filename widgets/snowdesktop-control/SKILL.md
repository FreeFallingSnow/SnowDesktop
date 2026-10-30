---
name: snowdesktop-control
description: Operate a running SnowDesktop installation on Windows through its local control IPC. Use to inspect or change SnowDesktop settings, arrange existing widgets, open navigation or storage popups, capture the screen, or replay application-local input. Component development and Windows-wide automation use other tools.
---

# SnowDesktop control

Use the bundled PowerShell client for SnowDesktop's application control IPC. It needs Windows PowerShell 5.1 or PowerShell 7; no extra packages are required. SnowDesktop must already be running in the same Windows user, session and integrity level.

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

For layout, read `desktop.getLayout` or `widgets.list`, identify existing widget/page IDs, and use `widgets.place` with integral grid coordinates/spans and `ifLayoutRevision`. Close storage popups and Quick Navigation first. Placement follows the application's collision/displacement rules and cannot place grouped children independently. Its `persistence:"queued"` result acknowledges the in-memory layout, not durable storage.

`widgets.open/close`, `navigation.open/close`, `dock.show/hide` and `settings.open` expose existing presentation actions. An `accepted` response is not completion: observe `app.getState` for readiness and animation settlement. This endpoint has no general live-widget create/delete, arbitrary program execution, generic file-operation or Workshop-publication method.

## Interaction and recording

Read [references/interaction.md](references/interaction.md) when the task requires visible input, screenshots, recordings or fixed-data demo exports. Query `ui.snapshot` to discover current geometry and coverage; do not treat advertised affordances as executable element commands. Settings UI private input is unsupported while `settingsPrivateInput` is zero. Use semantic settings calls for configuration rather than inventing a fallback input path.

On `stale_settings` or `stale_layout`, reread current state and reassess the intended patch. On `busy`, finish the relevant interaction and wait within a bounded deadline. A timeout/disconnect after execution begins has an uncertain outcome: query state before retrying a mutation. Report unavailable capabilities and incomplete persistence/background operations accurately.
