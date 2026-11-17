# Validation and packaging

Use the bundled executable resolved as
[`$SnowWidget` in the Skill](../SKILL.md#use-the-bundled-tools). Let `$Widget`
be the package directory and keep temporary evidence outside that directory.
Choose checks based on the outcome the user requested.

| Outcome | Checks |
| --- | --- |
| Read-only review | Inspect the component and available evidence. Run a targeted check if needed to substantiate a finding. |
| Source or manifest edit | Run `lint` and review its warnings. Run `permissions` when changing guarded APIs, permissions, data topics or tasks. Run existing `test` cases relevant to changed pure Lua behavior. |
| Creation or visual change | Render the default span and affected bounds or states that expose different behavior. Open each PNG at native size. Use the [layout acceptance cases](layout-system.md#layout-acceptance) and [visual review contract](visual-authoring.md#real-preview-contract). |
| Deliverable installable archive | Review the catalog cover declared by manifest `preview`, run `quality` and produce the final archive with `pack`. |
| Existing archive | Run `verify-install` when the requested outcome is installation or upgrade verification. |
| Existing listing's tags | Use [Workshop Manager](workshop-publishing.md#update-tags-on-a-bound-item). No archive is needed. |

```powershell
& $SnowWidget lint $Widget
& $SnowWidget permissions $Widget
& $SnowWidget test $Widget
& $SnowWidget preview $Widget $OutputPng
```

Use representative content and the real rendering path. Check each affected
desktop, panel, dialog or popover independently. A catalog PNG shows only the
surface rendered into it. Keep temporary adapters and fixtures outside the
delivered package and document their limits. Record runtime checks or user
observations for claims about pointer or keyboard input, context menus,
declarative events, multi-monitor DPI or permission prompts.

## Deliver an installable archive

Generate or refresh the catalog cover if it is missing or the existing one no
longer represents the component. Save the final cover in the package and set
manifest `preview` to its relative path. The default preview composites the
stage background and resolved host surface. Use `--content-only` for authored
content on a transparent canvas. See [visual authoring](visual-authoring.md#real-preview-contract)
for the catalog image options.

```powershell
& $SnowWidget quality $Widget
& $SnowWidget pack $Widget $Archive
```

`quality` combines package validation and source lint, and fails on unresolved
lint warnings. Resolve any failures before shipping. `pack` creates a temporary
archive and launches the matching SnowDesktop host in an isolated data
directory to validate a fresh installation. It exports the final archive only
after that succeeds and leaves the source and its existing cover in place.

For an update, supply the exact previous release archive with
`--previous-package` to also validate upgrade while a previous-version instance
is loaded. Use `verify-install` to check an archive that already exists:

```powershell
& $SnowWidget pack $Widget $Archive --previous-package $PreviousArchive
& $SnowWidget verify-install $Archive --previous-package $PreviousArchive
```

Choose the command for the artifact you have. Use `--host $HostExe` when the
matching executable is outside the normal bundled location. Record fresh
install and upgrade evidence separately. If the previous release archive is
unavailable, report the fresh result and the missing upgrade evidence.

## Observe the running desktop

If a change depends on a running instance, use the separate
`snowdesktop-control` Skill. SnowDesktop exposes it under
**Settings → Advanced Settings → Agent Operations → SnowDesktop Control Skill**
(**设置 → 高级设置 → Agent 操作 → SnowDesktop 操作 Skill**). Read the installed
Skill for available operations.

Start with `api.describe` and `app.getState` to check capabilities, the active
data directory and debug profile. Locate the real instance with `widgets.list`.
Use supported surface and interaction operations, then inspect the resulting
state. Read the current settings or layout revision before authorized writes.
