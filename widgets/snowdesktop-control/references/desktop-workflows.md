# Desktop organization workflows

Read the [batch editing guide](desktop-editing.md) for request fields, capability discovery and execution. Pick the workflow that fits the requested outcome and the current layout. A first-time user with an imported layout can use existing desktop management. A request to review or suggest changes stays at the planning stage.

## First-time desktop setup

Use this workflow when the user asks for an initial arrangement or wants to build a layout from scratch.

1. **Read the desktop and establish the goal.** Use `desktop.inspect` to learn the available pages, screens, icons, collections and space. Reuse preferences the user has already given. Ask one focused question when a missing preference would change the layout, such as which apps need direct access or whether the user wants more room for wallpaper. An empty desktop is a valid starting point. Base proposed additions on the user's stated activities and available components.
2. **Show the target arrangement.** For a new page or whole desktop, show a labeled sketch at the affected screens' proportions. Mark the direct-access icons, collection locations and relative sizes, reserved space, and content reached by scrolling or switching pages. Use the amount of content and its access priority to size each collection. Show a second option only when a concrete tradeoff calls for a choice.
3. **Check and apply the plan.** Preflight the complete arrangement with the batch editor. Revise conflicts, unintended partial rows and placement that would hide an essential entry. Apply within the requested scope when the arrangement is clear. Use the checks below to explain what landed, how to find essentials and what still needs visual confirmation.
4. **Offer one next feature.** After handing over the organized desktop, recommend a short, optional preview based on something the user said or a need that surfaced during the task. The user chooses whether to preview, apply or finish.

| User's interest | What to offer |
| --- | --- |
| Consistent icon appearance | **Icon Beautification.** Show or open a preview of the available shape and background styles. |
| More room for wallpaper or a different place for the bars | **Desktop Layout.** Open the preset preview and explain its effects on Dock, the status bar, the Windows taskbar and reserved space before applying. |
| Information or controls they want on the desktop | **Explore Widgets.** Find a relevant built-in or Workshop component, explain what it does and where it would fit, and let the user choose whether to add it. |

Use the host's advertised settings and preview actions or direct the user to the corresponding Usage Guide lesson. If the user chooses to finish, conclude the task there.

## Existing desktop management

Use this workflow to maintain an established arrangement, including sorting new items, adjusting collections or improving a crowded area.

1. **Read the baseline and limit the edit.** Identify the affected items, collections and pages by their current state. Preserve other groups, positions, screen assignments and appearance choices. Ask for clarification when several targets match or the requested changes require movement beyond the specified area.
2. **Match the preview to the change.** For a precise request, such as adding three known shortcuts to a named collection, preview the batch and carry out the edit. For a change that visibly rearranges an area, show a short before-and-after comparison of that area. Use a screen-proportioned sketch for an explicitly requested whole-page or whole-desktop reorganization.
3. **Check the affected result.** Use the checks below. Tell the user where the items moved and explain any changed way of reaching them, such as a new scroll area. Offer a feature tip when it directly explains the change they requested.

## Verify the result

- Before writing, check batch conflicts and the relevant collection capacities. Review fully visible items, partial rows and overflow against the intended presentation.
- After applying, read back the positions and membership. Check that each affected item is at its intended destination and the layout outside the requested scope is preserved. Report saving as pending when the result is `persistence: "queued"`.
- For visual changes, compare the reference or the intended adjustment with the user's screenshot, an authorized capture of the affected desktop, or the user's observation of the live result. Check that titles are readable in the normal resting state, frequent icons are easy to locate, intended first-screen rows are legible, and the bars leave the intended space. If live visual evidence is unavailable, report the confirmed layout data and mark the visual check as pending.
- Keep working plans and layout references in the task workspace, outside SnowDesktop's runtime data directory. See [interaction and recording](interaction.md) for supported capture capabilities and scope.

## Example requests

- “First time using SnowDesktop. I mainly do research. Keep my browser and Zotero within easy reach.” Use first-time setup. Keep those entries direct, show the arrangement to scale, and offer one exploration option based on the rest of the user's context.
- “Put the three apps I just installed into Development.” Use existing desktop management. Resolve the three shortcuts and the Development collection, preview the change, apply it, and verify membership.
- “These two collections are crowded.” Use existing desktop management. Inspect the affected collections, show the proposed fit or scroll change, and check the result against the actual desktop when visual evidence is available.
