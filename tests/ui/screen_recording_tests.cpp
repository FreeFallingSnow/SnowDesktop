#include "ui/preview/screen_capture.h"
#include "ui/preview/video_writer.h"
#include "test_temporary_directory.h"
#include "platform/native_import_patch.h"
#include <roapi.h>
#include <atomic>
#include <chrono>
#include <cwchar>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
void Require(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
template<class Action> void Invalid(Action action)
{
    bool rejected = false;
    try { action(); } catch (const std::invalid_argument&) { rejected = true; }
    Require(rejected, "invalid recording options were accepted");
}

// Exercise Service::Record's real asynchronous startup. Replacing only the
// monitor/WinRT/Media Foundation boundaries prevents any desktop capture. An
// encoder that cannot prepare must fail before WGC starts producing frames;
// otherwise its only static image can precede the encoder-ready timestamp.
std::atomic<unsigned> captureActivations{0}, encoderPreparations{0};
decltype(&LoadLibraryExW) nativeLoadLibrary = nullptr;
BOOL WINAPI EnumerateMonitor(HDC, LPCRECT, MONITORENUMPROC visit, LPARAM data)
{
    RECT bounds{0, 0, 64, 64};
    return visit(reinterpret_cast<HMONITOR>(1), nullptr, &bounds, data);
}
BOOL WINAPI MonitorInformation(HMONITOR, LPMONITORINFO info)
{
    info->rcMonitor = info->rcWork = RECT{0, 0, 64, 64};
    info->dwFlags = MONITORINFOF_PRIMARY;
    if (info->cbSize >= sizeof(MONITORINFOEXW))
        wcscpy_s(reinterpret_cast<MONITORINFOEXW*>(info)->szDevice, L"isolated-static-monitor");
    return TRUE;
}
HRESULT WINAPI RejectCaptureActivation(HSTRING, REFIID, void** result)
{
    ++captureActivations;
    *result = nullptr;
    return E_ACCESSDENIED;
}
HMODULE WINAPI RejectEncoderLibrary(LPCWSTR name, HANDLE file, DWORD flags)
{
    if (name && _wcsicmp(name, L"mfplat.dll") == 0)
    {
        ++encoderPreparations;
        SetLastError(ERROR_MOD_NOT_FOUND);
        return nullptr;
    }
    return nativeLoadLibrary(name, file, flags);
}
snowdesktop::screen_capture::State Await(snowdesktop::screen_capture::Service& service)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline)
    {
        const auto state = service.Current();
        if (state.status != "running") return state;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    throw std::runtime_error("isolated capture worker did not finish");
}
void StartupOrder()
{
    using namespace snowdesktop;
    test::NativeImportPatch enumerate("EnumDisplayMonitors", reinterpret_cast<ULONG_PTR>(&EnumerateMonitor));
    test::NativeImportPatch information("GetMonitorInfoW", reinterpret_cast<ULONG_PTR>(&MonitorInformation));
    test::NativeImportPatch activation("RoGetActivationFactory", reinterpret_cast<ULONG_PTR>(&RejectCaptureActivation));
    test::NativeImportPatch library("LoadLibraryExW", reinterpret_cast<ULONG_PTR>(&RejectEncoderLibrary));
    nativeLoadLibrary = reinterpret_cast<decltype(nativeLoadLibrary)>(library.original);
    test::TemporaryDirectory temporary;
    screen_capture::Service service;
    std::string error;
    Require(service.Record(L"primary", temporary.path, false, 1000, 30, error),
        "valid isolated recording must reach the production worker");
    const auto recording = Await(service);
    service.Stop();
    Require(encoderPreparations == 1 && captureActivations == 0,
        "recording must prepare its encoder before activating WGC, which can produce only one static frame");
    Require(recording.status == "failed" && recording.error == "video_media_components_unavailable" &&
        recording.frames == 0 && recording.capturedFrames == 0 && recording.path.empty(),
        "encoder preparation failure cannot start capture or invent completed video");

    Require(service.Screenshot(L"primary", temporary.path, false, error),
        "screenshot must still reach its independent capture startup");
    const auto screenshot = Await(service);
    service.Stop();
    Require(captureActivations == 1 && encoderPreparations == 1 && screenshot.status == "failed" && screenshot.path.empty(),
        "screenshot activates WGC without preparing an unrelated video encoder");
}
}
int main()
{
    try
    {
        using namespace snowdesktop;
        using namespace screen_capture;
        Require(RecordingFrameCount(8000, 30) == 240, "8 seconds at 30fps must retain 240 output slots");
        Require(RecordingFrameCount(120000, 60) == 7200, "maximum recording remains bounded");
        Require(RecordingFrameCount(100, 1) == 1 && RecordingFrameCount(101, 30) == 4,
            "short recordings round up to a complete CFR frame");
        for (const unsigned rate : {1U, 7U, 29U, 30U, 59U, 60U})
            for (const unsigned milliseconds : {100U, 101U, 999U, 1000U, 12345U, 120000U})
            {
                const auto frames = RecordingFrameCount(milliseconds, rate);
                const auto requested = static_cast<std::int64_t>(milliseconds) * 10000;
                Require(video_writer::FrameTime(frames, rate) >= requested,
                    "encoded CFR duration cannot truncate the requested real-time interval");
                Require(video_writer::FrameTime(frames - 1, rate) < requested,
                    "CFR rounding may add at most one frame");
            }
        Invalid([] { RecordingFrameCount(0, 30); });
        Invalid([] { RecordingFrameCount(99, 30); });
        Invalid([] { RecordingFrameCount(120001, 30); });
        Invalid([] { RecordingFrameCount(1000, 0); });
        Invalid([] { RecordingFrameCount(1000, 61); });
        Invalid([] { RecordingFrameCount(std::numeric_limits<unsigned>::max(), 60); });

        // These rejection paths run before monitor enumeration or WGC startup.
        // The ordinary regression does not capture the user's personal desktop.
        Service service;
        std::string error;
        test::TemporaryDirectory temporary;
        Require(!service.Record(L"unavailable", temporary.path, false, 99, 30, error) &&
            error == "invalid_recording_options", "invalid duration cannot start a capture worker");
        Require(!service.Record(L"unavailable", temporary.path, false, 1000, 61, error) &&
            error == "invalid_recording_options", "invalid rate cannot start a capture worker");
        Require(!service.Record(L"unavailable", L"relative-output", false, 1000, 30, error) &&
            error == "invalid_capture_root", "relative output cannot select a monitor or write files");
        Require(!service.Screenshot(L"unavailable", L"relative-output", false, error) &&
            error == "invalid_capture_root", "screenshot shares the protected output contract");
        const auto state = service.Current();
        Require(state.status == "idle" && state.id.empty() && state.source == "wgc-monitor" &&
            state.frames == 0 && state.capturedFrames == 0 && state.path.empty(),
            "rejected requests cannot fabricate frames or transition service state");
        Require(std::filesystem::is_empty(temporary.path), "rejected requests cannot leave capture artifacts");
        service.Cancel(); service.Stop(); service.Stop();
        Require(service.Current().status == "idle", "idle cancellation cannot invent a completed task");
        StartupOrder();
        std::cout << "real-time capture options, CFR and production startup-order checks passed; no desktop was captured\n";
        return 0;
    }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
