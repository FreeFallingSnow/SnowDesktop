#pragma once

#include "common/json_value.h"
#include "system/calendar/calendar_service.h"
#include "widget/runtime/widget_host_requests.h"
#include "widget_audio_analysis_provider.h"
#include "widget_system_data_provider.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

enum class LuaWidgetPreviewDataState
{
    Ready,
    Empty,
    Loading,
    Error,
    Stale,
    PermissionDenied,
};

struct LuaWidgetDataSnapshot
{
    struct FilesystemWatchEvent
    {
        std::string kind;
        std::string name;
        std::string oldName;
        std::string handle;
        std::string itemKind;
    };

    std::string topic;
    bool available = false;
    bool stale = true;
    bool warmingUp = false;
    std::int64_t timestampMs = 0;
    std::string error;
    snowdesktop::widget_runtime::WidgetCpuDataSnapshot cpu;
    snowdesktop::widget_runtime::WidgetMemoryDataSnapshot memory;
    snowdesktop::widget_runtime::WidgetProcessSummaryDataSnapshot
        processSummary;
    snowdesktop::widget_runtime::WidgetPowerDataSnapshot power;
    snowdesktop::widget_runtime::WidgetScreenTimeDataSnapshot screenTime;
    snowdesktop::widget_runtime::WidgetNetworkStatusDataSnapshot
        networkStatus;
    snowdesktop::widget_runtime::WidgetNetworkTrafficDataSnapshot
        networkTraffic;
    snowdesktop::widget_runtime::WidgetGpuDataSnapshot gpu;
    snowdesktop::widget_runtime::WidgetStorageVolumesDataSnapshot
        storageVolumes;
    snowdesktop::widget_runtime::WidgetStorageIoDataSnapshot storageIo;
    snowdesktop::widget_runtime::WidgetDisplayTopologyDataSnapshot
        displayTopology;
    snowdesktop::widget_runtime::WidgetDisplayDataSnapshot displayCurrent;
    snowdesktop::widget_runtime::WidgetAudioOutputDefaultDataSnapshot
        audioOutputDefault;
    snowdesktop::widget_runtime::WidgetAudioOutputVolumeDataSnapshot
        audioOutputVolume;
    JsonValue systemControl;
    snowdesktop::widget_runtime::WidgetAudioAnalysisDataSnapshot
        audioAnalysis;
    snowdesktop::widget_runtime::WidgetMediaSessionsDataSnapshot
        mediaSessions;
    snowdesktop::widget_runtime::WidgetMediaCurrentDataSnapshot
        mediaCurrent;
    snowdesktop::widget_runtime::WidgetMediaTimelineDataSnapshot
        mediaTimeline;
    snowdesktop::widget_runtime::WidgetMediaArtworkDataSnapshot
        mediaArtwork;
    std::vector<LuaDesktopItemInfo> desktopItems;
    std::uint64_t desktopRevision = 0;
    std::string desktopChangeReason;
    std::vector<snowdesktop::calendar::CalendarEvent> calendarEvents;
    std::string calendarSelectedDate;
    std::string calendarRangeStart;
    std::string calendarRangeEnd;
    std::uint64_t calendarRevision = 0;
    bool calendarTruncated = false;
    std::optional<std::uint64_t> calendarTotal;
    bool calendarHasMore = false;
    std::uint64_t calendarNextOffset = 0;
    std::string appIndexState;
    std::uint64_t appIndexRevision = 0;
    std::vector<FilesystemWatchEvent> filesystemWatchEvents;
    std::uint64_t filesystemWatchRevision = 0;
    bool filesystemWatchOverflow = false;
};

namespace snowdesktop::widget_runtime
{
struct DataSubscriptionSnapshot;

using WidgetDesktopDataReader = std::function<std::vector<LuaDesktopItemInfo>(bool selection)>;
using WidgetDisplayBoundsReader = std::function<std::optional<WidgetDisplayPixelRectDataSnapshot>()>;

LuaWidgetDataSnapshot BuildPreviewDataSnapshot(const DataSubscriptionSnapshot& binding,
    LuaWidgetPreviewDataState previewState, const WidgetDesktopDataReader& readDesktop);

bool PopulatePlatformDataSnapshot(LuaWidgetDataSnapshot& result,
    const DataSubscriptionSnapshot& binding, const WidgetSystemDataProvider& system,
    const WidgetAudioAnalysisProvider* audio, bool includeGpuDetails,
    std::int64_t timestampNow, const WidgetDisplayBoundsReader& readDisplayBounds);
}
