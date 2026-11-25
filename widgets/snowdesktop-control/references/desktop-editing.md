# Control interface and batch editing

## Connect

Use the bundled PowerShell client from Windows PowerShell 5.1 or PowerShell 7. SnowDesktop must already be running in the same Windows user, session and integrity level.

From the installed Skill directory, run `scripts/snowdesktop-control.ps1 -Method api.describe`, then run the script with `-Method app.getState`. Use the script's absolute path from other working directories. The installed `connection.json` binds the client to the SnowDesktop executable that installed the Skill. The client verifies the server executable before sending the request. If SnowDesktop was moved, reinstall the Skill from the intended installation or pass its verified absolute path with `-ExecutablePath`.

Pass named JSON objects through `-ParametersJson` or a UTF-8 `-ParametersFile`. The client returns JSON-RPC responses on stdout and exits nonzero on errors. Check `runtimeMode`, `dataDirectory`, `desktopDirectory` and `debugProfile` before changing the desktop or recording it. Discover methods and capabilities from the running version.

## Read and change settings

Use short English phrases with `settings.search`. Query `settings.catalog` on a concrete target for fields, choices, constraints and available actions. Before writing, read fresh state and pass its opaque `revision` as `ifRevision`. Follow the catalog's availability, permissions, Steam unlocks and confirmation arguments. Use the named actions when a setting needs a secret or user-selected value.

```powershell
& '<skill-directory>/scripts/snowdesktop-control.ps1' -Method settings.catalog
& '<skill-directory>/scripts/snowdesktop-control.ps1' -Method settings.get -ParametersJson '{"target":{"kind":"dock"}}'
```

On `stale_settings` or `stale_layout`, read the state again and reassess the change. On `busy`, let the relevant interaction finish and wait within a bounded deadline. After a timeout or disconnect during a change, check the resulting state before retrying.

### Query component properties

```powershell
& '<skill-directory>/scripts/snowdesktop-control.ps1' -Method desktop.inspect
& '<skill-directory>/scripts/snowdesktop-control.ps1' -Method settings.catalog -ParametersJson '{"target":{"kind":"native-widget","id":"<component instance ID>"}}'
```

- When the host advertises `desktopComponentCatalog:1`, read `desktop.inspect`'s `componentCatalog` for creatable components, supported sizes, presentations and controls.
- For an existing component, use the ID from `desktop.inspect`'s `widgets` and request its catalog for current values, choices, constraints and actions. Use `widget` as the target kind for a Lua component.

## Batch layout

Discover `desktopBatchEdits:1` via `api.describe`. Check for `desktopNativeComponents:1` before using group edits or mapping a user-selected directory by path. The batch uses the same verified SnowDesktop IPC connection and Agent Operations access setting as the other methods.

