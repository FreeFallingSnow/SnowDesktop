# Understand SnowDesktop

SnowDesktop is a Windows desktop organization and personalization app built around components. Use different components to organize apps and files and view your schedule, then combine them with Dock, Quick Navigation and appearance settings to make things easier to find, everyday actions more convenient, and the desktop more to your taste.

SnowDesktop supports Windows 10 and 11. [SnowDesktop website](https://snowdesktop.com/)

## Five desktop storage components

Use a Collection component to organize office, design, gaming and other apps by purpose, so you do not have to search through a screen full of icons. When you have more groups, use a Collection Group component to bring several Collections together. Switch between tabs to access different groups of apps in the same area of your desktop.

Use the Desktop Files component to display documents, images and other files by type. For folders you use often, Folder Mapping displays their contents directly on the desktop. View reference material and open documents without repeatedly navigating into folders; the files stay in their original locations.

### Collection

Create a Collection from the desktop context menu and drag in the apps and shortcuts you want to keep together. Name groups by purpose, such as work, design or games.

Keep a Collection small for a compact entry point, or enlarge it to open visible app icons directly. For larger sets of apps, a scrolling panel can use an icon grid or a list.

### Collection Group

A Collection Group brings several Collections together. Tabs let work, creative tools and games share one area of the desktop.

Create an empty Collection Group and add existing Collections. You can also drag the bottom of one Collection onto another, hold Ctrl, then release the mouse. Dragging the bottom moves the whole component; dragging an icon moves that shortcut.

### Desktop Files

Add a Desktop Files component to bring documents, images and archives together. Drag files into it, or use its context menu to collect desktop items. Enable automatic collection for all items or only selected file types when needed.

Filter by type or enter part of a filename to find an item. The context menu can show the category bar and search field, or switch to a list with file details.

### Folder Mapping

Folder Mapping displays an existing folder while its files stay in their original directory. Convert a folder already on the desktop through its context menu, or add Folder Mapping and select another directory.

A small mapping opens its contents in a popup. Enlarge it to display the contents directly on the desktop as a grid or a list.

### File Group

A File Group combines Desktop Files and Folder Mapping components, with tabs for switching between sources such as downloads, project material and desktop documents.

Create an empty File Group and add components, or drag the bottom of one file component onto another, hold Ctrl, and release the mouse.

[Component property query commands](references/desktop-editing.md#query-component-properties)

## Pages, displays and desktop visibility

Each SnowDesktop page has its own grid. Icons and widgets occupy rows and columns on that page. When the page is shown on another monitor, SnowDesktop calculates cell sizes and spacing from that monitor’s available space and uses the row and column counts saved for the page.

Say your Work page has project folders in the upper left and an agenda widget on the right. The grid cells can be wider on the external monitor. Open the same Work page on your laptop after disconnecting the monitor and the folders and agenda use that page’s grid, with cell sizes and spacing fitted to the laptop display. In Settings → Pages and displays, you can adjust each page’s grid, name, and order.

With one monitor, you can browse all desktop pages on that screen. In an extended setup, each display before the last one shows a fixed page. The last display starts with its assigned page and can also show the remaining pages when there are more pages than monitors.

With two monitors and three pages, for example, the first display can stay on Work while the last switches between Reference and Schedule. Unplug the external monitor and you can browse all three pages on your laptop. Move the pointer to the left or right edge of the single or last display and click when the page hint appears. You can also choose a page directly from Go To in the desktop context menu.

After organizing the desktop, you may want to put files and storage panels out of sight and enjoy the wallpaper. SnowDesktop can toggle desktop content with a double-click or hide it after an idle delay. Choose to keep useful items such as a clock, weather widget or Dock visible.

## Dock, status bar and Windows taskbar

Put frequently used apps and folders in Dock and dock it at the edge of the screen. When you need it, bring Dock to the foreground with a keyboard shortcut or an edge trigger to access your usual content above other windows.

Dock features smooth, lively animations that add movement to app launches and to minimizing and restoring windows.

### Pinned area

The pinned area holds your usual app entries, ready to launch even when the apps are not running. It also supports pinned Collections, so a group of apps can share one entry in Dock.

### Task area

The task area shows running apps and current tasks. Use it to return to windows you have already opened and switch between the tools you are using. With window previews enabled, hovering over an app shows thumbnails of its open windows.

Dock also provides hover magnification, app launch effects and animations for minimizing and restoring windows. These effects make the transitions between an icon and its window easier to follow, and can be adjusted to suit your preference.

### File area

Pin frequently used files, folders and file entries in the file area, then open a popup to browse their contents. Keep project material within reach while you work.

When an editor or design app is in the foreground, summon Dock with a shortcut or from the screen edge. You can drag files between the file-area popup and foreground apps that support file drag and drop. For video editing, bring up the project folder and drag footage from the popup straight into the editor's media area.

The same access point also lets you put files back into the corresponding folder from apps that can drag files out. Finding, importing and collecting material can stay close to the window you are working in.

### Bar layout presets

Whether you like the separate bottom Dock and top bar familiar from macOS, prefer apps and status information together at the bottom, or want a side Dock, start with SnowDesktop’s layout presets. Apply an arrangement, then adjust its position, appearance and visibility.

Dock provides app, task and file entry points; the status bar can show time, tray icons and system information. You can also keep the Windows taskbar and style it to match the desktop.

| Layout preset | Main effect |
| --- | --- |
| Native taskbar styling | Keeps and styles the Windows taskbar. Turns off Dock and the status bar. |
| Dock with taskbar | Keeps Dock visible, sets the Windows taskbar to auto-hide and turns off the status bar. |
| Bottom island Dock + top status bar | Puts launch and task switching at the bottom and status at the top. Requests that the Windows taskbar be hidden. |
| Bottom merged bar | Combines Dock and the status bar at the bottom. Reserves work area when always visible. |
| Side Dock + top status bar | Docks the app bar on the left and reserves work area. Puts the status bar at the top and sets the Windows taskbar to hidden. |

## Quick Navigation

Use Quick Navigation to find apps, desktop items and organized files by name, without looking through icons one by one. Connect Everything to also search the local files it has indexed.

Besides finding apps and files, Quick Navigation lets you start web searches, find settings and perform simple calculations, reducing the steps needed to find an entry point or switch tools.

Enable the feature and set its shortcut on the “Quick navigation” page in SnowDesktop. Call up the panel, enter an application's name, use the arrow keys to choose a result and press Enter to open it.

Quick Navigation includes items from SnowDesktop's desktop and mapped sources. Apps and files you have organized can therefore be reached from an input panel as well as from their place on the desktop.

Choose a source to browse its entries, or type a name to find a matching item. This is useful when you remember what something is called and want to open it while another window is in front.

With Everything available, Quick Navigation can also search the local files it has indexed. Everything supplies the file index; SnowDesktop brings the results into the same interface for finding and opening items.

The file search scope depends on your sources and Everything's index. Desktop and mapped entries remain their own sources, so the scope you can search reflects the content you have connected.

Use “Web search” to send a query to a configured search engine and open the search in your default browser.

Use “SnowDesktop settings” to find this app's configuration options and jump to the relevant settings page.

Use “Run command” to open a program or execute a run command from the same input panel.

Use “Calculator” for simple arithmetic. Read the result in the panel and press Enter to copy it for use elsewhere.

## Desktop widgets

Use the Schedule, ToDo List and Sticky Note components to keep today's plans, tasks and temporary notes on your desktop, ready to see when you return to it.

You can also add clocks, Month Calendar, Media Controls, System Monitor and other components to check the time, control music or follow system activity. Combine and arrange them as needed, keeping only what you use.

## Personalization

Use Icon beautification to give app icons a consistent style, combining background colors, glass textures and color filters so icons from different apps work better with your wallpaper. For a more prominent layout, turn your favorite apps into Large software icons, choose their shapes or use your own images.

Adjust the colors, transparency and frosted glass effects of Collections, Dock, the Status bar and desktop components to coordinate the whole desktop. Save styles you like as a Theme, switch whenever you want, and import or export themes without adjusting everything again.

## Steam Workshop and editions

Subscribe to community-made components and themes through Steam Workshop to add new tools and decorations to your desktop or try different looks.

If you have an idea of your own, AI can help turn it into a component. SnowDesktop provides a component creation Skill for use with AI coding tools that support Skills. AI can assist with describing requirements, generating the component, previewing and adjusting it, checking and packaging it, and publishing it to Workshop after your confirmation.

The Microsoft Store and Steam editions both provide core desktop features. Steam provides Workshop and large app icons. Advanced features depend on the current unlock state. [Compare editions](https://snowdesktop.com/compare/)

## What the agent can do

| Capability | What it provides |
| --- | --- |
| Control a running SnowDesktop installation | Read the current state, change supported settings and layouts, and open supported app surfaces. |
| Capture and replay | Take screenshots, record interactions, replay them, or export fixed-data demos. |
| Create widgets | Use the separate SnowDesktop Lua Widget Skill to design and package new widgets. |
