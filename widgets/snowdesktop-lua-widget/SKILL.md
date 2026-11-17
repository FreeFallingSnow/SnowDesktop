---
name: snowdesktop-lua-widget
description: Create, modify, debug, visually review, validate, package, and publish SnowDesktop API v2 Lua desktop widget packages. Use for folders containing widget.json and main.lua, including callable performance evaluation, responsive layout, host-owned materials, foreground themes, localization, resources, settings, data, interactions, previews, and the snowwidget CLI.
---

# SnowDesktop Lua Widget

A component is a package directory with `widget.json` and `main.lua`. Add
`assets/`, `modules/`, `tests/` and a license when the package needs them.
Preserve an existing component's UUID, which saved desktop layouts use.

## Confirm the development workspace

Before writing a user-authored package, verify that the target is inside the
development widget directory the user opened from SnowDesktop. If it is
unavailable or outside the authorized workspace, direct the user to
**Settings → Widgets → Component Development Tools → Open Development Widget
Directory** (**设置 → 组件 → 组件开发工具 → 打开开发组件目录**) and have
them open that directory in their coding client. Edit built-in packages and
distributed Skill files in the authorized source repository for explicit
maintenance tasks. Treat installed packages and unfamiliar downloads as
read-only.

## Choose the workflow

| Request | Start with |
| --- | --- |
| Create a widget or change a surface | The authoring steps below and the [layout system](references/layout-system.md) |
| Review a design or diagnose a layout issue | [Layout acceptance](references/layout-system.md#layout-acceptance) and [visual authoring](references/visual-authoring.md). Identify the affected surface, size and observable behavior |
| Debug an API or runtime failure | The installed CLI contracts and [API troubleshooting](references/api-troubleshooting.md) |
| Validate an edit or deliver an archive | [Validation and packaging](references/validation-and-packaging.md), matched to the requested outcome |
| Measure performance | [Performance evaluation](references/performance-evaluation.md), using a representative workload |
| Read or change Steam tags on a bound item | [Tag actions in Workshop Manager](references/workshop-publishing.md#update-tags-on-a-bound-item) |
| Plan or publish a new item or package update | [Workshop publishing](references/workshop-publishing.md#plan-a-package-publication) |

Read only the references needed for the task. A read-only review uses
inspection and existing evidence. Generate fresh evidence when a requested
claim needs it.

## Use the bundled tools

Resolve `$SkillRoot` from the directory containing the **loaded** `SKILL.md`.
Replace the example path below with that absolute directory. Run these
PowerShell commands from the component workspace or any other directory:

```powershell
$SkillRoot = (Resolve-Path -LiteralPath 'C:\Example\snowdesktop-lua-widget').Path
$SnowWidget = Join-Path $SkillRoot 'bin\snowwidget.exe'
$SteamBridge = Join-Path $SkillRoot 'bin\SnowDesktopSteamBridge.exe'
& $SnowWidget capabilities
```

Use the reported `executableSchemaVersions`, `executableApiVersions` and
feature capabilities before relying on an API. Use the binaries belonging to
the installed Skill. If a binary is missing, locate the matching distributed
Skill through SnowDesktop. For a CLI outside its normal install location,
use `--host` or `SNOWDESKTOP_HOST` where the command supports a host override.
Pass explicit package and output paths in the examples when working outside
the component directory.

| Need | Use |
| --- | --- |
| Callable Lua functions and permissions | `& $SnowWidget api-contract` |
| Declarative nodes, properties, events and limits | `& $SnowWidget view-contract` |
| Data topics, tasks and typed arguments | `& $SnowWidget system-contract` |
| Signatures for the editor | [LuaLS declarations](library/snowdesktop-v2.lua) |
| Behavior, callbacks and feature flags | Search the matching heading in the [API v2 reference](references/api-v2.md) |

## Author the package

1. For a new component, start with the deterministic initializer. The example
   writes to a new folder under the current authorized development directory.

   ```powershell
   $Widget = Join-Path (Get-Location).Path 'my-widget'
   & (Join-Path $SkillRoot 'scripts\init_widget.ps1') `
     -Destination $Widget -Slug 'my-widget' `
     -Name 'My Widget' -Description 'Display the current status' -Author 'Author'
   ```

   The initializer refuses an existing destination and generates a UUID and
   `en-US` starter keys. Replace starter labels, example behavior and unused
   settings with the first usable version. Preserve the UUID of a package
   you are editing.
2. For each changed surface, identify its primary information and action,
   useful starting spans, reachable states and data needs. Choose a local
   declarative `view` for controls, lists, forms and accessibility, or a local
   immediate `render` for canvas-based content. Return a single
   `widget.define` descriptor with exactly one desktop rendering path. Read
   [layout patterns](references/layout-patterns.md) for lists, search, forms
   and management popups.
3. Keep rendering callbacks free of I/O, resource creation, subscription
   changes and storage writes. Load declared resources and `module.require`
   dependencies at entry scope. Use VM-lifetime `state` for transient values
   and `storage` for persistent changes. Read the event contract before
   reacting to host settings or using gesture-gated tasks.
4. Leave outer material, border and selection treatment to the host. Check the
   [visual authoring reference](references/visual-authoring.md) for semantic
   foreground colors, optional background composition, settings, menus and
   appearance presets. Read the API reference's `widget.creationPresets`
   section for initial content modes or `widget.size.aspectRatio` sizes.

Keep `schemaVersion` and `apiVersion` at `2`, `version` in SemVer and
`dataVersion` positive. Use English manifest fallbacks and literal
`l10n.tr("key")` calls with translated values for every declared locale.
Declare only features and permissions the package uses. Probe optional
capabilities before invoking them. Publish a supporting host before requiring
newer features in a Workshop package.

Declare transferable resources with safe package-relative paths. Exclude
absolute paths, parent traversal, symlinks, junctions and files outside the
package. Exclude DLLs, scripts used as helpers and undeclared executables.
For an authorized native helper, follow the API reference's **Native EXE
helpers** contract for hashes, permissions, user consent and the fixed protocol.

## Verify the requested outcome

Use [validation and packaging](references/validation-and-packaging.md) to
select checks. Verify each changed surface with suitable evidence. A catalog
PNG cannot verify panel interactions, and successful lint cannot verify DPI,
permission UI or pointer behavior.

Use [Workshop publishing](references/workshop-publishing.md) for any requested
Steam change. Review the exact Steam target and changes with the user before
publishing.
