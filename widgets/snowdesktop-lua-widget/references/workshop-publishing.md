# Steam Workshop publishing

Use this reference only when the user asks to plan, create or update a
SnowDesktop component Workshop item.

When `bin\SnowDesktopSteamBridge.exe` is present, use it instead of automating
the Workshop Manager UI. First run:

```powershell
bin\SnowDesktopSteamBridge.exe configuration
```

Require `ok=true`, `steamworksCompiled=true` and
`componentWorkflowProtocolVersion=1`. Keep the executable beside its bundled
`steam_api64.dll`.

Always pass the active SnowDesktop data directory explicitly. This keeps the
project association, reuse policies, package staging and publication history in
the same `data\SteamWorkshopManager` store used by the Manager. Do not create a
second project-data directory beside the installed Skill.

## Plan before changing Steam

```powershell
bin\SnowDesktopSteamBridge.exe workshop component-plan `
  --source <widget-directory> `
  --data-directory <SnowDesktop-data-directory>
```

The command inspects, validates and packs the component, then returns one JSON
plan with `action` set to `create`, `update-content` or `update-metadata`. Show
the listing localizations, preview policy, tags, visibility, package version and
SHA-256 to the user. A successful plan is not authorization to publish.

After the user approves that exact plan, repeat the same source and policy
options with the matching confirmation flag:

```powershell
bin\SnowDesktopSteamBridge.exe workshop component-publish `
  --source <widget-directory> `
  --data-directory <SnowDesktop-data-directory> `
  --confirm-create
```

Use `--confirm-update` when the action starts with `update-`. Creation is always
private. Publishing emits JSON Lines progress and one final result. Preserve the
returned PublishedFileId even if a later localization update fails. Do not pass
`--open-page` unless the user asked to open Steam.

## Persistent source policies

- `--text-source package` reuses every title and description localization from
  `widget.json`; `steam` preserves current Steam text during updates;
  `manual-english --title ... [--description ...]` submits manual English.
- `--preview-source local` submits the manifest preview or `--preview FILE`;
  `steam` preserves the current Workshop preview during updates.
- `--tags-source local` submits persisted or repeated `--tag` values; `steam`
  preserves current Workshop tags. Use `--clear-tags` only with local tags.

New items cannot preserve Steam-managed values because none exist yet. A new
item requires an English title and primary preview. Changing only listing text,
preview, tags or visibility produces a metadata-only update when the packed
component SHA-256 is unchanged.