1. Call `desktop.inspect`. Use the [component property queries](#query-component-properties) to choose supported settings. Read `pages` for zero-based grid coordinates, `items` for stable icon keys and placement, and `widgets` for instance IDs, member items and applicable native presentation flags. `onDesktop` indicates a loose icon and `ownerWidgetId` identifies its current component when applicable.
2. Compose one JSON object and save it as a UTF-8 plan file. Use `desktop.inspect`'s `layoutRevision` as `ifLayoutRevision`. Preview the plan, then adjust placements or collection presentation based on the result.
3. Apply the same final plan with `desktop.applyEdits`. Verify the applied placements and IDs in the result. `persistence: "queued"` means the changes are in memory and background saving is pending. After a timeout or disconnect, inspect current state before retrying.

```powershell
& '<skill>/scripts/snowdesktop-control.ps1' -Method desktop.inspect
& '<skill>/scripts/snowdesktop-control.ps1' -Method desktop.previewEdits -ParametersFile '<plan.json>'
& '<skill>/scripts/snowdesktop-control.ps1' -Method desktop.applyEdits -ParametersFile '<plan.json>'
```

Each operation array is optional and holds up to 1024 entries. Use at least one operation.

| Field | Entry | Behavior |
| --- | --- | --- |
| `addWidgets` | `{ref,type,pageId?,column?,row?,columns?,rows?,title?,itemIds?,scrollContainer?,showTitle?,titleOnHover?,packageId?,folderItemId?,folderPath?,singleCellIconMode?,showSearchBox?,showFileCategories?}` | Supply all five placement fields for a standalone component. A new group child can omit all five. `ref` is a unique label within the plan, and apply returns its instance ID. Use the last three optional fields only when `desktopComponentCatalog:1` is advertised. |
| `moveWidgets` | `{id,pageId,column,row,columns,rows}` | Move or resize an existing standalone desktop component or release a child of a desktop collection or file group to a specific cell. |
| `setCollections` | `{id,itemIds,scrollContainer?,showTitle?,titleOnHover?}` | Replace a desktop collection's complete ordered member list. Assign every removed member to another collection or a cell in the same plan. |
| `setGroups` | `{id?\|ref?,children:[{id}\|{ref}],activeChild?:{id}\|{ref}}` | Replace the complete ordered tab list of a desktop collection or file group. Use the ID of an existing component or the ref of one added in this plan. |
| `moveIcons` | `{key,pageId,column,row}` | Move loose icons or return collection members to the desktop. The host reserves the icon's required span. |

Creatable types are `collection`, `collection-group`, `file-categories` (Desktop Files), `file-group`, `folder-mapping` and `lua`. Use `itemIds` and collection presentation fields only for collections. Discover view and content settings for existing native components with `settings.catalog` on their `native-widget` target, including Desktop Files' `collect-now` action when offered.

- **Collection groups** accept collections. **File groups** accept Desktop Files and folder mappings. Give every child omitted from an edited list another group or an explicit `moveWidgets` destination in the same batch. A group occupies its own grid area, including when its children are created in that batch. The result's `groups` array includes affected groups and ordered references. Read back `memberIds` and `activeMemberId` with `desktop.inspect`.
- **Folder mappings** take one source. Use `folderItemId` for a loose physical desktop folder or `folderPath` for an existing absolute directory the user selected. Mapping displays the actual directory's contents and replaces a matching loose folder projection. It preserves the source and its files. A shortcut is a separate entry. `desktop.inspect` reports each mapping's source.
- **Icon entrances** use `singleCellIconMode:true` on a standalone 1×1 native component, including a group. The icon opens the content popup, where its enabled search and tabs are available. Use `widgets.open` for a supported entrance. `desktop.inspect` reports the active icon presentation. A 1×1 compact preview is also available.
- **File filters and search** can be included in the batch. Set `showFileCategories` on Desktop Files or Folder Mapping, including individual file group sources. Set `showSearchBox` on a standalone view or a group owner whose catalog advertises search. For an existing component, read its `native-widget` catalog to resolve current keys and IDs. In a file group, `activeCategoryId` switches between sources and `contentCategoryId` filters the selected source. Call `set-search` with `{query}` only when advertised for the open view.
- **Lua components** need the `packageId` of an installed package. If the host reports `permission_required`, resolve the grant through SnowDesktop settings before retrying.

Each item receives at most one explicit destination. Adding an item to a collection removes it from its previous desktop collection. Desktop editing preserves Dock-owned and other independently managed items. A layout can swap positions in the same plan. A conflict with an occupied or unsupported position rejects the whole plan.

Collection previews report `fullyVisible`, `partiallyVisible`, `hidden`, and compact `thumbnails` when applicable. Counts are for the proposed first screen at the top of the collection. Icon entrances put contents behind a popup. A grouped child uses its parent's presentation, so it has no standalone first-screen estimate.

Close active desktop menus, drags, storage popups, and Quick Navigation before previewing or applying. If `stale_layout` is returned, inspect again and rebuild the plan. On older hosts without `desktopNativeComponents:1`, continue to use placed collections and mappings by desktop item where suitable. Mention when grouped tabs or a chosen path need the corresponding control in SnowDesktop. Hosts without batch support can still place individual existing components through `widgets.place`.


For a single component, `desktop.getLayout` provides the current layout revision used by `widgets.place`. Placement follows the application's displacement rules.

## Open and close app surfaces

Use `widgets.open` and `widgets.close`, `navigation.open` and `navigation.close`, `dock.show` and `dock.hide`, or `settings.open` when the running host supports them. After acceptance, check `app.getState` for readiness and settled animations. For an operation absent from the live catalog, point to the corresponding SnowDesktop control when available.
