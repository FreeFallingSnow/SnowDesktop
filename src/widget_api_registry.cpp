#include "widget_api_registry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

extern "C" {
#include <lauxlib.h>
}

namespace snowdesktop::widget_api
{
namespace
{
constexpr std::uint32_t kCurrentApiVersion = 2;
constexpr std::array<std::string_view, 201> kHostFeatures = {
    "animation.frame",
    "calendar.dateMath",
    "calendar.selection",
    "control.blur",
    "control.focus",
    "control.textArea",
    "control.textInput",
    "data.app.indexStatus",
    "data.audio.output.analysis",
    "data.audio.output.default",
    "data.audio.output.volume",
    "data.calendar.events",
    "data.calendar.selectedDate",
    "data.desktop.changes",
    "data.desktop.items",
    "data.desktop.selection",
    "data.filesystem.watch",
    "data.subscribe",
    "data.media.artwork",
    "data.media.current",
    "data.media.sessions",
    "data.media.timeline",
    "data.process.summary",
    "data.system.cpu",
    "data.system.display.topology",
    "data.system.display.current",
    "data.system.gpu",
    "data.system.memory",
    "data.system.network.status",
    "data.system.network.traffic",
    "data.system.power",
    "data.system.storage.volumes",
    "data.system.storage.io",
    "draw.advanced",
    "draw.immediate",
    "draw.marqueeText",
    "interaction.accessibility.metadata",
    "interaction.contextMenu",
    "interaction.contextMenu.resourceImage",
    "interaction.contextMenu.submenu",
    "interaction.keyboard",
    "interaction.pointerActions",
    "interaction.pointerCapture",
    "interaction.region",
    "interaction.scroll",
    "interaction.scroll.orientation",
    "interaction.tooltip",
    "interaction.tooltip.rich",
    "lifecycle.event",
    "lifecycle.model",
    "l10n.basic",
    "l10n.format",
    "layout.relativeUnits",
    "module.package",
    "resource.package",
    "schedule.absolute",
    "schedule.basic",
    "schedule.timeline",
    "schedule.visibility",
    "settings.appReference",
    "settings.appSearch",
    "settings.date",
    "settings.dependencies",
    "settings.description",
    "settings.desktopItemReference",
    "settings.fileHandle",
    "settings.fileReference",
    "settings.folderHandle",
    "settings.folderReference",
    "settings.groups",
    "settings.multiSelect",
    "settings.range",
    "settings.secretReference",
    "settings.select.localizedOptions",
    "settings.showWhen",
    "settings.time",
    "settings.url",
    "settings.validation",
    "slots.model",
    "slots.mutation.userGesture",
    "slots.nativeDrop",
    "slots.keyboardNavigation",
    "slots.pointerReorder",
    "slots.pointerTransfer",
    "slots.nativeContextMenu",
    "slots.event.changed",
    "slots.history",
    "slots.hostHistory",
    "slots.hostPicker",
    "state.transient",
    "storage.typed",
    "storage.transaction",
    "storage.writeBudget",
    "system.environment",
    "system.uptime",
    "task.media.control",
    "task.desktop.refresh",
    "task.desktop.search",
    "task.everything.search",
    "task.network.request",
    "task.network.headers",
    "task.network.requestBody",
    "task.network.secretReference",
    "task.notification.actions",
    "task.notification.lifecycle",
    "task.notification.schedule",
    "task.notification.show",
    "task.notification.structured",
    "task.app.launch",
    "task.app.search",
    "task.audio.output.control",
    "task.calendar.write",
    "task.clipboard.fileReference",
    "task.clipboard.image",
    "task.clipboard.text",
    "task.filesystem.picker",
    "task.filesystem.access",
    "task.filesystem.binary",
    "task.start",
    "task.system.openSettings",
    "task.shell.openUri",
    "task.shell.item",
    "time.basic",
    "time.calendar",
    "time.previewClock",
    "widget.context",
    "widget.dialog",
    "widget.panel",
    "widget.popover",
    "view.actionControls",
    "view.accessibility.metadata",
    "view.checkbox.indeterminate",
    "view.collection.basic",
    "view.collection.contentStates",
    "view.collection.orientation",
    "view.collection.selection",
    "view.collection.stickyHeaders",
    "view.collection.virtual",
    "view.collection.virtual.orientation",
    "view.collection.virtual.stickyHeaders",
    "view.collection.virtual.variableExtent",
    "view.dataSeries",
    "view.flex.layout",
    "view.flex.sizing",
    "view.focus.request",
    "view.flow.wrap",
    "view.font",
    "view.grid.placement",
    "view.grid.tracks",
    "view.grid.uniform",
    "view.identity.diagnostics",
    "view.image",
    "view.image.tint",
    "view.inputControls",
    "view.input.required",
    "view.input.selection",
    "view.keyboard.accessKey",
    "view.keyboard.events",
    "view.keyboardNavigation.basic",
    "view.keyboardNavigation.order",
    "view.layout.constraints",
    "view.layout.edgeInsets",
    "view.layout.overflow",
    "view.logicalSlots",
    "view.logicalSlots.dropStyle",
    "view.logicalSlots.emptyContent",
    "view.monthCalendar",
    "view.pointer.events",
    "view.positioning.basic",
    "view.progress.indeterminate",
    "view.referenceIcon",
    "view.scroll",
    "view.scroll.events",
    "view.scroll.initialTarget",
    "view.scroll.programmatic",
    "view.selectionControls",
    "view.shadow",
    "view.state.busy",
    "view.state.selected",
    "view.state.visibility",
    "view.statusVisuals",
    "view.styledText.actions",
    "view.styledText.basic",
    "view.styledText.inlineIcons",
    "view.surface.dialog",
    "view.surface.panel",
    "view.surface.popover",
    "view.text.flow",
    "view.text.locale",
    "view.text.typography",
    "view.theme.tokens",
    "view.tooltip",
    "view.tooltip.rich",
    "view.transform.affine",
    "view.transform.basic",
    "view.transition.enter",
    "view.transition.exit",
    "view.transition.layout",
    "view.transition.transform",
    "view.transition.visual",
    "view.tree.core",
};
using FunctionParameter = SystemFunctionParameterContract;
constexpr std::array<FunctionParameter, 0> kNoFunctionParameters{};
constexpr auto kCapabilityParameters = std::to_array<FunctionParameter>({
    { "featureOrApi", "string", true },
});
constexpr auto kTimePartsParameters = std::to_array<FunctionParameter>({
    { "epochMilliseconds", "integer", true },
    { "timeZone", "'local'|'utc'|string", true },
});
constexpr auto kTimeFormatParameters = std::to_array<FunctionParameter>({
    { "epochMilliseconds", "integer", true },
    { "options", "SnowTimeFormatOptions", true },
});
constexpr auto kTimeAddParameters = std::to_array<FunctionParameter>({
    { "epochMilliseconds", "integer", false },
    { "delta", "SnowTimeDelta", false },
    { "options", "SnowTimeZoneOptions", true },
});
constexpr auto kTimeCompareParameters = std::to_array<FunctionParameter>({
    { "left", "integer", false },
    { "right", "integer", false },
});
constexpr auto kFormatNumberParameters = std::to_array<FunctionParameter>({
    { "value", "number", false },
    { "options", "SnowNumberFormatOptions", true },
});
constexpr auto kFormatBytesParameters = std::to_array<FunctionParameter>({
    { "bytes", "number", false },
    { "options", "SnowBytesFormatOptions", true },
});
constexpr auto kFormatDurationParameters = std::to_array<FunctionParameter>({
    { "milliseconds", "integer", false },
    { "options", "SnowDurationFormatOptions", true },
});
constexpr auto kFormatRelativeTimeParameters =
    std::to_array<FunctionParameter>({
        { "deltaMilliseconds", "integer", false },
        { "options", "SnowRelativeTimeFormatOptions", true },
    });
constexpr auto kFormatListParameters = std::to_array<FunctionParameter>({
    { "values", "string[]", false },
    { "options", "SnowLocaleOptions", true },
});

constexpr std::array<SystemFunctionContract, 15>
kSystemFunctionContracts = {{
    { "widget.context", "widget.context", kNoFunctionParameters,
        "SnowWidgetContext" },
    { "system.capabilities", "system.environment", kCapabilityParameters,
        "SnowCapabilities|SnowCapability" },
    { "system.info", "system.environment", kNoFunctionParameters,
        "SnowSystemInfo" },
    { "system.uptime", "system.uptime", kNoFunctionParameters,
        "SnowSystemUptime" },
    { "time.now", "time.basic", kNoFunctionParameters, "integer" },
    { "time.monotonic", "time.basic", kNoFunctionParameters, "integer" },
    { "time.parts", "time.calendar", kTimePartsParameters,
        "SnowDateTimeParts" },
    { "time.format", "time.calendar", kTimeFormatParameters, "string" },
    { "time.add", "time.calendar", kTimeAddParameters, "integer" },
    { "time.compare", "time.calendar", kTimeCompareParameters, "-1|0|1" },
    { "l10n.formatNumber", "l10n.format", kFormatNumberParameters,
        "string" },
    { "l10n.formatBytes", "l10n.format", kFormatBytesParameters, "string" },
    { "l10n.formatDuration", "l10n.format", kFormatDurationParameters,
        "string" },
    { "l10n.formatRelativeTime", "l10n.format",
        kFormatRelativeTimeParameters, "string" },
    { "l10n.formatList", "l10n.format", kFormatListParameters, "string" },
}};
constexpr std::array<SystemDataTopicContract, 25>
kSystemDataTopicContracts = {{
    { "system.cpu", "data.system.cpu", "system.performance.read",
        500, 5000, 2000, false, false, "SnowDataSubscribeOptions",
        "SnowCpuDataValue" },
    { "system.memory", "data.system.memory", "system.performance.read",
        1000, 5000, 2000, false, false, "SnowDataSubscribeOptions",
        "SnowMemoryDataValue" },
    { "process.summary", "data.process.summary", "process.summary.read",
        1000, 10000, 0, true, false, "SnowDataSubscribeOptions",
        "SnowProcessSummaryDataValue" },
    { "system.gpu", "data.system.gpu", "system.performance.read",
        1000, 5000, 2000, false, false, "SnowDataSubscribeOptions",
        "SnowGpuDataValue" },
    { "system.power", "data.system.power", "system.power.read",
        2000, 10000, 2000, false, false, "SnowDataSubscribeOptions",
        "SnowPowerDataValue" },
    { "system.storage.volumes", "data.system.storage.volumes",
        "system.storage.read", 2000, 10000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowStorageVolumesDataValue" },
    { "system.storage.io", "data.system.storage.io",
        "system.storage.read", 1000, 5000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowStorageIoDataValue" },
    { "system.network.status", "data.system.network.status",
        "system.network.read", 2000, 10000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowNetworkStatusDataValue" },
    { "system.network.traffic", "data.system.network.traffic",
        "system.network.read", 1000, 5000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowNetworkTrafficDataValue" },
    { "system.display.current", "data.system.display.current",
        "system.display.read", 2000, 10000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowDisplayCurrentDataValue" },
    { "system.display.topology", "data.system.display.topology",
        "system.display.read", 2000, 10000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowDisplayTopologyDataValue" },
    { "media.sessions", "data.media.sessions", "media.read",
        500, 2000, 2000, false, false, "SnowDataSubscribeOptions",
        "SnowMediaSessionsDataValue" },
    { "media.current", "data.media.current", "media.read",
        500, 2000, 2000, false, false, "SnowDataSubscribeOptions",
        "SnowMediaCurrentDataValue" },
    { "media.timeline", "data.media.timeline", "media.read",
        500, 2000, 2000, false, false, "SnowDataSubscribeOptions",
        "SnowMediaTimelineDataValue" },
    { "media.artwork", "data.media.artwork", "media.read",
        500, 2000, 0, false, false, "SnowDataSubscribeOptions",
        "SnowMediaArtworkDataValue" },
    { "audio.output.default", "data.audio.output.default",
        "audio.output.read", 1000, 5000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowAudioOutputDefaultDataValue" },
    { "audio.output.volume", "data.audio.output.volume",
        "audio.output.read", 1000, 5000, 2000, false, false,
        "SnowDataSubscribeOptions", "SnowAudioOutputVolumeDataValue" },
    { "audio.output.analysis", "data.audio.output.analysis",
        "audio.output.analyze", 16, 1000, 0, true, false,
        "SnowAudioAnalysisSubscribeOptions",
        "SnowAudioOutputAnalysisDataValue" },
    { "desktop.items", "data.desktop.items", "desktop.read",
        100, 1000, 0, false, true, "SnowDataSubscribeOptions",
        "SnowDesktopItemsDataValue" },
    { "desktop.selection", "data.desktop.selection", "desktop.read",
        100, 1000, 0, false, true, "SnowDataSubscribeOptions",
        "SnowDesktopSelectionDataValue" },
    { "desktop.changes", "data.desktop.changes", "desktop.read",
        100, 1000, 0, false, true, "SnowDataSubscribeOptions",
        "SnowDesktopChangesDataValue" },
    { "app.indexStatus", "data.app.indexStatus", "app.discovery",
        100, 1000, 0, false, true, "SnowDataSubscribeOptions",
        "SnowAppIndexStatusDataValue" },
    { "calendar.events", "data.calendar.events", "calendar.read",
        100, 1000, 0, false, true,
        "SnowCalendarEventsSubscribeOptions", "SnowCalendarEventsDataValue" },
    { "calendar.selectedDate", "data.calendar.selectedDate",
        "calendar.read", 100, 1000, 0, false, true,
        "SnowDataSubscribeOptions", "SnowCalendarSelectedDateDataValue" },
    { "filesystem.watch", "data.filesystem.watch",
        "filesystem.userSelected.watch", 100, 1000, 0, true, false,
        "SnowFilesystemWatchSubscribeOptions",
        "SnowFilesystemWatchDataValue" },
}};
constexpr std::array<SystemTaskContract, 41> kSystemTaskContracts = {{
    { "network.request", "task.network.request", "network.internet",
        false, 2, "SnowNetworkRequestArguments", "SnowNetworkTaskValue" },
    { "notification.show", "task.notification.show", "notification.post",
        false, 2, "SnowNotificationShowArguments",
        "SnowNotificationTaskValue" },
    { "notification.update", "task.notification.lifecycle",
        "notification.post", false, 2, "SnowNotificationUpdateArguments",
        "SnowAcceptedTaskValue" },
    { "notification.dismiss", "task.notification.lifecycle",
        "notification.post", false, 2, "SnowNotificationReferenceArguments",
        "SnowAcceptedTaskValue" },
    { "notification.schedule", "task.notification.schedule",
        "notification.post", false, 2, "SnowNotificationScheduleArguments",
        "SnowNotificationTaskValue" },
    { "notification.cancel", "task.notification.schedule",
        "notification.post", false, 2, "SnowNotificationReferenceArguments",
        "SnowAcceptedTaskValue" },
    { "clipboard.read", "task.clipboard.text", "clipboard.read", true, 1,
        "SnowClipboardReadArguments", "SnowClipboardReadTaskValue" },
    { "clipboard.write", "task.clipboard.text", "clipboard.write", true, 1,
        "SnowClipboardWriteArguments", "SnowAcceptedTaskValue" },
    { "clipboard.clear", "task.clipboard.text", "clipboard.write", true, 1,
        nullptr, "SnowAcceptedTaskValue" },
    { "filesystem.pickOpen", "task.filesystem.picker",
        "filesystem.userSelected.read", true, 1,
        "SnowFilesystemPickOpenArguments", "SnowFilesystemPickerTaskValue" },
    { "filesystem.pickSave", "task.filesystem.picker",
        "filesystem.userSelected.write", true, 1,
        "SnowFilesystemPickSaveArguments", "SnowFilesystemPickerTaskValue" },
    { "filesystem.pickFolder", "task.filesystem.picker", "", true, 1,
        "SnowFilesystemPickFolderArguments", "SnowFilesystemPickerTaskValue" },
    { "filesystem.stat", "task.filesystem.access",
        "filesystem.userSelected.read", false, 4,
        "SnowFilesystemHandleArguments", "SnowFilesystemMetadata" },
    { "filesystem.list", "task.filesystem.access",
        "filesystem.userSelected.read", false, 2,
        "SnowFilesystemListArguments", "SnowFilesystemListTaskValue" },
    { "filesystem.read", "task.filesystem.access",
        "filesystem.userSelected.read", false, 2,
        "SnowFilesystemReadArguments", "SnowFilesystemReadTaskValue" },
    { "filesystem.write", "task.filesystem.access",
        "filesystem.userSelected.write", false, 1,
        "SnowFilesystemWriteArguments", "SnowFilesystemWriteTaskValue" },
    { "filesystem.release", "task.filesystem.access", "", false, 4,
        "SnowFilesystemHandleArguments", "SnowAcceptedTaskValue" },
    { "desktop.search", "task.desktop.search", "desktop.read", false, 2,
        "SnowItemSearchArguments", "SnowItemSearchTaskValue" },
    { "app.search", "task.app.search", "app.discovery", false, 2,
        "SnowAppSearchArguments", "SnowAppSearchTaskValue" },
    { "everything.search", "task.everything.search", "everything.search",
        false, 1, "SnowItemSearchArguments", "SnowItemSearchTaskValue" },
    { "shell.openUri", "task.shell.openUri", "shell.launch", true, 1,
        "SnowShellOpenUriArguments", "SnowAcceptedTaskValue" },
    { "shell.openItem", "task.shell.item", "desktop.action", true, 1,
        "SnowItemReferenceArguments", "SnowAcceptedTaskValue" },
    { "shell.revealItem", "task.shell.item", "desktop.action", true, 1,
        "SnowItemReferenceArguments", "SnowAcceptedTaskValue" },
    { "desktop.refresh", "task.desktop.refresh", "desktop.action", true, 1,
        nullptr, "SnowAcceptedTaskValue" },
    { "app.launch", "task.app.launch", "app.launch", true, 1,
        "SnowAppLaunchArguments", "SnowAcceptedTaskValue" },
    { "media.play", "task.media.control", "media.action", true, 1,
        "SnowMediaSessionArguments", "SnowMediaTaskValue" },
    { "media.pause", "task.media.control", "media.action", true, 1,
        "SnowMediaSessionArguments", "SnowMediaTaskValue" },
    { "media.toggle", "task.media.control", "media.action", true, 1,
        "SnowMediaSessionArguments", "SnowMediaTaskValue" },
    { "media.stop", "task.media.control", "media.action", true, 1,
        "SnowMediaSessionArguments", "SnowMediaTaskValue" },
    { "media.next", "task.media.control", "media.action", true, 1,
        "SnowMediaSessionArguments", "SnowMediaTaskValue" },
    { "media.previous", "task.media.control", "media.action", true, 1,
        "SnowMediaSessionArguments", "SnowMediaTaskValue" },
    { "media.seek", "task.media.control", "media.action", true, 1,
        "SnowMediaSeekArguments", "SnowMediaTaskValue" },
    { "media.setRate", "task.media.control", "media.action", true, 1,
        "SnowMediaRateArguments", "SnowMediaTaskValue" },
    { "media.setShuffle", "task.media.control", "media.action", true, 1,
        "SnowMediaShuffleArguments", "SnowMediaTaskValue" },
    { "media.setRepeat", "task.media.control", "media.action", true, 1,
        "SnowMediaRepeatArguments", "SnowMediaTaskValue" },
    { "audio.output.setVolume", "task.audio.output.control",
        "audio.output.control", true, 1, "SnowAudioOutputVolumeArguments",
        "SnowAudioOutputTaskValue" },
    { "audio.output.setMute", "task.audio.output.control",
        "audio.output.control", true, 1, "SnowAudioOutputMuteArguments",
        "SnowAudioOutputTaskValue" },
    { "calendar.create", "task.calendar.write", "calendar.write", false, 1,
        "SnowCalendarEventArguments", "SnowCalendarMutationTaskValue" },
    { "calendar.update", "task.calendar.write", "calendar.write", false, 1,
        "SnowCalendarUpdateArguments", "SnowCalendarMutationTaskValue" },
    { "calendar.remove", "task.calendar.write", "calendar.write", true, 1,
        "SnowCalendarRemoveArguments", "SnowCalendarMutationTaskValue" },
    { "system.openSettings", "task.system.openSettings", "shell.launch",
        true, 1, "SnowSystemSettingsArguments",
        "SnowSystemSettingsTaskValue" },
}};
constexpr std::array<std::string_view, 24> kV2SandboxLibraries = {
    "string", "table", "math", "utf8", "draw", "layout", "storage",
    "state", "schedule", "animation", "widget", "system", "time", "module",
    "resource", "data", "task", "interaction", "control", "calendar",
    "ui", "l10n", "view", "slots",
};

constexpr auto kPublicApiFunctionContracts =
    std::to_array<PublicApiFunctionContract>({
#define SNOW_WIDGET_PUBLIC_FUNCTION(                                      \
    library, name, callback, sinceApi, permission, untilApi)              \
    { library, name, sinceApi, permission, untilApi },
#define SNOW_WIDGET_MANIFEST_FUNCTION SNOW_WIDGET_PUBLIC_FUNCTION
#include "widget_public_api.inc"
#undef SNOW_WIDGET_MANIFEST_FUNCTION
#undef SNOW_WIDGET_PUBLIC_FUNCTION
    });
char kDefinedWidgetMarker = 0;
char kTransientStateTableKey = 0;
char kTransientStateDirtyKey = 0;

constexpr std::size_t kMaxTransientStateKeys = 256;
constexpr std::size_t kMaxTransientStateKeyBytes = 128;
constexpr std::size_t kMaxTransientStateDepth = 16;
constexpr std::size_t kMaxTransientStateNodes = 4096;
constexpr std::size_t kMaxTransientStateStringBytes = 64 * 1024;
constexpr std::size_t kMaxTransientStateValueBytes = 256 * 1024;

struct StateCopyBudget
{
    std::size_t nodes = 0;
    std::size_t stringBytes = 0;
    std::unordered_set<const void*> ancestors;
};

void PushTransientStateTable(lua_State* state)
{
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kTransientStateTableKey);
    if (lua_istable(state, -1)) return;
    lua_pop(state, 1);
    lua_newtable(state);
    lua_pushvalue(state, -1);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kTransientStateTableKey);
}

void MarkTransientStateDirty(lua_State* state)
{
    lua_pushboolean(state, 1);
    lua_rawsetp(state, LUA_REGISTRYINDEX, &kTransientStateDirtyKey);
}

std::size_t TableEntryCount(lua_State* state, int index)
{
    index = lua_absindex(state, index);
    std::size_t count = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        ++count;
        lua_pop(state, 1);
    }
    return count;
}

bool PushStateValueCopy(lua_State* state, int index,
    std::size_t depth, StateCopyBudget& budget, std::string& error)
{
    const int entryTop = lua_gettop(state);
    index = lua_absindex(state, index);
    if (++budget.nodes > kMaxTransientStateNodes)
    {
        error = "value exceeds the 4096-node limit";
        return false;
    }

    switch (lua_type(state, index))
    {
    case LUA_TNIL:
        lua_pushnil(state);
        return true;
    case LUA_TBOOLEAN:
        lua_pushboolean(state, lua_toboolean(state, index));
        return true;
    case LUA_TNUMBER:
    {
        const lua_Number value = lua_tonumber(state, index);
        if (!std::isfinite(static_cast<double>(value)))
        {
            error = "numbers must be finite";
            return false;
        }
        if (lua_isinteger(state, index))
            lua_pushinteger(state, lua_tointeger(state, index));
        else
            lua_pushnumber(state, value);
        return true;
    }
    case LUA_TSTRING:
    {
        std::size_t length = 0;
        const char* value = lua_tolstring(state, index, &length);
        if (length > kMaxTransientStateStringBytes ||
            budget.stringBytes >
                kMaxTransientStateValueBytes - length)
        {
            error = "strings exceed the transient state value limit";
            return false;
        }
        budget.stringBytes += length;
        lua_pushlstring(state, value, length);
        return true;
    }
    case LUA_TTABLE:
        break;
    default:
        error = "values must be nil, boolean, finite number, string, array, or object";
        return false;
    }

    if (depth >= kMaxTransientStateDepth)
    {
        error = "value exceeds the 16-level depth limit";
        return false;
    }
    if (lua_getmetatable(state, index) != 0)
    {
        lua_pop(state, 1);
        error = "tables with metatables are not allowed";
        return false;
    }
    const void* identity = lua_topointer(state, index);
    if (!budget.ancestors.insert(identity).second)
    {
        error = "cyclic tables are not allowed";
        return false;
    }

    bool hasIntegerKeys = false;
    bool hasStringKeys = false;
    std::size_t entryCount = 0;
    std::size_t maximumIndex = 0;
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        ++entryCount;
        if (lua_isinteger(state, -2))
        {
            const lua_Integer key = lua_tointeger(state, -2);
            if (key <= 0 ||
                static_cast<std::uint64_t>(key) >
                    kMaxTransientStateNodes)
            {
                error = "array keys must be contiguous positive integers";
                lua_settop(state, entryTop);
                budget.ancestors.erase(identity);
                return false;
            }
            hasIntegerKeys = true;
            maximumIndex = std::max(maximumIndex,
                static_cast<std::size_t>(key));
        }
        else if (lua_type(state, -2) == LUA_TSTRING)
        {
            std::size_t keyLength = 0;
            lua_tolstring(state, -2, &keyLength);
            if (keyLength == 0 || keyLength > kMaxTransientStateKeyBytes)
            {
                error = "object keys must contain 1 to 128 bytes";
                lua_settop(state, entryTop);
                budget.ancestors.erase(identity);
                return false;
            }
            hasStringKeys = true;
        }
        else
        {
            error = "table keys must be strings or positive integers";
            lua_settop(state, entryTop);
            budget.ancestors.erase(identity);
            return false;
        }
        lua_pop(state, 1);
    }
    if ((hasIntegerKeys && hasStringKeys) ||
        (hasIntegerKeys && maximumIndex != entryCount))
    {
        error = hasIntegerKeys && hasStringKeys
            ? "tables cannot mix array and object keys"
            : "array keys must be contiguous positive integers";
        budget.ancestors.erase(identity);
        return false;
    }

    lua_createtable(state,
        hasIntegerKeys ? static_cast<int>(entryCount) : 0,
        hasStringKeys ? static_cast<int>(entryCount) : 0);
    const int copy = lua_absindex(state, -1);
    lua_pushnil(state);
    while (lua_next(state, index) != 0)
    {
        const int value = lua_absindex(state, -1);
        lua_pushvalue(state, -2);
        if (!PushStateValueCopy(
                state, value, depth + 1, budget, error))
        {
            lua_settop(state, entryTop);
            budget.ancestors.erase(identity);
            return false;
        }
        lua_rawset(state, copy);
        lua_pop(state, 1);
    }
    budget.ancestors.erase(identity);
    return true;
}

bool StateValuesEqual(lua_State* state, int left, int right,
    std::size_t depth = 0)
{
    left = lua_absindex(state, left);
    right = lua_absindex(state, right);
    if (lua_rawequal(state, left, right)) return true;
    const int leftType = lua_type(state, left);
    const int rightType = lua_type(state, right);
    if (leftType != rightType &&
        !(leftType == LUA_TNUMBER && rightType == LUA_TNUMBER))
        return false;
    switch (leftType)
    {
    case LUA_TNIL:
        return true;
    case LUA_TBOOLEAN:
        return lua_toboolean(state, left) == lua_toboolean(state, right);
    case LUA_TNUMBER:
        return lua_tonumber(state, left) == lua_tonumber(state, right);
    case LUA_TSTRING:
    {
        std::size_t leftLength = 0;
        std::size_t rightLength = 0;
        const char* leftValue = lua_tolstring(state, left, &leftLength);
        const char* rightValue = lua_tolstring(state, right, &rightLength);
        return leftLength == rightLength &&
            std::memcmp(leftValue, rightValue, leftLength) == 0;
    }
    case LUA_TTABLE:
        break;
    default:
        return false;
    }
    if (depth >= kMaxTransientStateDepth ||
        TableEntryCount(state, left) != TableEntryCount(state, right))
        return false;

    lua_pushnil(state);
    while (lua_next(state, left) != 0)
    {
        lua_pushvalue(state, -2);
        lua_rawget(state, right);
        const bool equal = StateValuesEqual(
            state, -2, -1, depth + 1);
        lua_pop(state, 2);
        if (!equal)
        {
            lua_pop(state, 1);
            return false;
        }
    }
    return true;
}

std::string CheckedTransientStateKey(lua_State* state, int index)
{
    std::size_t length = 0;
    const char* value = luaL_checklstring(state, index, &length);
    if (length == 0 || length > kMaxTransientStateKeyBytes)
        luaL_argerror(state, index, "key must contain 1 to 128 bytes");
    return std::string(value, length);
}

bool FieldIsNilOrFunction(lua_State* state, int tableIndex,
    const char* field)
{
    lua_getfield(state, tableIndex, field);
    const bool valid = lua_isnil(state, -1) || lua_isfunction(state, -1);
    lua_pop(state, 1);
    return valid;
}

bool HasNonNilField(lua_State* state, int tableIndex, const char* field)
{
    lua_getfield(state, tableIndex, field);
    const bool present = !lua_isnil(state, -1);
    lua_pop(state, 1);
    return present;
}

bool HasGrantedPermission(lua_State* state, const char* permission)
{
    if (!permission || permission[0] == '\0') return true;
    lua_getfield(state, LUA_REGISTRYINDEX, "__widget_permissions");
    if (!lua_istable(state, -1))
    {
        lua_pop(state, 1);
        return false;
    }
    lua_getfield(state, -1, permission);
    const bool granted = lua_toboolean(state, -1) != 0;
    lua_pop(state, 2);
    return granted;
}

const char* PreviewName(SystemCapabilityPreview preview) noexcept
{
    return preview == SystemCapabilityPreview::Deterministic
        ? "deterministic" : "noSideEffects";
}

void PushCapabilityBase(lua_State* state, const char* name,
    const char* feature, const char* kind, const char* permission,
    bool requiresTrustedGesture, SystemCapabilityPreview preview)
{
    const bool hostAvailable = feature && SupportsFeature(feature);
    const bool authorized = HasGrantedPermission(state, permission);
    const bool available = hostAvailable && authorized;
    lua_createtable(state, 0, 10);
    lua_pushstring(state, name); lua_setfield(state, -2, "id");
    lua_pushstring(state, name); lua_setfield(state, -2, "name");
    lua_pushstring(state, feature); lua_setfield(state, -2, "feature");
    lua_pushstring(state, kind); lua_setfield(state, -2, "kind");
    lua_pushboolean(state, hostAvailable); lua_setfield(state, -2,
        "hostAvailable");
    lua_pushboolean(state, authorized); lua_setfield(state, -2,
        "authorized");
    lua_pushboolean(state, available); lua_setfield(state, -2, "available");
    lua_pushinteger(state, 1); lua_setfield(state, -2, "version");
    lua_pushboolean(state, requiresTrustedGesture);
    lua_setfield(state, -2, "requiresTrustedGesture");
    lua_pushstring(state, PreviewName(preview));
    lua_setfield(state, -2, "preview");
    if (permission && permission[0] != '\0')
    {
        lua_pushstring(state, permission);
        lua_setfield(state, -2, "permission");
    }
    if (!hostAvailable)
    {
        lua_pushliteral(state, "unsupported");
        lua_setfield(state, -2, "reason");
    }
    else if (!authorized)
    {
        lua_pushliteral(state, "permissionRequired");
        lua_setfield(state, -2, "reason");
    }
}

void PushCapability(lua_State* state,
    const SystemFunctionContract& contract)
{
    PushCapabilityBase(state, contract.name, contract.feature, "function",
        nullptr, false, SystemCapabilityPreview::Deterministic);
    lua_createtable(state, static_cast<int>(contract.parameters.size()), 0);
    int index = 1;
    for (const auto& parameter : contract.parameters)
    {
        lua_createtable(state, 0, 3);
        lua_pushstring(state, parameter.name);
        lua_setfield(state, -2, "name");
        lua_pushstring(state, parameter.type);
        lua_setfield(state, -2, "type");
        lua_pushboolean(state, parameter.optional);
        lua_setfield(state, -2, "optional");
        lua_rawseti(state, -2, index++);
    }
    lua_setfield(state, -2, "parameters");
    lua_pushstring(state, contract.resultType);
    lua_setfield(state, -2, "resultType");
}

void PushCapability(lua_State* state,
    const SystemDataTopicContract& contract)
{
    PushCapabilityBase(state, contract.name, contract.feature, "data",
        contract.requiredPermission, false, contract.preview);
    lua_pushinteger(state, contract.minimumIntervalMs);
    lua_setfield(state, -2, "minimumIntervalMs");
    lua_pushinteger(state, contract.hiddenIntervalMs);
    lua_setfield(state, -2, "hiddenIntervalMs");
    lua_pushstring(state, contract.optionsType);
    lua_setfield(state, -2, "optionsType");
    lua_pushstring(state, contract.valueType);
    lua_setfield(state, -2, "valueType");
}

void PushCapability(lua_State* state, const SystemTaskContract& contract)
{
    PushCapabilityBase(state, contract.name, contract.feature, "task",
        contract.requiredPermission, contract.requiresTrustedGesture,
        contract.preview);
    lua_pushinteger(state, static_cast<lua_Integer>(
        contract.maximumPerInstance));
    lua_setfield(state, -2, "maximumPerInstance");
    if (contract.argumentsType)
    {
        lua_pushstring(state, contract.argumentsType);
        lua_setfield(state, -2, "argumentsType");
    }
    lua_pushstring(state, contract.resultType);
    lua_setfield(state, -2, "resultType");
}
}

std::span<const std::string_view> HostFeatures() noexcept
{
    return kHostFeatures;
}

bool SupportsFeature(std::string_view feature) noexcept
{
    return std::find(kHostFeatures.begin(), kHostFeatures.end(), feature) !=
        kHostFeatures.end();
}

std::span<const SystemFunctionContract>
SystemFunctionContracts() noexcept
{
    return kSystemFunctionContracts;
}

std::span<const SystemDataTopicContract>
SystemDataTopicContracts() noexcept
{
    return kSystemDataTopicContracts;
}

std::span<const SystemTaskContract> SystemTaskContracts() noexcept
{
    return kSystemTaskContracts;
}

std::span<const std::string_view> SandboxLibraries() noexcept
{
    return kV2SandboxLibraries;
}

std::span<const PublicApiFunctionContract>
PublicApiFunctionContracts() noexcept
{
    return kPublicApiFunctionContracts;
}

std::vector<std::string> MissingFeatures(
    std::span<const std::string> requiredFeatures)
{
    std::vector<std::string> result;
    for (const auto& feature : requiredFeatures)
    {
        if (!SupportsFeature(feature) &&
            std::find(result.begin(), result.end(), feature) == result.end())
            result.push_back(feature);
    }
    return result;
}

int LuaDefineWidget(lua_State* state)
{
    luaL_checktype(state, 1, LUA_TTABLE);
    const int descriptor = lua_absindex(state, 1);
    for (const char* callback : {
        "render", "view", "setup", "event", "dispose", "menu",
        "panel", "dialog", "popover" })
    {
        if (!FieldIsNilOrFunction(state, descriptor, callback))
        {
            return luaL_error(state,
                "widget.define: '%s' must be a function when present",
                callback);
        }
    }

    const bool hasRender = HasNonNilField(state, descriptor, "render");
    const bool hasView = HasNonNilField(state, descriptor, "view");
    if (hasRender == hasView)
    {
        return luaL_error(state,
            "widget.define: choose exactly one of 'render' or 'view'");
    }
    if (hasView && !SupportsFeature("view.tree") &&
        !SupportsFeature("view.tree.core"))
    {
        return luaL_error(state,
            "widget.define: unsupported host feature 'view.tree'");
    }
    lua_pushlightuserdata(state, &kDefinedWidgetMarker);
    lua_pushboolean(state, 1);
    lua_rawset(state, descriptor);
    lua_settop(state, 1);
    return 1;
}

int LuaApiInfo(lua_State* state)
{
    lua_createtable(state, 0, 3);
    lua_pushinteger(state, kCurrentApiVersion);
    lua_setfield(state, -2, "current");
    lua_createtable(state, 1, 0);
    lua_pushinteger(state, kCurrentApiVersion);
    lua_rawseti(state, -2, 1);
    lua_setfield(state, -2, "supported");
    lua_createtable(state, static_cast<int>(kHostFeatures.size()), 0);
    int index = 1;
    for (const auto feature : kHostFeatures)
    {
        lua_pushlstring(state, feature.data(), feature.size());
        lua_rawseti(state, -2, index++);
    }
    lua_setfield(state, -2, "features");
    return 1;
}

int LuaHasFeature(lua_State* state)
{
    std::size_t length = 0;
    const char* feature = luaL_checklstring(state, 1, &length);
    lua_pushboolean(state,
        SupportsFeature(std::string_view(feature, length)) ? 1 : 0);
    return 1;
}

int LuaSystemCapabilities(lua_State* state)
{
    if (!lua_isnoneornil(state, 1))
    {
        std::size_t length = 0;
        const char* value = luaL_checklstring(state, 1, &length);
        const std::string_view query(value, length);
        for (const auto& contract : kSystemFunctionContracts)
        {
            if (query == contract.name)
            {
                PushCapability(state, contract);
                return 1;
            }
        }
        for (const auto& contract : kSystemDataTopicContracts)
        {
            if (query == contract.name)
            {
                PushCapability(state, contract);
                return 1;
            }
        }
        for (const auto& contract : kSystemTaskContracts)
        {
            if (query == contract.name)
            {
                PushCapability(state, contract);
                return 1;
            }
        }

        const bool available = SupportsFeature(query);
        lua_createtable(state, 0, 5);
        lua_pushlstring(state, query.data(), query.size());
        lua_setfield(state, -2, "id");
        lua_pushboolean(state, available);
        lua_setfield(state, -2, "hostAvailable");
        lua_pushboolean(state, available);
        lua_setfield(state, -2, "available");
        if (available)
        {
            lua_pushinteger(state, 1);
            lua_setfield(state, -2, "version");
            lua_createtable(state, 0, 0);
            int apiIndex = 1;
            for (const auto& contract : kSystemFunctionContracts)
            {
                if (query != contract.feature) continue;
                PushCapability(state, contract);
                lua_rawseti(state, -2, apiIndex++);
            }
            for (const auto& contract : kSystemDataTopicContracts)
            {
                if (query != contract.feature) continue;
                PushCapability(state, contract);
                lua_rawseti(state, -2, apiIndex++);
            }
            for (const auto& contract : kSystemTaskContracts)
            {
                if (query != contract.feature) continue;
                PushCapability(state, contract);
                lua_rawseti(state, -2, apiIndex++);
            }
            lua_setfield(state, -2, "apis");
        }
        else
        {
            lua_pushliteral(state, "unsupported");
            lua_setfield(state, -2, "reason");
        }
        return 1;
    }

    lua_createtable(state, 0, 3);
    lua_pushinteger(state, kCurrentApiVersion);
    lua_setfield(state, -2, "apiVersion");
    lua_createtable(state, static_cast<int>(kHostFeatures.size()), 0);
    int index = 1;
    for (const auto feature : kHostFeatures)
    {
        lua_createtable(state, 0, 3);
        lua_pushlstring(state, feature.data(), feature.size());
        lua_setfield(state, -2, "id");
        lua_pushboolean(state, 1);
        lua_setfield(state, -2, "available");
        lua_pushinteger(state, 1);
        lua_setfield(state, -2, "version");
        lua_rawseti(state, -2, index++);
    }
    lua_setfield(state, -2, "features");
    lua_createtable(state, static_cast<int>(kSystemFunctionContracts.size() +
        kSystemDataTopicContracts.size() + kSystemTaskContracts.size()), 0);
    int capabilityIndex = 1;
    for (const auto& contract : kSystemFunctionContracts)
    {
        PushCapability(state, contract);
        lua_rawseti(state, -2, capabilityIndex++);
    }
    for (const auto& contract : kSystemDataTopicContracts)
    {
        PushCapability(state, contract);
        lua_rawseti(state, -2, capabilityIndex++);
    }
    for (const auto& contract : kSystemTaskContracts)
    {
        PushCapability(state, contract);
        lua_rawseti(state, -2, capabilityIndex++);
    }
    lua_setfield(state, -2, "capabilities");
    return 1;
}

bool IsDefinedWidget(lua_State* state, int index) noexcept
{
    if (!state || !lua_istable(state, index)) return false;
    index = lua_absindex(state, index);
    lua_pushlightuserdata(state, &kDefinedWidgetMarker);
    lua_rawget(state, index);
    const bool defined = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return defined;
}

int LuaTransientStateGet(lua_State* state)
{
    const std::string key = CheckedTransientStateKey(state, 1);
    PushTransientStateTable(state);
    const int values = lua_absindex(state, -1);
    lua_pushlstring(state, key.data(), key.size());
    lua_rawget(state, values);
    if (lua_isnil(state, -1) && !lua_isnone(state, 2))
    {
        lua_pop(state, 1);
        StateCopyBudget budget;
        std::string error;
        if (!PushStateValueCopy(state, 2, 0, budget, error))
            return luaL_error(state, "state.get: invalid default: %s",
                error.c_str());
        return 1;
    }

    const int value = lua_absindex(state, -1);
    StateCopyBudget budget;
    std::string error;
    if (!PushStateValueCopy(state, value, 0, budget, error))
        return luaL_error(state, "state.get: stored value is invalid: %s",
            error.c_str());
    return 1;
}

int LuaTransientStateSet(lua_State* state)
{
    const std::string key = CheckedTransientStateKey(state, 1);
    luaL_checkany(state, 2);
    StateCopyBudget budget;
    std::string error;
    if (!PushStateValueCopy(state, 2, 0, budget, error))
        return luaL_error(state, "state.set: %s", error.c_str());
    const int copy = lua_absindex(state, -1);

    PushTransientStateTable(state);
    const int values = lua_absindex(state, -1);
    lua_pushlstring(state, key.data(), key.size());
    lua_rawget(state, values);
    const bool exists = !lua_isnil(state, -1);
    const bool equal = StateValuesEqual(state, -1, copy);
    lua_pop(state, 1);
    if (equal)
    {
        lua_pushboolean(state, 0);
        return 1;
    }
    if (!exists && !lua_isnil(state, copy) &&
        TableEntryCount(state, values) >= kMaxTransientStateKeys)
    {
        return luaL_error(state,
            "state.set: per-instance key limit exceeded");
    }

    lua_pushlstring(state, key.data(), key.size());
    lua_pushvalue(state, copy);
    lua_rawset(state, values);
    MarkTransientStateDirty(state);
    lua_pushboolean(state, 1);
    return 1;
}

int LuaTransientStateRemove(lua_State* state)
{
    CheckedTransientStateKey(state, 1);
    lua_settop(state, 1);
    lua_pushnil(state);
    return LuaTransientStateSet(state);
}

int LuaTransientStateHas(lua_State* state)
{
    const std::string key = CheckedTransientStateKey(state, 1);
    PushTransientStateTable(state);
    lua_pushlstring(state, key.data(), key.size());
    lua_rawget(state, -2);
    lua_pushboolean(state, !lua_isnil(state, -1));
    return 1;
}

int LuaTransientStateKeys(lua_State* state)
{
    PushTransientStateTable(state);
    const int values = lua_absindex(state, -1);
    std::vector<std::string> keys;
    keys.reserve(TableEntryCount(state, values));
    lua_pushnil(state);
    while (lua_next(state, values) != 0)
    {
        std::size_t length = 0;
        const char* key = lua_tolstring(state, -2, &length);
        keys.emplace_back(key, length);
        lua_pop(state, 1);
    }
    std::sort(keys.begin(), keys.end());
    lua_createtable(state, static_cast<int>(keys.size()), 0);
    for (std::size_t index = 0; index < keys.size(); ++index)
    {
        lua_pushlstring(state, keys[index].data(), keys[index].size());
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

int LuaTransientStateClear(lua_State* state)
{
    PushTransientStateTable(state);
    const bool changed = TableEntryCount(state, -1) != 0;
    lua_pop(state, 1);
    if (changed)
    {
        lua_newtable(state);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &kTransientStateTableKey);
        MarkTransientStateDirty(state);
    }
    lua_pushboolean(state, changed ? 1 : 0);
    return 1;
}

bool ConsumeTransientStateDirty(lua_State* state) noexcept
{
    if (!state) return false;
    lua_rawgetp(state, LUA_REGISTRYINDEX, &kTransientStateDirtyKey);
    const bool dirty = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    if (dirty)
    {
        lua_pushboolean(state, 0);
        lua_rawsetp(state, LUA_REGISTRYINDEX, &kTransientStateDirtyKey);
    }
    return dirty;
}

LibraryValidationError ValidateLibrary(
    const char* libraryName,
    std::span<const FunctionDescriptor> functions) noexcept
{
    if (!libraryName || libraryName[0] == '\0')
        return LibraryValidationError::MissingLibraryName;

    for (std::size_t index = 0; index < functions.size(); ++index)
    {
        const FunctionDescriptor& function = functions[index];
        if (!function.name || function.name[0] == '\0')
            return LibraryValidationError::MissingFunctionName;
        if (!function.callback)
            return LibraryValidationError::MissingCallback;
        if (function.sinceApi == 0 ||
            (function.untilApi != 0 &&
                function.untilApi < function.sinceApi))
            return LibraryValidationError::InvalidApiVersion;
        if (function.requiredPermission &&
            function.requiredPermission[0] == '\0')
        {
            return LibraryValidationError::EmptyRequiredPermission;
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (std::string_view(functions[previous].name) ==
                function.name)
            {
                return LibraryValidationError::DuplicateFunctionName;
            }
        }
    }
    return LibraryValidationError::None;
}

const char* DescribeValidationError(
    LibraryValidationError error) noexcept
{
    switch (error)
    {
    case LibraryValidationError::None:
        return "none";
    case LibraryValidationError::MissingLibraryName:
        return "missing library name";
    case LibraryValidationError::MissingFunctionName:
        return "missing function name";
    case LibraryValidationError::MissingCallback:
        return "missing callback";
    case LibraryValidationError::InvalidApiVersion:
        return "invalid API version";
    case LibraryValidationError::EmptyRequiredPermission:
        return "empty required permission";
    case LibraryValidationError::DuplicateFunctionName:
        return "duplicate function name";
    }
    return "unknown validation error";
}

CatalogValidationResult ValidateCatalog(
    std::span<const LibraryDescriptor> libraries) noexcept
{
    for (std::size_t index = 0; index < libraries.size(); ++index)
    {
        const LibraryDescriptor& library = libraries[index];
        const LibraryValidationError libraryError =
            ValidateLibrary(library.name, library.functions);
        if (libraryError != LibraryValidationError::None)
        {
            return {
                CatalogValidationError::InvalidLibrary,
                index,
                libraryError,
            };
        }
        for (std::size_t previous = 0; previous < index; ++previous)
        {
            if (std::string_view(libraries[previous].name) ==
                library.name)
            {
                return {
                    CatalogValidationError::DuplicateLibraryName,
                    index,
                    LibraryValidationError::None,
                };
            }
        }
    }
    return {};
}

const char* DescribeValidationError(
    CatalogValidationError error) noexcept
{
    switch (error)
    {
    case CatalogValidationError::None:
        return "none";
    case CatalogValidationError::InvalidLibrary:
        return "invalid library";
    case CatalogValidationError::DuplicateLibraryName:
        return "duplicate library name";
    }
    return "unknown validation error";
}

const FunctionDescriptor* FindFunction(
    std::span<const LibraryDescriptor> libraries,
    std::string_view libraryName,
    std::string_view functionName) noexcept
{
    if (libraryName.empty() || functionName.empty())
        return nullptr;

    for (const LibraryDescriptor& library : libraries)
    {
        if (!library.name || std::string_view(library.name) != libraryName)
            continue;
        for (const FunctionDescriptor& function : library.functions)
        {
            if (function.name &&
                std::string_view(function.name) == functionName)
            {
                return &function;
            }
        }
        return nullptr;
    }
    return nullptr;
}

void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    std::span<const FunctionDescriptor> functions)
{
    RegisterLibrary(state, libraryName, functions,
        (std::numeric_limits<std::uint32_t>::max)());
}

void RegisterLibrary(
    lua_State* state,
    const char* libraryName,
    std::span<const FunctionDescriptor> functions,
    std::uint32_t apiVersion)
{
    if (!state)
        throw std::invalid_argument(
            "cannot register a widget API library on a null Lua state");

    const LibraryValidationError validation =
        ValidateLibrary(libraryName, functions);
    if (validation != LibraryValidationError::None)
    {
        throw std::invalid_argument(
            std::string("invalid widget API library '") +
            (libraryName ? libraryName : "") + "': " +
            DescribeValidationError(validation));
    }

    const int entryTop = lua_gettop(state);
    const bool allVersions = apiVersion ==
        (std::numeric_limits<std::uint32_t>::max)();
    const auto exposed = std::count_if(functions.begin(), functions.end(),
        [apiVersion, allVersions](const FunctionDescriptor& function) {
            return allVersions || (function.sinceApi <= apiVersion &&
                (function.untilApi == 0 ||
                    apiVersion <= function.untilApi));
        });
    lua_createtable(state, 0, static_cast<int>(exposed));
    for (const FunctionDescriptor& function : functions)
    {
        if (!allVersions && (function.sinceApi > apiVersion ||
            (function.untilApi != 0 &&
                apiVersion > function.untilApi)))
            continue;
        lua_pushcfunction(state, function.callback);
        lua_setfield(state, -2, function.name);
    }
    lua_setglobal(state, libraryName);

    if (lua_gettop(state) != entryTop)
    {
        lua_settop(state, entryTop);
        throw std::logic_error(
            "widget API registration did not preserve the Lua stack");
    }
}

void RegisterLibraries(
    lua_State* state,
    std::span<const LibraryDescriptor> libraries)
{
    RegisterLibraries(state, libraries,
        (std::numeric_limits<std::uint32_t>::max)());
}

void RegisterLibraries(
    lua_State* state,
    std::span<const LibraryDescriptor> libraries,
    std::uint32_t apiVersion)
{
    if (!state)
        throw std::invalid_argument(
            "cannot register a widget API catalog on a null Lua state");

    const CatalogValidationResult validation =
        ValidateCatalog(libraries);
    if (validation.error != CatalogValidationError::None)
    {
        const LibraryDescriptor& library =
            libraries[validation.libraryIndex];
        std::string message =
            std::string("invalid widget API catalog at library '") +
            (library.name ? library.name : "") + "': " +
            DescribeValidationError(validation.error);
        if (validation.libraryError != LibraryValidationError::None)
        {
            message += ": ";
            message += DescribeValidationError(validation.libraryError);
        }
        throw std::invalid_argument(message);
    }

    const int entryTop = lua_gettop(state);
    for (const LibraryDescriptor& library : libraries)
        RegisterLibrary(state, library.name, library.functions, apiVersion);

    if (lua_gettop(state) != entryTop)
    {
        lua_settop(state, entryTop);
        throw std::logic_error(
            "widget API catalog registration did not preserve the Lua stack");
    }
}

void RegisterFunctionCatalog(
    lua_State* state,
    std::span<const CatalogFunctionDescriptor> functions,
    std::uint32_t apiVersion)
{
    if (!state)
        throw std::invalid_argument(
            "cannot register a widget API catalog on a null Lua state");

    struct MutableLibrary
    {
        std::string name;
        std::vector<FunctionDescriptor> functions;
    };
    std::vector<MutableLibrary> libraries;
    for (const CatalogFunctionDescriptor& entry : functions)
    {
        if (!entry.library || entry.library[0] == '\0')
            throw std::invalid_argument(
                "invalid flattened widget API catalog: missing library name");
        auto library = std::find_if(libraries.begin(), libraries.end(),
            [&entry](const MutableLibrary& candidate) {
                return candidate.name == entry.library;
            });
        if (library == libraries.end())
        {
            libraries.push_back({ entry.library, {} });
            library = std::prev(libraries.end());
        }
        library->functions.push_back(entry.function);
    }

    // Validate the entire flattened source before publishing its first global
    // so a malformed shared catalog cannot leave a partially registered VM.
    for (const MutableLibrary& library : libraries)
    {
        const LibraryValidationError validation = ValidateLibrary(
            library.name.c_str(), library.functions);
        if (validation != LibraryValidationError::None)
        {
            throw std::invalid_argument(
                "invalid flattened widget API catalog at library '" +
                library.name + "': " + DescribeValidationError(validation));
        }
    }

    const int entryTop = lua_gettop(state);
    for (const MutableLibrary& library : libraries)
    {
        RegisterLibrary(state, library.name.c_str(), library.functions,
            apiVersion);
    }
    if (lua_gettop(state) != entryTop)
    {
        lua_settop(state, entryTop);
        throw std::logic_error(
            "flattened widget API catalog registration did not preserve the Lua stack");
    }
}
}
