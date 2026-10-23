#include "../src/app/features/wallpaper_engine_capture.h"
#include "../src/platform/runtime_module_policy.h"
#include "platform/deployment_context.h"
#include "wallpaper_hook/remote_injection_cleanup.h"
#include "wallpaper_hook/shared_frame_access.h"
#include "wallpaper_hook/wallpaper_hook_worker.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <cwchar>
#include <string_view>
#include <thread>
#include <vector>

namespace snowdesktop::deployment
{
std::wstring GetRuntimeFilePath(const wchar_t* filename)
{
    return filename ? filename : L"";
}

std::wstring GetInjectableRuntimeFilePath(const wchar_t* filename)
{
    return GetRuntimeFilePath(filename);
}
}

namespace
{

void Check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAILED: " << message << '\n';
        std::exit(1);
    }
}

void TestSharedSurfaceAccess()
{
    using snow::wallpaper_hook::UseSharedFrame;
    for (const HRESULT acquire : { S_OK, static_cast<HRESULT>(WAIT_TIMEOUT),
            static_cast<HRESULT>(WAIT_ABANDONED), E_FAIL, S_FALSE })
    {
        int copies = 0, releases = 0, recreations = 0;
        const HRESULT result = UseSharedFrame([&] { return acquire; },
            [&] { ++releases; }, [&] { ++copies; return S_OK; }, [&] { ++recreations; });
        Check(result == acquire && copies == (acquire == S_OK ? 1 : 0) &&
            releases == copies && recreations == (acquire == WAIT_ABANDONED ? 1 : 0),
            "shared frame only copies/releases an owned surface; abandoned state requests recreation");
    }
    int generation = 1, copiedGeneration = 0, releases = 0;
    const auto copy = [&] { copiedGeneration = generation; return S_OK; };
    Check(UseSharedFrame([] { return static_cast<HRESULT>(WAIT_ABANDONED); },
        [&] { ++releases; }, copy, [&] { ++generation; }) == WAIT_ABANDONED &&
        copiedGeneration == 0 && releases == 0, "an abandoned generation never publishes a frame");
    Check(UseSharedFrame([] { return S_OK; }, [&] { ++releases; }, copy,
        [&] { ++generation; }) == S_OK && copiedGeneration == 2 && releases == 1,
        "a rebuilt shared surface can publish the next requested frame");
    Check(UseSharedFrame([] { return S_OK; }, [&] { ++releases; },
        [] { return E_FAIL; }, [] {}) == E_FAIL && releases == 2,
        "readback failure still releases the mutex it acquired");
}

struct WorkerFixture
{
    snow::wallpaper_hook::SharedState& state;
    int failuresRemaining = 0;
    bool disableFails = false;
    int initializes = 0, disables = 0, idleObservations = 0;
    ULONGLONG clock = 1;
    HRESULT Initialize() { ++initializes; return failuresRemaining-- > 0 ? E_FAIL : S_OK; }
    HRESULT Disable() { ++disables; return disableFails ? E_ACCESSDENIED : S_OK; }
    ULONGLONG Now() const { return clock; }
    bool Wait(DWORD milliseconds)
    {
        clock += milliseconds;
        if (state.status == static_cast<LONG>(snow::wallpaper_hook::Status::hooked))
        {
            InterlockedExchange(&state.capture_enabled, 1);
            InterlockedExchange(&state.shutdown_requested, 1);
        }
        else
        {
            ++idleObservations;
            Check(state.capture_enabled == 0,
                "failed or stopped capture disables publication even if entry disabling failed");
            if (initializes >= 3)
                return false;
            InterlockedExchange(&state.shutdown_requested, snow::wallpaper_hook::kRestartRequested);
        }
        return true;
    }
};

