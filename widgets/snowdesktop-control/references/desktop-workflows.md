# Desktop organization workflows

Read the [batch editing guide](desktop-editing.md) for request fields, capability discovery and execution. Pick the workflow that fits the requested outcome and the current layout. A first-time user with an imported layout can use existing desktop management. A request to review or suggest changes stays at the planning stage.

## First-time desktop setup

Use this workflow when the user asks for an initial arrangement or wants to build a layout from scratch.

1. **Read the desktop and establish the goal.** Use `desktop.inspect` to learn the screens, icons, components and space. For an open-ended first setup, ask about the user's main activities and which entries need direct access. Ask **Folder mapping (optional)** as a separate question: “Would you like to browse the contents of a specific folder directly from the desktop?” Offer yes, no and decide later. Ask for the actual directory only when the user wants a mapping. Reuse decisions they have already given. An empty desktop is a valid starting point.
2. **Choose where each kind of item belongs.** Keep direct access for the user's priorities. Put other app and system shortcuts in task-based collections. Show actual working files with Desktop Files or a requested folder mapping. When two or more related collection categories can share a desktop area, put them in a populated `collection-group` using `setGroups`. Putting separate collection panels next to each other leaves them as separate components. Give a category its own panel when it needs to stay visible alongside the others. Combine several file sources in a `file-group` when tabs fit how the user accesses them. A request to classify every item is satisfied when each item has an accessible destination. Choose compact, large-folder or scrolling views to match the content and keep panel presentation consistent for the same role.
3. **Show and check the target arrangement.** Follow [whole-screen composition](#whole-screen-composition) before previewing and applying a new page or whole-desktop layout. When the user asks to be guided or a choice materially changes access, settle it before applying. Use the checks below to verify the applied layout and explain how to find essentials.
4. **Offer one next feature.** At handoff, name a specific optional next step that matches the user's goal. The user chooses whether to preview it, apply it or finish.

| User's interest | What to offer |
| --- | --- |
| Consistent icon appearance | **Icon Beautification.** Show or open a preview of the available shape and background styles. |
| More room for wallpaper or a different place for the bars | **Desktop Layout.** Open the preset preview and explain its effects on Dock, the status bar, the Windows taskbar and reserved space before applying. |
| Regular access to working files | **Folder Mapping.** Ask which directory they want to browse from the desktop and show how it would fit. |
| Information or controls they want on the desktop | **Explore Widgets.** Find a relevant built-in or Workshop component, explain what it does and where it would fit, and let the user choose whether to add it. |

Use the host's advertised settings and preview actions or direct the user to the corresponding Usage Guide lesson. If the user chooses to finish, conclude the task there.

## Existing desktop management

Use this workflow to maintain an established arrangement, including sorting new items, adjusting collections or improving a crowded area.

1. **Read the baseline and limit the edit.** Identify the affected items, collections and pages by their current state. Preserve other groups, positions, screen assignments and appearance choices. Ask for clarification when several targets match or the requested changes require movement beyond the specified area.
2. **Match the preview to the change.** For a precise request, such as adding three known shortcuts to a named collection, preview the batch and carry out the edit. For a change that visibly rearranges an area, show a short before-and-after comparison. For a whole-page or whole-desktop reorganization, use the composition checks below.
3. **Check the affected result.** Use the checks below. Tell the user where the items moved and explain any changed way of reaching them, such as a new scroll area. Offer a feature tip when it directly explains the change they requested.

## Whole-screen composition

- Before choosing positions, inspect the user's wallpaper or a visual reference of each affected screen. Start with a supplied image or a view from SnowDesktop's existing capture interface when authorized. Identify the main subject, quiet areas that can hold panels, and where icons and titles will be legible. If the wallpaper is unavailable or obscured, ask for a usable reference when the design relies on it. Otherwise propose a compact edge layout that leaves a continuous clear area and say that wallpaper alignment remains to be verified.
- Show a labeled sketch at the screen's proportions. Include the wallpaper area kept clear, the taskbar and other reserved edges, direct entries, panel sizes, tabs, and mapped folders. Size panels for the intended visible content and leave the rest accessible through tabs, scrolling, or a popup. Use nearly full-screen coverage only when the user wants a dense launcher. If a layout fills the grid to show every icon, reduce the footprint before applying by grouping related categories, raising only the priority content, or using available panels on a user-approved screen.
- Preflight the placements and first-screen capacity. A collision-free preview confirms the grid fits. Check the sketch against the wallpaper reference separately, including the subject's outline, contrast, and the amount of uninterrupted space.

## Verify the result

- Before writing, check batch conflicts and the relevant collection capacities. Review fully visible items, partial rows and overflow against the intended presentation.
- After applying, read back the positions and membership. For any planned collection or file group, verify the parent type, ordered child IDs and active tab. Check that each affected item is at its intended destination and the layout outside the requested scope is preserved. Report saving as pending when the result is `persistence: "queued"`.
- For visual changes, compare the reference or the intended adjustment with the user's screenshot, an authorized capture of the affected desktop, or the user's observation of the live result. Check that titles are readable in the normal resting state, frequent icons are easy to locate, intended first-screen rows are legible, and the bars leave the intended space. If live visual evidence is unavailable, report the confirmed layout data and mark the visual check as pending.
- Keep working plans and layout references in the task workspace, outside SnowDesktop's runtime data directory. See [interaction and recording](interaction.md) for supported capture capabilities and scope.

## Example requests

- “I'm new to SnowDesktop. Help me organize my desktop.” Inspect the desktop, ask about the user's activities and direct access, and ask separately about optional folder mapping. Use the answers to choose components and show the arrangement.
- “Put the three apps I just installed into Development.” Use existing desktop management. Resolve the three shortcuts and the Development collection, preview the change, apply it, and verify membership.
- “These two collections are crowded.” Use existing desktop management. Inspect the affected collections, show the proposed fit or scroll change, and check the result against the actual desktop when visual evidence is available.
