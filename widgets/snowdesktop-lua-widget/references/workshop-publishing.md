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

## Multiple preview images

Keep `preview` as the primary cover. Add optional `previewGallery` to
`widget.json` for up to seven additional PNG/JPG/GIF images in display order:

```json
"preview": "workshop-preview.png",
"previewGallery": ["previews/gpu.png", "previews/memory.png"]
```

Each gallery path must be safe, relative to the component package, unique,
and point to a nonempty file smaller than 1 MiB. The cover is separate from
the array. This is one public gallery shared by all listing languages.
Before publishing multiple images, require
`configuration.componentPreviewGalleryProtocolVersion=1` and use the matching
bundled authoring CLI; older publishers can publish only the primary cover.
No new component runtime feature or API version is needed.

Local preview policy submits the cover and declared gallery. Missing
`previewGallery` preserves existing additional images; an explicit `[]`
removes only images previously uploaded by this tool for the same package.
Steam preview policy preserves all current images. Manually uploaded images
and videos are preserved. Do not use the reserved
`SnowDesktop-widget-<package UUID>-<index>-<SHA-256>.<extension>` filenames for
manual uploads.

The plan freezes images and reports ordered `preview.gallery` source paths and
SHA-256 hashes. Show all planned images, including any removal request, before
obtaining approval. Frozen copies are temporary and expire with the plan;
prepare a new plan for each publishing attempt. A changed prepared or staged
image aborts submission. Images are uploaded once, independently of the
existing multilingual title and description updates.

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

Use `--confirm-update` when the action starts with `update-`. Creation is
public by default; pass `--visibility private` to create a private item. Publishing emits JSON Lines progress and one final result. Preserve the
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
