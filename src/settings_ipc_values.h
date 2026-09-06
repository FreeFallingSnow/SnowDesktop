#pragma once

#include "settings_ipc_codec.h"
#include "settings_controller.h"
#include "widget_settings_service.h"
#include "page_layout_settings.h"
#include "settings_search_index.h"

// Private same-executable wire schema. Keep field lists explicit: no
// runtime pointers, object layout, native padding or persistence APIs.
namespace snowdesktop::settings_ipc
{
#define SD_IPC_FIELDS(Type, ...) \
    template<> struct Fields<Type> { \
        template<class Value> static auto Tie(Value& v) { return std::tie(__VA_ARGS__); } \
    }

SD_IPC_FIELDS(PersonalizationSettings,
    v.widgetBgR, v.widgetBgG, v.widgetBgB, v.widgetBorderR,
    v.widgetBorderG, v.widgetBorderB, v.widgetAlpha, v.widgetBorderAlpha,
    v.widgetBorderWidth, v.widgetEdgeHighlightEnabled, v.widgetEdgeHighlightWidth, v.widgetEdgeHighlightStrength,
    v.gradientEndA, v.barHeight, v.categorizedTabHeight, v.luaWidgetContentRowHeight,
    v.showCategoryTabCounts, v.backgroundPreset, v.cornerRadius, v.contextMenuStyle,
    v.glassEnabled, v.glassBlurRadius, v.acrylicEnabled, v.contentTheme);
SD_IPC_FIELDS(SystemTaskbarDynamicRule,
    v.enabled, v.themeMode, v.contentTheme, v.appearance);
SD_IPC_FIELDS(DockSettings,
    v.position, v.edgeAttached, v.floatingShortcutMode, v.floatingHotkeyModifiers,
    v.floatingHotkeyVirtualKey, v.floatingEdgeSwipeEnabled, v.monitorScope, v.showWindowsButton,
    v.showRunningApps, v.showWindowPreviews, v.showFrequentItems, v.keepWhenDesktopHidden,
    v.allowDesktopContentOverlap, v.showOnlyWhenSummoned, v.frequentItemCount, v.thicknessScale,
    v.systemTaskbarAutoHide, v.systemTaskbarAlignment, v.systemTaskbarBackdropEnabled, v.systemTaskbarFollowPersonalization,
    v.systemTaskbarContentTheme, v.systemTaskbarAppearance, v.systemTaskbarVisibleWindow, v.systemTaskbarMaximizedWindow,
    v.systemTaskbarShellUi);
SD_IPC_FIELDS(NavigationSettings,
    v.enabled, v.modifiers, v.virtualKey, v.desktopViewMode);
SD_IPC_FIELDS(GeneralSettings,
    v.autoStartEnabled, v.softwareDesktopEnabled, v.demoModeEnabled, v.doubleClickHideDesktop,
    v.desktopPassthroughHotkeyEnabled, v.desktopPassthroughHotkeyModifiers, v.desktopPassthroughHotkeyVirtualKey, v.pageNavigationKeyboardEnabled,
    v.pageNavigationPreviousModifiers, v.pageNavigationPreviousVirtualKey, v.pageNavigationNextModifiers, v.pageNavigationNextVirtualKey,
    v.quickNavTheme, v.collectionPopupTheme, v.dockEnabled, v.widgetDeveloperToolsEnabled,
    v.language);
SD_IPC_FIELDS(CategoryRule,
    v.id, v.customLabel, v.extensions);
SD_IPC_FIELDS(CategorySettings,
    v.tabFontSize, v.rules);
SD_IPC_FIELDS(IconBeautifySettings,
    v.enabled, v.preset, v.mode, v.backgroundOpacity,
    v.gradientEnabled, v.gradientDirection, v.backgroundStartR, v.backgroundStartG,
    v.backgroundStartB, v.backgroundEndR, v.backgroundEndG, v.backgroundEndB,
    v.shape, v.contentScale, v.textureHighlightStrength, v.textureHighlightSize,
    v.textureHighlightAngle, v.textureShadeStrength, v.textureEdgeHighlight, v.filterEnabled,
    v.filterStrength, v.filterTintR, v.filterTintG, v.filterTintB,
    v.outlineEnabled, v.outlineWidth, v.outlineOpacity, v.outlineR,
    v.outlineG, v.outlineB, v.shadowStrength);
SD_IPC_FIELDS(DesktopDisplaySettings,
    v.dockEnabled, v.iconSpacingScale, v.itemIconSizeScale, v.itemFontSizeCu,
    v.listItemFontSizeCu, v.itemFontWeight, v.shortcutArrowMode, v.iconBeautify);
SD_IPC_FIELDS(SettingsRoute,
    v.page, v.widgetInstanceId, v.focusId);
SD_IPC_FIELDS(SettingsActionResult,
    v.status, v.completedDomains, v.failedDomains, v.message);
SD_IPC_FIELDS(SettingsValues,
    v.personalization, v.dock, v.navigation, v.general,
    v.category, v.desktop);
SD_IPC_FIELDS(SettingsDomainRevisions,
    v.personalization, v.dock, v.systemTaskbar, v.navigation,
    v.general, v.category, v.desktop);
SD_IPC_FIELDS(SettingsSnapshot,
    v.revision, v.generation, v.initialized, v.sessionActive,
    v.route, v.values, v.domainRevisions, v.dirtyDomains,
    v.pendingPreviewDomains, v.pendingCommitDomains, v.retryRequired, v.externalReplacementPending,
    v.lastActionMessage);
SD_IPC_FIELDS(SettingsHostActions::Request,
    v.action, v.widgetInstanceId, v.value, v.boolValue,
    v.hotkeyTarget, v.modifiers, v.virtualKey);
SD_IPC_FIELDS(PageLayoutEntry,
    v.id, v.columns, v.rows, v.itemCount,
    v.widgetCount, v.role, v.monitorOrdinal, v.visible,
    v.activeOnLastMonitor);
SD_IPC_FIELDS(PageLayoutSnapshot,
    v.revision, v.monitorCount, v.pages);
SD_IPC_FIELDS(PageGridChangeImpact,
    v.valid, v.pageId, v.previousColumns, v.previousRows,
    v.columns, v.rows, v.displacedItemCount, v.displacedWidgetCount,
    v.resizedWidgetCount);
SD_IPC_FIELDS(PageLayoutOperationResult,
    v.status, v.message, v.snapshot);
SD_IPC_FIELDS(StaticSettingSearchDescriptor,
    v.page, v.focusId, v.label, v.description,
    v.context, v.keywords, v.visible);
SD_IPC_FIELDS(WidgetSettingSearchFieldDescriptor,
    v.key, v.focusId, v.label, v.description,
    v.groupLabel, v.visible);
SD_IPC_FIELDS(WidgetSettingsSearchDescriptor,
    v.instanceId, v.widgetName, v.fields, v.installed,
    v.visible);
SD_IPC_FIELDS(SettingsSearchIndexInput,
    v.languageTag, v.staticSettings, v.widgets, v.developerToolsVisible,
    v.debugVisible);
SD_IPC_FIELDS(widget_runtime::InteractionValue,
    v.type, v.boolean, v.integer, v.number,
    v.string, v.array, v.object);
SD_IPC_FIELDS(widget_runtime::WidgetSettingCondition,
    v.key, v.operation, v.values);
SD_IPC_FIELDS(widget_runtime::WidgetSettingOption,
    v.value, v.label);
SD_IPC_FIELDS(widget_runtime::WidgetSettingFieldSchema,
    v.key, v.label, v.description, v.group,
    v.validationMessage, v.rawType, v.searchKey, v.binding,
    v.access, v.emptyLabel, v.noResultsLabel, v.minimum,
    v.maximum, v.step, v.minimumLength, v.maximumLength,
    v.required, v.showWhen, v.enabledWhen, v.dependsOn,
    v.options, v.extensions);
SD_IPC_FIELDS(widget_runtime::WidgetSettingGroupSchema,
    v.id, v.label, v.description, v.collapsible,
    v.defaultExpanded);
SD_IPC_FIELDS(widget_runtime::WidgetSettingPresetSchema,
    v.id, v.label, v.values, v.isDefault,
    v.hostAppearanceValues);
SD_IPC_FIELDS(widget_runtime::WidgetHostAppearanceState,
    v.followPersonalization, v.presetId, v.backgroundColor, v.borderColor,
    v.backgroundOpacity, v.borderOpacity, v.borderWidth, v.edgeHighlightEnabled,
    v.edgeHighlightWidth, v.edgeHighlightStrength, v.gradientEndOpacity, v.glassEnabled,
    v.acrylicEnabled, v.contentTheme);
SD_IPC_FIELDS(widget_runtime::WidgetHostAppearancePatch,
    v.followPersonalization, v.presetId, v.backgroundColor, v.borderColor,
    v.backgroundOpacity, v.borderOpacity, v.borderWidth, v.edgeHighlightEnabled,
    v.edgeHighlightWidth, v.edgeHighlightStrength, v.gradientEndOpacity, v.glassEnabled,
    v.acrylicEnabled, v.contentTheme, v.clearContentTheme);
SD_IPC_FIELDS(widget_runtime::WidgetSettingOpaqueState,
    v.configured, v.available, v.canChoose, v.canClear,
    v.displayLabel);
SD_IPC_FIELDS(widget_runtime::WidgetSettingFieldState,
    v.schema, v.currentValue, v.defaultValue, v.hasStoredValue,
    v.visible, v.enabled, v.valid, v.validationError,
    v.diagnosticCode, v.searchQuery, v.opaque);
SD_IPC_FIELDS(widget_runtime::WidgetSettingsSnapshot,
    v.widgetId, v.packageId, v.widgetName, v.generation,
    v.revision, v.preview, v.customStyle, v.fields,
    v.groups, v.presets, v.defaultPresetId, v.hostAppearance);
SD_IPC_FIELDS(widget_runtime::WidgetSettingMutationGuard,
    v.widgetId, v.generation, v.revision);
SD_IPC_FIELDS(widget_runtime::WidgetSettingMutationResult,
    v.status, v.generation, v.revision, v.errorCode,
    v.message);
SD_IPC_FIELDS(widget_runtime::WidgetSettingSearchResult,
    v.id, v.title, v.source, v.type);
SD_IPC_FIELDS(widget_runtime::WidgetSettingSearchSnapshot,
    v.widgetId, v.settingKey, v.generation, v.requestId,
    v.query, v.pending, v.completed, v.errorCode,
    v.results);
SD_IPC_FIELDS(widget_runtime::WidgetSettingsLoadResult,
    v.status, v.snapshot, v.errorCode, v.message);
SD_IPC_FIELDS(widget_runtime::WidgetSettingsSnapshotChanged,
    v.widgetId, v.generation, v.revision);
SD_IPC_FIELDS(widget_runtime::WidgetSettingSearchCompleted,
    v.widgetId, v.settingKey, v.generation, v.requestId);

#undef SD_IPC_FIELDS
} // namespace snowdesktop::settings_ipc
