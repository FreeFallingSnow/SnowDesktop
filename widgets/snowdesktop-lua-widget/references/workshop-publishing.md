# Steam Workshop publishing

Choose the route that matches the requested change.

- For an existing item's tags, use Workshop Manager's **Read Steam tags** and
  **Publish tags**. This route reads or replaces the tag list independently.
- For first publication, package updates, or listing changes that use the
  package publishing workflow, use the matching Steam Bridge and review its
  plan.

## Update tags on a bound item

Open the bound project in **Workshop Manager → Publishing details** and
verify its Workshop item. Open **Tag actions → Read Steam tags** to load the
current list into the editor. Use **Add tag** in the same menu or edit the
comma-separated list. Review the complete final list with the user before
clicking **Publish tags**. This action replaces the entire Steam tag list. An
empty list clears the tags. After Steam confirms the change, the Manager saves
the submitted tags locally and shows the result in its bottom message area.
Use this workflow for tag-only requests. It requires no archive, Bridge plan
or component upload.

On an unbound project, use **Add tag** and **Save tags** to prepare the local
list that Workshop Manager will submit when it creates the listing.

### Supported widget tags

The [SnowDesktop Workshop](https://steamcommunity.com/workshop/browse/?appid=5080330)
lists these tags under **Widget**. Use the exact English spelling for Steam
filtering and choose the tags that describe the component.

- `Utilities` for general-purpose tools such as calendars, clocks and timers.
- `System Monitor` for hardware, system load and resource monitoring.
- `Media` for playback controls, lyrics and audio visualizers.
- `Fun & Interaction` for games and interactive experiences.
- `Decoration` for components such as photo displays and desktop ornaments.
- `News & Feeds` for news, RSS and other feed readers.

## Plan a package publication

Resolve `$SteamBridge` from the [loaded Skill](../SKILL.md#use-the-bundled-tools)
and keep the executable beside its bundled `steam_api64.dll`.

```powershell
$BridgeConfig = & $SteamBridge configuration | ConvertFrom-Json
```

Require `ok=true`, `steamworksCompiled=true`, `componentWorkflowProtocolVersion=1`
and `componentPlanApprovalProtocolVersion=1` for this CLI publishing workflow.
Use an updated bundled Bridge if that approval capability is absent. Pass the
active SnowDesktop data directory explicitly to use the same project associations
and history as Workshop Manager. Set `$Widget` to the package's full source path
and `$SnowDesktopData` to that installation's full data directory path.

```powershell
$PlanArgs = @('--source', $Widget, '--data-directory', $SnowDesktopData)
# Add the requested policy arguments before generating the plan.
$ReviewedPlan = & $SteamBridge workshop component-plan @PlanArgs | ConvertFrom-Json
```

The command inspects, packs and validates the archive locally before returning
its JSON plan. When a previous published archive is available, it also verifies
live upgrade. To use a specific released archive, include
`--previous-package` and its exact path in `$PlanArgs`.

Check the successful result and show the user the action (`create`,
`update-content` or `update-metadata`), the PublishedFileId for an update, the
package version and hash, listing text and localizations, cover and gallery,
tags, visibility and any change note. Identify which Steam-managed fields the
plan will preserve. Keep that exact result, including `planSha256`.

Once the user approves the displayed plan, use the same arguments and the
digest from that result:

```powershell
& $SteamBridge workshop component-publish @PlanArgs `
  --plan-sha256 $ReviewedPlan.planSha256 --confirm-create
```

For `update-content` or `update-metadata`, use `--confirm-update`. A changed
plan is rejected with `stale_plan`. Generate and review a fresh plan before
submitting it. Creation defaults to public. Include `--visibility private`
in `$PlanArgs` if the user wants a private item.

Publishing emits JSON Lines progress and a final result. Preserve the returned
PublishedFileId even if a later localization fails. Pass `--open-page` only
when the user asked to open Steam.

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

## Persistent source policies

- `--text-source package` reuses every title and description localization from
  `widget.json`; `steam` preserves current Steam text during updates;
  `manual-english --title ... [--description ...]` submits manual English.
- `--preview-source local` submits the manifest preview or `--preview FILE`;
  `steam` preserves the current Workshop preview during updates.
- `--tags-source local` submits persisted or repeated `--tag` values; `steam`
  preserves current Workshop tags. Repeat `--tag` once per value or use
  `--clear-tags` with local tags. During an ordinary content update, use
  `--tags-source steam` unless the user also asked to replace the tags.

New items cannot preserve Steam-managed values because none exist yet. A new
item requires an English title and primary preview. Changing only listing text,
preview, tags or visibility produces a metadata-only update when the packed
component SHA-256 is unchanged.
