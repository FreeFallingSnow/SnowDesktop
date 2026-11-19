#include "widget/runtime/widget_engine.h"
#include "widget/view/widget_render_host_context.h"
#include "widget/preview/widget_preview_context.h"
#include "common/utils.h"
#include "diagnostics/performance_trace.h"
#include "widget/storage/widget_storage_runtime.h"

#include <algorithm>
#include <utility>

using snowdesktop::widget_runtime::MutablePersistentStorage;
using snowdesktop::widget_runtime::PersistentStorage;
using snowdesktop::widget_runtime::SaveStorageFile;

static snowdesktop::widget_runtime::DiagnosticsLog g_widgetDiagnostics;

static bool EndsWithLastError(const std::string& key)
{
    constexpr const char* suffix = ".lastError";
    constexpr size_t suffixLen = 10;
    return key.size() >= suffixLen && key.compare(key.size() - suffixLen, suffixLen, suffix) == 0;
}

std::vector<WidgetErrorEntry> WidgetEngine::GetWidgetErrors() const
{
    std::vector<WidgetErrorEntry> result;
    for (const auto& kv : PersistentStorage())
    {
        if (!EndsWithLastError(kv.first))
            continue;
        result.push_back({ kv.first, kv.second });
    }
    std::sort(result.begin(), result.end(), [](const WidgetErrorEntry& a, const WidgetErrorEntry& b) {
        return a.key < b.key;
    });
    return result;
}

std::string WidgetEngine::GetSystemSnapshotError() const
{
    return systemSnapshotService_ ? systemSnapshotService_->GetLastError() : std::string{};
}

void WidgetEngine::ClearWidgetErrors()
{
    auto& storage = MutablePersistentStorage();
    for (auto it = storage.begin(); it != storage.end(); )
    {
        if (EndsWithLastError(it->first))
            it = storage.erase(it);
        else
            ++it;
    }
    SaveStorageFile();
}

void WidgetEngine::RuntimeRecordError(const std::wstring& widgetId, const std::string& message)
{
    std::string idUtf8 = WideToUtf8(widgetId);
    const int index = FindWidget(widgetId);
    bool openedCircuit = false;
    if (!previewOnly_ && !snowdesktop::widget_runtime::IsDryLoad() &&
        (index < 0 || !widgets_[index].preview))
    {
        MutablePersistentStorage()[idUtf8 + ".lastError"] = message;
        SaveStorageFile();
    }
    if (index >= 0)
    {
        auto& widget = widgets_[index];
        const bool wasOpen = widget.health.CircuitOpen();
        if (widget.health.RecordError())
            widget.valid = false;
        openedCircuit = !wasOpen && widget.health.CircuitOpen();
        const bool quotaExceeded = widget.quota &&
            (widget.quota->memoryExceeded ||
                widget.quota->executionExceeded);
        RecordWidgetHostFailure(widgetId, message, quotaExceeded,
            widget.health.CircuitOpen());
    }
    else
        RecordWidgetHostFailure(widgetId, message);
    RuntimeAddLog(widgetId, "error", message);
    if (openedCircuit)
        RuntimeInvalidateHost(widgetId);
}

void WidgetEngine::RuntimeRecordSuccess(const std::wstring& widgetId)
{
    const int index = FindWidget(widgetId);
    if (index < 0)
        return;
    auto& widget = widgets_[index];
    if (!widget.valid ||
        (widget.health.ConsecutiveErrors() == 0 &&
            !widget.health.RecoveryProbe()))
    {
        return;
    }
    const bool recovered = widget.health.RecoveryProbe();
    widget.health.RecordSuccess();
    widgetHostFailures_.erase(widgetId);
    if (recovered)
    {
        RuntimeAddLog(widgetId, "info",
            "Automatic runtime recovery probe succeeded");
    }
}

void WidgetEngine::RecordWidgetHostFailure(
    const std::wstring& widgetId, const std::string& message,
    bool quotaExceeded, bool circuitOpen)
{
    widgetHostFailures_[widgetId] = {
        snowdesktop::widget_runtime::ClassifyWidgetRuntimeFailure(
            quotaExceeded, circuitOpen, message),
        message
    };
}

void WidgetEngine::RuntimeAddLog(const std::wstring& widgetId, const std::string& level, const std::string& message)
{
    g_widgetDiagnostics.Add(
        WideToUtf8(widgetId), level, message);
}

