# Batch desktop editing

Discover `desktopBatchEdits:1` via `api.describe`. The feature is available through the same verified SnowDesktop IPC connection and Agent Operations access setting as the other methods.

1. Call `desktop.inspect`. Read `pages` for zero-based grid coordinates, `items` for each icon's `key` and placement, and `widgets` for instance IDs and collection `itemIds`. `onDesktop` indicates a loose icon and `ownerWidgetId` identifies its current component when applicable. An item key identifies the icon even when its display name changes or two icons have the same name.
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
| `addWidgets` | `{ref,type,pageId,column,row,columns,rows,title?,itemIds?,scrollContainer?,showTitle?,titleOnHover?,packageId?,folderItemId?}` | `ref` is a unique client label within the plan. The result uses it to identify the new component and supplies its instance ID after apply. |
| `moveWidgets` | `{id,pageId,column,row,columns,rows}` | Move or resize existing standalone desktop components. |
| `setCollections` | `{id,itemIds,scrollContainer?,showTitle?,titleOnHover?}` | Replace a desktop collection's complete ordered member list. Assign every removed member to another collection or a cell in the same plan. |
| `moveIcons` | `{key,pageId,column,row}` | Move loose icons or return collection members to the desktop. The host reserves the icon's required span. |

Creatable types are `collection`, `collection-group`, `file-categories`, `file-group`, `folder-mapping` and `lua`. Use `itemIds` and presentation fields only for collections. `folder-mapping` requires the `folderItemId` of a loose physical desktop folder. The widget replaces the folder's free desktop projection and displays that directory's contents. `lua` requires the `packageId` of an installed package. If the host reports `permission_required`, resolve the grant through SnowDesktop settings before retrying.

Each item receives at most one explicit destination. Adding an item to a collection removes it from its previous desktop collection. Desktop editing preserves Dock-owned and other independently managed items. A layout can swap positions in the same plan. A conflict with an occupied or unsupported position rejects the whole plan.

Collection previews report `fullyVisible`, `partiallyVisible`, `hidden`, and compact `thumbnails` when applicable. Counts are for the proposed first screen at the top of the collection. They help choose dimensions, persistent titles, and scrolling. A grouped child uses its parent's presentation, so it has no standalone first-screen estimate.

Close active desktop menus, drags, storage popups, and Quick Navigation before previewing or applying. If `stale_layout` is returned, inspect again and rebuild the plan. Older hosts can still place individual existing components through `widgets.place`. Report unavailable batch operations on those hosts.
