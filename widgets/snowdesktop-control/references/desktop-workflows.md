# Desktop organization workflows

Read the [batch editing guide](desktop-editing.md) for request fields, capability discovery and execution. Pick the workflow that fits the requested outcome and the current layout. A first-time user with an imported layout can use existing desktop management. A request to review or suggest changes stays at the planning stage.

## First-time desktop setup

Use this workflow when the user asks for an initial arrangement or wants to build a layout from scratch.

1. **Recommend a starting arrangement.** Use `desktop.inspect` to learn the screens, items, free space and available components. Describe where the user would go to find their actual apps or files and what part of the desktop would stay clear. Choose the components, layout and presentation as part of one recommendation. Present alternatives when requested or when the preview reveals a trade-off the user needs to decide.
2. **Clarify daily use and optional folders.** Follow the [question rules](../SKILL.md#ask-progressively). Ask about what the user reaches for often only if it would change the proposed access. When access to another folder would help and is undecided, ask in its own turn whether there is a folder they often open in File Explorer that they would like to browse from the desktop. Explain that its files stay in their original directory. Ask for the path after they opt in. They can continue with the desktop's current contents while leaving that decision for later.
3. **Build and check the arrangement.** Use the [batch editing guide](desktop-editing.md) and the live component catalog for presentation and field choices. Group app and system shortcuts by task and use collection tabs when categories can share an area. Use Desktop Files for actual desktop files and map explicitly chosen directories. Use a file group when several sources belong together. Check that file filters and search are ready when the user needs them. Follow [whole-screen composition](#whole-screen-composition), preview the plan, then apply it within the request. Verify the result and explain where to find essentials.
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

## Interaction preferences

Translate a style reference or a broad goal into observable behaviors. Useful dimensions include where apps launch and switch, where status and files are reachable, when controls appear, whether they reserve space, and how much motion the user wants. A style reference can describe any subset. Existing launch habits and display preferences help establish the scope.

- Compare the `runtime-services` catalog's desktop layout presets with the requested behavior. Read their effects on the Dock, status bar, Windows taskbar and work area. Use `apply-desktop-style` when a preset's combined effects fit. Use concrete fields such as `general.dockEnabled`, `general.statusBar` and `dock` for focused changes or combinations a preset cannot express. Check the current values and explain consequential changes, such as replacing the user's visible taskbar or reserving a screen edge. Reinspect grid geometry after changing bars and before applying storage placements.
- Discover motion independently. `apply-style-animations` sets a preset's recommended Dock hover and launch animations. Search for the specific motion when the user cares about an action such as window minimize and restore. For example, a request for macOS-style app and status placement can use the `island` preset when its taskbar behavior fits. If motion is undecided and relevant, ask separately whether the user wants the optional Genie window effect. If the user already asked for it, check `dock.windowEffect` and live support and apply it directly within the request.
- For a goal such as quick file access on an otherwise empty desktop, check storage icon entrances, Dock file access and Quick Navigation. Choose using the desired number of steps, source scope and whether any contents need to be visible at rest. For a request focused on legibility, check appearance overrides and the wallpaper before altering placement.
- Verify the changed settings and describe how the user now opens, switches or finds the relevant content. Verify actual visibility and motion when the visual result is available.

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

- “I'm new to SnowDesktop. Help me organize my desktop.” Inspect the desktop and show how your proposal would make common tasks easier. If a decision remains, ask about it in a separate turn and explain the practical effect of the choices.
- “Put the three apps I just installed into Development.” Use existing desktop management. Resolve the three shortcuts and the Development collection, preview the change, apply it, and verify membership.
- “These two collections are crowded.” Use existing desktop management. Inspect the affected collections, show the proposed fit or scroll change, and check the result against the actual desktop when visual evidence is available.
