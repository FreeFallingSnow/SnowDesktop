#pragma once

#include "settings_ipc_values.h"
#include <array>
#include <string_view>
#include <utility>

namespace snowdesktop::control_settings::guidance
{
using Description = std::pair<std::string_view, std::string_view>;

template<class T>
inline constexpr auto Fields = std::array<Description, 0>{};

// Every member of a documented reflected type has a matching description.
// Keep descriptions at the semantic owner, so repeated theme/property types
// inherit the same explanation in every target that embeds them.
template<class T>
consteval bool CoversFields()
{
    auto names = settings_ipc::Fields<T>::Names;
    for (std::size_t index = 0; index < Fields<T>.size(); ++index)
    {
        const auto comma = names.find(',');
        auto name = names.substr(0, comma);
        const auto first = name.find_first_not_of(" \t\r\n");
        const auto last = name.find_last_not_of(" \t\r\n");
        if (first == std::string_view::npos)
            return false;
        name = name.substr(first, last - first + 1);
        const auto& [key, description] = Fields<T>[index];
        if (!name.starts_with("v.") || name.substr(2) != key || description.empty())
            return false;
        names = comma == std::string_view::npos ? std::string_view{} : names.substr(comma + 1);
    }
    return names.empty();
}

template<>
inline constexpr auto Fields<SettingsValues> = std::to_array<Description>({
    {"personalization", "Shared visual and interaction defaults. A surface or component with an independent theme can override them."},
    {"dock", "Dock layout and Windows taskbar integration. Windows shell actions can finish asynchronously."},
    {"navigation", "Quick Navigation shortcut, search engines, desktop views, favorites, layout and colors."},
    {"general", "Application-wide desktop, language, animation, status bar, calendar and menu preferences."},
    {"category", "Desktop file classification and the shared appearance of category tabs."},
    {"desktop", "Desktop-grid item metrics, title presentation and icon effects stored with the desktop layout."},
});

// See ResolvePageItemVisualMetrics, ApplyIconSpacingToPage and the
// layout-backed setters in app_page_grid.cpp for these coupled values.
template<>
inline constexpr auto Fields<DesktopDisplaySettings> = std::to_array<Description>({
    {"dockEnabled", "Mirrors the shared Dock enable switch. Change general.dockEnabled to control the Dock."},
    {"iconSpacingScale", "Changes the shared desktop layout spacing, including gaps around components and gaps in fixed collection previews. The grid solver clamps the gaps to preserve room for an icon and its title. At fixed page dimensions the icon baseline uses default spacing. To give the icon rows more breathing room in a fixed large-folder collection, use largeFolderTitleLines for one-line captions."},
    {"scrollingIconSpacingYScale", "Changes vertical separation in scrolling icon grids. The viewport uses icon and title height as its row-content baseline, then adds spacing."},
    {"listRowSpacingScale", "Changes row spacing in the applicable scrolling list views. List icon size and caption font are controlled separately by desktop.itemIconSizeScale and desktop.listItemFontSizeCu."},
    {"itemIconSizeScale", "Multiplies the ordinary icon edge length, with 1 meaning the page baseline. The baseline uses page columns, rows and the visual work area, reserves space for the collapsed title at desktop.itemFontSizeCu, and uses default grid gaps. The scaled icon is capped to fit the pitch. List rows use a separate compact icon baseline with the same multiplier."},
    {"itemFontSizeCu", "Sets the caption size in component units before scaling by the page pitch. Taller collapsed title text reduces the height available to ordinary grid icons. List and fan captions have separate font sizes."},
    {"listItemFontSizeCu", "Sets the caption size for native component list rows in component units. The row layout also considers its compact icon, metadata and list spacing."},
    {"fanTitleFontSizeCu", "Sets the component-unit font size for titles shown by the fan presentation. Desktop-grid and list captions use their own font settings."},
    {"itemFontWeight", "Sets the DirectWrite font weight of item captions in the desktop and component renderers. It changes the glyph weight independently of caption point size and icon size."},
    {"desktopTitleLines", "Chooses one or two lines for ordinary desktop item captions. The page icon baseline is calculated independently of this line-count choice."},
    {"largeFolderTitleLines", "Chooses one or two lines for item captions in fixed large-folder collection grids with titles visible. One line shortens the title area and distributes the freed height between rows and at both outer edges, including above the first row. Icon size, row centers and grid capacity stay the same. Scrolling icon grids use scrollingTitleLines."},
    {"scrollingTitleLines", "Chooses one or two lines in scrolling icon-grid captions. List mode uses its own row text layout."},
    {"shortcutArrowMode", "Controls the shortcut overlay on shell items that are links. 0 shows it on non-application shortcuts, 1 hides it on all shortcuts, and 2 shows it on all shortcuts. Steam and Epic Games launch links count as applications."},
    {"iconBeautify", "Global treatment for ordinary icons and their loading placeholders on supported surfaces. The loading glyph keeps its native proportions within the styled plate. Individual icon appearance can inherit or override the treatment of the loaded icon, and file thumbnails have a separate participation switch."},
    {"titleEllipsis", "Enables ellipsis trimming in title layouts when the full caption exceeds the available text area."},
    {"groupComponentBadges", "Shows component-type badges on eligible item icons in a group. The renderer only draws badges for supported component types."},
    {"beautifyFileThumbnails", "Includes rendered file thumbnails in the global icon beautification treatment. The icon appearance settings still determine the treatment."},
    {"iconShadows", "Independent drop-shadow settings for ordinary icons by surface, Dock task thumbnail badges, and the subject inside a beautified icon."},
});

template<>
inline constexpr auto Fields<IconShadowSettings> = std::to_array<Description>({
    {"fan", "Enable shadows for icons in the fan presentation."},
    {"dock", "Enable shadows for Dock item icons."},
    {"exposedWidget", "Enable shadows for icons drawn in expanded desktop components."},
    {"desktop", "Enable shadows for ordinary icons on the desktop grid."},
    {"popup", "Enable shadows for item icons in a native component popup."},
    {"quickNavigation", "Enable shadows for icons in Quick Navigation."},
    {"subject", "Enable the shadow for the foreground subject inside a beautified icon."},
    {"fanStrength", "Set shadow strength for fan icons when fan shadows are enabled."},
    {"dockStrength", "Set shadow strength for Dock icons when Dock shadows are enabled."},
    {"exposedWidgetStrength", "Set shadow strength for icons in expanded components when their shadows are enabled."},
    {"desktopStrength", "Set shadow strength for desktop grid icons when desktop shadows are enabled."},
    {"popupStrength", "Set shadow strength for popup icons when popup shadows are enabled."},
    {"quickNavigationStrength", "Set shadow strength for Quick Navigation icons when their shadows are enabled."},
    {"subjectStrength", "Set shadow strength for the extracted icon subject when subject shadows are enabled."},
    {"dockThumbnailBadge", "Enable the drop shadow behind the application badge at the lower right of each Dock task thumbnail. The default is on. This setting applies when the Dock shows separate window thumbnails and works independently of the ordinary Dock icon shadow."},
    {"dockThumbnailBadgeStrength", "Set the Dock task thumbnail badge shadow strength as a percentage from 20 to 200, with 100 as the default. The saved strength takes effect when dockThumbnailBadge is enabled."},
});

template<>
inline constexpr auto Fields<widget_runtime::WidgetHostAppearancePatch> = std::to_array<Description>({
    {"followPersonalization", "Choose whether the component inherits shared personalization. Custom host appearance takes effect when the component uses independent appearance."},
    {"presetId", "Select the component's host surface appearance preset. The component instance and the saved theme binding determine which appearance is effective."},
    {"backgroundColor", "Set the independent host panel's background color. This changes the surface beneath the component's Lua-rendered content."},
    {"borderColor", "Set the independent host panel border color. The border must have a visible width and opacity."},
    {"backgroundOpacity", "Set opacity of the host surface background in the independent appearance. A selected preset and gradient determine which paint is used."},
    {"borderOpacity", "Set opacity of the host panel border. The border color and width are independent settings."},
    {"borderWidth", "Set thickness of the host panel border rather than stroke widths inside Lua-rendered content."},
    {"edgeHighlightEnabled", "Enable the host panel's highlighted edge. Its strength, width and light parameters are independently configurable."},
    {"edgeHighlightWidth", "Set the width of the host edge highlight when enabled."},
    {"edgeHighlightStrength", "Set the intensity of the host edge highlight when enabled."},
    {"edgeLight", "Set directional light, glow and shadow parameters used by the host edge effect."},
    {"gradientEndOpacity", "Set the opacity at the end of the legacy host panel gradient. A custom panel gradient defines its own stop opacities."},
    {"glassEnabled", "Request the host panel's glass material for the independent component appearance."},
    {"acrylicEnabled", "Request acrylic material for the independent host surface."},
    {"contentTheme", "Override the host foreground theme supplied to this component. Use it to choose text and foreground contrast independently of the panel paint."},
    {"clearContentTheme", "Remove the foreground theme override so the component uses the inherited foreground theme."},
    {"panelGradient", "Set the host panel gradient, including its angle, interval and stops. This changes the host surface and leaves any Lua-drawn background under the component's control."},
});


template<>
inline constexpr auto Fields<GeneralSettings> = std::to_array<Description>({
    {"showAdvancedSettings", "Reveals the advanced settings group, including Runtime Modules. Access to the Agent Operations page is independent."},
    {"agentOperationsEnabled", "Allows local agent requests when enabled by the user in Settings > Agent Operations. This value is read-only over the control interface. Turning it off denies new requests and cancels private input, capture and demo export."},
    {"steamShowRunningStatus", "Shows SnowDesktop as running in Steam when a compatible bridge connects. If the connection fails or is lost, SnowDesktop retries after 30 seconds with a fresh bridge process. Turning this off stops the running-status bridge."},
    {"steamRoomVisible", "Keeps the Steam room service listening in the background for participation. The bridge must be available."},
    {"runtimeModules", "Controls which optional runtime modules may load. Use runtime-services for their actual state."},
    {"diagnosticLogModules", "Selects which diagnostic modules emit detailed logs. Unknown flag bits are rejected."},
    {"autoStartEnabled", "Reports the Windows logon-task state. Use the set-auto-start action to change it."},
    {"softwareDesktopEnabled", "Enables SnowDesktop's desktop presentation. Turning it off restores normal Windows desktop operation."},
    {"demoModeEnabled", "Uses synthetic presentation data for demos. This setting changes what the desktop can display."},
    {"doubleClickHideDesktop", "Lets a double-click on free desktop space toggle desktop item visibility."},
    {"desktopIdleHideEnabled", "Hides desktop items after the configured idle delay. Review the Windows-visibility pause preference before changing the threshold."},
    {"desktopIdleHideDelaySeconds", "Sets the inactivity time in seconds before the enabled idle-hide rule takes effect."},
    {"desktopIdleHidePauseWithWindows", "Pauses desktop idle hiding while qualifying windows are present. Applies only when idle hiding is enabled."},
    {"screenTimeEnabled", "Collects local screen-time statistics. Turning it off stops collection and preserves history."},
    {"softwareDesktopHotkeyEnabled", "Registers the configured shortcut for enabling or disabling the software desktop."},
    {"softwareDesktopHotkeyModifiers", "Sets the Windows modifier mask for the software desktop hotkey. Change alongside its virtual key if replacing the chord."},
    {"softwareDesktopHotkeyVirtualKey", "Sets the Windows virtual key for the software desktop hotkey."},
    {"forceRestartHotkeyEnabled", "Registers the configured emergency restart shortcut."},
    {"forceRestartHotkeyModifiers", "Sets the Windows modifier mask for the emergency restart shortcut."},
    {"forceRestartHotkeyVirtualKey", "Sets the Windows virtual key for the emergency restart shortcut."},
    {"desktopPassthroughHotkeyEnabled", "Enables the shortcut that switches desktop and Dock input temporarily to passthrough. The active state is transient."},
    {"desktopPassthroughHotkeyModifiers", "Sets the Windows modifier mask for the passthrough shortcut."},
    {"desktopPassthroughHotkeyVirtualKey", "Sets the Windows virtual key for the passthrough shortcut."},
    {"pageNavigationKeyboardEnabled", "Enables desktop page-switching keyboard shortcuts."},
    {"pageNavigationPreviousModifiers", "Sets the Windows modifier mask for switching to the previous desktop page."},
    {"pageNavigationPreviousVirtualKey", "Sets the Windows virtual key for switching to the previous desktop page."},
    {"pageNavigationNextModifiers", "Sets the Windows modifier mask for switching to the next desktop page."},
    {"pageNavigationNextVirtualKey", "Sets the Windows virtual key for switching to the next desktop page."},
    {"quickNavTheme", "Sets the legacy Quick Navigation theme selection used by the surface resolver. 0 is dark, 1 light, 2 dark acrylic and 3 light acrylic."},
    {"collectionPopupTheme", "Sets the legacy collection popup theme selection used by the surface resolver. 0 is dark, 1 light, 2 dark acrylic and 3 light acrylic."},
    {"dockEnabled", "Turns the SnowDesktop Dock on or off. This is the writable source for the desktop.dockEnabled projection."},
    {"widgetDeveloperToolsEnabled", "Shows the Component Development Tools entry in Settings."},
    {"calendarDisplay", "Configures the calendar shown by built-in calendar displays and compatible Lua component data."},
    {"language", "Sets the UI language to an installed locale or system. The AI guidance remains in English."},
    {"font", "Selects the application UI font package and family. The selected font is applied after restart."},
    {"animationMode", "Sets the application animation policy. 0 follows system accessibility, 1 enables animations, and 2 disables them."},
    {"popupAnimationEffect", "Selects how popups animate when permitted. 0 disables the effect, 1 fades, and 2 scales."},
    {"animationSpeed", "Selects the shared animation duration preset. The runtime derives a duration multiplier from this index."},
    {"widgetHoverExpandEffect", "Selects the animation for expanding a component on hover. It only applies to components configured for hover expansion."},
    {"widgetHoverVisibilityEffect", "Selects the animation when a hover-only component appears or disappears. 0 disables the transition and 1 fades."},
    {"animationFrameLimit", "Sets a requested animation frame cap of 30, 60 or 120 FPS. 0 leaves the cap automatic, and power-saving preferences can reduce the effective rate."},
    {"animationEnergySaver", "Lets the runtime lower the effective animation frame limit while system Energy Saver is active."},
    {"animationOnBattery", "Enables the battery-dependent animation frame-limit policy."},
    {"quickNavigationAppearance", "Selects the independent appearance of Quick Navigation if it overrides the shared appearance."},
    {"collectionPopupAppearance", "Selects the independent appearance of native collection popups if they override the shared appearance."},
    {"globalQuickNavigationAppearance", "Stores the material snapshot used for Quick Navigation when a global theme binding supplies one."},
    {"globalCollectionPopupAppearance", "Stores the material snapshot used for collection popups when a global theme binding supplies one."},
    {"tooltipAppearanceOverride", "Overrides tooltip appearance when enabled. Otherwise the tooltip resolver uses collection popup appearance."},
    {"barPanelAppearanceOverride", "Overrides appearance for the relevant bar panels when enabled."},
    {"shellExtensions", "Configures Windows Shell context menu extension visibility, placement and per-location overrides."},
    {"statusBar", "Configures the SnowDesktop status bar and its built-in control center, indicators and appearance."},
    {"contextMenuExpandQuickActions", "Expands the quick actions in supported SnowDesktop context menus."},
    {"contextMenuHidePageManagement", "Hides the page management entry from the desktop context menu."},
    {"contextMenuHideMoreManagement", "Hides the additional management commands in the desktop context menu."},
    {"tooltipFontSize", "Sets the native tooltip font size before host scaling. Tooltip material is controlled separately."},
    {"gridPreviewHotkeyEnabled", "Enables the desktop grid overlay shortcut. It is a visual preview of the grid."},
    {"gridPreviewHotkeyModifiers", "Sets the Windows modifier mask for the desktop grid preview shortcut."},
    {"gridPreviewHotkeyVirtualKey", "Sets the Windows virtual key for the desktop grid preview shortcut."},
    {"touchSimulationEnabled", "Enables system-wide mouse-to-touch simulation while the binding is held. Physical mouse movement and buttons are intercepted during conversion. Aim with the separate pointer, click or drag with the left button for touch, and scroll to pinch. The enabled setting is restored at startup."},
    {"touchSimulationModifiers", "Sets the Windows MOD_* mask of modifiers to hold with the main key or mouse button. A standalone modifier is recorded as the virtual key itself, so its own mask bit must be omitted. Change together with touchSimulationKey when replacing a binding."},
    {"touchSimulationKey", "Sets the Windows virtual-key code of the held trigger. Keyboard keys and the right, middle and two extended mouse buttons are supported. The default is Right Shift (161). Set the key and modifiers to zero and disable simulation to clear the binding."},
});

template<>
inline constexpr auto Fields<PersonalizationSettings> = std::to_array<Description>({
    {"widgetBgR", "Sets the red component of panel background paint from 0 to 1. The surface preset and overrides determine where it is used."},
    {"widgetBgG", "Sets the green component of panel background paint from 0 to 1. The surface preset and overrides determine where it is used."},
    {"widgetBgB", "Sets the blue component of panel background paint from 0 to 1. The surface preset and overrides determine where it is used."},
    {"widgetBorderR", "Sets the red component of the host panel outline from 0 to 1."},
    {"widgetBorderG", "Sets the green component of the host panel outline from 0 to 1."},
    {"widgetBorderB", "Sets the blue component of the host panel outline from 0 to 1."},
    {"widgetAlpha", "Sets base panel opacity from 0 to 1 and the start opacity for the legacy gradient. A custom panel gradient has its own stop opacities. Independent component appearance can replace it."},
    {"widgetBorderAlpha", "Sets outline opacity from 0 to 1 independently of panel opacity and edge highlights."},
    {"widgetBorderWidth", "Sets the host panel outline width in logical pixels. It is separate from the edge highlight width."},
    {"widgetEdgeHighlightEnabled", "Enables the two-sided edge reflection for the host surface. Its width, intensity and directional light terms are separate."},
    {"widgetEdgeHighlightWidth", "Sets the thickness of the enabled edge reflection in logical pixels."},
    {"widgetEdgeHighlightStrength", "Sets the intensity of the enabled edge reflection from 0 to 1."},
    {"edgeLight", "Configures the direction, width, glow and shadows of the edge reflection."},
    {"gradientEndA", "Sets end opacity for the legacy panel gradient. A custom panel gradient uses explicit stop opacity."},
    {"barHeight", "Sets the nominal height of the native component title and action strip before layout scaling."},
    {"scrollableTitleBarOnTop", "Enables the top title bar layout where scrollable native components support it. Per-component collapse and hover expansion apply only to eligible top bars."},
    {"widgetTransformCursors", "Shows the native transform cursors on supported component resize and move handles."},
    {"categorizedTabHeight", "Sets the nominal height of native category tab strips before component scaling."},
    {"luaWidgetContentRowHeight", "Sets the default row metric used by Lua component layouts that consume the host row height."},
    {"showCategoryTabCounts", "Shows item counts on supported category tabs."},
    {"showGroupTabCounts", "Shows item counts on supported group tabs."},
    {"popupHoverOpen", "Allows eligible native component popups to open on hover after popupHoverDelayMs."},
    {"popupHoverDelayMs", "Sets in milliseconds how long a supported opener must be hovered before auto-opening the popup."},
    {"popupCloseOnMouseLeave", "Closes applicable popups when the pointer leaves the popup and its applicable opener region."},
    {"backgroundPreset", "Selects the surface material preset. 0 is dark, 1 light, 6 dark glass, 7 light glass, 9 custom, 10 dark acrylic, 11 light acrylic, 12 taskbar transparent, and 13 transparent glass. Individual surfaces support different choices."},
    {"cornerRadius", "Sets host panel corner curvature in logical units before per-surface scaling and geometry clamps."},
    {"contextMenuStyle", "Selects the Windows menu appearance. 0 is opaque Win11 following system light or dark, 1 and 2 are Win11 light and dark, 3 and 4 are opaque Win11 light and dark, 5 and 6 are Win10 light and dark, 7 is Win11 following the system theme, and 8 is Win10 following the system theme."},
    {"glassEnabled", "Requests the glass material in the custom surface appearance. The active preset and host support determine the actual rendering."},
    {"glassBlurRadius", "Sets the blur radius for the glass material when the surface uses it."},
    {"acrylicEnabled", "Requests acrylic in the custom surface appearance. The resolved material and surface support determine whether acrylic is used."},
    {"contentTheme", "Selects foreground colors separately from material. 0 is dark foreground theme mode and 1 is light foreground theme mode."},
    {"panelGradient", "Specifies custom background paint stops, angle and interval where the surface supports custom gradients."},
    {"groupTabHoverSwitch", "Changes the selected group child when its tab is hovered long enough. tabHoverDelayMs supplies the delay."},
    {"categoryTabHoverSwitch", "Changes the active file category when its tab is hovered long enough. tabHoverDelayMs supplies the delay."},
    {"tabHoverDelayMs", "Sets the hover delay in milliseconds for both enabled group and category tab hover switching."},
    {"windowPinBorderEnabled", "Shows the outline around a pinned topmost window."},
    {"windowPinBorderR", "Sets the enabled topmost window outline's red channel from 0 to 1."},
    {"windowPinBorderG", "Sets the enabled topmost window outline's green channel from 0 to 1."},
    {"windowPinBorderB", "Sets the enabled topmost window outline's blue channel from 0 to 1."},
    {"windowPinBorderWidth", "Sets the thickness of the enabled topmost window outline."},
    {"windowPinBorderOpacity", "Sets the opacity of the enabled topmost window outline from 0 to 1."},
    {"folderDoubleClickPreview", "Opens the SnowDesktop folder preview on supported folder double-clicks. Folder handling depends on the active native gesture path."},
});

template<>
inline constexpr auto Fields<snowdesktop::SurfaceTheme> = std::to_array<Description>({
    {"mode", "Selects the surface resolver mode. -1 follows the global binding and 4 applies independent appearance. The status bar also accepts dedicated glass and transparent modes. Use the target's enum and surface-specific options."},
    {"customized", "Records whether this surface has its own appearance settings for the resolver."},
    {"appearance", "Stores independent material values used when the surface mode selects them."},
});

template<>
inline constexpr auto Fields<PopupAppearanceRule> = std::to_array<Description>({
    {"enabled", "Applies this surface override when on. Turning it off returns to the appearance inherited by that surface."},
    {"theme", "Defines the resolved appearance used by the enabled override."},
});

template<>
inline constexpr auto Fields<StatusBarAppearanceRule> = std::to_array<Description>({
    {"enabled", "Applies the configured status bar appearance when the corresponding window state is active."},
    {"theme", "Defines the material and foreground theme for this window-state override."},
});

template<>
inline constexpr auto Fields<PanelGradient> = std::to_array<Description>({
    {"enabled", "Uses the custom gradient where the selected surface material supports it."},
    {"angle", "Rotates the gradient axis in degrees across the surface."},
    {"start", "Sets the normalized start of the gradient interval along its axis and must precede end."},
    {"end", "Sets the normalized end of the gradient interval along its axis and must follow start."},
    {"stops", "Supplies the ordered color and opacity stops used to paint that interval. The native validator requires distinct valid positions."},
});

template<>
inline constexpr auto Fields<PanelGradientStop> = std::to_array<Description>({
    {"position", "Sets the stop position from 0 to 1 within the gradient interval."},
    {"color", "Sets the stop's packed RGB color as an integer."},
    {"opacity", "Sets the alpha of this stop from 0 transparent to 1 opaque."},
});

template<>
inline constexpr auto Fields<CategorySettings> = std::to_array<Description>({
    {"tabFontSize", "Sets the nominal font size for file category tabs before the host scales their layout."},
    {"rules", "Edits the category labels and extension match rules used to classify desktop items. .URL:STEAM and .URL:EPIC match their respective game launch shortcuts. Applying changes can move items between categories or affect automatic collection."},
    {"collectProgramsEnabled", "Includes eligible program shortcuts in category collection. Their eligibility still depends on classification and the target component's collection rules."},
});

template<>
inline constexpr auto Fields<CategoryRule> = std::to_array<Description>({
    {"id", "Identifies this category in tabs and collection ownership preferences. Keep existing IDs when changing a rule."},
    {"customLabel", "Overrides the name shown on the category tab."},
    {"extensions", "Sets the file extensions matched by this category rule."},
    {"enabled", "Includes this rule when classifying items and determining applicable collection sources."},
});


template<>
inline constexpr auto Fields<DockSettings> = std::to_array<Description>({
    {"position", "Selects the Dock edge. 0 is bottom, 1 top, 2 left, and 3 right. Desktop style and merged status bar constraints can change which positions are applicable."},
    {"edgeAttached", "Attaches the Dock to its monitor edge. Island distance and island backdrop settings apply to the detached presentation."},
    {"edgeDistanceScale", "Multiplies the detached Dock's nominal margin from the edge. It uses the desktop component margin as a baseline."},
    {"floatingShortcutMode", "Enables the floating Dock hotkey. Edge swipe is controlled separately by floatingEdgeSwipeEnabled."},
    {"floatingHotkeyModifiers", "Sets the Windows modifier mask for summoning the floating Dock."},
    {"floatingHotkeyVirtualKey", "Sets the Windows virtual key for summoning the floating Dock."},
    {"floatingEdgeSwipeEnabled", "Enables gesture-based invocation of the floating Dock subject to the fullscreen and monitor rules."},
    {"summonOnAppMinimize", "Allows the window-action path to summon the Dock when an application window is minimized."},
    {"edgeRevealGesture", "Selects the reveal trigger at the eligible screen edge. 0 uses swipe and 1 uses hover."},
    {"edgeHoverDelayMs", "Sets the dwell time in milliseconds for the hover reveal gesture."},
    {"fullscreenPolicy", "Controls Dock behavior in fullscreen. 0 permits it, 1 blocks gesture invocation, and 2 applies full protection. Per-executable exceptions can override it."},
    {"fullscreenExceptions", "Overrides Dock fullscreen protection for individual executables."},
    {"monitorScope", "Selects the eligible monitor scope. 0 is the first monitor, 1 the last, and 2 all eligible monitors."},
    {"showWindowsButton", "Shows the Windows or Start button in the Dock."},
    {"showQuickSearchButton", "Shows the Quick Navigation button in the Dock."},
    {"startIconBorder", "Adds a border and insets the default Dock Start icon while icon beautification is off for it. Turn off to draw the default icon at the full Dock icon size. Defaults to on."},
    {"quickSearchIconBorder", "Adds a border and insets the default Dock Quick Search icon while icon beautification is off for it. Turn off to draw the default icon at the full Dock icon size. Defaults to on."},
    {"recycleBinIconBorder", "Adds a border and insets the default Dock Recycle Bin icon while icon beautification is off for the current state. Turn off to draw the default icon at the full Dock icon size. The empty and full states share this preference. Defaults to on."},
    {"showRunningApps", "Reports the running-app display state. The current product keeps it enabled."},
    {"showWindowPreviews", "Opens a larger preview of an application's windows when hovering over its Dock icon or swiping from the icon toward the screen content. Clicking an icon with multiple windows opens the picker even when this setting is off."},
    {"minimizeWindowsIntoApplicationIcon", "On groups minimized windows under their application icons. Off keeps the application icons and shows a separate thumbnail for each eligible minimized window in the Dock task area. The thumbnails anchor minimize and restore animations. Clicking a thumbnail restores its window. An encrypted cache can reuse the last available image for the same window if SnowDesktop restarts within the same Windows session. The showWindowPreviews setting controls larger previews from the application icons independently."},
    {"singleClickLaunchItems", "Enables launching ordinary Dock items with a single click."},
    {"freeItemOrder", "Lets entries move between the pinned and file sections. Quick Search is sortable within the file section, including relative to Recycle Bin and separators. Off returns ordinary entries to their default sections, keeps separators in their current sections, and returns Quick Search and Recycle Bin to the end of the file section in that order. The resulting order becomes the current layout and is used when free item order is turned on again."},
    {"startMenuKeepTaskbarHidden", "Keeps the Windows taskbar hidden for the applicable Start menu invocation path."},
    {"startMenuKeepTaskbarHiddenOnWinKey", "Extends taskbar hiding to the Windows-key Start menu path when applicable."},
    {"showFrequentItems", "Displays frequently used items along with the Dock's other entries. frequentItemCount limits their count."},
    {"keepWhenDesktopHidden", "Keeps the Dock eligible when SnowDesktop hides desktop items."},
    {"allowDesktopContentOverlap", "Allows desktop content to occupy the Dock area where the layout otherwise avoids it."},
    {"showOnlyWhenSummoned", "Keeps the Dock hidden until summoned. On a touchscreen, swipe inward from its edge; Windows can reserve its own system edge gestures."},
    {"reserveScreenSpace", "Reserves a work-area strip where the persistent Dock supports reservation. This can change desktop layout geometry."},
    {"lastMonitorUseHomeSize", "Uses the home Dock sizing on the last monitor instead of deriving sizes independently for that monitor."},
    {"mergedBarHeight", "Sets the merged Dock and status bar height independently of page density and merged icon scaling."},
    {"frequentItemCount", "Limits how many frequent items can appear when that Dock section is enabled."},
    {"thicknessScale", "Scales the ordinary Dock's thickness and associated layout sizing. The merged bar uses its own height."},
    {"mergedIconScale", "Scales the icon within the merged bar without changing its height."},
    {"systemTaskbarAutoHide", "Requests the Windows taskbar's auto-hide behavior. Native shell support and taskbar position determine whether the change takes effect."},
    {"suppressSystemTaskbar", "Suppresses the Windows taskbar where the integration is supported."},
    {"suppressSystemTaskbarAllMonitors", "Extends taskbar suppression to secondary monitors when suppression is enabled."},
    {"systemTaskbarAlignment", "Requests left alignment with 0 or centered alignment with 1. The Windows shell applies the request asynchronously."},
    {"systemTaskbarBackdropEnabled", "Enables SnowDesktop's backdrop treatment for the Windows taskbar. Availability depends on shell support and runtime modules."},
    {"systemTaskbarFollowPersonalization", "Inherits the global personalization for the taskbar backdrop instead of using independent appearance."},
    {"systemTaskbarContentTheme", "Sets taskbar foreground colors. -1 inherits, 0 selects light and 1 selects dark."},
    {"systemTaskbarAppearance", "Stores independent base taskbar material when appearance following is off."},
    {"systemTaskbarVisibleWindow", "Overrides taskbar appearance when an applicable window is visible."},
    {"systemTaskbarMaximizedWindow", "Overrides taskbar appearance for the maximized-window scene."},
    {"systemTaskbarShellUi", "Overrides taskbar appearance while eligible Windows shell UI is active."},
    {"hoverEffect", "Selects the Dock icon hover effect. It runs only under the active animation policy."},
    {"hoverScale", "Sets the magnification ceiling for the relevant Dock icon hover effect."},
    {"launchEffect", "Selects the animation for a Dock entry when launched. The active animation policy can suppress it."},
    {"windowEffect", "Selects the Dock-integrated window minimize and restore effect. 3 is the macOS-style Genie effect, which falls back to Scale when Genie cannot run. Availability depends on the window and current animation policy."},
    {"followComponentAppearance", "Inherits shared component appearance for the Dock. Independent Dock appearance uses its own preset and custom values."},
    {"customAppearance", "Stores independent Dock material used by the custom preset."},
    {"appearancePreset", "Selects the Dock material preset when independent appearance is active."},
    {"classicTaskbarSystemTheme", "Sets the shared Windows 10 shell theme for taskbar and panels. -1 matches appearance, 0 is light and 1 is dark."},
    {"trayApplications", "Configures executable-specific rules for using eligible application tray actions from the Dock."},
    {"followComponentCornerRadius", "Uses the component corner radius for the Dock instead of the independent Dock radius."},
    {"cornerRadius", "Sets the independent Dock corner radius when it does not follow the component radius."},
    {"attentionDotSizeScale", "Scales the visual size of the running or attention marker independently of the Dock icon."},
    {"attentionDotOffset", "Shifts the indicator relative to the icon in units scaled by its layout."},
    {"blurReservedSpace", "Adds the reserved strip's backdrop for a detached, persistent Dock that reserves work area."},
    {"maximizedWindow", "Defines the Dock appearance override for a maximized application window."},
    {"appWindow", "Defines the Dock appearance override for the normal application-window scene."},
});

template<>
inline constexpr auto Fields<DockAppearanceRule> = std::to_array<Description>({
    {"enabled", "Enables this Dock appearance override for the corresponding window scene."},
    {"appearancePreset", "Selects the Dock material for the scene. -1 uses the shared theme and the custom preset uses appearance."},
    {"appearance", "Stores the independent surface material used for this scene's custom appearance."},
});

template<>
inline constexpr auto Fields<SystemTaskbarDynamicRule> = std::to_array<Description>({
    {"enabled", "Enables the taskbar backdrop override for this window or shell scene."},
    {"themeMode", "Selects this scene's material. 0 native, 1 global, 2 dark, 3 light, 4 dark glass, 5 light glass, 6 dark acrylic, 7 light acrylic, 8 custom, and 9 transparent."},
    {"contentTheme", "Sets the foreground contrast for this override. -1 leaves the preset's foreground theme in effect."},
    {"appearance", "Supplies independent material values when this rule selects the custom theme."},
});

template<>
inline constexpr auto Fields<DockFullscreenException> = std::to_array<Description>({
    {"executable", "Identifies the executable whose fullscreen protection this exception overrides."},
    {"policy", "Applies 0 allow, 1 block gestures or 2 full protection to a matching application."},
});

template<>
inline constexpr auto Fields<DockTrayRule> = std::to_array<Description>({
    {"executable", "Matches the tray-owning executable for a Dock tray action rule."},
    {"matchPath", "Matches the full executable path when enabled. Otherwise the filename is used."},
    {"enabled", "Activates this executable-specific Dock tray behavior."},
    {"doubleClick", "Uses the tray double-click action for this mapping instead of the ordinary tray click action."},
});

template<>
inline constexpr auto Fields<NavigationSettings> = std::to_array<Description>({
    {"enabled", "Enables the configured Quick Navigation keyboard shortcut."},
    {"modifiers", "Sets the Windows modifier mask for the Quick Navigation shortcut."},
    {"virtualKey", "Sets the Windows virtual key for the Quick Navigation shortcut."},
    {"desktopViewMode", "Selects how the Quick Navigation desktop tab presents entries. 0 is tiles, 1 source, 2 initials, and 3 frequency."},
    {"lastCollapsed", "Remembers the last collapsed state of the Quick Navigation interface."},
    {"layout", "Controls the Quick Navigation panel and result layout in logical units."},
    {"prefixes", "Sets the trigger tokens for application, file, web, settings, Run and calculator queries in that order."},
    {"defaultEngine", "Selects the default search engine by stable engine ID. The ID must exist in engines."},
    {"engines", "Defines the named web query engines, their prefixes and URL templates."},
    {"colors", "Overrides individual Quick Navigation color roles. Omitted roles follow the selected theme. Use #RRGGBB or #RRGGBBAA with alpha last."},
    {"mappingViewMode", "Selects presentation for mapped folders. 0 is tiles, 1 source order, and 2 initials."},
    {"widgetSortByInitial", "Stores per-component Quick Navigation alphabetical ordering using stable component IDs. Missing or false uses the original order."},
    {"appSuggestionsEnabled", "Shows eligible application suggestions in Quick Navigation."},
    {"favoritesEnabled", "Enables the Quick Navigation favorites section."},
    {"favorites", "Stores ordered Quick Navigation shortcuts to files, folders, desktop items and applications."},
});

template<>
inline constexpr auto Fields<QuickNavigationLayout> = std::to_array<Description>({
    {"expandedWidth", "Sets the preferred panel width in expanded Quick Navigation mode before window constraints."},
    {"collapsedWidth", "Sets the preferred panel width in collapsed Quick Navigation mode before window constraints."},
    {"maximumHeight", "Caps the Quick Navigation panel height. The monitor work area can impose a smaller cap."},
    {"visibleRows", "Limits the number of visible content rows before scrolling."},
    {"padding", "Sets inset between the Quick Navigation panel edge and its content."},
    {"searchHeight", "Sets the height of the search input area."},
    {"iconSize", "Sets the preferred Quick Navigation tile icon size independently of ordinary desktop icon size."},
    {"gridGap", "Sets spacing between Quick Navigation tile columns."},
    {"rowGap", "Sets spacing between applicable Quick Navigation rows."},
    {"fontSize", "Sets the primary result label font size."},
    {"secondaryFontSize", "Sets the metadata or secondary result text size."},
    {"searchFontSize", "Sets the Quick Navigation search input font size."},
    {"resultRowHeight", "Sets the preferred height of a search result row before layout constraints."},
    {"labelLines", "Sets how many lines Quick Navigation tile labels may use."},
    {"cornerRadius", "Sets the main panel corner radius."},
    {"searchRadius", "Sets the search field corner radius."},
    {"tabRadius", "Sets the radius of the tab background."},
    {"itemRadius", "Sets the selectable result or tile background corner radius."},
});

template<>
inline constexpr auto Fields<QuickNavigationSearchEngine> = std::to_array<Description>({
    {"id", "Stable engine identifier referenced by defaultEngine."},
    {"name", "Label shown for this search engine in the UI."},
    {"prefix", "Token that selects the engine in the Quick Navigation search input."},
    {"url", "Search URL template. The query substitution marker is {query}."},
});

template<>
inline constexpr auto Fields<QuickNavigationFavorite> = std::to_array<Description>({
    {"type", "Determines whether the favorite opens a file, folder, desktop item, or application."},
    {"target", "Stores the path or parsing name according to the favorite type."},
    {"desktopKey", "Preserves the stable layout identity for a favorite created from a desktop item."},
    {"name", "Sets the label shown for this favorite."},
});

template<>
inline constexpr auto Fields<app_fonts::Selection> = std::to_array<Description>({
    {"package", "Chooses the installed or imported font package. Read fonts for available selections. The font takes effect after restart."},
    {"family", "Chooses a family within the font package. The font takes effect after restart."},
});

template<>
inline constexpr auto Fields<calendar::DisplayPreferences> = std::to_array<Description>({
    {"enabled", "Enables alternate calendar display in supported calendar surfaces."},
    {"calendar", "Selects the calendar system by its advertised ID."},
    {"holidaysEnabled", "Retired display preference retained for saved-data compatibility."},
    {"region", "Retired display preference retained for saved-data compatibility."},
});


template<>
inline constexpr auto Fields<StatusBarSettings> = std::to_array<Description>({
    {"enabled", "Shows the SnowDesktop status bar on the selected monitor scope. Its merged layout depends on the active desktop style and Dock."},
    {"position", "Places the standalone status bar at the bottom with 0 or top with 1. Merged positioning follows the combined bar."},
    {"monitorScope", "Chooses first, last or all eligible monitors with values 0, 1 or 2."},
    {"scale", "Scales standalone status bar presentation. The merged bar height comes from dock.mergedBarHeight."},
    {"theme", "Selects the base status bar appearance when no enabled scene override applies."},
    {"clock", "Shows the status bar clock and its click target."},
    {"tray", "Enables the status bar tray presentation. Which icons can be pinned depends on the tray runtime and visible shell entries."},
    {"network", "Includes the network indicator in the control-center display."},
    {"volume", "Includes the volume indicator in the control-center display."},
    {"battery", "Includes the battery indicator when battery information is available."},
    {"controlCenter", "Shows the control-center entry point and its configured indicators."},
    {"cpu", "Displays sampled CPU utilization when the counter is available."},
    {"memory", "Displays used physical memory as a share of total memory when memory data is available."},
    {"gpu", "Displays the highest available utilization across sampled GPU adapters."},
    {"traffic", "Displays sampled download and upload rates when available."},
    {"audioControls", "Includes audio controls in the built-in control-center panel."},
    {"brightnessControls", "Includes supported display brightness controls in the built-in control-center panel."},
    {"wifiControls", "Includes Wi-Fi controls in the built-in control-center panel."},
    {"bluetoothControls", "Includes Bluetooth controls in the built-in control-center panel."},
    {"mediaControls", "Includes media session controls in the built-in control-center panel."},
    {"powerControls", "Includes power controls in the built-in control-center panel."},
    {"pinnedTrayItems", "Selects eligible tray entries by persistent ID for direct display."},
    {"trayOrder", "Orders known tray entries by persistent ID."},
    {"menu", "Shows the system menu button."},
    {"quickSearch", "Shows the Quick Navigation button."},
    {"taskView", "Shows a button that opens Windows Task View."},
    {"clockSystemPanel", "Uses the Windows panel for the clock click behavior instead of the built-in calendar panel."},
    {"controlCenterSystemPanel", "Uses the Windows panel for the control-center click behavior instead of the built-in panel."},
    {"inputMethodSystemPanel", "Uses the Windows panel for the input-method click behavior."},
    {"leftOrder", "Stores the configured ordering of leading status bar items."},
    {"rightOrder", "Stores the configured ordering for trailing status bar items."},
    {"legacyShellUi", "Retired scene retained to round-trip existing saved settings."},
    {"maximizedWindow", "Overrides the bar's appearance for an eligible maximized window when enabled."},
    {"legacyVisibleWindow", "Retired scene retained to round-trip existing saved settings."},
    {"noWindow", "Retired scene retained to round-trip existing saved settings."},
    {"inputMethod", "Shows the input method indicator when available."},
    {"quickControlOrder", "Selects and orders quick controls by identifier. Omitted controls stay hidden."},
    {"hiddenQuickControls", "Stores quick control IDs explicitly hidden from the panel."},
    {"clockShowSeconds", "Includes seconds in the clock text when the clock is shown."},
    {"timeNotificationRightAligned", "Aligns time and notifications to the right in the standalone bar. Merged layout and centered app names can force right alignment."},
    {"batteryPercentage", "Adds battery percentage text to the control-center indicator when valid battery data is present."},
    {"showDesktop", "Shows the desktop control for the status bar."},
    {"controlCenterSingleIcon", "Uses one control-center glyph instead of the group of indicators where applicable."},
    {"notifications", "Shows the notification entry and uses the available quiet or pending state for its glyph."},
    {"showOnlyWhenSummoned", "Keeps an eligible independent status bar hidden without reserving screen space. A touch swipe inward from its screen edge can reveal it. A merged bar follows the Dock."},
    {"edgeRevealGesture", "Chooses swipe with 0 or hover with 1 for an eligible independent status bar reveal."},
    {"foregroundAppName", "Displays the name of the active application when available."},
    {"foregroundAppNameCentered", "Centers the foreground application name. The clock and notification layout adjusts around it."},
    {"appWindow", "Overrides the bar appearance for the eligible non-maximized application-window scene when enabled."},
    {"metricOrder", "Orders the CPU, memory, GPU and traffic counters. Only enabled counters appear."},
});

template<>
inline constexpr auto Fields<IconBeautifySettings> = std::to_array<Description>({
    {"enabled", "Enables the icon beautification pipeline for the applicable scope. Individual icon configuration can inherit or override the global setting."},
    {"preset", "Selects the icon treatment preset. 0 is none, 1 default beautify, 5 custom, 6 frosted glass, 7 dark frosted glass and 8 light frosted glass."},
    {"mode", "Selects subject treatment. 0 detects the icon structure, 1 adds a theme plate and shrinks the subject, and 2 adds a theme plate without shrinking for an individual icon."},
    {"backgroundOpacity", "Sets opacity of the generated background plate from 0 to 1."},
    {"glassEnabled", "Enables the glass treatment on the icon plate where the resolved preset uses it."},
    {"glassBlurRadius", "Sets the glass plate blur radius when that treatment is active."},
    {"edgeHighlightEnabled", "Enables the light reflection on the generated icon plate edge."},
    {"edgeHighlightWidth", "Sets the width of the enabled light reflection."},
    {"edgeHighlightStrength", "Sets the intensity of the enabled plate light reflection."},
    {"edgeLight", "Sets the directional lighting and glow on an enabled icon plate edge."},
    {"gradientEnabled", "Uses a gradient for the legacy icon plate colors. An enabled backgroundGradient supplies an explicit gradient."},
    {"gradientDirection", "Selects the axis of the legacy gradient. The explicit backgroundGradient supplies its own angle."},
    {"backgroundStartR", "Sets the red channel of the first legacy plate color."},
    {"backgroundStartG", "Sets the green channel of the first legacy plate color."},
    {"backgroundStartB", "Sets the blue channel of the first legacy plate color."},
    {"backgroundEndR", "Sets the red channel of the second legacy plate color when the legacy gradient is enabled."},
    {"backgroundEndG", "Sets the green channel of the second legacy plate color when the legacy gradient is enabled."},
    {"backgroundEndB", "Sets the blue channel of the second legacy plate color when the legacy gradient is enabled."},
    {"shape", "Selects the plate mask. 0 legacy rounded, 1 continuous rounded, 2 circle, 3 soft rounded and 10 pebble."},
    {"contentScale", "Sets the subject size relative to the plate when the mode scales the subject. It leaves the outer desktop icon size to desktop.itemIconSizeScale and the page metrics."},
    {"textureHighlightStrength", "Sets the strength of the texture highlight within the generated plate."},
    {"textureHighlightSize", "Sets the coverage of the generated texture highlight within the plate."},
    {"textureHighlightAngle", "Shifts the direction of the generated texture highlight."},
    {"textureShadeStrength", "Sets the strength of the generated plate shading."},
    {"textureEdgeHighlight", "Sets the texture's own edge highlight independently of the directional plate light."},
    {"filterEnabled", "Enables the color filter on the rendered icon subject."},
    {"filterStrength", "Sets how strongly the chosen color filter is blended into the subject."},
    {"filterTintR", "Sets the red channel of the subject filter tint."},
    {"filterTintG", "Sets the green channel of the subject filter tint."},
    {"filterTintB", "Sets the blue channel of the subject filter tint."},
    {"outlineEnabled", "Draws an independent outline around the generated icon plate."},
    {"outlineWidth", "Sets the enabled plate outline thickness."},
    {"outlineOpacity", "Sets alpha of the enabled plate outline from 0 to 1."},
    {"outlineR", "Sets the red channel of the enabled plate outline."},
    {"outlineG", "Sets the green channel of the enabled plate outline."},
    {"outlineB", "Sets the blue channel of the enabled plate outline."},
    {"shadowStrength", "Sets the shadow built into this icon treatment. Surface drop shadows are configured separately in desktop.iconShadows."},
    {"backgroundSource", "Selects the plate color source. 0 uses custom colors, 1 detects the original plate where possible, and 2 derives color from the icon accent."},
    {"backgroundGradient", "Supplies explicit plate color stops, opacity and angle when enabled."},
    {"filterMode", "Selects the subject color filter. 0 uses multiply, 1 preserves luminosity, and 2 maps luminosity to two tones."},
});

template<>
inline constexpr auto Fields<EdgeLightSettings> = std::to_array<Description>({
    {"direction", "Rotates the primary light direction around the surface edge."},
    {"spread", "Sets the angular span of the directional edge lighting."},
    {"feather", "Softens the boundary between directional edge light regions."},
    {"ambient", "Sets the base reflection contributed around the edge."},
    {"primary", "Sets strength of the main directional reflection."},
    {"opposite", "Sets strength of the reflection on the opposite side."},
    {"minimumWidth", "Sets the smallest relative edge-light band width."},
    {"widthVariation", "Controls how much the edge-light width varies around the contour."},
    {"innerGlow", "Sets the inner halo extent in the edge light."},
    {"outerGlow", "Sets the outer halo extent in the edge light."},
    {"glowStrength", "Sets how strongly the halo contributes to the edge light."},
    {"glowFalloff", "Controls attenuation of the edge-light halo."},
    {"glowThreshold", "Sets the threshold used to produce the halo."},
    {"shadowStrength", "Sets the shadow intensity around the highlighted edge."},
    {"shadowFalloff", "Controls attenuation of the edge shadow."},
});

template<>
inline constexpr auto Fields<runtime_modules::Policy> = std::to_array<Description>({
    {"allowed", "Specifies whether the taskbar, tray, window minimize, Start menu and wallpaper preview runtime modules are allowed, in that order. Disabling a module can disable dependent features. Fullscreen windows without the WS_MINIMIZEBOX style use native Windows minimize and restore even when the window minimize module is allowed."},
});

template<>
inline constexpr auto Fields<shell_extensions::Preferences> = std::to_array<Description>({
    {"enabled", "Legacy extension selector state preserved for compatibility. Active visibility comes from the scoped rules."},
    {"selections", "Legacy provider selections preserved for compatibility."},
    {"hidden", "Legacy hidden-item list preserved for compatibility."},
    {"shown", "Legacy shown-item list preserved for compatibility."},
    {"rulesVersion", "Version of the stored rule format."},
    {"rules", "Sets extension command visibility for object or background menus by stable command ID."},
    {"overrides", "Sets extension command visibility for specific invocation locations."},
    {"showAll", "Expands the commands of the current object or context and disables the granular visibility filtering while enabled."},
});

template<>
inline constexpr auto Fields<shell_extensions::Selection> = std::to_array<Description>({
    {"provider", "Legacy provider ID for this selection."},
    {"command", "Legacy command ID. Empty selects the provider group."},
    {"label", "Legacy display label for the selection. It is not used for command matching."},
    {"placement", "Legacy command placement in the menu."},
});

template<>
inline constexpr auto Fields<shell_extensions::HiddenItem> = std::to_array<Description>({
    {"id", "Legacy stable shell command ID."},
    {"context", "Legacy location of this entry."},
});

template<>
inline constexpr auto Fields<shell_extensions::Rule> = std::to_array<Description>({
    {"id", "Identifies the shell extension command to display or hide."},
    {"category", "Selects the object menu with 0 or the background menu with 1."},
    {"shown", "Selects whether the command is shown for the category."},
});

template<>
inline constexpr auto Fields<shell_extensions::LocationOverride> = std::to_array<Description>({
    {"id", "Identifies the shell extension command to override."},
    {"context", "Chooses the menu location for this override."},
    {"visibility", "Use 0 to inherit the broader rule, 1 to show, or 2 to hide the command."},
});
}
