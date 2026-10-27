#include "widget_author_profile.h"
#include "widget/runtime/widget_engine.h"
#include "diagnostics/performance_capture.h"
#include "diagnostics/performance_analysis.h"
#include "diagnostics/performance_artifacts.h"
#include "diagnostics/performance_trace.h"
#include "ui/render/ui_animation_scheduler.h"

#include <windows.h>
#include <algorithm>
#include <chrono>
#include <iomanip>
#include <sstream>

namespace snowdesktop::widget_authoring
{
namespace
{
namespace perf = snowdesktop::performance;
using Clock = std::chrono::steady_clock;
constexpr wchar_t Instance[] = L"__snowwidget_author_preview__";
std::string Utf8(const std::filesystem::path& path)
{
    const auto value = path.u8string();
    return std::string(value.begin(), value.end());
}
std::string Quote(std::string_view text)
{
    std::ostringstream out;
    out << '"';
    for (const unsigned char ch : text)
    {
        if (ch == '"' || ch == '\\') out << '\\' << static_cast<char>(ch);
        else if (ch < 32) out << "\\u00" << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(ch) << std::dec;
        else out << static_cast<char>(ch);
    }
    out << '"';
    return out.str();
}
}
std::string PreviewProfileResult::ToJson() const
{
    std::ostringstream out;
    out << "{\"schemaVersion\":1,\"processRole\":\"widget-preview\",\"quality\":" << Quote(quality)
        << ",\"error\":" << Quote(error) << ",\"drawCalls\":" << drawCalls
        << ",\"requestedSeconds\":" << seconds << ",\"warmupSeconds\":" << warmupSeconds
        << ",\"redrawIntervalMs\":" << intervalMs << ",\"workload\":" << Quote(redraw ? "redraw" : "idle")
        << ",\"manifest\":" << Quote(Utf8(manifest)) << ",\"report\":" << Quote(Utf8(report))
        << ",\"inputSampleEvery\":" << inputSampleEvery << ",\"moduleFilter\":[";
    for (std::size_t index = 0; index < modules.size(); ++index)
    { if (index) out << ','; out << Quote(modules[index]); }
    out << "],\"coverage\":\"isolated preview; live services and desktop presentation excluded; initial load and warmup excluded\"}";
    return out.str();
}

struct WidgetAuthorProfile::Impl
{
    WidgetEngine& engine;
    PreviewProfileOptions options;
    UiAnimationScheduler scheduler;
    HWND window = nullptr;
    bool dirty = true;
    explicit Impl(WidgetEngine& value) : engine(value) {}
    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && perf::IsControlMessage(message, wp, lp))
            return perf::HandleControlMessage(hwnd, message, wp, lp, +[](void* owner) {
                static_cast<Impl*>(owner)->engine.RecordPerformanceResources();
            }, self);
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    ~Impl()
    {
        perf::Shutdown();
        engine.SetWidgetTimerRequestCallback({});
        engine.SetWidgetTimerKillCallback({});
        engine.SetInvalidateCallback({});
        if (window) DestroyWindow(window);
    }
    bool Pump(DWORD timeout)
    {
        const auto timer = scheduler.WaitHandle();
        const auto wake = MsgWaitForMultipleObjects(1, &timer, FALSE, timeout, QS_ALLINPUT);
        if (wake == WAIT_FAILED) return false;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) return false;
            TranslateMessage(&message); DispatchMessageW(&message);
        }
        // The scheduler timer auto-resets when the multi-object wait succeeds.
        // Keep that result instead of consuming and then losing the same signal.
        if (wake == WAIT_OBJECT_0 || WaitForSingleObject(timer, 0) == WAIT_OBJECT_0)
            scheduler.DispatchDue();
        engine.TickRuntime();
        return true;
    }
};
WidgetAuthorProfile::WidgetAuthorProfile(WidgetEngine& engine) : impl_(std::make_unique<Impl>(engine)) {}
WidgetAuthorProfile::~WidgetAuthorProfile() = default;
bool WidgetAuthorProfile::Initialize(const PreviewProfileOptions& options, std::string& error)
{
    auto& state = *impl_;
    if (!options.seconds || options.seconds > 60 || options.warmupSeconds > 30 ||
        options.intervalMs < 8 || options.intervalMs > 1000 || !options.inputSampleEvery || options.inputSampleEvery > 1000 ||
        options.modules.size() > perf::CaptureModules.size() ||
        std::any_of(options.modules.begin(), options.modules.end(), [](const auto& module) {
            return std::find(perf::CaptureModules.begin(), perf::CaptureModules.end(), module) == perf::CaptureModules.end(); }) ||
        !perf::IsLocalCapturePath(options.directory / L"trace.pftrace") || std::filesystem::exists(options.directory))
    { error = "invalidProfileOptionsOrExistingOutput"; return false; }
    state.options = options;
    std::sort(state.options.modules.begin(), state.options.modules.end());
    if (std::adjacent_find(state.options.modules.begin(), state.options.modules.end()) != state.options.modules.end())
    { error = "invalidProfileModuleFilter"; return false; }
    if (!state.scheduler.Initialize()) { error = "profileSchedulerUnavailable"; return false; }
    WNDCLASSW type{}; type.lpfnWndProc = Impl::WindowProc;
    type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"SnowDesktopAuthorProfileControl";
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    { error = "profileControlUnavailable"; return false; }
    state.window = CreateWindowW(type.lpszClassName, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, nullptr, type.hInstance, &state);
    if (!state.window) { error = "profileControlUnavailable"; return false; }
    perf::ConfigureControl(state.window, {});
    state.engine.SetWidgetTimerRequestCallback([&state](const std::wstring& instance, UINT interval) -> UINT_PTR {
        return static_cast<UINT_PTR>(state.scheduler.ScheduleInterval(interval, [&state, instance](UiScheduleToken token) {
            state.engine.OnWidgetTimer(instance, static_cast<UINT_PTR>(token));
        }));
    });
    state.engine.SetWidgetTimerKillCallback([&state](UINT_PTR token) { state.scheduler.Cancel(token); });
    state.engine.SetInvalidateCallback([&state](const auto&, const auto&, auto) { state.dirty = true; });
    return true;
}
PreviewProfileResult WidgetAuthorProfile::Measure(const std::function<bool()>& draw)
{
    auto& state = *impl_;
    PreviewProfileResult result;
    result.seconds = state.options.seconds; result.warmupSeconds = state.options.warmupSeconds;
    result.intervalMs = state.options.intervalMs; result.redraw = state.options.redraw;
    result.modules = state.options.modules; result.inputSampleEvery = state.options.inputSampleEvery;
    const auto cadence = state.options.redraw ? state.scheduler.ScheduleInterval(state.options.intervalMs, [&state](auto) {
        state.dirty = true;
    }) : 0;
    const auto work = [&] {
        if (!state.dirty) return true;
        state.dirty = false;
        perf::Scope render("author.preview", "render", Instance);
        // This offscreen host owns the desktop-surface draw, just as the real
        // composition host does. Consume actual component invalidations here.
        perf::DrawLink("desktop", Instance, true);
        if (!draw()) return false;
        ++result.drawCalls;
        return true;
    };
    const auto warmupEnd = Clock::now() + std::chrono::seconds(state.options.warmupSeconds);
    while (Clock::now() < warmupEnd)
        if (!state.Pump(state.options.redraw ? state.options.intervalMs : 1000) || !work()) { result.error = "profileWarmupFailed"; return result; }
    result.drawCalls = 0;
    perf::CaptureOptions capture;
    capture.session = "widget-profile"; capture.output = state.options.directory / L"trace.pftrace";
    capture.seconds = state.options.seconds; capture.bufferKiB = 65536;
    capture.preset = perf::CapturePreset::Widget; capture.processRole = perf::CaptureProcessRole::WidgetPreview;
    capture.scopeCpu = state.options.threadCpu; capture.selectedInstances = {Instance};
    capture.modules = state.options.modules; capture.inputSampleEvery = state.options.inputSampleEvery;
    if (!perf::Start(capture, result.error)) return result;
    auto snapshot = perf::Snapshot(capture.session);
    const auto startupEnd = Clock::now() + std::chrono::seconds(20);
    while (snapshot.phase == perf::CapturePhase::Starting && Clock::now() < startupEnd)
    {
        if (!state.Pump(20)) break;
        snapshot = perf::Snapshot(capture.session);
    }
    if (snapshot.phase != perf::CapturePhase::Recording)
    { result.error = snapshot.error.empty() ? "profileStartFailed" : snapshot.error; return result; }
    state.dirty = true;
    const auto end = Clock::now() + std::chrono::seconds(state.options.seconds + 5);
    while (Clock::now() < end)
    {
        if (!state.Pump(state.options.redraw ? state.options.intervalMs : 1000)) { result.error = "profileWorkloadCanceled"; break; }
        snapshot = perf::Snapshot(capture.session);
        if (snapshot.phase == perf::CapturePhase::Recording && !work()) { result.error = "profileWorkloadFailed"; break; }
        if (snapshot.phase == perf::CapturePhase::Completed || snapshot.phase == perf::CapturePhase::Failed) break;
    }
    state.scheduler.Cancel(cadence);
    if (snapshot.phase != perf::CapturePhase::Completed)
    { if (result.error.empty()) result.error = snapshot.error.empty() ? "profileCaptureFailed" : snapshot.error; return result; }
    result.manifest = snapshot.manifest;
    const auto analysis = perf::AnalyzeCapture(snapshot.manifest, nullptr, state.options.offline);
    result.error = analysis.error; result.quality = analysis.quality; result.report = analysis.report;
    return result;
}
}
