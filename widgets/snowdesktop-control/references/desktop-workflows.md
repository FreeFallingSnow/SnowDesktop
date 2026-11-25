# Desktop organization workflows

Read the [batch editing guide](desktop-editing.md) for request fields, capability discovery and execution. Pick the workflow that fits the requested outcome and the current layout. A first-time user with an imported layout can use existing desktop management. A request to review or suggest changes stays at the planning stage.

## First-time desktop setup

Use this workflow when the user asks for an initial arrangement or wants to build a layout from scratch.

1. **Recommend a starting arrangement.** Use `desktop.inspect` to learn the screens, items, free space and available components. Describe where the user would go to find their actual apps or files and what part of the desktop would stay clear. Choose the components, layout and presentation as part of one recommendation. Present alternatives when requested or when the preview reveals a trade-off the user needs to decide.
2. **Clarify daily use and optional folders.** Follow the [question rules](../SKILL.md#ask-progressively). Ask about what the user reaches for often only if it would change the proposed access. When access to another folder would help and is undecided, ask in its own turn whether there is a folder they often open in File Explorer that they would like to browse from the desktop. Explain that its files stay in their original directory. Ask for the path after they opt in. They can continue with the desktop's current contents while leaving that decision for later.
3. **Keep everyday tasks easy to reach.** Group apps and shortcuts by what the user does with them. Keep frequent tools or categories visible through an appropriate entry or panel. Use compact entrances for occasional content or when the user wants a nearly empty desktop. Weigh how quickly the user can reach common tools and current files against how much space the layout leaves.
4. **Apply the arrangement.** Use the [batch editing guide](desktop-editing.md) and the live component catalog to choose the settings. Use Desktop Files for desktop files and a file group for related sources. Keep folders reachable through their existing entries until the user chooses to show their contents. Enable file filters or search where useful. Follow [whole-screen composition](#whole-screen-composition), apply the previewed plan within the request, and read back the changed settings and positions. Explain where to find the user's essentials.
5. **Offer one next feature.** At handoff, name a specific optional next step that matches the user's goal. The user chooses whether to preview it, apply it or finish.

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

Compose each screen around its usable area, the wallpaper and the user's everyday activities. Panels often line up along one edge or form an L across two adjacent edges. Align panels along the edges they share, give visible content the room it needs, and keep the remaining open space continuous.

Standalone collections and tabbed Collection Groups can share a layout. A standalone collection keeps a category visible on its own. Tabs let categories the user opens in turn share one panel. Choose what stays visible based on the categories the user needs to reach or see together.

Loose desktop items and components shown as icons share the same icon grid. A vertical run from the upper left is a familiar Windows starting point, with placement guided by available space and the user's habits. The Dock also gives compatible components a place within reach. Use placement controls supported by the running app.

Show and preview the arrangement at the screen's proportions. Review panel alignment, first-screen capacity, icon and title legibility, the wallpaper subject and the space reserved for the taskbar or other bars. Use a supplied wallpaper reference or SnowDesktop's capture when authorized. Treat wallpaper fit as provisional without a usable view.

## Verify the result

- Before writing, check batch conflicts and the relevant collection capacities. Review fully visible items, partial rows and overflow against the intended presentation.
- After applying, read back the positions and membership. For any planned collection or file group, verify the parent type, ordered child IDs and active tab. Check that each affected item is at its intended destination and the layout outside the requested scope is preserved. Report saving as pending when the result is `persistence: "queued"`.
- For visual changes, compare the reference or the intended adjustment with the user's screenshot, an authorized capture of the affected desktop, or the user's observation of the live result. Check that titles are readable in the normal resting state, frequent icons are easy to locate, intended first-screen rows are legible, and the bars leave the intended space. If live visual evidence is unavailable, report the confirmed layout data and mark the visual check as pending.
- Keep working plans and layout references in the task workspace, outside SnowDesktop's runtime data directory. See [interaction and recording](interaction.md) for supported capture capabilities and scope.

## Example requests

- “I'm new to SnowDesktop. Help me organize my desktop.” Inspect the desktop and show how your proposal would make common tasks easier. If a decision remains, ask about it in a separate turn and explain the practical effect of the choices.
- “Put the three apps I just installed into Development.” Use existing desktop management. Resolve the three shortcuts and the Development collection, preview the change, apply it, and verify membership.
- “These two collections are crowded.” Use existing desktop management. Inspect the affected collections, show the proposed fit or scroll change, and check the result against the actual desktop when visual evidence is available.