void TestWorkerRetryAndRetention(bool initializationFails = true)
{
    using namespace snow::wallpaper_hook;
    for (const bool disableFails : { false, true })
    {
        SharedState state{};
        // An original Present remains in flight across failure, stop and retry.
        // The actual worker does not release its state or terminate at one second.
        state.active_hooks = 1;
        state.magic = kMagic;
        WorkerFixture operations{ state };
        operations.failuresRemaining = initializationFails ? 1 : 0;
        operations.disableFails = disableFails;
        RunCaptureHookWorker(state, operations);
        Check(operations.initializes == 3 && operations.disables == 3 &&
            operations.idleObservations == 3 && state.active_hooks == 1 && state.magic == kMagic,
            "initialization failure and later shutdown keep a live worker/state for retry with callbacks in flight");
        Check(state.status == static_cast<LONG>(disableFails ? Status::failed : Status::stopping),
            "entry disabling failures are retained as failures, not claimed as successful teardown");
        InterlockedDecrement(&state.active_hooks);
    }
}

HANDLE remoteReadGate = nullptr;
HANDLE remoteReadEntered = nullptr;
std::atomic_bool remotePathRead = false;

DWORD WINAPI DelayedPathReader(void* path)
{
    SetEvent(remoteReadEntered);
    WaitForSingleObject(remoteReadGate, INFINITE);
    MEMORY_BASIC_INFORMATION information{};
    if (VirtualQuery(path, &information, sizeof(information)) && information.State == MEM_COMMIT)
        remotePathRead = std::wcscmp(static_cast<const wchar_t*>(path), L"pending-library-path") == 0;
    return remotePathRead ? 42 : 0;
}

void TestPendingInjectionArgument()
{
    remoteReadGate = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    remoteReadEntered = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE process = OpenProcess(PROCESS_VM_OPERATION | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION,
        FALSE, GetCurrentProcessId());
    auto* path = static_cast<wchar_t*>(VirtualAllocEx(process, nullptr, 4096,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    Check(remoteReadGate && remoteReadEntered && process && path, "isolated injection ownership fixture starts");
    wcscpy_s(path, 2048, L"pending-library-path");
    HANDLE thread = CreateThread(nullptr, 0, DelayedPathReader, path, 0, nullptr);
    Check(thread && WaitForSingleObject(remoteReadEntered, 1000) == WAIT_OBJECT_0,
        "controlled target waits before reading its native thread argument");
    std::atomic_bool completed = false;
    std::atomic_bool loaded = false;
    HANDLE initialWaitDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE continueCleanup = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE holdingArgument = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE cleanupDone = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread cleanup([&] {
        int waits = 0;
        loaded = snow::wallpaper_hook::WaitAndCompleteRemoteInjection(process, thread, path, 0,
            [&](HANDLE handle, DWORD milliseconds) {
                if (++waits > 1)
                    SetEvent(holdingArgument);
                const DWORD result = WaitForSingleObject(handle, milliseconds);
                if (waits == 1)
                {
                    SetEvent(initialWaitDone);
                    WaitForSingleObject(continueCleanup, INFINITE);
                }
                return result;
            });
        completed = true;
        SetEvent(cleanupDone);
    });
    Check(WaitForSingleObject(initialWaitDone, 1000) == WAIT_OBJECT_0,
        "argument owner starts the zero-duration initial wait");
    SetEvent(continueCleanup);
    const HANDLE boundaryEvents[]{ holdingArgument, cleanupDone };
    const DWORD ownershipBoundary = WaitForMultipleObjects(2, boundaryEvents, FALSE, 2000);
    MEMORY_BASIC_INFORMATION information{};
    const bool retained = ownershipBoundary == WAIT_OBJECT_0 &&
        VirtualQuery(path, &information, sizeof(information)) && information.State == MEM_COMMIT;
    SetEvent(remoteReadGate);
    cleanup.join();
    Check(retained, "pending target argument remains allocated while the initial wait expires");
    Check(completed && loaded && remotePathRead,
        "delayed target reads the intact argument before the reaper finishes");
    VirtualQuery(path, &information, sizeof(information));
    Check(information.State == MEM_FREE, "the target allocation is released after actual thread completion");
    CloseHandle(initialWaitDone);
    CloseHandle(continueCleanup);
    CloseHandle(holdingArgument);
    CloseHandle(cleanupDone);
    CloseHandle(remoteReadEntered);
    CloseHandle(remoteReadGate);
}

void TestLegacyHookInjectionGuard()
{
    using snowdesktop::wallpaper_engine_capture::CanInjectCaptureHook;
    wchar_t name[128]{};
    _snwprintf_s(name, 128, _TRUNCATE,
        L"Local\\SnowDesktop.WallpaperEngine.DxgiHook.v3.%lu",
        static_cast<unsigned long>(GetCurrentProcessId()));
    std::wstring error;
    Check(CanInjectCaptureHook(GetCurrentProcessId(), error),
        "a process without a legacy producer remains eligible for injection");
    DWORD before = 0, after = 0;
    GetProcessHandleCount(GetCurrentProcess(), &before);
    HANDLE legacy = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
        PAGE_READWRITE, 0, 4096, name);
    Check(legacy && GetLastError() != ERROR_ALREADY_EXISTS,
        "isolated old-protocol mapping is created for this test process");
    for (int attempt = 0; attempt < 16; ++attempt)
        Check(!CanInjectCaptureHook(GetCurrentProcessId(), error) &&
            error.find(L"restart") != std::wstring::npos,
            "a live legacy producer prevents competing Hook injection and explains recovery");
    CloseHandle(legacy);
    GetProcessHandleCount(GetCurrentProcess(), &after);
    Check(before == after && CanInjectCaptureHook(GetCurrentProcessId(), error),
        "legacy probes close their handles and permit injection after the old owner exits");
}

} // namespace