std::vector<WidgetDiagnosticEntry> WidgetEngine::GetWidgetDiagnostics() const
{
    std::vector<WidgetDiagnosticEntry> result;
    result.reserve(widgets_.size());
    for (const auto& widget : widgets_)
    {
        WidgetDiagnosticEntry entry;
        entry.widgetId = widget.widgetId;
        entry.name = widget.name;
        entry.scriptPath = widget.filePath;
        entry.packageId = widget.packageId;
        entry.packageVersion = widget.manifest.version;
        entry.valid = widget.valid;
        entry.hasManifest = widget.manifest.hasManifest;
        entry.permissions = widget.manifest.permissions;
        for (const auto& permission : widget.manifest.optionalPermissions)
        {
            if (std::find(entry.permissions.begin(),
                    entry.permissions.end(), permission) ==
                entry.permissions.end())
            {
                entry.permissions.push_back(permission);
            }
        }
        if (widget.quota)
        {
            entry.memoryBytes = widget.quota->memoryBytes;
            entry.memoryLimit = widget.quota->memoryLimit;
            entry.lastCallbackMs = widget.quota->lastExecutionMs;
            entry.executionQuotaExceeded = widget.quota->executionExceeded;
            entry.memoryQuotaExceeded = widget.quota->memoryExceeded;
        }
        entry.circuitOpen = widget.health.CircuitOpen();
        if (widget.viewTree)
            entry.desktopViewNodes =
                snowdesktop::widget_runtime::InspectViewTree(
                    *widget.viewTree);
        if (widget.panelViewTree)
        {
            entry.auxiliaryViewNodes =
                snowdesktop::widget_runtime::InspectViewTree(
                    *widget.panelViewTree);
            entry.auxiliarySurface = widget.panelSurface;
        }
        std::string errorKey = WideToUtf8(widget.widgetId) + ".lastError";
        auto errIt = PersistentStorage().find(errorKey);
        if (errIt != PersistentStorage().end())
            entry.lastError = errIt->second;
        std::string logKey = WideToUtf8(widget.widgetId);
        entry.logs = g_widgetDiagnostics.EntriesFor(logKey);
        result.push_back(std::move(entry));
    }
    return result;
}

void WidgetEngine::RecordPerformanceResources() const noexcept
{
    if (!snowdesktop::performance::Enabled() || !d2dState_) return;
    try
    {
        using snowdesktop::performance::Value;
        snowdesktop::performance::Scope scope("profiler", "sample.widget.resources");
        auto imageUsage = d2dState_->runtimeImageCache.Measure();
        for (const auto& widget : widgets_)
        {
            imageUsage.owners.try_emplace(widget.widgetId);
            if (widget.quota)
            {
                Value("widget.memory", "lua_bytes", widget.widgetId, static_cast<double>(widget.quota->memoryBytes));
                snowdesktop::lua_runtime::SampleAllocationTraffic(*widget.quota, widget.widgetId);
            }
            Value("widget.state", "valid", widget.widgetId, widget.valid ? 1 : 0);
            Value("widget.state", "visible", widget.widgetId, widget.hostVisible ? 1 : 0);
            Value("widget.state", "timers", widget.widgetId, static_cast<double>(widget.namedTimers.Size()));
            Value("widget.state", "animation_requests", widget.widgetId, static_cast<double>(widget.animationFrames.Size()));
            Value("widget.package", widget.packageId, widget.widgetId, 1);
        }
        const auto bitmapBytes = [](ID2D1Bitmap* bitmap) -> std::size_t {
            if (!bitmap) return 0;
            const auto size = bitmap->GetPixelSize();
            return std::size_t(size.width) * size.height * 4;
        };
        const auto cacheBytes = [&](const auto& cache) {
            std::size_t bytes = 0;
            for (const auto& [key, bitmap] : cache) bytes += bitmapBytes(bitmap.Get());
            return bytes;
        };
        for (const auto& [id, usage] : imageUsage.owners)
        {
            Value("widget.memory", "runtime_image_sources", id, static_cast<double>(usage.sources));
            Value("widget.memory", "runtime_image_referenced_bytes", id, static_cast<double>(usage.pixelBytes));
            Value("widget.memory", "runtime_bitmap_bgra_bytes_estimate", id, static_cast<double>(usage.bitmapBytes));
        }
        const auto shared = [](std::string_view name, std::size_t value) {
            Value("widget.shared.memory", name, {}, static_cast<double>(value));
        };
        // Source pixels can be shared across owners. Report their deduplicated
        // total separately; owner rows are references, not exclusive ownership.
        shared("runtime_image_unique_bytes", imageUsage.uniquePixelBytes);
        shared("runtime_bitmap_bgra_bytes_estimate", imageUsage.bitmapBytes);
        shared("package_image_decoded_bytes", d2dState_->packageImageCache.Bytes());
        shared("package_image_encoded_bytes", d2dState_->packageImageCache.EncodedBytes());
        shared("package_image_sources", d2dState_->packageImageCache.Size());
        shared("package_bitmap_bgra_bytes_estimate", cacheBytes(d2dState_->imageCache));
        shared("image_sampling_bgra_bytes_estimate", d2dState_->imageSamplingCache.RetainedBytes());
        shared("image_sampling_entries", d2dState_->imageSamplingCache.Size());
        shared("shell_icon_bgra_bytes_estimate", cacheBytes(d2dState_->shellIconCache));
        shared("shell_icon_count", d2dState_->shellIconCache.size());
        const auto resourceCounts = d2dState_->ResourceCounts();
        shared("text_format_count", resourceCounts.textFormats);
        shared("text_layout_cache_entries", d2dState_->textLayoutCache.Size());
        shared("text_layout_cache_input_bytes", d2dState_->textLayoutCache.RetainedInputBytes());
        shared("private_text_format_count", resourceCounts.privateTextFormats);
        shared("private_font_count", resourceCounts.privateFonts);
        shared("brush_count", resourceCounts.brushes);
        shared("background_cache_retained_bytes_estimate", d2dState_->backgroundCache.RetainedBytes());
        shared("background_cache_entries", d2dState_->backgroundCache.Size());
    }
    catch (...)
    {
        // Diagnostic allocation failure must not change the running desktop.
        snowdesktop::performance::Value("profiler", "widget_resources_error", {}, 1);
    }
}