int main(int argc, char** argv)
{
    const std::string_view mode = argc > 1 ? argv[1] : "all";
    if (mode == "all" || mode == "surface") TestSharedSurfaceAccess();
    if (mode == "all" || mode == "worker") TestWorkerRetryAndRetention();
    if (mode == "all" || mode == "retention") TestWorkerRetryAndRetention(false);
    if (mode == "all" || mode == "injection") TestPendingInjectionArgument();
    if (mode == "all" || mode == "legacy") TestLegacyHookInjectionGuard();
    if (mode != "all")
    {
        Check(mode == "surface" || mode == "worker" || mode == "retention" ||
            mode == "injection" || mode == "legacy", "known capture test mode");
        return 0;
    }
    {
        namespace modules = snowdesktop::runtime_modules;
        modules::Policy policy; policy.allowed[static_cast<std::size_t>(modules::Module::WallpaperPreview)] = false;
        modules::Apply(policy);
        const auto disabled = snowdesktop::wallpaper_engine_capture::CaptureOneShotForMonitor({0, 0, 100, 100}, 100);
        Check(disabled.backdrop.Empty() && !disabled.wallpaperEngineDetected && disabled.error.empty(), "disabled preview avoids process discovery, capture and injection while permitting a static fallback");
        modules::Apply({});
    }
    using snowdesktop::wallpaper_engine_capture::CropFrameToDesktopRegion;
    using snowdesktop::wallpaper_engine_capture::CancellableWaitResult;
    using snowdesktop::wallpaper_engine_capture::WaitForHandleOrCancellation;

    HANDLE waitEvent = CreateEventW(nullptr, TRUE, TRUE, nullptr);
    Check(waitEvent != nullptr,
        "cancellable wait test event is available");
    Check(WaitForHandleOrCancellation(waitEvent, 100) ==
            CancellableWaitResult::Signaled,
        "cancellable wait reports a signaled handle");
    ResetEvent(waitEvent);
    Check(WaitForHandleOrCancellation(waitEvent, 20) ==
            CancellableWaitResult::TimedOut,
        "cancellable wait preserves finite timeouts");
    std::atomic_bool cancelWait = false;
    std::thread cancelThread([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        cancelWait.store(true, std::memory_order_relaxed);
    });
    const auto waitStarted = std::chrono::steady_clock::now();
    const auto cancelledResult = WaitForHandleOrCancellation(
        waitEvent, 2000, &cancelWait);
    const auto cancelledElapsed = std::chrono::steady_clock::now() -
        waitStarted;
    cancelThread.join();
    namespace modules = snowdesktop::runtime_modules;
    std::thread disableModule([] {
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        modules::Policy policy;
        policy.allowed[static_cast<std::size_t>(modules::Module::WallpaperPreview)] = false;
        modules::Apply(policy);
    });
    const auto disabledWaitStarted = std::chrono::steady_clock::now();
    const auto disabledWait = WaitForHandleOrCancellation(waitEvent, 2000);
    disableModule.join();
    const auto disabledWaitElapsed = std::chrono::steady_clock::now() - disabledWaitStarted;
    modules::Apply({});
    Check(disabledWait == CancellableWaitResult::Cancelled &&
            disabledWaitElapsed < std::chrono::milliseconds(750),
        "disabling the preview module interrupts a pending injection wait without a caller cancellation flag");
    CloseHandle(waitEvent);
    Check(cancelledResult == CancellableWaitResult::Cancelled &&
            cancelledElapsed < std::chrono::milliseconds(750),
        "cancellable wait exits promptly without consuming its full timeout");

    const std::vector<std::uint32_t> source{
        0x00112233u, 0x00445566u, 0x00778899u, 0x00aabbccu,
        0x00010203u, 0x00040506u, 0x00070809u, 0x000a0b0cu,
    };
    const RECT sourceBounds{ -2, 3, 2, 5 };
    const RECT centerBounds{ -1, 3, 1, 5 };
    const auto center = CropFrameToDesktopRegion(source.data(), 4, 2,
        sourceBounds, centerBounds);
    Check(center.width == 2 && center.height == 2 &&
            EqualRect(&center.desktopBounds, &centerBounds),
        "one-shot frame crop preserves physical desktop bounds");
    Check(center.pixels == std::vector<std::uint32_t>{
            0xff445566u, 0xff778899u,
            0xff040506u, 0xff070809u },
        "one-shot frame crop selects the matching source pixels and forces opacity");

    const std::vector<std::uint32_t> scaledSource{
        0x00102030u, 0x00405060u,
    };
    const RECT scaledDesktop{ 0, 0, 4, 2 };
    const auto scaled = CropFrameToDesktopRegion(scaledSource.data(), 2, 1,
        scaledDesktop, scaledDesktop);
    Check(scaled.width == 4 && scaled.height == 2 &&
            scaled.pixels == std::vector<std::uint32_t>{
                0xff102030u, 0xff102030u,
                0xff405060u, 0xff405060u,
                0xff102030u, 0xff102030u,
                0xff405060u, 0xff405060u },
        "one-shot frame crop maps renderer pixels back to physical monitor coordinates");

    const RECT partialRequest{ 1, 4, 6, 8 };
    const RECT expectedIntersection{ 1, 4, 2, 5 };
    const auto partial = CropFrameToDesktopRegion(source.data(), 4, 2,
        sourceBounds, partialRequest);
    Check(partial.width == 1 && partial.height == 1 &&
            EqualRect(&partial.desktopBounds, &expectedIntersection) &&
            partial.pixels.front() == 0xff0a0b0cu,
        "one-shot frame crop clips requests to the published output rectangle");

    const RECT outside{ 20, 20, 30, 30 };
    Check(CropFrameToDesktopRegion(source.data(), 4, 2,
            sourceBounds, outside).Empty() &&
          CropFrameToDesktopRegion(nullptr, 4, 2,
            sourceBounds, centerBounds).Empty(),
        "one-shot frame crop rejects missing and non-overlapping sources");

    std::cout << "wallpaper engine capture tests passed\n";
    return 0;
}
