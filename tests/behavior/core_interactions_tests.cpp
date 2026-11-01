#include "app/app.h"
#include "widgets/widget_folder_search.h"
#include "ui/input/private_pointer.h"
#include "layout/folder_sort_rules.h"
#include "app/shell/shell_refresh_snapshot.h"
#include "desktop/desktop_namespace_registry.h"
#include "diagnostics/debug_profile.h"
#include "ole_file_source.h"
#include "global_drag_ole_driver.inc"
#include "global_drag_mixed_source.inc"
#include "global_drag_mixed_driver.inc"
#include "global_drag_payload_fixture.inc"
#include "global_drag_directory_fixture.inc"
#include "global_drag_directory_lease.inc"
#include "global_drag_directory_live.inc"
#include "global_drag_virtual_staging.inc"
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <exception>
#include <map>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>
namespace behavior { struct OwnedProfileDesktopRoot; }
// Supplies an isolated environment and observations; all routing, queues, I/O,
// completion handling and persistence below execute production implementations.
struct NativeDragIntegrationAccess
{
    static inline HWND completionWindow = nullptr;
    static inline HWND fixtureControlWindow = nullptr;
    static inline DesktopApp* asyncOwner = nullptr;
    static inline WNDPROC previousWindowProc = nullptr;
    static inline WNDPROC previousControlProc = nullptr;
    static inline std::exception_ptr asyncException;
    static inline std::atomic_bool wrongThreadShutdown{false};
    static inline std::atomic_bool profileReserved{false};
    static inline std::uint64_t asyncReadMessages = 0;
    static inline std::uint64_t asyncShellTimers = 0;
    static inline std::uint64_t fileCompletionMessages = 0;
    static inline std::uint64_t graceTimerMessages = 0;
    static inline std::uint64_t maintenanceTimerMessages = 0;
    static inline std::uint64_t sourceCompletionMessages = 0;
    static inline std::uint64_t commitCompletionMessages = 0;
    static inline std::uint64_t layoutSaveTimerMessages = 0;
    static inline std::uint64_t controlBackgroundMessages = 0;
    struct UrlMessageObservation
    {
        std::size_t traceIndex = 0;
        snowdesktop::UrlDropDownloadOutcome outcome = snowdesktop::UrlDropDownloadOutcome::Failed;
        std::wstring originalUrl, finalUrl, localPath, contentType, shortcutFallbackUrl;
        std::string error;
        std::size_t remainingUrls = 0, replacementShortcuts = 0;
        bool retryableCandidateFailure = false, returned = false, malformed = false;
    };
    static inline std::vector<UrlMessageObservation> urlMessages;
    static inline bool urlObservationFailed = false;
    struct RefreshMessageObservation
    {
        std::size_t traceIndex = 0;
        std::uint64_t beforeRevision = 0, afterRevision = 0;
        bool beforeRunning = false, afterRunning = false, beforeReady = false, afterReady = false, returned = false;
    };
    static inline std::vector<RefreshMessageObservation> refreshMessages;
    static inline bool refreshObservationFailed = false;
    static inline int productionDispatch = 0; // 1: desktop WndProc; 2: control WndProc
    static inline bool nativePointerDispatch = false, fixtureMaintenanceDisabled = false;
    static inline LONG_PTR previousWindowData = 0;
    static inline std::unique_ptr<drag_global::ole_driver::MessageTrace> messageTrace;
    static inline std::weak_ptr<drag_global::ole_driver::V3HostEpoch> v3HostEpoch;
    static inline std::weak_ptr<drag_global::ole_driver::NativeHostEpoch> nativeHostEpoch;
    static inline std::shared_ptr<drag_global::ole_driver::V4UnicodeSourceOwner> v4UnicodeSourceOwner;
    static inline std::shared_ptr<drag_global::V4IndependentShellOwner> v4IndependentShellOwner;
    static inline std::shared_ptr<drag_global::directory_fixture::DirectoryShellOwner> v4DirectoryShellOwner;
    static inline std::shared_ptr<drag_global::mixed_fixture::MixedOwner> v4MixedOwner;
    static inline std::shared_ptr<drag_global::mixed_fixture::ConfiguredContentOwner> v4ConfiguredContentOwner;
    static inline std::size_t productionDispatchDepth = 0;
    static inline bool fixtureTeardownIncomplete = false;
    static inline bool fixtureStopInProgress = false;

private:
    static LRESULT DispatchProduction(HWND window, UINT message, WPARAM wp, LPARAM lp, bool control)
    {
        using namespace drag_global::ole_driver;
        if (!messageTrace || productionDispatchDepth == (std::numeric_limits<std::size_t>::max)())
            throw std::runtime_error("production message trace unavailable or dispatch depth exhausted");
        ++productionDispatchDepth;
        struct DispatchScope final { ~DispatchScope() { --NativeDragIntegrationAccess::productionDispatchDepth; } } dispatchScope;
        const auto token = messageTrace->Enter(control ? MessageRoute::control_window_proc :
            MessageRoute::desktop_window_proc, window, message, wp, lp);
        snowdesktop::external_transfer_observation::WindowScope windowScope;
        if (windowScope.Id()) (void)messageTrace->BindWindowScope(token, windowScope.Id());
        std::size_t urlIndex = (std::numeric_limits<std::size_t>::max)();
        std::size_t refreshIndex = (std::numeric_limits<std::size_t>::max)();
        if (message == kBackgroundShellReadyMessage)
        {
            const DWORD savedError = GetLastError();
            try
            {
                RefreshMessageObservation event;
                event.traceIndex = token; event.beforeRevision = asyncOwner->shellRefreshRevision_.Current();
                event.beforeRunning = asyncOwner->shellRefreshRevision_.Running();
                event.beforeReady = static_cast<bool>(asyncOwner->readyShellRefresh_);
                refreshIndex = refreshMessages.size();
                refreshMessages.push_back(event);
            }
            catch (...) { refreshObservationFailed = true; refreshIndex = (std::numeric_limits<std::size_t>::max)(); }
            SetLastError(savedError);
        }
        if (message == kUrlDropDownloadCompletedMessage)
        {
            const DWORD savedError = GetLastError();
            try
            {
                UrlMessageObservation event;
                event.traceIndex = token;
                const auto* completion = reinterpret_cast<const DesktopApp::UrlDropDownloadUiCompletion*>(lp);
                if (!completion) event.malformed = true;
                else
                {
                    event.outcome = completion->result.outcome;
                    event.originalUrl = completion->result.originalUrl; event.finalUrl = completion->result.finalUrl;
                    event.localPath = completion->result.localPath; event.contentType = completion->result.contentType;
                    event.error = completion->result.error;
                    event.retryableCandidateFailure = completion->result.retryableCandidateFailure;
                    event.shortcutFallbackUrl = completion->shortcutFallbackUrl;
                    event.remainingUrls = completion->remainingUrls.size();
                    event.replacementShortcuts = completion->replacementShortcuts.size();
                }
                urlIndex = urlMessages.size();
                urlMessages.push_back(std::move(event));
            }
            catch (...) { urlObservationFailed = true; urlIndex = (std::numeric_limits<std::size_t>::max)(); }
            SetLastError(savedError);
        }
        const auto result = control ? DesktopApp::ControlWndProc(window, message, wp, lp)
            : DesktopApp::WndProc(window, message, wp, lp);
        messageTrace->Return(token, result);
        if (urlIndex != (std::numeric_limits<std::size_t>::max)())
        {
            if (urlIndex >= urlMessages.size()) urlObservationFailed = true;
            else urlMessages[urlIndex].returned = true;
        }
        if (refreshIndex != (std::numeric_limits<std::size_t>::max)())
        {
            if (!asyncOwner || refreshIndex >= refreshMessages.size()) refreshObservationFailed = true;
            else
            {
                auto& event = refreshMessages[refreshIndex];
                event.afterRevision = asyncOwner->shellRefreshRevision_.Current();
                event.afterRunning = asyncOwner->shellRefreshRevision_.Running();
                event.afterReady = static_cast<bool>(asyncOwner->readyShellRefresh_);
                event.returned = true;
            }
        }
        return result;
    }

    static LRESULT CALLBACK FixtureControlProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (asyncOwner && window == fixtureControlWindow &&
            ((message == WM_TIMER && wp == kLayoutScrollSaveTimerId) ||
             message == kBackgroundShellReadyMessage || message == kShellFileOperationCompletedMessage ||
             message == kUrlDropDownloadCompletedMessage))
        {
            try
            {
                if (message == WM_TIMER) ++layoutSaveTimerMessages;
                else if (message == kBackgroundShellReadyMessage) ++controlBackgroundMessages;
                else if (message == kShellFileOperationCompletedMessage)
                {
                    ++fileCompletionMessages;
                    const auto* completion = reinterpret_cast<const snowdesktop::ShellFileOperationUiCompletion*>(lp);
                    if (!completion) throw std::runtime_error("control completion message has no owned result");
                    if (completion->fileOperation) ++commitCompletionMessages; else ++sourceCompletionMessages;
                }
                return DispatchProduction(window, message, wp, lp, true);
            }
            catch (...) { asyncException = std::current_exception(); return 0; }
        }
        return previousControlProc ? CallWindowProcW(previousControlProc, window, message, wp, lp)
            : DefWindowProcW(window, message, wp, lp);
    }

    static bool IsPrivatePointerMessage(UINT message)
    {
        switch (message)
        {
        case WM_MOUSEMOVE: case WM_MOUSELEAVE: case WM_MOUSEWHEEL: case WM_MOUSEHWHEEL:
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK:
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK:
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK:
        case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        case WM_CAPTURECHANGED: case WM_CANCELMODE: case WM_SETFOCUS: case WM_KILLFOCUS:
        case WM_KEYDOWN: case WM_KEYUP: case WM_SYSKEYDOWN: case WM_SYSKEYUP: case WM_CHAR: case WM_SYSCHAR:
            return true;
        default: return false;
        }
    }
    static LRESULT CALLBACK AsyncWindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (asyncOwner && window == completionWindow && nativePointerDispatch && productionDispatch == 1 &&
            snowdesktop::private_pointer::IsDispatchingVirtualInput() && IsPrivatePointerMessage(message))
        {
            try { return DispatchProduction(window, message, wp, lp, false); }
            catch (...) { asyncException = std::current_exception(); return 0; }
        }
        if (asyncOwner && window == completionWindow &&
            (message == kBackgroundShellReadyMessage || message == kShellFileOperationCompletedMessage ||
                message == kUrlDropDownloadCompletedMessage ||
                (message == WM_TIMER && (wp == kShellChangeTimerId || wp == kExternalOleDragLeaveGraceTimerId ||
                    wp == kDesktopHostWatchTimerId))))
        {
            try
            {
                if (productionDispatch && (message == kBackgroundShellReadyMessage ||
                    message == kShellFileOperationCompletedMessage || message == kUrlDropDownloadCompletedMessage ||
                    (message == WM_TIMER && (wp == kExternalOleDragLeaveGraceTimerId || wp == kShellChangeTimerId))))
                {
                    if (message == WM_TIMER)
                    {
                        if (wp == kExternalOleDragLeaveGraceTimerId) ++graceTimerMessages;
                        else ++asyncShellTimers;
                    }
                    else if (message == kBackgroundShellReadyMessage) ++asyncReadMessages;
                    else if (message == kShellFileOperationCompletedMessage)
                    {
                        ++fileCompletionMessages;
                        const auto* completion = reinterpret_cast<const snowdesktop::ShellFileOperationUiCompletion*>(lp);
                        if (!completion) throw std::runtime_error("production completion message has no owned result");
                        if (completion->fileOperation) ++commitCompletionMessages; else ++sourceCompletionMessages;
                    }
                    return DispatchProduction(window, message, wp, lp, productionDispatch == 2);
                }
                if (message == WM_TIMER && wp == kDesktopHostWatchTimerId)
                {
                    // KillTimer does not remove an already queued WM_TIMER.
                    if (fixtureMaintenanceDisabled) return 0;
                    ++maintenanceTimerMessages;
                    // Production OnTimer invokes this same guarded service.
                    // The remaining desktop recovery/watch services require
                    // an initialized host and stay outside this fixture.
                    asyncOwner->DrainBackgroundShellWork();
                    return 0;
                }
                if (message == kBackgroundShellReadyMessage)
                {
                    ++asyncReadMessages;
                    // Keep the production read/delivery services while the
                    // isolated fixture supplies its own running-window input.
                    // The full dispatcher also starts global window capture,
                    // which this message-only owner does not initialize.
                    asyncOwner->PollInitialShellRead();
                    asyncOwner->DrainBackgroundShellWork();
                    return 0;
                }
                else if (message == WM_TIMER && wp == kExternalOleDragLeaveGraceTimerId) ++graceTimerMessages;
                else if (message == WM_TIMER) ++asyncShellTimers;
                else ++fileCompletionMessages;
                return asyncOwner->HandleMessage(window, message, wp, lp);
            }
            catch (...) { asyncException = std::current_exception(); return 0; }
        }
        return previousWindowProc ? CallWindowProcW(previousWindowProc, window, message, wp, lp)
            : DefWindowProcW(window, message, wp, lp);
    }
public:
    static void BindAsync(DesktopApp& app)
    {
        if (asyncOwner || app.hwnd_ != completionWindow || !IsWindow(completionWindow))
            throw std::runtime_error("async native fixture must exclusively own its message-only window");
        // Keep real model reads, timers, delivery and reload. Pixel providers
        // and global clipboard/cache services are outside this contract.
        app.iconWork_.Stop();
        for (const auto* clsid : {kDesktopIconClsidThisPC, kDesktopIconClsidUserFiles, kDesktopIconClsidNetwork,
                kDesktopIconClsidControlPanel, kDesktopIconClsidRecycleBin, kDesktopIconClsidApplications})
            app.settingsIconVisibility_[ToUpperInvariant(clsid)] = false;
        for (const auto& entry : snowdesktop::LoadDesktopNamespaceRegistrations())
            app.settingsIconVisibility_[ToUpperInvariant(entry.clsid)] = false;
        asyncException = {};
        messageTrace = std::make_unique<drag_global::ole_driver::MessageTrace>(kShellFileOperationCompletedMessage);
        asyncReadMessages = 0;
        asyncShellTimers = 0;
        fileCompletionMessages = 0;
        graceTimerMessages = 0;
        maintenanceTimerMessages = 0;
        sourceCompletionMessages = 0;
        commitCompletionMessages = 0;
        layoutSaveTimerMessages = 0;
        controlBackgroundMessages = 0;
        urlMessages.clear(); urlObservationFailed = false;
        refreshMessages.clear(); refreshObservationFailed = false;
        productionDispatch = 0;
        nativePointerDispatch = false;
        fixtureMaintenanceDisabled = false;
        previousWindowProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(completionWindow,
            GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&AsyncWindowProc)));
        if (!previousWindowProc) throw std::runtime_error("install the isolated native completion dispatcher");
        asyncOwner = &app;
        if (!SetTimer(completionWindow, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr))
            throw std::runtime_error("start the native fixture's production-period background delivery timer");
    }
    static void EnableProductionDispatch(DesktopApp& app, bool control)
    {
        if (fixtureTeardownIncomplete || fixtureStopInProgress || asyncOwner != &app || productionDispatch) throw std::runtime_error("dispatch fixture requires one bound host");
        SetLastError(ERROR_SUCCESS);
        previousWindowData = SetWindowLongPtrW(completionWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app));
        if (!previousWindowData && GetLastError() != ERROR_SUCCESS) throw std::runtime_error("bind host to owned message window");
        productionDispatch = control ? 2 : 1;
        if (control) app.controlHwnd_ = completionWindow;
    }
    static void DisableFixtureMaintenance(DesktopApp& app)
    {
        if (asyncOwner != &app || !completionWindow || fixtureMaintenanceDisabled ||
            !KillTimer(completionWindow, kDesktopHostWatchTimerId))
            throw std::runtime_error("disable isolated fixture maintenance timer");
        fixtureMaintenanceDisabled = true;
    }
    static void EnableNativePointerDispatch(DesktopApp& app)
    {
        DWORD process = 0;
        if (fixtureTeardownIncomplete || fixtureStopInProgress || asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            nativePointerDispatch || snowdesktop::private_pointer::Active() ||
            GetWindowThreadProcessId(completionWindow, &process) != GetCurrentThreadId() || process != GetCurrentProcessId())
            throw std::runtime_error("native input requires the bound production desktop window on its owning thread");
        nativePointerDispatch = true;
    }
    static void EnableFixtureControlWindow(DesktopApp& app)
    {
        if (fixtureTeardownIncomplete || fixtureStopInProgress || asyncOwner != &app || productionDispatch != 1 || fixtureControlWindow || app.controlHwnd_)
            throw std::runtime_error("control fixture requires a separate bound desktop owner");
        fixtureControlWindow = CreateWindowExW(0, L"STATIC", L"Behavior control", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!fixtureControlWindow) throw std::runtime_error("create isolated control message window");
        SetLastError(ERROR_SUCCESS);
        if (!SetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&app)) &&
            GetLastError() != ERROR_SUCCESS) throw std::runtime_error("bind control owner");
        SetLastError(ERROR_SUCCESS);
        previousControlProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(fixtureControlWindow,
            GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FixtureControlProc)));
        if (!previousControlProc && GetLastError() != ERROR_SUCCESS)
            throw std::runtime_error("install isolated control dispatcher");
        app.controlHwnd_ = fixtureControlWindow;
    }
    static void EnsureUnbindAllowed(DesktopApp& app)
    {
        if (asyncOwner != &app) return;
        if (!messageTrace || messageTrace->OwnerThread() != GetCurrentThreadId() || productionDispatchDepth != 0)
            throw std::runtime_error("cannot unbind a cross-thread or active production message dispatch");
        if (auto epoch = v3HostEpoch.lock(); epoch && !epoch->CanInvalidate())
            throw std::runtime_error("cannot unbind an active v3 host execution");
        if (auto epoch = nativeHostEpoch.lock(); epoch && !epoch->CanInvalidate())
            throw std::runtime_error("cannot unbind an active native host execution");
        if (v4UnicodeSourceOwner && !v4UnicodeSourceOwner->CanInvalidate())
            throw std::runtime_error("cannot unbind an active v4 source execution");
        if (v4IndependentShellOwner && !v4IndependentShellOwner->CanInvalidate())
            throw std::runtime_error("cannot unbind an active v4 Shell source execution");
        if (v4DirectoryShellOwner && !v4DirectoryShellOwner->CanInvalidate())
            throw std::runtime_error("cannot unbind an active v4 directory source execution");
        if (v4MixedOwner && (!v4MixedOwner->CanInvalidate() || !v4IndependentShellOwner))
            throw std::runtime_error("cannot unbind an active or incomplete v4 mixed source owner");
        if (v4ConfiguredContentOwner)
        {
            using Activity = snowdesktop::ShellFileOperationWorker::ObservedActivity;
            if (!v4ConfiguredContentOwner->CanInvalidate() || !FileDropIdle(app) ||
                app.externalSlotReadWorker_.TryObserveActivity() != Activity::idle ||
                app.shellFileOperationWorker_.TryObserveActivity() != Activity::idle)
                throw std::runtime_error("cannot release a v4 content source while its execution or file work is unfinished or unavailable");
        }
        if (v4IndependentShellOwner && v4IndependentShellOwner->source_)
        {
            using Activity = snowdesktop::ShellFileOperationWorker::ObservedActivity;
            const auto* source = v4IndependentShellOwner->source_.Get();
            if (source->starts != source->ends || !FileDropIdle(app) ||
                app.externalSlotReadWorker_.TryObserveActivity() != Activity::idle ||
                app.shellFileOperationWorker_.TryObserveActivity() != Activity::idle)
                throw std::runtime_error("cannot release a v4 Shell source while its file work is unfinished or unavailable");
        }
        if (v4DirectoryShellOwner && v4DirectoryShellOwner->source_)
        {
            using Activity = snowdesktop::ShellFileOperationWorker::ObservedActivity;
            const auto* source = v4DirectoryShellOwner->source_.Get();
            if (source->starts != source->ends || !FileDropIdle(app) ||
                app.externalSlotReadWorker_.TryObserveActivity() != Activity::idle ||
                app.shellFileOperationWorker_.TryObserveActivity() != Activity::idle)
                throw std::runtime_error("cannot release a v4 directory source while its file work is unfinished or unavailable");
        }
    }
private:
    static void UnbindAsync(DesktopApp& app)
    {
        if (asyncOwner != &app) return;
        EnsureUnbindAllowed(app);
        fixtureTeardownIncomplete = true;
        if (auto epoch = v3HostEpoch.lock()) epoch->Invalidate();
        v3HostEpoch.reset();
        if (auto epoch = nativeHostEpoch.lock()) epoch->Invalidate();
        nativeHostEpoch.reset();
        const auto restore = [](HWND window, int field, LONG_PTR value) {
            SetLastError(ERROR_SUCCESS);
            const auto previous = SetWindowLongPtrW(window, field, value);
            if (!previous && GetLastError() != ERROR_SUCCESS)
                throw std::runtime_error("restore owned fixture window binding failed");
        };
        KillTimer(completionWindow, kShellChangeTimerId);
        KillTimer(completionWindow, kExternalOleDragLeaveGraceTimerId);
        KillTimer(completionWindow, kDesktopHostWatchTimerId);
        if (fixtureControlWindow)
        {
            KillTimer(fixtureControlWindow, kLayoutScrollSaveTimerId);
            if (previousControlProc) restore(fixtureControlWindow, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(previousControlProc));
            restore(fixtureControlWindow, GWLP_USERDATA, 0);
            if (IsWindow(fixtureControlWindow) && !DestroyWindow(fixtureControlWindow) && IsWindow(fixtureControlWindow))
                throw std::runtime_error("owned control window could not be destroyed");
            app.controlHwnd_ = nullptr;
            fixtureControlWindow = nullptr;
            previousControlProc = nullptr;
        }
        // Remove the fixture dispatcher before changing its bound app pointer.
        restore(completionWindow, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(previousWindowProc));
        if (productionDispatch) restore(completionWindow, GWLP_USERDATA, previousWindowData);
        productionDispatch = 0;
        nativePointerDispatch = false;
        fixtureMaintenanceDisabled = false;
        previousWindowData = 0;
        previousWindowProc = nullptr;
        asyncOwner = nullptr;
        messageTrace.reset();
        fixtureTeardownIncomplete = false;
    }
public:
    static void RequestAsync(DesktopApp& app, bool disk = false) { app.ReloadItems(disk); }
    static bool AsyncIdle(const DesktopApp& app)
    {
        return app.desktopItemsReady_ && !app.shellReloadPending_ && !app.shellRefreshRevision_.Running() &&
            !app.readyShellRefresh_ && !app.layoutReload_.Pending();
    }
    static ComPtr<OleDragDropAdapter> ExternalAdapter(DesktopApp& app)
    {
        if (fixtureTeardownIncomplete || fixtureStopInProgress || !nativeHostEpoch.expired())
            throw drag_global::ole_driver::ObservationUnavailable("adapter unavailable during teardown or a native host epoch");
        ComPtr<OleDragDropAdapter> adapter;
        adapter.Attach(new OleDragDropAdapter(&app));
        return adapter;
    }
    // Create the adapter here so that its production handler and the epoch
    // necessarily refer to the same bound app; never accept a caller-provided
    // adapter as evidence of that relationship.
    static std::pair<ComPtr<OleDragDropAdapter>, std::shared_ptr<drag_global::ole_driver::V3HostEpoch>>
    CreateV3AdapterAndHostEpoch(DesktopApp& app, IDataObject* source)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        DWORD desktopProcess = 0, controlProcess = 0;
        if (!source || fixtureTeardownIncomplete || fixtureStopInProgress || !v3HostEpoch.expired() ||
            !nativeHostEpoch.expired() || asyncOwner != &app || productionDispatch != 1 ||
            !profileReserved.load(std::memory_order_acquire) || !messageTrace ||
            messageTrace->OwnerThread() != GetCurrentThreadId() || messageTrace->LostOrCrossThread() ||
            !completionWindow || !fixtureControlWindow || completionWindow == fixtureControlWindow ||
            app.hwnd_ != completionWindow || app.controlHwnd_ != fixtureControlWindow ||
            !IsWindow(completionWindow) || !IsWindow(fixtureControlWindow) ||
            GetWindowThreadProcessId(completionWindow, &desktopProcess) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(fixtureControlWindow, &controlProcess) != GetCurrentThreadId() ||
            desktopProcess != GetCurrentProcessId() || controlProcess != GetCurrentProcessId() ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(completionWindow, GWLP_USERDATA)) != &app ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA)) != &app ||
            GetWindowLongPtrW(completionWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&AsyncWindowProc) ||
            GetWindowLongPtrW(fixtureControlWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&FixtureControlProc))
            throw ObservationUnavailable("v3 host epoch window, owner, trace or source binding unavailable");
        auto adapter = ExternalAdapter(app);
        if (!adapter) throw ObservationUnavailable("v3 host epoch adapter unavailable");
        auto epoch = std::shared_ptr<drag_global::ole_driver::V3HostEpoch>(
            new drag_global::ole_driver::V3HostEpoch(*messageTrace, completionWindow, fixtureControlWindow,
                &app, adapter.Get(), source));
        v3HostEpoch = epoch;
        return {std::move(adapter), std::move(epoch)};
    }
    static std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> CreateNativeHostEpoch(DesktopApp& app)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        DWORD desktopProcess = 0, controlProcess = 0;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (fixtureTeardownIncomplete || fixtureStopInProgress || !nativeHostEpoch.expired() || !v3HostEpoch.expired() ||
            v4UnicodeSourceOwner || v4IndependentShellOwner || v4DirectoryShellOwner || v4MixedOwner || v4ConfiguredContentOwner || asyncOwner != &app ||
            productionDispatch != 1 || !nativePointerDispatch || !profileReserved.load(std::memory_order_acquire) ||
            !messageTrace || messageTrace->OwnerThread() != GetCurrentThreadId() || messageTrace->LostOrCrossThread() ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) || (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            !completionWindow || !fixtureControlWindow || completionWindow == fixtureControlWindow ||
            app.hwnd_ != completionWindow || app.controlHwnd_ != fixtureControlWindow ||
            !IsWindow(completionWindow) || !IsWindow(fixtureControlWindow) ||
            GetWindowThreadProcessId(completionWindow, &desktopProcess) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(fixtureControlWindow, &controlProcess) != GetCurrentThreadId() ||
            desktopProcess != GetCurrentProcessId() || controlProcess != GetCurrentProcessId() ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(completionWindow, GWLP_USERDATA)) != &app ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA)) != &app ||
            GetWindowLongPtrW(completionWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&AsyncWindowProc) ||
            GetWindowLongPtrW(fixtureControlWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&FixtureControlProc))
            throw ObservationUnavailable("native host epoch requires its isolated device, windows and live STA");
        auto epoch = std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>(
            new drag_global::ole_driver::NativeHostEpoch(*messageTrace, completionWindow, fixtureControlWindow, &app));
        nativeHostEpoch = epoch;
        return epoch;
    }
    static bool SameNativeHostEpoch(const DesktopApp& app,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        return epoch && nativeHostEpoch.lock() == epoch && asyncOwner == &app && nativePointerDispatch &&
            !fixtureTeardownIncomplete && !fixtureStopInProgress && messageTrace &&
            epoch->Matches(messageTrace.get(), completionWindow, fixtureControlWindow);
    }
    static std::shared_ptr<drag_global::V4CategoryObservationSource> CreateV4CategoryObservationSource(
        std::shared_ptr<behavior::OwnedProfileDesktopRoot> root,
        std::shared_ptr<drag_global::V4IndependentFilePayload> payload,
        std::shared_ptr<const drag_global::event_graph::V4FamilyIdentityContext> identity,
        std::shared_ptr<const drag_global::V4FamilyValidatedContext> verified,
        std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> epoch);
    // The fixture owns the COM source on the same live STA as the profile;
    // bindings borrow the owner and become invalid before teardown.
    static std::shared_ptr<drag_global::ole_driver::V4UnicodeSourceOwner>
    CreateV4UnicodeSourceOwner(DesktopApp& app)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        DWORD desktopProcess = 0, controlProcess = 0;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (fixtureTeardownIncomplete || fixtureStopInProgress || v4UnicodeSourceOwner || v4IndependentShellOwner || v4DirectoryShellOwner || v4MixedOwner || v4ConfiguredContentOwner ||
            !v3HostEpoch.expired() || !nativeHostEpoch.expired() || asyncOwner != &app ||
            !profileReserved.load(std::memory_order_acquire) || productionDispatch != 1 || !messageTrace ||
            messageTrace->OwnerThread() != GetCurrentThreadId() || messageTrace->LostOrCrossThread() ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
            (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            !completionWindow || !fixtureControlWindow || completionWindow == fixtureControlWindow ||
            app.hwnd_ != completionWindow || app.controlHwnd_ != fixtureControlWindow ||
            !IsWindow(completionWindow) || !IsWindow(fixtureControlWindow) ||
            GetWindowThreadProcessId(completionWindow, &desktopProcess) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(fixtureControlWindow, &controlProcess) != GetCurrentThreadId() ||
            desktopProcess != GetCurrentProcessId() || controlProcess != GetCurrentProcessId() ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(completionWindow, GWLP_USERDATA)) != &app ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA)) != &app ||
            GetWindowLongPtrW(completionWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&AsyncWindowProc) ||
            GetWindowLongPtrW(fixtureControlWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&FixtureControlProc))
            throw ObservationUnavailable("v4 Unicode source owner requires the isolated live STA host");
        auto owner = std::shared_ptr<drag_global::ole_driver::V4UnicodeSourceOwner>(
            new drag_global::ole_driver::V4UnicodeSourceOwner);
        v4UnicodeSourceOwner = owner;
        return owner;
    }
    static std::shared_ptr<drag_global::V4IndependentShellOwner> CreateV4IndependentShellOwner(DesktopApp& app)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        DWORD desktopProcess = 0, controlProcess = 0;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (fixtureTeardownIncomplete || fixtureStopInProgress || v4UnicodeSourceOwner || v4IndependentShellOwner || v4DirectoryShellOwner || v4MixedOwner || v4ConfiguredContentOwner ||
            !v3HostEpoch.expired() || !nativeHostEpoch.expired() || asyncOwner != &app ||
            !profileReserved.load(std::memory_order_acquire) || productionDispatch != 1 || !messageTrace ||
            messageTrace->OwnerThread() != GetCurrentThreadId() || messageTrace->LostOrCrossThread() ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
            (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            !completionWindow || !fixtureControlWindow || completionWindow == fixtureControlWindow ||
            app.hwnd_ != completionWindow || app.controlHwnd_ != fixtureControlWindow ||
            !IsWindow(completionWindow) || !IsWindow(fixtureControlWindow) ||
            GetWindowThreadProcessId(completionWindow, &desktopProcess) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(fixtureControlWindow, &controlProcess) != GetCurrentThreadId() ||
            desktopProcess != GetCurrentProcessId() || controlProcess != GetCurrentProcessId() ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(completionWindow, GWLP_USERDATA)) != &app ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA)) != &app ||
            GetWindowLongPtrW(completionWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&AsyncWindowProc) ||
            GetWindowLongPtrW(fixtureControlWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&FixtureControlProc))
            throw ObservationUnavailable("v4 Shell source owner requires the isolated live STA host");
        auto owner = std::shared_ptr<drag_global::V4IndependentShellOwner>(new drag_global::V4IndependentShellOwner);
        v4IndependentShellOwner = owner;
        return owner;
    }
    static std::shared_ptr<drag_global::directory_fixture::DirectoryShellOwner> CreateV4DirectoryShellOwner(DesktopApp& app)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        DWORD desktopProcess = 0, controlProcess = 0;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (fixtureTeardownIncomplete || fixtureStopInProgress || v4UnicodeSourceOwner || v4IndependentShellOwner || v4DirectoryShellOwner || v4MixedOwner || v4ConfiguredContentOwner ||
            !v3HostEpoch.expired() || !nativeHostEpoch.expired() || asyncOwner != &app ||
            !profileReserved.load(std::memory_order_acquire) || productionDispatch != 1 || !messageTrace ||
            messageTrace->OwnerThread() != GetCurrentThreadId() || messageTrace->LostOrCrossThread() ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
            (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            !completionWindow || !fixtureControlWindow || completionWindow == fixtureControlWindow ||
            app.hwnd_ != completionWindow || app.controlHwnd_ != fixtureControlWindow ||
            !IsWindow(completionWindow) || !IsWindow(fixtureControlWindow) ||
            GetWindowThreadProcessId(completionWindow, &desktopProcess) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(fixtureControlWindow, &controlProcess) != GetCurrentThreadId() ||
            desktopProcess != GetCurrentProcessId() || controlProcess != GetCurrentProcessId() ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(completionWindow, GWLP_USERDATA)) != &app ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA)) != &app ||
            GetWindowLongPtrW(completionWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&AsyncWindowProc) ||
            GetWindowLongPtrW(fixtureControlWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&FixtureControlProc))
            throw ObservationUnavailable("v4 directory source owner requires the isolated live STA host");
        auto owner = std::shared_ptr<drag_global::directory_fixture::DirectoryShellOwner>(
            new drag_global::directory_fixture::DirectoryShellOwner);
        v4DirectoryShellOwner = owner;
        return owner;
    }
    static std::shared_ptr<drag_global::mixed_fixture::MixedOwner> CreateV4MixedOwner(DesktopApp& app)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        DWORD desktopProcess = 0, controlProcess = 0;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (fixtureTeardownIncomplete || fixtureStopInProgress || v4UnicodeSourceOwner || v4DirectoryShellOwner || v4MixedOwner || v4ConfiguredContentOwner ||
            !v4IndependentShellOwner || !v4IndependentShellOwner->CanInvalidate() ||
            !v3HostEpoch.expired() || !nativeHostEpoch.expired() || asyncOwner != &app ||
            !profileReserved.load(std::memory_order_acquire) || productionDispatch != 1 || !messageTrace ||
            messageTrace->OwnerThread() != GetCurrentThreadId() || messageTrace->LostOrCrossThread() ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
            (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            !completionWindow || !fixtureControlWindow || completionWindow == fixtureControlWindow ||
            app.hwnd_ != completionWindow || app.controlHwnd_ != fixtureControlWindow ||
            !IsWindow(completionWindow) || !IsWindow(fixtureControlWindow) ||
            GetWindowThreadProcessId(completionWindow, &desktopProcess) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(fixtureControlWindow, &controlProcess) != GetCurrentThreadId() ||
            desktopProcess != GetCurrentProcessId() || controlProcess != GetCurrentProcessId() ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(completionWindow, GWLP_USERDATA)) != &app ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA)) != &app ||
            GetWindowLongPtrW(completionWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&AsyncWindowProc) ||
            GetWindowLongPtrW(fixtureControlWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&FixtureControlProc))
            throw ObservationUnavailable("v4 mixed source owner requires its isolated live STA file owner and host");
        auto owner = std::shared_ptr<drag_global::mixed_fixture::MixedOwner>(new drag_global::mixed_fixture::MixedOwner);
        v4MixedOwner = owner;
        return owner;
    }
    static std::shared_ptr<drag_global::mixed_fixture::ConfiguredContentOwner> CreateV4ConfiguredContentOwner(DesktopApp& app)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        DWORD desktopProcess = 0, controlProcess = 0;
        APTTYPE apartment{}; APTTYPEQUALIFIER qualifier{};
        if (fixtureTeardownIncomplete || fixtureStopInProgress || v4UnicodeSourceOwner || v4IndependentShellOwner ||
            v4DirectoryShellOwner || v4MixedOwner || v4ConfiguredContentOwner ||
            !v3HostEpoch.expired() || !nativeHostEpoch.expired() || asyncOwner != &app ||
            !profileReserved.load(std::memory_order_acquire) || productionDispatch != 1 || !messageTrace ||
            messageTrace->OwnerThread() != GetCurrentThreadId() || messageTrace->LostOrCrossThread() ||
            FAILED(CoGetApartmentType(&apartment, &qualifier)) ||
            (apartment != APTTYPE_STA && apartment != APTTYPE_MAINSTA) ||
            !completionWindow || !fixtureControlWindow || completionWindow == fixtureControlWindow ||
            app.hwnd_ != completionWindow || app.controlHwnd_ != fixtureControlWindow ||
            !IsWindow(completionWindow) || !IsWindow(fixtureControlWindow) ||
            GetWindowThreadProcessId(completionWindow, &desktopProcess) != GetCurrentThreadId() ||
            GetWindowThreadProcessId(fixtureControlWindow, &controlProcess) != GetCurrentThreadId() ||
            desktopProcess != GetCurrentProcessId() || controlProcess != GetCurrentProcessId() ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(completionWindow, GWLP_USERDATA)) != &app ||
            reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(fixtureControlWindow, GWLP_USERDATA)) != &app ||
            GetWindowLongPtrW(completionWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&AsyncWindowProc) ||
            GetWindowLongPtrW(fixtureControlWindow, GWLP_WNDPROC) != reinterpret_cast<LONG_PTR>(&FixtureControlProc))
            throw ObservationUnavailable("v4 configured content owner requires the isolated live STA host");
        auto owner = std::shared_ptr<drag_global::mixed_fixture::ConfiguredContentOwner>(
            new drag_global::mixed_fixture::ConfiguredContentOwner);
        v4ConfiguredContentOwner = owner;
        return owner;
    }
    static POINTL ExternalPoint(POINT point)
    {
        const POINT original = point;
        if (!ClientToScreen(completionWindow, &point))
            throw std::runtime_error("convert native OLE input through the owned message-only window");
        POINT roundTrip = point;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != original.x || roundTrip.y != original.y)
            throw std::runtime_error("owned message-window coordinate conversion must round trip");
        return {point.x, point.y};
    }
    static bool FileDropIdle(const DesktopApp& app)
    {
        return AsyncIdle(app) && app.shellFileOperationInFlight_ == 0 && app.pendingLandingCaches_.empty();
    }
    static bool ExternalLeaveIdle(const DesktopApp& app)
    {
        return !app.externalOleDragLeavePending_ && !app.dragDropController_.IsExternalDragActive();
    }
    static bool LayoutSaveIdle(const DesktopApp& app)
    {
        return !app.layoutSavePending_ && !app.layoutScrollSave_.Pending();
    }
    struct FileWorkersObservation
    {
        snowdesktop::ShellFileOperationWorker::ObservedActivity read, file;
        std::uintptr_t readIdentity = 0, fileIdentity = 0;
    };
    static FileWorkersObservation ObserveFileWorkers(DesktopApp& app)
    {
        // Instantaneous only: idle cannot exclude a later enqueue. Numeric
        // identities are compared with copied log values, never dereferenced.
        return {app.externalSlotReadWorker_.TryObserveActivity(), app.shellFileOperationWorker_.TryObserveActivity(),
            reinterpret_cast<std::uintptr_t>(&app.externalSlotReadWorker_),
            reinterpret_cast<std::uintptr_t>(&app.shellFileOperationWorker_)};
    }
    static std::uint64_t LayoutRevision(const DesktopApp& app) { return app.layoutSaveRevision_; }
    static std::uint64_t ShellRefreshRevision(const DesktopApp& app) { return app.shellRefreshRevision_.Current(); }
    static void ThrowAsyncException()
    {
        if (wrongThreadShutdown.load()) throw std::runtime_error("profile shutdown attempted on a different thread");
        if (asyncException) std::rethrow_exception(asyncException);
    }
    static DesktopApp& Scene(bool secondary)
    {
        auto& app = *new DesktopApp;
        GridPage page;
        page.id = L"native-page";
        page.monitorId = L"native-monitor";
        page.bounds = secondary ? RECT{-800, 0, -400, 300} : RECT{0, 0, 400, 300};
        page.workArea = page.visualWorkArea = page.bounds;
        page.columns = 4; page.rows = 3;
        page.cellWidth = page.cellHeight = 80;
        page.itemPitchWidth = page.itemPitchHeight = 100;
        page.gapX = page.gapY = 20;
        page.marginX = page.marginY = 10;
        app.gridPages_.push_back(page);
        for (int i = 0; i < 2; ++i)
        {
            DesktopItem item;
            item.name = i ? L"B" : L"A";
            item.layoutKey = item.parsingName = item.name;
            item.gridCell = {page.id, i * 2, 0};
            item.gridSpan = {1, 1};
            item.selected = true;
            item.bounds = {page.bounds.left + 10 + i * 200, 10,
                page.bounds.left + 90 + i * 200, 90};
            app.items_.push_back(std::move(item));
        }
        app.containers_.push_back(std::make_unique<DesktopGrid>(&app.gridPages_, &app.items_, &app));
        for (auto& item : app.items_)
            app.items_oo_.push_back(std::make_unique<DesktopIcon>(&item, app.containers_.front().get(), &app));
        return app;
    }
    static void ConfigureModel(DesktopApp& app, const std::filesystem::path& layout)
    {
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        app.hwnd_ = completionWindow;
        app.initializationExperimentDirectory_ = layout;
        app.generalSettings_.dockEnabled = false;
        app.generalSettings_.statusBar.enabled = false;
        app.categorySettings_.collectProgramsEnabled = true;
        app.gridPages_.front().id = L"__page:1";
        app.savedPageIds_ = {L"__page:1"};
        app.savedPageColumns_[L"__page:1"] = 4;
        app.savedPageRows_[L"__page:1"] = 3;
        app.savedPageNames_[L"__page:1"] = L"Model page";
        app.lastMonitorPageId_.clear();
    }
    static DesktopItem FileItem(const std::filesystem::path& path)
    {
        DesktopItem item;
        item.name = path.filename().wstring();
        item.parsingName = path.wstring();
        item.layoutKey = ToUpperInvariant(path.wstring());
        item.gridSpan = {1, 1};
        item.iconBitmap = CreateBitmap(512, 512, 1, 32, nullptr);
        if (!item.iconBitmap) throw std::runtime_error("native fixture bitmap allocation failed");
        item.iconBitmapSize = {512, 512};
        item.iconState = IconState::FullQuality;
        return item;
    }
    static void PopulateModel(DesktopApp& app, const std::vector<std::filesystem::path>& files, int source)
    {
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            auto item = FileItem(files[i]);
            item.gridCell = {L"__page:1", static_cast<int>(i) * 2, 0};
            item.selected = true;
            app.items_.push_back(std::move(item));
        }
        if (source)
        {
            DesktopWidget widget;
            widget.id = L"native-origin";
            widget.type = source == 1 ? DesktopWidgetType::Collection : DesktopWidgetType::FileCategories;
            widget.title = L"Origin";
            widget.gridCell = {L"__page:1", 0, 0};
            widget.gridSpan = {1, 1};
            widget.autoCollect = false;
            for (const auto& item : app.items_) widget.itemKeys.push_back(item.layoutKey);
            app.widgets_.push_back(std::move(widget));
        }
        app.desktopItemsReady_ = true;
        app.RefreshDesktopItemIndexCache();
        app.LayoutItems();
    }
    // Only establishes an isolated initial state. Selection, drag start and
    // ownership changes must come from subsequent production pointer input.
    static void PrepareExpandedNativeSource(DesktopApp& app,
        const std::vector<std::filesystem::path>& files, int source)
    {
        if (files.empty() || (source != 1 && source != 2) || app.gridPages_.empty() ||
            !NativeDragIdle(app) || !ContainerWorkIdle(app))
            throw drag_global::ole_driver::ObservationUnavailable("native member fixture initial conditions unavailable");
        ResetContainerScene(app);
        PopulateModel(app, files, source);
        auto found = std::find_if(app.widgets_.begin(), app.widgets_.end(),
            [](const auto& widget) { return widget.id == L"native-origin"; });
        if (found == app.widgets_.end())
            throw drag_global::ole_driver::ObservationUnavailable("native member fixture source missing");
        found->gridSpan = {4, 3}; found->singleCellIconMode = false;
        found->scrollContainerMode = source == 1; found->dateHeaders = false; found->showSearchBox = false;
        found->titleBarCollapsed = false; found->titleBarExpandOnHover = false;
        for (auto& item : app.items_) item.selected = false;
        app.LayoutItems();
    }
    // Prepare only an unselected isolated desktop. Subsequent selection and
    // drag transitions must be driven through the production window path.
    static void PrepareNativeMultiDesktop(DesktopApp& app, const std::vector<std::filesystem::path>& files)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (files.size() != 2 || app.gridPages_.empty() || !NativeDragIdle(app) || !ContainerWorkIdle(app) || !LayoutSaveIdle(app))
            throw ObservationUnavailable("native multi desktop initial conditions unavailable");
        std::error_code error;
        const bool same = std::filesystem::equivalent(files[0], files[1], error);
        if (error || same) throw ObservationUnavailable("native multi desktop distinct files unavailable");
        ResetContainerScene(app);
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear();
        for (std::size_t i = 0; i < files.size(); ++i)
        {
            auto item = FileItem(files[i]);
            if (item.selected) throw ObservationUnavailable("native multi desktop default selection unavailable");
            item.gridCell = {L"__page:1", static_cast<int>(i) * 2, 0};
            app.items_.push_back(std::move(item));
        }
        app.desktopItemsReady_ = true;
        app.RefreshDesktopItemIndexCache();
        app.LayoutItems();
        if (!app.SaveLayoutSlots(false)) throw ObservationUnavailable("native multi desktop initial layout save unavailable");
    }
    struct NativeMultiDesktopMember
    {
        std::filesystem::path path;
        std::wstring key, typeName, desktopIconClsid;
        FILE_ID_INFO fileId{};
        DWORD fileAttributes = 0;
        bool selected = false, isShortcut = false, isApplicationShortcut = false;
        RECT bounds{};
        GridCell cell;
    };
    struct NativeMultiDesktopSelection
    {
        std::uintptr_t host = 0;
        DWORD ownerThread = 0;
        std::uint64_t layoutRevision = 0, selectionRevision = 0;
        std::size_t actualItemCount = 0, widgetCount = 0;
        std::vector<NativeMultiDesktopMember> members;
        std::vector<std::wstring> extraItemKeys, extraSelectedItemKeys;
        std::wstring anchorScope, anchorKey;
        std::vector<std::wstring> selectedDockReferences, selectedRunningKeys, selectedWidgetIds;
        std::vector<std::pair<std::wstring, std::wstring>> selectedWidgetFolderEntries;
        std::vector<std::string> otherSurfaceReasons;
        bool otherSurfacesComplete = false;
    };
    static NativeMultiDesktopSelection InspectNativeMultiDesktopSelection(
        DesktopApp& app, const std::vector<std::filesystem::path>& files)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (files.size() != 2 || asyncOwner != &app || productionDispatch != 1 || !nativePointerDispatch ||
            !completionWindow || !IsWindow(completionWindow) || app.hwnd_ != completionWindow ||
            GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("native multi desktop observation host unavailable");
        NativeMultiDesktopSelection out;
        out.host = reinterpret_cast<std::uintptr_t>(&app); out.ownerThread = GetCurrentThreadId();
        out.layoutRevision = app.layoutSaveRevision_; out.selectionRevision = app.selectionController_.Revision();
        out.anchorScope = app.selectionController_.anchorScope_; out.anchorKey = app.selectionController_.anchorKey_;
        out.actualItemCount = app.items_.size(); out.widgetCount = app.widgets_.size();
        for (const auto& entry : app.dockEntries_) if (entry.selected) out.selectedDockReferences.push_back(entry.reference);
        for (const auto& entry : app.dockUnpinnedRunningApps_) if (entry.selected) out.selectedRunningKeys.push_back(entry.identityKey);
        for (const auto& widget : app.widgets_)
        {
            if (widget.selected) out.selectedWidgetIds.push_back(widget.id);
            for (const auto& entry : widget.folderEntries)
                if (entry.selected) out.selectedWidgetFolderEntries.emplace_back(widget.id, entry.fullPath);
        }
        // This is deliberately a narrow isolated-host observation. A live or
        // pending popup/search source can carry selection outside these lists.
        if (!app.widgets_.empty()) out.otherSurfaceReasons.emplace_back("widgets_present");
        if (!app.dockEntries_.empty() || !app.dockUnpinnedRunningApps_.empty()) out.otherSurfaceReasons.emplace_back("dock_entries_present");
        if (app.popupWidgetIndex_ != static_cast<std::size_t>(-1) || app.pendingCollectionPopupOpen_ || app.popupMouseDownItem_ ||
            app.dockFolderPopupOpen_ || app.dockFolderPopupContainer_ || app.dockTrayPopup_ ||
            app.dockFolderPopupDragSourceContainer_ || !app.dockFolderPopupDragItems_.empty() || !app.dockFolderPopupDragSourceItems_.empty())
            out.otherSurfaceReasons.emplace_back("popup_or_popup_drag_state_present");
        if (app.dockFolderPopupWidget_.selected || app.dockFolderPopupDragSourceWidget_.selected ||
            std::any_of(app.dockFolderPopupWidget_.folderEntries.begin(), app.dockFolderPopupWidget_.folderEntries.end(),
                [](const auto& entry) { return entry.selected; }) ||
            std::any_of(app.dockFolderPopupDragSourceWidget_.folderEntries.begin(), app.dockFolderPopupDragSourceWidget_.folderEntries.end(),
                [](const auto& entry) { return entry.selected; }))
            out.otherSurfaceReasons.emplace_back("popup_model_selection_present");
        std::size_t desktopContainers = 0;
        for (const auto& container : app.containers_)
        {
            if (!container) { out.otherSurfaceReasons.emplace_back("null_container"); continue; }
            if (dynamic_cast<const DesktopGrid*>(container.get())) ++desktopContainers;
            else out.otherSurfaceReasons.emplace_back("unverified_container_present");
            if (const auto* scrolling = dynamic_cast<const ScrollingItemWidget*>(container.get()); scrolling && scrolling->folderSearch_)
                out.otherSurfaceReasons.emplace_back("search_state_present");
        }
        if (desktopContainers != 1) out.otherSurfaceReasons.emplace_back("desktop_container_not_unique");
        out.otherSurfacesComplete = out.otherSurfaceReasons.empty();
        std::vector<const DesktopItem*> used;
        for (const auto& path : files)
        {
            const DesktopItem* found = nullptr;
            for (const auto& item : app.items_)
            {
                std::error_code error; const bool same = std::filesystem::equivalent(item.parsingName, path, error);
                if (error) throw ObservationUnavailable("native multi desktop member identity unavailable");
                if (!same) continue;
                if (found) throw ObservationUnavailable("native multi desktop member ambiguous");
                found = &item;
            }
            if (!found || std::find(used.begin(), used.end(), found) != used.end())
                throw ObservationUnavailable("native multi desktop member missing or duplicated");
            struct FileGuard { HANDLE handle = INVALID_HANDLE_VALUE; ~FileGuard() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); } } file;
            file.handle = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            if (file.handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file.handle, FileIdInfo, &id, sizeof(id)) ||
                !GetFileInformationByHandleEx(file.handle, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                std::all_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE value) { return value == 0; }))
                throw ObservationUnavailable("native multi desktop file ID unavailable");
            used.push_back(found);
            out.members.push_back({path, found->layoutKey, found->typeName, found->desktopIconClsid, id, tag.FileAttributes,
                found->selected, found->isShortcut, found->isApplicationShortcut, found->bounds, found->gridCell});
        }
        if (out.members[0].fileId.VolumeSerialNumber == out.members[1].fileId.VolumeSerialNumber &&
            std::memcmp(out.members[0].fileId.FileId.Identifier, out.members[1].fileId.FileId.Identifier,
                sizeof(out.members[0].fileId.FileId.Identifier)) == 0)
            throw ObservationUnavailable("native multi desktop file IDs are not distinct");
        for (const auto& item : app.items_)
            if (std::find(used.begin(), used.end(), &item) == used.end())
            { out.extraItemKeys.push_back(item.layoutKey); if (item.selected) out.extraSelectedItemKeys.push_back(item.layoutKey); }
        return out;
    }
    static bool NativeDragCaptured(const DesktopApp& app, const std::filesystem::path& source)
    {
        if (!app.dragSession_.IsActive() || app.dragSession_.Source() != Desktop(app) ||
            app.dragSession_.Items().size() != 1) return false;
        const auto paths = app.dragSession_.SourceList().FilePaths();
        if (paths.size() != 1) return false;
        std::error_code error;
        return std::filesystem::equivalent(std::filesystem::path(paths.front()), source, error) && !error;
    }
    static bool NativeMemberDragCaptured(DesktopApp& app, const std::filesystem::path& path)
    {
        if (!app.dragSession_.IsActive() || app.dragSession_.Source() != MemberContainer(app, L"native-origin") ||
            app.dragSession_.Items().size() != 1) return false;
        const auto paths = app.dragSession_.SourceList().FilePaths();
        if (paths.size() != 1) return false;
        std::error_code error;
        const bool same = std::filesystem::equivalent(std::filesystem::path(paths.front()), path, error);
        if (error) throw drag_global::ole_driver::ObservationUnavailable("native captured member identity unavailable");
        return same;
    }
    static bool NativeCategoryDragCaptured(DesktopApp& app, const std::filesystem::path& path,
        std::uintptr_t expectedContainer)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (!expectedContainer || !app.dragSession_.IsActive() || app.dragSession_.Items().size() != 1) return false;
        const auto* capturedSource = app.dragSession_.Source();
        FileCategories* source = nullptr;
        std::size_t matches = 0, liveMatches = 0;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<FileCategories*>(container.get());
            if (widget && widget->GetWidgetData() && widget->GetWidgetData()->id == L"native-origin") ++matches;
            if (container.get() == capturedSource)
            {
                ++liveMatches;
                source = widget;
            }
        }
        if (matches != 1 || liveMatches != 1 || !source || reinterpret_cast<std::uintptr_t>(source) != expectedContainer ||
            !source->GetWidgetData() || source->GetWidgetData()->id != L"native-origin" ||
            source->GetWidgetData()->type != DesktopWidgetType::FileCategories) return false;
        const auto paths = app.dragSession_.SourceList().FilePaths();
        if (paths.size() != 1) return false;
        std::error_code error;
        const bool same = std::filesystem::equivalent(std::filesystem::path(paths.front()), path, error);
        if (error) throw ObservationUnavailable("native category captured identity unavailable");
        return same;
    }
    static bool NativeDragIdle(const DesktopApp& app)
    {
        return !app.mouseDown_ && !app.dragSession_.IsActive() && !app.dragSession_.HasContext() &&
            !app.dragDropController_.IsTransportActive();
    }
    static HWND HostWindow(const DesktopApp& app) { return app.hwnd_; }
    static bool SessionActive(const DesktopApp& app) { return app.dragSession_.IsActive(); }
    static POINT SessionPoint(const DesktopApp& app) { return app.dragSession_.CurrentPoint(); }
    static Container* Target(const DesktopApp& app) { return app.dragSession_.TargetContainer(); }
    static Slot* TargetSlot(const DesktopApp& app) { return app.dragSession_.TargetSlot(); }
    static HitRegion Region(const DesktopApp& app) { return app.dragSession_.TargetRegion(); }
    static Container* Desktop(const DesktopApp& app) { return app.containers_.front().get(); }
    static Container* DesktopOrNull(const DesktopApp& app)
    { return app.containers_.empty() ? nullptr : app.containers_.front().get(); }
    static std::optional<std::size_t> ObserveFreeDesktopCopies(const DesktopApp& app, const std::filesystem::path& path)
    {
        if (app.containers_.empty()) return std::nullopt;
        std::size_t count = 0;
        for (const auto& object : app.items_oo_)
        {
            const auto* icon = dynamic_cast<const DesktopIcon*>(object.get());
            if (!icon || icon->GetContainer() != app.containers_.front().get()) continue;
            std::error_code error;
            const bool same = std::filesystem::equivalent(icon->GetPath(), path, error);
            if (error) return std::nullopt;
            if (same) ++count;
        }
        return count;
    }
    static DropPreviewList ObserveOlePreview(DesktopApp& app, POINT point, int mods)
    {
        // Observe the target chosen by Enter/Over; do not repair it by resolving again.
        return app.GetCachedDesktopDropPreview(false, app.dragSession_.SourceList(),
            Target(app), TargetSlot(app), Region(app), mods, point);
    }
    static bool Save(DesktopApp& app) { return app.SaveLayoutSlots(false); }
    static void Load(DesktopApp& app) { app.LoadLayoutSlots(); }
    static const auto& Items(const DesktopApp& app) { return app.items_; }
    static bool ProgramCollectionEnabled(const DesktopApp& app) noexcept
    { return app.categorySettings_.collectProgramsEnabled; }
    static void SetFixtureProgramCollectionEnabled(DesktopApp& app, bool enabled) noexcept
    { app.categorySettings_.collectProgramsEnabled = enabled; }
    struct TextDesktopInitial
    {
        std::wstring pageId, pageName, sentinelKey;
        int columns = 0, rows = 0;
        GridCell sentinelCell, targetCell;
        GridSpan sentinelSpan;
        RECT pageBounds{}, targetBounds{};
        std::uint64_t layoutRevision = 0;
        bool pageRecorded = false;
        std::optional<int> savedColumns, savedRows;
    };
    static TextDesktopInitial InspectTextDesktopInitial(const DesktopApp& app,
        const std::filesystem::path& sentinel, POINT targetClient)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (app.gridPages_.size() != 1 || app.items_.size() != 1 || !app.widgets_.empty() ||
            app.generalSettings_.dockEnabled || !AsyncIdle(app) || !LayoutSaveIdle(app))
            throw ObservationUnavailable("text initial desktop scene unavailable");
        const auto& page = app.gridPages_.front(); const auto& item = app.items_.front();
        std::error_code error;
        if (!std::filesystem::equivalent(item.parsingName, sentinel, error) || error || item.name.empty() ||
            page.columns <= 0 || page.rows <= 0 || !PtInRect(&page.bounds, targetClient))
            throw ObservationUnavailable("text initial page or sentinel identity unavailable");
        if (item.gridCell.pageId != page.id || item.gridCell.column < 0 || item.gridCell.row < 0 ||
            item.gridSpan.columns <= 0 || item.gridSpan.rows <= 0 ||
            static_cast<std::int64_t>(item.gridCell.column) + item.gridSpan.columns > page.columns ||
            static_cast<std::int64_t>(item.gridCell.row) + item.gridSpan.rows > page.rows ||
            static_cast<std::int64_t>(item.gridSpan.columns) * item.gridSpan.rows >=
                static_cast<std::int64_t>(page.columns) * page.rows)
            throw ObservationUnavailable("text initial sentinel page occupancy unavailable");
        const auto target = GridCellFromPoint(app.gridPages_, targetClient);
        if (target.pageId != page.id || target.column < 0 || target.row < 0 ||
            target.column >= page.columns || target.row >= page.rows)
            throw ObservationUnavailable("text initial target grid cell unavailable");
        const auto right = static_cast<std::int64_t>(item.gridCell.column) + item.gridSpan.columns;
        const auto bottom = static_cast<std::int64_t>(item.gridCell.row) + item.gridSpan.rows;
        if (item.gridCell.pageId == target.pageId && target.column >= item.gridCell.column && target.column < right &&
            target.row >= item.gridCell.row && target.row < bottom)
            throw ObservationUnavailable("text initial target is occupied");
        const auto targetBounds = GetGridRect(app.gridPages_, target, {1, 1});
        if (targetBounds.right <= targetBounds.left || targetBounds.bottom <= targetBounds.top)
            throw ObservationUnavailable("text initial target bounds unavailable");
        const auto named = app.savedPageNames_.find(page.id);
        const auto savedColumns = app.savedPageColumns_.find(page.id);
        const auto savedRows = app.savedPageRows_.find(page.id);
        return {page.id, named == app.savedPageNames_.end() ? std::wstring{} : named->second, item.layoutKey,
            page.columns, page.rows, item.gridCell, target, item.gridSpan, page.bounds, targetBounds, LayoutRevision(app),
            std::find(app.savedPageIds_.begin(), app.savedPageIds_.end(), page.id) != app.savedPageIds_.end(),
            savedColumns == app.savedPageColumns_.end() ? std::nullopt : std::optional<int>{savedColumns->second},
            savedRows == app.savedPageRows_.end() ? std::nullopt : std::optional<int>{savedRows->second}};
    }
    static void PrepareReload(DesktopApp& app)
    {
        // These providers are outside the layout contract. Do not read the
        // user's clipboard or collect caches while exercising ReloadItems.
        app.clipboardReadWork_.Stop();
        app.dataCleanupWorker_.Stop();
        app.settingsController_ = std::make_unique<snowdesktop::SettingsController>(
            snowdesktop::CreateNativeSettingsStore());
        if (!app.settingsController_->Initialize().Succeeded())
            throw std::runtime_error("initialize actual native settings store in the isolated profile");
        auto general = app.generalSettings_;
        general.dockEnabled = true;
        snowdesktop::DesktopDisplaySettings desktop;
        desktop.dockEnabled = true;
        if (!app.settingsController_->SynchronizeGeneral(general) ||
            !app.settingsController_->SynchronizeDesktop(desktop))
            throw std::runtime_error("seed distinct controller mirrors before actual disk reload");
    }
    static void PerturbBeforeDiskRead(DesktopApp& app)
    {
        for (auto& item : app.items_) item.gridCell = {L"__page:1", 0, 2};
        for (auto& widget : app.widgets_) widget.itemKeys.clear();
    }
    static std::wstring LayoutPath(const DesktopApp& app) { return app.GetLayoutPath(); }
    static void StopModel(DesktopApp& app)
    {
        // Guard and invalidate before touching workers or layout: those calls
        // may reenter and must not start another execution during teardown.
        if (fixtureStopInProgress) throw std::runtime_error("reentrant fixture stop is unavailable");
        EnsureUnbindAllowed(app);
        fixtureStopInProgress = true;
        struct StopScope final { ~StopScope() { NativeDragIntegrationAccess::fixtureStopInProgress = false; } } stopScope;
        if (asyncOwner == &app)
        {
            fixtureTeardownIncomplete = true;
            if (auto epoch = v3HostEpoch.lock()) epoch->Invalidate();
            if (auto epoch = nativeHostEpoch.lock()) epoch->Invalidate();
            if (v4UnicodeSourceOwner) v4UnicodeSourceOwner->Invalidate();
            v4UnicodeSourceOwner.reset();
            if (v4ConfiguredContentOwner) v4ConfiguredContentOwner->Invalidate();
            v4ConfiguredContentOwner.reset();
            if (v4MixedOwner) v4MixedOwner->Invalidate();
            v4MixedOwner.reset();
            if (v4IndependentShellOwner) v4IndependentShellOwner->Invalidate();
            v4IndependentShellOwner.reset();
            if (v4DirectoryShellOwner) v4DirectoryShellOwner->Invalidate();
            v4DirectoryShellOwner.reset();
        }
        app.FlushQueuedLayoutSave();
        app.CancelDeferredLayoutSave();
        app.StopShellFileOperationWorker();
        app.StopUrlDropDownloadWorker();
        UnbindAsync(app);
        app.folderNotifications_.Clear();
        app.StopIconLoader();
        app.appIndexWork_.Stop();
        app.dataCleanupWorker_.Stop();
        if (app.largeIconAssets_) app.largeIconAssets_->Stop();
        app.items_oo_.clear(); app.containers_.clear(); app.items_.clear(); app.widgets_.clear();
        app.hwnd_ = nullptr;
        app.controlHwnd_ = nullptr;
    }
    static void SeedExternalStorage(DesktopApp& app, DesktopWidgetType rootType,
        DesktopWidgetType memberType, bool icon, const std::filesystem::path& member)
    {
        const bool grouped = IsGroupWidgetType(rootType);
        DesktopWidget target;
        target.id = grouped ? L"ingress-active" : L"ingress-root";
        target.type = memberType; target.title = target.customTitle = L"Existing destination";
        target.gridCell = {L"__page:1", 0, 0}; target.gridSpan = {4, 3};
        target.autoCollect = false; target.activeCategoryId = L"all";
        if (memberType == DesktopWidgetType::FolderMapping)
        {
            target.sourceFolderPath = member.parent_path().wstring();
            auto snapshot = snowdesktop::shell_refresh::ReadFolder(target.sourceFolderPath, false);
            if (!snapshot.complete) throw std::runtime_error("read real mapped ingress baseline");
            target.folderEntries = std::move(snapshot.entries);
        }
        else
        {
            auto item = FileItem(member); target.itemKeys = {item.layoutKey};
            app.items_.push_back(std::move(item));
        }
        if (!grouped) { target.gridSpan = icon ? GridSpan{1, 1} : GridSpan{4, 3}; target.singleCellIconMode = icon; }
        app.widgets_.push_back(std::move(target));
        if (grouped)
        {
            DesktopWidget inactive;
            inactive.id = L"ingress-inactive"; inactive.type = DesktopWidgetType::Collection;
            inactive.title = inactive.customTitle = L"Unchanged inactive source";
            inactive.gridCell = {L"__page:1", 0, 0}; inactive.gridSpan = {2, 2};
            inactive.autoCollect = false; inactive.privacyMode = true;
            if (rootType == DesktopWidgetType::FileGroup) inactive.type = DesktopWidgetType::FileCategories;
            app.widgets_.push_back(std::move(inactive));
            DesktopWidget root;
            root.id = L"ingress-root"; root.type = rootType;
            root.title = root.customTitle = L"Ingress group";
            root.childWidgetIds = {L"ingress-active", L"ingress-inactive"}; root.activeCategoryId = L"ingress-active";
            root.gridCell = {L"__page:1", 0, 0}; root.gridSpan = icon ? GridSpan{1, 1} : GridSpan{4, 3};
            root.singleCellIconMode = icon; root.autoCollect = false;
            app.widgets_.push_back(std::move(root));
        }
        app.desktopItemsReady_ = true; app.RefreshDesktopItemIndexCache(); app.LayoutItems();
    }
    static WidgetContainer* MemberContainer(DesktopApp& app, const std::wstring& id)
    {
        for (const auto& container : app.containers_)
            if (auto* widget = dynamic_cast<WidgetContainer*>(container.get());
                widget && widget->GetWidgetData()->id == id) return widget;
        throw std::runtime_error("native member fixture requires its real container");
    }
    struct CollectionIngressGeometry
    {
        std::uintptr_t host = 0, container = 0;
        DWORD ownerThread = 0;
        POINT client{}, screen{};
        RECT viewport{};
        std::uint64_t layoutRevision = 0;
    };
    // Read the isolated host's actual expanded collection geometry. It is
    // only an input snapshot; the production route still decides the target.
    static CollectionIngressGeometry InspectCollectionIngress(DesktopApp& app, POINT client)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            app.hwnd_ != completionWindow || GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("collection ingress host unavailable");
        WidgetContainer* target = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (!widget || !widget->GetWidgetData() || widget->GetWidgetData()->id != L"ingress-root") continue;
            if (target) throw ObservationUnavailable("collection ingress target ambiguous");
            target = widget;
        }
        if (!target || !dynamic_cast<Collection*>(target) || target->IsHosted() || target->IsPopupHosted() ||
            target->IsCollapsed() || target->GetWidgetData()->singleCellIconMode || target->GetWidgetData()->titleBarCollapsed ||
            target->GetWidgetData()->type != DesktopWidgetType::Collection)
            throw ObservationUnavailable("collection ingress expanded target unavailable");
        const auto viewport = target->GetContentViewportRect();
        if (viewport.right <= viewport.left || viewport.bottom <= viewport.top || !PtInRect(&viewport, client))
            throw ObservationUnavailable("collection ingress point outside content viewport");
        POINT screen = client;
        if (!ClientToScreen(completionWindow, &screen)) throw ObservationUnavailable("collection ingress screen coordinate unavailable");
        POINT roundTrip = screen;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
            throw ObservationUnavailable("collection ingress coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(target), GetCurrentThreadId(),
            client, screen, viewport, app.layoutSaveRevision_};
    }
    // Observes the actual expanded desktop-files view without replacing its
    // production hit test or drop routing.
    static CollectionIngressGeometry InspectDesktopFilesIngress(DesktopApp& app, POINT client)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            app.hwnd_ != completionWindow || GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("desktop files ingress host unavailable");
        WidgetContainer* target = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (!widget || !widget->GetWidgetData() || widget->GetWidgetData()->id != L"ingress-root") continue;
            if (target) throw ObservationUnavailable("desktop files ingress target ambiguous");
            target = widget;
        }
        if (!target || !dynamic_cast<FileCategories*>(target) || target->IsHosted() || target->IsPopupHosted() ||
            target->IsCollapsed() || target->GetWidgetData()->singleCellIconMode || target->GetWidgetData()->titleBarCollapsed ||
            target->GetWidgetData()->type != DesktopWidgetType::FileCategories)
            throw ObservationUnavailable("desktop files ingress expanded target unavailable");
        const auto viewport = target->GetContentViewportRect();
        if (viewport.right <= viewport.left || viewport.bottom <= viewport.top || !PtInRect(&viewport, client))
            throw ObservationUnavailable("desktop files ingress point outside content viewport");
        POINT screen = client;
        if (!ClientToScreen(completionWindow, &screen)) throw ObservationUnavailable("desktop files ingress screen coordinate unavailable");
        POINT roundTrip = screen;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
            throw ObservationUnavailable("desktop files ingress coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(target), GetCurrentThreadId(),
            client, screen, viewport, app.layoutSaveRevision_};
    }
    // Observes the actual expanded folder-mapping view without replacing its
    // production hit test or drop routing.
    static CollectionIngressGeometry InspectFolderMappingIngress(DesktopApp& app, POINT client)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            app.hwnd_ != completionWindow || GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("folder mapping ingress host unavailable");
        WidgetContainer* target = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (!widget || !widget->GetWidgetData() || widget->GetWidgetData()->id != L"ingress-root") continue;
            if (target) throw ObservationUnavailable("folder mapping ingress target ambiguous");
            target = widget;
        }
        if (!target || !dynamic_cast<FolderMapping*>(target) || target->IsHosted() || target->IsPopupHosted() ||
            target->IsCollapsed() || target->GetWidgetData()->singleCellIconMode || target->GetWidgetData()->titleBarCollapsed ||
            target->GetWidgetData()->type != DesktopWidgetType::FolderMapping)
            throw ObservationUnavailable("folder mapping ingress expanded target unavailable");
        const auto viewport = target->GetContentViewportRect();
        if (viewport.right <= viewport.left || viewport.bottom <= viewport.top || !PtInRect(&viewport, client))
            throw ObservationUnavailable("folder mapping ingress point outside content viewport");
        POINT screen = client;
        if (!ClientToScreen(completionWindow, &screen)) throw ObservationUnavailable("folder mapping ingress screen coordinate unavailable");
        POINT roundTrip = screen;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
            throw ObservationUnavailable("folder mapping ingress coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(target), GetCurrentThreadId(),
            client, screen, viewport, app.layoutSaveRevision_};
    }
    struct GroupIngressGeometry
    {
        std::uintptr_t host = 0, group = 0, activeWidget = 0;
        DWORD ownerThread = 0;
        std::wstring activeId;
        POINT client{}, screen{};
        RECT viewport{};
        std::uint64_t layoutRevision = 0;
    };
    // This exposes the currently displayed group member and viewport only.
    // The production drop controller still determines hit and membership.
    static GroupIngressGeometry InspectGroupIngress(DesktopApp& app, POINT client)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            app.hwnd_ != completionWindow || GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("group ingress host unavailable");
        CollectionGroup* group = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* candidate = dynamic_cast<CollectionGroup*>(container.get());
            if (!candidate || !candidate->GetWidgetData() || candidate->GetWidgetData()->id != L"ingress-root") continue;
            if (group) throw ObservationUnavailable("group ingress target ambiguous");
            group = candidate;
        }
        if (!group || group->IsHosted() || group->IsPopupHosted() || group->IsCollapsed() ||
            group->GetWidgetData()->type != DesktopWidgetType::CollectionGroup ||
            group->GetWidgetData()->singleCellIconMode || group->GetWidgetData()->titleBarCollapsed)
            throw ObservationUnavailable("group ingress expanded target unavailable");
        const auto activeId = group->GetActiveCollectionId();
        if (activeId.empty() || std::count(group->GetWidgetData()->childWidgetIds.begin(),
            group->GetWidgetData()->childWidgetIds.end(), activeId) != 1)
            throw ObservationUnavailable("group ingress active member unavailable");
        DesktopWidget* active = nullptr;
        for (auto& widget : app.widgets_)
        {
            if (widget.id != activeId) continue;
            if (active) throw ObservationUnavailable("group ingress active member ambiguous");
            active = &widget;
        }
        if (!active || active->type != DesktopWidgetType::Collection || group->GetContentViewData() != active)
            throw ObservationUnavailable("group ingress active collection unavailable");
        const auto viewport = group->GetContentViewportRect();
        if (viewport.right <= viewport.left || viewport.bottom <= viewport.top || !PtInRect(&viewport, client))
            throw ObservationUnavailable("group ingress point outside content viewport");
        POINT screen = client;
        if (!ClientToScreen(completionWindow, &screen)) throw ObservationUnavailable("group ingress screen coordinate unavailable");
        POINT roundTrip = screen;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
            throw ObservationUnavailable("group ingress coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(group),
            reinterpret_cast<std::uintptr_t>(active), GetCurrentThreadId(), activeId, client, screen, viewport, app.layoutSaveRevision_};
    }
    struct V4GroupChildSnapshot
    {
        std::wstring id;
        std::uintptr_t widget = 0;
        std::int64_t type = 0;
    };
    struct V4GroupChildrenSnapshot
    {
        std::uintptr_t host = 0, group = 0, groupData = 0, contentData = 0;
        DWORD ownerThread = 0;
        std::wstring groupId, activeId;
        std::int64_t groupType = 0;
        std::optional<std::size_t> activeIndex;
        std::optional<POINT> clientPoint, screenPoint;
        RECT viewport{};
        std::uint64_t layoutRevision = 0;
        std::vector<V4GroupChildSnapshot> children;
    };
    // Value-only observation of an existing live group. It does not perform
    // hit testing, select a child, or establish a later asynchronous commit.
    // The production active-ID getter returns a string by value before the
    // byte budget is checked; the limits do not bound all work or allocation.
    static std::optional<V4GroupChildrenSnapshot> InspectV4GroupChildren(
        DesktopApp& app, std::wstring_view groupId, std::optional<POINT> clientPoint,
        std::size_t maxChildren, std::size_t maxWidgets, std::size_t maxUtf16Bytes,
        bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            if (groupId.empty() || asyncOwner != &app || productionDispatch != 1 || productionDispatchDepth ||
                !completionWindow || !IsWindow(completionWindow) || app.hwnd_ != completionWindow ||
                GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId()) return std::nullopt;
            if (app.widgets_.size() > maxWidgets || app.containers_.size() > maxWidgets)
            { resourceUnavailable = true; return std::nullopt; }
            const auto* widgetStorage = app.widgets_.data(); const auto widgetCount = app.widgets_.size();
            const auto* containerStorage = app.containers_.data(); const auto containerCount = app.containers_.size();
            const auto revision = app.layoutSaveRevision_;
            std::optional<V4GroupChildrenSnapshot> snapshot;
            const auto capture = [&](auto* group, const std::wstring& activeId) -> bool {
                if (!group || !group->GetWidgetData() || group->GetWidgetData()->id != groupId || snapshot) return false;
                const auto* data = group->GetWidgetData();
                if (data->childWidgetIds.size() > maxChildren) { resourceUnavailable = true; return false; }
                std::size_t remaining = maxUtf16Bytes;
                const auto charge = [&](std::wstring_view text) {
                    if (text.size() > remaining / sizeof(wchar_t)) return false;
                    remaining -= text.size() * sizeof(wchar_t); return true;
                };
                if (!charge(data->id) || !charge(activeId)) { resourceUnavailable = true; return false; }
                for (const auto& id : data->childWidgetIds)
                    if (!charge(id)) { resourceUnavailable = true; return false; }
                const auto owner = std::count_if(app.widgets_.begin(), app.widgets_.end(), [&](const auto& item) {
                    return item.id == data->id && &item == data;
                });
                const auto ownerIds = std::count_if(app.widgets_.begin(), app.widgets_.end(), [&](const auto& item) {
                    return item.id == data->id;
                });
                if (owner != 1 || ownerIds != 1) return false;
                V4GroupChildrenSnapshot value;
                value.host = reinterpret_cast<std::uintptr_t>(&app); value.group = reinterpret_cast<std::uintptr_t>(group);
                value.groupData = reinterpret_cast<std::uintptr_t>(data);
                value.contentData = reinterpret_cast<std::uintptr_t>(group->GetContentViewData());
                value.ownerThread = GetCurrentThreadId(); value.groupId = data->id; value.activeId = activeId;
                value.groupType = static_cast<std::int64_t>(data->type); value.viewport = group->GetContentViewportRect();
                value.layoutRevision = revision; value.clientPoint = clientPoint;
                if (clientPoint)
                {
                    POINT screen = *clientPoint;
                    if (!ClientToScreen(completionWindow, &screen)) return false;
                    POINT roundTrip = screen;
                    if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != clientPoint->x || roundTrip.y != clientPoint->y) return false;
                    value.screenPoint = screen;
                }
                value.children.reserve(data->childWidgetIds.size());
                for (std::size_t i = 0; i < data->childWidgetIds.size(); ++i)
                {
                    const auto& id = data->childWidgetIds[i];
                    if (std::count(data->childWidgetIds.begin(), data->childWidgetIds.end(), id) != 1) return false;
                    const DesktopWidget* found = nullptr;
                    for (const auto& widget : app.widgets_)
                        if (widget.id == id) { if (found) return false; found = &widget; }
                    if (!found) return false;
                    if (id == activeId) value.activeIndex = i;
                    value.children.push_back({id, reinterpret_cast<std::uintptr_t>(found), static_cast<std::int64_t>(found->type)});
                }
                if (!activeId.empty() && !value.activeIndex) return false;
                snapshot = std::move(value); return true;
            };
            for (const auto& container : app.containers_)
            {
                if (auto* group = dynamic_cast<CollectionGroup*>(container.get()); group && group->GetWidgetData() &&
                    group->GetWidgetData()->id == groupId && !capture(group, group->GetActiveCollectionId())) return std::nullopt;
                if (auto* group = dynamic_cast<FileGroup*>(container.get()); group && group->GetWidgetData() &&
                    group->GetWidgetData()->id == groupId && !capture(group, group->GetActiveSourceId())) return std::nullopt;
            }
            if (!snapshot || asyncOwner != &app || productionDispatch != 1 || productionDispatchDepth ||
                !completionWindow || !IsWindow(completionWindow) || app.hwnd_ != completionWindow ||
                GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId() ||
                app.layoutSaveRevision_ != revision || app.widgets_.data() != widgetStorage || app.widgets_.size() != widgetCount ||
                app.containers_.data() != containerStorage || app.containers_.size() != containerCount) return std::nullopt;
            const auto matches = [&](auto* group, const std::wstring& activeId) {
                if (!group || reinterpret_cast<std::uintptr_t>(group) != snapshot->group || !group->GetWidgetData()) return false;
                const auto* data = group->GetWidgetData(); const auto viewport = group->GetContentViewportRect();
                if (reinterpret_cast<std::uintptr_t>(data) != snapshot->groupData || data->id != snapshot->groupId ||
                    static_cast<std::int64_t>(data->type) != snapshot->groupType || activeId != snapshot->activeId ||
                    reinterpret_cast<std::uintptr_t>(group->GetContentViewData()) != snapshot->contentData ||
                    !EqualRect(&viewport, &snapshot->viewport) || data->childWidgetIds.size() != snapshot->children.size()) return false;
                for (std::size_t i = 0; i < snapshot->children.size(); ++i)
                {
                    const auto& child = snapshot->children[i];
                    if (data->childWidgetIds[i] != child.id) return false;
                    const DesktopWidget* found = nullptr;
                    for (const auto& widget : app.widgets_)
                        if (widget.id == child.id) { if (found) return false; found = &widget; }
                    if (!found || reinterpret_cast<std::uintptr_t>(found) != child.widget ||
                        static_cast<std::int64_t>(found->type) != child.type) return false;
                }
                return true;
            };
            bool stillLive = false;
            for (const auto& container : app.containers_)
            {
                if (auto* group = dynamic_cast<CollectionGroup*>(container.get()); group &&
                    reinterpret_cast<std::uintptr_t>(group) == snapshot->group)
                { if (stillLive || !matches(group, group->GetActiveCollectionId())) return std::nullopt; stillLive = true; }
                if (auto* group = dynamic_cast<FileGroup*>(container.get()); group &&
                    reinterpret_cast<std::uintptr_t>(group) == snapshot->group)
                { if (stillLive || !matches(group, group->GetActiveSourceId())) return std::nullopt; stillLive = true; }
            }
            if (!stillLive || std::count_if(app.widgets_.begin(), app.widgets_.end(), [&](const auto& item) {
                    return item.id == snapshot->groupId;
                }) != 1 || std::count_if(app.widgets_.begin(), app.widgets_.end(), [&](const auto& item) {
                    return item.id == snapshot->groupId && reinterpret_cast<std::uintptr_t>(&item) == snapshot->groupData;
                }) != 1) return std::nullopt;
            return snapshot;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return std::nullopt; }
        catch (const std::length_error&) { resourceUnavailable = true; return std::nullopt; }
        catch (...) { return std::nullopt; }
    }
    struct V4GroupTabIngressSnapshot
    {
        V4GroupChildrenSnapshot group;
        std::wstring childId;
        std::uintptr_t child = 0;
        RECT tab{};
        POINT client{}, screen{};
    };
    // The rectangle is a point suggestion, not evidence that the production
    // hit test or asynchronous activation selected this child.
    static std::optional<V4GroupTabIngressSnapshot> InspectV4GroupTabIngress(
        DesktopApp& app, std::wstring_view groupId, std::wstring_view childId,
        std::size_t maxChildren, std::size_t maxWidgets, std::size_t maxUtf16Bytes,
        bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            auto before = InspectV4GroupChildren(app, groupId, std::nullopt,
                maxChildren, maxWidgets, maxUtf16Bytes, resourceUnavailable);
            if (!before || childId.empty()) return std::nullopt;
            const auto child = std::find_if(before->children.begin(), before->children.end(),
                [&](const auto& item) { return item.id == childId; });
            if (child == before->children.end()) return std::nullopt;
            const std::wstring childName(childId);
            const auto readTab = [&]() -> std::optional<RECT> {
                for (const auto& container : app.containers_)
                {
                    if (auto* group = dynamic_cast<CollectionGroup*>(container.get()); group &&
                        reinterpret_cast<std::uintptr_t>(group) == before->group)
                        return group->GetTabRectById(childName);
                    if (auto* group = dynamic_cast<FileGroup*>(container.get()); group &&
                        reinterpret_cast<std::uintptr_t>(group) == before->group)
                        return group->GetSourceTabRectById(childName);
                }
                return std::nullopt;
            };
            auto tab = readTab();
            if (!tab || tab->right <= tab->left || tab->bottom <= tab->top) return std::nullopt;
            const auto midpoint = [](LONG a, LONG b) {
                return static_cast<LONG>(static_cast<std::int64_t>(a) +
                    (static_cast<std::int64_t>(b) - static_cast<std::int64_t>(a)) / 2);
            };
            POINT client{midpoint(tab->left, tab->right), midpoint(tab->top, tab->bottom)};
            POINT screen = client;
            if (!ClientToScreen(completionWindow, &screen)) return std::nullopt;
            POINT roundTrip = screen;
            if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
                return std::nullopt;
            auto after = InspectV4GroupChildren(app, groupId, std::nullopt,
                maxChildren, maxWidgets, maxUtf16Bytes, resourceUnavailable);
            if (!after) return std::nullopt;
            auto afterTab = readTab();
            if (!afterTab || !EqualRect(&*tab, &*afterTab) ||
                before->host != after->host || before->group != after->group ||
                before->groupData != after->groupData || before->contentData != after->contentData ||
                before->ownerThread != after->ownerThread || before->groupId != after->groupId ||
                before->groupType != after->groupType || before->activeId != after->activeId ||
                before->activeIndex != after->activeIndex || before->layoutRevision != after->layoutRevision ||
                !EqualRect(&before->viewport, &after->viewport) || before->children.size() != after->children.size())
                return std::nullopt;
            for (std::size_t i = 0; i < before->children.size(); ++i)
                if (before->children[i].id != after->children[i].id ||
                    before->children[i].widget != after->children[i].widget ||
                    before->children[i].type != after->children[i].type) return std::nullopt;
            V4GroupTabIngressSnapshot result;
            result.childId = childName; result.child = child->widget;
            result.tab = *tab; result.client = client; result.screen = screen;
            result.group = std::move(*before);
            return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return std::nullopt; }
        catch (const std::length_error&) { resourceUnavailable = true; return std::nullopt; }
        catch (...) { return std::nullopt; }
    }
    struct V4NativeGroupMemberSource
    {
        V4GroupChildrenSnapshot group;
        std::uintptr_t container = 0, item = 0, childData = 0;
        std::wstring memberKey, itemPath;
        FILE_ID_INFO fileId{};
        std::size_t memberIndex = 0;
        RECT memberBounds{}, visible{}, viewport{};
        POINT client{}, screen{};
    };
    // A live input snapshot only. The selected point is not a hit or capture;
    // group membership, asynchronous work and layout commit need their own
    // same-session observations. Counts do not bound getter allocations.
    static std::optional<V4NativeGroupMemberSource> InspectNativeGroupMemberSource(
        DesktopApp& app, const std::filesystem::path& path, std::wstring_view groupId,
        std::size_t maxChildren, std::size_t maxWidgets, std::size_t maxUtf16Bytes,
        bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            if (!nativePointerDispatch || app.items_.size() > maxWidgets)
            { resourceUnavailable = app.items_.size() > maxWidgets; return std::nullopt; }
            auto snapshot = InspectV4GroupChildren(app, groupId, std::nullopt,
                maxChildren, maxWidgets, maxUtf16Bytes, resourceUnavailable);
            if (!snapshot || !snapshot->activeIndex || *snapshot->activeIndex >= snapshot->children.size()) return std::nullopt;
            CollectionGroup* group = nullptr;
            for (const auto& container : app.containers_)
                if (auto* value = dynamic_cast<CollectionGroup*>(container.get()); value &&
                    reinterpret_cast<std::uintptr_t>(value) == snapshot->group)
                { if (group) return std::nullopt; group = value; }
            const auto& active = snapshot->children[*snapshot->activeIndex];
            if (!group || group->IsHosted() || group->IsPopupHosted() || group->IsCollapsed() ||
                !group->GetWidgetData() || group->GetWidgetData()->type != DesktopWidgetType::CollectionGroup ||
                group->GetWidgetData()->singleCellIconMode || group->GetWidgetData()->titleBarCollapsed ||
                active.type != static_cast<std::int64_t>(DesktopWidgetType::Collection) ||
                snapshot->contentData != active.widget) return std::nullopt;
            const DesktopWidget* child = nullptr;
            for (const auto& widget : app.widgets_)
                if (reinterpret_cast<std::uintptr_t>(&widget) == active.widget && widget.id == active.id)
                { if (child) return std::nullopt; child = &widget; }
            if (!child || child->itemKeys.size() > maxChildren)
            { resourceUnavailable = child && child->itemKeys.size() > maxChildren; return std::nullopt; }
            const DesktopItem* item = nullptr;
            for (const auto& candidate : app.items_)
            {
                std::error_code error; const bool same = std::filesystem::equivalent(candidate.parsingName, path, error);
                if (error) return std::nullopt;
                if (same) { if (item) return std::nullopt; item = &candidate; }
            }
            if (!item || item->selected) return std::nullopt;
            std::size_t remaining = maxUtf16Bytes;
            const auto charge = [&](std::wstring_view value) {
                if (value.size() > remaining / sizeof(wchar_t)) return false;
                remaining -= value.size() * sizeof(wchar_t); return true;
            };
            if (!charge(snapshot->groupId) || !charge(snapshot->activeId))
            { resourceUnavailable = true; return std::nullopt; }
            for (const auto& member : snapshot->children)
                if (!charge(member.id)) { resourceUnavailable = true; return std::nullopt; }
            if (!charge(item->layoutKey) || !charge(item->parsingName))
            { resourceUnavailable = true; return std::nullopt; }
            const auto found = std::find(child->itemKeys.begin(), child->itemKeys.end(), item->layoutKey);
            if (found == child->itemKeys.end() || std::find(std::next(found), child->itemKeys.end(), item->layoutKey) != child->itemKeys.end())
                return std::nullopt;
            struct ScopedHandle { HANDLE value = INVALID_HANDLE_VALUE; ~ScopedHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } };
            ScopedHandle file{CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
            FILE_ID_INFO fileId{}; FILE_ATTRIBUTE_TAG_INFO tag{};
            if (file.value == INVALID_HANDLE_VALUE || !GetFileInformationByHandleEx(file.value, FileIdInfo, &fileId, sizeof(fileId)) ||
                !GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                !std::any_of(std::begin(fileId.FileId.Identifier), std::end(fileId.FileId.Identifier), [](BYTE byte) { return byte != 0; }))
                return std::nullopt;
            const auto stillNamed = [&](const wchar_t* name) {
                ScopedHandle named{CreateFileW(name, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO attributes{};
                return named.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(named.value, FileIdInfo, &id, sizeof(id)) &&
                    GetFileInformationByHandleEx(named.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
                    !(attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    id.VolumeSerialNumber == fileId.VolumeSerialNumber &&
                    std::memcmp(id.FileId.Identifier, fileId.FileId.Identifier, sizeof(id.FileId.Identifier)) == 0;
            };
            if (!stillNamed(path.c_str()) || !stillNamed(item->parsingName.c_str())) return std::nullopt;
            const auto index = static_cast<std::size_t>(found - child->itemKeys.begin());
            const RECT bounds = group->GetMemberLayoutRect(index), viewport = group->GetContentViewportRect();
            RECT visible{};
            if (!IntersectRect(&visible, &bounds, &viewport) || visible.right <= visible.left || visible.bottom <= visible.top)
                return std::nullopt;
            const auto midpoint = [](LONG a, LONG b) { return static_cast<LONG>(static_cast<std::int64_t>(a) +
                (static_cast<std::int64_t>(b) - static_cast<std::int64_t>(a)) / 2); };
            POINT client{midpoint(visible.left, visible.right), midpoint(visible.top, visible.bottom)}, screen = client;
            if (!ClientToScreen(completionWindow, &screen)) return std::nullopt;
            POINT roundTrip = screen;
            if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
                return std::nullopt;
            const auto stableBounds = group->GetMemberLayoutRect(index), stableViewport = group->GetContentViewportRect();
            if (!EqualRect(&bounds, &stableBounds) || !EqualRect(&viewport, &stableViewport) ||
                app.layoutSaveRevision_ != snapshot->layoutRevision || group->GetContentViewData() != child ||
                group->GetActiveCollectionId() != active.id || child->itemKeys.size() <= index ||
                child->itemKeys[index] != item->layoutKey || item->selected ||
                !stillNamed(path.c_str()) || !stillNamed(item->parsingName.c_str())) return std::nullopt;
            V4NativeGroupMemberSource result;
            result.container = reinterpret_cast<std::uintptr_t>(static_cast<Container*>(group));
            result.item = reinterpret_cast<std::uintptr_t>(item); result.childData = active.widget;
            result.memberKey = item->layoutKey; result.itemPath = item->parsingName; result.fileId = fileId;
            result.memberIndex = index; result.memberBounds = bounds; result.visible = visible; result.viewport = viewport;
            result.client = client; result.screen = screen; result.group = std::move(*snapshot);
            return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return std::nullopt; }
        catch (const std::length_error&) { resourceUnavailable = true; return std::nullopt; }
        catch (...) { return std::nullopt; }
    }
    struct V4NativeGroupMemberCapture
    {
        std::uintptr_t host = 0, source = 0, origin = 0, item = 0;
        DWORD ownerThread = 0;
        HWND pointerCapture = nullptr;
        bool active = false, hasOriginWidget = false, hasOriginSurface = false;
        std::int64_t originWidgetType = 0, originSurface = 0, entryKind = 0;
        std::wstring originWidgetId, entryWidgetId, desktopKey, filePath;
        std::size_t sourceIndex = 0, desktopIndex = 0, memberIndex = 0, itemCount = 0, entryCount = 0;
        std::optional<FILE_ID_INFO> fileId;
        std::uint64_t layoutRevision = 0;
    };
    // Read the live session only after confirming that its source pointer is a
    // unique currently owned container. This is a local capture, not proof of
    // a release, commit, persistence, or absence of later work.
    static std::optional<V4NativeGroupMemberCapture> InspectNativeGroupMemberCapture(
        DesktopApp& app, const V4NativeGroupMemberSource& expected,
        std::size_t maxChildren, std::size_t maxWidgets, std::size_t maxUtf16Bytes,
        bool& resourceUnavailable)
    {
        resourceUnavailable = false;
        try
        {
            if (!nativePointerDispatch || expected.group.host != reinterpret_cast<std::uintptr_t>(&app) ||
                expected.group.ownerThread != GetCurrentThreadId() || !app.dragSession_.IsActive()) return std::nullopt;
            auto group = InspectV4GroupChildren(app, expected.group.groupId, std::nullopt,
                maxChildren, maxWidgets, maxUtf16Bytes, resourceUnavailable);
            if (!group || group->group != expected.group.group || group->groupData != expected.group.groupData ||
                group->contentData != expected.childData || group->activeId != expected.group.activeId) return std::nullopt;
            const auto* source = app.dragSession_.Source();
            std::size_t live = 0;
            for (const auto& container : app.containers_) if (container.get() == source) ++live;
            if (live != 1 || reinterpret_cast<std::uintptr_t>(source) != expected.container) return std::nullopt;
            const auto& list = app.dragSession_.SourceList();
            if (list.entries.size() != 1 || app.dragSession_.Items().size() != 1) return std::nullopt;
            const auto& entry = list.entries.front();
            std::size_t remaining = maxUtf16Bytes;
            const auto charge = [&](std::wstring_view value) {
                if (value.size() > remaining / sizeof(wchar_t)) return false;
                remaining -= value.size() * sizeof(wchar_t); return true;
            };
            if (!charge(group->groupId) || !charge(group->activeId))
            { resourceUnavailable = true; return std::nullopt; }
            for (const auto& child : group->children)
                if (!charge(child.id)) { resourceUnavailable = true; return std::nullopt; }
            if (!charge(list.originWidgetId) || !charge(entry.widgetId) || !charge(entry.desktopKey) || !charge(entry.filePath))
            { resourceUnavailable = true; return std::nullopt; }
            V4NativeGroupMemberCapture result;
            result.host = reinterpret_cast<std::uintptr_t>(&app); result.source = reinterpret_cast<std::uintptr_t>(source);
            result.origin = reinterpret_cast<std::uintptr_t>(list.origin); result.item = reinterpret_cast<std::uintptr_t>(entry.item);
            result.ownerThread = GetCurrentThreadId(); result.pointerCapture = snowdesktop::private_pointer::Capture();
            result.active = true; result.hasOriginWidget = list.hasOriginWidget; result.hasOriginSurface = list.hasOriginSurface;
            result.originWidgetType = static_cast<std::int64_t>(list.originWidgetType);
            result.originSurface = static_cast<std::int64_t>(list.originSurface); result.entryKind = static_cast<std::int64_t>(entry.kind);
            result.originWidgetId = list.originWidgetId; result.entryWidgetId = entry.widgetId;
            result.desktopKey = entry.desktopKey; result.filePath = entry.filePath;
            result.sourceIndex = entry.sourceIndex; result.desktopIndex = entry.desktopIndex; result.memberIndex = entry.memberIndex;
            result.itemCount = app.dragSession_.Items().size(); result.entryCount = list.entries.size();
            result.layoutRevision = app.layoutSaveRevision_;
            if (!result.filePath.empty())
            {
                struct ScopedHandle { HANDLE value; ~ScopedHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); } } file{
                    CreateFileW(result.filePath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
                if (file.value != INVALID_HANDLE_VALUE && GetFileInformationByHandleEx(file.value, FileIdInfo, &id, sizeof(id)) &&
                    GetFileInformationByHandleEx(file.value, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                    !(tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                    std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE byte) { return byte != 0; }))
                {
                    const auto stillNamed = [&] {
                        ScopedHandle named{CreateFileW(result.filePath.c_str(), FILE_READ_ATTRIBUTES,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                            FILE_FLAG_OPEN_REPARSE_POINT, nullptr)};
                        FILE_ID_INFO check{}; FILE_ATTRIBUTE_TAG_INFO attributes{};
                        return named.value != INVALID_HANDLE_VALUE &&
                            GetFileInformationByHandleEx(named.value, FileIdInfo, &check, sizeof(check)) &&
                            GetFileInformationByHandleEx(named.value, FileAttributeTagInfo, &attributes, sizeof(attributes)) &&
                            !(attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
                            check.VolumeSerialNumber == id.VolumeSerialNumber &&
                            std::memcmp(check.FileId.Identifier, id.FileId.Identifier, sizeof(id.FileId.Identifier)) == 0;
                    };
                    const bool firstName = stillNamed();
                    const auto& current = app.dragSession_.SourceList();
                    const bool sessionStable = app.dragSession_.IsActive() && app.dragSession_.Source() == source &&
                        reinterpret_cast<std::uintptr_t>(current.origin) == result.origin && current.entries.size() == 1 &&
                        app.dragSession_.Items().size() == 1 &&
                        static_cast<std::int64_t>(current.entries.front().kind) == result.entryKind && current.entries.front().filePath == result.filePath &&
                        current.entries.front().desktopKey == result.desktopKey && current.entries.front().widgetId == result.entryWidgetId;
                    if (firstName && sessionStable && stillNamed()) result.fileId = id;
                }
            }
            return result;
        }
        catch (const std::bad_alloc&) { resourceUnavailable = true; return std::nullopt; }
        catch (const std::length_error&) { resourceUnavailable = true; return std::nullopt; }
        catch (...) { return std::nullopt; }
    }
    // Like the collection-group snapshot, this observes only the current
    // host, active source and viewport; production still performs hit testing.
    static GroupIngressGeometry InspectFileGroupIngress(DesktopApp& app, POINT client)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            app.hwnd_ != completionWindow || GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("file group ingress host unavailable");
        FileGroup* group = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* candidate = dynamic_cast<FileGroup*>(container.get());
            if (!candidate || !candidate->GetWidgetData() || candidate->GetWidgetData()->id != L"ingress-root") continue;
            if (group) throw ObservationUnavailable("file group ingress target ambiguous");
            group = candidate;
        }
        if (!group || group->IsHosted() || group->IsPopupHosted() || group->IsCollapsed() ||
            group->GetWidgetData()->type != DesktopWidgetType::FileGroup ||
            group->GetWidgetData()->singleCellIconMode || group->GetWidgetData()->titleBarCollapsed)
            throw ObservationUnavailable("file group ingress expanded target unavailable");
        const auto activeId = group->GetActiveSourceId();
        if (activeId.empty() || std::count(group->GetWidgetData()->childWidgetIds.begin(),
            group->GetWidgetData()->childWidgetIds.end(), activeId) != 1)
            throw ObservationUnavailable("file group ingress active source unavailable");
        DesktopWidget* active = nullptr;
        for (auto& widget : app.widgets_)
        {
            if (widget.id != activeId) continue;
            if (active) throw ObservationUnavailable("file group ingress active source ambiguous");
            active = &widget;
        }
        if (!active || active->type != DesktopWidgetType::FileCategories || group->GetContentViewData() != active)
            throw ObservationUnavailable("file group ingress active file source unavailable");
        const auto viewport = group->GetContentViewportRect();
        if (viewport.right <= viewport.left || viewport.bottom <= viewport.top || !PtInRect(&viewport, client))
            throw ObservationUnavailable("file group ingress point outside content viewport");
        POINT screen = client;
        if (!ClientToScreen(completionWindow, &screen)) throw ObservationUnavailable("file group ingress screen coordinate unavailable");
        POINT roundTrip = screen;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
            throw ObservationUnavailable("file group ingress coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(group),
            reinterpret_cast<std::uintptr_t>(active), GetCurrentThreadId(), activeId, client, screen, viewport, app.layoutSaveRevision_};
    }
    // Like the collection-group snapshot, this observes only the current
    // host, active source and viewport; production still performs hit testing.
    static GroupIngressGeometry InspectFileGroupMappingIngress(DesktopApp& app, POINT client)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            app.hwnd_ != completionWindow || GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("file group ingress host unavailable");
        FileGroup* group = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* candidate = dynamic_cast<FileGroup*>(container.get());
            if (!candidate || !candidate->GetWidgetData() || candidate->GetWidgetData()->id != L"ingress-root") continue;
            if (group) throw ObservationUnavailable("file group ingress target ambiguous");
            group = candidate;
        }
        if (!group || group->IsHosted() || group->IsPopupHosted() || group->IsCollapsed() ||
            group->GetWidgetData()->type != DesktopWidgetType::FileGroup ||
            group->GetWidgetData()->singleCellIconMode || group->GetWidgetData()->titleBarCollapsed)
            throw ObservationUnavailable("file group ingress expanded target unavailable");
        const auto activeId = group->GetActiveSourceId();
        if (activeId.empty() || std::count(group->GetWidgetData()->childWidgetIds.begin(),
            group->GetWidgetData()->childWidgetIds.end(), activeId) != 1)
            throw ObservationUnavailable("file group ingress active source unavailable");
        DesktopWidget* active = nullptr;
        for (auto& widget : app.widgets_)
        {
            if (widget.id != activeId) continue;
            if (active) throw ObservationUnavailable("file group ingress active source ambiguous");
            active = &widget;
        }
        if (!active || active->type != DesktopWidgetType::FolderMapping || group->GetContentViewData() != active)
            throw ObservationUnavailable("file group ingress active mapping source unavailable");
        const auto viewport = group->GetContentViewportRect();
        if (viewport.right <= viewport.left || viewport.bottom <= viewport.top || !PtInRect(&viewport, client))
            throw ObservationUnavailable("file group ingress point outside content viewport");
        POINT screen = client;
        if (!ClientToScreen(completionWindow, &screen)) throw ObservationUnavailable("file group ingress screen coordinate unavailable");
        POINT roundTrip = screen;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
            throw ObservationUnavailable("file group ingress coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(group),
            reinterpret_cast<std::uintptr_t>(active), GetCurrentThreadId(), activeId, client, screen, viewport, app.layoutSaveRevision_};
    }
    // A read-only geometry candidate, not a hit-test or proof that the device
    // will select this member. Actual WndProc delivery and capture are checked
    // independently by the execution observer.
    static POINT NativeMemberInputCandidate(DesktopApp& app, const std::filesystem::path& path)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        const DesktopItem* item = nullptr;
        for (const auto& candidate : app.items_)
        {
            std::error_code error;
            const bool same = std::filesystem::equivalent(candidate.parsingName, path, error);
            if (error) throw ObservationUnavailable("native member physical identity unavailable");
            if (!same) continue;
            if (item) throw ObservationUnavailable("native member physical identity is ambiguous");
            item = &candidate;
        }
        if (!item || item->selected) throw ObservationUnavailable("native member baseline or selection unavailable");
        WidgetContainer* source = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (!widget || !widget->GetWidgetData() || widget->GetWidgetData()->id != L"native-origin") continue;
            if (source) throw ObservationUnavailable("native member source container is ambiguous");
            source = widget;
        }
        if (!source || !dynamic_cast<Collection*>(source) || source->IsHosted() || source->IsPopupHosted())
            throw ObservationUnavailable("native member standalone collection unavailable");
        const auto& data = *source->GetWidgetData();
        // FileCategories has a separate filtered/sorted visible index and can
        // lazily build its view; itemKeys alone cannot safely identify a slot.
        if (data.gridSpan.columns != 4 || data.gridSpan.rows != 3 || data.singleCellIconMode || data.showSearchBox ||
            data.titleBarCollapsed || source->IsCollapsed() || data.type != DesktopWidgetType::Collection || !data.scrollContainerMode)
            throw ObservationUnavailable("native member source geometry unsupported");
        const auto found = std::find(data.itemKeys.begin(), data.itemKeys.end(), item->layoutKey);
        if (found == data.itemKeys.end() || std::find(std::next(found), data.itemKeys.end(), item->layoutKey) != data.itemKeys.end())
            throw ObservationUnavailable("native member key missing or ambiguous");
        const auto bounds = source->GetMemberLayoutRect(static_cast<std::size_t>(found - data.itemKeys.begin()));
        const auto viewport = source->GetContentViewportRect();
        RECT visible{};
        if (!IntersectRect(&visible, &bounds, &viewport) || visible.right <= visible.left || visible.bottom <= visible.top)
            throw ObservationUnavailable("native member candidate has no visible geometry");
        const auto x = static_cast<std::int64_t>(visible.left) +
            (static_cast<std::int64_t>(visible.right) - visible.left) / 2;
        const auto y = static_cast<std::int64_t>(visible.top) +
            (static_cast<std::int64_t>(visible.bottom) - visible.top) / 2;
        if (x < (std::numeric_limits<LONG>::min)() || x > (std::numeric_limits<LONG>::max)() ||
            y < (std::numeric_limits<LONG>::min)() || y > (std::numeric_limits<LONG>::max)())
            throw ObservationUnavailable("native member candidate coordinates unavailable");
        return POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
    }
    struct NativeCategorySourceGeometry
    {
        std::uintptr_t host = 0, container = 0;
        DWORD ownerThread = 0;
        std::wstring activeCategory, memberKey;
        std::vector<std::wstring> visibleKeys;
        std::size_t visibleIndex = 0;
        POINT client{}, screen{};
        RECT memberBounds{}, viewport{};
        std::uint64_t layoutRevision = 0;
    };
    // The visible index comes from the production FileCategories view. The
    // returned point remains an input candidate, not proof of a hit or capture.
    static NativeCategorySourceGeometry InspectNativeCategorySource(DesktopApp& app, const std::filesystem::path& path)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !nativePointerDispatch || !completionWindow ||
            !IsWindow(completionWindow) || app.hwnd_ != completionWindow ||
            GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("native category host unavailable");
        const DesktopItem* item = nullptr;
        for (const auto& candidate : app.items_)
        {
            std::error_code error;
            const bool same = std::filesystem::equivalent(candidate.parsingName, path, error);
            if (error) throw ObservationUnavailable("native category physical identity unavailable");
            if (!same) continue;
            if (item) throw ObservationUnavailable("native category physical identity ambiguous");
            item = &candidate;
        }
        if (!item || item->selected) throw ObservationUnavailable("native category baseline selection unavailable");
        FileCategories* source = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<FileCategories*>(container.get());
            if (!widget || !widget->GetWidgetData() || widget->GetWidgetData()->id != L"native-origin") continue;
            if (source) throw ObservationUnavailable("native category source ambiguous");
            source = widget;
        }
        if (!source || source->IsHosted() || source->IsPopupHosted() || source->IsCollapsed() ||
            source->GetWidgetData()->type != DesktopWidgetType::FileCategories ||
            source->GetWidgetData()->singleCellIconMode || source->GetWidgetData()->titleBarCollapsed ||
            source->GetWidgetData()->showSearchBox || source->GetWidgetData()->dateHeaders)
            throw ObservationUnavailable("native category expanded source unavailable");
        const auto active = source->CachedActiveCategoryId();
        const auto keys = source->GetSearchResultKeys();
        const auto found = std::find(keys.begin(), keys.end(), item->layoutKey);
        if (active.empty() || found == keys.end() || std::find(std::next(found), keys.end(), item->layoutKey) != keys.end())
            throw ObservationUnavailable("native category visible member unavailable");
        const auto index = static_cast<std::size_t>(found - keys.begin());
        const auto bounds = source->GetMemberLayoutRect(index);
        const auto viewport = source->GetContentViewportRect();
        RECT visible{};
        if (!IntersectRect(&visible, &bounds, &viewport) || visible.right <= visible.left || visible.bottom <= visible.top)
            throw ObservationUnavailable("native category member has no visible geometry");
        const auto x = static_cast<std::int64_t>(visible.left) + (static_cast<std::int64_t>(visible.right) - visible.left) / 2;
        const auto y = static_cast<std::int64_t>(visible.top) + (static_cast<std::int64_t>(visible.bottom) - visible.top) / 2;
        if (x < (std::numeric_limits<LONG>::min)() || x > (std::numeric_limits<LONG>::max)() ||
            y < (std::numeric_limits<LONG>::min)() || y > (std::numeric_limits<LONG>::max)())
            throw ObservationUnavailable("native category coordinates unavailable");
        POINT client{static_cast<LONG>(x), static_cast<LONG>(y)}, screen = client;
        if (!ClientToScreen(completionWindow, &screen)) throw ObservationUnavailable("native category screen coordinate unavailable");
        POINT roundTrip = screen;
        if (!ScreenToClient(completionWindow, &roundTrip) || roundTrip.x != client.x || roundTrip.y != client.y)
            throw ObservationUnavailable("native category coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(source), GetCurrentThreadId(),
            active, item->layoutKey, keys, index, client, screen, bounds, viewport, app.layoutSaveRevision_};
    }
    struct NativeCategorySourceState
    {
        std::uintptr_t host = 0, container = 0;
        DWORD ownerThread = 0;
        DesktopWidget widget;
        std::wstring activeCategory;
        std::vector<std::wstring> visibleKeys;
        std::uint64_t layoutRevision = 0;
    };
    static NativeCategorySourceState InspectNativeCategoryState(DesktopApp& app)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !completionWindow || !IsWindow(completionWindow) ||
            app.hwnd_ != completionWindow || GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("native category state host unavailable");
        FileCategories* source = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<FileCategories*>(container.get());
            if (!widget || !widget->GetWidgetData() || widget->GetWidgetData()->id != L"native-origin") continue;
            if (source) throw ObservationUnavailable("native category state source ambiguous");
            source = widget;
        }
        if (!source || source->IsHosted() || source->IsPopupHosted() || source->IsCollapsed() ||
            source->GetWidgetData()->type != DesktopWidgetType::FileCategories ||
            source->GetWidgetData()->singleCellIconMode || source->GetWidgetData()->titleBarCollapsed ||
            source->GetWidgetData()->showSearchBox || source->GetWidgetData()->dateHeaders)
            throw ObservationUnavailable("native category state source unavailable");
        const auto active = source->CachedActiveCategoryId();
        const auto visible = source->GetSearchResultKeys();
        if (active.empty()) throw ObservationUnavailable("native category active view unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(source), GetCurrentThreadId(),
            *source->GetWidgetData(), active, visible, app.layoutSaveRevision_};
    }
    struct NativeCollectionReorderGeometry
    {
        std::uintptr_t host = 0, container = 0;
        DWORD ownerThread = 0;
        std::wstring movingKey, targetKey;
        std::vector<std::wstring> itemKeys;
        std::size_t movingIndex = 0, targetIndex = 0;
        RECT movingBounds{}, targetBounds{}, viewport{};
        POINT movingClient{}, targetClient{}, movingScreen{}, targetScreen{};
        std::uint64_t layoutRevision = 0;
    };
    static NativeCollectionReorderGeometry InspectNativeCollectionReorderGeometry(
        DesktopApp& app, const std::filesystem::path& moving, const std::filesystem::path& target)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (asyncOwner != &app || productionDispatch != 1 || !nativePointerDispatch || !completionWindow ||
            !IsWindow(completionWindow) || app.hwnd_ != completionWindow ||
            GetWindowThreadProcessId(completionWindow, nullptr) != GetCurrentThreadId())
            throw ObservationUnavailable("native reorder host unavailable");
        const auto findItem = [&](const std::filesystem::path& path) -> const DesktopItem* {
            const DesktopItem* result = nullptr;
            for (const auto& item : app.items_)
            {
                std::error_code error; const bool same = std::filesystem::equivalent(item.parsingName, path, error);
                if (error) throw ObservationUnavailable("native reorder physical identity unavailable");
                if (!same) continue;
                if (result) throw ObservationUnavailable("native reorder item ambiguous");
                result = &item;
            }
            return result;
        };
        const auto* movingItem = findItem(moving); const auto* targetItem = findItem(target);
        if (!movingItem || !targetItem || movingItem == targetItem || movingItem->selected || targetItem->selected ||
            movingItem->layoutKey == targetItem->layoutKey)
            throw ObservationUnavailable("native reorder input members unavailable");
        Collection* source = nullptr;
        for (const auto& container : app.containers_)
        {
            auto* widget = dynamic_cast<Collection*>(container.get());
            if (!widget || !widget->GetWidgetData() || widget->GetWidgetData()->id != L"native-origin") continue;
            if (source) throw ObservationUnavailable("native reorder source ambiguous");
            source = widget;
        }
        if (!source || source->IsHosted() || source->IsPopupHosted() || source->IsCollapsed() ||
            source->GetWidgetData()->type != DesktopWidgetType::Collection || source->GetWidgetData()->singleCellIconMode ||
            source->GetWidgetData()->titleBarCollapsed || source->GetWidgetData()->showSearchBox || source->GetWidgetData()->dateHeaders)
            throw ObservationUnavailable("native reorder expanded collection unavailable");
        const auto keys = source->GetWidgetData()->itemKeys;
        const auto uniqueIndex = [&](const std::wstring& key) {
            const auto found = std::find(keys.begin(), keys.end(), key);
            if (found == keys.end() || std::find(std::next(found), keys.end(), key) != keys.end())
                throw ObservationUnavailable("native reorder member key unavailable");
            return static_cast<std::size_t>(found - keys.begin());
        };
        const auto from = uniqueIndex(movingItem->layoutKey), to = uniqueIndex(targetItem->layoutKey);
        const auto movingBounds = source->GetMemberLayoutRect(from), targetBounds = source->GetMemberLayoutRect(to);
        const auto viewport = source->GetContentViewportRect();
        RECT movingVisible{}, targetVisible{};
        if (!IntersectRect(&movingVisible, &movingBounds, &viewport) || !IntersectRect(&targetVisible, &targetBounds, &viewport) ||
            movingVisible.right <= movingVisible.left || movingVisible.bottom <= movingVisible.top ||
            targetVisible.right <= targetVisible.left || targetVisible.bottom <= targetVisible.top)
            throw ObservationUnavailable("native reorder members not visible");
        const auto point = [](const RECT& rect, bool upper) {
            const auto x = static_cast<std::int64_t>(rect.left) + (static_cast<std::int64_t>(rect.right) - rect.left) / 2;
            const auto height = static_cast<std::int64_t>(rect.bottom) - rect.top;
            const auto y = static_cast<std::int64_t>(rect.top) + (upper ? (height - 1) / 4 : height / 2);
            if (x < (std::numeric_limits<LONG>::min)() || x > (std::numeric_limits<LONG>::max)() ||
                y < (std::numeric_limits<LONG>::min)() || y > (std::numeric_limits<LONG>::max)())
                throw ObservationUnavailable("native reorder point unavailable");
            return POINT{static_cast<LONG>(x), static_cast<LONG>(y)};
        };
        const auto fromClient = point(movingVisible, false), toClient = point(targetVisible, true);
        POINT fromScreen = fromClient, toScreen = toClient;
        if (!ClientToScreen(completionWindow, &fromScreen) || !ClientToScreen(completionWindow, &toScreen))
            throw ObservationUnavailable("native reorder screen conversion unavailable");
        POINT fromRound = fromScreen, toRound = toScreen;
        if (!ScreenToClient(completionWindow, &fromRound) || !ScreenToClient(completionWindow, &toRound) ||
            fromRound.x != fromClient.x || fromRound.y != fromClient.y || toRound.x != toClient.x || toRound.y != toClient.y)
            throw ObservationUnavailable("native reorder coordinate roundtrip unavailable");
        return {reinterpret_cast<std::uintptr_t>(&app), reinterpret_cast<std::uintptr_t>(source), GetCurrentThreadId(),
            movingItem->layoutKey, targetItem->layoutKey, keys, from, to, movingBounds, targetBounds, viewport,
            fromClient, toClient, fromScreen, toScreen, app.layoutSaveRevision_};
    }
    static bool NativeCollectionReorderSourceCaptured(DesktopApp& app, const std::filesystem::path& moving,
        std::uintptr_t expectedContainer)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (!expectedContainer || !app.dragSession_.IsActive() || app.dragSession_.Items().size() != 1) return false;
        const auto* capturedSource = app.dragSession_.Source();
        Collection* source = nullptr; std::size_t live = 0, named = 0;
        for (const auto& container : app.containers_)
        {
            auto* collection = dynamic_cast<Collection*>(container.get());
            if (collection && collection->GetWidgetData() && collection->GetWidgetData()->id == L"native-origin") ++named;
            if (container.get() == capturedSource) { ++live; source = collection; }
        }
        if (live != 1 || named != 1 || !source || reinterpret_cast<std::uintptr_t>(source) != expectedContainer ||
            !source->GetWidgetData() || source->GetWidgetData()->id != L"native-origin" ||
            source->GetWidgetData()->type != DesktopWidgetType::Collection || source->IsHosted() ||
            source->IsPopupHosted() || source->IsCollapsed()) return false;
        const auto paths = app.dragSession_.SourceList().FilePaths();
        if (paths.size() != 1) return false;
        std::error_code error;
        const bool same = std::filesystem::equivalent(std::filesystem::path(paths.front()), moving, error);
        if (error) throw ObservationUnavailable("native reorder captured source identity unavailable");
        return same;
    }
    // Records only the live container and region; it deliberately does not
    // dereference a potentially stale target Slot or infer its member.
    static bool NativeCollectionReorderRouteObserved(DesktopApp& app, const std::filesystem::path& moving,
        std::uintptr_t expectedContainer)
    {
        if (!NativeCollectionReorderSourceCaptured(app, moving, expectedContainer) || !app.dragSession_.TargetSlot() ||
            app.dragSession_.TargetRegion() != HitRegion::SortBefore) return false;
        Container* target = app.dragSession_.TargetContainer();
        std::size_t live = 0;
        for (const auto& container : app.containers_)
            if (auto* collection = dynamic_cast<Collection*>(container.get());
                container.get() == target && collection && reinterpret_cast<std::uintptr_t>(collection) == expectedContainer) ++live;
        return live == 1 && app.dragSession_.Source() == target;
    }
    struct NativeLandingGeometry
    {
        GridCell cell;
        RECT clientCell{}, screenPage{};
        POINT screenCenter{};
        int columns = 0, rows = 0;
        std::uint64_t layoutRevision = 0;
    };
    // Restricted initial-scene observation. It neither resolves a drag target
    // nor commits a landing; the actual WndProc and business oracle must do so.
    static NativeLandingGeometry InspectNativeLanding(DesktopApp& app, const GridCell& cell)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (app.gridPages_.size() != 1 || app.items_.size() != 2 || app.widgets_.size() != 1 ||
            app.widgets_.front().id != L"native-origin" || app.generalSettings_.dockEnabled ||
            !NativeDragIdle(app) || !ContainerWorkIdle(app))
            throw ObservationUnavailable("native landing restricted scene unavailable");
        const auto& page = app.gridPages_.front();
        if (cell.pageId != page.id || cell.column < 0 || cell.row < 0 ||
            cell.column >= page.columns || cell.row >= page.rows)
            throw ObservationUnavailable("native landing is outside the observed page");
        for (const auto& item : app.items_)
        {
            if (item.name.empty() || item.gridCell.pageId != cell.pageId || app.IsItemInAnyWidget(item)) continue;
            const auto right = static_cast<std::int64_t>(item.gridCell.column) + item.gridSpan.columns;
            const auto bottom = static_cast<std::int64_t>(item.gridCell.row) + item.gridSpan.rows;
            if (cell.column >= item.gridCell.column && cell.column < right && cell.row >= item.gridCell.row && cell.row < bottom)
                throw ObservationUnavailable("native landing initial item occupancy unavailable");
        }
        for (const auto& widget : app.widgets_)
        {
            if (widget.gridCell.pageId != cell.pageId) continue;
            const auto right = static_cast<std::int64_t>(widget.gridCell.column) + widget.gridSpan.columns;
            const auto bottom = static_cast<std::int64_t>(widget.gridCell.row) + widget.gridSpan.rows;
            if (cell.column >= widget.gridCell.column && cell.column < right && cell.row >= widget.gridCell.row && cell.row < bottom)
                throw ObservationUnavailable("native landing overlaps a widget");
        }
        const RECT rect = GetGridRect(app.gridPages_, cell, {1, 1});
        if (rect.right <= rect.left || rect.bottom <= rect.top || page.bounds.right <= page.bounds.left ||
            page.bounds.bottom <= page.bounds.top || rect.left < page.bounds.left || rect.top < page.bounds.top ||
            rect.right > page.bounds.right || rect.bottom > page.bounds.bottom)
            throw ObservationUnavailable("native landing grid geometry unavailable");
        const auto x = static_cast<std::int64_t>(rect.left) + (static_cast<std::int64_t>(rect.right) - rect.left) / 2;
        const auto y = static_cast<std::int64_t>(rect.top) + (static_cast<std::int64_t>(rect.bottom) - rect.top) / 2;
        if (x < (std::numeric_limits<LONG>::min)() || x > (std::numeric_limits<LONG>::max)() ||
            y < (std::numeric_limits<LONG>::min)() || y > (std::numeric_limits<LONG>::max)())
            throw ObservationUnavailable("native landing center unavailable");
        const auto center = ExternalPoint({static_cast<LONG>(x), static_cast<LONG>(y)});
        const auto topLeft = ExternalPoint({page.bounds.left, page.bounds.top});
        const auto bottomRight = ExternalPoint({page.bounds.right, page.bounds.bottom});
        return {cell, rect, RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y},
            POINT{center.x, center.y}, page.columns, page.rows, LayoutRevision(app)};
    }
    // These values are read from the live isolated desktop model. They do not
    // perform hit testing, select the item or authorize a pointer dispatch.
    static NativeLandingGeometry InspectNativeDesktopLanding(DesktopApp& app, const GridCell& cell)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (app.gridPages_.size() != 1 || app.items_.size() != 2 || !app.widgets_.empty() ||
            app.generalSettings_.dockEnabled || !NativeDragIdle(app) || !ContainerWorkIdle(app))
            throw ObservationUnavailable("native desktop landing restricted scene unavailable");
        const auto& page = app.gridPages_.front();
        if (cell.pageId != page.id || cell.column < 0 || cell.row < 0 || cell.column >= page.columns || cell.row >= page.rows)
            throw ObservationUnavailable("native desktop landing is outside the observed page");
        for (const auto& item : app.items_)
        {
            if (item.gridCell.pageId != cell.pageId || item.name.empty() || item.gridSpan.columns <= 0 ||
                item.gridSpan.rows <= 0 || item.gridCell.column < 0 || item.gridCell.row < 0 ||
                static_cast<std::int64_t>(item.gridCell.column) + item.gridSpan.columns > page.columns ||
                static_cast<std::int64_t>(item.gridCell.row) + item.gridSpan.rows > page.rows || app.IsItemInAnyWidget(item))
                throw ObservationUnavailable("native desktop item occupancy unavailable");
            const auto right = static_cast<std::int64_t>(item.gridCell.column) + item.gridSpan.columns;
            const auto bottom = static_cast<std::int64_t>(item.gridCell.row) + item.gridSpan.rows;
            if (cell.column >= item.gridCell.column && cell.column < right && cell.row >= item.gridCell.row && cell.row < bottom)
                throw ObservationUnavailable("native desktop landing initially occupied");
        }
        const RECT rect = GetGridRect(app.gridPages_, cell, {1, 1});
        if (rect.right <= rect.left || rect.bottom <= rect.top || page.bounds.right <= page.bounds.left ||
            page.bounds.bottom <= page.bounds.top || rect.left < page.bounds.left || rect.top < page.bounds.top ||
            rect.right > page.bounds.right || rect.bottom > page.bounds.bottom)
            throw ObservationUnavailable("native desktop landing grid geometry unavailable");
        const auto x = static_cast<std::int64_t>(rect.left) + (static_cast<std::int64_t>(rect.right) - rect.left) / 2;
        const auto y = static_cast<std::int64_t>(rect.top) + (static_cast<std::int64_t>(rect.bottom) - rect.top) / 2;
        if (x < (std::numeric_limits<LONG>::min)() || x > (std::numeric_limits<LONG>::max)() ||
            y < (std::numeric_limits<LONG>::min)() || y > (std::numeric_limits<LONG>::max)())
            throw ObservationUnavailable("native desktop landing center unavailable");
        const auto center = ExternalPoint({static_cast<LONG>(x), static_cast<LONG>(y)});
        const auto topLeft = ExternalPoint({page.bounds.left, page.bounds.top});
        const auto bottomRight = ExternalPoint({page.bounds.right, page.bounds.bottom});
        return {cell, rect, RECT{topLeft.x, topLeft.y, bottomRight.x, bottomRight.y},
            POINT{center.x, center.y}, page.columns, page.rows, LayoutRevision(app)};
    }
    struct NativeDesktopSourceGeometry
    {
        RECT clientBounds{};
        POINT screenCenter{};
        GridCell cell;
        std::uint64_t layoutRevision = 0;
    };
    static NativeDesktopSourceGeometry InspectNativeDesktopSource(DesktopApp& app, const std::filesystem::path& path)
    {
        using drag_global::ole_driver::ObservationUnavailable;
        if (app.gridPages_.size() != 1 || app.items_.size() != 2 || !app.widgets_.empty() || app.generalSettings_.dockEnabled ||
            !NativeDragIdle(app) || !ContainerWorkIdle(app))
            throw ObservationUnavailable("native desktop source restricted scene unavailable");
        const DesktopItem* found = nullptr;
        for (const auto& item : app.items_)
        {
            std::error_code error;
            const bool same = std::filesystem::equivalent(item.parsingName, path, error);
            if (error) throw ObservationUnavailable("native desktop source identity unavailable");
            if (!same) continue;
            if (found) throw ObservationUnavailable("native desktop source identity ambiguous");
            found = &item;
        }
        if (!found || found->selected || found->name.empty() || app.IsItemInAnyWidget(*found) ||
            found->gridCell.pageId != app.gridPages_.front().id || found->bounds.right <= found->bounds.left ||
            found->bounds.bottom <= found->bounds.top)
            throw ObservationUnavailable("native desktop source baseline or bounds unavailable");
        const auto x = static_cast<std::int64_t>(found->bounds.left) +
            (static_cast<std::int64_t>(found->bounds.right) - found->bounds.left) / 2;
        const auto y = static_cast<std::int64_t>(found->bounds.top) +
            (static_cast<std::int64_t>(found->bounds.bottom) - found->bounds.top) / 2;
        if (x < (std::numeric_limits<LONG>::min)() || x > (std::numeric_limits<LONG>::max)() ||
            y < (std::numeric_limits<LONG>::min)() || y > (std::numeric_limits<LONG>::max)())
            throw ObservationUnavailable("native desktop source center unavailable");
        const auto screen = ExternalPoint({static_cast<LONG>(x), static_cast<LONG>(y)});
        return {found->bounds, POINT{screen.x, screen.y}, found->gridCell, LayoutRevision(app)};
    }
    static const auto& Widgets(const DesktopApp& app) { return app.widgets_; }
    struct SceneFolderEntry
    {
        std::wstring name, displayName, fullPath, typeName;
        bool isDirectory = false, selected = false;
        std::optional<std::uint64_t> fileSize;
        FILETIME lastWriteTime{};
    };
    struct SceneWidget
    {
        std::wstring id, title, customTitle, sourceFolderPath, sourceDesktopItemKey, activeCategoryId;
        DesktopWidgetType type;
        GridCell cell;
        GridSpan span;
        bool selected = false, autoCollect = false, iconMode = false, privacyMode = false;
        int folderSortMode = 0;
        bool folderSortAscending = false, contentSortAscending = false;
        snowdesktop::list_detail_rules::Column contentSortColumn;
        std::vector<std::wstring> itemKeys, childWidgetIds;
        std::vector<MissingDesktopWidgetItem> missingItems;
        std::vector<SceneFolderEntry> folderEntries;
    };
    struct SceneItem
    {
        std::wstring name, parsingName, layoutKey, desktopIconClsid;
        GridCell cell;
        GridSpan span;
        RECT bounds{};
        bool selected = false;
    };
    struct NativeSceneSnapshot
    {
        std::vector<SceneWidget> widgets;
        std::vector<SceneItem> items;
        std::vector<DockEntry> dock;
        std::vector<std::string> unresolved{"visible_slot_cache_and_desktop_ownership_unobserved",
            "remaining_widget_configuration_and_non_native_surfaces_unobserved"};
    };
    // Copy stable model values without rebuilding slot caches or retaining
    // pointers that production may invalidate. Runtime slots remain unresolved.
    static NativeSceneSnapshot CaptureNativeScene(DesktopApp& app)
    {
        NativeSceneSnapshot snapshot;
        snapshot.dock = app.dockEntries_;
        for (const auto& widget : app.widgets_)
        {
            SceneWidget value{widget.id, widget.title, widget.customTitle, widget.sourceFolderPath,
                widget.sourceDesktopItemKey, widget.activeCategoryId, widget.type, widget.gridCell, widget.gridSpan,
                widget.selected, widget.autoCollect, widget.singleCellIconMode, widget.privacyMode,
                widget.folderSortMode, widget.folderSortAscending, widget.contentSortAscending,
                widget.contentSortColumn, widget.itemKeys, widget.childWidgetIds, widget.missingItems, {}};
            for (const auto& entry : widget.folderEntries)
                value.folderEntries.push_back({entry.name, entry.displayName, entry.fullPath, entry.typeName,
                    entry.isDirectory, entry.selected, entry.fileSize, entry.lastWriteTime});
            snapshot.widgets.push_back(std::move(value));
        }
        for (const auto& item : app.items_)
            snapshot.items.push_back({item.name, item.parsingName, item.layoutKey, item.desktopIconClsid,
                item.gridCell, item.gridSpan, item.bounds, item.selected});
        return snapshot;
    }
    struct DragCapturedEntry
    {
        DropSourceKind kind;
        std::size_t sourceIndex, desktopIndex, memberIndex;
        std::wstring desktopKey, filePath, displayName, dockReference, widgetId;
        DockEntryType dockEntryType;
        bool fromDock, protectedDesktopIcon, desktopWidgetIcon;
        std::optional<DesktopWidgetType> widgetType;
        GridCell originalCell;
        GridSpan originalSpan;
    };
    struct NativeDragSnapshot
    {
        bool active = false;
        bool sourceIsDesktop = false, targetIsDesktop = false, targetUnknown = false, targetSlotPresent = false;
        bool targetSlotIdentityUnresolved = false, targetContentIdentityUnresolved = false;
        bool desktopIdentityUnresolved = false, targetObservationAvailable = false;
        bool hasOriginSurface = false, hasOriginWidget = false, hasDesktopIcons = false, hasFolderEntries = false;
        bool hasExternalFiles = false, hasWidgets = false, hasCollectionGroupEntries = false;
        bool hasFileGroupEntries = false, hasFileGroupSourceLabels = false, hasSearchResults = false;
        snowdesktop::slot_contract::SlotSurfaceKind originSurface = snowdesktop::slot_contract::SlotSurfaceKind::Desktop;
        DesktopWidgetType originWidgetType = DesktopWidgetType::Collection;
        std::wstring originWidgetId, searchViewId, targetWidgetId;
        HitRegion targetRegion = HitRegion::None;
        std::vector<DragCapturedEntry> entries;
    };
    static NativeDragSnapshot CaptureNativeDrag(const DesktopApp& app)
    {
        NativeDragSnapshot snapshot;
        snapshot.active = app.dragSession_.IsActive();
        if (!snapshot.active) return snapshot;
        const auto& source = app.dragSession_.SourceList();
        const auto* desktop = app.containers_.empty() ? nullptr : dynamic_cast<const DesktopGrid*>(app.containers_.front().get());
        snapshot.desktopIdentityUnresolved = desktop == nullptr;
        snapshot.sourceIsDesktop = desktop && source.origin == desktop;
        snapshot.hasOriginSurface = source.hasOriginSurface; snapshot.originSurface = source.originSurface;
        snapshot.hasOriginWidget = source.hasOriginWidget; snapshot.originWidgetType = source.originWidgetType;
        snapshot.originWidgetId = source.originWidgetId; snapshot.searchViewId = source.searchViewId;
        snapshot.hasDesktopIcons = source.hasDesktopIcons; snapshot.hasFolderEntries = source.hasFolderEntries;
        snapshot.hasExternalFiles = source.hasExternalFiles; snapshot.hasWidgets = source.hasWidgets;
        snapshot.hasCollectionGroupEntries = source.hasCollectionGroupEntries; snapshot.hasFileGroupEntries = source.hasFileGroupEntries;
        snapshot.hasFileGroupSourceLabels = source.hasFileGroupSourceLabels; snapshot.hasSearchResults = source.hasSearchResults;
        const auto* target = app.dragSession_.TargetContainer();
        const auto owned = std::find_if(app.containers_.begin(), app.containers_.end(),
            [&](const auto& container) { return container.get() == target; });
        snapshot.targetIsDesktop = target && desktop && target == desktop;
        if (target && owned == app.containers_.end()) snapshot.targetUnknown = true;
        else if (const auto* widget = dynamic_cast<const WidgetContainer*>(target); widget && widget->GetWidgetData())
        {
            const auto* data = widget->GetWidgetData();
            const auto found = std::find_if(app.widgets_.begin(), app.widgets_.end(),
                [&](const auto& candidate) { return &candidate == data; });
            if (found == app.widgets_.end()) snapshot.targetUnknown = true;
            else snapshot.targetWidgetId = found->id;
            // Resolving a group's active view may rebuild mutable caches and
            // does not necessarily identify the hit child. Preserve uncertainty.
            snapshot.targetContentIdentityUnresolved = true;
        }
        else if (target && !snapshot.targetIsDesktop) snapshot.targetUnknown = true;
        if (!snapshot.targetUnknown)
        {
            snapshot.targetObservationAvailable = true;
            snapshot.targetRegion = app.dragSession_.TargetRegion();
            snapshot.targetSlotPresent = app.dragSession_.TargetSlot() != nullptr;
            // A transient slot can disappear independently of its parent;
            // never dereference it or rebuild a cache merely to observe it.
            snapshot.targetSlotIdentityUnresolved = snapshot.targetSlotPresent;
        }
        for (const auto& entry : source.entries)
            snapshot.entries.push_back({entry.kind, entry.sourceIndex, entry.desktopIndex, entry.memberIndex,
                entry.desktopKey, entry.filePath, entry.displayName, entry.dockReference, entry.widgetId,
                entry.dockEntryType, entry.fromDock, entry.protectedDesktopIcon, entry.desktopWidgetIcon,
                entry.widgetType, entry.originalCell, entry.originalSpan});
        return snapshot;
    }
    struct RightMenuBridge final : OleDragDropHandler
    {
        DesktopApp& app;
        DesktopApp::RightDragMenuPresenter menu;
        DWORD dropKeys = 0;
        RightMenuBridge(DesktopApp& host, DesktopApp::RightDragMenuPresenter presenter)
            : app(host), menu(std::move(presenter)) {}
        HRESULT HandleOleDragEnter(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
        { return app.HandleOleDragEnter(data, keys, point, effect); }
        HRESULT HandleOleDragOver(DWORD keys, POINTL point, DWORD* effect) override
        { return app.HandleOleDragOver(keys, point, effect); }
        HRESULT HandleOleDragLeave() override { return app.HandleOleDragLeave(); }
        HRESULT HandleOleDrop(IDataObject* data, DWORD keys, POINTL point, DWORD* effect) override
        {
            dropKeys = keys;
            return app.HandleOleDropWithMenu(data, keys, point, effect, menu);
        }
        HRESULT HandleOleQueryContinueDrag(BOOL escape, DWORD keys) override
        { return app.HandleOleQueryContinueDrag(escape, keys); }
        HRESULT HandleOleGiveFeedback(DWORD effect) override { return app.HandleOleGiveFeedback(effect); }
    };
#include "container_access.inc"
#include "module_access.inc"
};

#include "core_drag_matrix.h"
#include "drag_field_inventory.h"
#include "global_drag_model.h"

namespace behavior
{
namespace fs = std::filesystem;
using Access = NativeDragIntegrationAccess;
std::string scene;
void Require(bool ok, const std::string& message)
{ if (!ok) throw std::runtime_error(scene + ": " + message); }
void RequireBusiness(bool ok, const std::string& message)
{ if (!ok) throw drag_global::ole_driver::BusinessAssertionFailure(scene + ": " + message); }
void RequireObservation(bool ok, const std::string& message)
{ if (!ok) throw drag_global::ole_driver::ObservationUnavailable(scene + ": " + message); }
void RequireEvent(bool ok, const std::string& message)
{ if (!ok) throw drag_global::ole_driver::EventAssertionFailure(scene + ": " + message); }
void Write(const fs::path& file, std::string_view bytes)
{
    fs::create_directories(file.parent_path());
    std::ofstream stream(file, std::ios::binary); stream << bytes;
    Require(static_cast<bool>(stream), "write isolated input");
}
std::string Read(const fs::path& file)
{
    std::ifstream stream(file, std::ios::binary);
    Require(static_cast<bool>(stream), "read physical output");
    return {std::istreambuf_iterator<char>(stream), {}};
}
void RequireObservedFilePayload(const ole_file_source_fixture::Source& source,
    const std::vector<fs::path>& expected)
{
    Require(!source.fileDropObservationFailed && !source.formatProbeObservationFailed && source.fileReads >= 0 &&
        source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads),
        "observe every production file-data read without changing the provider result");
    bool captured = false;
    for (const auto& event : source.fileDropReads)
    {
        Require(event.threadId == source.owner, "file-data event runs on the source apartment thread");
        if (source.asynchronous)
            Require(event.startsAtEntry > event.endsAtEntry,
                "asynchronous file-data event occurs while the source lease is active");
        if (FAILED(event.result)) continue;
        Require(event.capture == ole_file_source_fixture::FileDropCapture::captured &&
            event.paths.size() == expected.size(), "actual CF_HDROP event contains the selected member count");
        for (std::size_t i = 0; i < expected.size(); ++i)
        {
            const auto actual = event.paths[i].wstring();
            const auto wanted = expected[i].wstring();
            Require(CompareStringOrdinal(actual.c_str(), -1, wanted.c_str(), -1, TRUE) == CSTR_EQUAL,
                "Shell provider's actual CF_HDROP paths and order match the requested fixture input");
        }
        captured = true;
    }
    Require(captured, "accepted file drop must expose a successfully observed file-data event");
}
template<class Predicate> void Wait(Predicate done)
{
    const auto deadline = GetTickCount64() + 12000;
    Access::ThrowAsyncException();
    while (!done())
    {
        Require(GetTickCount64() < deadline, "bounded completion deadline");
        MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) DispatchMessageW(&message);
        Access::ThrowAsyncException();
    }
    Access::ThrowAsyncException();
}
bool Same(const GridCell& a, const GridCell& b)
{ return a.pageId == b.pageId && a.column == b.column && a.row == b.row; }
const DesktopItem& Find(DesktopApp& app, const fs::path& path)
{
    const auto& items = Access::Items(app);
    const auto it = std::find_if(items.begin(), items.end(), [&](const auto& v) {
        std::error_code error;
        return fs::equivalent(fs::path(v.parsingName), path, error) && !error;
    });
    if (it == items.end())
    {
        std::cerr << "Expected path: " << WideToUtf8(path.wstring()) << " model count=" << items.size() << '\n';
        for (const auto& item : items) std::cerr << "Observed: " << WideToUtf8(item.layoutKey) << " path=" << WideToUtf8(item.parsingName) << '\n';
        Require(false, "physical output must be present in refreshed model");
    }
    return *it;
}
// The isolated copy profile contains filesystem-backed desktop items. An
// identity-query failure is unavailable evidence; a complete scan with no
// matching item or duplicate matches is a business-model mismatch.
const DesktopItem& FindCopyBusiness(DesktopApp& app, const fs::path& path)
{
    const DesktopItem* found = nullptr;
    for (const auto& item : Access::Items(app))
    {
        std::error_code error;
        const bool same = fs::equivalent(fs::path(item.parsingName), path, error);
        if (error) throw drag_global::ole_driver::ObservationUnavailable("desktop model file identity unavailable");
        if (!same) continue;
        RequireBusiness(!found, "desktop model must contain a physical item only once");
        found = &item;
    }
    RequireBusiness(found != nullptr, "physical output must be present in the refreshed model");
    return *found;
}
struct Profile
{
    fs::path root, desktop;
    DesktopApp* app = nullptr;
    HWND ownedCompletionWindow = nullptr;
    DWORD ownerThread = 0;
    bool apartmentInitialized = false;
    bool ownsRoot = false;
    bool ownsReservation = false;
    using DesktopSetup = std::function<fs::path(const fs::path&)>;
    explicit Profile(const DesktopSetup& setupDesktop = {})
    {
      try
      {
        bool unreserved = false;
        Require(Access::profileReserved.compare_exchange_strong(unreserved, true),
            "profile fixture requires an exclusive process reservation");
        ownsReservation = true;
        ownerThread = GetCurrentThreadId();
        Require(!Access::completionWindow && !Access::asyncOwner && !Access::fixtureControlWindow,
            "profile fixture requires exclusive ownership of its message windows");
        Require(SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)), "initialize STA");
        apartmentInitialized = true;
        GUID guid{}; Require(SUCCEEDED(CoCreateGuid(&guid)), "allocate profile identity");
        wchar_t id[40]{}; StringFromGUID2(guid, id, 40);
        root = fs::weakly_canonical(fs::temp_directory_path()) / (std::wstring(L"SnowBehavior-") + id);
        std::error_code rootError;
        ownsRoot = fs::create_directory(root, rootError);
        Require(ownsRoot && !rootError, "create a new exclusively owned profile root");
        namespace debug = snowdesktop::debug_profile;
        const auto paths = debug::ResolvePaths(root / L"normal-data");
        debug::Configuration config; config.enabled = true; config.desktop = paths.defaultDesktop;
        if (setupDesktop)
        {
            const auto proposed = setupDesktop(root);
            std::error_code ec;
            const auto canonicalRoot = fs::weakly_canonical(root, ec);
            Require(!ec, "resolve isolated profile root");
            const auto canonicalDesktop = fs::weakly_canonical(proposed, ec);
            Require(!ec && fs::is_directory(canonicalDesktop), "resolve prepared target directory");
            const auto relative = canonicalDesktop.lexically_relative(canonicalRoot);
            Require(!relative.empty() && relative != fs::path(L".") &&
                relative.begin() != relative.end() && *relative.begin() != fs::path(L".."),
                "prepared target must remain inside the isolated profile");
            config.desktop = canonicalDesktop;
        }
        std::string error;
        Require(debug::Write(paths, config, error) && debug::Initialize(paths.normalData, {}, error), "isolate profile: " + error);
        desktop = debug::Current().configuration.desktop;
        if (setupDesktop)
        {
            std::error_code ec;
            Require(fs::equivalent(desktop, config.desktop, ec) && !ec,
                "debug host binds the actual prepared target directory");
        }
        ownedCompletionWindow = CreateWindowExW(0, L"STATIC", L"Behavior completion", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, GetModuleHandleW(nullptr), nullptr);
        Require(ownedCompletionWindow != nullptr, "create owned message-only window");
        Access::completionWindow = ownedCompletionWindow;
        // No Run(): no desktop window, hooks, Explorer or user's files are involved.
        app = &Access::Scene(false);
        fs::create_directories(root / L"layout");
        Access::ConfigureModel(*app, root / L"layout");
        Require(!setupDesktop || !fs::exists(desktop / L"Sentinel.txt"),
            "prepared target does not already contain the fixture sentinel");
        Write(desktop / L"Sentinel.txt", "untouched");
        Access::PopulateModel(*app, {desktop / L"Sentinel.txt"}, 0);
        Access::PrepareReload(*app); Access::BindAsync(*app);
        if (setupDesktop)
        {
            // Read the real target contents through the production Shell
            // refresh; the initial sentinel is only bootstrap fixture input.
            std::vector<fs::path> entries;
            for (const auto& entry : fs::directory_iterator(desktop))
            {
                const auto attributes = GetFileAttributesW(entry.path().c_str());
                Require(attributes != INVALID_FILE_ATTRIBUTES &&
                    !(attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_REPARSE_POINT)),
                    "prepared target visibility or reparse behavior is not supported by this fixture");
                entries.push_back(entry.path());
            }
            Access::RequestAsync(*app);
            Wait([&] { return Access::FileDropIdle(*app); });
            Require(Access::Items(*app).size() == entries.size(), "production refresh sees every prepared target entry");
            for (const auto& entry : entries) static_cast<void>(Find(*app, entry));
        }
        Require(Access::Save(*app), "persist starting layout");
      }
      catch (...) { Cleanup(); throw; }
    }
    ~Profile() { Cleanup(); }
    void Cleanup() noexcept
    {
        if (!ownsReservation) return;
        if (ownerThread && ownerThread != GetCurrentThreadId())
        {
            Access::wrongThreadShutdown.store(true);
            try { std::wcerr << L"Wrong-thread profile shutdown; retaining owned resources: " << root.wstring() << L'\n'; }
            catch (...) { /* Keep the cross-thread failure flag. */ }
            return;
        }
        // Only this profile's bound window may be checked or retained. The
        // pointer session must deliver end/cancel before its host is stopped.
        if (ownedCompletionWindow && Access::completionWindow == ownedCompletionWindow &&
            Access::nativePointerDispatch && snowdesktop::private_pointer::Active())
        {
            try { throw std::runtime_error("private pointer session must end before profile shutdown"); }
            catch (...) { Access::asyncException = std::current_exception(); }
            try { std::wcerr << L"Active pointer session; retaining owned window and directory: " << root.wstring() << L'\n'; }
            catch (...) { /* Preserve the session-order failure. */ }
            return;
        }
        // If shutdown fails, retain the owned window and directory: workers may
        // still be using them. A failed constructor also runs this same path.
        bool stopped = !app;
        if (app)
        {
            try { Access::StopModel(*app); stopped = true; }
            catch (...)
            {
                stopped = false;
                Access::asyncException = std::current_exception();
                try { std::wcerr << L"Profile shutdown failed; retaining owned window and directory: " << root.wstring() << L'\n'; }
                catch (...) { /* Keep the original shutdown exception. */ }
            }
        }
        if (!stopped) return;
        app = nullptr;
        if (ownedCompletionWindow)
        {
            if (IsWindow(ownedCompletionWindow) && !DestroyWindow(ownedCompletionWindow) && IsWindow(ownedCompletionWindow))
            {
                try { throw std::runtime_error("owned completion window could not be destroyed"); }
                catch (...) { Access::asyncException = std::current_exception(); }
                try { std::wcerr << L"Window shutdown failed; retaining owned directory: " << root.wstring() << L'\n'; }
                catch (...) { /* Preserve the window-shutdown failure. */ }
                return;
            }
            if (Access::completionWindow == ownedCompletionWindow) Access::completionWindow = nullptr;
            ownedCompletionWindow = nullptr;
        }
        try
        {
            // The uninitialized host graph has process lifetime; its destructor
            // would shut down global Shell services this fixture never started.
            std::error_code error;
            if (ownsRoot && !root.empty())
            {
                const auto resolved = fs::weakly_canonical(root, error);
                if (!error && resolved.parent_path() == fs::weakly_canonical(fs::temp_directory_path()) &&
                    resolved.filename().wstring().starts_with(L"SnowBehavior-")) fs::remove_all(resolved, error);
                else error = std::make_error_code(std::errc::operation_not_permitted);
                if (error) std::wcerr << L"Profile directory retained after cleanup: " << root.wstring() << L'\n';
            }
        }
        catch (...) { try { std::wcerr << L"Profile directory cleanup could not be verified\n"; } catch (...) {} }
        if (apartmentInitialized) { CoUninitialize(); apartmentInitialized = false; }
        Access::profileReserved.store(false);
        ownsReservation = false;
    }
};
// Holds the isolated Profile and desktop directory identities through model
// generation and execution. A path in a checkpoint never grants ownership.
// The shared Profile outlives both directory handles.
struct OwnedProfileDesktopRoot final
{
    std::shared_ptr<Profile> profile;
    HANDLE root = INVALID_HANDLE_VALUE, desktop = INVALID_HANDLE_VALUE;
    FILE_ID_INFO rootId{}, desktopId{};
    DWORD ownerThread = 0;

    explicit OwnedProfileDesktopRoot(std::shared_ptr<Profile> value) : profile(std::move(value)), ownerThread(GetCurrentThreadId())
    {
        using drag_global::ole_driver::ObservationUnavailable;
        try
        {
            if (!profile || !profile->ownsRoot || !profile->ownsReservation || profile->ownerThread != ownerThread || !profile->app ||
                Access::asyncOwner != profile->app || profile->root.empty() || profile->desktop.empty())
                throw ObservationUnavailable("owned profile root unavailable");
            std::error_code error;
            const auto rootPath = fs::weakly_canonical(profile->root, error);
            if (error) throw ObservationUnavailable("owned profile root path unavailable");
            const auto desktopPath = fs::weakly_canonical(profile->desktop, error);
            if (error) throw ObservationUnavailable("owned profile desktop path unavailable");
            const auto relative = desktopPath.lexically_relative(rootPath);
            // The restricted native fixture uses a direct child so holding
            // the root and child handles fences every workspace component.
            if (relative.empty() || relative == fs::path(L".") || relative.has_parent_path() ||
                *relative.begin() == fs::path(L".."))
                throw ObservationUnavailable("owned profile desktop is not a direct root child");
            const auto openDirectory = [](const fs::path& path) {
                return CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                    FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            };
            root = openDirectory(profile->root);
            desktop = openDirectory(profile->desktop);
            if (root == INVALID_HANDLE_VALUE || desktop == INVALID_HANDLE_VALUE ||
                !ReadDirectoryId(root, rootId) || !ReadDirectoryId(desktop, desktopId) || !StillOwned())
                throw ObservationUnavailable("owned profile directory handles or identities unavailable");
        }
        catch (...)
        {
            if (desktop != INVALID_HANDLE_VALUE) CloseHandle(desktop);
            if (root != INVALID_HANDLE_VALUE) CloseHandle(root);
            desktop = root = INVALID_HANDLE_VALUE;
            throw;
        }
    }
    ~OwnedProfileDesktopRoot()
    {
        if (desktop != INVALID_HANDLE_VALUE) CloseHandle(desktop);
        if (root != INVALID_HANDLE_VALUE) CloseHandle(root);
    }
    OwnedProfileDesktopRoot(const OwnedProfileDesktopRoot&) = delete;
    OwnedProfileDesktopRoot& operator=(const OwnedProfileDesktopRoot&) = delete;
    static bool ReadDirectoryId(HANDLE handle, FILE_ID_INFO& id) noexcept
    {
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &attributes, sizeof(attributes)) ||
            !(attributes.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (attributes.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
            !GetFileInformationByHandleEx(handle, FileIdInfo, &id, sizeof(id))) return false;
        return std::any_of(std::begin(id.FileId.Identifier), std::end(id.FileId.Identifier), [](BYTE byte) { return byte != 0; });
    }
    static bool SameId(const FILE_ID_INFO& a, const FILE_ID_INFO& b) noexcept
    {
        return a.VolumeSerialNumber == b.VolumeSerialNumber &&
            std::memcmp(a.FileId.Identifier, b.FileId.Identifier, sizeof(a.FileId.Identifier)) == 0;
    }
    bool StillOwned() const noexcept
    {
        if (!profile || ownerThread != GetCurrentThreadId() || !profile->ownsRoot || !profile->ownsReservation ||
            profile->ownerThread != ownerThread || !profile->app || Access::asyncOwner != profile->app ||
            root == INVALID_HANDLE_VALUE || desktop == INVALID_HANDLE_VALUE) return false;
        FILE_ID_INFO actualRoot{}, actualDesktop{};
        if (!ReadDirectoryId(root, actualRoot) || !ReadDirectoryId(desktop, actualDesktop) ||
            !SameId(rootId, actualRoot) || !SameId(desktopId, actualDesktop)) return false;
        const auto verifyPath = [](const fs::path& path, const FILE_ID_INFO& expected) {
            HANDLE current = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (current == INVALID_HANDLE_VALUE) return false;
            FILE_ID_INFO id{};
            const bool valid = ReadDirectoryId(current, id) && SameId(id, expected);
            CloseHandle(current);
            return valid;
        };
        return verifyPath(profile->root, rootId) && verifyPath(profile->desktop, desktopId);
    }
};
// The normal debug desktop lives below normal-data.debug. A V4 fixture uses a
// direct child of its freshly created profile root so that this owner can
// retain both relevant directory handles for the same isolated host.
std::unique_ptr<OwnedProfileDesktopRoot> CreateV4OwnedProfileDesktopRoot()
{
    auto profile = std::make_shared<Profile>([](const fs::path& root) {
        const auto desktop = root / L"desktop";
        std::error_code error;
        const bool created = fs::create_directory(desktop, error);
        RequireObservation(created && !error, "create the isolated V4 desktop directory");
        return desktop;
    });
    return std::make_unique<OwnedProfileDesktopRoot>(std::move(profile));
}
struct V4DirectoryPreparedInput
{
    drag_global::directory_fixture::FixtureObservation fixture;
    std::shared_ptr<drag_global::directory_fixture::DirectoryCopyLease> lease;
    drag_global::directory_fixture::DirectoryCopyLeaseObservation leaseObservation;
};
V4DirectoryPreparedInput PrepareV4DirectoryInput(const drag_global::V4FamilyDescriptor& family,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    const std::shared_ptr<drag_global::V4OwnedChildDirectoryAttempt>& parent,
    std::size_t maxNodes, std::size_t maxBytes)
{
    V4DirectoryPreparedInput result;
    if (!parent || !parent->available || !parent->created || !parent->identityObserved)
    { result.fixture.unresolved.emplace_back("directory_owned_parent_unavailable"); return result; }
    result.fixture = drag_global::directory_fixture::Prepare(family, verified, identity, parent->physicalPath,
        *parent, maxNodes, maxBytes,
        [&](const auto& fixture, HANDLE root, const std::vector<HANDLE>& nodes) {
            auto handoff = drag_global::directory_fixture::PrepareDirectoryCopyLeaseDuringFixture(
                family, verified, identity, fixture, parent, root, nodes, maxNodes, maxBytes);
            result.lease = std::move(handoff.lease);
            result.leaseObservation = std::move(handoff.observation);
        });
    return result;
}
bool PayloadEndpointsPresent(const drag_global::PayloadFixtureResult& fixture,
    const drag_global::PayloadInputSnapshot& snapshot)
{
    if (!snapshot.captured) return false;
    const auto endpoint = [&](const fs::path& path, const fs::path& relative, std::size_t index) {
        if (path.empty()) return relative.empty() && index == (std::numeric_limits<std::size_t>::max)();
        return !relative.empty() && index < snapshot.entries.size() &&
            snapshot.entries[index].relative == relative && snapshot.entries[index].kind == "directory";
    };
    if (!endpoint(fixture.desktopDirectory, snapshot.desktopRelative, snapshot.desktopEntryIndex) ||
        !endpoint(fixture.sourceMappingDirectory, snapshot.sourceMappingRelative, snapshot.sourceMappingEntryIndex) ||
        !endpoint(fixture.targetMappingDirectory, snapshot.targetMappingRelative, snapshot.targetMappingEntryIndex)) return false;
    return std::any_of(snapshot.entries.begin(), snapshot.entries.end(), [&](const auto& item) {
        return item.relative == snapshot.targetRelative && item.kind == "directory";
    });
}
bool SamePayloadEndpointIndices(const drag_global::PayloadInputSnapshot& left,
    const drag_global::PayloadInputSnapshot& right)
{
    return left.desktopEntryIndex == right.desktopEntryIndex &&
        left.sourceMappingEntryIndex == right.sourceMappingEntryIndex &&
        left.targetMappingEntryIndex == right.targetMappingEntryIndex;
}
// Bind one model-generated source graph to the production host's debug
// desktop, then independently capture the complete actual input baseline.
// Constructing this fixture alone never means the candidate was executed.
struct GeneratedPayloadProfile
{
    drag_global::PayloadFixtureResult payload;
    drag_global::PayloadInputSnapshot before, after;
    std::unique_ptr<Profile> profile;
    // V3 has an independent family descriptor; never pass it through a V1/V2 parser.
    std::optional<drag_global::SelectionSchemaVersion> version = drag_global::SelectionSchemaVersion::legacy_v1;

    GeneratedPayloadProfile(const std::vector<drag_global::Axis>& axes, const drag_global::Candidate& candidate,
        drag_global::SelectionSchemaVersion schemaVersion = drag_global::SelectionSchemaVersion::legacy_v1)
        : version(schemaVersion)
    {
        Initialize([&](const fs::path& parent) {
            return drag_global::PreparePayloadFixture(axes, candidate, parent, false, schemaVersion);
        });
    }
    explicit GeneratedPayloadProfile(const drag_global::V3FamilyValidatedContext& context) : version(std::nullopt)
    {
        if (context.Status() != drag_global::V3CoordinateStatus::coordinate_ready)
            throw drag_global::ole_driver::ObservationUnavailable("v3 generated profile coordinate unavailable");
        Initialize([&](const fs::path& parent) { return drag_global::PrepareV3PayloadFixture(context, parent); });
    }
    template <typename Prepare>
    void Initialize(Prepare&& prepare)
    {
        profile = std::make_unique<Profile>([&](const fs::path& root) {
            const auto parent = root / L"generated-payload";
            fs::create_directories(parent);
            payload = prepare(parent);
            Require(payload.payload_prepared, "prepare generated payload: " + payload.error);
            before = drag_global::CapturePayloadInputSnapshot(payload);
            Require(before.captured && !payload.desktopDirectory.empty() && PayloadEndpointsPresent(payload, before),
                "capture generated payload and explicit directory endpoints before host setup: " + before.error);
            return payload.desktopDirectory;
        });
        after = drag_global::CapturePayloadInputSnapshot(payload);
        Require(after.captured && PayloadEndpointsPresent(payload, after),
            "capture generated payload and directory endpoints after host setup: " + after.error);
        Require(after.desktopRelative == before.desktopRelative && after.targetRelative == before.targetRelative &&
            after.sourceMappingRelative == before.sourceMappingRelative && after.targetMappingRelative == before.targetMappingRelative,
            "host setup preserves each generated directory role");
        const auto sentinel = (after.desktopRelative / L"Sentinel.txt").lexically_normal();
        Require(after.entries.size() == before.entries.size() + 1, "host setup adds exactly its sentinel to the source graph");
        for (const auto& original : before.entries)
        {
            const auto item = std::find_if(after.entries.begin(), after.entries.end(), [&](const auto& current) {
                return current.relative == original.relative;
            });
            Require(item != after.entries.end() && item->kind == original.kind && item->bytes == original.bytes &&
                item->volumeSerial == original.volumeSerial && item->fileIndexHigh == original.fileIndexHigh &&
                item->fileIndexLow == original.fileIndexLow, "host setup preserves each prepared input identity and bytes");
        }
        const auto added = std::find_if(after.entries.begin(), after.entries.end(), [&](const auto& item) {
            return item.relative == sentinel;
        });
        Require(added != after.entries.end() && added->kind == "file" && added->bytes == "untouched",
            "host setup sentinel is part of the actual input baseline");
        Require(after.memberEntryIndices.size() == payload.members.size(), "actual input preserves captured member positions");
        for (const auto index : after.memberEntryIndices)
            Require(index < after.entries.size(), "actual input contains every selected member");
        std::error_code ec;
        Require(fs::equivalent(profile->desktop, payload.desktopDirectory, ec) && !ec,
            "production host uses the generated desktop directory");
    }
};
fs::path LinkTarget(const fs::path& path)
{
    ComPtr<IShellLinkW> link; ComPtr<IPersistFile> persist;
    Require(SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) &&
        SUCCEEDED(link.As(&persist)) && SUCCEEDED(persist->Load(path.c_str(), STGM_READ)), "read persisted shortcut");
    std::array<wchar_t, 32768> target{};
    Require(SUCCEEDED(link->GetPath(target.data(), static_cast<int>(target.size()), nullptr, SLGP_RAWPATH)), "resolve shortcut");
    return target.data();
}
std::vector<fs::path> Outputs(const Profile& p)
{
    std::vector<fs::path> result;
    for (const auto& entry : fs::directory_iterator(p.desktop))
        if (entry.path().filename() != L"Sentinel.txt") result.push_back(entry.path());
    return result;
}
void Reload(Profile& p, const std::vector<std::pair<fs::path, GridCell>>& expected, bool saveFirst = true)
{
    if (saveFirst) Require(Access::Save(*p.app), "save committed state");
    Access::PerturbBeforeDiskRead(*p.app);
    Access::RequestAsync(*p.app, true);
    Wait([&] { return Access::FileDropIdle(*p.app); });
    for (const auto& [path, cell] : expected)
        Require(Same(Find(*p.app, path).gridCell, cell), "disk reload must recover each committed landing");
    Require(Read(p.desktop / L"Sentinel.txt") == "untouched", "unrelated file survives");
}
void Submit(Profile& p, ole_file_source_fixture::Source& source, DWORD keys, DWORD allowed,
    DWORD expected, POINT point, bool cancel = false, DWORD enterKeys = MK_LBUTTON, bool deferredDefault = false,
    Container* expectedTarget = nullptr)
{
    auto adapter = Access::ExternalAdapter(*p.app);
    auto effect = allowed;
    Require(SUCCEEDED(adapter->DragEnter(&source, enterKeys, Access::ExternalPoint(point), &effect)), "enter OLE boundary");
    Require(!source.formatProbeObservationFailed, "observe source format probes without altering their results");
    Require(source.starts == 0 && (!source.asynchronous || source.fileReads == 0), "hover cannot render delayed files");
    if (effect && expectedTarget)
    {
        Require(Access::Target(*p.app) == expectedTarget, "hover selects requested native target");
        if (expectedTarget == Access::Desktop(*p.app))
            Require(Access::Region(*p.app) == HitRegion::Empty, "hover hits the requested empty desktop cell");
    }
    if (cancel)
    {
        Require(SUCCEEDED(adapter->DragLeave()), "cancel through real OLE leave");
        Wait([&] { return Access::ExternalLeaveIdle(*p.app); });
        return;
    }
    effect = allowed;
    const auto status = adapter->Drop(&source, keys, Access::ExternalPoint(point), &effect);
    // A synchronous optimized Shell move has already removed the source;
    // returning MOVE would tell the drag source to remove it a second time.
    const DWORD returned = !source.asynchronous && expected == DROPEFFECT_MOVE ? DROPEFFECT_NONE : expected;
    Require(SUCCEEDED(status) && (deferredDefault ? (effect == DROPEFFECT_COPY || effect == DROPEFFECT_MOVE) : effect == returned),
        "release must honor the gesture, capabilities and optimized move protocol");
}
void RequireCompletionTrace(std::size_t start, HWND expectedWindow,
    drag_global::ole_driver::MessageRoute expectedRoute)
{
    using namespace drag_global::ole_driver;
    Require(Access::messageTrace && !Access::messageTrace->LostOrCrossThread(),
        "production message observation must remain complete on the UI thread");
    const auto& observations = Access::messageTrace->Observations();
    Require(start <= observations.size(), "completion trace starts within observed messages");
    std::uint64_t sourceSequence = 0, commitSequence = 0;
    for (std::size_t index = start; index < observations.size(); ++index)
    {
        const auto& event = observations[index];
        if (event.message != kShellFileOperationCompletedMessage) continue;
        Require(event.window == expectedWindow && event.route == expectedRoute &&
            !event.malformedCompletion && event.completion && event.returned &&
            event.enteredThread == GetCurrentThreadId() && event.returnedThread == event.enteredThread &&
            event.returnedSequence > event.enteredSequence,
            "real completion enters and returns from a production window procedure");
        if (event.completion->succeeded)
        {
            if (event.completion->fileOperation && !commitSequence) commitSequence = event.enteredSequence;
            else if (!event.completion->fileOperation && !sourceSequence) sourceSequence = event.enteredSequence;
        }
    }
    Require(sourceSequence && commitSequence && sourceSequence < commitSequence,
        "successful source and file-operation completion messages reach the expected production window in order");
}
// Keep the original object open with delete sharing: comparisons then open the
// current path while both handles exist, without blocking production changes.
class ObservedPhysicalFile
{
public:
    explicit ObservedPhysicalFile(const fs::path& path)
    {
        file_ = CreateFileW(path.c_str(), GENERIC_READ | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
            throw drag_global::ole_driver::ObservationUnavailable("physical file could not be opened for observation");
        FILE_ATTRIBUTE_TAG_INFO attributes{};
        if (!GetFileInformationByHandleEx(file_, FileAttributeTagInfo, &attributes, sizeof(attributes)))
        {
            CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
            throw drag_global::ole_driver::ObservationUnavailable("physical file attributes could not be observed");
        }
        if (attributes.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        {
            CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
            Require(false, "physical output must be a regular non-reparse file");
        }
        if (!GetFileInformationByHandleEx(file_, FileIdInfo, &id_, sizeof(id_)))
        {
            CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
            throw drag_global::ole_driver::ObservationUnavailable("128-bit file ID is unavailable");
        }
        const bool zeroId = std::all_of(std::begin(id_.FileId.Identifier), std::end(id_.FileId.Identifier),
            [](BYTE byte) { return byte == 0; });
        if (zeroId)
        {
            CloseHandle(file_); file_ = INVALID_HANDLE_VALUE;
            throw drag_global::ole_driver::ObservationUnavailable("physical file identity requires a nonzero file ID");
        }
    }
    ~ObservedPhysicalFile() { if (file_ != INVALID_HANDLE_VALUE) CloseHandle(file_); }
    ObservedPhysicalFile(const ObservedPhysicalFile&) = delete;
    ObservedPhysicalFile& operator=(const ObservedPhysicalFile&) = delete;
    bool SameObject(const ObservedPhysicalFile& other) const
    {
        return id_.VolumeSerialNumber == other.id_.VolumeSerialNumber &&
            std::memcmp(id_.FileId.Identifier, other.id_.FileId.Identifier, sizeof(id_.FileId.Identifier)) == 0;
    }
    FILE_ID_INFO Identity() const noexcept { return id_; }
    bool PathStillNamesObject(const fs::path& path) const
    {
        ObservedPhysicalFile current(path);
        return SameObject(current);
    }
    // Size/time bracketing detects common concurrent changes; it is not an
    // atomic read or proof against same-size writes with unchanged timestamps.
    bool HasBytes(const std::vector<std::byte>& expected) const
    {
        FILE_STANDARD_INFO beforeSize{}, afterSize{};
        FILE_BASIC_INFO beforeTime{}, afterTime{};
        if (!GetFileInformationByHandleEx(file_, FileStandardInfo, &beforeSize, sizeof(beforeSize)) ||
            !GetFileInformationByHandleEx(file_, FileBasicInfo, &beforeTime, sizeof(beforeTime)) ||
            beforeSize.EndOfFile.QuadPart < 0)
            throw drag_global::ole_driver::ObservationUnavailable("physical file metadata could not be observed");
        if (static_cast<std::uint64_t>(beforeSize.EndOfFile.QuadPart) != expected.size())
        {
            if (!GetFileInformationByHandleEx(file_, FileStandardInfo, &afterSize, sizeof(afterSize)) ||
                !GetFileInformationByHandleEx(file_, FileBasicInfo, &afterTime, sizeof(afterTime)) ||
                beforeSize.EndOfFile.QuadPart != afterSize.EndOfFile.QuadPart ||
                beforeTime.LastWriteTime.QuadPart != afterTime.LastWriteTime.QuadPart)
                throw drag_global::ole_driver::ObservationUnavailable("physical file size changed during observation");
            return false;
        }
        LARGE_INTEGER zero{};
        if (!SetFilePointerEx(file_, zero, nullptr, FILE_BEGIN))
            throw drag_global::ole_driver::ObservationUnavailable("physical file seek failed during observation");
        std::vector<std::byte> actual(expected.size());
        std::size_t offset = 0;
        while (offset < actual.size())
        {
            const auto amount = static_cast<DWORD>((std::min)(actual.size() - offset, static_cast<std::size_t>(4096)));
            DWORD count = 0;
            if (!ReadFile(file_, actual.data() + offset, amount, &count, nullptr) || count == 0)
                throw drag_global::ole_driver::ObservationUnavailable("physical file read failed or changed during observation");
            offset += count;
        }
        if (!GetFileInformationByHandleEx(file_, FileStandardInfo, &afterSize, sizeof(afterSize)) ||
            !GetFileInformationByHandleEx(file_, FileBasicInfo, &afterTime, sizeof(afterTime)) ||
            beforeSize.EndOfFile.QuadPart != afterSize.EndOfFile.QuadPart ||
            beforeTime.LastWriteTime.QuadPart != afterTime.LastWriteTime.QuadPart)
            throw drag_global::ole_driver::ObservationUnavailable("physical file changed or could not be re-observed");
        return actual == expected;
    }
private:
    HANDLE file_ = INVALID_HANDLE_VALUE;
    FILE_ID_INFO id_{};
};

// Independent observation for a restricted one-file generated copy. It
// reads production completions, filesystem content, placement and saved
// layout; it does not choose or perform the operation. The global executor
// must still close every candidate axis and the selected event graph.
struct GeneratedCopyObservation
{
    GeneratedPayloadProfile& generated;
    ole_file_source_fixture::Source& source;
    fs::path original, output;
    std::string originalBytes;
    std::vector<std::byte> originalByteVector;
    std::unique_ptr<ObservedPhysicalFile> sourceFile;
    std::size_t traceStart = 0, initialModelCount = 0;
    std::optional<drag_global::ole_driver::FileSourceInitialObservation> sourceInitial;
    std::optional<Access::FileWorkersObservation> workersBefore;
    bool passiveActive = false;
    std::vector<std::pair<fs::path, GridCell>> initialCells;
    std::uint64_t initialLayoutRevision = 0;
    drag_global::ole_driver::CompletionHostState initialCompletion;

    GeneratedCopyObservation(GeneratedPayloadProfile& fixture, ole_file_source_fixture::Source& provider)
        : generated(fixture), source(provider)
    {
        auto& profile = *generated.profile;
        Require(generated.after.captured && generated.payload.members.size() == 1 &&
            generated.payload.members.front().kind == "file" && generated.after.memberEntryIndices.size() == 1 &&
            generated.after.memberEntryIndices.front() < generated.after.entries.size(),
            "generated copy requires one actual captured file");
        Require(source.asynchronous && Access::asyncOwner == profile.app && Access::productionDispatch == 1 &&
            Access::fixtureControlWindow && Access::messageTrace,
            "generated copy requires the same isolated asynchronous source and production window routes");
        original = generated.payload.members.front().path;
        output = profile.desktop / original.filename();
        Require(!fs::exists(output), "generated copy requires a destination without a name collision");
        originalBytes = generated.after.entries[generated.after.memberEntryIndices.front()].bytes;
        Require(Read(original) == originalBytes, "generated source bytes match the post-setup input baseline");
        originalByteVector.resize(originalBytes.size());
        if (!originalBytes.empty()) std::memcpy(originalByteVector.data(), originalBytes.data(), originalBytes.size());
        sourceFile = std::make_unique<ObservedPhysicalFile>(original);
        Require(sourceFile->HasBytes(originalByteVector) && sourceFile->PathStillNamesObject(original),
            "generated source baseline belongs to the retained physical file object");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        for (const auto& item : Access::Items(*profile.app))
        {
            Require(item.gridCell.pageId != L"__page:1" || item.gridCell.column > 1 || item.gridCell.row > 1 ||
                item.gridCell.column + item.gridSpan.columns <= 1 || item.gridCell.row + item.gridSpan.rows <= 1,
                "generated copy's requested landing cell is initially empty");
            const fs::path path(item.parsingName);
            Require(fs::exists(path), "generated initial model item has an observable physical identity");
            initialCells.emplace_back(path, item.gridCell);
        }
        initialModelCount = Access::Items(*profile.app).size();
        initialLayoutRevision = Access::LayoutRevision(*profile.app);
        traceStart = Access::messageTrace->Observations().size();
        initialCompletion = {Access::FileDropIdle(*profile.app), static_cast<std::uint64_t>(source.starts),
            static_cast<std::uint64_t>(source.ends)};
        Require(source.starts == 0 && source.ends == 0, "generated source starts without an active lease");
    }

    ~GeneratedCopyObservation()
    {
        if (passiveActive) snowdesktop::external_transfer_observation::Stop();
    }
    void StartPassiveObservation()
    {
        using Activity = snowdesktop::ShellFileOperationWorker::ObservedActivity;
        if (passiveActive || workersBefore)
            throw drag_global::ole_driver::ObservationUnavailable("passive_transfer_already_started");
        const auto state = Access::ObserveFileWorkers(*generated.profile->app);
        if (state.read != Activity::idle || state.file != Activity::idle || !state.readIdentity || !state.fileIdentity ||
            state.readIdentity == state.fileIdentity)
            throw drag_global::ole_driver::ObservationUnavailable("passive_transfer_initial_workers_not_observed_idle");
        workersBefore = state;
        if (!snowdesktop::external_transfer_observation::Enable())
            throw drag_global::ole_driver::ObservationUnavailable("passive_transfer_log_enable_unavailable");
        passiveActive = true;
    }
    drag_global::ole_driver::PassiveTransferEvidence FinishPassiveObservation()
    {
        using Activity = snowdesktop::ShellFileOperationWorker::ObservedActivity;
        if (!passiveActive || !workersBefore)
            throw drag_global::ole_driver::ObservationUnavailable("passive_transfer_not_started");
        const auto after = Access::ObserveFileWorkers(*generated.profile->app);
        snowdesktop::external_transfer_observation::Stop();
        passiveActive = false;
        if (after.read != Activity::idle || after.file != Activity::idle ||
            after.readIdentity != workersBefore->readIdentity || after.fileIdentity != workersBefore->fileIdentity ||
            !snowdesktop::external_transfer_observation::Complete())
            throw drag_global::ole_driver::ObservationUnavailable("passive_transfer_worker_or_log_boundary_unavailable");
        auto evidence = drag_global::ole_driver::InspectPassiveTransfer(static_cast<IDataObject*>(&source));
        for (const auto& worker : evidence.workers)
        {
            if (!worker.worker)
            {
                evidence.unresolved.emplace_back("passive_transfer_worker_instance_unobserved");
                continue;
            }
            if ((worker.kind == 1 && worker.worker != after.readIdentity) ||
                (worker.kind == 2 && worker.worker != after.fileIdentity))
                evidence.unresolved.emplace_back("passive_transfer_worker_instance_mismatch");
        }
        // Both observations are instantaneous. A late enqueue, queued UI
        // message or unrelated reentrant commit cannot be ruled out here.
        evidence.unresolved.emplace_back("passive_worker_snapshots_not_a_quiescence_proof");
        evidence.unresolved.emplace_back("passive_one_capture_per_process_requires_case_isolation");
        return evidence;
    }
    void PrepareSourceEvidence(const std::vector<drag_global::Axis>& axes, const drag_global::Candidate& candidate)
    {
        if (sourceInitial || !generated.version)
            throw drag_global::ole_driver::ObservationUnavailable("legacy source evidence baseline unavailable or already captured");
        sourceInitial.emplace(drag_global::ole_driver::CaptureSingleFileSourceInitial(
            axes, candidate, generated.payload, generated.after, source, *generated.version));
        if (!sourceInitial->Captured())
        {
            std::string reason = "file_source_initial_observation_unavailable";
            for (const auto& obligation : sourceInitial->Obligations()) reason += ":" + obligation;
            throw drag_global::ole_driver::ObservationUnavailable(reason);
        }
    }
    drag_global::ole_driver::FileSourceEvidence SourceEvidence(
        const std::vector<drag_global::Axis>& axes, const drag_global::Candidate& candidate) const
    {
        if (!sourceInitial || !generated.version)
        {
            drag_global::ole_driver::FileSourceEvidence evidence;
            evidence.obligations.emplace_back("legacy_file_source_initial_capture_unavailable");
            return evidence;
        }
        return drag_global::ole_driver::InspectSingleFileSourceEvidence(axes, candidate, generated.payload,
            *sourceInitial, source, *generated.version);
    }
    void PrepareV3SourceEvidence(const drag_global::V3FamilyValidatedContext& context)
    {
        if (sourceInitial || generated.version)
            throw drag_global::ole_driver::ObservationUnavailable("v3 source evidence baseline unavailable or already captured");
        sourceInitial.emplace(drag_global::ole_driver::CaptureV3SingleFileSourceInitial(
            context, generated.payload, generated.after, source));
        if (!sourceInitial->Captured())
        {
            std::string reason = "v3_file_source_initial_observation_unavailable";
            for (const auto& obligation : sourceInitial->Obligations()) reason += ":" + obligation;
            throw drag_global::ole_driver::ObservationUnavailable(reason);
        }
    }
    drag_global::ole_driver::FileSourceEvidence V3SourceEvidence(
        const drag_global::V3FamilyValidatedContext& context) const
    {
        if (!sourceInitial || generated.version)
        {
            drag_global::ole_driver::FileSourceEvidence evidence;
            evidence.obligations.emplace_back("v3_file_source_initial_capture_unavailable");
            return evidence;
        }
        return drag_global::ole_driver::InspectV3SingleFileSourceEvidence(context, generated.payload, *sourceInitial, source);
    }

    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            Require(Access::Target(*generated.profile->app) == Access::Desktop(*generated.profile->app) &&
                Access::Region(*generated.profile->app) == HitRegion::Empty,
                "generated copy retains the actual empty desktop target immediately before Drop");
    }
    void ObserveOleReturn(const drag_global::ole_driver::Observation& event)
    {
        using namespace drag_global::ole_driver;
        const auto point = Access::ExternalPoint({150, 150});
        Require(SUCCEEDED(event.result) && event.offeredEffect &&
            *event.offeredEffect == (DROPEFFECT_COPY | DROPEFFECT_MOVE) && event.keyState && event.screenPoint &&
            event.screenPoint->x == point.x && event.screenPoint->y == point.y && event.returnedEffect,
            "generated OLE event preserves the actual offered effect and screen-coordinate inputs");
        if (event.kind == Kind::enter)
        {
            Require(*event.keyState == MK_LBUTTON &&
                (*event.returnedEffect == DROPEFFECT_COPY || *event.returnedEffect == DROPEFFECT_MOVE) &&
                Access::Target(*generated.profile->app) == Access::Desktop(*generated.profile->app) &&
                Access::Region(*generated.profile->app) == HitRegion::Empty,
                "generated OLE enter reaches the requested empty desktop target");
            // The exact default hover effect still needs the real volume and
            // available-hover-data oracle; either allowed result is observed.
        }
        else if (event.kind == Kind::drop)
            Require(*event.keyState == MK_CONTROL && *event.returnedEffect == DROPEFFECT_COPY,
                "generated Ctrl release negotiates copy at the real OLE drop boundary");
        else Require(false, "generated single-copy observer does not handle this OLE event");
    }

    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        auto& profile = *generated.profile;
        Require(terminal.kind == Kind::drop && SUCCEEDED(terminal.result) && terminal.returnedEffect &&
            *terminal.returnedEffect == DROPEFFECT_COPY,
            "generated Ctrl copy returns the negotiated copy effect at the real OLE drop boundary");
        const auto evidence = WaitForCompletion(*Access::messageTrace, traceStart, Access::fixtureControlWindow,
            MessageRoute::control_window_proc, terminal, initialCompletion, kShellFileOperationCompletedMessage, 12000,
            [&] { return CompletionHostState{Access::FileDropIdle(*profile.app),
                static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)}; },
            [] { Access::ThrowAsyncException(); });
        Require(evidence.signal == CompletionSignal::completed_messages,
            "generated copy requires real source and file commit completion messages: " + evidence.detail);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::Items(*profile.app).size() == initialModelCount + 1; });
    }

    void AssertResults()
    {
        auto& profile = *generated.profile;
        if (source.formatProbeObservationFailed)
            throw drag_global::ole_driver::ObservationUnavailable("source format observations were incomplete");
        RequireBusiness(source.starts == 1 && source.ends == 1 && SUCCEEDED(source.outcome) && source.endEffect == DROPEFFECT_COPY &&
            !source.wrongApartment && !source.fileReadBeforeStart,
            "generated copy completes its actual source lease on the owning apartment");
        RequireObservedFilePayload(source, {original});
        const auto actual = drag_global::CapturePayloadInputSnapshot(generated.payload);
        if (!actual.captured)
            throw drag_global::ole_driver::ObservationUnavailable("generated copy filesystem snapshot unavailable");
        RequireBusiness(actual.entries.size() == generated.after.entries.size() + 1,
            "generated copy creates exactly one physical output in the isolated graph");
        for (const auto& previous : generated.after.entries)
        {
            const auto observed = std::find_if(actual.entries.begin(), actual.entries.end(), [&](const auto& item) {
                return item.relative == previous.relative;
            });
            RequireBusiness(observed != actual.entries.end() && observed->kind == previous.kind && observed->bytes == previous.bytes &&
                observed->volumeSerial == previous.volumeSerial && observed->fileIndexHigh == previous.fileIndexHigh &&
                observed->fileIndexLow == previous.fileIndexLow,
                "generated copy preserves every existing input object and its bytes");
        }
        const auto outputRelative = output.lexically_relative(generated.payload.directory);
        const auto copied = std::find_if(actual.entries.begin(), actual.entries.end(), [&](const auto& item) {
            return item.relative == outputRelative;
        });
        RequireBusiness(copied != actual.entries.end() && copied->kind == "file" && copied->bytes == originalBytes,
            "generated copy produces the selected source bytes and preserves the source");
        ObservedPhysicalFile outputFile(output);
        RequireBusiness(sourceFile->PathStillNamesObject(original) && sourceFile->HasBytes(originalByteVector) &&
            outputFile.PathStillNamesObject(output) && outputFile.HasBytes(originalByteVector) &&
            !sourceFile->SameObject(outputFile),
            "copied bytes belong to a distinct retained output while the original object remains in place");
        const auto cell = FindCopyBusiness(*profile.app, output).gridCell;
        RequireBusiness(cell.pageId == L"__page:1" && cell.column == 1 && cell.row == 1,
            "generated copy lands at the requested desktop cell");
        Wait([&] { return Access::LayoutSaveIdle(*profile.app); });
        RequireBusiness(Access::LayoutRevision(*profile.app) > initialLayoutRevision,
            "generated copy completes production layout persistence");
        auto expectedCells = initialCells;
        expectedCells.emplace_back(output, cell);
        // Trigger the same production disk reload as Reload(), then classify
        // observed model differences separately from lookup/I/O failures.
        Access::PerturbBeforeDiskRead(*profile.app);
        Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        RequireBusiness(Access::Items(*profile.app).size() == expectedCells.size(),
            "generated copy preserves all existing desktop members after production reload");
        for (const auto& [path, expected] : expectedCells)
            RequireBusiness(Same(FindCopyBusiness(*profile.app, path).gridCell, expected),
                "production reload and any deferred layout save preserve each landing");
        const auto reloaded = drag_global::CapturePayloadInputSnapshot(generated.payload);
        if (!reloaded.captured)
            throw drag_global::ole_driver::ObservationUnavailable("reloaded filesystem snapshot unavailable");
        RequireBusiness(reloaded.root == actual.root && reloaded.targetRelative == actual.targetRelative &&
            reloaded.desktopRelative == actual.desktopRelative && reloaded.sourceMappingRelative == actual.sourceMappingRelative &&
            reloaded.targetMappingRelative == actual.targetMappingRelative && SamePayloadEndpointIndices(reloaded, actual) &&
            reloaded.memberEntryIndices == actual.memberEntryIndices && reloaded.entries.size() == actual.entries.size(),
            "production reload preserves the physical source and destination graph");
        for (std::size_t i = 0; i < actual.entries.size(); ++i)
        {
            const auto& previous = actual.entries[i]; const auto& item = reloaded.entries[i];
            RequireBusiness(item.relative == previous.relative && item.kind == previous.kind && item.bytes == previous.bytes &&
                item.volumeSerial == previous.volumeSerial && item.fileIndexHigh == previous.fileIndexHigh &&
                item.fileIndexLow == previous.fileIndexLow,
                "production reload preserves each file object's identity and bytes");
        }
        RequireBusiness(sourceFile->PathStillNamesObject(original) && sourceFile->HasBytes(originalByteVector) &&
            outputFile.PathStillNamesObject(output) && outputFile.HasBytes(originalByteVector) &&
            !sourceFile->SameObject(outputFile),
            "the retained source and output objects and their bytes survive production reload");
    }
};
// V4's one-file copy oracle observes a real isolated profile and Shell source.
// It neither performs the transfer nor treats a completion callback alone as
// evidence that the requested file and layout were committed.
struct V4CopyBusinessEvidence
{
    fs::path source, output;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId, outputId;
    std::optional<GridCell> outputCell, reloadedOutputCell;
    std::optional<ole_content_output_observation::Evidence> outputBytes, reloadedOutputBytes;
    std::optional<std::uint64_t> initialLayoutRevision, committedLayoutRevision;
    std::optional<drag_global::ole_driver::CompletionEvidence> completion;
    std::optional<std::size_t> initialModelCount, finalModelCount, reloadedModelCount;
    bool sourcePayloadObserved = false, assertionsCompleted = false;
};

struct V4GeneratedCopyObservation
{
    Profile& profile;
    ole_file_source_fixture::Source& source;
    fs::path original, output;
    std::vector<std::byte> expected;
    GridCell landing;
    std::unique_ptr<ObservedPhysicalFile> originalFile, sentinelFile;
    std::vector<std::pair<fs::path, GridCell>> initialCells;
    std::size_t traceStart = 0;
    drag_global::ole_driver::CompletionHostState initialCompletion;
    V4CopyBusinessEvidence evidence;

    V4GeneratedCopyObservation(Profile& host, ole_file_source_fixture::Source& provider,
        const drag_global::V4SingleFileFixture& fixture, GridCell expectedLanding)
        : profile(host), source(provider), original(fixture.file), output(host.desktop / fixture.file.filename()),
          expected(fixture.bytes), landing(std::move(expectedLanding))
    {
        RequireObservation(fixture.inputPrepared && fixture.created && fixture.fileIdentityObserved &&
            source.asynchronous && Access::asyncOwner == profile.app && Access::productionDispatch == 1 &&
            Access::fixtureControlWindow && Access::messageTrace && !Access::messageTrace->LostOrCrossThread(),
            "v4 copy requires its isolated source, model and production window routes");
        std::error_code error;
        const bool outputExists = fs::exists(output, error);
        RequireObservation(!error && !outputExists && original != output,
            "v4 copy requires an unoccupied output path");
        originalFile = std::make_unique<ObservedPhysicalFile>(original);
        RequireObservation(OwnedProfileDesktopRoot::SameId(originalFile->Identity(), fixture.fileId) &&
            originalFile->PathStillNamesObject(original) && originalFile->HasBytes(expected),
            "v4 copy source identity and exact bytes match the prepared candidate");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        RequireObservation(source.starts == 0 && source.ends == 0, "v4 copy starts without an active source operation");
        const auto sentinel = profile.desktop / L"Sentinel.txt";
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        sentinelFile = std::make_unique<ObservedPhysicalFile>(sentinel);
        RequireObservation(Outputs(profile).empty() && sentinelFile->HasBytes(sentinelBytes),
            "v4 copy begins with only its unchanged isolated sentinel");
        for (const auto& item : Access::Items(*profile.app))
        {
            RequireObservation(item.gridCell.pageId != landing.pageId || landing.column < item.gridCell.column ||
                landing.row < item.gridCell.row || landing.column >= item.gridCell.column + item.gridSpan.columns ||
                landing.row >= item.gridCell.row + item.gridSpan.rows, "v4 copy landing is initially unoccupied");
            initialCells.emplace_back(fs::path(item.parsingName), item.gridCell);
        }
        RequireObservation(initialCells.size() == 1, "v4 copy starts with one desktop member");
        error.clear();
        const bool initialSentinel = fs::equivalent(initialCells.front().first, sentinel, error);
        RequireObservation(!error && initialSentinel, "v4 copy starts with its sentinel in the desktop model");
        evidence.source = original; evidence.output = output; evidence.expectedBytes = expected;
        evidence.sourceId = originalFile->Identity(); evidence.initialModelCount = initialCells.size();
        evidence.initialLayoutRevision = Access::LayoutRevision(*profile.app);
        traceStart = Access::messageTrace->Observations().size();
        initialCompletion = {true, static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)};
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            RequireEvent(Access::Target(*profile.app) == Access::Desktop(*profile.app) &&
                Access::Region(*profile.app) == HitRegion::Empty,
                "v4 copy reaches the requested empty desktop target before Drop");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(Access::messageTrace && !Access::messageTrace->LostOrCrossThread(),
            "v4 copy completion trace is unavailable");
        RequireEvent(terminal.kind == Kind::drop && SUCCEEDED(terminal.result) && terminal.returnedEffect &&
            *terminal.returnedEffect == DROPEFFECT_COPY, "v4 copy Drop reports the negotiated copy effect");
        evidence.completion = WaitForCompletion(*Access::messageTrace, traceStart, Access::fixtureControlWindow,
            MessageRoute::control_window_proc, terminal, initialCompletion, kShellFileOperationCompletedMessage, 12000,
            [&] { return CompletionHostState{Access::FileDropIdle(*profile.app),
                static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)}; },
            [] { Access::ThrowAsyncException(); });
        RequireEvent(evidence.completion->signal != CompletionSignal::failed_message,
            "v4 copy production file-operation completion reports success");
        RequireObservation(evidence.completion->signal == CompletionSignal::completed_messages,
            "v4 copy requires real source and file-operation completion messages");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
    }
    void AssertResults()
    {
        RequireObservation(evidence.completion &&
            evidence.completion->signal == drag_global::ole_driver::CompletionSignal::completed_messages &&
            !source.fileDropObservationFailed && !source.formatProbeObservationFailed && !source.wrongApartment,
            "v4 copy needs its observed completion and source events");
        RequireObservation(source.starts == 1 && source.ends == 1 && SUCCEEDED(source.outcome) &&
            source.endEffect == DROPEFFECT_COPY && !source.fileReadBeforeStart,
            "v4 copy completes its actual asynchronous source operation");
        RequireObservation(source.fileReads >= 0 && source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads),
            "v4 copy source event log is complete");
        for (const auto& event : source.fileDropReads)
        {
            RequireObservation(event.threadId == source.owner && event.startsAtEntry > event.endsAtEntry,
                "v4 copy source read is observed within its STA lease");
            if (FAILED(event.result)) continue;
            RequireObservation(event.capture == ole_file_source_fixture::FileDropCapture::captured,
                "v4 copy source payload is observable");
            RequireObservation(event.paths.size() == 1, "v4 copy provider did not supply one selected source path");
            const int compared = CompareStringOrdinal(event.paths.front().c_str(), -1, original.c_str(), -1, FALSE);
            RequireObservation(compared != 0, "v4 copy source path comparison is unavailable");
            RequireObservation(compared == CSTR_EQUAL, "v4 copy provider did not supply the selected source path");
            evidence.sourcePayloadObserved = true;
        }
        RequireObservation(evidence.sourcePayloadObserved, "v4 copy production source read was not observed");
        const auto outputs = Outputs(profile);
        RequireBusiness(outputs.size() == 1 && outputs.front() == output,
            "v4 copy creates exactly the selected output");
        ObservedPhysicalFile copied(output);
        evidence.outputId = copied.Identity();
        evidence.outputBytes = ole_content_output_observation::ExactBytes(output, expected);
        RequireObservation(evidence.outputBytes->observed, "v4 copy output bytes could not be observed");
        RequireBusiness(!originalFile->SameObject(copied) && originalFile->PathStillNamesObject(original) &&
            originalFile->HasBytes(expected) && copied.PathStillNamesObject(output) && copied.HasBytes(expected) &&
            evidence.outputBytes->matches,
            "v4 copy retains the source and creates a distinct file with its exact bytes");
        evidence.finalModelCount = Access::Items(*profile.app).size();
        evidence.outputCell = FindCopyBusiness(*profile.app, output).gridCell;
        RequireBusiness(*evidence.finalModelCount == initialCells.size() + 1 && Same(*evidence.outputCell, landing),
            "v4 copy adds one desktop member at the requested landing");
        for (const auto& [path, cell] : initialCells)
            RequireBusiness(Same(FindCopyBusiness(*profile.app, path).gridCell, cell),
                "v4 copy preserves the existing desktop members and positions");
        evidence.committedLayoutRevision = Access::LayoutRevision(*profile.app);
        RequireBusiness(*evidence.committedLayoutRevision > *evidence.initialLayoutRevision,
            "v4 copy commits a new production layout revision");
        Access::PerturbBeforeDiskRead(*profile.app);
        Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        evidence.reloadedModelCount = Access::Items(*profile.app).size();
        evidence.reloadedOutputCell = FindCopyBusiness(*profile.app, output).gridCell;
        RequireBusiness(*evidence.reloadedModelCount == initialCells.size() + 1 &&
            Same(*evidence.reloadedOutputCell, landing), "v4 copy landing survives production reload");
        for (const auto& [path, cell] : initialCells)
            RequireBusiness(Same(FindCopyBusiness(*profile.app, path).gridCell, cell),
                "v4 copy preserves prior members after production reload");
        evidence.reloadedOutputBytes = ole_content_output_observation::ExactBytes(output, expected);
        RequireObservation(evidence.reloadedOutputBytes->observed, "v4 copy reloaded bytes could not be observed");
        RequireBusiness(originalFile->PathStillNamesObject(original) && originalFile->HasBytes(expected) &&
            copied.PathStillNamesObject(output) && copied.HasBytes(expected) && evidence.reloadedOutputBytes->matches,
            "v4 copy retains both file identities and bytes after production reload");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireBusiness(sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") &&
            sentinelFile->HasBytes(sentinelBytes), "v4 copy preserves the unrelated sentinel file and its bytes");
        evidence.assertionsCompleted = true;
    }
};

struct V4BatchCopyMemberEvidence
{
    fs::path source, output;
    std::vector<std::byte> expectedBytes;
    std::optional<FILE_ID_INFO> sourceId, outputId;
    std::optional<GridCell> outputCell, reloadedCell;
    std::optional<ole_content_output_observation::Evidence> outputBytes, reloadedBytes;
};
struct V4BatchCopyBusinessEvidence
{
    std::vector<V4BatchCopyMemberEvidence> members;
    std::vector<std::size_t> modelIndexByReadOrder;
    std::optional<drag_global::ole_driver::CompletionEvidence> completion;
    std::optional<std::uint64_t> initialRevision, committedRevision;
    std::optional<std::size_t> initialCount, finalCount, reloadedCount;
    std::optional<bool> firstReceivedLandingMatched;
    bool sourcePayloadObserved = false, independentAssertionsCompleted = false, assertionsCompleted = false;
    bool baselineRevalidated = false;
    std::vector<std::string> unresolved;
};

// This observer only reads the results of a real batched copy. It never
// creates output files, selects members or commits their layout.
struct V4BatchCopyObservation
{
private:
    Profile& profile;
    ole_file_source_fixture::Source& source;
    std::shared_ptr<const drag_global::V4IndependentFilePayload> payload;
    GridCell landing;
    std::vector<std::unique_ptr<ObservedPhysicalFile>> originals;
    std::vector<std::pair<fs::path, GridCell>> initialCells;
    std::unique_ptr<ObservedPhysicalFile> sentinel;
    std::size_t traceStart = 0;
    drag_global::ole_driver::CompletionHostState initialCompletion;
    V4BatchCopyBusinessEvidence evidence;
    bool unsealedObservationUsed_ = false, awaitUsed_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4BatchObserverIdentity> identity_{
        new drag_global::ole_driver::V4BatchObserverIdentity};

public:
    V4BatchCopyBusinessEvidence Evidence() const { return evidence; }
    V4BatchCopyObservation(const V4BatchCopyObservation&) = delete;
    V4BatchCopyObservation& operator=(const V4BatchCopyObservation&) = delete;
    V4BatchCopyObservation(V4BatchCopyObservation&&) = delete;
    V4BatchCopyObservation& operator=(V4BatchCopyObservation&&) = delete;
    V4BatchCopyObservation(Profile& host, ole_file_source_fixture::Source& provider,
        std::shared_ptr<const drag_global::V4IndependentFilePayload> prepared, GridCell expectedLanding)
        : profile(host), source(provider), payload(std::move(prepared)), landing(std::move(expectedLanding))
    {
        RequireObservation(payload && payload->status == drag_global::V4IndependentFileStatus::input_prepared &&
            !payload->files.empty() && source.asynchronous && Access::asyncOwner == profile.app &&
            Access::productionDispatch == 1 && Access::fixtureControlWindow && Access::messageTrace &&
            !Access::messageTrace->LostOrCrossThread(), "v4 batch needs its isolated source and production host");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        RequireObservation(source.starts == 0 && source.ends == 0 && Outputs(profile).empty(),
            "v4 batch starts without an active source or existing output");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        sentinel = std::make_unique<ObservedPhysicalFile>(profile.desktop / L"Sentinel.txt");
        RequireObservation(sentinel->HasBytes(sentinelBytes), "v4 batch sentinel baseline is available");
        evidence.members.reserve(payload->files.size()); originals.reserve(payload->files.size());
        for (const auto& input : payload->files)
        {
            RequireObservation(input.created && input.identityObserved && input.bytesObserved && input.copyObservationHandle,
                "v4 batch source has a completed copy handoff");
            const auto destination = profile.desktop / input.path.filename();
            std::error_code error;
            const bool exists = fs::exists(destination, error);
            RequireObservation(!error && !exists && destination != input.path,
                "v4 batch destination is initially unoccupied");
            for (const auto& member : evidence.members)
            {
                const int compared = CompareStringOrdinal(member.output.c_str(), -1, destination.c_str(), -1, TRUE);
                RequireObservation(compared != 0 && compared != CSTR_EQUAL,
                    "v4 batch requires distinct observable destination names");
            }
            auto original = std::make_unique<ObservedPhysicalFile>(input.path);
            RequireObservation(OwnedProfileDesktopRoot::SameId(original->Identity(), input.identity) &&
                original->PathStillNamesObject(input.path) && original->HasBytes(input.bytes),
                "v4 batch source identity and bytes match its prepared input");
            V4BatchCopyMemberEvidence member;
            member.source = input.path; member.output = destination; member.expectedBytes = input.bytes;
            member.sourceId = original->Identity(); evidence.members.push_back(std::move(member));
            originals.push_back(std::move(original));
        }
        for (const auto& item : Access::Items(*profile.app))
        {
            RequireObservation(item.gridCell.pageId != landing.pageId || landing.column < item.gridCell.column ||
                landing.row < item.gridCell.row || landing.column >= item.gridCell.column + item.gridSpan.columns ||
                landing.row >= item.gridCell.row + item.gridSpan.rows, "v4 batch landing is initially unoccupied");
            initialCells.emplace_back(fs::path(item.parsingName), item.gridCell);
        }
        RequireObservation(initialCells.size() == 1, "v4 batch starts with one desktop member");
        std::error_code sentinelError;
        const bool initialSentinel = fs::equivalent(initialCells.front().first, profile.desktop / L"Sentinel.txt", sentinelError);
        RequireObservation(!sentinelError && initialSentinel, "v4 batch starts with its sentinel in the desktop model");
        evidence.initialCount = initialCells.size(); evidence.initialRevision = Access::LayoutRevision(*profile.app);
        traceStart = Access::messageTrace->Observations().size();
        initialCompletion = {true, static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)};
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input) const
    {
        if (input.kind == drag_global::ole_driver::Kind::drop)
            RequireEvent(Access::Target(*profile.app) == Access::Desktop(*profile.app) &&
                Access::Region(*profile.app) == HitRegion::Empty, "v4 batch reaches the requested desktop target");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(!awaitUsed_, "v4 batch completion observation cannot be repeated");
        awaitUsed_ = true;
        RequireObservation(Access::messageTrace && !Access::messageTrace->LostOrCrossThread(), "v4 batch trace is unavailable");
        RequireEvent(terminal.kind == Kind::drop && SUCCEEDED(terminal.result) && terminal.returnedEffect &&
            *terminal.returnedEffect == DROPEFFECT_COPY, "v4 batch Drop reports copy");
        evidence.completion = WaitForCompletion(*Access::messageTrace, traceStart, Access::fixtureControlWindow,
            MessageRoute::control_window_proc, terminal, initialCompletion, kShellFileOperationCompletedMessage, 12000,
            [&] { return CompletionHostState{Access::FileDropIdle(*profile.app),
                static_cast<std::uint64_t>(source.starts), static_cast<std::uint64_t>(source.ends)}; },
            [] { Access::ThrowAsyncException(); });
        RequireEvent(evidence.completion->signal != CompletionSignal::failed_message, "v4 batch file operation reports success");
        RequireObservation(evidence.completion->signal == CompletionSignal::completed_messages,
            "v4 batch needs real source and file-operation completion messages");
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
    }
private:
    void AssertResults()
    {
        RequireObservation(evidence.completion &&
            evidence.completion->signal == drag_global::ole_driver::CompletionSignal::completed_messages &&
            !source.wrongApartment && !source.fileDropObservationFailed && !source.formatProbeObservationFailed &&
            source.fileReads >= 0 && source.fileDropReads.size() == static_cast<std::size_t>(source.fileReads),
            "v4 batch completion and source observations are available");
        RequireBusiness(source.starts == 1 && source.ends == 1 && SUCCEEDED(source.outcome) &&
            source.endEffect == DROPEFFECT_COPY && !source.fileReadBeforeStart, "v4 batch source operation completes");
        for (const auto& read : source.fileDropReads)
        {
            RequireObservation(read.threadId == source.owner && read.startsAtEntry > read.endsAtEntry,
                "v4 batch read is observed within its source lease");
            if (FAILED(read.result)) continue;
            RequireObservation(read.capture == ole_file_source_fixture::FileDropCapture::captured, "v4 batch payload is observable");
            RequireObservation(read.paths.size() == evidence.members.size(),
                "v4 batch provider did not supply the requested member count");
            std::vector<std::size_t> observedOrder;
            for (const auto& actualPath : read.paths)
            {
                std::optional<std::size_t> match;
                for (std::size_t i = 0; i < evidence.members.size(); ++i)
                {
                    const int compared = CompareStringOrdinal(actualPath.c_str(), -1, evidence.members[i].source.c_str(), -1, FALSE);
                    RequireObservation(compared != 0, "v4 batch path comparison is unavailable");
                    if (compared == CSTR_EQUAL)
                    { RequireObservation(!match, "v4 batch source identity is ambiguous"); match = i; }
                }
                RequireObservation(match && std::find(observedOrder.begin(), observedOrder.end(), *match) == observedOrder.end(),
                    "v4 batch provider paths cannot be uniquely bound to the candidate");
                observedOrder.push_back(*match);
            }
            RequireObservation(!evidence.sourcePayloadObserved || observedOrder == evidence.modelIndexByReadOrder,
                "v4 batch provider changed its observed path order during the operation");
            evidence.modelIndexByReadOrder = std::move(observedOrder);
            evidence.sourcePayloadObserved = true;
        }
        RequireObservation(evidence.sourcePayloadObserved, "v4 batch production source read was not observed");
        const auto outputs = Outputs(profile);
        RequireObservation(outputs.size() == evidence.members.size(), "v4 batch creates exactly one output per source");
        std::vector<std::unique_ptr<ObservedPhysicalFile>> copies;
        for (std::size_t i = 0; i < evidence.members.size(); ++i)
        {
            auto& member = evidence.members[i];
            RequireObservation(std::find(outputs.begin(), outputs.end(), member.output) != outputs.end(), "v4 batch output name is present");
            auto output = std::make_unique<ObservedPhysicalFile>(member.output);
            member.outputId = output->Identity(); member.outputBytes = ole_content_output_observation::ExactBytes(member.output, member.expectedBytes);
            RequireObservation(member.outputBytes->observed, "v4 batch output bytes are observable");
            RequireObservation(!originals[i]->SameObject(*output) && originals[i]->PathStillNamesObject(member.source) &&
                originals[i]->HasBytes(member.expectedBytes) && output->HasBytes(member.expectedBytes) && member.outputBytes->matches,
                "v4 batch retains each source and copies its exact bytes into a distinct output");
            member.outputCell = FindCopyBusiness(*profile.app, member.output).gridCell;
            for (std::size_t j = 0; j < i; ++j)
                RequireObservation(!Same(*member.outputCell, *evidence.members[j].outputCell) && !output->SameObject(*copies[j]),
                    "v4 batch outputs have distinct layout cells and file identities");
            copies.push_back(std::move(output));
        }
        // The provider read is real, but its exact relationship to the paths
        // committed by the asynchronous task is not yet established here.
        // Preserve the comparison without classifying a mismatch as a product
        // failure until the same receipt proves that relationship.
        evidence.firstReceivedLandingMatched = Same(*evidence.members[evidence.modelIndexByReadOrder.front()].outputCell, landing);
        evidence.finalCount = Access::Items(*profile.app).size();
        RequireObservation(*evidence.finalCount == initialCells.size() + evidence.members.size(), "v4 batch adds all desktop members");
        for (const auto& [path, cell] : initialCells)
            RequireObservation(Same(FindCopyBusiness(*profile.app, path).gridCell, cell), "v4 batch preserves prior desktop positions");
        evidence.committedRevision = Access::LayoutRevision(*profile.app);
        RequireObservation(*evidence.committedRevision > *evidence.initialRevision, "v4 batch persists a new layout revision");
        Access::PerturbBeforeDiskRead(*profile.app); Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        evidence.reloadedCount = Access::Items(*profile.app).size();
        RequireObservation(*evidence.reloadedCount == initialCells.size() + evidence.members.size(), "v4 batch members survive reload");
        for (std::size_t i = 0; i < evidence.members.size(); ++i)
        {
            auto& member = evidence.members[i]; member.reloadedCell = FindCopyBusiness(*profile.app, member.output).gridCell;
            member.reloadedBytes = ole_content_output_observation::ExactBytes(member.output, member.expectedBytes);
            RequireObservation(member.reloadedBytes->observed, "v4 batch reloaded output bytes are observable");
            RequireObservation(Same(*member.reloadedCell, *member.outputCell) && member.reloadedBytes->matches &&
                originals[i]->PathStillNamesObject(member.source) && originals[i]->HasBytes(member.expectedBytes) &&
                copies[i]->PathStillNamesObject(member.output) && copies[i]->HasBytes(member.expectedBytes),
                "v4 batch preserves source, output bytes, identity and placement after reload");
        }
        for (const auto& [path, cell] : initialCells)
            RequireObservation(Same(FindCopyBusiness(*profile.app, path).gridCell, cell), "v4 batch preserves prior members after reload");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sentinel->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinel->HasBytes(sentinelBytes),
            "v4 batch preserves the unrelated sentinel file");
        evidence.independentAssertionsCompleted = true;
    }
public:
    void AssertUnsealedResults()
    {
        RequireObservation(!evidence.baselineRevalidated, "v4 bound observer cannot run an unsealed business observation");
        unsealedObservationUsed_ = true;
        AssertResults();
    }
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4BatchExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& owner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
        OwnedProfileDesktopRoot& root)
    {
        RequireObservation(key && !unsealedObservationUsed_ && !awaitUsed_ && payload && payload->files.size() == 1 && !payload->candidateId.empty() &&
            root.profile && root.profile.get() == &profile && root.StillOwned() && sentinel &&
            evidence.initialCount && evidence.initialRevision && Access::asyncOwner == profile.app &&
            Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::LayoutRevision(*profile.app) == *evidence.initialRevision &&
            Access::Items(*profile.app).size() == *evidence.initialCount && Outputs(profile).empty(),
            "v4 single file execution baseline unavailable");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sentinel->PathStillNamesObject(profile.desktop / L"Sentinel.txt") &&
            sentinel->HasBytes(sentinelBytes) && originals.size() == 1 && evidence.members.size() == 1 &&
            originals.front()->PathStillNamesObject(evidence.members.front().source) &&
            originals.front()->HasBytes(evidence.members.front().expectedBytes),
            "v4 single file execution source or sentinel changed");
        for (const auto& [path, cell] : initialCells)
            RequireObservation(Same(FindCopyBusiness(*profile.app, path).gridCell, cell),
                "v4 single file initial model changed before Drop");
        RequireObservation(key->BindBaseline(owner, epoch, profile.app, static_cast<IDataObject*>(&source),
            payload->candidateId, identity_), "v4 single file execution baseline could not be bound");
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4BatchExecutionKey>& key,
        const std::shared_ptr<drag_global::V4IndependentShellOwner>& owner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && !unsealedObservationUsed_ && awaitUsed_ && payload && key->BeginCapture(owner, epoch, profile.app,
            static_cast<IDataObject*>(&source), payload->candidateId, identity_),
            "v4 single file business capture scope unavailable");
        try { AssertResults(); }
        catch (const std::exception& error)
        { evidence.unresolved.push_back(std::string("v4_single_file_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("v4_single_file_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4BatchCopyBusinessEvidence>(evidence);
        RequireObservation(key->Publish(owner, epoch, profile.app, static_cast<IDataObject*>(&source),
            payload->candidateId, identity_, std::move(snapshot)), "v4 single file business snapshot could not be sealed");
    }
};

// The manifest is candidate input, while every output below is read from the
// isolated production host after its Drop. The task/refresh attribution is a
// separate obligation and this partial observer cannot mark a case complete.
struct V4VirtualLayoutDiskSnapshot;
struct V4VirtualBusinessEvidence
{
    struct Member
    {
        std::size_t descriptorIndex = 0;
        fs::path output;
        std::vector<std::byte> expectedBytes;
        std::optional<FILE_ID_INFO> outputId;
        std::optional<GridCell> outputCell, reloadedCell;
        std::optional<ole_content_output_observation::Evidence> outputBytes, reloadedBytes;
        std::optional<DWORD> outputAttributes;
        std::optional<bool> outputPresent, distinctFromSentinel, distinctFromOtherOutputs, outputIdentityRetained;
    };
    std::string candidateId;
    std::vector<Member> members;
    std::optional<FILE_ID_INFO> sentinelId;
    std::optional<GridCell> sentinelCell, reloadedSentinelCell, requestedLanding;
    std::optional<std::uint64_t> initialRevision, committedRevision;
    std::optional<std::size_t> initialCount, finalCount, reloadedCount;
    std::optional<bool> descriptorZeroLandingMatched, sentinelIdentityRetained, sentinelBytesRetained;
    std::vector<fs::path> outputs;
    std::vector<std::pair<fs::path, GridCell>> finalMembers, reloadedMembers;
    std::vector<std::string> unresolved;
    bool outputEnumerationObserved = false, baselineRevalidated = false;
    bool sourceCallsCaptured = false;
    std::shared_ptr<const V4VirtualLayoutDiskSnapshot> beforeReloadLayout;
    std::optional<bool> descriptorReadObserved, memberReadsObserved;
    bool refreshMessageObserved = false, independentAssertionsCompleted = false, assertionsCompleted = false;
};
struct V4VirtualCopyObservation
{
private:
    OwnedProfileDesktopRoot& ownedRoot;
    Profile& profile;
    ole_content_source_fixture::Source& source;
    GridCell landing;
    V4VirtualBusinessEvidence evidence;
    std::unique_ptr<ObservedPhysicalFile> sentinel;
    std::size_t callStart = 0, traceStart = (std::numeric_limits<std::size_t>::max)();
    std::size_t refreshStart = (std::numeric_limits<std::size_t>::max)();
    std::uint64_t beforeRevision = 0, requestedRevision = 0;
    const drag_global::V4FamilyValidatedContext* verified_ = nullptr;
    const drag_global::event_graph::V4FamilyIdentityContext* familyIdentity_ = nullptr;
    void CaptureLayoutBeforeReload();

    std::shared_ptr<const drag_global::ole_driver::V4VirtualObserverIdentity> identity_{
        new drag_global::ole_driver::V4VirtualObserverIdentity};
public:
    V4VirtualCopyObservation(const V4VirtualCopyObservation&) = delete;
    V4VirtualCopyObservation& operator=(const V4VirtualCopyObservation&) = delete;
    V4VirtualCopyObservation(V4VirtualCopyObservation&&) = delete;
    V4VirtualCopyObservation& operator=(V4VirtualCopyObservation&&) = delete;
    V4VirtualBusinessEvidence Evidence() const { return evidence; }
private:
    GridCell ReadCell(const fs::path& path) const
    {
        std::optional<GridCell> cell;
        for (const auto& item : Access::Items(*profile.app))
        {
            std::error_code error;
            const bool same = fs::equivalent(fs::path(item.parsingName), path, error);
            RequireObservation(!error, "v4 virtual model identity is observable");
            if (!same) continue;
            RequireObservation(!cell, "v4 virtual model identity is unique");
            cell = item.gridCell;
        }
        RequireObservation(cell.has_value(), "v4 virtual model member is observable");
        return *cell;
    }
public:
    static Profile& BoundProfile(OwnedProfileDesktopRoot& root)
    {
        RequireObservation(root.profile && root.profile->app && root.StillOwned(), "v4 virtual owned profile unavailable");
        return *root.profile;
    }
    V4VirtualCopyObservation(OwnedProfileDesktopRoot& root, ole_content_source_fixture::Source& provider,
        const drag_global::mixed_fixture::ConfiguredContentBinding& binding,
        const drag_global::mixed_fixture::VirtualExpectedManifest& manifest, GridCell expectedLanding)
        : ownedRoot(root), profile(BoundProfile(root)), source(provider), landing(std::move(expectedLanding))
    {
        RequireObservation(manifest.ready && !manifest.resourceUnavailable && !manifest.candidateId.empty() &&
            binding.inputPrepared && !binding.resourceUnavailable && binding.candidateId == manifest.candidateId &&
            binding.SourceObject() == &source && !manifest.members.empty() && source.ownerThread == GetCurrentThreadId() && Access::asyncOwner == profile.app &&
            Access::productionDispatch == 1 && Access::fixtureControlWindow && Access::messageTrace &&
            !Access::messageTrace->LostOrCrossThread(), "v4 virtual input needs its isolated candidate and live host");
        const auto& expectedEntries = binding.ConfiguredEntries();
        RequireObservation(source.entries.size() == expectedEntries.size(), "v4 virtual source entries match their binding");
        for (std::size_t i = 0; i < expectedEntries.size(); ++i)
        {
            const auto& actual = source.entries[i]; const auto& expected = expectedEntries[i];
            RequireObservation(actual.format == expected.format && actual.index == expected.index && actual.tymed == expected.tymed &&
                actual.bytes == expected.bytes && actual.readFailure == expected.readFailure &&
                actual.getDataFailure == expected.getDataFailure, "v4 virtual source entries match their binding");
        }
        const auto descriptorId = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
        const auto contentsId = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
        RequireObservation(descriptorId && contentsId, "v4 virtual format identifiers are available");
        const auto descriptor = std::find_if(expectedEntries.begin(), expectedEntries.end(), [&](const auto& entry) {
            return entry.format == descriptorId && entry.index == -1 && entry.tymed == TYMED_HGLOBAL; });
        RequireObservation(descriptor != expectedEntries.end() && descriptor->getDataFailure == S_OK &&
            descriptor->bytes.size() >= offsetof(FILEGROUPDESCRIPTORW, fgd),
            "v4 virtual descriptor entry is available");
        UINT descriptorCount = 0;
        std::memcpy(&descriptorCount, descriptor->bytes.data(), sizeof(descriptorCount));
        RequireObservation(descriptorCount == manifest.members.size() &&
            descriptor->bytes.size() == offsetof(FILEGROUPDESCRIPTORW, fgd) +
                static_cast<std::size_t>(descriptorCount) * sizeof(FILEDESCRIPTORW),
            "v4 virtual descriptor count matches the manifest");
        for (std::size_t index = 0; index < manifest.members.size(); ++index)
        {
            const auto& member = manifest.members[index];
            RequireObservation(member.descriptorIndex == index && member.descriptorIndex < descriptorCount,
                "v4 virtual manifest preserves each distinct descriptor index");
            FILEDESCRIPTORW item{};
            std::memcpy(&item, descriptor->bytes.data() + offsetof(FILEGROUPDESCRIPTORW, fgd) +
                member.descriptorIndex * sizeof(item), sizeof(item));
            const auto* end = std::find(std::cbegin(item.cFileName), std::cend(item.cFileName), L'\0');
            RequireObservation(end != std::cend(item.cFileName) && std::wstring(std::cbegin(item.cFileName), end) == member.name,
                "v4 virtual manifest name matches its descriptor");
            const auto stream = std::find_if(expectedEntries.begin(), expectedEntries.end(), [&](const auto& entry) {
                return entry.format == contentsId && entry.tymed == TYMED_ISTREAM && entry.index >= 0 &&
                    static_cast<std::size_t>(entry.index) == member.descriptorIndex; });
            RequireObservation(stream != expectedEntries.end() && stream->bytes == member.bytes &&
                stream->getDataFailure == S_OK && !stream->readFailure,
                "v4 virtual manifest bytes match its successful stream input");
        }
        { std::lock_guard lock(source.observationMutex); callStart = source.calls.size(); }
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        RequireObservation(Outputs(profile).empty() && Access::Items(*profile.app).size() == 1,
            "v4 virtual desktop starts with only its sentinel");
        sentinel = std::make_unique<ObservedPhysicalFile>(profile.desktop / L"Sentinel.txt");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sentinel->HasBytes(sentinelBytes), "v4 virtual sentinel baseline is available");
        evidence.candidateId = manifest.candidateId; evidence.sentinelId = sentinel->Identity();
        evidence.requestedLanding = landing;
        evidence.sentinelCell = ReadCell(profile.desktop / L"Sentinel.txt");
        evidence.initialCount = Access::Items(*profile.app).size(); evidence.initialRevision = Access::LayoutRevision(*profile.app);
        for (const auto& member : manifest.members)
        {
            const auto output = profile.desktop / member.name;
            std::error_code error;
            const bool exists = fs::exists(output, error);
            RequireObservation(!error && !exists, "v4 virtual destination is initially unoccupied");
            evidence.members.push_back({member.descriptorIndex, output, member.bytes});
        }
        Access::DisableFixtureMaintenance(*profile.app);
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input)
    {
        if (input.kind != drag_global::ole_driver::Kind::drop) return;
        RequireObservation(traceStart == (std::numeric_limits<std::size_t>::max)() && Access::messageTrace &&
            !Access::messageTrace->LostOrCrossThread() && !Access::refreshObservationFailed && Access::FileDropIdle(*profile.app),
            "v4 virtual Drop requires a clean host observation baseline");
        RequireObservation(Access::Target(*profile.app) == Access::Desktop(*profile.app) && Access::Region(*profile.app) == HitRegion::Empty,
            "v4 virtual Drop reaches the requested empty desktop");
        traceStart = Access::messageTrace->Observations().size(); refreshStart = Access::refreshMessages.size();
        beforeRevision = Access::ShellRefreshRevision(*profile.app);
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        using namespace drag_global::ole_driver;
        RequireObservation(traceStart != (std::numeric_limits<std::size_t>::max)() && terminal.returnedEffect,
            "v4 virtual Drop completion baseline is unavailable");
        RequireObservation(terminal.kind == Kind::drop && SUCCEEDED(terminal.result) && *terminal.returnedEffect == DROPEFFECT_COPY,
            "v4 virtual Drop reports copy");
        requestedRevision = Access::ShellRefreshRevision(*profile.app);
        RequireObservation(requestedRevision > beforeRevision, "v4 virtual Drop did not expose a new refresh revision");
        Wait([&] {
            RequireObservation(Access::messageTrace && !Access::messageTrace->LostOrCrossThread() && !Access::refreshObservationFailed,
                "v4 virtual refresh trace is unavailable");
            const auto& events = Access::messageTrace->Observations();
            for (std::size_t i = refreshStart; i < Access::refreshMessages.size(); ++i)
            {
                const auto& refresh = Access::refreshMessages[i];
                if (!refresh.returned || refresh.traceIndex < traceStart || refresh.traceIndex >= events.size() ||
                    refresh.beforeRevision != requestedRevision || refresh.afterRevision != requestedRevision ||
                    !refresh.beforeRunning || refresh.afterRunning || refresh.beforeReady || !refresh.afterReady) continue;
                const auto& event = events[refresh.traceIndex];
                if (event.window == Access::fixtureControlWindow && event.route == MessageRoute::control_window_proc &&
                    event.message == kBackgroundShellReadyMessage && event.returned && event.enteredThread == GetCurrentThreadId() &&
                    event.returnedThread == event.enteredThread && event.returnedSequence > event.enteredSequence)
                    evidence.refreshMessageObserved = true;
            }
            return evidence.refreshMessageObserved && Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app);
        });
    }
private:
    void AssertResults()
    {
        const auto descriptor = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileGroupDescriptorW"));
        const auto contents = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"FileContents"));
        std::vector<bool> membersRead(evidence.members.size());
        if (descriptor && contents && !source.wrongApartment.load() && !source.observationFailed.load())
        {
            std::lock_guard lock(source.observationMutex);
            if (callStart <= source.calls.size())
            {
                evidence.sourceCallsCaptured = true;
                bool descriptorRead = false;
                for (std::size_t i = callStart; i < source.calls.size(); ++i)
                {
                    const auto& call = source.calls[i];
                    if (call.query || call.thread != source.ownerThread || call.aspect != DVASPECT_CONTENT ||
                        call.targetDevicePresent || FAILED(call.result)) continue;
                    if (call.format == descriptor && call.index == -1 && (call.requestedTymed & TYMED_HGLOBAL) &&
                        call.mediumObserved && call.returnedTymed == TYMED_HGLOBAL)
                        descriptorRead = true;
                    if (call.format == contents && (call.requestedTymed & TYMED_ISTREAM) &&
                        call.mediumObserved && call.returnedTymed == TYMED_ISTREAM)
                        for (std::size_t j = 0; j < evidence.members.size(); ++j)
                            if (call.index >= 0 && static_cast<std::size_t>(call.index) == evidence.members[j].descriptorIndex)
                                membersRead[j] = true;
                }
                evidence.descriptorReadObserved = descriptorRead;
                evidence.memberReadsObserved = std::all_of(membersRead.begin(), membersRead.end(), [](bool read) { return read; });
            }
        }
        if (source.wrongApartment.load() || source.observationFailed.load())
        {
            evidence.sourceCallsCaptured = false;
            evidence.descriptorReadObserved.reset(); evidence.memberReadsObserved.reset();
        }
        if (!evidence.sourceCallsCaptured) evidence.unresolved.emplace_back("v4_virtual_source_calls_unavailable");
        if (!evidence.refreshMessageObserved) evidence.unresolved.emplace_back("v4_virtual_refresh_window_unavailable");
        std::error_code enumerationError;
        std::filesystem::directory_iterator iterator(profile.desktop, enumerationError), end;
        for (; !enumerationError && iterator != end; iterator.increment(enumerationError))
            if (iterator->path().filename() != L"Sentinel.txt") evidence.outputs.push_back(iterator->path());
        evidence.outputEnumerationObserved = !enumerationError;
        if (enumerationError) evidence.unresolved.emplace_back("v4_virtual_output_enumeration_unavailable");
        for (const auto& item : Access::Items(*profile.app)) evidence.finalMembers.emplace_back(fs::path(item.parsingName), item.gridCell);
        evidence.finalCount = evidence.finalMembers.size(); evidence.committedRevision = Access::LayoutRevision(*profile.app);
        std::vector<std::unique_ptr<ObservedPhysicalFile>> files(evidence.members.size());
        for (std::size_t i = 0; i < evidence.members.size(); ++i)
        {
            auto& member = evidence.members[i];
            SetLastError(ERROR_SUCCESS);
            const DWORD attributes = GetFileAttributesW(member.output.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES)
            {
                const DWORD error = GetLastError();
                if (error == ERROR_FILE_NOT_FOUND && ownedRoot.StillOwned()) member.outputPresent = false;
                else evidence.unresolved.emplace_back("v4_virtual_output_presence_unavailable");
                continue;
            }
            member.outputPresent = true; member.outputAttributes = attributes;
            if (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            try
            {
                files[i] = std::make_unique<ObservedPhysicalFile>(member.output);
                member.outputId = files[i]->Identity(); member.distinctFromSentinel = !sentinel->SameObject(*files[i]);
                member.outputBytes = ole_content_output_observation::ExactBytes(member.output, member.expectedBytes);
                if (!member.outputBytes->observed) evidence.unresolved.emplace_back("v4_virtual_output_bytes_unavailable");
                try { member.outputCell = ReadCell(member.output); }
                catch (...) { evidence.unresolved.emplace_back("v4_virtual_output_model_member_unavailable"); }
            }
            catch (...) { evidence.unresolved.emplace_back("v4_virtual_output_identity_unavailable"); }
        }
        if (std::all_of(files.begin(), files.end(), [](const auto& file) { return static_cast<bool>(file); }))
            for (std::size_t i = 0; i < files.size(); ++i)
            {
                bool distinct = true;
                for (std::size_t j = 0; j < files.size(); ++j)
                    if (i != j && files[i]->SameObject(*files[j])) distinct = false;
                evidence.members[i].distinctFromOtherOutputs = distinct;
            }
        if (evidence.members.front().outputCell) evidence.descriptorZeroLandingMatched = Same(*evidence.members.front().outputCell, landing);
        CaptureLayoutBeforeReload();
        Access::PerturbBeforeDiskRead(*profile.app); Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        for (const auto& item : Access::Items(*profile.app)) evidence.reloadedMembers.emplace_back(fs::path(item.parsingName), item.gridCell);
        evidence.reloadedCount = evidence.reloadedMembers.size();
        for (std::size_t i = 0; i < evidence.members.size(); ++i)
        {
            auto& member = evidence.members[i];
            if (!files[i]) continue;
            try { member.reloadedCell = ReadCell(member.output); }
            catch (...) { evidence.unresolved.emplace_back("v4_virtual_reloaded_model_member_unavailable"); }
            member.reloadedBytes = ole_content_output_observation::ExactBytes(member.output, member.expectedBytes);
            if (!member.reloadedBytes->observed) evidence.unresolved.emplace_back("v4_virtual_reloaded_bytes_unavailable");
            try { member.outputIdentityRetained = files[i]->PathStillNamesObject(member.output); }
            catch (...) { evidence.unresolved.emplace_back("v4_virtual_reloaded_identity_unavailable"); }
        }
        try { evidence.reloadedSentinelCell = ReadCell(profile.desktop / L"Sentinel.txt"); }
        catch (...) { evidence.unresolved.emplace_back("v4_virtual_reloaded_sentinel_unavailable"); }
        try
        {
            const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
                std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
            evidence.sentinelIdentityRetained = sentinel->PathStillNamesObject(profile.desktop / L"Sentinel.txt");
            evidence.sentinelBytesRetained = sentinel->HasBytes(sentinelBytes);
        }
        catch (...) { evidence.unresolved.emplace_back("v4_virtual_sentinel_identity_or_bytes_unavailable"); }
        // These are snapshots, including observable negative values. They do
        // not classify a product failure until same-receipt completion holds.
        evidence.independentAssertionsCompleted = evidence.baselineRevalidated && evidence.sourceCallsCaptured && evidence.outputEnumerationObserved &&
            evidence.refreshMessageObserved && evidence.unresolved.empty() &&
            evidence.descriptorReadObserved == std::optional<bool>{true} &&
            evidence.memberReadsObserved == std::optional<bool>{true} && evidence.outputs.size() == evidence.members.size() &&
            evidence.initialCount && evidence.finalCount && evidence.sentinelCell &&
            evidence.finalCount == std::optional<std::size_t>{*evidence.initialCount + evidence.members.size()} &&
            evidence.reloadedCount == evidence.finalCount && evidence.sentinelIdentityRetained == std::optional<bool>{true} &&
            evidence.sentinelBytesRetained == std::optional<bool>{true} && evidence.reloadedSentinelCell &&
            Same(*evidence.sentinelCell, *evidence.reloadedSentinelCell) &&
            std::all_of(evidence.members.begin(), evidence.members.end(), [](const auto& member) {
                return member.outputPresent == std::optional<bool>{true} && member.outputId &&
                    member.distinctFromSentinel == std::optional<bool>{true} &&
                    member.distinctFromOtherOutputs == std::optional<bool>{true} &&
                    member.outputIdentityRetained == std::optional<bool>{true} && member.outputBytes &&
                    member.outputBytes->observed && member.outputBytes->matches && member.reloadedBytes &&
                    member.reloadedBytes->observed && member.reloadedBytes->matches && member.outputCell &&
                    member.reloadedCell && Same(*member.outputCell, *member.reloadedCell);
            });
    }
public:
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4VirtualExecutionKey>& key,
        const std::shared_ptr<drag_global::mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
    {
        RequireObservation(key && traceStart != (std::numeric_limits<std::size_t>::max)() &&
            evidence.initialCount && evidence.initialRevision && evidence.sentinelCell && evidence.requestedLanding && sentinel,
            "v4 virtual execution baseline is unavailable");
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(ownedRoot.StillOwned() && Access::asyncOwner == profile.app && Access::FileDropIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app) && Access::LayoutRevision(*profile.app) == *evidence.initialRevision &&
            Access::Items(*profile.app).size() == *evidence.initialCount && Outputs(profile).empty() &&
            Same(ReadCell(profile.desktop / L"Sentinel.txt"), *evidence.sentinelCell) &&
            Same(*evidence.requestedLanding, landing) && sentinel->PathStillNamesObject(profile.desktop / L"Sentinel.txt") &&
            sentinel->HasBytes(sentinelBytes), "v4 virtual execution baseline changed before Drop");
        RequireObservation(verified.Status() == drag_global::V4CoordinateStatus::coordinate_ready &&
            identity.Status() == drag_global::V4FamilyStatus::schema_only && verified.FamilyBinding() == identity.FamilyBinding(),
            "v4 virtual layout context unavailable");
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        RequireObservation(!raw.resourceUnavailable && raw.errors.empty() && raw.id == evidence.candidateId,
            "v4 virtual layout candidate mismatch");
        RequireObservation(
            key->BindBaseline(owner, epoch, profile.app, static_cast<IDataObject*>(&source), evidence.candidateId, identity_),
            "v4 virtual execution baseline could not be bound");
        verified_ = &verified; familyIdentity_ = &identity;
        evidence.baselineRevalidated = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4VirtualExecutionKey>& key,
        const std::shared_ptr<drag_global::mixed_fixture::ConfiguredContentOwner>& owner,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch)
    {
        RequireObservation(key && key->BeginCapture(owner, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_), "v4 virtual business capture scope is unavailable");
        try { AssertResults(); }
        catch (const std::exception& error)
        { evidence.unresolved.push_back(std::string("v4_virtual_business_snapshot_incomplete:") + error.what()); }
        catch (...) { evidence.unresolved.emplace_back("v4_virtual_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const V4VirtualBusinessEvidence>(evidence);
        RequireObservation(key && key->Publish(owner, epoch, profile.app, static_cast<IDataObject*>(&source),
            evidence.candidateId, identity_, std::move(snapshot)), "v4 virtual business observation could not be sealed");
    }
};

struct V4VirtualOutputBindingEvidence
{
    struct Member
    {
        std::size_t descriptorIndex = 0, outputIndex = 0;
        bool actualDestinationMatches = false, outputIdentityObserved = false;
        bool bytesAndReloadObserved = false;
        std::optional<bool> landingMatches;
    };
    bool sameReceiptObserved = false, localBindingObserved = false, valueComparisonsMatch = false, observationUnavailable = true;
    std::string candidateId;
    std::vector<Member> members;
    std::vector<std::string> unresolved;
};

// Compare independently opened output files and model observations with the
// paths actually returned by the same production operation. A mismatch is
// retained as unavailable until the entire same-case chain is authenticated.
V4VirtualOutputBindingEvidence InspectV4VirtualOutputBinding(
    const drag_global::mixed_fixture::VirtualExpectedManifest& manifest,
    const drag_global::ole_driver::V4VirtualFlowEvidence& flow,
    const drag_global::ole_driver::V4VirtualRefreshEvidence& refresh,
    const V4VirtualBusinessEvidence& business, const GridCell& requestedLanding)
{
    V4VirtualOutputBindingEvidence result;
    try
    {
        result.candidateId = manifest.candidateId;
        if (!manifest.ready || manifest.resourceUnavailable || manifest.candidateId.empty() ||
            business.candidateId != manifest.candidateId || !flow.identityValid || !flow.outputMappingObserved ||
            !refresh.localRefreshBound || refresh.batch != flow.batch ||
            manifest.members.empty() || flow.members.size() != manifest.members.size() ||
            business.members.size() != manifest.members.size())
        { result.unresolved.emplace_back("v4_virtual_output_same_case_or_local_completion_unavailable"); return result; }
        std::vector<bool> usedOutputs(manifest.members.size());
        for (std::size_t i = 0; i < manifest.members.size(); ++i)
        {
            const auto& expected = manifest.members[i]; const auto& observed = business.members[i];
            const auto member = std::find_if(flow.members.begin(), flow.members.end(), [&](const auto& value) {
                return value.descriptorIndex == expected.descriptorIndex; });
            if (member == flow.members.end() ||
                std::find_if(std::next(member), flow.members.end(), [&](const auto& value) {
                    return value.descriptorIndex == expected.descriptorIndex; }) != flow.members.end() ||
                observed.descriptorIndex != expected.descriptorIndex || observed.expectedBytes != expected.bytes ||
                member->outputIndex >= usedOutputs.size() || usedOutputs[member->outputIndex])
            { result.unresolved.emplace_back("v4_virtual_output_member_identity_unavailable"); return result; }
            usedOutputs[member->outputIndex] = true;
            const auto& path = observed.output.native();
            const bool pathMatches = path.size() <= (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t) &&
                member->actualDestination.size() == path.size() * sizeof(wchar_t) &&
                (member->actualDestination.empty() ||
                 std::memcmp(member->actualDestination.data(), path.data(), member->actualDestination.size()) == 0);
            V4VirtualOutputBindingEvidence::Member value;
            value.descriptorIndex = expected.descriptorIndex; value.outputIndex = member->outputIndex;
            value.actualDestinationMatches = pathMatches; value.outputIdentityObserved = observed.outputId.has_value();
            value.bytesAndReloadObserved = observed.outputBytes && observed.outputBytes->observed && observed.outputBytes->matches &&
                observed.reloadedBytes && observed.reloadedBytes->observed && observed.reloadedBytes->matches &&
                observed.outputIdentityRetained == std::optional<bool>{true} &&
                observed.distinctFromSentinel == std::optional<bool>{true} &&
                observed.distinctFromOtherOutputs == std::optional<bool>{true} &&
                observed.outputCell && observed.reloadedCell && Same(*observed.outputCell, *observed.reloadedCell);
            if (member->outputIndex == 0 && observed.outputCell)
                value.landingMatches = Same(*observed.outputCell, requestedLanding);
            result.members.push_back(std::move(value));
        }
        const auto first = std::find_if(result.members.begin(), result.members.end(), [](const auto& item) { return item.outputIndex == 0; });
        result.valueComparisonsMatch = first != result.members.end() && first->landingMatches.has_value() && *first->landingMatches &&
            std::all_of(result.members.begin(), result.members.end(), [](const auto& item) {
                return item.actualDestinationMatches && item.outputIdentityObserved && item.bytesAndReloadObserved; });
        if (!result.valueComparisonsMatch) result.unresolved.emplace_back("v4_virtual_output_mapping_or_observation_unavailable");
        // Public value snapshots cannot authenticate that business and flow
        // came from the same receipt; their numeric values alone are not a seal.
        result.unresolved.emplace_back("v4_virtual_output_same_receipt_binding_unimplemented");
        result.unresolved.emplace_back("v4_virtual_output_staging_identity_and_full_causality_unverified");
        return result;
    }
    catch (...) { result.unresolved.emplace_back("v4_virtual_output_binding_observation_unavailable"); return result; }
}

V4VirtualOutputBindingEvidence InspectSealedV4VirtualOutputBinding(
    const drag_global::ole_driver::V4ConfiguredContentOleRunReceipt& receipt,
    const drag_global::V4FamilyValidatedContext& verified,
    const drag_global::event_graph::V4FamilyIdentityContext& identity,
    const std::vector<bool>& present, const drag_global::event_graph::V4JoinedGraphVariant& selected,
    const drag_global::mixed_fixture::ConfiguredContentBinding& source,
    const std::shared_ptr<drag_global::mixed_fixture::ConfiguredContentOwner>& owner,
    OleDragDropAdapter* adapter, const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch,
    const drag_global::ole_driver::MessageTrace& trace, HWND control,
    const drag_global::mixed_fixture::VirtualExpectedManifest& manifest, std::size_t byteBudget, std::size_t virtualBudget)
{
    V4VirtualOutputBindingEvidence result;
    try
    {
        const auto business = receipt.SealedBusiness();
        if (!business || !business->baselineRevalidated || !business->requestedLanding ||
            !receipt.SameBoundHost(source, owner, adapter, epoch, trace, control))
        { result.unresolved.emplace_back("v4_virtual_output_sealed_receipt_unavailable"); return result; }
        const auto flow = drag_global::ole_driver::InspectV4VirtualFlow(receipt, verified, identity, present, selected,
            source, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        const auto refresh = drag_global::ole_driver::InspectV4VirtualRefresh(receipt, verified, identity, present, selected,
            source, owner, adapter, epoch, trace, control, manifest, byteBudget, virtualBudget);
        if (!flow.identityValid || !flow.outputMappingObserved || !refresh.localRefreshBound || refresh.batch != flow.batch)
        { result.unresolved.emplace_back("v4_virtual_output_same_receipt_flow_or_refresh_unavailable"); return result; }
        result = InspectV4VirtualOutputBinding(manifest, flow, refresh, *business, *business->requestedLanding);
        result.sameReceiptObserved = true;
        result.unresolved.erase(std::remove(result.unresolved.begin(), result.unresolved.end(),
            "v4_virtual_output_same_receipt_binding_unimplemented"), result.unresolved.end());
        result.localBindingObserved = !result.members.empty() && result.members.size() == manifest.members.size() &&
            std::all_of(result.members.begin(), result.members.end(), [](const auto& member) {
                return member.actualDestinationMatches && member.outputIdentityObserved;
            });
        // Target/geometry, staging ownership, late work and complete product
        // result classification are separate from this local receipt binding.
        return result;
    }
    catch (...) { result.unresolved.emplace_back("v4_virtual_output_sealed_binding_unavailable"); return result; }
}

// Pure Unicode-text ingress observation. The fixed UTF-8 bytes below are an
// independent expectation; production chooses and writes the destination.
// This is not a format-precedence oracle for mixed payloads.
struct TextRefreshWindowEvidence
{
    struct Entry
    {
        NativeDragIntegrationAccess::RefreshMessageObservation refresh;
        drag_global::ole_driver::MessageObservation message;
        bool nestedObserved = false;
    };
    std::uint64_t beforeDropRevision = 0, requestedRevision = 0;
    std::vector<Entry> entries;
    std::optional<std::size_t> traceStart, traceEnd, refreshStart, refreshEnd;
    bool beforeDropRevisionObserved = false, requestedRevisionObserved = false;
    bool sameBoundHost = false, observationUnavailable = true;
    std::vector<std::string> unresolved;
};
// This contains only values read from the isolated host and filesystem in the
// same attempt. An absent field means the relevant observation did not finish.
struct TextBusinessEvidence
{
    std::vector<std::byte> expectedBytes;
    std::vector<fs::path> outputs;
    std::optional<FILE_ID_INFO> sentinelId, outputId;
    std::optional<GridCell> initialSentinelCell, outputCell, reloadedSentinelCell, reloadedOutputCell;
    std::optional<std::uint64_t> initialLayoutRevision, committedLayoutRevision;
    std::optional<ole_content_output_observation::Evidence> outputBytes, reloadedBytes;
    std::optional<std::size_t> reloadedModelCount;
    bool outputEnumerationObserved = false, assertionsCompleted = false;
};
struct GeneratedTextObservation
{
    Profile& profile;
    ole_content_source_fixture::Source& source;
    std::uint64_t initialLayoutRevision = 0, revisionBeforeDrop = 0, revisionRequested = 0;
    bool requestedRevisionObserved = false;
    std::size_t callStart = 0, traceBeforeDrop = (std::numeric_limits<std::size_t>::max)();
    std::size_t refreshBeforeDrop = (std::numeric_limits<std::size_t>::max)();
    std::size_t traceAfterCompletion = (std::numeric_limits<std::size_t>::max)();
    std::size_t refreshAfterCompletion = (std::numeric_limits<std::size_t>::max)();
    GridCell sentinelCell;
    std::unique_ptr<ObservedPhysicalFile> sentinelFile;
    mutable TextBusinessEvidence businessEvidence;
    std::vector<std::byte> expected{
        std::byte{0xef}, std::byte{0xbb}, std::byte{0xbf}, std::byte{'S'}, std::byte{'n'},
        std::byte{'o'}, std::byte{'w'}, std::byte{' '}, std::byte{0xe9}, std::byte{0x9b}, std::byte{0xaa}};

    GeneratedTextObservation(Profile& host, ole_content_source_fixture::Source& provider) : profile(host), source(provider)
    {
        RequireObservation(Access::asyncOwner == profile.app && Access::productionDispatch == 1 && Access::fixtureControlWindow &&
            Access::messageTrace, "generated text input requires its own production window routes");
        const std::wstring input = L"Snow \u96EA";
        RequireObservation(source.entries.size() == 1 && source.entries.front().format == CF_UNICODETEXT &&
            source.entries.front().index == -1 && source.entries.front().tymed == TYMED_HGLOBAL &&
            SUCCEEDED(source.entries.front().getDataFailure) && !source.entries.front().readFailure &&
            source.entries.front().bytes.size() == (input.size() + 1) * sizeof(wchar_t) &&
            std::memcmp(source.entries.front().bytes.data(), input.c_str(), source.entries.front().bytes.size()) == 0 &&
            source.ownerThread == GetCurrentThreadId(), "pure text provider contains only the exact requested Unicode input");
        { std::lock_guard lock(source.observationMutex); callStart = source.calls.size(); }
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        RequireObservation(Outputs(profile).empty() && Access::Items(*profile.app).size() == 1 &&
            Read(profile.desktop / L"Sentinel.txt") == "untouched", "generated text starts with only its isolated sentinel");
        sentinelCell = Find(*profile.app, profile.desktop / L"Sentinel.txt").gridCell;
        sentinelFile = std::make_unique<ObservedPhysicalFile>(profile.desktop / L"Sentinel.txt");
        businessEvidence.expectedBytes = expected;
        businessEvidence.sentinelId = sentinelFile->Identity();
        businessEvidence.initialSentinelCell = sentinelCell;
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireObservation(sentinelFile->HasBytes(sentinelBytes), "initial sentinel bytes belong to the retained file object");
        Access::DisableFixtureMaintenance(*profile.app);
        initialLayoutRevision = Access::LayoutRevision(*profile.app);
        businessEvidence.initialLayoutRevision = initialLayoutRevision;
    }
    void BeforeOleCall(const drag_global::ole_driver::Input& input)
    {
        if (input.kind != drag_global::ole_driver::Kind::drop) return;
        RequireObservation(traceBeforeDrop == (std::numeric_limits<std::size_t>::max)() && Access::messageTrace &&
            !Access::messageTrace->LostOrCrossThread(), "text drop trace baseline is available");
        RequireEvent(Access::Target(*profile.app) == Access::Desktop(*profile.app) &&
            Access::Region(*profile.app) == HitRegion::Empty,
            "pure text retains the actual empty desktop target immediately before Drop");
        RequireObservation(Access::FileDropIdle(*profile.app) && !Access::refreshObservationFailed,
            "text refresh begins from an idle observed host");
        traceBeforeDrop = Access::messageTrace->Observations().size();
        refreshBeforeDrop = Access::refreshMessages.size();
        revisionBeforeDrop = Access::ShellRefreshRevision(*profile.app);
    }
    bool ProductionRefreshReturned() const
    {
        RequireObservation(Access::messageTrace && !Access::messageTrace->LostOrCrossThread() && !Access::refreshObservationFailed &&
            traceBeforeDrop != (std::numeric_limits<std::size_t>::max)() &&
            refreshBeforeDrop != (std::numeric_limits<std::size_t>::max)() && revisionRequested > revisionBeforeDrop,
            "text refresh requires the recorded request revision and complete window trace");
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(traceBeforeDrop <= events.size() && refreshBeforeDrop <= Access::refreshMessages.size(),
            "text refresh observation baselines remain valid");
        for (std::size_t i = refreshBeforeDrop; i < Access::refreshMessages.size(); ++i)
        {
            const auto& refresh = Access::refreshMessages[i];
            if (!refresh.returned || refresh.traceIndex < traceBeforeDrop || refresh.traceIndex >= events.size() ||
                refresh.beforeRevision != revisionRequested || refresh.afterRevision != revisionRequested ||
                !refresh.beforeRunning || refresh.afterRunning || refresh.beforeReady || !refresh.afterReady) continue;
            const auto& event = events[refresh.traceIndex];
            if (event.window == Access::fixtureControlWindow && event.route == drag_global::ole_driver::MessageRoute::control_window_proc &&
                event.message == kBackgroundShellReadyMessage && event.returned && event.enteredThread == GetCurrentThreadId() &&
                event.returnedThread == event.enteredThread && event.returnedSequence > event.enteredSequence)
            {
                // Reject nested dispatches visible to this trace; unobserved
                // reentrancy and message-to-work attribution remain unresolved.
                const bool nested = std::any_of(events.begin(), events.end(), [&](const auto& other) {
                    return other.enteredSequence > event.enteredSequence && other.enteredSequence < event.returnedSequence;
                });
                if (!nested) return true;
            }
        }
        return false;
    }
    TextRefreshWindowEvidence CaptureRefreshWindowEvidence(
        const drag_global::ole_driver::V4OleRunReceipt& receipt,
        const drag_global::ole_driver::V4UnicodeSourceBinding& binding,
        const std::shared_ptr<drag_global::ole_driver::V4UnicodeSourceOwner>& owner,
        OleDragDropAdapter* adapter,
        const std::shared_ptr<const drag_global::ole_driver::V3HostEpoch>& epoch) const
    {
        TextRefreshWindowEvidence result;
        result.beforeDropRevision = revisionBeforeDrop;
        result.requestedRevision = revisionRequested;
        result.beforeDropRevisionObserved = traceBeforeDrop != (std::numeric_limits<std::size_t>::max)();
        result.requestedRevisionObserved = requestedRevisionObserved;
        if (!Access::messageTrace || Access::refreshObservationFailed || Access::messageTrace->LostOrCrossThread() ||
            traceBeforeDrop == (std::numeric_limits<std::size_t>::max)() ||
            refreshBeforeDrop == (std::numeric_limits<std::size_t>::max)() ||
            !receipt.SameBoundHost(binding, owner, adapter, epoch, *Access::messageTrace, Access::fixtureControlWindow))
        { result.unresolved.emplace_back("text_refresh_same_host_or_baseline_unavailable"); return result; }
        result.sameBoundHost = true;
        const auto& events = Access::messageTrace->Observations();
        if (traceAfterCompletion == (std::numeric_limits<std::size_t>::max)() ||
            refreshAfterCompletion == (std::numeric_limits<std::size_t>::max)() ||
            traceBeforeDrop > traceAfterCompletion || traceAfterCompletion > events.size() ||
            refreshBeforeDrop > refreshAfterCompletion || refreshAfterCompletion > Access::refreshMessages.size())
        { result.unresolved.emplace_back("text_refresh_baseline_invalid"); return result; }
        result.traceStart = traceBeforeDrop; result.traceEnd = traceAfterCompletion;
        result.refreshStart = refreshBeforeDrop; result.refreshEnd = refreshAfterCompletion;
        for (std::size_t i = refreshBeforeDrop; i < refreshAfterCompletion; ++i)
        {
            const auto& refresh = Access::refreshMessages[i];
            if (refresh.traceIndex < traceBeforeDrop || refresh.traceIndex >= traceAfterCompletion)
            { result.unresolved.emplace_back("text_refresh_message_trace_unavailable"); continue; }
            const auto& message = events[refresh.traceIndex];
            const bool nested = std::any_of(events.begin() + traceBeforeDrop, events.begin() + traceAfterCompletion, [&](const auto& other) {
                return other.enteredSequence > message.enteredSequence && other.enteredSequence < message.returnedSequence;
            });
            result.entries.push_back({refresh, message, nested});
        }
        if (revisionRequested <= revisionBeforeDrop || result.entries.size() != 1)
            result.unresolved.emplace_back("text_refresh_revision_or_unique_window_message_unavailable");
        if (result.entries.size() == 1)
        {
            const auto& entry = result.entries.front();
            const auto& message = entry.message; const auto& refresh = entry.refresh;
            if (!refresh.returned || !message.returned || message.window != Access::fixtureControlWindow ||
                message.route != drag_global::ole_driver::MessageRoute::control_window_proc ||
                message.message != kBackgroundShellReadyMessage || message.enteredThread != GetCurrentThreadId() ||
                message.returnedThread != message.enteredThread || message.returnedSequence <= message.enteredSequence ||
                refresh.beforeRevision != revisionRequested || refresh.afterRevision != revisionRequested ||
                !refresh.beforeRunning || refresh.afterRunning || refresh.beforeReady || !refresh.afterReady || entry.nestedObserved)
                result.unresolved.emplace_back("text_refresh_window_completion_unavailable");
        }
        // A matching message and revision do not identify the worker result or
        // prove that this Drop requested it; keep those separate obligations.
        result.unresolved.emplace_back("text_refresh_request_and_worker_attribution_unimplemented");
        result.observationUnavailable = true;
        return result;
    }
    void ObserveOleReturn(const drag_global::ole_driver::Observation& event)
    {
        using namespace drag_global::ole_driver;
        const auto point = Access::ExternalPoint({150, 150});
        RequireObservation(event.keyState && event.offeredEffect && event.returnedEffect && event.screenPoint,
            "text OLE event data is incomplete");
        RequireEvent(SUCCEEDED(event.result) && *event.offeredEffect == DROPEFFECT_COPY &&
            event.screenPoint->x == point.x && event.screenPoint->y == point.y,
            "text OLE event records the offered effect and actual screen point");
        if (event.kind == Kind::enter)
            RequireEvent(*event.keyState == MK_LBUTTON && *event.returnedEffect == DROPEFFECT_COPY &&
                Access::Target(*profile.app) == Access::Desktop(*profile.app) && Access::Region(*profile.app) == HitRegion::Empty,
                "text OLE enter reaches the empty desktop target with copy effect");
        else if (event.kind == Kind::drop)
        {
            RequireEvent(*event.keyState == MK_CONTROL && *event.returnedEffect == DROPEFFECT_COPY,
                "text OLE release reports copy");
            revisionRequested = Access::ShellRefreshRevision(*profile.app);
            requestedRevisionObserved = true;
            RequireObservation(traceBeforeDrop != (std::numeric_limits<std::size_t>::max)() && revisionRequested > revisionBeforeDrop,
                "the successful text Drop schedules a new production refresh revision");
        }
        else throw ObservationUnavailable("pure text observation does not handle this OLE event");
    }
    void Await(const drag_global::ole_driver::Observation& terminal)
    {
        RequireObservation(terminal.returnedEffect.has_value(), "pure text terminal effect was not observed");
        RequireEvent(terminal.kind == drag_global::ole_driver::Kind::drop && SUCCEEDED(terminal.result) &&
            *terminal.returnedEffect == DROPEFFECT_COPY,
            "pure text drop has a successful production terminal event");
        // Wait for the observed host completion, then check model contents in
        // the business oracle. A wrong item count must not masquerade as a timeout.
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            ProductionRefreshReturned(); });
        // Freeze the observation interval before the business oracle requests
        // a later layout reload, whose messages are a different operation.
        RequireObservation(Access::messageTrace && !Access::messageTrace->LostOrCrossThread() && !Access::refreshObservationFailed,
            "text completion trace boundary unavailable");
        traceAfterCompletion = Access::messageTrace->Observations().size();
        refreshAfterCompletion = Access::refreshMessages.size();
    }
    void AssertResults() const
    {
        RequireObservation(!source.wrongApartment.load() && !source.observationFailed.load(),
            "text source call observation remains complete on its owning apartment");
        bool actualTextRead = false;
        {
            std::lock_guard lock(source.observationMutex);
            RequireObservation(callStart <= source.calls.size(), "text call baseline remains valid");
            for (std::size_t i = callStart; i < source.calls.size(); ++i)
            {
                const auto& call = source.calls[i];
                if (!call.query && call.format == CF_UNICODETEXT && call.index == -1 &&
                    call.aspect == DVASPECT_CONTENT && (call.requestedTymed & TYMED_HGLOBAL) &&
                    !call.targetDevicePresent && SUCCEEDED(call.result) && call.thread == source.ownerThread)
                    actualTextRead = true;
            }
        }
        RequireObservation(actualTextRead, "production Unicode data read was not observed");
        const auto outputs = Outputs(profile);
        businessEvidence.outputs = outputs;
        businessEvidence.outputEnumerationObserved = true;
        RequireBusiness(outputs.size() == 1 && _wcsicmp(outputs.front().extension().c_str(), L".txt") == 0,
            "pure text creates one physical text output");
        const auto evidence = ole_content_output_observation::ExactBytes(outputs.front(), expected);
        businessEvidence.outputBytes = evidence;
        RequireObservation(evidence.observed, "text output bytes could not be observed: " + evidence.error);
        RequireBusiness(evidence.matches, "text output has the independently expected UTF-8 bytes");
        ObservedPhysicalFile outputFile(outputs.front());
        businessEvidence.outputId = outputFile.Identity();
        const std::vector<std::byte> sentinelBytes{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
            std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
        RequireBusiness(!sentinelFile->SameObject(outputFile) && outputFile.HasBytes(expected) &&
            sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinelFile->HasBytes(sentinelBytes),
            "text drop retains the original sentinel file and bytes");
        const auto cell = FindCopyBusiness(*profile.app, outputs.front()).gridCell;
        businessEvidence.outputCell = cell;
        RequireBusiness(cell.pageId == L"__page:1" && cell.column == 1 && cell.row == 1,
            "text output lands at the requested desktop cell");
        Wait([&] { return Access::LayoutSaveIdle(*profile.app); });
        businessEvidence.committedLayoutRevision = Access::LayoutRevision(*profile.app);
        RequireBusiness(*businessEvidence.committedLayoutRevision > initialLayoutRevision,
            "production text drop schedules or performs layout persistence");
        Access::PerturbBeforeDiskRead(*profile.app);
        Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        businessEvidence.reloadedSentinelCell = FindCopyBusiness(*profile.app, profile.desktop / L"Sentinel.txt").gridCell;
        businessEvidence.reloadedOutputCell = FindCopyBusiness(*profile.app, outputs.front()).gridCell;
        RequireBusiness(Same(*businessEvidence.reloadedSentinelCell, sentinelCell) &&
            Same(*businessEvidence.reloadedOutputCell, cell),
            "text drop preserves both desktop member locations after reload");
        const auto afterReload = ole_content_output_observation::ExactBytes(outputs.front(), expected);
        businessEvidence.reloadedBytes = afterReload;
        businessEvidence.reloadedModelCount = Access::Items(*profile.app).size();
        RequireObservation(afterReload.observed, "reloaded text bytes could not be observed: " + afterReload.error);
        RequireBusiness(afterReload.matches && *businessEvidence.reloadedModelCount == 2 &&
            sentinelFile->PathStillNamesObject(profile.desktop / L"Sentinel.txt") && sentinelFile->HasBytes(sentinelBytes) &&
            outputFile.PathStillNamesObject(outputs.front()) && outputFile.HasBytes(expected),
            "text bytes, physical identities and desktop membership remain available after production reload");
        businessEvidence.assertionsCompleted = true;
    }
};

struct NativeMemberBusinessSnapshot
{
    bool sourceObservationAvailable = false, sourcePresent = false;
    std::vector<std::wstring> sourceKeys;
    std::optional<std::size_t> freeCopies;
    std::optional<GridCell> landing;
    std::optional<ole_content_output_observation::Evidence> movingBytes, sentinelBytes;
};
struct V4NativeDesktopLayoutSnapshot;
struct NativeMemberBusinessEvidence
{
    fs::path moving, sentinel;
    std::wstring movingKey, sentinelKey;
    GridCell expectedLanding;
    std::vector<std::byte> expectedMoving, expectedSentinel;
    std::optional<FILE_ID_INFO> movingId, sentinelId;
    std::optional<std::uint64_t> initialRevision, committedRevision;
    NativeMemberBusinessSnapshot afterDrop, afterReload;
    std::shared_ptr<const V4NativeDesktopLayoutSnapshot> beforeReloadLayout;
    std::optional<std::string> localBusinessMismatch;
    std::vector<std::string> unresolved;
    bool captureObserved = false, assertionsCompleted = false;
};

// Restricted collection-member result observer. The fixture creates only the
// initial membership; a real device and window route must capture and commit
// the gesture before these independent file and layout checks are used.
struct GeneratedNativeMemberObservation
{
private:
    friend struct NativeMemberDeviceObservation;
    OwnedProfileDesktopRoot& root;
    Profile& profile;
    fs::path moving, sentinel;
    std::vector<std::byte> expectedMoving;
    const std::vector<std::byte> expectedSentinel{std::byte{'u'}, std::byte{'n'}, std::byte{'t'}, std::byte{'o'},
        std::byte{'u'}, std::byte{'c'}, std::byte{'h'}, std::byte{'e'}, std::byte{'d'}};
    GridCell expectedLanding;
    std::wstring movingKey, sentinelKey;
    std::optional<DesktopWidget> initialWidget;
    std::uint64_t initialRevision = 0;
    std::unique_ptr<ObservedPhysicalFile> movingFile, sentinelFile;
    bool captured = false;
    NativeMemberBusinessEvidence businessEvidence;
    std::string candidateId_;
    bool baselineBound_ = false, captureStarted_ = false;
    std::shared_ptr<const drag_global::ole_driver::V4NativeMemberObserverIdentity> identity_{
        new drag_global::ole_driver::V4NativeMemberObserverIdentity};
    void CaptureLayoutBeforeReload();
    static Profile& BoundProfile(OwnedProfileDesktopRoot& ownedRoot)
    {
        RequireObservation(ownedRoot.profile && ownedRoot.profile->app && ownedRoot.StillOwned(), "native member root unavailable");
        return *ownedRoot.profile;
    }

public:
    GeneratedNativeMemberObservation(const GeneratedNativeMemberObservation&) = delete;
    GeneratedNativeMemberObservation& operator=(const GeneratedNativeMemberObservation&) = delete;
    GeneratedNativeMemberObservation(GeneratedNativeMemberObservation&&) = delete;
    GeneratedNativeMemberObservation& operator=(GeneratedNativeMemberObservation&&) = delete;
    NativeMemberBusinessEvidence Evidence() const { return businessEvidence; }
    const GridCell& ExpectedLanding() const noexcept { return expectedLanding; }
    GeneratedNativeMemberObservation(OwnedProfileDesktopRoot& ownedRoot, fs::path source, std::vector<std::byte> bytes, GridCell landing,
        const drag_global::V4FamilyValidatedContext& verified,
        const drag_global::event_graph::V4FamilyIdentityContext& identity)
        : root(ownedRoot), profile(BoundProfile(ownedRoot)), moving(std::move(source)), sentinel(profile.desktop / L"Sentinel.txt"),
          expectedMoving(std::move(bytes)), expectedLanding(std::move(landing))
    {
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        RequireObservation(root.StillOwned() && root.profile && root.profile->app &&
            verified.Status() == drag_global::V4CoordinateStatus::coordinate_ready &&
            identity.Status() == drag_global::V4FamilyStatus::schema_only &&
            verified.FamilyBinding() == identity.FamilyBinding() && !raw.resourceUnavailable && raw.errors.empty() && !raw.id.empty(),
            "native member candidate or root unavailable");
        candidateId_ = raw.id;
        RequireObservation(Access::asyncOwner == profile.app && Access::nativePointerDispatch &&
            Access::productionDispatch == 1 && Access::messageTrace && Access::fixtureControlWindow,
            "native member observation requires its isolated production routes");
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::NativeDragIdle(*profile.app); });
        const auto& movingItem = Find(*profile.app, moving);
        const auto& sentinelItem = Find(*profile.app, sentinel);
        movingKey = movingItem.layoutKey; sentinelKey = sentinelItem.layoutKey;
        businessEvidence.moving = moving; businessEvidence.sentinel = sentinel;
        businessEvidence.movingKey = movingKey; businessEvidence.sentinelKey = sentinelKey;
        businessEvidence.expectedLanding = expectedLanding;
        businessEvidence.expectedMoving = expectedMoving; businessEvidence.expectedSentinel = expectedSentinel;
        RequireObservation(!movingItem.selected && !sentinelItem.selected && movingKey != sentinelKey,
            "native member input starts with distinct unselected items");
        const auto* widget = SourceWidget();
        RequireObservation(widget && widget->type == DesktopWidgetType::Collection && widget->itemKeys.size() == 2 &&
            std::count(widget->itemKeys.begin(), widget->itemKeys.end(), movingKey) == 1 &&
            std::count(widget->itemKeys.begin(), widget->itemKeys.end(), sentinelKey) == 1,
            "native member input has the two independently identified collection members");
        initialWidget = *widget;
        movingFile = std::make_unique<ObservedPhysicalFile>(moving);
        sentinelFile = std::make_unique<ObservedPhysicalFile>(sentinel);
        businessEvidence.movingId = movingFile->Identity(); businessEvidence.sentinelId = sentinelFile->Identity();
        RequireObservation(!movingFile->SameObject(*sentinelFile) && movingFile->HasBytes(expectedMoving) &&
            sentinelFile->HasBytes(expectedSentinel), "native member input bytes and file identities are available");
        RequireObservation(Access::Save(*profile.app), "native member initial layout could not be persisted");
        Wait([&] { return Access::LayoutSaveIdle(*profile.app); });
        initialRevision = Access::LayoutRevision(*profile.app);
        businessEvidence.initialRevision = initialRevision;
    }
private:
    const DesktopWidget* SourceWidget() const
    {
        const DesktopWidget* found = nullptr;
        for (const auto& widget : Access::Widgets(*profile.app))
        {
            if (widget.id != L"native-origin") continue;
            if (found) throw drag_global::ole_driver::ObservationUnavailable("native member source widget identity ambiguous");
            found = &widget;
        }
        return found;
    }
    void VerifyBaseline() const
    {
        const auto* widget = SourceWidget();
        RequireObservation(initialWidget && widget && widget->type == initialWidget->type &&
            widget->itemKeys == initialWidget->itemKeys && Same(widget->gridCell, initialWidget->gridCell) &&
            widget->gridSpan.columns == initialWidget->gridSpan.columns && widget->gridSpan.rows == initialWidget->gridSpan.rows &&
            Access::LayoutRevision(*profile.app) == initialRevision,
            "native member initial membership or layout changed before input");
        const auto& movingItem = Find(*profile.app, moving);
        const auto& sentinelItem = Find(*profile.app, sentinel);
        RequireObservation(movingItem.layoutKey == movingKey && sentinelItem.layoutKey == sentinelKey &&
            !movingItem.selected && !sentinelItem.selected && movingFile->PathStillNamesObject(moving) &&
            sentinelFile->PathStillNamesObject(sentinel) && movingFile->HasBytes(expectedMoving) &&
            sentinelFile->HasBytes(expectedSentinel), "native member initial selection or physical inputs changed");
    }
    void ObserveCapture()
    {
        RequireEvent(Access::NativeMemberDragCaptured(*profile.app, moving),
            "actual native drag captures the requested source member");
        captured = true;
        businessEvidence.captureObserved = true;
    }
    void AwaitCompletion() const
    {
        RequireObservation(captured, "native member source capture has not been observed");
        Wait([&] { return Access::NativeDragIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::ContainerWorkIdle(*profile.app); });
    }
    void AssertResults()
    {
        RequireObservation(captured, "native member business oracle requires the observed production capture");
        const auto check = [&](NativeMemberBusinessSnapshot& snapshot) {
            const auto* widget = SourceWidget();
            snapshot.sourceObservationAvailable = true; snapshot.sourcePresent = widget != nullptr;
            if (widget) snapshot.sourceKeys = widget->itemKeys;
            RequireBusiness(widget && widget->type == DesktopWidgetType::Collection &&
                widget->itemKeys.size() == 1 && widget->itemKeys.front() == sentinelKey,
                "the production drop removes only the moved member from its collection");
            RequireObservation(initialWidget.has_value(), "native member initial widget observation unavailable");
            RequireBusiness(widget->title == initialWidget->title && widget->customTitle == initialWidget->customTitle &&
                Same(widget->gridCell, initialWidget->gridCell) &&
                widget->gridSpan.columns == initialWidget->gridSpan.columns && widget->gridSpan.rows == initialWidget->gridSpan.rows &&
                widget->autoCollect == initialWidget->autoCollect && widget->scrollContainerMode == initialWidget->scrollContainerMode &&
                widget->singleCellIconMode == initialWidget->singleCellIconMode,
                "native member transfer preserves the source collection settings and layout");
            RequireObservation(Access::DesktopOrNull(*profile.app) != nullptr, "native desktop container unavailable");
            snapshot.freeCopies = Access::ObserveFreeDesktopCopies(*profile.app, moving);
            RequireObservation(snapshot.freeCopies.has_value(), "native desktop file identities unavailable");
            snapshot.landing = FindCopyBusiness(*profile.app, moving).gridCell;
            RequireBusiness(*snapshot.freeCopies == 1 && Same(*snapshot.landing, expectedLanding),
                "the moved member appears once at the requested desktop landing");
            snapshot.movingBytes = ole_content_output_observation::ExactBytes(moving, expectedMoving);
            snapshot.sentinelBytes = ole_content_output_observation::ExactBytes(sentinel, expectedSentinel);
            RequireObservation(snapshot.movingBytes->observed && snapshot.sentinelBytes->observed,
                "native member output byte observations unavailable");
            RequireBusiness(movingFile->PathStillNamesObject(moving) && sentinelFile->PathStillNamesObject(sentinel) &&
                snapshot.movingBytes->matches && snapshot.sentinelBytes->matches &&
                movingFile->HasBytes(expectedMoving) && sentinelFile->HasBytes(expectedSentinel),
                "logical ownership transfer preserves both physical file identities and contents");
        };
        check(businessEvidence.afterDrop);
        businessEvidence.committedRevision = Access::LayoutRevision(*profile.app);
        RequireBusiness(*businessEvidence.committedRevision > initialRevision,
            "native member drop schedules or commits a layout change");
        CaptureLayoutBeforeReload();
        Access::PerturbBeforeDiskRead(*profile.app);
        Access::RequestAsync(*profile.app, true);
        Wait([&] { return Access::ContainerWorkIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app); });
        check(businessEvidence.afterReload);
        businessEvidence.assertionsCompleted = true;
    }
public:
    void BindExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeMemberExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && !baselineBound_ && !captureStarted_ && !captured && root.StillOwned() &&
            root.profile && root.profile.get() == &profile && profile.app && Access::SameNativeHostEpoch(*profile.app, epoch),
            "native member same-run baseline unavailable");
        VerifyBaseline();
        RequireObservation(key->BindBaseline(epoch, profile.app, candidateId_, identity_), "native member baseline binding unavailable");
        baselineBound_ = true;
    }
    void CaptureExecution(const std::shared_ptr<drag_global::ole_driver::V4NativeMemberExecutionKey>& key,
        const std::shared_ptr<drag_global::ole_driver::NativeHostEpoch>& epoch)
    {
        RequireObservation(key && baselineBound_ && !captureStarted_ && captured && root.StillOwned() &&
            root.profile && root.profile.get() == &profile && profile.app &&
            key->BeginCapture(epoch, profile.app, candidateId_, identity_), "native member capture binding unavailable");
        captureStarted_ = true;
        try { AssertResults(); }
        catch (const drag_global::ole_driver::BusinessAssertionFailure& error)
        { businessEvidence.localBusinessMismatch = error.what(); businessEvidence.unresolved.emplace_back("native_member_business_mismatch_attribution_unverified"); }
        catch (const std::exception& error)
        { businessEvidence.unresolved.push_back(std::string("native_member_business_snapshot_incomplete:") + error.what()); }
        catch (...) { businessEvidence.unresolved.emplace_back("native_member_business_snapshot_incomplete"); }
        auto snapshot = std::make_shared<const NativeMemberBusinessEvidence>(businessEvidence);
        RequireObservation(key->Publish(epoch, profile.app, candidateId_, identity_, std::move(snapshot)),
            "native member sealing unavailable");
    }
};

// Value-only host observations for the restricted successful member move.
// A callback match is local evidence, not authorization to execute a v4 graph.
struct NativeMemberDeviceObservation
{
    GeneratedNativeMemberObservation& business;
    drag_global::EventProgram program;
    std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> epoch;
    POINT startScreen{}, targetScreen{};
    std::size_t traceStart = 0, beforeEnd = (std::numeric_limits<std::size_t>::max)();
    bool initialHover = false, ended = false;
    std::vector<bool> seen;
    std::vector<std::optional<std::size_t>> moveClasses;
    std::map<std::size_t, snowdesktop::private_pointer::QueuedEventIdentity> classTokens;
    std::map<std::pair<std::uint64_t, std::uint64_t>, std::size_t> tokenClasses;
    std::optional<snowdesktop::private_pointer::QueuedEventIdentity> lastQueued;

    NativeMemberDeviceObservation(GeneratedNativeMemberObservation& observer,
        const drag_global::event_graph::V4FamilyIdentityContext& identity,
        const drag_global::V4FamilyValidatedContext& verified, const std::vector<bool>& present,
        const drag_global::event_graph::V4NativeJoinedGraphVariant& graph,
        std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> host, POINT start, POINT target)
        : business(observer), program(verified.TransferProgram()), epoch(std::move(host)), startScreen(start), targetScreen(target),
          seen(program.steps.size(), false)
    {
        RequireHost();
        const auto classes = drag_global::ole_driver::V4NativeMoveClassEvidence::Inspect(identity, verified, present, graph);
        RequireObservation(classes.IdentityValid() && classes.CompleteMoveBindings() && !classes.ResourceUnavailable() &&
            classes.ByProgramIndex().size() == program.steps.size(), "native member selected move classes unavailable");
        moveClasses = classes.ByProgramIndex();
        traceStart = Access::messageTrace->Observations().size();
    }
    void RequireHost() const
    {
        RequireObservation(Access::SameNativeHostEpoch(*business.profile.app, epoch),
            "native member same-host epoch unavailable");
    }
    void BeforeBegin() const
    {
        RequireHost();
        RequireObservation(Access::NativeDragIdle(*business.profile.app) && Access::ContainerWorkIdle(*business.profile.app) &&
            Access::LayoutSaveIdle(*business.profile.app), "native member pre-input host unavailable");
        business.VerifyBaseline();
        const auto client = Access::NativeMemberInputCandidate(*business.profile.app, business.moving);
        const auto screen = Access::ExternalPoint(client);
        RequireObservation(screen.x == startScreen.x && screen.y == startScreen.y,
            "native member candidate coordinates changed before input");
        const auto landing = Access::InspectNativeLanding(*business.profile.app, business.expectedLanding);
        RequireObservation(landing.screenCenter.x == targetScreen.x && landing.screenCenter.y == targetScreen.y &&
            landing.layoutRevision == business.initialRevision, "native member landing baseline changed before input");
    }
    void Observe(drag_global::ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
        const snowdesktop::private_pointer::State& state,
        const snowdesktop::private_pointer::DeliveryObservationLog& log,
        std::optional<snowdesktop::private_pointer::QueuedEventIdentity> queued,
        std::optional<drag_global::ole_driver::NativeInputExpectation> expected)
    {
        using namespace drag_global::ole_driver;
        using namespace snowdesktop::private_pointer;
        RequireHost();
        RequireObservation(!log.failed && log.session && log.ownerThread == GetCurrentThreadId() &&
            state.error.empty() && state.pendingEvents == 0 &&
            std::none_of(state.keys.begin(), state.keys.end(), [](bool pressed) { return pressed; }),
            "native member device observation unavailable");
        if (phase == NativeDevicePhase::session_end)
        {
            RequireObservation(!queued && !expected && index && *index < program.steps.size() && !state.enabled &&
                program.steps[*index].kind == "end_pointer_session" && state.buttons == 0 && initialHover && !ended,
                "native member end state unavailable");
            ended = true;
            return;
        }
        RequireObservation(queued && expected && queued->ownerThread == log.ownerThread && queued->session == log.session &&
            queued->event && lastQueued && queued->ownerThread == lastQueued->ownerThread &&
            queued->session == lastQueued->session && queued->event == lastQueued->event && Access::messageTrace,
            "native member queued input identity unavailable");
        const DeliveryObservation* primary = nullptr;
        for (const auto& event : log.entries)
        {
            if (event.kind != DeliveryObservationKind::begin || !event.identity.queuedEvent || event.identity.forwarded ||
                event.identity.ownerThread != queued->ownerThread || event.identity.session != queued->session ||
                event.identity.event != queued->event) continue;
            RequireObservation(!primary, "native member queued input delivery ambiguous");
            primary = &event;
        }
        RequireObservation(primary && primary->window == Access::completionWindow,
            "native member queued input delivery unavailable");
        POINT client = expected->screen;
        RequireObservation(ScreenToClient(Access::completionWindow, &client) != FALSE &&
            client.x == expected->clientBeforeDispatch.x && client.y == expected->clientBeforeDispatch.y &&
            client.x >= (std::numeric_limits<short>::min)() && client.x <= (std::numeric_limits<short>::max)() &&
            client.y >= (std::numeric_limits<short>::min)() && client.y <= (std::numeric_limits<short>::max)(),
            "native member input coordinate observation unavailable");
        POINT roundTrip = client;
        RequireObservation(ClientToScreen(Access::completionWindow, &roundTrip) != FALSE &&
            roundTrip.x == expected->screen.x && roundTrip.y == expected->screen.y,
            "native member input coordinate mapping is not reversible");
        const auto point = MAKELPARAM(static_cast<short>(client.x), static_cast<short>(client.y));
        RequireObservation(primary->message == expected->message && primary->wParam == expected->buttons && primary->lParam == point,
            "native member input differs from requested delivery");
        const auto evidence = InspectNativeDelivery(*Access::messageTrace, traceStart, &log, primary->identity,
            primary->window, MessageRoute::desktop_window_proc, expected->message, expected->buttons, point);
        RequireObservation(evidence.matchedMessageAndPermit, "native member production window return unavailable");
        if (phase == NativeDevicePhase::initial_hover)
        {
            RequireObservation(!index && !initialHover && state.enabled && state.buttons == 0 &&
                expected->screen.x == startScreen.x && expected->screen.y == startScreen.y,
                "native member initial hover unavailable");
            initialHover = true;
        }
        else
        {
            RequireObservation(initialHover && index && *index < seen.size() && !seen[*index],
                "native member program input identity unavailable");
            const auto& kind = program.steps[*index].kind;
            if (kind == "source_start")
                RequireObservation(expected->message == WM_LBUTTONDOWN && expected->buttons == MK_LBUTTON &&
                    expected->screen.x == startScreen.x && expected->screen.y == startScreen.y,
                    "native member press input unavailable");
            else if (kind == "release")
                RequireObservation(expected->message == WM_LBUTTONUP && expected->buttons == 0 &&
                    expected->screen.x == targetScreen.x && expected->screen.y == targetScreen.y,
                    "native member release input unavailable");
            else
            {
                RequireObservation(expected->message == WM_MOUSEMOVE && expected->buttons == MK_LBUTTON,
                    "native member movement input unavailable");
                RequireObservation(moveClasses[*index].has_value(), "native member movement graph class unavailable");
                const auto classId = *moveClasses[*index];
                const auto [classEntry, classInserted] = classTokens.emplace(classId, *queued);
                RequireObservation(classInserted || (classEntry->second.ownerThread == queued->ownerThread &&
                    classEntry->second.session == queued->session && classEntry->second.event == queued->event),
                    "native member shared movement class has different queued tokens");
                const auto [tokenEntry, tokenInserted] = tokenClasses.emplace(std::make_pair(queued->session, queued->event), classId);
                RequireObservation(tokenInserted || tokenEntry->second == classId,
                    "native member distinct movement classes share a queued token");
            }
            seen[*index] = true;
        }
    }
    bool ProductionInputEntered() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(traceStart <= events.size(), "native member input trace baseline unavailable");
        RequireObservation(lastQueued.has_value(), "native member pre-dispatch token unavailable");
        const bool queuedInput = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(traceStart), events.end(), [&](const auto& event) {
            return event.window == Access::completionWindow && event.route == drag_global::ole_driver::MessageRoute::desktop_window_proc &&
                event.enteredThread == GetCurrentThreadId() && event.inputLParam && event.delivery.queuedEvent &&
                !event.delivery.forwarded && event.delivery.ownerThread == lastQueued->ownerThread &&
                event.delivery.session == lastQueued->session && event.delivery.event == lastQueued->event && event.delivery.delivery &&
                (event.message == WM_MOUSEMOVE || event.message == WM_LBUTTONDOWN || event.message == WM_LBUTTONUP);
        });
        if (queuedInput) return true;
        if (beforeEnd != (std::numeric_limits<std::size_t>::max)())
        {
            RequireObservation(beforeEnd <= events.size(), "native member cleanup trace baseline unavailable");
            const bool cleanupInput = std::any_of(events.begin() + static_cast<std::ptrdiff_t>(beforeEnd), events.end(), [&](const auto& event) {
                return event.window == Access::completionWindow && event.route == drag_global::ole_driver::MessageRoute::desktop_window_proc &&
                    event.enteredThread == GetCurrentThreadId() && !event.delivery.queuedEvent && !event.delivery.forwarded &&
                    event.delivery.ownerThread == lastQueued->ownerThread && event.delivery.session == lastQueued->session &&
                    event.delivery.event == 0 && event.delivery.delivery;
            });
            if (cleanupInput) return true;
            throw drag_global::ole_driver::ObservationUnavailable("native member cleanup entry cannot be excluded");
        }
        return false;
    }
    void BeforeQueued(drag_global::ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
        const snowdesktop::private_pointer::QueuedEventIdentity& token)
    {
        RequireHost();
        RequireObservation(token.ownerThread == GetCurrentThreadId() && token.session && token.event &&
            ((phase == drag_global::ole_driver::NativeDevicePhase::initial_hover && !index) ||
                (phase == drag_global::ole_driver::NativeDevicePhase::program_input && index && *index < program.steps.size())),
            "native member pre-dispatch token unavailable");
        lastQueued = token;
    }
    void BeforeEnd(const snowdesktop::private_pointer::State& state,
        const snowdesktop::private_pointer::DeliveryObservationLog& log)
    {
        RequireHost();
        RequireObservation(initialHover && !ended && lastQueued && !log.failed && log.session == lastQueued->session &&
            log.ownerThread == lastQueued->ownerThread && beforeEnd == (std::numeric_limits<std::size_t>::max)() &&
            state.enabled && !state.closing && state.pendingEvents == 0 && state.buttons == 0 && !state.captured &&
            state.point.x == targetScreen.x && state.point.y == targetScreen.y,
            "native member released pointer boundary unavailable");
        beforeEnd = Access::messageTrace->Observations().size();
    }
    void Ended() const
    {
        RequireHost();
        const auto& events = Access::messageTrace->Observations();
        RequireObservation(ended && beforeEnd != (std::numeric_limits<std::size_t>::max)() && beforeEnd <= events.size(),
            "native member pointer end boundary unavailable");
        bool cancel = false, leave = false;
        for (std::size_t i = beforeEnd; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.window != Access::completionWindow || event.route != drag_global::ole_driver::MessageRoute::desktop_window_proc ||
                !event.returned || event.enteredThread != GetCurrentThreadId() || event.returnedThread != event.enteredThread) continue;
            if (!cancel && event.message == WM_CANCELMODE) cancel = true;
            else if (cancel && event.message == WM_MOUSELEAVE) leave = true;
        }
        RequireObservation(cancel && leave, "native member pointer-end messages unavailable");
    }
    drag_global::ole_driver::NativeHostBoundaries Boundaries()
    {
        return {
            [this] { BeforeBegin(); },
            [this](auto phase, auto index, const auto& state, const auto& log, auto queued, auto expected) {
                Observe(phase, index, state, log, queued, expected); },
            [this](const auto&) { RequireHost(); business.ObserveCapture(); RequireHost(); },
            [this](const auto&) { RequireHost(); business.AwaitCompletion(); RequireHost(); },
            [this](const auto&, const auto& state, const auto& log) { BeforeEnd(state, log); },
            [this](const auto&) { Ended(); },
            [](const auto&) { throw drag_global::ole_driver::ObservationUnavailable("native member requires sealed execution observer"); },
            [this] { RequireHost(); Access::ThrowAsyncException(); },
            [this] { return ProductionInputEntered(); },
            [this](auto phase, auto index, const auto& token) { BeforeQueued(phase, index, token); }};
    }
};

// Observation for one generated desktop item moved to an empty desktop cell.
// The private pointer and production window procedure perform the gesture;
// this fixture only reads capture, completion, file identity and saved layout.
struct GeneratedNativeMoveObservation
{
    GeneratedPayloadProfile& generated;
    const drag_global::EventProgram& program;
    fs::path original;
    POINT startScreen{}, targetScreen{};
    std::uint64_t deviceSequence = 0;
    bool initialDeviceHover = false, deviceEnded = false, deviceCaptured = false;
    std::vector<bool> observedProgramInputs;
    std::vector<std::optional<std::size_t>> moveClasses;
    std::size_t traceStart = 0;
    std::size_t traceBeforeEnd = (std::numeric_limits<std::size_t>::max)();
    std::uint64_t captureSequence = 0;
    std::uint64_t initialLayoutRevision = 0;
    GridCell originalCell;
    RECT originalBounds{};
    std::vector<std::pair<fs::path, GridCell>> expectedCells;

    GeneratedNativeMoveObservation(GeneratedPayloadProfile& fixture, const drag_global::EventProgram& steps,
        std::vector<std::optional<std::size_t>> classes)
        : generated(fixture), program(steps), observedProgramInputs(steps.steps.size(), false), moveClasses(std::move(classes))
    {
        Require(moveClasses.size() == program.steps.size(), "native event classes match the complete program");
        auto& profile = *generated.profile;
        Require(generated.after.captured && generated.payload.members.size() == 1 &&
            generated.payload.members.front().kind == "file" && generated.after.memberEntryIndices.size() == 1 &&
            generated.after.memberEntryIndices.front() < generated.after.entries.size(),
            "generated native move requires one captured file");
        Require(Access::asyncOwner == profile.app && Access::productionDispatch == 1 &&
            Access::nativePointerDispatch && Access::messageTrace,
            "generated native move requires its owning production window route");
        original = generated.payload.members.front().path;
        Wait([&] { return Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::NativeDragIdle(*profile.app); });
        const auto current = drag_global::CapturePayloadInputSnapshot(generated.payload);
        Require(current.captured && current.root == generated.after.root &&
            current.targetRelative == generated.after.targetRelative && current.desktopRelative == generated.after.desktopRelative &&
            current.sourceMappingRelative == generated.after.sourceMappingRelative && current.targetMappingRelative == generated.after.targetMappingRelative &&
            SamePayloadEndpointIndices(current, generated.after) && current.memberEntryIndices == generated.after.memberEntryIndices &&
            current.entries.size() == generated.after.entries.size(), "native input graph is unchanged before dispatch");
        for (std::size_t i = 0; i < current.entries.size(); ++i)
        {
            const auto& item = current.entries[i]; const auto& before = generated.after.entries[i];
            Require(item.relative == before.relative && item.kind == before.kind && item.bytes == before.bytes &&
                item.volumeSerial == before.volumeSerial && item.fileIndexHigh == before.fileIndexHigh &&
                item.fileIndexLow == before.fileIndexLow, "native input identity and bytes match the pre-dispatch baseline");
        }
        const auto& source = Find(*profile.app, original);
        Require(source.gridSpan.columns == 1 && source.gridSpan.rows == 1 && source.gridCell.pageId == L"__page:1" &&
            (source.gridCell.column != 1 || source.gridCell.row != 1),
            "generated native move requires one non-target desktop cell");
        originalCell = source.gridCell;
        originalBounds = source.bounds;
        const POINT startClient{(source.bounds.left + source.bounds.right) / 2,
            (source.bounds.top + source.bounds.bottom) / 2};
        const auto start = Access::ExternalPoint(startClient);
        const auto target = Access::ExternalPoint({150, 150});
        startScreen = {start.x, start.y}; targetScreen = {target.x, target.y};
        std::size_t sourceCount = 0;
        for (const auto& item : Access::Items(*profile.app))
        {
            const fs::path path(item.parsingName);
            std::error_code ec;
            const bool isSource = fs::equivalent(path, original, ec) && !ec;
            if (isSource) ++sourceCount;
            else Require(item.gridCell.pageId != L"__page:1" || item.gridCell.column > 1 || item.gridCell.row > 1 ||
                item.gridCell.column + item.gridSpan.columns <= 1 || item.gridCell.row + item.gridSpan.rows <= 1,
                "generated native move target cell is initially empty");
            expectedCells.emplace_back(path, isSource ? GridCell{L"__page:1", 1, 1} : item.gridCell);
        }
        Require(sourceCount == 1, "generated native source has exactly one desktop model owner");
        initialLayoutRevision = Access::LayoutRevision(*profile.app);
        traceStart = Access::messageTrace->Observations().size();
    }

    void VerifyImmediatelyBeforeDispatch() const
    {
        auto& profile = *generated.profile;
        Require(Access::FileDropIdle(*profile.app) && Access::LayoutSaveIdle(*profile.app) &&
            Access::NativeDragIdle(*profile.app), "native host remains idle immediately before input");
        const auto current = drag_global::CapturePayloadInputSnapshot(generated.payload);
        Require(current.captured && current.root == generated.after.root &&
            current.targetRelative == generated.after.targetRelative && current.desktopRelative == generated.after.desktopRelative &&
            current.sourceMappingRelative == generated.after.sourceMappingRelative && current.targetMappingRelative == generated.after.targetMappingRelative &&
            SamePayloadEndpointIndices(current, generated.after) && current.memberEntryIndices == generated.after.memberEntryIndices &&
            current.entries.size() == generated.after.entries.size(), "native input graph still matches its baseline");
        for (std::size_t i = 0; i < current.entries.size(); ++i)
        {
            const auto& item = current.entries[i]; const auto& before = generated.after.entries[i];
            Require(item.relative == before.relative && item.kind == before.kind && item.bytes == before.bytes &&
                item.volumeSerial == before.volumeSerial && item.fileIndexHigh == before.fileIndexHigh &&
                item.fileIndexLow == before.fileIndexLow, "native input identity and bytes remain stable before input");
        }
        const auto& source = Find(*profile.app, original);
        Require(Same(source.gridCell, originalCell) && EqualRect(&source.bounds, &originalBounds),
            "native source identity and bounds still match the prepared press point");
        const auto start = Access::ExternalPoint({(source.bounds.left + source.bounds.right) / 2,
            (source.bounds.top + source.bounds.bottom) / 2});
        const auto target = Access::ExternalPoint({150, 150});
        Require(start.x == startScreen.x && start.y == startScreen.y && target.x == targetScreen.x && target.y == targetScreen.y,
            "native screen mapping remains stable before input");
        Require(Access::Items(*profile.app).size() == expectedCells.size(), "native initial model member count is unchanged");
        for (const auto& [path, target] : expectedCells)
        {
            std::error_code ec;
            const bool isSource = fs::equivalent(path, original, ec) && !ec;
            Require(Same(Find(*profile.app, path).gridCell, isSource ? originalCell : target),
                "native initial model positions remain unchanged before input");
        }
    }

    // Device state samples precede or may be overwritten during delivery.
    // A separate queued token and value log correlate one actual permit and
    // WndProc return against requested input, but do not establish complete
    // graph semantics or business outcome.
    void ObserveDeviceState(drag_global::ole_driver::NativeDevicePhase phase, std::optional<std::size_t> index,
        const snowdesktop::private_pointer::State& state,
        const snowdesktop::private_pointer::DeliveryObservationLog& deliveryLog,
        std::optional<snowdesktop::private_pointer::QueuedEventIdentity> queued,
        std::optional<drag_global::ole_driver::NativeInputExpectation> expectedInput)
    {
        using namespace drag_global::ole_driver;
        using namespace snowdesktop::private_pointer;
        if (deliveryLog.failed || !deliveryLog.session || deliveryLog.ownerThread != GetCurrentThreadId())
            throw ObservationUnavailable("native delivery log unavailable");
        if (phase == NativeDevicePhase::session_end)
        {
            if (queued || expectedInput) throw ObservationUnavailable("native end must not reuse an input event identity or input expectation");
        }
        else
        {
            if (!queued || !expectedInput || queued->ownerThread != deliveryLog.ownerThread || queued->session != deliveryLog.session ||
                !queued->event || !Access::messageTrace)
                throw ObservationUnavailable("native queued event identity unavailable");
            const DeliveryObservation* primary = nullptr;
            for (const auto& event : deliveryLog.entries)
            {
                if (event.kind != DeliveryObservationKind::begin || !event.identity.queuedEvent || event.identity.forwarded ||
                    event.identity.ownerThread != queued->ownerThread || event.identity.session != queued->session ||
                    event.identity.event != queued->event) continue;
                if (primary) throw ObservationUnavailable("native queued event has ambiguous primary deliveries");
                primary = &event;
            }
            if (!primary || primary->window != Access::completionWindow)
                throw ObservationUnavailable("native queued event primary delivery unavailable");
            POINT client = expectedInput->screen;
            if (!ScreenToClient(Access::completionWindow, &client))
                throw ObservationUnavailable("native expected screen-to-client mapping unavailable");
            POINT roundTrip = client;
            if (!ClientToScreen(Access::completionWindow, &roundTrip) ||
                roundTrip.x != expectedInput->screen.x || roundTrip.y != expectedInput->screen.y ||
                client.x != expectedInput->clientBeforeDispatch.x || client.y != expectedInput->clientBeforeDispatch.y ||
                client.x < (std::numeric_limits<short>::min)() || client.x > (std::numeric_limits<short>::max)() ||
                client.y < (std::numeric_limits<short>::min)() || client.y > (std::numeric_limits<short>::max)())
                throw ObservationUnavailable("native expected coordinate mapping changed or is not representable and reversible");
            // Matching before/after mappings does not rule out transient movement
            // during synchronous delivery; the geometry gate stays unresolved.
            const LPARAM expectedPoint = MAKELPARAM(static_cast<short>(expectedInput->clientBeforeDispatch.x),
                static_cast<short>(expectedInput->clientBeforeDispatch.y));
            if (primary->message != expectedInput->message || primary->wParam != expectedInput->buttons ||
                primary->lParam != expectedPoint)
                throw ObservationUnavailable("native requested input and delivered message differ; dispatch geometry is not yet fully fenced");
            const auto evidence = InspectNativeDelivery(*Access::messageTrace, traceStart, &deliveryLog,
                primary->identity, primary->window, MessageRoute::desktop_window_proc, expectedInput->message,
                expectedInput->buttons, expectedPoint);
            if (!evidence.matchedMessageAndPermit)
                throw ObservationUnavailable("native queued event cannot be uniquely linked to the production window return");
            // This restricted message match does not establish complete graph
            // semantics or the final business outcome; those gates remain.
        }
        // A State sample's sequence may be overwritten by nested processing.
        // No graph role is inferred from it or from this log without a binding.

        Require(state.pendingEvents == 0 && state.error.empty() &&
            std::none_of(state.keys.begin(), state.keys.end(), [](bool pressed) { return pressed; }),
            "generated native input has no pending events or pressed keyboard modifiers");
        if (phase == NativeDevicePhase::session_end)
        {
            Require(index && *index < program.steps.size() && program.steps[*index].kind == "end_pointer_session" &&
                initialDeviceHover && !deviceEnded && !state.enabled && state.buttons == 0 &&
                state.sequence == deviceSequence, "the same private pointer session ended after its last input");
            deviceEnded = true;
            deviceCaptured = false;
            return;
        }
        Require(state.enabled && !deviceEnded && state.sequence >= deviceSequence && !state.trace.empty() &&
            (state.sequence > deviceSequence || phase == NativeDevicePhase::program_input),
            "generated native input comes from the active private pointer session in sequence");
        const auto& sample = state.trace.back();
        Require(sample.sequence == state.sequence && sample.window == Access::completionWindow &&
            sample.point.x == state.point.x && sample.point.y == state.point.y,
            "private pointer trace records this session's actual host and coordinates");
        if (phase == NativeDevicePhase::initial_hover)
        {
            Require(!index && !initialDeviceHover && state.buttons == 0 && sample.message == WM_MOUSEMOVE &&
                state.point.x == startScreen.x && state.point.y == startScreen.y,
                "private pointer initially hovers the generated source without buttons");
            initialDeviceHover = true;
        }
        else
        {
            Require(index && *index < program.steps.size() && initialDeviceHover && !observedProgramInputs[*index],
                "native device input belongs to one not-yet-observed modeled program step");
            observedProgramInputs[*index] = true;
            const auto& kind = program.steps[*index].kind;
            if (kind != "source_start" && kind != "release")
                Require(moveClasses[*index].has_value(), "modeled movement has a selected graph class");
            // No identity binding is inferred from state.sequence: nested
            // processing may have overwritten it before the sample was made.
            if (kind == "source_start")
                Require(sample.message == WM_LBUTTONDOWN && state.buttons == 1 &&
                    state.captured == Access::completionWindow && state.point.x == startScreen.x && state.point.y == startScreen.y,
                    "private pointer presses the generated source");
            else if (kind == "release")
                Require(sample.message == WM_LBUTTONUP && state.buttons == 0 && !state.captured &&
                    state.point.x == targetScreen.x && state.point.y == targetScreen.y,
                    "private pointer releases at the generated target");
            else Require(sample.message == WM_MOUSEMOVE && state.buttons == 1 && state.captured == Access::completionWindow,
                "private pointer moves with left button captured by the production host");
        }
        deviceCaptured = state.captured == Access::completionWindow;
        deviceSequence = state.sequence;
    }
    bool ProductionInputEntered() const
    {
        Require(Access::messageTrace && !Access::messageTrace->LostOrCrossThread(),
            "native production message trace remains complete on the owning thread");
        const auto& events = Access::messageTrace->Observations();
        Require(traceStart <= events.size(), "native trace baseline remains valid");
        return std::any_of(events.begin() + static_cast<std::ptrdiff_t>(traceStart), events.end(), [&](const auto& event) {
            return event.window == Access::completionWindow && event.route == drag_global::ole_driver::MessageRoute::desktop_window_proc &&
                event.enteredSequence && event.enteredThread == GetCurrentThreadId() && event.inputLParam && (event.message == WM_MOUSEMOVE || event.message == WM_LBUTTONDOWN || event.message == WM_LBUTTONUP);
        });
    }
    void Capture()
    {
        Require(!captureSequence && deviceCaptured && ProductionInputEntered() &&
            Access::NativeDragCaptured(*generated.profile->app, original),
            "production host captures the selected generated desktop file");
        const auto captured = Access::CaptureNativeDrag(*generated.profile->app);
        Require(captured.active && captured.sourceIsDesktop && captured.entries.size() == 1 &&
            captured.entries.front().kind == DropSourceKind::DesktopIcon &&
            captured.entries.front().desktopKey == Find(*generated.profile->app, original).layoutKey,
            "production source list retains the generated desktop item's captured identity");
        const auto& events = Access::messageTrace->Observations();
        for (std::size_t i = traceStart; i < events.size(); ++i)
            if (events[i].window == Access::completionWindow &&
                events[i].route == drag_global::ole_driver::MessageRoute::desktop_window_proc && events[i].returned &&
                events[i].enteredThread == GetCurrentThreadId() && events[i].returnedThread == events[i].enteredThread &&
                events[i].message == WM_MOUSEMOVE && events[i].wParam == MK_LBUTTON)
                captureSequence = (std::max)(captureSequence, events[i].returnedSequence);
        Require(captureSequence != 0, "native capture is observed after a real pressed-pointer move");
    }
    void BeforeEnd(const snowdesktop::private_pointer::State& state)
    {
        Require(Access::messageTrace && !Access::messageTrace->LostOrCrossThread() &&
            traceBeforeEnd == (std::numeric_limits<std::size_t>::max)() && initialDeviceHover && !deviceEnded &&
            state.enabled && !state.closing && state.pendingEvents == 0 && state.buttons == 0 &&
            state.sequence == deviceSequence && state.hovered == Access::completionWindow &&
            !state.captured && state.point.x == targetScreen.x && state.point.y == targetScreen.y &&
            std::none_of(state.keys.begin(), state.keys.end(), [](bool pressed) { return pressed; }),
            "record the same released pointer session and complete pre-end trace");
        traceBeforeEnd = Access::messageTrace->Observations().size();
    }
    void Await() const
    {
        auto& profile = *generated.profile;
        Wait([&] { return Access::NativeDragIdle(*profile.app) && Access::FileDropIdle(*profile.app) &&
            Access::LayoutSaveIdle(*profile.app); });
    }
    void Ended() const
    {
        Require(deviceEnded && !snowdesktop::private_pointer::Active() && Access::NativeDragIdle(*generated.profile->app) &&
            ProductionInputEntered(), "private pointer end and production drag completion have both finished with a complete trace");
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& kind = program.steps[i].kind;
            if (kind == "source_start" || kind == "release" || kind == "move_past_drag_threshold" ||
                kind == "pointer_direction" || kind == "hover" || kind == "pointer_direction_later" || kind == "hover_later")
                Require(observedProgramInputs[i], "every modeled native pointer input has a device observation");
        }
        const auto& events = Access::messageTrace->Observations();
        POINT startClient = startScreen, targetClient = targetScreen;
        Require(ScreenToClient(Access::completionWindow, &startClient) && ScreenToClient(Access::completionWindow, &targetClient),
            "native pointer message coordinates map through the owned window");
        const auto startParam = MAKELPARAM(startClient.x, startClient.y);
        const auto targetParam = MAKELPARAM(targetClient.x, targetClient.y);
        std::uint64_t initialHover = 0, down = 0, move = 0, up = 0;
        for (std::size_t i = traceStart; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.window != Access::completionWindow || event.route != drag_global::ole_driver::MessageRoute::desktop_window_proc ||
                !event.returned || !event.inputLParam || event.enteredThread != GetCurrentThreadId() ||
                event.returnedThread != event.enteredThread) continue;
            if (!initialHover && !down && event.message == WM_MOUSEMOVE && *event.inputLParam == startParam && event.wParam == 0)
                initialHover = event.enteredSequence;
            else if (initialHover && !down && event.message == WM_LBUTTONDOWN && *event.inputLParam == startParam && event.wParam == MK_LBUTTON)
                down = event.enteredSequence;
            else if (down && !move && event.message == WM_MOUSEMOVE && *event.inputLParam == targetParam && event.wParam == MK_LBUTTON)
                move = event.enteredSequence;
            else if (move && !up && event.message == WM_LBUTTONUP && *event.inputLParam == targetParam && event.wParam == 0)
                up = event.enteredSequence;
        }
        Require(initialHover && down && move && up && captureSequence && initialHover < down &&
            down < captureSequence && captureSequence < up && down < move && move < up,
            "real desktop WndProc receives initial hover, ordered input and the matching capture");
        Require(traceBeforeEnd <= events.size(), "pointer-end trace baseline is available");
        std::uint64_t cancel = 0, leave = 0;
        for (std::size_t i = traceBeforeEnd; i < events.size(); ++i)
        {
            const auto& event = events[i];
            if (event.window != Access::completionWindow || event.route != drag_global::ole_driver::MessageRoute::desktop_window_proc ||
                !event.returned || event.enteredThread != GetCurrentThreadId() || event.returnedThread != event.enteredThread) continue;
            if (!cancel && event.message == WM_CANCELMODE) cancel = event.enteredSequence;
            else if (cancel && !leave && event.message == WM_MOUSELEAVE) leave = event.enteredSequence;
        }
        Require(cancel && leave && up < cancel && cancel < leave,
            "private pointer end delivers cancel and leave to the same production host after release");
    }
    void AssertResults() const
    {
        auto& profile = *generated.profile;
        Await();
        Require(Access::LayoutRevision(*profile.app) > initialLayoutRevision,
            "generated native move schedules or performs production layout persistence");
        Require(Access::Items(*profile.app).size() == expectedCells.size(),
            "generated native move preserves all desktop model members");
        for (const auto& [path, cell] : expectedCells)
            Require(Same(Find(*profile.app, path).gridCell, cell), "generated native move commits only the requested landing");
        Reload(profile, expectedCells, false);
        Await();
        Require(Access::Items(*profile.app).size() == expectedCells.size(), "native reload preserves all desktop members");
        for (const auto& [path, cell] : expectedCells)
            Require(Same(Find(*profile.app, path).gridCell, cell), "native reload preserves each committed landing");
        const auto actual = drag_global::CapturePayloadInputSnapshot(generated.payload);
        Require(actual.captured && actual.root == generated.after.root && actual.targetRelative == generated.after.targetRelative &&
            actual.desktopRelative == generated.after.desktopRelative && actual.sourceMappingRelative == generated.after.sourceMappingRelative &&
            actual.targetMappingRelative == generated.after.targetMappingRelative && SamePayloadEndpointIndices(actual, generated.after) &&
            actual.entries.size() == generated.after.entries.size() && actual.memberEntryIndices == generated.after.memberEntryIndices,
            "native move preserves the recorded physical file graph");
        for (std::size_t i = 0; i < actual.entries.size(); ++i)
        {
            const auto& item = actual.entries[i]; const auto& before = generated.after.entries[i];
            Require(item.relative == before.relative && item.kind == before.kind && item.bytes == before.bytes &&
                item.volumeSerial == before.volumeSerial && item.fileIndexHigh == before.fileIndexHigh &&
                item.fileIndexLow == before.fileIndexLow, "native move preserves physical input identities and bytes");
        }
    }
};
void ProductionMessageWiring(bool control)
{
    Profile p;
    Access::EnableProductionDispatch(*p.app, control);
    // Observe a real background read through the selected production WndProc.
    Access::RequestAsync(*p.app);
    Wait([&] { return Access::asyncReadMessages > 0 && Access::FileDropIdle(*p.app); });
    const auto sourcePath = p.root / L"incoming/Dispatch.txt";
    Write(sourcePath, "dispatch-payload");
    auto source = ole_file_source_fixture::Make({sourcePath}, true);
    const auto traceBefore = Access::messageTrace->Observations().size();
    Submit(p, *source.Get(), MK_CONTROL, DROPEFFECT_COPY | DROPEFFECT_MOVE,
        DROPEFFECT_COPY, {150,150});
    const auto output = p.desktop / L"Dispatch.txt";
    Wait([&] { return source->ends == 1 && Access::FileDropIdle(*p.app) &&
        Access::sourceCompletionMessages > 0 && Access::commitCompletionMessages > 0 && Access::Items(*p.app).size() == 2; });
    RequireCompletionTrace(traceBefore, Access::completionWindow,
        control ? drag_global::ole_driver::MessageRoute::control_window_proc :
                  drag_global::ole_driver::MessageRoute::desktop_window_proc);
    Require(source->starts == 1 && SUCCEEDED(source->outcome) && source->endEffect == DROPEFFECT_COPY &&
        Read(sourcePath) == "dispatch-payload" && Read(output) == "dispatch-payload",
        "production message routing completes the matching source and physical copy");
    const auto cell = Find(*p.app, output).gridCell;
    Require(cell.column == 1 && cell.row == 1, "production completion retains requested landing");
    Reload(p, {{output, cell}});
}
void FileCase(const drag_matrix::Case& test, bool deferredDefault = false, bool corruptExternalSource = false)
{
    Profile p;
    Access::EnableProductionDispatch(*p.app, false);
    Access::EnableFixtureControlWindow(*p.app);
    const bool delayed = test.asynchronous;
    const bool cancel = test.cancel;
    const bool link = test.release.effect == DROPEFFECT_LINK;
    const bool move = test.release.effect == DROPEFFECT_MOVE;
    const bool batch = test.directory;
    const DWORD keys = test.release.keys;
    const DWORD allowed = test.allowed;
    const DWORD expected = cancel ? DROPEFFECT_NONE : test.release.effect & allowed;
    const auto file = p.root / L"source/Payload.txt";
    Write(file, corruptExternalSource ? "wrong-payload" : "payload-bytes");
    std::vector<fs::path> files{file};
    if (batch) { files.push_back(p.root / L"source/Folder"); Write(files.back() / L"nested.txt", "nested-bytes"); }
    Wait([&] { return Access::FileDropIdle(*p.app) && Access::LayoutSaveIdle(*p.app); });
    const auto before = Read(Access::LayoutPath(*p.app));
    auto source = ole_file_source_fixture::Make(files, delayed);
    if (corruptExternalSource)
    {
        Require(delayed && !batch && test.release.effect == DROPEFFECT_COPY,
            "byte-corruption control requires one delayed copy source");
        // The provider's real file contains incorrect bytes before the drag;
        // production still reads, routes, copies and reports completion.
    }
    const auto traceBefore = Access::messageTrace->Observations().size();
    const auto sourceMessagesBefore = Access::sourceCompletionMessages;
    const auto commitMessagesBefore = Access::commitCompletionMessages;
    const auto layoutRevisionBefore = Access::LayoutRevision(*p.app);
    Submit(p, *source.Get(), keys, allowed, expected, {150,150}, cancel,
        MK_LBUTTON | (test.enterShift ? MK_SHIFT : 0), deferredDefault, Access::Desktop(*p.app));
    if (expected) Wait([&] { return Access::FileDropIdle(*p.app) && (!delayed || source->ends == 1) && Access::Items(*p.app).size() == 1 + files.size(); });
    else Wait([&] { return Access::FileDropIdle(*p.app) && Access::LayoutSaveIdle(*p.app); });
    const auto outputs = Outputs(p);
    Require(outputs.size() == (expected ? files.size() : 0), "exact physical output count");
    Require(!source->wrongApartment && !source->fileReadBeforeStart && !source->formatProbeObservationFailed,
        "provider apartment, lease ordering and format observation");
    if (!expected)
    {
        Require(source->starts == 0 && source->ends == 0 && Read(file) == "payload-bytes" &&
            (!batch || Read(files.back() / L"nested.txt") == "nested-bytes") &&
            Read(Access::LayoutPath(*p.app)) == before, "cancel/reject has no I/O or saved-state side effects");
        return;
    }
    if (delayed) Require(source->starts == 1 && source->ends == 1 && SUCCEEDED(source->outcome) &&
        source->endEffect == (move ? DROPEFFECT_NONE : expected), "one balanced source lease reports the performed effect");
    if (delayed) Require(Access::sourceCompletionMessages > sourceMessagesBefore &&
        Access::commitCompletionMessages > commitMessagesBefore,
        "asynchronous source and file commit completions reach a production window procedure");
    if (delayed) RequireCompletionTrace(traceBefore, Access::fixtureControlWindow,
        drag_global::ole_driver::MessageRoute::control_window_proc);
    RequireObservedFilePayload(*source.Get(), files);
    std::vector<std::pair<fs::path, GridCell>> cells;
    for (const auto& original : files)
    {
        Require(fs::exists(original) == !move, "move removes source; copy/link retain it");
        if (!move) Require(Read(original == file ? original : original / L"nested.txt") ==
            (original == file ? (corruptExternalSource ? "wrong-payload" : "payload-bytes") : "nested-bytes"),
            "copy/link retain original source bytes");
        const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const auto& out) {
            return link ? fs::equivalent(LinkTarget(out), original) : out.filename() == original.filename();
        });
        Require(it != outputs.end(), "output identity belongs to its selected source");
        if (!link) Require(Read(original == file ? *it : *it / L"nested.txt") ==
            (original == file ? "payload-bytes" : "nested-bytes"), "output bytes survive actual I/O");
        cells.emplace_back(*it, Find(*p.app, *it).gridCell);
    }
    Require(cells.front().second.pageId == L"__page:1" && cells.front().second.column == 1 && cells.front().second.row == 1,
        "first output occupies the requested empty cell");
    if (batch) Require(!Same(cells[0].second, cells[1].second), "batch outputs cannot overlap");
    Wait([&] { return Access::LayoutSaveIdle(*p.app); });
    Require(Access::LayoutRevision(*p.app) > layoutRevisionBefore,
        "production drop and refresh must schedule or perform the layout save");
    Reload(p, cells, false);
}
void CorruptCopyControl()
{
    const drag_matrix::Case test{true, false, {"copy", MK_CONTROL, DROPEFFECT_COPY},
        DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, true, false};
    try { FileCase(test, false, true); }
    catch (const std::exception& error)
    {
        std::cerr << "CONTROL_OBSERVED " << error.what() << std::endl;
        Require(std::string_view(error.what()).find("output bytes survive actual I/O") != std::string_view::npos,
            "corrupt source must fail at the output-byte business assertion");
        std::cout << "EXPECTED FAILURE output bytes survive actual I/O" << std::endl;
        return;
    }
    Require(false, "corrupt source unexpectedly passed the output-byte business assertion");
}
void Files(std::string_view row)
{
    Require(row == "sync-copy" || row == "copy" || row == "move" || row == "default-move" || row == "link" || row == "batch" || row == "unsupported" || row == "cancel", "unknown file matrix row");
    const bool move = row == "move" || row == "default-move";
    const bool link = row == "link";
    drag_matrix::Case test{row != "sync-copy", row == "batch",
        {link ? "link" : move ? "move" : "copy", row == "default-move" || row == "cancel" ? 0u : link ? MK_ALT : move ? MK_SHIFT : MK_CONTROL,
         static_cast<DWORD>(link ? DROPEFFECT_LINK : move ? DROPEFFECT_MOVE : DROPEFFECT_COPY)},
        static_cast<DWORD>(row == "unsupported" ? DROPEFFECT_MOVE : DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK),
        true, row == "cancel"};
    FileCase(test, row == "default-move");
}
struct HeldRead
{
    MSG held{};
    ~HeldRead() { if (held.hwnd) DispatchMessageW(&held); }
    void Capture()
    {
        const auto deadline = GetTickCount64() + 12000;
        while (!held.hwnd)
        {
            Require(GetTickCount64() < deadline, "read completion gate deadline");
            MsgWaitForMultipleObjectsEx(0, nullptr, 10, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG next{};
            while (PeekMessageW(&next, nullptr, 0, 0, PM_REMOVE))
            {
                if (next.hwnd == Access::completionWindow && next.message == kShellFileOperationCompletedMessage)
                {
                    const auto* result = reinterpret_cast<const snowdesktop::ShellFileOperationUiCompletion*>(next.lParam);
                    Require(result && !result->fileOperation && result->succeeded, "gate the real source read before file submission");
                    held = next; break;
                }
                DispatchMessageW(&next); Access::ThrowAsyncException();
            }
        }
    }
    void Deliver() { const auto next = held; held = {}; DispatchMessageW(&next); Access::ThrowAsyncException(); }
};
void Interleaved(bool failFirst)
{
    Profile p;
    const auto a = p.root / L"a/Same.txt", b = p.root / L"b/Same.txt";
    Write(a, "first"); Write(b, "second");
    auto first = ole_file_source_fixture::Make({a}, true), second = ole_file_source_fixture::Make({b}, true);
    Submit(p, *first.Get(), MK_CONTROL, 7, DROPEFFECT_COPY, {150,150});
    HeldRead gate; gate.Capture();
    Require(Outputs(p).empty() && first->ends == 0, "first request remains pending without committing");
    if (failFirst) Require(fs::remove(a), "remove only owned source to trigger actual commit failure");
    Submit(p, *second.Get(), MK_CONTROL, 7, DROPEFFECT_COPY, {350,250});
    Wait([&] { return second->ends == 1 && Access::FileDropIdle(*p.app) && Access::Items(*p.app).size() == 2; });
    auto outputs = Outputs(p);
    Require(outputs.size() == 1 && Read(outputs[0]) == "second" && first->ends == 0, "second completes independently before first");
    const auto secondPath = outputs[0]; const auto secondCell = Find(*p.app, secondPath).gridCell;
    Require(secondCell.column == 3 && secondCell.row == 2, "second request retains its own landing");
    Access::RequestAsync(*p.app);
    Wait([&] { return Access::FileDropIdle(*p.app); });
    gate.Deliver();
    Wait([&] { return first->ends == 1 && Access::FileDropIdle(*p.app) && Access::Items(*p.app).size() == (failFirst ? 2u : 3u); });
    Require(first->starts == 1 && first->ends == 1 && second->ends == 1 &&
        (failFirst ? FAILED(first->outcome) : SUCCEEDED(first->outcome)), "each request reports its own outcome once");
    outputs = Outputs(p);
    Require(outputs.size() == (failFirst ? 1u : 2u) && Read(secondPath) == "second" &&
        Same(Find(*p.app, secondPath).gridCell, secondCell), "late success/failure cannot overwrite or relocate second");
    std::vector<std::pair<fs::path, GridCell>> cells{{secondPath, secondCell}};
    if (!failFirst)
    {
        const auto it = std::find_if(outputs.begin(), outputs.end(), [&](const auto& out) { return Read(out) == "first"; });
        Require(it != outputs.end() && *it != secondPath, "same-name requests preserve distinct bytes and identities");
        const auto cell = Find(*p.app, *it).gridCell;
        Require(cell.column == 1 && cell.row == 1, "late first completion retains original landing");
        cells.emplace_back(*it, cell);
    }
    Reload(p, cells);
}
}
std::shared_ptr<drag_global::V4CategoryObservationSource>
NativeDragIntegrationAccess::CreateV4CategoryObservationSource(
    std::shared_ptr<behavior::OwnedProfileDesktopRoot> root,
    std::shared_ptr<drag_global::V4IndependentFilePayload> payload,
    std::shared_ptr<const drag_global::event_graph::V4FamilyIdentityContext> identity,
    std::shared_ptr<const drag_global::V4FamilyValidatedContext> verified,
    std::shared_ptr<drag_global::ole_driver::NativeHostEpoch> epoch)
{
    using namespace drag_global;
    using Source = V4CategoryObservationSource;
    try
    {
        if (!root || !payload || !identity || !verified || !epoch || !root->StillOwned() || !root->profile ||
            !root->profile->app || !SameNativeHostEpoch(*root->profile->app, epoch) || verified->TaskBActive() ||
            identity->Status() != V4FamilyStatus::schema_only || verified->Status() != V4CoordinateStatus::coordinate_ready ||
            identity->FamilyBinding() != verified->FamilyBinding() || identity->Axes().size() != verified->Axes().size() ||
            payload->status != V4IndependentFileStatus::input_prepared ||
            !behavior::OwnedProfileDesktopRoot::SameId(payload->rootId, root->desktopId)) return {};
        for (std::size_t i = 0; i < identity->Axes().size(); ++i)
        {
            const auto& a = identity->Axes()[i]; const auto& b = verified->Axes()[i];
            if (a.id != b.id || a.role != b.role || a.label != b.label || a.values != b.values) return {};
        }
        const auto candidate = identity->RawCandidateIdentity(verified->Coordinate());
        if (candidate.resourceUnavailable || !candidate.errors.empty() || candidate.id.empty() ||
            candidate.id != payload->candidateId) return {};
        Source::Reader reader = [root, payload, identity, verified, epoch, candidateId = candidate.id](
            std::size_t maxRules, std::size_t maxMembers, std::size_t maxUtf16Bytes,
            CategorySettings& settings, Source::Targets& targets, bool& resourceUnavailable) {
            resourceUnavailable = false;
            try
            {
                if (!root->StillOwned() || !root->profile || !root->profile->app ||
                    !SameNativeHostEpoch(*root->profile->app, epoch) || productionDispatchDepth ||
                    payload->status != V4IndependentFileStatus::input_prepared || payload->candidateId != candidateId ||
                    !behavior::OwnedProfileDesktopRoot::SameId(payload->rootId, root->desktopId)) return false;
                auto& app = *root->profile->app;
                const auto raw = identity->RawCandidateIdentity(verified->Coordinate());
                if (raw.resourceUnavailable) { resourceUnavailable = true; return false; }
                if (!raw.errors.empty() || raw.id != candidateId) return false;
                const auto get = [&](std::string_view id) { return Value(verified->Axes(), verified->Coordinate(), id); };
                const auto countValue = get("P02.source_actual_count");
                const std::string decimal = countValue == "zero" ? "0" : countValue == "one" ? "1" :
                    countValue == "two" ? "2" : countValue.starts_with("count_") ? std::string(countValue.substr(6)) : std::string();
                if (decimal.empty()) return false;
                const auto count = V4MaterializeCount({V3CountKind::exact, decimal}, maxMembers, false);
                if (count.status == V4AxisStatus::resource_unavailable) { resourceUnavailable = true; return false; }
                if (payload->files.size() > maxMembers) { resourceUnavailable = true; return false; }
                if (count.status != V4AxisStatus::schema_only) return false;
                const auto& actualSettings = app.GetCategorySettings();
                if (actualSettings.rules.size() > maxRules || app.items_.size() > maxMembers)
                { resourceUnavailable = true; return false; }
                std::size_t remaining = maxUtf16Bytes;
                const auto charge = [&](const std::wstring& text) {
                    if (text.size() > remaining / sizeof(wchar_t)) return false;
                    remaining -= text.size() * sizeof(wchar_t); return true;
                };
                for (const auto& rule : actualSettings.rules)
                    if (!charge(rule.id) || !charge(rule.customLabel) || !charge(rule.extensions))
                    { resourceUnavailable = true; return false; }
                const auto layoutRevision = app.layoutSaveRevision_;
                const auto selectionRevision = app.selectionController_.Revision();
                std::vector<const DesktopItem*> matched(count.value, nullptr);
                std::vector<std::filesystem::path> matchedPaths(count.value);
                std::vector<bool> mapped(count.value, false);
                std::vector<HANDLE> pins;
                struct Pins { std::vector<HANDLE>& values; ~Pins() { for (auto value : values) CloseHandle(value); } } pinGuard{pins};
                pins.reserve(payload->files.size());
                const auto* itemStorage = app.items_.data(); const auto itemCount = app.items_.size();
                constexpr char digits[] = "0123456789abcdef";
                for (std::size_t i = 0; i < payload->files.size(); ++i)
                {
                    const auto& file = payload->files[i];
                    const auto relation = get("V4.file.member" + std::to_string(i) + "_logical_relation");
                    if (!relation.starts_with("member_") || !file.created || !file.identityObserved ||
                        !file.handle || file.handle == INVALID_HANDLE_VALUE) return false;
                    HANDLE parent = CreateFileW(file.path.parent_path().c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    if (parent == INVALID_HANDLE_VALUE) return false;
                    struct ParentPin { HANDLE value; ~ParentPin() { CloseHandle(value); } } parentGuard{parent};
                    FILE_ID_INFO parentId{};
                    if (!behavior::OwnedProfileDesktopRoot::ReadDirectoryId(parent, parentId) ||
                        !behavior::OwnedProfileDesktopRoot::SameId(parentId, root->desktopId)) return false;
                    const auto logical = V4MaterializeCount({V3CountKind::exact, std::string(relation.substr(7))}, maxMembers, false);
                    if (logical.status == V4AxisStatus::resource_unavailable) { resourceUnavailable = true; return false; }
                    if (logical.status != V4AxisStatus::schema_only || logical.value >= count.value || mapped[logical.value]) return false;
                    std::string pathValue = "utf16le_hex_";
                    for (wchar_t unit : file.path.native())
                    {
                        const auto value = static_cast<std::uint16_t>(unit);
                        for (unsigned shift : {0u, 8u}) { const auto byte = (value >> shift) & 255u;
                            pathValue.push_back(digits[byte >> 4]); pathValue.push_back(digits[byte & 15]); }
                    }
                    if (get("V4.file_path.index_" + std::to_string(i) + ".utf16le") != pathValue) return false;
                    HANDLE pin = CreateFileW(file.path.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    if (pin == INVALID_HANDLE_VALUE) return false;
                    pins.push_back(pin);
                    FILE_ID_INFO actual{}, held{}; FILE_ATTRIBUTE_TAG_INFO tag{};
                    if (!GetFileInformationByHandleEx(pin, FileIdInfo, &actual, sizeof(actual)) ||
                        !GetFileInformationByHandleEx(file.handle, FileIdInfo, &held, sizeof(held)) ||
                        !GetFileInformationByHandleEx(pin, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                        (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                        std::none_of(std::begin(actual.FileId.Identifier), std::end(actual.FileId.Identifier), [](BYTE value) { return value != 0; }) ||
                        !behavior::OwnedProfileDesktopRoot::SameId(actual, file.identity) ||
                        !behavior::OwnedProfileDesktopRoot::SameId(held, file.identity)) return false;
                    for (const auto& item : app.items_)
                        if (std::filesystem::path(item.parsingName) == file.path)
                        {
                            if (matched[logical.value]) return false;
                            matched[logical.value] = &item;
                        }
                    matchedPaths[logical.value] = file.path;
                    mapped[logical.value] = true;
                    if (matched[logical.value] && !charge(matched[logical.value]->shortcutTarget.extension))
                    { resourceUnavailable = true; return false; }
                }
                CategorySettings copied = actualSettings;
                Source::Targets copiedTargets(count.value);
                for (std::size_t i = 0; i < matched.size(); ++i)
                    if (matched[i]) copiedTargets[i] = matched[i]->shortcutTarget;
                if (!root->StillOwned() || !SameNativeHostEpoch(app, epoch) || productionDispatchDepth ||
                    app.layoutSaveRevision_ != layoutRevision || app.selectionController_.Revision() != selectionRevision ||
                    app.items_.data() != itemStorage || app.items_.size() != itemCount ||
                    copied.rules.size() != app.GetCategorySettings().rules.size()) return false;
                for (std::size_t i = 0; i < copied.rules.size(); ++i)
                {
                    const auto& a = copied.rules[i]; const auto& b = app.GetCategorySettings().rules[i];
                    if (a.id != b.id || a.customLabel != b.customLabel || a.extensions != b.extensions || a.enabled != b.enabled) return false;
                }
                for (std::size_t i = 0; i < payload->files.size(); ++i)
                {
                    FILE_ID_INFO current{}, held{};
                    HANDLE named = CreateFileW(payload->files[i].path.c_str(), FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    if (named == INVALID_HANDLE_VALUE) return false;
                    struct NamedPin { HANDLE value; ~NamedPin() { CloseHandle(value); } } namedGuard{named};
                    FILE_ID_INFO namedId{}; FILE_ATTRIBUTE_TAG_INFO tag{};
                    if (!GetFileInformationByHandleEx(pins[i], FileIdInfo, &current, sizeof(current)) ||
                        !GetFileInformationByHandleEx(payload->files[i].handle, FileIdInfo, &held, sizeof(held)) ||
                        !GetFileInformationByHandleEx(named, FileIdInfo, &namedId, sizeof(namedId)) ||
                        !GetFileInformationByHandleEx(named, FileAttributeTagInfo, &tag, sizeof(tag)) ||
                        (tag.FileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) ||
                        !behavior::OwnedProfileDesktopRoot::SameId(current, payload->files[i].identity) ||
                        !behavior::OwnedProfileDesktopRoot::SameId(held, payload->files[i].identity) ||
                        !behavior::OwnedProfileDesktopRoot::SameId(namedId, payload->files[i].identity)) return false;
                }
                for (std::size_t i = 0; i < matched.size(); ++i)
                    if (matched[i])
                    {
                        if (std::filesystem::path(matched[i]->parsingName) != matchedPaths[i] ||
                            std::count_if(app.items_.begin(), app.items_.end(), [&](const auto& item) {
                                return std::filesystem::path(item.parsingName) == matchedPaths[i];
                            }) != 1) return false;
                        const auto& a = *copiedTargets[i]; const auto& b = matched[i]->shortcutTarget;
                        if (a.classified != b.classified || a.directory != b.directory || a.application != b.application ||
                            a.extension != b.extension) return false;
                    }
                settings = std::move(copied); targets = std::move(copiedTargets); return true;
            }
            catch (const std::bad_alloc&) { resourceUnavailable = true; return false; }
            catch (const std::length_error&) { resourceUnavailable = true; return false; }
            catch (...) { return false; }
        };
        return std::shared_ptr<Source>(new Source(root, identity->FamilyBinding(), candidate.id, std::move(reader)));
    }
    catch (...) { return {}; }
}
#include "container_scenarios.h"
#include "module_contracts.h"
#include "global_drag_event_graph.inc"
#include "global_drag_directory_business.inc"
#include "global_drag_collection_business.inc"
#include "global_drag_desktop_files_business.inc"
#include "global_drag_mapping_business.inc"
#include "global_drag_group_business.inc"
#include "global_drag_native_desktop_business.inc"
#include "global_drag_native_category_business.inc"
#include "global_drag_native_reorder_business.inc"
#include "global_drag_native_multiselect_business.inc"
#include "global_drag_virtual_geometry.inc"
#include "global_drag_leave_business.inc"
#include "global_drag_move_business.inc"
void behavior::GeneratedNativeMemberObservation::CaptureLayoutBeforeReload()
{
    RequireObservation(root.StillOwned() && root.profile && root.profile.get() == &profile && !businessEvidence.beforeReloadLayout,
        "native member pre-reload layout scope unavailable");
    auto snapshot = CaptureV4NativeDesktopLayout(root, 4u * 1024u * 1024u);
    businessEvidence.beforeReloadLayout = std::make_shared<const V4NativeDesktopLayoutSnapshot>(std::move(snapshot));
}
void behavior::V4VirtualCopyObservation::CaptureLayoutBeforeReload()
{
    RequireObservation(verified_ && familyIdentity_ && evidence.baselineRevalidated && !evidence.beforeReloadLayout,
        "v4 virtual pre-reload layout observation scope unavailable");
    constexpr std::size_t layoutObservationByteBudget = 4u * 1024u * 1024u;
    auto snapshot = CaptureV4VirtualLayoutDiskSnapshot(ownedRoot, *verified_, *familyIdentity_, layoutObservationByteBudget);
    if (!snapshot.captured || snapshot.resourceUnavailable || !snapshot.parsed)
        evidence.unresolved.emplace_back("v4_virtual_pre_reload_layout_snapshot_unavailable");
    evidence.beforeReloadLayout = std::make_shared<const V4VirtualLayoutDiskSnapshot>(std::move(snapshot));
}
#include "global_drag_adapter.inc"
#include "global_drag_v3_scheduler.h"
#include "global_drag_child_protocol.inc"

namespace drag_global
{
class ScheduledV3Executor final
{
public:
    static std::unique_ptr<v3_scheduler::ScheduledV3Outcome> Run(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t replayWorkBudget,
        std::size_t checkpointByteBudget)
    {
        if (!attempt.TryBeginExecution()) return {};
        const auto endExecution = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(endExecution)> executionGuard(&attempt, endExecution);
        bool receiptConsumed = false, bridgeEntered = false;
        try
        {
            auto result = RunBridge(preState, familyIndex, caseIndex, attempt, executionId,
                decimalDigitBudget, memberBudget, replayWorkBudget, checkpointByteBudget,
                receiptConsumed, bridgeEntered);
            // A failed or reentrant Consume must not seal another execution's
            // lineage; retain the published in-flight state as unavailable.
            if (!receiptConsumed) return {};
            v3_scheduler::OutcomeFacts facts;
            facts.executionId = std::string(executionId);
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            facts.failure = std::move(result.failure);
            facts.receiptConsumed = receiptConsumed; facts.bridgeEntered = bridgeEntered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.businessFailureObserved = result.businessFailureObserved;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.observationUnavailable = result.observationUnavailable;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            // This codec does not persist the detailed event/COM evidence.
            // A checkpoint produced from these facts cannot establish PASS.
            facts.obligations.emplace_back("v3_detailed_execution_evidence_persistence_unimplemented");
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV3Outcome>(
                new v3_scheduler::ScheduledV3Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; } // in-flight outcome remains unknown
    }
private:
    // Reconstruct the scheduler record before giving it to the real adapter.
    // Other product cases remain unimplemented; recovery never creates a new
    // capability for an old in-flight attempt.
    static ingress_adapter::BridgeResult RunBridge(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t replayWorkBudget,
        std::size_t checkpointByteBudget, bool& receiptConsumed, bool& bridgeEntered)
    {
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size())
            {
                result.observationUnavailable = true;
                result.obligations.emplace_back("v3_scheduled_family_index_unavailable");
                return result;
            }
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayCase(family, caseIndex, decimalDigitBudget, memberBudget, replayWorkBudget);
            if (!replay.unexecutedRecord)
            {
                result.observationUnavailable = true;
                result.obligations = replay.errors;
                result.obligations.emplace_back("v3_scheduled_case_replay_unavailable");
                return result;
            }
            const auto& record = *replay.unexecutedRecord;
            if (record.kind != "graph_hypothesis" || !record.coordinate || !record.graph)
            {
                result.obligations.insert(result.obligations.end(), record.unresolved.begin(), record.unresolved.end());
                result.obligations.emplace_back("v3_scheduled_case_graph_or_mapping_unimplemented");
                return result;
            }
            const auto decoded = NextV3FamilyCoordinate(family.coordinate, decimalDigitBudget);
            if (decoded.status != V3FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate)
            {
                result.observationUnavailable = true;
                result.obligations.emplace_back("v3_scheduled_family_unavailable");
                return result;
            }
            const auto identity = event_graph::V3FamilyIdentityContext::Create(*decoded.family, memberBudget);
            const auto verified = V3FamilyValidatedContext::Create(*decoded.family, memberBudget, *record.coordinate);
            if (identity.Status() != V3FamilyStatus::schema_only || verified.Status() != V3CoordinateStatus::coordinate_ready)
            {
                result.observationUnavailable = true;
                result.obligations.emplace_back("v3_scheduled_family_or_coordinate_unavailable");
                return result;
            }
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.id != record.candidateId)
            {
                result.observationUnavailable = true;
                result.obligations.emplace_back("v3_scheduled_candidate_identity_mismatch");
                return result;
            }
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget))
            {
                result.observationUnavailable = true;
                result.obligations.emplace_back("v3_scheduled_inflight_receipt_unavailable");
                return result;
            }
            receiptConsumed = true;
            bridgeEntered = true;
            return ingress_adapter::ExecuteV3DesktopSingleCopy(identity, verified,
                record.graph->present, record.graph->graph);
        }
        catch (const std::exception& error)
        {
            result.observationUnavailable = true;
            result.productionEntryUnknown = bridgeEntered;
            result.failure = error.what();
            result.obligations.emplace_back("v3_scheduled_adapter_observation_unavailable");
            return result;
        }
        catch (...)
        {
            result.observationUnavailable = true;
            result.productionEntryUnknown = bridgeEntered;
            result.obligations.emplace_back("v3_scheduled_adapter_unknown_observation_unavailable");
            return result;
        }
    }

};

// The v4 scheduler record, its regenerated family and the bridge all refer to
// the same candidate and selected event graph. This joins the preview to the
// real adapter entry without treating a prepared fixture as an execution.
class ScheduledV4Executor final
{
    struct EvidenceWriter
    {
        std::string bytes;
        std::size_t budget;
        void Token(std::string_view value)
        {
            const auto length = std::to_string(value.size()) + ":";
            if (length.size() > budget - bytes.size() || value.size() > budget - bytes.size() - length.size())
                throw std::length_error("v4_evidence_budget_unavailable");
            bytes.append(length); bytes.append(value);
        }
        template<class T> void Number(T value) { Token(std::to_string(value)); }
        void Raw(const void* data, std::size_t size)
        { Token(size ? std::string_view(static_cast<const char*>(data), size) : std::string_view{}); }
        void Bytes(const std::vector<std::byte>& value) { Raw(value.data(), value.size()); }
        void Wide(const std::wstring& value)
        {
            if (value.size() > (std::numeric_limits<std::size_t>::max)() / sizeof(wchar_t))
                throw std::length_error("v4_evidence_wide_size_unavailable");
            Raw(value.data(), value.size() * sizeof(wchar_t));
        }
        template<class T> void OptionalNumber(const std::optional<T>& value)
        { Number(value.has_value()); if (value) Number(*value); }
        void Cell(const std::optional<GridCell>& value)
        { Number(value.has_value()); if (value) { Wide(value->pageId); Number(value->column); Number(value->row); } }
        void FileId(const std::optional<FILE_ID_INFO>& value)
        {
            Number(value.has_value());
            if (value) { Number(value->VolumeSerialNumber); Raw(value->FileId.Identifier, sizeof(value->FileId.Identifier)); }
        }
        void Content(const std::optional<ole_content_output_observation::Evidence>& value)
        {
            Number(value.has_value()); if (!value) return;
            Number(value->observed); Number(value->matches); Token(value->error); OptionalNumber(value->actualSize);
            Number(value->actualBytesComplete); Bytes(value->actualBytes);
        }
        void OleEvent(const ole_driver::Observation& event)
        {
            Number(event.programIndex); Number(static_cast<int>(event.kind)); Number(event.result);
            OptionalNumber(event.keyState); OptionalNumber(event.offeredEffect); OptionalNumber(event.returnedEffect);
            Number(event.screenPoint.has_value());
            if (event.screenPoint) { Number(event.screenPoint->x); Number(event.screenPoint->y); }
        }
        void ContentCall(const ole_content_source_fixture::Call& call)
        {
            Number(call.query); Number(call.format); Number(call.aspect); Number(call.index);
            Number(call.requestedTymed); Number(call.targetDevicePresent); Number(call.result);
            Number(call.thread); Number(call.returnedTymed); Number(call.mediumObserved); Bytes(call.hglobalBytes);
        }
        void OleAdapterLog(const std::optional<snowdesktop::ole_adapter_observation::State>& value)
        {
            Number(value.has_value()); if (!value) return;
            Number(value->active); Number(value->failed); Number(value->session); Number(value->ownerThread);
            Number(value->count); Number(value->depth); Number(value->capacity);
        }
        void OleCapture(const ole_driver::V2CapturedOleReturn& capture)
        {
            OleEvent(capture.observation);
            const auto scope = [&](const auto& value) {
                Number(value.session); Number(value.call); Number(value.parent); Number(value.thread);
                Number(value.adapter); Number(value.depth); Number(static_cast<int>(value.kind));
                Number(static_cast<int>(value.phase)); Number(value.result);
            };
            scope(capture.entered); scope(capture.returned); OleAdapterLog(capture.adapterLog);
            Number(capture.hostSnapshot.has_value());
            if (capture.hostSnapshot)
            {
                const auto& host = *capture.hostSnapshot;
                Number(reinterpret_cast<std::uintptr_t>(host.window)); Number(host.sessionPoint.x); Number(host.sessionPoint.y);
                Number(host.target); Number(host.region); Number(host.thread);
                Number(host.unresolved.size()); for (const auto& reason : host.unresolved) Token(reason);
            }
        }
        void AppEvent(const snowdesktop::external_transfer_observation::Event& event)
        {
            const auto& scope = event.adapterScope;
            Number(event.task); Number(event.object); Number(event.thread); Number(static_cast<int>(event.phase));
            Number(event.value); Number(event.extra); Number(event.localTask); Number(event.localWork); Number(scope.observed);
            if (scope.observed)
            {
                Number(scope.session); Number(scope.call); Number(scope.parent); Number(scope.adapter);
                Number(scope.thread); Number(scope.depth); Number(scope.kind);
            }
            const auto& native = event.nativeScope;
            Number(native.session); Number(native.event); Number(native.delivery); Number(native.forward);
            Number(native.ownerThread); Number(native.forwarded); Number(native.queuedEvent); Number(event.currentRevision);
            Number(event.windowScope);
            Number(event.hasBlob); if (event.hasBlob) { Number(event.blobOffset); Number(event.blobSize); }
        }
        void DeliveryIdentity(const snowdesktop::private_pointer::DeliveryIdentity& value)
        {
            Number(value.ownerThread); Number(value.session); Number(value.event); Number(value.delivery);
            Number(value.forwarded); Number(value.forward); Number(value.queuedEvent);
        }
        void DeliveryLog(const snowdesktop::private_pointer::DeliveryObservationLog& log)
        {
            Number(log.ownerThread); Number(log.session); Number(log.failed); OptionalNumber(log.recordedCapacity); Number(log.entries.size());
            for (const auto& event : log.entries)
            {
                Number(static_cast<int>(event.kind)); DeliveryIdentity(event.identity);
                Number(reinterpret_cast<std::uintptr_t>(event.window)); Number(event.message);
                Number(event.wParam); Number(event.lParam); Number(event.result);
            }
        }
        void NativeState(const snowdesktop::private_pointer::State& state)
        {
            Number(state.enabled); Number(state.closing);
            Number(state.bounds.left); Number(state.bounds.top); Number(state.bounds.right); Number(state.bounds.bottom);
            Number(state.point.x); Number(state.point.y); Number(state.buttons);
            for (const auto key : state.keys) Number(key);
            Number(reinterpret_cast<std::uintptr_t>(state.hovered)); Number(reinterpret_cast<std::uintptr_t>(state.captured));
            Number(reinterpret_cast<std::uintptr_t>(state.focused)); Number(reinterpret_cast<std::uintptr_t>(state.foreground));
            Number(reinterpret_cast<std::uintptr_t>(state.cursor)); Number(state.sequence); Number(state.pendingEvents); Token(state.error);
            Number(state.trace.size());
            for (const auto& sample : state.trace)
            {
                Number(sample.sequence); Raw(&sample.milliseconds, sizeof(sample.milliseconds));
                Number(sample.point.x); Number(sample.point.y); Number(sample.buttons);
                Number(reinterpret_cast<std::uintptr_t>(sample.window)); Number(sample.message); Token(sample.phase);
                Number(sample.virtualKey); Number(sample.codeUnit);
            }
        }
        void WindowMessage(const ole_driver::MessageObservation& message)
        {
            Number(message.enteredSequence); Number(message.returnedSequence); Number(message.windowScope); Number(static_cast<int>(message.route));
            Number(reinterpret_cast<std::uintptr_t>(message.window)); Number(message.message); Number(message.wParam);
            OptionalNumber(message.inputLParam); DeliveryIdentity(message.delivery);
            Number(message.enteredThread); Number(message.returnedThread); Number(message.returned); Number(message.malformedCompletion);
            Number(message.completion.has_value());
            if (message.completion)
            {
                const auto& completion = *message.completion;
                Number(completion.fileOperation); Number(completion.succeeded); Number(completion.identity);
                Number(completion.task); Number(completion.work);
            }
            Number(message.windowResult);
        }
    };
    static void WriteDiagnosticBudget(EvidenceWriter& out,
        const std::optional<snowdesktop::external_transfer_observation::BudgetObservation>& budget)
    {
        out.Number(budget.has_value());
        if (!budget) return;
        out.Number(budget->eventCapacity); out.Number(budget->blobCapacity); out.Number(budget->stagingHandleCapacity);
        out.Number(budget->committedBytes);
        out.Number(budget->recordedEvents); out.Number(budget->recordedBlobBytes); out.Number(budget->recordedStagingHandles);
        out.Number(budget->captureClaimed); out.Number(budget->writersDrained); out.Number(budget->recordsLost);
        out.Number(budget->capacityExceeded);
    }
    static void RecordDiagnosticCapacity(v3_scheduler::OutcomeFacts& facts,
        const std::optional<snowdesktop::external_transfer_observation::BudgetObservation>& budget,
        const std::optional<snowdesktop::background_work_observation::BudgetObservation>& background)
    {
        if ((!budget || !budget->capacityExceeded) && (!background || !background->capacityExceeded)) return;
        facts.observationUnavailable = true;
        if (budget && budget->capacityExceeded) facts.obligations.emplace_back("v4_external_diagnostic_capacity_exceeded");
        if (background && background->capacityExceeded) facts.obligations.emplace_back("v4_background_diagnostic_capacity_exceeded");
        // This observation does not establish that fixture preparation or
        // the production entry had no side effects, so it cannot by itself
        // authorize an automatically retryable work state.
    }
    static void WriteBackgroundDiagnosticBudget(EvidenceWriter& out,
        const std::optional<snowdesktop::background_work_observation::BudgetObservation>& budget)
    {
        out.Number(budget.has_value());
        if (!budget) return;
        out.Number(budget->eventCapacity); out.Number(budget->committedBytes); out.Number(budget->recordedEvents);
        out.Number(budget->captureClaimed); out.Number(budget->writersDrained); out.Number(budget->recordsLost);
        out.Number(budget->capacityExceeded);
    }
    static void WriteGroupChildren(EvidenceWriter& out,
        const std::optional<behavior::Access::V4GroupChildrenSnapshot>& value)
    {
        out.Number(value.has_value());
        if (!value) return;
        const auto& actual = *value;
        out.Number(actual.host); out.Number(actual.group); out.Number(actual.groupData); out.Number(actual.contentData);
        out.Number(actual.ownerThread); out.Wide(actual.groupId); out.Wide(actual.activeId); out.Number(actual.groupType);
        out.OptionalNumber(actual.activeIndex);
        const auto point = [&](const std::optional<POINT>& p) {
            out.Number(p.has_value()); if (p) { out.Number(p->x); out.Number(p->y); }
        };
        point(actual.clientPoint); point(actual.screenPoint);
        out.Number(actual.viewport.left); out.Number(actual.viewport.top);
        out.Number(actual.viewport.right); out.Number(actual.viewport.bottom); out.Number(actual.layoutRevision);
        out.Number(actual.children.size());
        for (const auto& child : actual.children) { out.Wide(child.id); out.Number(child.widget); out.Number(child.type); }
    }
    static std::string EncodeEvidence(const ingress_adapter::BridgeResult& result,
        const v3_scheduler::CaseRecord& item, std::string_view executionId, std::size_t budget)
    {
        EvidenceWriter out{{}, budget};
        out.Token("v4_attempt_partial_observations_v132"); out.Token(executionId);
        out.Token(item.candidateId); out.Token(item.graphId); out.Token(item.presence);
        out.Number(result.productionStarted); out.Number(result.productionEntryUnknown);
        out.OptionalNumber(result.unattributedBusinessFailureObserved); out.OptionalNumber(result.unattributedEventOracleFailureObserved);
        WriteDiagnosticBudget(out, result.v4DiagnosticBudget);
        out.OleAdapterLog(result.v4OleAdapterLogLast);
        WriteBackgroundDiagnosticBudget(out, result.v4BackgroundDiagnosticBudget);
        out.Number(result.eventDispatchObservationRecorded); out.Number(result.eventDispatchFailureObserved);
        const auto writeCollectionInput = [&](const std::optional<ingress_adapter::BridgeResult::V4TextProgramCollectionInput>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& config = *input;
            out.Token(config.candidateId); out.Number(config.hostIdentity); out.Number(config.ownerThread);
            out.Number(config.actual); out.Number(config.candidateMatched);
            out.Number(config.matchingPreCallProgramIndices.size());
            for (const auto index : config.matchingPreCallProgramIndices) out.Number(index);
        };
        const auto writeDesktopInput = [&](const std::optional<ingress_adapter::BridgeResult::V4TextDesktopInput>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& desktop = *input; const auto& initial = desktop.initial;
            out.Token(desktop.candidateId); out.Number(desktop.hostIdentity); out.Number(desktop.ownerThread);
            out.Wide(initial.pageId); out.Wide(initial.pageName); out.Wide(initial.sentinelKey);
            out.Number(initial.columns); out.Number(initial.rows);
            out.Cell(std::optional<GridCell>{initial.sentinelCell}); out.Cell(std::optional<GridCell>{initial.targetCell});
            out.Number(initial.sentinelSpan.columns); out.Number(initial.sentinelSpan.rows);
            out.Number(initial.pageBounds.left); out.Number(initial.pageBounds.top); out.Number(initial.pageBounds.right); out.Number(initial.pageBounds.bottom);
            out.Number(initial.targetBounds.left); out.Number(initial.targetBounds.top); out.Number(initial.targetBounds.right); out.Number(initial.targetBounds.bottom);
            out.Number(initial.layoutRevision); out.Number(initial.pageRecorded);
            out.OptionalNumber(initial.savedColumns); out.OptionalNumber(initial.savedRows); out.Number(desktop.candidateMatched);
            out.Number(desktop.matchingPreCallProgramIndices.size());
            for (const auto index : desktop.matchingPreCallProgramIndices) out.Number(index);
        };
        writeCollectionInput(result.v4TextProgramCollectionInput); writeDesktopInput(result.v4TextDesktopInput);
        writeCollectionInput(result.v4FileProgramCollectionInput); writeDesktopInput(result.v4FileDesktopInput);
        writeCollectionInput(result.v4DirectoryProgramCollectionInput); writeDesktopInput(result.v4DirectoryDesktopInput);
        writeCollectionInput(result.v4VirtualProgramCollectionInput);
        writeCollectionInput(result.v4BatchProgramCollectionInput); writeDesktopInput(result.v4BatchDesktopInput);
        writeCollectionInput(result.v4LeaveProgramCollectionInput); writeDesktopInput(result.v4LeaveDesktopInput);
        writeCollectionInput(result.v4RejectProgramCollectionInput); writeDesktopInput(result.v4RejectDesktopInput);
        writeCollectionInput(result.v4MoveProgramCollectionInput); writeDesktopInput(result.v4MoveDesktopInput);
        out.Number(result.v4PinnedCopyLeaseInput.has_value());
        if (result.v4PinnedCopyLeaseInput)
        {
            const auto& lease = *result.v4PinnedCopyLeaseInput;
            out.Token(lease.candidateId); out.Number(lease.ownerBoundAndInitiallyObserved);
            out.Number(lease.matchingPreCallProgramIndices.size());
            for (const auto index : lease.matchingPreCallProgramIndices) out.Number(index);
            out.OptionalNumber(lease.receiptPreflightObserved); out.OptionalNumber(lease.receiptBeforeDropObserved);
            out.OptionalNumber(lease.receiptBeforeLeaveObserved);
            out.OptionalNumber(lease.receiptAfterAwaitObserved); out.OptionalNumber(lease.receiptSameLease);
        }
        const auto writeVirtualLayoutDisk = [&](const behavior::V4VirtualLayoutDiskSnapshot* input) {
            out.Number(input != nullptr); if (!input) return;
            const auto& snapshot = *input;
            out.Number(snapshot.captured); out.Number(snapshot.parsed); out.Number(snapshot.resourceUnavailable);
            out.Token(snapshot.candidateId); out.Token(snapshot.rawBytes); out.Token(snapshot.parseError);
            out.Wide(snapshot.path.native()); out.FileId(snapshot.fileId); out.OptionalNumber(snapshot.layoutRevision);
            out.Number(snapshot.modelItems.size());
            for (const auto& item : snapshot.modelItems)
            { out.Wide(item.key); out.Wide(item.path.native()); out.Cell(std::optional<GridCell>{item.cell}); }
            out.Number(snapshot.diskItems.size());
            for (const auto& item : snapshot.diskItems)
            {
                out.Token(item.key); out.Number(item.page.has_value()); if (item.page) out.Token(*item.page);
                out.OptionalNumber(item.column); out.OptionalNumber(item.row); out.Number(item.width); out.Number(item.height);
                out.Number(item.largeIcon.has_value());
            }
            out.Number(snapshot.unresolved.size()); for (const auto& reason : snapshot.unresolved) out.Token(reason);
        };
        writeVirtualLayoutDisk(result.v4VirtualLayoutDiskSnapshot ? &*result.v4VirtualLayoutDiskSnapshot : nullptr);
        out.Number(result.v4VirtualLayoutEvents.has_value());
        if (result.v4VirtualLayoutEvents)
        {
            const auto& events = *result.v4VirtualLayoutEvents;
            out.Number(events.sameReceipt); out.Number(events.modelTimerObserved); out.Number(events.synchronousSaveObserved);
            out.Number(events.writeInputObserved); out.OptionalNumber(events.snapshotPathMatchesWrite);
            out.Bytes(events.writePathBytes); out.Bytes(events.writeInputBytes);
            out.OptionalNumber(events.snapshotRevisionMatches); out.OptionalNumber(events.snapshotBytesMatchWrite);
            out.OptionalNumber(events.saveRevision);
            out.Number(events.records.size());
            for (const auto& record : events.records) { out.Number(record.index); out.AppEvent(record.event); }
            writeVirtualLayoutDisk(events.beforeReloadDisk ? &*events.beforeReloadDisk : nullptr);
            writeVirtualLayoutDisk(events.disk ? &*events.disk : nullptr);
            out.Number(events.unresolved.size()); for (const auto& reason : events.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualGeometry.has_value());
        if (result.v4VirtualGeometry)
        {
            const auto& geometry = *result.v4VirtualGeometry; const auto& initial = geometry.initial;
            out.Number(geometry.prepared); out.Number(geometry.resourceUnavailable); out.Token(geometry.candidateId);
            out.Number(geometry.client.x); out.Number(geometry.client.y); out.Number(geometry.screen.x); out.Number(geometry.screen.y);
            out.Cell(std::optional<GridCell>{geometry.expectedCell});
            out.Wide(initial.pageId); out.Wide(initial.pageName); out.Wide(initial.sentinelKey);
            out.Number(initial.columns); out.Number(initial.rows);
            out.Cell(std::optional<GridCell>{initial.sentinelCell}); out.Cell(std::optional<GridCell>{initial.targetCell});
            out.Number(initial.sentinelSpan.columns); out.Number(initial.sentinelSpan.rows);
            out.Number(initial.pageBounds.left); out.Number(initial.pageBounds.top); out.Number(initial.pageBounds.right); out.Number(initial.pageBounds.bottom);
            out.Number(initial.targetBounds.left); out.Number(initial.targetBounds.top); out.Number(initial.targetBounds.right); out.Number(initial.targetBounds.bottom);
            out.Number(initial.layoutRevision); out.Number(initial.pageRecorded);
            out.OptionalNumber(initial.savedColumns); out.OptionalNumber(initial.savedRows);
            out.Number(geometry.pointsByProgramIndex.size());
            for (const auto& point : geometry.pointsByProgramIndex)
            { out.Number(point.has_value()); if (point) { out.Number(point->x); out.Number(point->y); } }
            out.Number(geometry.unresolved.size()); for (const auto& reason : geometry.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualGeometryPreCallIndices.size());
        for (const auto index : result.v4VirtualGeometryPreCallIndices) out.Number(index);
        const auto writeOleInputs = [&](const std::vector<ingress_adapter::BridgeResult::V4FileOleInput>& inputs) {
            out.Number(inputs.size());
            for (const auto& input : inputs)
            {
                out.Number(input.programIndex); out.Number(static_cast<int>(input.kind));
                out.Token(input.buttons); out.Token(input.modifiers); out.Token(input.parameter);
                out.OptionalNumber(input.keyState); out.OptionalNumber(input.allowedEffect);
                out.Number(input.screenPoint.has_value());
                if (input.screenPoint) { out.Number(input.screenPoint->x); out.Number(input.screenPoint->y); }
                out.Number(input.candidateMatched);
            }
        };
        writeOleInputs(result.v4FileOleInputs); writeOleInputs(result.v4DirectoryOleInputs);
        out.Number(result.v4FileProviderInput.has_value());
        if (result.v4FileProviderInput)
        {
            const auto& provider = *result.v4FileProviderInput;
            out.Token(provider.candidateId); out.Number(provider.sourceIdentity); out.Number(provider.ownerThread);
            out.Number(provider.asynchronous); out.Number(provider.candidateMatched);
            out.Number(provider.starts); out.Number(provider.ends); out.Number(provider.checkedBeforeEnter);
        }
        const auto writeConflict = [&](const std::optional<ingress_adapter::BridgeResult::V4FileConflictInput>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& conflict = *input;
            out.Token(conflict.candidateId); out.Wide(conflict.destination.wstring());
            out.Number(conflict.ownerThread); out.Number(conflict.queryError); out.OptionalNumber(conflict.destinationExists);
            out.Number(conflict.candidateMatched); out.Number(conflict.checkedBeforeDrop);
        };
        writeConflict(result.v4FileConflictInput); writeConflict(result.v4DirectoryConflictInput);
        writeConflict(result.v4BatchConflictInput);
        out.Number(result.v4DirectoryInput.has_value());
        if (result.v4DirectoryInput)
        {
            const auto& directory = *result.v4DirectoryInput;
            out.Token(directory.candidateId); out.Wide(directory.sourcePath.wstring());
            out.Number(directory.sourceIdentity); out.Number(directory.ownerThread);
            out.Number(directory.sourceConstructed); out.Number(directory.initialTreeObserved);
            out.Number(directory.unresolved.size()); for (const auto& reason : directory.unresolved) out.Token(reason);
        }
        out.Number(result.v4MixedInput.has_value());
        if (result.v4MixedInput)
        {
            const auto& mixed = *result.v4MixedInput;
            out.Token(mixed.candidateId); out.Number(mixed.combinedSource); out.Number(mixed.fileSource);
            out.Number(mixed.ownerThread); out.Number(mixed.sourcePrepared); out.Number(mixed.resourceUnavailable);
            out.Number(mixed.initialCallsObserved);
            out.Number(mixed.calls.size());
            for (const auto& call : mixed.calls)
            {
                out.Number(call.query); out.Number(call.enumeration); out.Number(call.formatPresent); out.Number(call.targetDevicePresent);
                out.Number(static_cast<int>(call.phase)); out.Number(call.call); out.Number(call.parent); out.Number(call.depth);
                out.Number(call.format); out.Number(call.aspect); out.Number(call.tymed); out.Number(call.thread); out.Number(call.direction);
                out.Number(call.index); out.Number(call.result);
            }
            out.Number(mixed.unresolved.size()); for (const auto& reason : mixed.unresolved) out.Token(reason);
        }
        const auto writeMixedCalls = [&](const std::optional<std::vector<mixed_fixture::Call>>& calls) {
            out.Number(calls.has_value()); if (!calls) return;
            out.Number(calls->size());
            for (const auto& call : *calls)
            { out.Number(call.query); out.Number(call.enumeration); out.Number(call.formatPresent); out.Number(call.targetDevicePresent);
              out.Number(static_cast<int>(call.phase)); out.Number(call.call); out.Number(call.parent); out.Number(call.depth);
              out.Number(call.format); out.Number(call.aspect); out.Number(call.tymed); out.Number(call.thread); out.Number(call.direction);
              out.Number(call.index); out.Number(call.result); }
        };
        const auto writeMixedFileReads = [&](const std::optional<std::vector<ole_file_source_fixture::FileDropRead>>& reads) {
            out.Number(reads.has_value()); if (!reads) return;
            out.Number(reads->size());
            for (const auto& read : *reads)
            { out.Number(read.result); out.Number(static_cast<int>(read.capture)); out.Number(read.threadId);
              out.Number(read.startsAtEntry); out.Number(read.endsAtEntry); out.Number(read.paths.size());
              for (const auto& path : read.paths) out.Wide(path.wstring()); }
        };
        const auto writeMixedContentCalls = [&](const std::optional<std::vector<ole_content_source_fixture::Call>>& calls) {
            out.Number(calls.has_value()); if (!calls) return;
            out.Number(calls->size()); for (const auto& call : *calls) out.ContentCall(call);
        };
        out.OptionalNumber(result.v4MixedProviderBaselineCaptured);
        writeMixedCalls(result.v4MixedBaselineCalls); writeMixedFileReads(result.v4MixedBaselineFileReads);
        writeMixedContentCalls(result.v4MixedBaselineContentCalls);
        out.OptionalNumber(result.v4MixedProviderRecordsCaptured);
        writeMixedCalls(result.v4MixedCalls); writeMixedFileReads(result.v4MixedFileReads);
        writeMixedContentCalls(result.v4MixedContentCalls);
        out.Number(result.v4DirectoryLiveSource.has_value());
        if (result.v4DirectoryLiveSource)
        {
            const auto& live = *result.v4DirectoryLiveSource;
            out.Number(live.captured); out.Number(live.resourceUnavailable); out.Token(live.candidateId);
            out.Wide(live.source.wstring());
            out.FileId(live.captured ? std::optional<FILE_ID_INFO>{live.rootIdentity} : std::nullopt);
            out.Number(live.ownerThread); out.Number(live.sourceAddress); out.Number(live.nodes.size());
            for (const auto& node : live.nodes)
            {
                out.Token(node.slot); out.Wide(node.relative.wstring()); out.Number(node.directory);
                out.Number(node.createAttempted); out.Number(node.created); out.Number(node.identityObserved);
                out.Number(node.bytesObserved);
                out.FileId(node.identityObserved ? std::optional<FILE_ID_INFO>{node.identity} : std::nullopt);
                out.Bytes(node.actualBytes);
            }
            out.Number(live.unresolved.size()); for (const auto& reason : live.unresolved) out.Token(reason);
        }
        const auto writeDirectoryLease = [&](const std::optional<directory_fixture::DirectoryCopyLeaseObservation>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& lease = *input;
            out.Number(lease.inputObserved); out.Number(lease.resourceUnavailable); out.Number(lease.fixtureLeaseOverlapObserved);
            out.Token(lease.candidateId);
            out.Number(lease.unresolved.size()); for (const auto& reason : lease.unresolved) out.Token(reason);
        };
        writeDirectoryLease(result.v4DirectoryLease);
        writeDirectoryLease(result.v4DirectorySourcePreparation);
        writeDirectoryLease(result.v4DirectorySourceAtExecution);
        out.Number(result.v4CompletionTrace.has_value());
        if (result.v4CompletionTrace)
        {
            out.Number(result.v4CompletionTrace->size());
            for (const auto& message : *result.v4CompletionTrace) out.WindowMessage(message);
        }
        out.Number(result.v4OleObservations.size());
        for (const auto& event : result.v4OleObservations) out.OleEvent(event);
        out.Number(result.v4CapturedOleReturns.size());
        for (const auto& capture : result.v4CapturedOleReturns) out.OleCapture(capture);
        const auto writeFixtureInput = [&](const std::optional<ole_driver::V4UnicodeFixtureInputObservation>& input) {
            out.Number(input.has_value());
            if (!input) return;
            const auto& fixture = *input;
            out.Number(fixture.snapshotComplete); out.Number(fixture.resourceUnavailable);
            out.Number(fixture.ownerThread); out.Number(fixture.sourceIdentity);
            out.Number(fixture.format); out.Number(fixture.index); out.Number(fixture.tymed);
            out.Number(fixture.configuredGetFailure); out.Number(fixture.configuredReadFailure); out.Bytes(fixture.bytes);
            out.Number(fixture.axes.size());
            for (const auto& axis : fixture.axes)
            { out.Token(axis.axis); out.Token(axis.selected); out.Token(axis.observed); out.Number(axis.matches); }
            out.Number(fixture.unresolved.size()); for (const auto& reason : fixture.unresolved) out.Token(reason);
        };
        writeFixtureInput(result.v4UnicodeFixtureInput);
        writeFixtureInput(result.v4SourcePreparation);
        writeFixtureInput(result.v4SourceAtExecution);
        const auto writeFileInput = [&](const std::optional<ole_driver::V4SingleFileInputObservation>& input) {
            out.Number(input.has_value()); if (!input) return;
            out.Number(input->captured); out.Number(input->resourceUnavailable); out.Token(input->candidateId);
            out.Wide(input->root.wstring()); out.Wide(input->file.wstring());
            out.FileId(input->captured ? std::optional<FILE_ID_INFO>{input->rootId} : std::nullopt);
            out.FileId(input->captured ? std::optional<FILE_ID_INFO>{input->fileId} : std::nullopt);
            out.Bytes(input->bytes); out.Number(input->ownerThread); out.Number(input->sourceAddress);
            out.Number(input->unresolved.size()); for (const auto& reason : input->unresolved) out.Token(reason);
        };
        writeFileInput(result.v4FileSourcePreparation);
        writeFileInput(result.v4FileSourceAtExecution);
        const auto writeBatchInput = [&](const std::optional<ole_driver::V4IndependentFileInputObservation>& input) {
            out.Number(input.has_value()); if (!input) return;
            out.Number(input->captured); out.Number(input->resourceUnavailable); out.Token(input->candidateId);
            out.Wide(input->root.wstring());
            out.FileId(input->captured ? std::optional<FILE_ID_INFO>{input->rootId} : std::nullopt);
            out.Number(input->ownerThread); out.Number(input->sourceAddress); out.Number(input->files.size());
            for (const auto& file : input->files)
            { out.Wide(file.path.wstring()); out.FileId(std::optional<FILE_ID_INFO>{file.id}); out.Bytes(file.bytes); }
            out.Number(input->exactSourceAxes.size());
            for (const auto& [axis, value] : input->exactSourceAxes) { out.Token(axis); out.Token(value); }
            out.Number(input->unresolved.size()); for (const auto& reason : input->unresolved) out.Token(reason);
        };
        writeBatchInput(result.v4BatchSourcePreparation); writeBatchInput(result.v4BatchSourceAtExecution);
        out.Number(result.v4FileSourceInterval.has_value());
        if (result.v4FileSourceInterval)
        {
            const auto& interval = *result.v4FileSourceInterval;
            const auto state = [&](const ole_file_source_fixture::FileDropCallState& value) {
                out.Number(value.count); out.Number(value.depth); out.Number(value.owner); out.Number(value.failed);
            };
            state(interval.before); state(interval.after); out.Number(interval.events.size());
            for (const auto& event : interval.events)
            { out.Number(event.call); out.Number(event.parent); out.Number(event.depth); out.Number(event.thread);
              out.Number(static_cast<int>(event.phase)); out.Number(event.result); }
        }
        const auto writePayload = [&](const std::optional<ole_driver::V4FilePayloadObservation>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& payload = *input;
            out.Number(payload.identityValid); out.Number(payload.observationUnavailable);
            out.Number(payload.successfulPayloadObserved); out.Number(payload.candidatePathMismatchObserved);
            out.Number(payload.sourceStarts); out.Number(payload.sourceEnds); out.Number(payload.fileReads);
            out.Number(payload.endOutcome); out.Number(payload.endEffect); out.Wide(payload.expectedPath.wstring());
            out.Number(payload.reads.size());
            for (const auto& read : payload.reads)
            {
                out.Number(read.result); out.Number(static_cast<int>(read.capture)); out.Number(read.threadId);
                out.Number(read.startsAtEntry); out.Number(read.endsAtEntry); out.Number(read.paths.size());
                for (const auto& path : read.paths) out.Wide(path.wstring());
            }
            out.OptionalNumber(payload.setupProbePrefixCount); out.Number(payload.probes.size());
            for (const auto& probe : payload.probes)
            { out.Number(probe.hasFormat); out.Number(probe.format); out.Number(probe.aspect); out.Number(probe.index);
              out.Number(probe.tymed); out.Number(probe.targetDevicePresent); out.Number(probe.result); out.Number(probe.threadId); }
            out.Number(payload.unresolved.size()); for (const auto& reason : payload.unresolved) out.Token(reason);
        };
        writePayload(result.v4FilePayload); writePayload(result.v4DirectorySource);
        const auto writePathFlow = [&](const std::optional<ole_driver::V4FilePathFlowObservation>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& flow = *input;
            out.Number(flow.identityValid); out.Number(flow.valuesCaptured); out.Number(flow.exactValuesMatch);
            out.Number(flow.observationUnavailable); out.Number(flow.task); out.Number(flow.readWork); out.Number(flow.fileWork);
            out.Number(flow.content); out.Number(flow.operationResult); out.Number(flow.sourceOrder.has_value());
            if (flow.sourceOrder)
            {
                const auto& order = *flow.sourceOrder;
                out.Number(static_cast<int>(order.status)); out.Token(order.candidateId); out.Number(order.readIndex);
                out.Number(order.sourceResult); out.Number(order.thread); out.Number(static_cast<int>(order.capture));
                out.Number(order.startsAtEntry); out.Number(order.endsAtEntry);
                out.Number(order.modelIndexByActualOrder.size()); for (const auto index : order.modelIndexByActualOrder) out.Number(index);
                out.Number(order.actualPaths.size()); for (const auto& path : order.actualPaths) out.Wide(path.wstring());
                out.Number(order.unresolved.size()); for (const auto& reason : order.unresolved) out.Token(reason);
            }
            out.Number(flow.values.size());
            for (const auto& value : flow.values)
            {
                out.Number(value.index); out.Number(value.readLogIndex); out.Number(value.commitLogIndex);
                out.OptionalNumber(value.modelIndex); out.Bytes(value.providerUtf16); out.Bytes(value.readUtf16); out.Bytes(value.commitUtf16);
            }
            out.Number(flow.unresolved.size()); for (const auto& reason : flow.unresolved) out.Token(reason);
        };
        writePathFlow(result.v4FilePathFlow); writePathFlow(result.v4DirectoryPathFlow);
        const auto writeTaskWindow = [&](const std::optional<ole_driver::V4FileTaskWindowObservation>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& window = *input;
            out.Number(window.identityValid); out.Number(window.localAdapterScopeMatched); out.Number(window.observationUnavailable);
            out.Number(window.directCallsiteMatched); out.Number(window.directCallsite);
            out.OptionalNumber(window.callsiteEnterIndex); out.OptionalNumber(window.transferOriginIndex); out.OptionalNumber(window.callsiteReturnIndex);
            out.Number(window.drop.has_value()); if (window.drop) out.OleCapture(*window.drop);
            out.Number(window.passive.has_value());
            if (window.passive)
            {
                const auto& passive = *window.passive; const auto& scope = passive.adapterScope;
                out.Number(passive.task); out.Number(scope.observed); out.Number(scope.session); out.Number(scope.call);
                out.Number(scope.parent); out.Number(scope.adapter); out.Number(scope.thread); out.Number(scope.depth); out.Number(scope.kind);
                out.Number(passive.readEnqueued); out.Number(passive.readWorkerEntered); out.Number(passive.readWorkerReturned);
                out.Number(passive.readCallbacks); out.Number(passive.fileEnqueued); out.Number(passive.fileCallbacks);
                out.Number(passive.sourceCompletions); out.Number(passive.fileCompletions); out.Number(passive.sourceEnds);
                out.Number(passive.endEntered); out.OptionalNumber(passive.endRequestedStatus); out.OptionalNumber(passive.endReturnedStatus);
                out.Number(passive.endRequestedEffect); out.Number(passive.endLocalTask); out.Number(passive.endLocalWork); out.Number(passive.endThread);
                out.Number(passive.finishedClosure); out.Number(passive.finishedWork); out.Number(passive.finishedThread);
                out.Number(passive.finishedEntered); out.Number(passive.finishedReturned); out.Number(passive.finishedDuplicates);
                out.Number(passive.finishedContextMismatch); out.OptionalNumber(passive.finishedInput); out.OptionalNumber(passive.finishedOutcome);
                out.Number(passive.contentObject); out.Number(passive.contentReadWork); out.Number(passive.contentReadThread);
                out.Number(passive.contentCommitThread); out.Number(passive.contentReadRecords); out.Number(passive.contentCommitRecords);
                out.OptionalNumber(passive.contentReadPathCount); out.OptionalNumber(passive.contentCommitPathCount);
                out.Number(passive.workers.size());
                for (const auto& worker : passive.workers)
                {
                    out.Number(worker.work); out.Number(worker.worker); out.Number(worker.kind); out.Number(worker.workerThread);
                    out.Number(worker.dequeued); out.Number(worker.entered); out.Number(worker.returned); out.Number(worker.finished);
                    out.Number(worker.cancelled); out.Number(worker.completion); out.Number(worker.messageThread);
                    out.Number(worker.queueEntries); out.Number(worker.queueAccepted); out.Number(worker.queueFailed); out.Number(worker.callbacks);
                    out.Number(worker.posts); out.Number(worker.postFailed); out.Number(worker.messageEntered); out.Number(worker.messageReturned);
                    out.OptionalNumber(worker.executeResult); out.OptionalNumber(worker.callbackResult);
                }
                out.Number(passive.filePathsGetData.size());
                for (const auto& call : passive.filePathsGetData)
                { out.Number(call.call); out.Number(call.work); out.Number(call.sourceInterface); out.Number(call.thread);
                  out.Number(call.enterIndex); out.Number(call.returnIndex); out.Number(call.entered); out.Number(call.returned); out.OptionalNumber(call.result); }
                out.Number(passive.unresolved.size()); for (const auto& reason : passive.unresolved) out.Token(reason);
            }
            out.Number(window.events.size());
            for (const auto& record : window.events) { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(window.messages.size()); for (const auto& message : window.messages) out.WindowMessage(message);
            out.Number(window.messageCandidates.size());
            for (const auto& candidate : window.messageCandidates)
            { out.Number(candidate.work); out.Number(candidate.traceIndex); out.Number(candidate.fileOperation); }
            out.Number(window.localChain.has_value());
            if (window.localChain)
            {
                const auto& chain = *window.localChain;
                out.Number(chain.task); out.Number(chain.readWork); out.Number(chain.fileWork);
                out.Number(chain.readCompletion); out.Number(chain.fileCompletion); out.Number(chain.content); out.Number(chain.operationResult);
                out.Number(chain.readMessage); out.Number(chain.fileMessage); out.Number(chain.pathCount);
                out.Number(chain.resultReadIndex); out.Number(chain.resultQueueIndex);
            }
            out.Number(window.unresolved.size()); for (const auto& reason : window.unresolved) out.Token(reason);
        };
        writeTaskWindow(result.v4FileTaskWindow); writeTaskWindow(result.v4DirectoryTaskWindow);
        out.Number(result.v4DirectoryAbiEnvelope.has_value());
        if (result.v4DirectoryAbiEnvelope)
        {
            const auto& envelope = *result.v4DirectoryAbiEnvelope;
            out.Number(envelope.uniqueEnvelopeObserved); out.Number(envelope.observationUnavailable);
            out.Number(envelope.records.size());
            for (const auto& record : envelope.records) { out.Number(record.index); out.AppEvent(record.event); }
            out.Number(envelope.unresolved.size()); for (const auto& reason : envelope.unresolved) out.Token(reason);
        }
        const auto writeFileGraph = [&](const std::optional<ole_driver::V4FileGraphEvidence>& input) {
            out.Number(input.has_value()); if (!input) return;
            const auto& graph = *input;
            out.Number(graph.identityValid); out.Number(graph.hypothesisObservationRecorded);
            out.Number(graph.hypothesisNotMet); out.Number(graph.observationUnavailable);
            out.Number(graph.events.size());
            for (const auto& event : graph.events)
            { out.Token(event.id); out.Number(event.selectedPresent); out.Number(static_cast<int>(event.state));
              out.OptionalNumber(event.logIndex); out.OptionalNumber(event.traceIndex); }
            out.Number(graph.orders.size());
            for (const auto& order : graph.orders)
            { out.Token(order.before); out.Token(order.after); out.Token(order.basis); out.Number(order.selectedOrderAgrees); }
            out.Number(graph.oleEvidence.size()); for (const auto& event : graph.oleEvidence) out.OleCapture(event);
            out.Number(graph.payload.has_value());
            if (graph.payload)
            {
                const auto& payload = *graph.payload;
                out.Number(payload.identityValid); out.Number(payload.observationUnavailable);
                out.Number(payload.successfulPayloadObserved); out.Number(payload.candidatePathMismatchObserved);
                out.Number(payload.sourceStarts); out.Number(payload.sourceEnds); out.Number(payload.fileReads);
                out.Number(payload.endOutcome); out.Number(payload.endEffect); out.Wide(payload.expectedPath.wstring());
                out.Number(payload.reads.size());
                for (const auto& read : payload.reads)
                { out.Number(read.result); out.Number(static_cast<int>(read.capture)); out.Number(read.threadId);
                  out.Number(read.startsAtEntry); out.Number(read.endsAtEntry); out.Number(read.paths.size());
                  for (const auto& path : read.paths) out.Wide(path.wstring()); }
                out.OptionalNumber(payload.setupProbePrefixCount); out.Number(payload.probes.size());
                for (const auto& probe : payload.probes)
                { out.Number(probe.hasFormat); out.Number(probe.format); out.Number(probe.aspect); out.Number(probe.index);
                  out.Number(probe.tymed); out.Number(probe.targetDevicePresent); out.Number(probe.result); out.Number(probe.threadId); }
                out.Number(payload.unresolved.size()); for (const auto& reason : payload.unresolved) out.Token(reason);
            }
            out.Number(graph.taskWindow.has_value());
            if (graph.taskWindow)
            {
                const auto& window = *graph.taskWindow;
                out.Number(window.identityValid); out.Number(window.localAdapterScopeMatched); out.Number(window.observationUnavailable);
                out.Number(window.directCallsiteMatched); out.Number(window.directCallsite);
                out.OptionalNumber(window.callsiteEnterIndex); out.OptionalNumber(window.transferOriginIndex); out.OptionalNumber(window.callsiteReturnIndex);
                out.Number(window.drop.has_value()); if (window.drop) out.OleCapture(*window.drop);
                out.Number(window.passive.has_value());
                if (window.passive)
                {
                    const auto& passive = *window.passive; const auto& scope = passive.adapterScope;
                    out.Number(passive.task); out.Number(scope.observed); out.Number(scope.session); out.Number(scope.call);
                    out.Number(scope.parent); out.Number(scope.adapter); out.Number(scope.thread); out.Number(scope.depth); out.Number(scope.kind);
                    out.Number(passive.readEnqueued); out.Number(passive.readWorkerEntered); out.Number(passive.readWorkerReturned);
                    out.Number(passive.readCallbacks); out.Number(passive.fileEnqueued); out.Number(passive.fileCallbacks);
                    out.Number(passive.sourceCompletions); out.Number(passive.fileCompletions); out.Number(passive.sourceEnds);
                    out.Number(passive.endEntered); out.OptionalNumber(passive.endRequestedStatus); out.OptionalNumber(passive.endReturnedStatus);
                    out.Number(passive.endRequestedEffect); out.Number(passive.endLocalTask); out.Number(passive.endLocalWork); out.Number(passive.endThread);
                    out.Number(passive.finishedClosure); out.Number(passive.finishedWork); out.Number(passive.finishedThread);
                    out.Number(passive.finishedEntered); out.Number(passive.finishedReturned); out.Number(passive.finishedDuplicates);
                    out.Number(passive.finishedContextMismatch); out.OptionalNumber(passive.finishedInput); out.OptionalNumber(passive.finishedOutcome);
                    out.Number(passive.contentObject); out.Number(passive.contentReadWork); out.Number(passive.contentReadThread);
                    out.Number(passive.contentCommitThread); out.Number(passive.contentReadRecords); out.Number(passive.contentCommitRecords);
                    out.OptionalNumber(passive.contentReadPathCount); out.OptionalNumber(passive.contentCommitPathCount);
                    out.Number(passive.workers.size());
                    for (const auto& worker : passive.workers)
                    { out.Number(worker.work); out.Number(worker.worker); out.Number(worker.kind); out.Number(worker.workerThread);
                      out.Number(worker.dequeued); out.Number(worker.entered); out.Number(worker.returned); out.Number(worker.finished);
                      out.Number(worker.cancelled); out.Number(worker.completion); out.Number(worker.messageThread);
                      out.Number(worker.queueEntries); out.Number(worker.queueAccepted); out.Number(worker.queueFailed); out.Number(worker.callbacks);
                      out.Number(worker.posts); out.Number(worker.postFailed); out.Number(worker.messageEntered); out.Number(worker.messageReturned);
                      out.OptionalNumber(worker.executeResult); out.OptionalNumber(worker.callbackResult); }
                    out.Number(passive.filePathsGetData.size());
                    for (const auto& call : passive.filePathsGetData)
                    { out.Number(call.call); out.Number(call.work); out.Number(call.sourceInterface); out.Number(call.thread);
                      out.Number(call.enterIndex); out.Number(call.returnIndex); out.Number(call.entered); out.Number(call.returned); out.OptionalNumber(call.result); }
                    out.Number(passive.unresolved.size()); for (const auto& reason : passive.unresolved) out.Token(reason);
                }
                out.Number(window.events.size());
                for (const auto& event : window.events) { out.Number(event.index); out.AppEvent(event.event); out.Bytes(event.blob); }
                out.Number(window.messages.size()); for (const auto& message : window.messages) out.WindowMessage(message);
                out.Number(window.messageCandidates.size());
                for (const auto& candidate : window.messageCandidates)
                { out.Number(candidate.work); out.Number(candidate.traceIndex); out.Number(candidate.fileOperation); }
                out.Number(window.localChain.has_value());
                if (window.localChain)
                { const auto& chain = *window.localChain; out.Number(chain.task); out.Number(chain.readWork); out.Number(chain.fileWork);
                  out.Number(chain.readCompletion); out.Number(chain.fileCompletion); out.Number(chain.content); out.Number(chain.operationResult);
                  out.Number(chain.readMessage); out.Number(chain.fileMessage); out.Number(chain.pathCount);
                  out.Number(chain.resultReadIndex); out.Number(chain.resultQueueIndex); }
                out.Number(window.unresolved.size()); for (const auto& reason : window.unresolved) out.Token(reason);
            }
            out.Number(graph.abiEnvelope.has_value());
            if (graph.abiEnvelope)
            {
                const auto& envelope = *graph.abiEnvelope;
                out.Number(envelope.uniqueEnvelopeObserved); out.Number(envelope.observationUnavailable);
                out.Number(envelope.records.size());
                for (const auto& record : envelope.records) { out.Number(record.index); out.AppEvent(record.event); }
                out.Number(envelope.unresolved.size()); for (const auto& reason : envelope.unresolved) out.Token(reason);
            }
            out.Number(graph.unresolved.size()); for (const auto& reason : graph.unresolved) out.Token(reason);
        };
        writeFileGraph(result.v4FileGraph); writeFileGraph(result.v4DirectoryGraph);
        const auto writeConfiguredSnapshot = [&](const std::optional<ole_driver::V4ConfiguredContentSnapshot>& snapshot) {
            out.Number(snapshot.has_value()); if (!snapshot) return;
            out.Number(snapshot->captured); out.Number(snapshot->resourceUnavailable);
            out.Number(snapshot->entries.size());
            for (const auto& entry : snapshot->entries)
            { out.Number(entry.format); out.Number(entry.index); out.Number(entry.tymed); out.Bytes(entry.bytes);
              out.Number(entry.readFailure); out.Number(entry.getDataFailure); }
            out.Number(snapshot->calls.size()); for (const auto& call : snapshot->calls) out.ContentCall(call);
            out.Number(snapshot->streams.size());
            for (const auto& stream : snapshot->streams)
            { out.Number(stream.index); out.Number(stream.identity); out.Number(stream.providerCallId);
              out.Number(stream.observationUnavailable); out.Number(stream.reads.size());
              for (const auto& read : stream.reads)
              { out.Number(read.requested); out.Number(read.returned); out.Number(read.result); out.Number(read.thread);
                out.Number(read.readId); out.Bytes(read.bytes); } }
            out.Number(snapshot->unresolved.size()); for (const auto& reason : snapshot->unresolved) out.Token(reason);
        };
        out.Number(result.v4ConfiguredInput.has_value());
        if (result.v4ConfiguredInput)
        { const auto& input = *result.v4ConfiguredInput; out.Token(input.candidateId); out.Number(input.sourceIdentity);
          out.Number(input.ownerThread); out.Number(input.inputPrepared); out.Number(input.resourceUnavailable);
          out.Number(input.unresolved.size()); for (const auto& reason : input.unresolved) out.Token(reason); }
        writeConfiguredSnapshot(result.v4ConfiguredPreparation); writeConfiguredSnapshot(result.v4ConfiguredSourceInitial);
        writeConfiguredSnapshot(result.v4ConfiguredSourceAtExecution); writeConfiguredSnapshot(result.v4ConfiguredSourceAfterAwait);
        writeConfiguredSnapshot(result.v4ConfiguredSourceAfterDispatch);
        out.OptionalNumber(result.v4ConfiguredAwaitReturned);
        const auto writeConfiguredObservation = [&](const std::optional<ole_driver::V4ConfiguredContentObservation>& value) {
            out.Number(value.has_value()); if (!value) return;
            const auto& observation = *value;
            out.Number(observation.identityValid); out.Number(observation.awaitReturned);
            out.Number(observation.providerIntervalObserved); out.Number(observation.observationUnavailable);
            out.Number(observation.oleEvidence.size()); for (const auto& item : observation.oleEvidence) out.OleCapture(item);
            writeConfiguredSnapshot(observation.afterAwait);
            out.Number(observation.records.size());
            for (const auto& record : observation.records)
            { out.Number(record.index); out.AppEvent(record.event); out.Number(record.blobCopied); out.Bytes(record.blob); }
            out.Number(observation.providerCalls.size());
            for (const auto& call : observation.providerCalls)
            { out.Number(call.callIndex); out.Number(call.enterIndex); out.Number(call.returnIndex); out.Number(call.callId); }
            out.Number(observation.completionTrace.size()); for (const auto& item : observation.completionTrace) out.WindowMessage(item);
            out.Number(observation.unresolved.size()); for (const auto& reason : observation.unresolved) out.Token(reason);
        };
        writeConfiguredObservation(result.v4ConfiguredObservation);
        out.Number(result.v4VirtualFailures.has_value());
        if (result.v4VirtualFailures)
        {
            const auto& failure = *result.v4VirtualFailures;
            out.Number(failure.identityValid); out.Number(failure.diagnosticComplete); out.Number(failure.batchBound);
            out.Number(failure.observationUnavailable); out.Number(failure.batch); out.Number(static_cast<int>(failure.executionStatus));
            out.Number(failure.productionStarted); out.Number(failure.productionEntryUnknown);
            out.Number(failure.businessFailureObserved); out.Number(failure.eventOracleFailureObserved); out.Token(failure.executionFailure);
            out.Number(failure.oleObservations.size()); for (const auto& event : failure.oleObservations) out.OleEvent(event);
            out.Number(failure.capturedReturns.size()); for (const auto& event : failure.capturedReturns) out.OleCapture(event);
            writeConfiguredSnapshot(failure.afterDispatch);
            out.Number(failure.records.size());
            for (const auto& record : failure.records)
            { out.Number(record.index); out.AppEvent(record.event); out.Number(record.blobCopied); out.Bytes(record.blob); }
            out.Number(failure.failures.size());
            for (const auto& fact : failure.failures)
            { out.Number(static_cast<int>(fact.kind)); out.Number(fact.firstIndex); out.Number(fact.lastIndex);
              out.Number(fact.callOrMember); out.OptionalNumber(fact.descriptorIndex); out.OptionalNumber(fact.format);
              out.Number(fact.actualValue); out.Number(fact.returned); }
            out.Number(failure.unresolved.size()); for (const auto& reason : failure.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualTrackedShell.has_value());
        if (result.v4VirtualTrackedShell)
        {
            const auto& tracked = *result.v4VirtualTrackedShell;
            out.Number(tracked.localItemObserved); out.Number(tracked.observationUnavailable); out.Number(tracked.resourceUnavailable);
            out.Number(tracked.batch); out.Number(tracked.work); out.Number(tracked.call);
            out.Number(tracked.records.size());
            for (const auto& record : tracked.records)
            { out.Number(record.index); out.AppEvent(record.event); out.Number(record.blobCopied); out.Bytes(record.blob); }
            out.Number(tracked.unresolved.size()); for (const auto& reason : tracked.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualFlow.has_value());
        if (result.v4VirtualFlow)
        {
            const auto& flow = *result.v4VirtualFlow;
            out.Number(flow.identityValid); out.Number(flow.batchObserved); out.Number(flow.pipelineReturned);
            out.Number(flow.callbackReturned); out.Number(flow.observationUnavailable); out.Number(flow.batch);
            out.Number(flow.operationResult); out.Number(flow.outputMappingObserved);
            out.Number(flow.members.size());
            for (const auto& member : flow.members)
            { out.Number(member.descriptorIndex); out.Number(member.outputIndex); out.Number(member.memberId); out.Number(member.providerCallId);
              out.Number(member.readIds.size()); for (const auto id : member.readIds) out.Number(id);
              out.Bytes(member.stagedPath); out.Bytes(member.pipelinePath); out.Bytes(member.actualDestination);
              out.FileId(member.stagedFileId); out.OptionalNumber(member.stagedAttributes); out.OptionalNumber(member.referencesSource);
              out.Number(member.observedReadValuesMatch); out.Number(member.terminalReadObserved); }
            writeConfiguredObservation(flow.content);
            out.Number(flow.unresolved.size()); for (const auto& reason : flow.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualStagingHandleReleaseDeferred);
        out.Number(result.v4VirtualStagingHandles.has_value());
        if (result.v4VirtualStagingHandles)
        {
            const auto& handles = *result.v4VirtualStagingHandles;
            out.Number(handles.logComplete); out.Number(handles.observationUnavailable); out.Number(handles.records.size());
            for (const auto& handle : handles.records)
            {
                out.Number(handle.batch); out.Number(handle.member); out.Number(handle.source); out.Number(handle.thread);
                out.Number(handle.handleIdentityObserved);
                if (handle.handleIdentityObserved) out.FileId(std::optional<FILE_ID_INFO>{handle.handleIdentity});
                out.Number(handle.namedIdentityObserved);
                if (handle.namedIdentityObserved) out.FileId(std::optional<FILE_ID_INFO>{handle.namedIdentity});
                out.Number(handle.actualSize); out.Bytes(handle.actualBytes); out.Number(handle.bytesComplete); out.Number(handle.nameMatchesHandle);
            }
        }
        out.Number(result.v4VirtualStagingComparison.has_value());
        if (result.v4VirtualStagingComparison)
        {
            const auto& comparison = *result.v4VirtualStagingComparison;
            out.Number(comparison.sameReceiptObserved); out.Number(comparison.comparisonComplete);
            out.Number(comparison.observationUnavailable); out.Number(comparison.resourceUnavailable);
            out.Number(comparison.members.size());
            for (const auto& member : comparison.members)
            {
                out.Number(member.descriptorIndex); out.Number(member.batch); out.Number(member.member);
                out.OptionalNumber(member.fileIdentityMatches); out.OptionalNumber(member.bytesMatch);
                out.OptionalNumber(member.namedIdentityMatches); out.OptionalNumber(member.sourceAndThreadMatches);
            }
            out.Number(comparison.unresolved.size());
            for (const auto& reason : comparison.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualRefresh.has_value());
        if (result.v4VirtualRefresh)
        {
            const auto& refresh = *result.v4VirtualRefresh;
            out.Number(refresh.localRefreshBound); out.Number(refresh.observationUnavailable);
            out.Number(refresh.batch); out.Number(refresh.revision); out.Number(refresh.serial); out.Number(refresh.windowScope);
            out.Number(refresh.host); out.Number(refresh.context); out.Number(refresh.workerState);
            out.OptionalNumber(refresh.windowTraceIndex);
            out.Number(refresh.appRecords.size());
            for (const auto& record : refresh.appRecords)
            { out.Number(record.index); out.AppEvent(record.event); out.Number(record.blobCopied); out.Bytes(record.blob); }
            out.Number(refresh.workerRecords.size());
            for (const auto& record : refresh.workerRecords)
            { const auto& event = record.event; out.Number(record.index); out.Number(static_cast<int>(event.phase));
              out.Number(event.state); out.Number(event.context); out.Number(event.window); out.Number(event.serial);
              out.Number(event.thread); out.Number(event.message); out.Number(event.result); }
            out.Number(refresh.windowMessage.has_value()); if (refresh.windowMessage) out.WindowMessage(*refresh.windowMessage);
            out.Number(refresh.modelSnapshotConsumed); out.OptionalNumber(refresh.modelOutcome);
            out.Number(refresh.modelLayoutSaveObserved); out.OptionalNumber(refresh.modelLayoutSaveRevision);
            out.OptionalNumber(refresh.modelWindowTraceIndex);
            out.Number(refresh.modelWindowMessage.has_value());
            if (refresh.modelWindowMessage) out.WindowMessage(*refresh.modelWindowMessage);
            out.Number(refresh.unresolved.size()); for (const auto& reason : refresh.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualProductionChain.has_value());
        if (result.v4VirtualProductionChain)
        {
            const auto& chain = *result.v4VirtualProductionChain;
            out.Number(chain.localTrackedToRefresh); out.Number(chain.observationUnavailable); out.Number(chain.resourceUnavailable);
            out.OptionalNumber(chain.streamBytesMatch); out.OptionalNumber(chain.terminalReadObserved);
            out.Number(chain.modelSnapshotConsumed); out.OptionalNumber(chain.modelOutcome);
            out.Number(chain.modelLayoutSaveObserved); out.OptionalNumber(chain.modelLayoutSaveRevision);
            out.Number(chain.tracked.has_value());
            if (chain.tracked)
            {
                const auto& tracked = *chain.tracked;
                out.Number(tracked.localItemObserved); out.Number(tracked.observationUnavailable); out.Number(tracked.resourceUnavailable);
                out.Number(tracked.batch); out.Number(tracked.work); out.Number(tracked.call); out.Number(tracked.records.size());
                for (const auto& record : tracked.records)
                { out.Number(record.index); out.AppEvent(record.event); out.Number(record.blobCopied); out.Bytes(record.blob); }
                out.Number(tracked.unresolved.size()); for (const auto& reason : tracked.unresolved) out.Token(reason);
            }
            out.Number(chain.refresh.has_value());
            if (chain.refresh)
            {
                const auto& refresh = *chain.refresh;
                out.Number(refresh.localRefreshBound); out.Number(refresh.observationUnavailable);
                out.Number(refresh.batch); out.Number(refresh.revision); out.Number(refresh.serial); out.Number(refresh.windowScope);
                out.Number(refresh.host); out.Number(refresh.context); out.Number(refresh.workerState);
                out.OptionalNumber(refresh.windowTraceIndex); out.Number(refresh.appRecords.size());
                for (const auto& record : refresh.appRecords)
                { out.Number(record.index); out.AppEvent(record.event); out.Number(record.blobCopied); out.Bytes(record.blob); }
                out.Number(refresh.workerRecords.size());
                for (const auto& record : refresh.workerRecords)
                { const auto& event = record.event; out.Number(record.index); out.Number(static_cast<int>(event.phase));
                  out.Number(event.state); out.Number(event.context); out.Number(event.window); out.Number(event.serial);
                  out.Number(event.thread); out.Number(event.message); out.Number(event.result); }
                out.Number(refresh.windowMessage.has_value()); if (refresh.windowMessage) out.WindowMessage(*refresh.windowMessage);
                out.Number(refresh.modelSnapshotConsumed); out.OptionalNumber(refresh.modelOutcome);
                out.Number(refresh.modelLayoutSaveObserved); out.OptionalNumber(refresh.modelLayoutSaveRevision);
                out.OptionalNumber(refresh.modelWindowTraceIndex); out.Number(refresh.modelWindowMessage.has_value());
                if (refresh.modelWindowMessage) out.WindowMessage(*refresh.modelWindowMessage);
                out.Number(refresh.unresolved.size()); for (const auto& reason : refresh.unresolved) out.Token(reason);
            }
            out.Number(chain.unresolved.size()); for (const auto& reason : chain.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualManifest.has_value());
        if (result.v4VirtualManifest)
        { const auto& manifest = *result.v4VirtualManifest; out.Number(manifest.ready); out.Number(manifest.resourceUnavailable);
          out.Token(manifest.candidateId); out.Number(manifest.members.size());
          for (const auto& member : manifest.members) { out.Number(member.descriptorIndex); out.Wide(member.name); out.Bytes(member.bytes); }
          out.Number(manifest.unresolved.size()); for (const auto& reason : manifest.unresolved) out.Token(reason); }
        out.Number(result.v4VirtualManifestInput.has_value());
        if (result.v4VirtualManifestInput)
        {
            const auto& input = *result.v4VirtualManifestInput;
            out.Token(input.candidateId); out.Number(input.sourceIdentity); out.Number(input.ownerThread);
            out.Number(input.captured); out.Number(input.resourceUnavailable); out.OptionalNumber(input.descriptorCount);
            out.Number(input.members.size());
            for (const auto& member : input.members) { out.Number(member.descriptorIndex); out.Wide(member.name); out.Bytes(member.bytes); }
            out.Number(input.unresolved.size()); for (const auto& reason : input.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualAxisInputAudit.has_value());
        if (result.v4VirtualAxisInputAudit)
        {
            const auto& audit = *result.v4VirtualAxisInputAudit;
            out.Token(audit.candidateId); out.Number(audit.captured); out.Number(audit.resourceUnavailable);
            out.Number(audit.axes.size());
            for (const auto& axis : audit.axes)
            { out.Token(axis.id); out.Token(axis.selected); out.Token(axis.state); out.Token(axis.actual); out.Token(axis.evidence);
              out.Number(axis.programIndices.size()); for (const auto index : axis.programIndices) out.Number(index); }
            out.Number(audit.unresolved.size()); for (const auto& reason : audit.unresolved) out.Token(reason);
        }
        out.Number(result.v4VirtualBusinessSealed); out.Number(result.v4VirtualBusiness.has_value());
        if (result.v4VirtualBusiness)
        {
            const auto& virtualFiles = *result.v4VirtualBusiness;
            out.Token(virtualFiles.candidateId); out.Number(virtualFiles.members.size());
            for (const auto& member : virtualFiles.members)
            { out.Number(member.descriptorIndex); out.Wide(member.output.wstring()); out.Bytes(member.expectedBytes);
              out.FileId(member.outputId); out.Cell(member.outputCell); out.Cell(member.reloadedCell);
              out.Content(member.outputBytes); out.Content(member.reloadedBytes); out.OptionalNumber(member.outputAttributes);
              out.OptionalNumber(member.outputPresent);
              out.OptionalNumber(member.distinctFromSentinel); out.OptionalNumber(member.distinctFromOtherOutputs);
              out.OptionalNumber(member.outputIdentityRetained); }
            out.FileId(virtualFiles.sentinelId); out.Cell(virtualFiles.sentinelCell); out.Cell(virtualFiles.reloadedSentinelCell);
            out.Cell(virtualFiles.requestedLanding);
            out.OptionalNumber(virtualFiles.initialRevision); out.OptionalNumber(virtualFiles.committedRevision);
            out.OptionalNumber(virtualFiles.initialCount); out.OptionalNumber(virtualFiles.finalCount); out.OptionalNumber(virtualFiles.reloadedCount);
            out.OptionalNumber(virtualFiles.descriptorZeroLandingMatched); out.OptionalNumber(virtualFiles.sentinelIdentityRetained);
            out.OptionalNumber(virtualFiles.sentinelBytesRetained); out.Number(virtualFiles.sourceCallsCaptured);
            writeVirtualLayoutDisk(virtualFiles.beforeReloadLayout.get());
            out.Number(virtualFiles.outputEnumerationObserved); out.Number(virtualFiles.baselineRevalidated);
            out.Number(virtualFiles.outputs.size());
            for (const auto& path : virtualFiles.outputs) out.Wide(path.wstring());
            const auto writeModelMembers = [&](const auto& members) {
                out.Number(members.size()); for (const auto& value : members)
                { out.Wide(value.first.wstring()); out.Wide(value.second.pageId); out.Number(value.second.column); out.Number(value.second.row); }
            };
            writeModelMembers(virtualFiles.finalMembers); writeModelMembers(virtualFiles.reloadedMembers);
            out.Number(virtualFiles.unresolved.size()); for (const auto& reason : virtualFiles.unresolved) out.Token(reason);
            out.OptionalNumber(virtualFiles.descriptorReadObserved);
            out.OptionalNumber(virtualFiles.memberReadsObserved); out.Number(virtualFiles.refreshMessageObserved);
            out.Number(virtualFiles.independentAssertionsCompleted); out.Number(virtualFiles.assertionsCompleted);
        }
        out.Number(result.v4VirtualOutputBinding.has_value());
        if (result.v4VirtualOutputBinding)
        {
            const auto& binding = *result.v4VirtualOutputBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.localBindingObserved); out.Number(binding.valueComparisonsMatch);
            out.Number(binding.observationUnavailable); out.Token(binding.candidateId);
            out.Number(binding.members.size());
            for (const auto& member : binding.members)
            { out.Number(member.descriptorIndex); out.Number(member.outputIndex); out.Number(member.actualDestinationMatches);
              out.Number(member.outputIdentityObserved); out.Number(member.bytesAndReloadObserved); out.OptionalNumber(member.landingMatches); }
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4MixedGraph.has_value());
        if (result.v4MixedGraph)
        {
            const auto& graph = *result.v4MixedGraph;
            out.Number(graph.identityValid); out.Number(graph.hypothesisObservationRecorded); out.Number(graph.hypothesisNotMet);
            out.Number(graph.observationUnavailable); out.Number(graph.providerRecordsComplete);
            out.Number(graph.diagnosticRecordsComplete); out.Number(graph.completionTraceCaptured);
            out.Number(graph.unicodeEnvelopeObserved);
            if (graph.unicodeEnvelopeObserved)
                for (const auto index : graph.unicodeEnvelopeIndices) out.Number(index);
            out.Number(graph.events.size());
            for (const auto& event : graph.events)
            { out.Token(event.id); out.Number(event.selectedPresent); out.Number(static_cast<int>(event.state));
              out.OptionalNumber(event.logIndex); out.OptionalNumber(event.traceIndex); }
            out.Number(graph.orders.size());
            for (const auto& order : graph.orders)
            { out.Token(order.before); out.Token(order.after); out.Token(order.basis); out.Number(order.selectedOrderAgrees); }
            out.Number(graph.oleEvidence.size()); for (const auto& event : graph.oleEvidence) out.OleCapture(event);
            out.Number(graph.mixedCalls.size());
            for (const auto& call : graph.mixedCalls)
            { out.Number(call.query); out.Number(call.enumeration); out.Number(call.formatPresent); out.Number(call.targetDevicePresent);
              out.Number(static_cast<int>(call.phase)); out.Number(call.call); out.Number(call.parent); out.Number(call.depth);
              out.Number(call.format); out.Number(call.aspect); out.Number(call.tymed); out.Number(call.thread); out.Number(call.direction);
              out.Number(call.index); out.Number(call.result); }
            out.Number(graph.fileReads.size());
            for (const auto& read : graph.fileReads)
            { out.Number(read.result); out.Number(static_cast<int>(read.capture)); out.Number(read.threadId);
              out.Number(read.startsAtEntry); out.Number(read.endsAtEntry); out.Number(read.paths.size());
              for (const auto& path : read.paths) out.Wide(path.wstring()); }
            out.Number(graph.contentCalls.size()); for (const auto& call : graph.contentCalls) out.ContentCall(call);
            out.Number(graph.diagnosticRecords.size());
            for (const auto& record : graph.diagnosticRecords)
            { out.Number(record.index); out.AppEvent(record.event); out.Number(record.blobCopied); out.Bytes(record.blob); }
            out.Number(graph.completionTrace.size()); for (const auto& message : graph.completionTrace) out.WindowMessage(message);
            writeTaskWindow(graph.taskWindow);
            out.Number(graph.unresolved.size()); for (const auto& reason : graph.unresolved) out.Token(reason);
        }
        const auto writeMoveSourceAfter = [&](const std::optional<V4SharedMoveSourceAfter>& after) {
            out.Number(after.has_value()); if (!after) return;
            out.FileId(after->heldId); out.FileId(after->namedId);
            out.OptionalNumber(after->heldIdentityMatches); out.OptionalNumber(after->heldBytesMatch);
            out.OptionalNumber(after->originalNamePresent); out.OptionalNumber(after->originalNameMatchesSource);
            out.Number(after->observed); out.Number(after->resourceUnavailable);
            out.Number(after->unresolved.size()); for (const auto& reason : after->unresolved) out.Token(reason);
        };
        out.Number(result.v4SharedMoveInput.has_value());
        if (result.v4SharedMoveInput)
        {
            const auto& input = *result.v4SharedMoveInput;
            out.Token(input.candidateId); out.Number(input.ownerBoundAndInitiallyObserved);
            out.Number(input.receiptBeforeDropObserved); out.Number(input.receiptSameLease);
            out.Number(input.matchingPreCallProgramIndices.size());
            for (const auto index : input.matchingPreCallProgramIndices) out.Number(index);
            writeMoveSourceAfter(input.afterAwait);
        }
        out.Number(result.v4TrackedShellMove.has_value());
        if (result.v4TrackedShellMove)
        {
            const auto& tracked = *result.v4TrackedShellMove;
            out.Number(tracked.volumeDecisionObserved); out.Number(tracked.shellCallbackObserved);
            out.Number(tracked.observationUnavailable); out.Number(tracked.resourceUnavailable);
            out.Number(tracked.task); out.Number(tracked.readWork); out.Number(tracked.fileWork); out.Number(tracked.call);
            out.Number(tracked.records.size());
            for (const auto& record : tracked.records)
            { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(tracked.messages.size()); for (const auto& message : tracked.messages) out.WindowMessage(message);
            out.Number(tracked.drop.has_value()); if (tracked.drop) out.OleCapture(*tracked.drop);
            out.Number(tracked.localChain.has_value());
            if (tracked.localChain)
            {
                const auto& chain = *tracked.localChain;
                out.Number(chain.task); out.Number(chain.readWork); out.Number(chain.fileWork);
                out.Number(chain.readCompletion); out.Number(chain.fileCompletion); out.Number(chain.content); out.Number(chain.operationResult);
                out.Number(chain.readMessage); out.Number(chain.fileMessage); out.Number(chain.pathCount);
                out.Number(chain.resultReadIndex); out.Number(chain.resultQueueIndex);
            }
            out.Number(tracked.abi.has_value());
            if (tracked.abi)
            {
                const auto& abi = *tracked.abi;
                out.Number(abi.uniqueEnvelopeObserved); out.Number(abi.observationUnavailable); out.Number(abi.records.size());
                for (const auto& record : abi.records) { out.Number(record.index); out.AppEvent(record.event); }
                out.Number(abi.unresolved.size()); for (const auto& reason : abi.unresolved) out.Token(reason);
            }
            out.Number(tracked.unresolved.size()); for (const auto& reason : tracked.unresolved) out.Token(reason);
        }
        out.Number(result.v4MoveBusinessBinding.has_value());
        if (result.v4MoveBusinessBinding)
        {
            const auto& binding = *result.v4MoveBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.trackedMoveObserved); out.Number(binding.completionObserved);
            out.Number(binding.observationUnavailable); out.OptionalNumber(binding.readCompletionEntered); out.OptionalNumber(binding.fileCompletionEntered);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.outputBytesMatch); out.OptionalNumber(binding.outputSameSource);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4MoveBusinessSealed); out.Number(result.v4MoveBusiness.has_value());
        if (result.v4MoveBusiness)
        {
            const auto& move = *result.v4MoveBusiness;
            out.Token(move.candidateId); out.Wide(move.source.wstring()); out.Wide(move.output.wstring()); out.Wide(move.sentinel.wstring());
            out.Bytes(move.expectedBytes); out.FileId(move.sourceId); out.FileId(move.outputId); out.FileId(move.sentinelId);
            writeMoveSourceAfter(move.sourceAfter);
            out.Number(move.completion.has_value());
            if (move.completion)
            { out.Number(static_cast<int>(move.completion->signal)); out.Number(move.completion->sourceEntered);
              out.Number(move.completion->commitEntered); out.Token(move.completion->detail); }
            out.OptionalNumber(move.outputPresent); out.OptionalNumber(move.outputBytesMatch);
            out.OptionalNumber(move.outputSameSource); out.OptionalNumber(move.sentinelPreserved);
            out.OptionalNumber(move.reloadedOutputBytesMatch); out.OptionalNumber(move.reloadedOutputIdentitySame);
            out.OptionalNumber(move.reloadedCellMatches);
            out.Cell(move.outputCell); out.Cell(move.reloadedCell);
            out.OptionalNumber(move.initialRevision); out.OptionalNumber(move.finalRevision); out.OptionalNumber(move.reloadedRevision);
            out.Number(move.inputObserved); out.Number(move.baselineRevalidated); out.Number(move.finalObserved); out.Number(move.reloadObserved);
            out.Number(move.unresolved.size()); for (const auto& reason : move.unresolved) out.Token(reason);
        }
        out.Number(result.v4TrackedShellCopy.has_value());
        if (result.v4TrackedShellCopy)
        {
            const auto& tracked = *result.v4TrackedShellCopy;
            out.Number(tracked.identityValid); out.Number(tracked.localItemObserved);
            out.Number(tracked.observationUnavailable); out.Number(tracked.resourceUnavailable);
            out.Number(tracked.task); out.Number(tracked.fileWork); out.Number(tracked.call);
            out.Number(tracked.records.size());
            for (const auto& record : tracked.records)
            { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(tracked.unresolved.size()); for (const auto& reason : tracked.unresolved) out.Token(reason);
        }
        out.Number(result.v4CopyBusiness.has_value());
        if (result.v4CopyBusiness)
        {
            const auto& copy = *result.v4CopyBusiness;
            out.Wide(copy.source.wstring()); out.Wide(copy.output.wstring()); out.Bytes(copy.expectedBytes);
            out.FileId(copy.sourceId); out.FileId(copy.outputId); out.Cell(copy.outputCell); out.Cell(copy.reloadedOutputCell);
            out.Content(copy.outputBytes); out.Content(copy.reloadedOutputBytes);
            out.OptionalNumber(copy.initialLayoutRevision); out.OptionalNumber(copy.committedLayoutRevision);
            out.Number(copy.completion.has_value());
            if (copy.completion)
            { out.Number(static_cast<int>(copy.completion->signal)); out.Number(copy.completion->sourceEntered);
              out.Number(copy.completion->commitEntered); out.Token(copy.completion->detail); }
            out.OptionalNumber(copy.initialModelCount); out.OptionalNumber(copy.finalModelCount);
            out.OptionalNumber(copy.reloadedModelCount); out.Number(copy.sourcePayloadObserved); out.Number(copy.assertionsCompleted);
        }
        out.Number(result.v4CollectionIngressInput.has_value());
        if (result.v4CollectionIngressInput)
        {
            const auto& input = *result.v4CollectionIngressInput;
            out.Token(input.candidateId); out.Number(input.viewportInputMatched); out.Number(input.events.size());
            for (const auto& event : input.events)
            {
                const auto& geometry = event.geometry;
                out.Number(event.programIndex); out.Number(static_cast<int>(event.kind));
                out.Number(geometry.host); out.Number(geometry.container); out.Number(geometry.ownerThread);
                out.Number(geometry.client.x); out.Number(geometry.client.y);
                out.Number(geometry.screen.x); out.Number(geometry.screen.y);
                out.Number(geometry.viewport.left); out.Number(geometry.viewport.top);
                out.Number(geometry.viewport.right); out.Number(geometry.viewport.bottom); out.Number(geometry.layoutRevision);
            }
            out.Number(input.inactiveTabInputMatched); out.Number(input.resourceUnavailable); out.Number(input.generatedFromModel);
            out.Number(input.tabEvents.size());
            for (const auto& event : input.tabEvents)
            {
                const auto& geometry = event.geometry; const auto& group = geometry.group;
                out.Number(event.programIndex); out.Number(static_cast<int>(event.kind));
                out.Wide(geometry.childId); out.Number(geometry.child);
                out.Number(geometry.tab.left); out.Number(geometry.tab.top);
                out.Number(geometry.tab.right); out.Number(geometry.tab.bottom);
                out.Number(geometry.client.x); out.Number(geometry.client.y);
                out.Number(geometry.screen.x); out.Number(geometry.screen.y);
                out.Number(group.host); out.Number(group.group); out.Number(group.groupData); out.Number(group.contentData);
                out.Number(group.ownerThread); out.Wide(group.groupId); out.Wide(group.activeId); out.Number(group.groupType);
                out.OptionalNumber(group.activeIndex);
                const auto point = [&](const std::optional<POINT>& value) {
                    out.Number(value.has_value()); if (value) { out.Number(value->x); out.Number(value->y); }
                };
                point(group.clientPoint); point(group.screenPoint);
                out.Number(group.viewport.left); out.Number(group.viewport.top);
                out.Number(group.viewport.right); out.Number(group.viewport.bottom); out.Number(group.layoutRevision);
                out.Number(group.children.size());
                for (const auto& child : group.children) { out.Wide(child.id); out.Number(child.widget); out.Number(child.type); }
            }
            out.Number(input.matchingPreCallProgramIndices.size());
            for (const auto index : input.matchingPreCallProgramIndices) out.Number(index);
        }
        out.Number(result.v4CollectionBusinessBinding.has_value());
        if (result.v4CollectionBusinessBinding)
        {
            const auto& binding = *result.v4CollectionBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.pathFlowObserved); out.Number(binding.completionObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.sourcePreserved);
            out.OptionalNumber(binding.outputBytesMatch); out.OptionalNumber(binding.membershipMatches);
            out.OptionalNumber(binding.reloadMatches); out.OptionalNumber(binding.widgetPreserved);
            out.OptionalNumber(binding.reloadedWidgetPreserved); out.OptionalNumber(binding.noDesktopDuplicate);
            out.OptionalNumber(binding.readCompletionEntered); out.OptionalNumber(binding.fileCompletionEntered);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4CollectionBusinessSealed); out.Number(result.v4CollectionCopyBusiness.has_value());
        if (result.v4CollectionCopyBusiness)
        {
            const auto& collection = *result.v4CollectionCopyBusiness;
            out.Token(collection.candidateId); out.Wide(collection.source.wstring()); out.Wide(collection.output.wstring());
            out.Wide(collection.existing.wstring()); out.Bytes(collection.expectedBytes);
            out.FileId(collection.sourceId); out.FileId(collection.outputId); out.FileId(collection.existingId);
            const auto paths = [&](const auto& values) { out.Number(values.size()); for (const auto& path : values) out.Wide(path.wstring()); };
            paths(collection.initialMembers); paths(collection.finalMembers); paths(collection.reloadedMembers);
            out.Number(collection.completion.has_value());
            if (collection.completion)
            { out.Number(static_cast<int>(collection.completion->signal)); out.Number(collection.completion->sourceEntered);
              out.Number(collection.completion->commitEntered); out.Token(collection.completion->detail); }
            out.OptionalNumber(collection.initialRevision); out.OptionalNumber(collection.finalRevision);
            out.OptionalNumber(collection.reloadedRevision); out.OptionalNumber(collection.sourcePreserved);
            out.OptionalNumber(collection.existingPreserved); out.OptionalNumber(collection.outputBytesMatch);
            out.OptionalNumber(collection.membershipMatches); out.OptionalNumber(collection.reloadedMembershipMatches);
            out.OptionalNumber(collection.noDesktopDuplicate); out.OptionalNumber(collection.widgetPreserved);
            out.OptionalNumber(collection.reloadedWidgetPreserved); out.Number(collection.inputObserved);
            out.Number(collection.finalObserved); out.Number(collection.reloadObserved); out.Number(collection.baselineRevalidated);
            out.Number(collection.unresolved.size()); for (const auto& reason : collection.unresolved) out.Token(reason);
        }
        out.Number(result.v4DesktopFilesIngressInput.has_value());
        if (result.v4DesktopFilesIngressInput)
        {
            const auto& input = *result.v4DesktopFilesIngressInput;
            out.Token(input.candidateId); out.Number(input.viewportInputMatched); out.Number(input.events.size());
            for (const auto& event : input.events)
            {
                const auto& geometry = event.geometry;
                out.Number(event.programIndex); out.Number(static_cast<int>(event.kind));
                out.Number(geometry.host); out.Number(geometry.container); out.Number(geometry.ownerThread);
                out.Number(geometry.client.x); out.Number(geometry.client.y);
                out.Number(geometry.screen.x); out.Number(geometry.screen.y);
                out.Number(geometry.viewport.left); out.Number(geometry.viewport.top);
                out.Number(geometry.viewport.right); out.Number(geometry.viewport.bottom); out.Number(geometry.layoutRevision);
            }
            out.Number(input.matchingPreCallProgramIndices.size());
            for (const auto index : input.matchingPreCallProgramIndices) out.Number(index);
        }
        out.Number(result.v4DesktopFilesBusinessBinding.has_value());
        if (result.v4DesktopFilesBusinessBinding)
        {
            const auto& binding = *result.v4DesktopFilesBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.pathFlowObserved); out.Number(binding.completionObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.sourcePreserved);
            out.OptionalNumber(binding.outputBytesMatch); out.OptionalNumber(binding.membershipMatches);
            out.OptionalNumber(binding.reloadMatches); out.OptionalNumber(binding.widgetPreserved);
            out.OptionalNumber(binding.reloadedWidgetPreserved); out.OptionalNumber(binding.noDesktopDuplicate);
            out.OptionalNumber(binding.readCompletionEntered); out.OptionalNumber(binding.fileCompletionEntered);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4DesktopFilesBusinessSealed); out.Number(result.v4DesktopFilesCopyBusiness.has_value());
        if (result.v4DesktopFilesCopyBusiness)
        {
            const auto& collection = *result.v4DesktopFilesCopyBusiness;
            out.Token(collection.candidateId); out.Wide(collection.source.wstring()); out.Wide(collection.output.wstring());
            out.Wide(collection.existing.wstring()); out.Bytes(collection.expectedBytes);
            out.FileId(collection.sourceId); out.FileId(collection.outputId); out.FileId(collection.existingId);
            const auto paths = [&](const auto& values) { out.Number(values.size()); for (const auto& path : values) out.Wide(path.wstring()); };
            paths(collection.initialMembers); paths(collection.finalMembers); paths(collection.reloadedMembers);
            out.Number(collection.completion.has_value());
            if (collection.completion)
            { out.Number(static_cast<int>(collection.completion->signal)); out.Number(collection.completion->sourceEntered);
              out.Number(collection.completion->commitEntered); out.Token(collection.completion->detail); }
            out.OptionalNumber(collection.initialRevision); out.OptionalNumber(collection.finalRevision);
            out.OptionalNumber(collection.reloadedRevision); out.OptionalNumber(collection.sourcePreserved);
            out.OptionalNumber(collection.existingPreserved); out.OptionalNumber(collection.outputBytesMatch);
            out.OptionalNumber(collection.membershipMatches); out.OptionalNumber(collection.reloadedMembershipMatches);
            out.OptionalNumber(collection.noDesktopDuplicate); out.OptionalNumber(collection.widgetPreserved);
            out.OptionalNumber(collection.reloadedWidgetPreserved); out.Number(collection.inputObserved);
            out.Number(collection.finalObserved); out.Number(collection.reloadObserved); out.Number(collection.baselineRevalidated);
            out.Number(collection.unresolved.size()); for (const auto& reason : collection.unresolved) out.Token(reason);
        }
        out.Number(result.v4FolderMappingIngressInput.has_value());
        if (result.v4FolderMappingIngressInput)
        {
            const auto& input = *result.v4FolderMappingIngressInput;
            out.Token(input.candidateId); out.Number(input.viewportInputMatched); out.Number(input.events.size());
            for (const auto& event : input.events)
            {
                const auto& geometry = event.geometry;
                out.Number(event.programIndex); out.Number(static_cast<int>(event.kind));
                out.Number(geometry.host); out.Number(geometry.container); out.Number(geometry.ownerThread);
                out.Number(geometry.client.x); out.Number(geometry.client.y);
                out.Number(geometry.screen.x); out.Number(geometry.screen.y);
                out.Number(geometry.viewport.left); out.Number(geometry.viewport.top);
                out.Number(geometry.viewport.right); out.Number(geometry.viewport.bottom); out.Number(geometry.layoutRevision);
            }
            out.Number(input.matchingPreCallProgramIndices.size());
            for (const auto index : input.matchingPreCallProgramIndices) out.Number(index);
        }
        out.Number(result.v4FolderMappingBusinessBinding.has_value());
        if (result.v4FolderMappingBusinessBinding)
        {
            const auto& binding = *result.v4FolderMappingBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.pathFlowObserved); out.Number(binding.completionObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.sourcePreserved);
            out.OptionalNumber(binding.outputBytesMatch); out.OptionalNumber(binding.membershipMatches);
            out.OptionalNumber(binding.reloadMatches); out.OptionalNumber(binding.widgetPreserved);
            out.OptionalNumber(binding.reloadedWidgetPreserved); out.OptionalNumber(binding.noDesktopDuplicate);
            out.OptionalNumber(binding.mappedDirectoryPreserved); out.OptionalNumber(binding.sentinelPreserved);
            out.OptionalNumber(binding.existingPreserved); out.OptionalNumber(binding.readCompletionEntered); out.OptionalNumber(binding.fileCompletionEntered);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4FolderMappingBusinessSealed); out.Number(result.v4FolderMappingCopyBusiness.has_value());
        if (result.v4FolderMappingCopyBusiness)
        {
            const auto& collection = *result.v4FolderMappingCopyBusiness;
            out.Token(collection.candidateId); out.Wide(collection.source.wstring()); out.Wide(collection.output.wstring());
            out.Wide(collection.existing.wstring()); out.Wide(collection.mappedDirectory.wstring()); out.Bytes(collection.expectedBytes);
            out.FileId(collection.sourceId); out.FileId(collection.outputId); out.FileId(collection.existingId);
            out.FileId(collection.mappedDirectoryId); out.FileId(collection.sentinelId);
            const auto paths = [&](const auto& values) { out.Number(values.size()); for (const auto& path : values) out.Wide(path.wstring()); };
            paths(collection.initialMembers); paths(collection.finalMembers); paths(collection.reloadedMembers);
            out.Number(collection.completion.has_value());
            if (collection.completion)
            { out.Number(static_cast<int>(collection.completion->signal)); out.Number(collection.completion->sourceEntered);
              out.Number(collection.completion->commitEntered); out.Token(collection.completion->detail); }
            out.OptionalNumber(collection.initialRevision); out.OptionalNumber(collection.finalRevision);
            out.OptionalNumber(collection.reloadedRevision); out.OptionalNumber(collection.sourcePreserved);
            out.OptionalNumber(collection.existingPreserved); out.OptionalNumber(collection.outputBytesMatch);
            out.OptionalNumber(collection.membershipMatches); out.OptionalNumber(collection.reloadedMembershipMatches);
            out.OptionalNumber(collection.noDesktopDuplicate); out.OptionalNumber(collection.widgetPreserved);
            out.OptionalNumber(collection.reloadedWidgetPreserved); out.OptionalNumber(collection.sentinelPreserved);
            out.OptionalNumber(collection.mappedDirectoryPreserved);
            out.OptionalNumber(collection.mappedParentVerifiedBeforeDrop); out.OptionalNumber(collection.mappedDistinctFromSourceAndDesktop);
            out.Number(collection.inputObserved);
            out.Number(collection.finalObserved); out.Number(collection.reloadObserved); out.Number(collection.baselineRevalidated);
            out.Number(collection.unresolved.size()); for (const auto& reason : collection.unresolved) out.Token(reason);
        }
        out.Number(result.v4GroupIngressInput.has_value());
        if (result.v4GroupIngressInput)
        {
            const auto& input = *result.v4GroupIngressInput;
            out.Token(input.candidateId); out.Number(input.viewportInputMatched); out.Number(input.events.size());
            for (const auto& event : input.events)
            {
                const auto& geometry = event.geometry;
                out.Number(event.programIndex); out.Number(static_cast<int>(event.kind));
                out.Number(geometry.host); out.Number(geometry.group); out.Number(geometry.activeWidget);
                out.Number(geometry.ownerThread); out.Wide(geometry.activeId);
                out.Number(geometry.client.x); out.Number(geometry.client.y);
                out.Number(geometry.screen.x); out.Number(geometry.screen.y);
                out.Number(geometry.viewport.left); out.Number(geometry.viewport.top);
                out.Number(geometry.viewport.right); out.Number(geometry.viewport.bottom); out.Number(geometry.layoutRevision);
            }
            out.Number(input.matchingPreCallProgramIndices.size());
            for (const auto index : input.matchingPreCallProgramIndices) out.Number(index);
        }
        out.Number(result.v4GroupAxisInput.has_value());
        if (result.v4GroupAxisInput)
        {
            const auto& input = *result.v4GroupAxisInput;
            out.Token(input.candidateId);
            const auto side = [&](const auto& value) {
                out.Token(value.childrenKind); out.Token(value.active); out.Token(value.hit);
                out.Number(value.valuesComplete); out.Number(value.children.size());
                for (const auto& child : value.children)
                {
                    out.Number(child.index); out.Token(child.id); out.Token(child.type);
                    out.OptionalNumber(child.idMatches); out.OptionalNumber(child.typeMatches);
                }
            };
            side(input.source); side(input.target);
            out.Number(input.actualTarget.has_value());
            if (input.actualTarget)
            {
                const auto& actual = *input.actualTarget;
                out.Number(actual.host); out.Number(actual.group); out.Number(actual.groupData); out.Number(actual.contentData);
                out.Number(actual.ownerThread); out.Wide(actual.groupId); out.Wide(actual.activeId); out.Number(actual.groupType);
                out.OptionalNumber(actual.activeIndex);
                const auto point = [&](const std::optional<POINT>& value) {
                    out.Number(value.has_value()); if (value) { out.Number(value->x); out.Number(value->y); }
                };
                point(actual.clientPoint); point(actual.screenPoint);
                out.Number(actual.viewport.left); out.Number(actual.viewport.top);
                out.Number(actual.viewport.right); out.Number(actual.viewport.bottom); out.Number(actual.layoutRevision);
                out.Number(actual.children.size());
                for (const auto& child : actual.children) { out.Wide(child.id); out.Number(child.widget); out.Number(child.type); }
            }
            out.OptionalNumber(input.targetChildCountMatches); out.OptionalNumber(input.targetActiveMatches);
            out.OptionalNumber(input.activeContentMatches);
            WriteGroupChildren(out, input.actualSource);
            out.OptionalNumber(input.sourceChildCountMatches); out.OptionalNumber(input.sourceActiveMatches);
            out.OptionalNumber(input.sourceActiveContentMatches);
            out.Number(input.sameHostBindingObserved); out.Number(input.resourceUnavailable); out.Number(input.observationUnavailable);
            out.Number(input.unresolved.size()); for (const auto& reason : input.unresolved) out.Token(reason);
        }
        out.Number(result.v4GroupHitActivation.has_value());
        if (result.v4GroupHitActivation)
        {
            const auto& observed = *result.v4GroupHitActivation;
            out.Number(observed.identityValid); out.Number(observed.hitAdapterBound); out.Number(observed.hitWindowScopeObserved);
            out.Number(observed.completionWindowBound); out.Number(observed.activationValuesObserved);
            out.Number(observed.observationUnavailable); out.Number(observed.resourceUnavailable);
            out.Number(observed.callsite); out.Number(observed.task); out.Number(observed.work);
            out.Number(observed.groupObject); out.Number(observed.containerObject); out.Number(observed.widgetObject);
            out.OptionalNumber(observed.groupKind); out.OptionalNumber(observed.hitDecision);
            out.OptionalNumber(observed.hitEqualsDestination); out.OptionalNumber(observed.completionSucceeded);
            out.OptionalNumber(observed.ownerIdUnchanged); out.OptionalNumber(observed.resolvedTypeMatchesHit);
            out.OptionalNumber(observed.activeAfterEqualsDestination);
            out.OptionalNumber(observed.hitTraceIndex); out.OptionalNumber(observed.completionTraceIndex);
            out.Number(observed.records.size());
            for (const auto& record : observed.records) { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(observed.unresolved.size()); for (const auto& reason : observed.unresolved) out.Token(reason);
        }
        out.Number(result.v4GroupHitBinding.has_value());
        if (result.v4GroupHitBinding)
        {
            const auto& binding = *result.v4GroupHitBinding;
            out.Number(binding.localReceiptObserved); out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.groupObjectMatches); out.OptionalNumber(binding.groupDataMatches);
            out.OptionalNumber(binding.groupKindMatches); out.OptionalNumber(binding.expectedHitMatches);
            out.OptionalNumber(binding.inactiveActivationAndSealedStateAgree);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4GroupBusinessBinding.has_value());
        if (result.v4GroupBusinessBinding)
        {
            const auto& binding = *result.v4GroupBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.pathFlowObserved); out.Number(binding.completionObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.sourcePreserved);
            out.OptionalNumber(binding.existingPreserved); out.OptionalNumber(binding.outputBytesMatch);
            out.OptionalNumber(binding.membershipMatches); out.OptionalNumber(binding.reloadMatches);
            out.OptionalNumber(binding.rootPreserved); out.OptionalNumber(binding.activePreserved); out.OptionalNumber(binding.inactivePreserved);
            out.OptionalNumber(binding.reloadedRootPreserved); out.OptionalNumber(binding.reloadedActivePreserved);
            out.OptionalNumber(binding.reloadedInactivePreserved); out.OptionalNumber(binding.noDesktopDuplicate);
            out.OptionalNumber(binding.mappedDirectoryPreserved); out.OptionalNumber(binding.sentinelPreserved);
            out.OptionalNumber(binding.readCompletionEntered); out.OptionalNumber(binding.fileCompletionEntered);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4GroupBusinessSealed); out.Number(result.v4GroupCopyBusiness.has_value());
        if (result.v4GroupCopyBusiness)
        {
            const auto& group = *result.v4GroupCopyBusiness;
            out.Token(group.candidateId); out.Number(group.fileGroup); out.Number(group.mapped); out.Number(group.targetInactive);
            out.Wide(group.source.wstring()); out.Wide(group.output.wstring());
            out.Wide(group.existing.wstring()); out.Wide(group.mappedDirectory.wstring()); out.Bytes(group.expectedBytes);
            out.FileId(group.sourceId); out.FileId(group.outputId); out.FileId(group.existingId);
            out.FileId(group.mappedDirectoryId); out.FileId(group.sentinelId);
            const auto paths = [&](const auto& values) { out.Number(values.size()); for (const auto& path : values) out.Wide(path.wstring()); };
            paths(group.initialMembers); paths(group.finalMembers); paths(group.reloadedMembers);
            paths(group.initialInactiveMembers); paths(group.finalInactiveMembers); paths(group.reloadedInactiveMembers);
            out.Number(group.completion.has_value());
            if (group.completion)
            { out.Number(static_cast<int>(group.completion->signal)); out.Number(group.completion->sourceEntered);
              out.Number(group.completion->commitEntered); out.Token(group.completion->detail); }
            out.OptionalNumber(group.initialRevision); out.OptionalNumber(group.finalRevision); out.OptionalNumber(group.reloadedRevision);
            out.OptionalNumber(group.sourcePreserved); out.OptionalNumber(group.existingPreserved); out.OptionalNumber(group.outputBytesMatch);
            out.OptionalNumber(group.membershipMatches); out.OptionalNumber(group.reloadedMembershipMatches);
            out.OptionalNumber(group.rootPreserved); out.OptionalNumber(group.activePreserved); out.OptionalNumber(group.inactivePreserved);
            out.OptionalNumber(group.reloadedRootPreserved); out.OptionalNumber(group.reloadedActivePreserved);
            out.OptionalNumber(group.reloadedInactivePreserved); out.OptionalNumber(group.noDesktopDuplicate);
            out.OptionalNumber(group.mappedDirectoryPreserved); out.OptionalNumber(group.sentinelPreserved);
            out.OptionalNumber(group.mappedParentVerifiedBeforeDrop); out.OptionalNumber(group.mappedDistinctFromSourceAndDesktop);
            out.OptionalNumber(group.inactiveMembershipMatches); out.OptionalNumber(group.reloadedInactiveMembershipMatches);
            out.OptionalNumber(group.activeMembersUnchanged); out.OptionalNumber(group.reloadedActiveMembersUnchanged);
            out.OptionalNumber(group.inactiveActivated); out.OptionalNumber(group.reloadedInactiveActivated);
            out.OptionalNumber(group.rootStateExcludingActivationMatches); out.OptionalNumber(group.reloadedRootStateExcludingActivationMatches);
            out.OptionalNumber(group.inactiveSettingsPreserved); out.OptionalNumber(group.reloadedInactiveSettingsPreserved);
            out.Number(group.inputObserved); out.Number(group.baselineRevalidated); out.Number(group.finalObserved); out.Number(group.reloadObserved);
            out.Number(group.unresolved.size()); for (const auto& reason : group.unresolved) out.Token(reason);
        }
        out.Number(result.v4RestrictedCopyBusinessVerdict.has_value());
        if (result.v4RestrictedCopyBusinessVerdict)
            out.Number(static_cast<int>(*result.v4RestrictedCopyBusinessVerdict));
        out.Number(result.v4SingleOwnedFileBusinessBinding.has_value());
        if (result.v4SingleOwnedFileBusinessBinding)
        {
            const auto& binding = *result.v4SingleOwnedFileBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.pathFlowObserved); out.Number(binding.completionObserved);
            out.Number(binding.trackedCopyObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.outputBytesMatch);
            out.OptionalNumber(binding.reloadMatches); out.OptionalNumber(binding.firstLandingMatches);
            out.OptionalNumber(binding.readCompletionEntered); out.OptionalNumber(binding.fileCompletionEntered);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4BatchBusinessSealed); out.Number(result.v4BatchCopyBusiness.has_value());
        if (result.v4BatchCopyBusiness)
        {
            const auto& batch = *result.v4BatchCopyBusiness;
            out.Number(batch.members.size());
            for (const auto& member : batch.members)
            {
                out.Wide(member.source.wstring()); out.Wide(member.output.wstring()); out.Bytes(member.expectedBytes);
                out.FileId(member.sourceId); out.FileId(member.outputId); out.Cell(member.outputCell); out.Cell(member.reloadedCell);
                out.Content(member.outputBytes); out.Content(member.reloadedBytes);
            }
            out.Number(batch.modelIndexByReadOrder.size()); for (const auto index : batch.modelIndexByReadOrder) out.Number(index);
            out.Number(batch.completion.has_value());
            if (batch.completion)
            { out.Number(static_cast<int>(batch.completion->signal)); out.Number(batch.completion->sourceEntered);
              out.Number(batch.completion->commitEntered); out.Token(batch.completion->detail); }
            out.OptionalNumber(batch.initialRevision); out.OptionalNumber(batch.committedRevision);
            out.OptionalNumber(batch.initialCount); out.OptionalNumber(batch.finalCount); out.OptionalNumber(batch.reloadedCount);
            out.OptionalNumber(batch.firstReceivedLandingMatched); out.Number(batch.sourcePayloadObserved);
            out.Number(batch.independentAssertionsCompleted); out.Number(batch.assertionsCompleted);
            out.Number(batch.baselineRevalidated); out.Number(batch.unresolved.size());
            for (const auto& reason : batch.unresolved) out.Token(reason);
        }
        out.Number(result.v4LeaveHostObservation.has_value());
        if (result.v4LeaveHostObservation)
        {
            const auto& host = *result.v4LeaveHostObservation;
            out.Number(host.immediateFinalizeObserved); out.Number(host.graceTimerFinalizeObserved);
            out.Number(host.observationUnavailable); out.Number(host.resourceUnavailable);
            out.Number(host.records.size());
            for (const auto& record : host.records) { out.Number(record.index); out.AppEvent(record.event); }
            out.Number(host.timerMessage.has_value()); if (host.timerMessage) out.WindowMessage(*host.timerMessage);
            out.Number(host.unresolved.size()); for (const auto& reason : host.unresolved) out.Token(reason);
        }
        out.Number(result.v4LeaveBusinessBinding.has_value());
        if (result.v4LeaveBusinessBinding)
        {
            const auto& binding = *result.v4LeaveBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.leaveReturnObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.sourcePreserved);
            out.OptionalNumber(binding.sentinelPreserved); out.OptionalNumber(binding.initialCellsPreserved);
            out.OptionalNumber(binding.layoutBytesUnchanged); out.OptionalNumber(binding.layoutFileIdentityUnchanged);
            out.OptionalNumber(binding.sourceLogAvailable);
            out.OptionalNumber(binding.outputCount); out.OptionalNumber(binding.finalItemCount);
            out.OptionalNumber(binding.starts); out.OptionalNumber(binding.ends); out.OptionalNumber(binding.fileReads);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4LeaveBusinessSealed); out.Number(result.v4LeaveBusiness.has_value());
        if (result.v4LeaveBusiness)
        {
            const auto& leave = *result.v4LeaveBusiness;
            out.Token(leave.candidateId); out.Wide(leave.source.wstring()); out.Wide(leave.sentinel.wstring());
            out.Bytes(leave.expectedBytes); out.FileId(leave.sourceId); out.FileId(leave.sentinelId);
            out.Token(leave.initialLayout); out.Token(leave.finalLayout);
            const auto writeLeaveLayout = [&](const auto& snapshot) {
                out.Number(snapshot.has_value()); if (!snapshot) return;
                out.Number(snapshot->captured); out.Number(snapshot->resourceUnavailable);
                out.Token(snapshot->candidateId); out.Wide(snapshot->path.wstring()); out.Token(snapshot->rawBytes);
                out.FileId(snapshot->fileId); out.OptionalNumber(snapshot->revision);
                out.Number(snapshot->unresolved.size()); for (const auto& reason : snapshot->unresolved) out.Token(reason);
            };
            writeLeaveLayout(leave.initialLayoutSnapshot); writeLeaveLayout(leave.beforeLeaveLayoutSnapshot);
            writeLeaveLayout(leave.finalLayoutSnapshot);
            out.OptionalNumber(leave.initialRevision); out.OptionalNumber(leave.finalRevision);
            out.OptionalNumber(leave.initialItemCount); out.OptionalNumber(leave.finalItemCount); out.OptionalNumber(leave.outputCount);
            out.OptionalNumber(leave.preLeaveStarts); out.OptionalNumber(leave.preLeaveEnds); out.OptionalNumber(leave.preLeaveFileReads);
            out.OptionalNumber(leave.preLeaveCallRecordCount);
            out.OptionalNumber(leave.starts); out.OptionalNumber(leave.ends); out.OptionalNumber(leave.fileReads);
            out.OptionalNumber(leave.sourcePreserved); out.OptionalNumber(leave.sentinelPreserved);
            out.OptionalNumber(leave.initialCellsPreserved); out.OptionalNumber(leave.layoutBytesUnchanged);
            out.OptionalNumber(leave.layoutFileIdentityUnchanged); out.OptionalNumber(leave.sourceLogAvailable);
            out.Number(leave.collectionTarget);
            const auto members = [&](const auto& values) { out.Number(values.size()); for (const auto& path : values) out.Wide(path.wstring()); };
            members(leave.initialCollectionMembers); members(leave.finalCollectionMembers); members(leave.reloadedCollectionMembers);
            out.OptionalNumber(leave.collectionWidgetPreserved); out.OptionalNumber(leave.collectionMembersPreserved);
            out.OptionalNumber(leave.reloadedCollectionMembersPreserved); out.OptionalNumber(leave.reloadedRevision);
            out.Number(leave.inputObserved); out.Number(leave.baselineRevalidated); out.Number(leave.leaveAwaitReturned); out.Number(leave.finalObserved);
            out.Number(leave.unresolved.size()); for (const auto& reason : leave.unresolved) out.Token(reason);
        }
        out.Number(result.v4RejectBusinessBinding.has_value());
        if (result.v4RejectBusinessBinding)
        {
            const auto& binding = *result.v4RejectBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.rejectReturnObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceSnapshotMatches); out.OptionalNumber(binding.sourcePreserved);
            out.OptionalNumber(binding.sentinelPreserved); out.OptionalNumber(binding.initialCellsPreserved);
            out.OptionalNumber(binding.layoutBytesUnchanged); out.OptionalNumber(binding.layoutFileIdentityUnchanged);
            out.OptionalNumber(binding.sourceLogAvailable);
            out.OptionalNumber(binding.outputCount); out.OptionalNumber(binding.finalItemCount);
            out.OptionalNumber(binding.starts); out.OptionalNumber(binding.ends); out.OptionalNumber(binding.fileReads);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4RejectBusinessSealed); out.Number(result.v4RejectBusiness.has_value());
        if (result.v4RejectBusiness)
        {
            const auto& reject = *result.v4RejectBusiness;
            out.Token(reject.candidateId); out.Wide(reject.source.wstring()); out.Wide(reject.sentinel.wstring());
            out.Bytes(reject.expectedBytes); out.FileId(reject.sourceId); out.FileId(reject.sentinelId);
            out.Token(reject.initialLayout); out.Token(reject.finalLayout);
            const auto writeRejectLayout = [&](const auto& snapshot) {
                out.Number(snapshot.has_value()); if (!snapshot) return;
                out.Number(snapshot->captured); out.Number(snapshot->resourceUnavailable);
                out.Token(snapshot->candidateId); out.Wide(snapshot->path.wstring()); out.Token(snapshot->rawBytes);
                out.FileId(snapshot->fileId); out.OptionalNumber(snapshot->revision);
                out.Number(snapshot->unresolved.size()); for (const auto& reason : snapshot->unresolved) out.Token(reason);
            };
            writeRejectLayout(reject.initialLayoutSnapshot); writeRejectLayout(reject.beforeDropLayoutSnapshot);
            writeRejectLayout(reject.finalLayoutSnapshot);
            out.OptionalNumber(reject.initialRevision); out.OptionalNumber(reject.finalRevision);
            out.OptionalNumber(reject.initialItemCount); out.OptionalNumber(reject.finalItemCount); out.OptionalNumber(reject.outputCount);
            out.OptionalNumber(reject.preDropStarts); out.OptionalNumber(reject.preDropEnds); out.OptionalNumber(reject.preDropFileReads);
            out.OptionalNumber(reject.preDropCallRecordCount);
            out.OptionalNumber(reject.starts); out.OptionalNumber(reject.ends); out.OptionalNumber(reject.fileReads);
            out.OptionalNumber(reject.sourcePreserved); out.OptionalNumber(reject.sentinelPreserved);
            out.OptionalNumber(reject.initialCellsPreserved); out.OptionalNumber(reject.layoutBytesUnchanged);
            out.OptionalNumber(reject.layoutFileIdentityUnchanged); out.OptionalNumber(reject.sourceLogAvailable);
            out.Number(reject.inputObserved); out.Number(reject.baselineRevalidated); out.Number(reject.rejectAwaitReturned); out.Number(reject.finalObserved);
            out.Number(reject.unresolved.size()); for (const auto& reason : reject.unresolved) out.Token(reason);
        }
        out.Number(result.v4DirectoryBusinessBinding.has_value());
        if (result.v4DirectoryBusinessBinding)
        {
            const auto& binding = *result.v4DirectoryBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.pathFlowObserved); out.Number(binding.completionObserved);
            out.Number(binding.observationUnavailable); out.Number(binding.resourceUnavailable);
            out.OptionalNumber(binding.sourceLeaseMatches); out.OptionalNumber(binding.landingMatched);
            out.OptionalNumber(binding.independentAssertionsCompleted); out.OptionalNumber(binding.reloadObserved);
            out.OptionalNumber(binding.readCompletionEntered); out.OptionalNumber(binding.fileCompletionEntered);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4DirectoryBusinessSealed); out.Number(result.v4DirectoryCopyBusiness.has_value());
        if (result.v4DirectoryCopyBusiness)
        {
            const auto& directory = *result.v4DirectoryCopyBusiness;
            out.Token(directory.candidateId); out.Wide(directory.source.wstring()); out.Wide(directory.output.wstring());
            out.FileId(directory.originalRootId); out.FileId(directory.sourceRootId); out.FileId(directory.outputRootId);
            out.Number(directory.members.size());
            for (const auto& member : directory.members)
            {
                out.Token(member.slot); out.Token(member.objectIdentity); out.Wide(member.relative.wstring());
                out.Number(member.directory); out.Bytes(member.expectedBytes);
                out.FileId(member.originalId); out.FileId(member.sourceId); out.FileId(member.outputId);
                out.Content(member.outputBytes); out.Content(member.reloadedBytes);
            }
            out.Number(directory.completion.has_value());
            if (directory.completion)
            { out.Number(static_cast<int>(directory.completion->signal)); out.Number(directory.completion->sourceEntered);
              out.Number(directory.completion->commitEntered); out.Token(directory.completion->detail); }
            out.Cell(directory.outputCell); out.Cell(directory.reloadedCell);
            out.OptionalNumber(directory.initialRevision); out.OptionalNumber(directory.committedRevision);
            out.OptionalNumber(directory.initialCount); out.OptionalNumber(directory.finalCount); out.OptionalNumber(directory.reloadedCount);
            out.OptionalNumber(directory.landingMatched); out.Number(directory.sourcePayloadObserved); out.Number(directory.baselineRevalidated);
            out.Number(directory.independentAssertionsCompleted); out.Number(directory.assertionsCompleted);
            out.Number(directory.localBusinessMismatch.has_value());
            if (directory.localBusinessMismatch) out.Token(*directory.localBusinessMismatch);
            out.Number(directory.unresolved.size()); for (const auto& reason : directory.unresolved) out.Token(reason);
        }
        out.Number(result.v4UnicodeRead.has_value());
        if (result.v4UnicodeRead)
        {
            const auto& read = *result.v4UnicodeRead;
            out.Number(read.LocalLogCaptured()); out.Number(read.ResourceOrObservationUnavailable());
            out.Number(read.Calls().size());
            for (const auto& call : read.Calls()) out.ContentCall(call);
        }
        out.Number(result.v4UnicodeCaller.has_value());
        if (result.v4UnicodeCaller)
        {
            const auto& caller = *result.v4UnicodeCaller;
            out.Number(caller.actualCallerReturned); out.Number(caller.uniqueProviderCallObserved);
            out.Number(caller.observationUnavailable); out.Number(caller.records.size());
            for (const auto& record : caller.records) { out.Number(record.index); out.AppEvent(record.event); }
            out.Number(caller.providerCall.has_value()); if (caller.providerCall) out.ContentCall(*caller.providerCall);
            out.Number(caller.unresolved.size()); for (const auto& reason : caller.unresolved) out.Token(reason);
        }
        out.Number(result.v4UnicodeProviderInterval.has_value());
        if (result.v4UnicodeProviderInterval)
        {
            const auto& provider = *result.v4UnicodeProviderInterval;
            out.Number(provider.localProviderIntervalObserved); out.Number(provider.observationUnavailable);
            out.Number(provider.providerRecords.size());
            for (const auto& record : provider.providerRecords) { out.Number(record.index); out.AppEvent(record.event); }
            out.Number(provider.unresolved.size()); for (const auto& reason : provider.unresolved) out.Token(reason);
        }
        out.Number(result.v4TextGraph.has_value());
        if (result.v4TextGraph)
        {
            const auto& graph = *result.v4TextGraph;
            out.Number(graph.identityValid); out.Number(graph.hypothesisObservationRecorded);
            out.Number(graph.hypothesisNotMet); out.Number(graph.observationUnavailable);
            out.Number(graph.events.size());
            for (const auto& event : graph.events)
            {
                out.Token(event.id); out.Number(event.selectedPresent); out.Number(static_cast<int>(event.state));
                out.OptionalNumber(event.logIndex); out.OptionalNumber(event.traceIndex);
            }
            out.Number(graph.orders.size());
            for (const auto& order : graph.orders)
            { out.Token(order.before); out.Token(order.after); out.Token(order.basis); out.Number(order.selectedOrderAgrees); }
            out.Number(graph.oleEvidence.size()); for (const auto& capture : graph.oleEvidence) out.OleCapture(capture);
            const auto records = [&](const auto& values) {
                out.Number(values.size());
                for (const auto& record : values) { out.Number(record.index); out.AppEvent(record.event); }
            };
            records(graph.callerEvidence); records(graph.providerEvidence);
            out.Number(graph.providerCall.has_value()); if (graph.providerCall) out.ContentCall(*graph.providerCall);
            out.Number(graph.unresolved.size()); for (const auto& reason : graph.unresolved) out.Token(reason);
        }
        out.Number(result.v4UnicodeConsumption.has_value());
        if (result.v4UnicodeConsumption)
        {
            const auto& consumption = *result.v4UnicodeConsumption;
            out.Number(consumption.localValuesCaptured); out.Number(consumption.observationUnavailable);
            out.Number(consumption.records.size());
            for (const auto& record : consumption.records)
            { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.actualBytes); }
            out.Number(consumption.unresolved.size()); for (const auto& reason : consumption.unresolved) out.Token(reason);
        }
        out.Number(result.v4UnicodeRefreshLink.has_value());
        if (result.v4UnicodeRefreshLink)
        {
            const auto& link = *result.v4UnicodeRefreshLink;
            out.Number(link.sameDropScopeAndOrderObserved); out.Number(link.observationUnavailable);
            out.OptionalNumber(link.requestedRevision);
            out.Number(link.unresolved.size()); for (const auto& reason : link.unresolved) out.Token(reason);
        }
        out.Number(result.v4RefreshRequest.has_value());
        if (result.v4RefreshRequest)
        {
            const auto& refresh = *result.v4RefreshRequest;
            out.Number(refresh.RequestMarkerCaptured());
            if (refresh.RequestMarkerCaptured())
            {
                out.Number(refresh.BeforeRevision()); out.Number(refresh.AfterRevision());
                out.Number(refresh.BeforeFlags()); out.Number(refresh.AfterFlags());
            }
            out.Number(refresh.ResourceOrObservationUnavailable());
            const auto requestRecord = [&](const auto& record) {
                out.Number(record.has_value());
                if (record) { out.Number(record->index); out.AppEvent(record->event); }
            };
            requestRecord(refresh.EnterRecord()); requestRecord(refresh.ReturnRecord());
        }
        out.Number(result.v4RefreshWorker.has_value());
        if (result.v4RefreshWorker)
        {
            const auto& worker = *result.v4RefreshWorker;
            out.Number(worker.LogScanComplete()); out.Number(worker.LocalRecordsCaptured()); out.Number(worker.ObservationUnavailable());
            if (worker.LocalRecordsCaptured())
            { out.Number(worker.Context()); out.Number(worker.State()); out.Number(worker.Serial()); out.Number(worker.Revision()); }
            out.Number(worker.AppRecords().size());
            for (const auto& record : worker.AppRecords())
            {
                out.Number(record.index); out.AppEvent(record.event);
            }
            out.Number(worker.WorkerRecords().size());
            for (const auto& record : worker.WorkerRecords())
            {
                const auto& e = record.event;
                out.Number(record.index); out.Number(static_cast<int>(e.phase)); out.Number(e.state); out.Number(e.context);
                out.Number(e.window); out.Number(e.serial); out.Number(e.thread); out.Number(e.message); out.Number(e.result);
            }
        }
        out.Number(result.v4TextRefresh.has_value());
        if (result.v4TextRefresh)
        {
            const auto& refresh = *result.v4TextRefresh;
            out.Number(refresh.sameBoundHost); out.Number(refresh.observationUnavailable);
            out.Number(refresh.beforeDropRevisionObserved); if (refresh.beforeDropRevisionObserved) out.Number(refresh.beforeDropRevision);
            out.Number(refresh.requestedRevisionObserved); if (refresh.requestedRevisionObserved) out.Number(refresh.requestedRevision);
            out.OptionalNumber(refresh.traceStart); out.OptionalNumber(refresh.traceEnd);
            out.OptionalNumber(refresh.refreshStart); out.OptionalNumber(refresh.refreshEnd);
            out.Number(refresh.entries.size());
            for (const auto& entry : refresh.entries)
            {
                const auto& r = entry.refresh; const auto& m = entry.message;
                out.Number(r.traceIndex); out.Number(r.beforeRevision); out.Number(r.afterRevision);
                out.Number(r.beforeRunning); out.Number(r.afterRunning); out.Number(r.beforeReady); out.Number(r.afterReady); out.Number(r.returned);
                out.Number(m.enteredSequence); out.Number(m.returnedSequence); out.Number(m.windowScope); out.Number(static_cast<int>(m.route));
                out.Number(reinterpret_cast<std::uintptr_t>(m.window)); out.Number(m.message); out.Number(m.wParam);
                out.Number(m.enteredThread); out.Number(m.returnedThread); out.Number(m.returned); out.Number(m.windowResult);
                out.Number(entry.nestedObserved);
            }
        }
        out.Number(result.v4TextBusiness.has_value());
        if (result.v4TextBusiness)
        {
            const auto& business = *result.v4TextBusiness;
            out.Bytes(business.expectedBytes); out.Number(business.outputEnumerationObserved); out.Number(business.outputs.size());
            for (const auto& path : business.outputs) out.Wide(path.wstring());
            out.FileId(business.sentinelId); out.FileId(business.outputId);
            out.Cell(business.initialSentinelCell); out.Cell(business.outputCell);
            out.Cell(business.reloadedSentinelCell); out.Cell(business.reloadedOutputCell);
            out.OptionalNumber(business.initialLayoutRevision); out.OptionalNumber(business.committedLayoutRevision);
            out.Content(business.outputBytes); out.Content(business.reloadedBytes);
            out.OptionalNumber(business.reloadedModelCount); out.Number(business.assertionsCompleted);
        }
        // This format is explicitly partial: it cannot certify complete graph,
        // request-to-worker attribution, persistence, or a product PASS.
        out.Token("full_execution_evidence_unimplemented");
        return std::move(out.bytes);
    }
    static std::string EncodeNativeEvidence(const ingress_adapter::BridgeResult& result,
        const v3_scheduler::CaseRecord& item, std::string_view executionId, std::size_t budget)
    {
        EvidenceWriter out{{}, budget};
        out.Token("v4_native_partial_observations_v43"); out.Token(executionId);
        out.Token(item.candidateId); out.Token(item.graphId); out.Token(item.presence);
        out.Number(result.productionStarted); out.Number(result.productionEntryUnknown);
        out.OptionalNumber(result.unattributedBusinessFailureObserved); out.OptionalNumber(result.unattributedEventOracleFailureObserved);
        WriteDiagnosticBudget(out, result.v4DiagnosticBudget);
        WriteBackgroundDiagnosticBudget(out, result.v4BackgroundDiagnosticBudget);
        out.Number(result.v4GroupAxisInput.has_value());
        if (result.v4GroupAxisInput)
        {
            const auto& input = *result.v4GroupAxisInput;
            out.Token(input.candidateId);
            const auto side = [&](const auto& value) {
                out.Token(value.childrenKind); out.Token(value.active); out.Token(value.hit);
                out.Number(value.valuesComplete); out.Number(value.children.size());
                for (const auto& child : value.children)
                { out.Number(child.index); out.Token(child.id); out.Token(child.type); out.OptionalNumber(child.idMatches); out.OptionalNumber(child.typeMatches); }
            };
            side(input.source); side(input.target);
            WriteGroupChildren(out, input.actualSource); WriteGroupChildren(out, input.actualTarget);
            out.OptionalNumber(input.sourceChildCountMatches); out.OptionalNumber(input.sourceActiveMatches);
            out.OptionalNumber(input.sourceActiveContentMatches); out.OptionalNumber(input.targetChildCountMatches);
            out.OptionalNumber(input.targetActiveMatches); out.OptionalNumber(input.activeContentMatches);
            out.Number(input.sameHostBindingObserved); out.Number(input.resourceUnavailable); out.Number(input.observationUnavailable);
            out.Number(input.unresolved.size()); for (const auto& reason : input.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeGroupMemberInitial != nullptr);
        if (result.v4NativeGroupMemberInitial)
        {
            const auto& initial = *result.v4NativeGroupMemberInitial;
            out.Token(initial.candidateId); out.Wide(initial.moving.wstring()); out.Wide(initial.sentinel.wstring());
            out.Bytes(initial.expectedBytes); out.FileId(initial.movingId); out.FileId(initial.sentinelId);
            out.Number(initial.preparationAttempted); out.Number(initial.prepared); out.Number(initial.resourceUnavailable);
            out.Number(initial.source.has_value());
            if (initial.source)
            {
                const auto& source = *initial.source;
                WriteGroupChildren(out, std::optional<behavior::Access::V4GroupChildrenSnapshot>{source.group});
                out.Number(source.container); out.Number(source.item); out.Number(source.childData);
                out.Wide(source.memberKey); out.Wide(source.itemPath); out.FileId(std::optional<FILE_ID_INFO>{source.fileId});
                out.Number(source.memberIndex);
                const auto rect = [&](const RECT& r) { out.Number(r.left); out.Number(r.top); out.Number(r.right); out.Number(r.bottom); };
                rect(source.memberBounds); rect(source.visible); rect(source.viewport);
                out.Number(source.client.x); out.Number(source.client.y); out.Number(source.screen.x); out.Number(source.screen.y);
            }
            out.Number(initial.unresolved.size()); for (const auto& reason : initial.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeMultiEvidence.has_value());
        if (result.v4NativeMultiEvidence) ingress_adapter::WriteV4NativeMultiEvidence(out, *result.v4NativeMultiEvidence);
        const auto writeCategoryGeometry = [&](const behavior::Access::NativeCategorySourceGeometry& geometry) {
            out.Number(geometry.host); out.Number(geometry.container); out.Number(geometry.ownerThread);
            out.Wide(geometry.activeCategory); out.Wide(geometry.memberKey); out.Number(geometry.visibleKeys.size());
            for (const auto& key : geometry.visibleKeys) out.Wide(key);
            out.Number(geometry.visibleIndex); out.Number(geometry.client.x); out.Number(geometry.client.y);
            out.Number(geometry.screen.x); out.Number(geometry.screen.y);
            out.Number(geometry.memberBounds.left); out.Number(geometry.memberBounds.top);
            out.Number(geometry.memberBounds.right); out.Number(geometry.memberBounds.bottom);
            out.Number(geometry.viewport.left); out.Number(geometry.viewport.top);
            out.Number(geometry.viewport.right); out.Number(geometry.viewport.bottom); out.Number(geometry.layoutRevision);
        };
        const auto writeCategoryState = [&](const behavior::Access::NativeCategorySourceState& state) {
            const auto& widget = state.widget;
            out.Number(state.host); out.Number(state.container); out.Number(state.ownerThread);
            out.Wide(widget.id); out.Number(static_cast<int>(widget.type)); out.Wide(widget.title); out.Wide(widget.customTitle);
            out.Cell(std::optional<GridCell>{widget.gridCell}); out.Number(widget.gridSpan.columns); out.Number(widget.gridSpan.rows);
            out.Number(widget.autoCollect); out.Number(widget.singleCellIconMode); out.Number(widget.titleBarCollapsed);
            out.Number(widget.showSearchBox); out.Number(widget.dateHeaders); out.Number(widget.showFileCategories);
            out.Number(widget.scrollContainerMode); out.Wide(widget.activeCategoryId);
            out.Number(widget.itemKeys.size()); for (const auto& key : widget.itemKeys) out.Wide(key);
            out.Wide(state.activeCategory); out.Number(state.visibleKeys.size()); for (const auto& key : state.visibleKeys) out.Wide(key);
            out.Number(state.layoutRevision);
        };
        const auto writeReorderGeometry = [&](const behavior::Access::NativeCollectionReorderGeometry& geometry) {
            out.Number(geometry.host); out.Number(geometry.container); out.Number(geometry.ownerThread);
            out.Wide(geometry.movingKey); out.Wide(geometry.targetKey); out.Number(geometry.itemKeys.size());
            for (const auto& key : geometry.itemKeys) out.Wide(key);
            out.Number(geometry.movingIndex); out.Number(geometry.targetIndex);
            const auto rect = [&](const RECT& value) { out.Number(value.left); out.Number(value.top); out.Number(value.right); out.Number(value.bottom); };
            rect(geometry.movingBounds); rect(geometry.targetBounds); rect(geometry.viewport);
            out.Number(geometry.movingClient.x); out.Number(geometry.movingClient.y); out.Number(geometry.targetClient.x); out.Number(geometry.targetClient.y);
            out.Number(geometry.movingScreen.x); out.Number(geometry.movingScreen.y); out.Number(geometry.targetScreen.x); out.Number(geometry.targetScreen.y);
            out.Number(geometry.layoutRevision);
        };
        out.Number(result.v4NativeReorderInitialInput.has_value());
        if (result.v4NativeReorderInitialInput)
        {
            const auto& input = *result.v4NativeReorderInitialInput;
            out.Token(input.candidateId); writeReorderGeometry(input.geometry); out.Number(input.sourceBaselineMatches);
            out.OptionalNumber(input.sourceAndReleasePointsMatch);
        }
        out.Number(result.v4NativeReorderPointProposal.has_value());
        if (result.v4NativeReorderPointProposal)
        {
            const auto& proposal = *result.v4NativeReorderPointProposal;
            out.Number(proposal.prepared); out.Number(proposal.revalidated); out.Number(proposal.generatedPointsUsed);
            out.Token(proposal.candidateId); out.Number(proposal.points.size());
            for (const auto& point : proposal.points) { out.Number(point.programIndex); out.Number(point.screen.x); out.Number(point.screen.y); }
            out.Number(proposal.unresolved.size()); for (const auto& reason : proposal.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCategoryInitialInput.has_value());
        if (result.v4NativeCategoryInitialInput)
        {
            const auto& input = *result.v4NativeCategoryInitialInput; const auto& landing = input.landing;
            out.Token(input.candidateId); writeCategoryGeometry(input.source); out.Cell(std::optional<GridCell>{landing.cell});
            out.Number(landing.clientCell.left); out.Number(landing.clientCell.top); out.Number(landing.clientCell.right); out.Number(landing.clientCell.bottom);
            out.Number(landing.screenPage.left); out.Number(landing.screenPage.top); out.Number(landing.screenPage.right); out.Number(landing.screenPage.bottom);
            out.Number(landing.screenCenter.x); out.Number(landing.screenCenter.y); out.Number(landing.columns); out.Number(landing.rows);
            out.Number(landing.layoutRevision); out.Number(input.landingMatches); out.OptionalNumber(input.sourceAndReleasePointsMatch);
            out.OptionalNumber(input.sourceBaselineMatches);
        }
        out.Number(result.v4NativeCategoryPointProposal.has_value());
        if (result.v4NativeCategoryPointProposal)
        {
            const auto& proposal = *result.v4NativeCategoryPointProposal;
            out.Number(proposal.prepared); out.Number(proposal.revalidated); out.Number(proposal.generatedPointsUsed);
            out.Token(proposal.candidateId); out.Number(proposal.points.size());
            for (const auto& point : proposal.points) { out.Number(point.programIndex); out.Number(point.screen.x); out.Number(point.screen.y); }
            out.Number(proposal.unresolved.size()); for (const auto& reason : proposal.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCollectionPointProposal.has_value());
        if (result.v4NativeCollectionPointProposal)
        {
            const auto& proposal = *result.v4NativeCollectionPointProposal;
            out.Number(proposal.prepared); out.Number(proposal.revalidated); out.Number(proposal.generatedPointsUsed);
            out.Token(proposal.candidateId); out.Number(proposal.points.size());
            for (const auto& point : proposal.points)
            { out.Number(point.programIndex); out.Number(point.screen.x); out.Number(point.screen.y); }
            out.Number(proposal.unresolved.size()); for (const auto& reason : proposal.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCollectionInitialInput.has_value());
        if (result.v4NativeCollectionInitialInput)
        {
            const auto& input = *result.v4NativeCollectionInitialInput; const auto& landing = input.landing;
            out.Token(input.candidateId); out.Number(input.host); out.Number(input.thread);
            out.Number(input.sourceScreen.x); out.Number(input.sourceScreen.y); out.Cell(std::optional<GridCell>{landing.cell});
            out.Number(landing.clientCell.left); out.Number(landing.clientCell.top);
            out.Number(landing.clientCell.right); out.Number(landing.clientCell.bottom);
            out.Number(landing.screenPage.left); out.Number(landing.screenPage.top);
            out.Number(landing.screenPage.right); out.Number(landing.screenPage.bottom);
            out.Number(landing.screenCenter.x); out.Number(landing.screenCenter.y);
            out.Number(landing.columns); out.Number(landing.rows); out.Number(landing.layoutRevision);
            out.Number(input.landingMatches); out.OptionalNumber(input.sourceAndReleasePointsMatch);
        }
        out.Number(result.v4NativeDesktopProposalRevalidated); out.Number(result.v4NativeDesktopUsedGeneratedPoints);
        out.Number(result.v4NativeDesktopPointProposal.has_value());
        if (result.v4NativeDesktopPointProposal)
        {
            const auto& proposal = *result.v4NativeDesktopPointProposal;
            out.Number(proposal.prepared); out.Number(proposal.resourceUnavailable); out.Token(proposal.candidateId);
            out.Number(proposal.source.x); out.Number(proposal.source.y); out.Number(proposal.target.x); out.Number(proposal.target.y);
            out.Number(proposal.screenPage.left); out.Number(proposal.screenPage.top);
            out.Number(proposal.screenPage.right); out.Number(proposal.screenPage.bottom); out.Number(proposal.layoutRevision);
            out.Number(proposal.points.size());
            for (const auto& point : proposal.points)
            { out.Number(point.programIndex); out.Number(point.screen.x); out.Number(point.screen.y); }
            out.Number(proposal.unresolved.size()); for (const auto& reason : proposal.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeDesktopGeometry.has_value());
        if (result.v4NativeDesktopGeometry)
        {
            const auto& geometry = *result.v4NativeDesktopGeometry;
            const auto rect = [&](const RECT& value) { out.Number(value.left); out.Number(value.top); out.Number(value.right); out.Number(value.bottom); };
            const auto cell = [&](const GridCell& value) { out.Wide(value.pageId); out.Number(value.column); out.Number(value.row); };
            out.Token(geometry.candidateId); out.Number(geometry.hostIdentity); out.Number(geometry.ownerThread);
            rect(geometry.source.clientBounds); out.Number(geometry.source.screenCenter.x); out.Number(geometry.source.screenCenter.y);
            cell(geometry.source.cell); out.Number(geometry.source.layoutRevision);
            cell(geometry.landing.cell); rect(geometry.landing.clientCell); rect(geometry.landing.screenPage);
            out.Number(geometry.landing.screenCenter.x); out.Number(geometry.landing.screenCenter.y);
            out.Number(geometry.landing.columns); out.Number(geometry.landing.rows); out.Number(geometry.landing.layoutRevision);
            out.Number(geometry.candidateMatched);
        }
        out.Number(result.v4NativeDesktopLayoutBinding.has_value());
        if (result.v4NativeDesktopLayoutBinding)
        {
            const auto& binding = *result.v4NativeDesktopLayoutBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.observationUnavailable);
            out.OptionalNumber(binding.pathMatches); out.OptionalNumber(binding.bytesMatch); out.OptionalNumber(binding.revisionMatches);
            out.FileId(binding.snapshotFileId); out.OptionalNumber(binding.snapshotRevision); out.OptionalNumber(binding.saveRevision);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeDesktopBusinessSealed); out.Number(result.v4NativeDesktopBusiness.has_value());
        if (result.v4NativeDesktopBusiness)
        {
            const auto& business = *result.v4NativeDesktopBusiness;
            out.Token(business.candidateId); out.Wide(business.source.wstring()); out.Bytes(business.expectedBytes);
            out.FileId(business.sourceId); out.Cell(business.initialCell); out.Cell(business.finalCell); out.Cell(business.reloadedCell);
            out.OptionalNumber(business.initialRevision); out.OptionalNumber(business.finalRevision); out.OptionalNumber(business.reloadedRevision);
            const auto members = [&](const auto& values) {
                out.Number(values.size()); for (const auto& value : values)
                { out.Wide(value.first.wstring()); out.Wide(value.second.pageId); out.Number(value.second.column); out.Number(value.second.row); }
            };
            members(business.initialMembers); members(business.finalMembers); members(business.reloadedMembers);
            out.OptionalNumber(business.targetMatched); out.OptionalNumber(business.membersPreserved);
            out.OptionalNumber(business.sourcePreserved); out.OptionalNumber(business.sentinelPreserved);
            out.Number(business.diskParsed); out.OptionalNumber(business.sourceDiskCellMatches);
            out.OptionalNumber(business.sentinelDiskCellMatches);
            out.Number(business.beforeReloadLayout.has_value());
            if (business.beforeReloadLayout)
            {
                const auto& snapshot = *business.beforeReloadLayout;
                out.Wide(snapshot.path.wstring()); out.Token(snapshot.rawBytes); out.FileId(snapshot.fileId);
                out.OptionalNumber(snapshot.revision); out.Number(snapshot.captured); out.Number(snapshot.resourceUnavailable);
                out.Number(snapshot.unresolved.size()); for (const auto& reason : snapshot.unresolved) out.Token(reason);
            }
            out.OptionalNumber(business.reloadMatched); out.Number(business.initialObserved); out.Number(business.finalObserved);
            out.Number(business.reloadObserved); out.Number(business.unresolved.size());
            for (const auto& reason : business.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeQueuedRecords.size());
        for (const auto& record : result.v4NativeQueuedRecords)
        {
            out.Number(static_cast<int>(record.phase)); out.OptionalNumber(record.programIndex);
            out.Number(record.token.ownerThread); out.Number(record.token.session); out.Number(record.token.event);
        }
        out.Number(result.v4NativeDeviceRecords.size());
        for (const auto& record : result.v4NativeDeviceRecords)
        {
            out.Number(static_cast<int>(record.phase)); out.OptionalNumber(record.programIndex);
            out.NativeState(record.state); out.DeliveryLog(record.delivery);
            out.Number(record.queued.has_value());
            if (record.queued) { out.Number(record.queued->ownerThread); out.Number(record.queued->session); out.Number(record.queued->event); }
            out.Number(record.expected.has_value());
            if (record.expected)
            {
                out.Number(record.expected->message); out.Number(record.expected->screen.x); out.Number(record.expected->screen.y);
                out.Number(record.expected->clientBeforeDispatch.x); out.Number(record.expected->clientBeforeDispatch.y);
                out.Number(record.expected->buttons);
            }
            out.Number(record.trace.size()); for (const auto& message : record.trace) out.WindowMessage(message);
        }
        out.Number(result.v4NativeTraceRecords.size());
        for (const auto& record : result.v4NativeTraceRecords)
        { out.Token(record.boundary); out.Number(record.messages.size()); for (const auto& message : record.messages) out.WindowMessage(message); }
        out.Number(result.v4NativeBeforeEndStates.size());
        for (const auto& state : result.v4NativeBeforeEndStates) out.NativeState(state);
        out.Number(result.v4NativeBeforeEndDelivery.size());
        for (const auto& log : result.v4NativeBeforeEndDelivery) out.DeliveryLog(log);
        out.Number(result.v4NativeGraph.has_value());
        if (result.v4NativeGraph)
        {
            const auto& graph = *result.v4NativeGraph;
            out.Number(graph.identityValid); out.Number(graph.observationUnavailable);
            out.Number(graph.hypothesisObservationRecorded); out.Number(graph.hypothesisNotMetObserved);
            out.Number(graph.nodes.size());
            for (const auto& node : graph.nodes)
            {
                out.Token(node.id); out.Number(node.selectedPresent); out.OptionalNumber(node.selectedClass);
                out.Number(node.observedLocal); out.Number(node.deviceIndex); out.Number(node.traceIndex); out.Number(node.sequence);
            }
            out.Number(graph.queuedRecords.size());
            for (const auto& record : graph.queuedRecords)
            { out.Number(static_cast<int>(record.phase)); out.OptionalNumber(record.programIndex);
              out.Number(record.token.ownerThread); out.Number(record.token.session); out.Number(record.token.event); }
            out.Number(graph.deviceRecords.size());
            for (const auto& record : graph.deviceRecords)
            {
                out.Number(static_cast<int>(record.phase)); out.OptionalNumber(record.programIndex);
                out.NativeState(record.state); out.DeliveryLog(record.delivery);
                out.Number(record.queued.has_value());
                if (record.queued) { out.Number(record.queued->ownerThread); out.Number(record.queued->session); out.Number(record.queued->event); }
                out.Number(record.expected.has_value());
                if (record.expected)
                { out.Number(record.expected->message); out.Number(record.expected->screen.x); out.Number(record.expected->screen.y);
                  out.Number(record.expected->clientBeforeDispatch.x); out.Number(record.expected->clientBeforeDispatch.y); out.Number(record.expected->buttons); }
                out.Number(record.trace.size()); for (const auto& message : record.trace) out.WindowMessage(message);
            }
            out.Number(graph.traceRecords.size());
            for (const auto& record : graph.traceRecords)
            { out.Token(record.boundary); out.Number(record.messages.size()); for (const auto& message : record.messages) out.WindowMessage(message); }
            out.Number(graph.unresolved.size()); for (const auto& reason : graph.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCleanup.has_value());
        if (result.v4NativeCleanup)
        {
            const auto& cleanup = *result.v4NativeCleanup;
            out.Number(cleanup.intervalObserved); out.Number(cleanup.observationUnavailable); out.Number(cleanup.matched.size());
            for (const auto& match : cleanup.matched)
            {
                out.Number(match.traceIndex); out.Number(static_cast<int>(match.begin.kind)); out.DeliveryIdentity(match.begin.identity);
                out.Number(reinterpret_cast<std::uintptr_t>(match.begin.window)); out.Number(match.begin.message);
                out.Number(match.begin.wParam); out.Number(match.begin.lParam); out.Number(match.begin.result);
            }
            out.Number(cleanup.unresolved.size());
            for (const auto& reason : cleanup.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeLayoutSave.has_value());
        if (result.v4NativeLayoutSave)
        {
            const auto& layout = *result.v4NativeLayoutSave;
            out.Number(layout.requestMarkerMatched); out.Number(layout.timerWindowReturned); out.Number(layout.timerMarkerBound);
            out.Number(layout.writerAndFinishMarked); out.Number(layout.writerInputsObserved); out.Number(layout.observationUnavailable);
            out.Number(layout.finishCompletionWindowReturned); out.Number(layout.finishSynchronousTimerReturned);
            out.OptionalNumber(layout.finishTraceIndex);
            out.OptionalNumber(layout.queueAccepted); out.OptionalNumber(layout.writeSucceeded);
            out.OptionalNumber(layout.finishReturnedSuccess); out.OptionalNumber(layout.finishRevisionMatched);
            out.OptionalNumber(layout.requestedRevision); out.OptionalNumber(layout.saveRevision);
            const auto index = [](std::size_t value) -> std::optional<std::size_t> {
                if (value == ole_driver::MessageTrace::InvalidToken) return std::nullopt;
                return value;
            };
            out.OptionalNumber(index(layout.releaseTraceIndex)); out.OptionalNumber(index(layout.timerTraceIndex));
            out.Number(layout.records.size());
            for (const auto& record : layout.records) { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(layout.unresolved.size()); for (const auto& reason : layout.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCategoryCalls.has_value());
        if (result.v4NativeCategoryCalls)
        {
            const auto& calls = *result.v4NativeCategoryCalls;
            out.Number(calls.identityValid); out.Number(calls.observationUnavailable); out.Number(calls.resourceUnavailable);
            out.Number(calls.records.size());
            for (const auto& record : calls.records) { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(calls.occurrences.size());
            for (const auto& occurrence : calls.occurrences)
            {
                out.Number(occurrence.callId); out.Number(occurrence.site); out.Number(occurrence.branch);
                out.Number(occurrence.widget); out.Number(occurrence.item); out.Number(occurrence.settings);
                out.Number(occurrence.enterIndex); out.Number(occurrence.returnIndex); out.Number(occurrence.leaveIndex);
                const std::optional<std::size_t> traceIndex = occurrence.traceIndex == ole_driver::MessageTrace::InvalidToken
                    ? std::nullopt : std::optional<std::size_t>(occurrence.traceIndex);
                out.OptionalNumber(traceIndex); out.Number(occurrence.windowScopedInternalReturnObserved);
                out.OptionalNumber(occurrence.ruleCountMatches); out.Number(occurrence.rules.size());
                for (const auto& rule : occurrence.rules)
                {
                    out.Number(rule.index); out.Number(rule.declarationIndex); out.Number(rule.idIndex); out.Number(rule.labelIndex);
                    out.Number(rule.extensionsIndex); out.Number(rule.enabledIndex);
                    out.OptionalNumber(rule.idMatches); out.OptionalNumber(rule.labelMatches);
                    out.OptionalNumber(rule.extensionsMatches); out.OptionalNumber(rule.enabledMatches);
                }
            }
            out.Number(calls.unresolved.size()); for (const auto& reason : calls.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCategoryDesktopCommit.has_value());
        if (result.v4NativeCategoryDesktopCommit)
        {
            const auto& commit = *result.v4NativeCategoryDesktopCommit;
            out.Number(commit.identityValid); out.Number(commit.releaseScopedCommitObserved);
            out.Number(commit.observationUnavailable); out.Number(commit.resourceUnavailable);
            out.Number(commit.commitId); out.Number(commit.origin);
            const std::optional<std::size_t> releaseIndex = commit.releaseTraceIndex == ole_driver::MessageTrace::InvalidToken
                ? std::nullopt : std::optional<std::size_t>(commit.releaseTraceIndex);
            out.OptionalNumber(releaseIndex); out.OptionalNumber(commit.applied); out.OptionalNumber(commit.layoutRequestInsideCommit);
            out.Number(commit.records.size());
            for (const auto& record : commit.records) { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(commit.unresolved.size()); for (const auto& reason : commit.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeReorderProduction.has_value());
        if (result.v4NativeReorderProduction)
        {
            const auto& observation = *result.v4NativeReorderProduction;
            out.Number(observation.identityValid); out.Number(observation.releaseScopedCommitObserved);
            out.Number(observation.observationUnavailable); out.Number(observation.resourceUnavailable); out.Number(observation.commitId);
            out.Number(observation.releaseTraceIndex); out.Number(observation.records.size());
            for (const auto& record : observation.records) { out.Number(record.index); out.AppEvent(record.event); out.Bytes(record.blob); }
            out.Number(observation.unresolved.size()); for (const auto& reason : observation.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeReorderCommitBinding.has_value());
        if (result.v4NativeReorderCommitBinding)
        {
            const auto& binding = *result.v4NativeReorderCommitBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.recordsComplete); out.Number(binding.observationUnavailable);
            out.OptionalNumber(binding.commitId); out.OptionalNumber(binding.containerMatches); out.OptionalNumber(binding.inputIndicesMatch);
            out.OptionalNumber(binding.beforeKeysMatch); out.OptionalNumber(binding.afterKeysMatch);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeReorderBusinessBinding.has_value());
        if (result.v4NativeReorderBusinessBinding)
        {
            const auto& binding = *result.v4NativeReorderBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.graphIdentityObserved); out.Number(binding.cleanupIntervalObserved);
            out.Number(binding.observationUnavailable); out.OptionalNumber(binding.sourceSnapshotMatches);
            out.OptionalNumber(binding.layoutPathMatches); out.OptionalNumber(binding.layoutBytesMatch); out.OptionalNumber(binding.layoutRevisionMatches);
            out.FileId(binding.snapshotFileId); out.OptionalNumber(binding.snapshotRevision); out.OptionalNumber(binding.saveRevision);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeReorderBusinessSealed); out.Number(result.v4NativeReorderBusiness.has_value());
        if (result.v4NativeReorderBusiness)
        {
            const auto& business = *result.v4NativeReorderBusiness;
            out.Token(business.candidateId); out.Wide(business.moving.wstring()); out.Wide(business.target.wstring());
            out.Bytes(business.expectedMoving); out.Bytes(business.expectedTarget); out.FileId(business.movingId); out.FileId(business.targetId);
            out.Number(business.initialGeometry.has_value()); if (business.initialGeometry) writeReorderGeometry(*business.initialGeometry);
            const auto snapshot = [&](const auto& value) {
                out.Number(value.itemKeys.size()); for (const auto& key : value.itemKeys) out.Wide(key);
                out.OptionalNumber(value.sourcePresent); out.OptionalNumber(value.manualOrder); out.OptionalNumber(value.expectedOrderObserved);
                out.Content(value.movingBytes); out.Content(value.targetBytes);
                out.OptionalNumber(value.movingIdentityPreserved); out.OptionalNumber(value.targetIdentityPreserved); out.OptionalNumber(value.layoutRevision);
            };
            snapshot(business.afterDrop); snapshot(business.afterReload);
            out.Number(business.beforeReloadLayout.has_value());
            if (business.beforeReloadLayout)
            {
                const auto& layout = *business.beforeReloadLayout;
                out.Wide(layout.path.wstring()); out.Token(layout.rawBytes); out.FileId(layout.fileId);
                out.OptionalNumber(layout.revision); out.Number(layout.captured); out.Number(layout.resourceUnavailable);
                out.Number(layout.unresolved.size()); for (const auto& reason : layout.unresolved) out.Token(reason);
            }
            out.Number(business.inputObserved); out.Number(business.baselineRevalidated); out.Number(business.captureObserved);
            out.Number(business.routeObserved); out.Number(business.finalObserved); out.Number(business.reloadObserved);
            out.Number(business.unresolved.size()); for (const auto& reason : business.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCategoryBusinessBinding.has_value());
        if (result.v4NativeCategoryBusinessBinding)
        {
            const auto& binding = *result.v4NativeCategoryBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.graphIdentityObserved); out.Number(binding.cleanupIntervalObserved);
            out.Number(binding.observationUnavailable); out.OptionalNumber(binding.sourceSnapshotMatches);
            out.OptionalNumber(binding.layoutPathMatches); out.OptionalNumber(binding.layoutBytesMatch); out.OptionalNumber(binding.layoutRevisionMatches);
            out.FileId(binding.snapshotFileId); out.OptionalNumber(binding.snapshotRevision); out.OptionalNumber(binding.saveRevision);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeCategoryBusinessSealed); out.Number(result.v4NativeCategoryBusiness.has_value());
        if (result.v4NativeCategoryBusiness)
        {
            const auto& business = *result.v4NativeCategoryBusiness;
            out.Token(business.candidateId); out.Wide(business.moving.wstring()); out.Wide(business.sentinel.wstring());
            out.Wide(business.movingKey); out.Wide(business.sentinelKey); out.Bytes(business.expectedMoving); out.Bytes(business.expectedSentinel);
            out.FileId(business.movingId); out.FileId(business.sentinelId);
            out.Number(business.initialGeometry.has_value()); if (business.initialGeometry) writeCategoryGeometry(*business.initialGeometry);
            out.Number(business.initialState.has_value()); if (business.initialState) writeCategoryState(*business.initialState);
            const auto snapshot = [&](const auto& value) {
                out.Number(value.source.has_value()); if (value.source) writeCategoryState(*value.source);
                out.OptionalNumber(value.freeCopies); out.Cell(value.movingCell); out.Content(value.movingBytes); out.Content(value.sentinelBytes);
                out.OptionalNumber(value.movingIdentityPreserved); out.OptionalNumber(value.sentinelIdentityPreserved);
            };
            snapshot(business.afterDrop); snapshot(business.afterReload);
            out.Number(business.beforeReloadLayout.has_value());
            if (business.beforeReloadLayout)
            {
                const auto& layout = *business.beforeReloadLayout;
                out.Wide(layout.path.wstring()); out.Token(layout.rawBytes); out.FileId(layout.fileId);
                out.OptionalNumber(layout.revision); out.Number(layout.captured); out.Number(layout.resourceUnavailable);
                out.Number(layout.unresolved.size()); for (const auto& reason : layout.unresolved) out.Token(reason);
            }
            out.Number(business.inputObserved); out.Number(business.baselineRevalidated); out.Number(business.captureObserved);
            out.Number(business.finalObserved); out.Number(business.reloadObserved);
            out.Number(business.unresolved.size()); for (const auto& reason : business.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeMemberBusinessBinding.has_value());
        if (result.v4NativeMemberBusinessBinding)
        {
            const auto& binding = *result.v4NativeMemberBusinessBinding;
            out.Number(binding.sameReceiptObserved); out.Number(binding.graphIdentityObserved); out.Number(binding.cleanupIntervalObserved);
            out.Number(binding.observationUnavailable); out.OptionalNumber(binding.sourceSnapshotMatches);
            out.OptionalNumber(binding.layoutPathMatches); out.OptionalNumber(binding.layoutBytesMatch); out.OptionalNumber(binding.layoutRevisionMatches);
            out.FileId(binding.snapshotFileId); out.OptionalNumber(binding.snapshotRevision); out.OptionalNumber(binding.saveRevision);
            out.Number(binding.unresolved.size()); for (const auto& reason : binding.unresolved) out.Token(reason);
        }
        out.Number(result.v4NativeMemberBusinessSealed); out.Number(result.v4NativeMemberBusiness.has_value());
        if (result.v4NativeMemberBusiness)
        {
            const auto& business = *result.v4NativeMemberBusiness;
            out.Wide(business.moving.wstring()); out.Wide(business.sentinel.wstring());
            out.Wide(business.movingKey); out.Wide(business.sentinelKey); out.Cell(std::optional<GridCell>{business.expectedLanding});
            out.Bytes(business.expectedMoving); out.Bytes(business.expectedSentinel);
            out.FileId(business.movingId); out.FileId(business.sentinelId);
            out.OptionalNumber(business.initialRevision); out.OptionalNumber(business.committedRevision);
            const auto snapshot = [&](const auto& value) {
                out.Number(value.sourceObservationAvailable); out.Number(value.sourcePresent); out.Number(value.sourceKeys.size());
                for (const auto& key : value.sourceKeys) out.Wide(key);
                out.OptionalNumber(value.freeCopies); out.Cell(value.landing);
                out.Content(value.movingBytes); out.Content(value.sentinelBytes);
            };
            snapshot(business.afterDrop); snapshot(business.afterReload);
            out.Number(business.captureObserved); out.Number(business.assertionsCompleted);
            out.Number(business.beforeReloadLayout != nullptr);
            if (business.beforeReloadLayout)
            {
                const auto& layout = *business.beforeReloadLayout;
                out.Wide(layout.path.wstring()); out.Token(layout.rawBytes); out.FileId(layout.fileId);
                out.OptionalNumber(layout.revision); out.Number(layout.captured); out.Number(layout.resourceUnavailable);
                out.Number(layout.unresolved.size()); for (const auto& reason : layout.unresolved) out.Token(reason);
            }
            out.Number(business.localBusinessMismatch.has_value());
            if (business.localBusinessMismatch) out.Token(*business.localBusinessMismatch);
            out.Number(business.unresolved.size()); for (const auto& reason : business.unresolved) out.Token(reason);
        }
        out.Token("full_execution_evidence_unimplemented");
        return std::move(out.bytes);
    }
public:
    static std::unique_ptr<v3_scheduler::ScheduledV4Outcome> Run(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget)
    {
        if (!attempt.TryBeginExecution(v3_scheduler::FamilyVersion::v4)) return {};
        const auto end = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(end)> guard(&attempt, end);
        bool consumed = false, entered = false;
        try
        {
            auto result = RunBridge(preState, familyIndex, caseIndex, attempt, executionId, coordinateBudget,
                axisBudget, replayWorkBudget, checkpointByteBudget, consumed, entered);
            if (!consumed) return {};
            v3_scheduler::OutcomeFacts facts;
            facts.executionId = std::string(executionId);
            try
            {
                facts.evidenceBytes = EncodeEvidence(result, preState.families.at(familyIndex).cases.at(caseIndex),
                    executionId, checkpointByteBudget);
                facts.evidenceVersion = "v4_attempt_partial_observations_v132";
            }
            catch (...)
            {
                result.observationUnavailable = true;
                result.obligations.emplace_back("v4_evidence_encoding_unavailable");
            }
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            facts.failure = std::move(result.failure);
            facts.receiptConsumed = consumed; facts.bridgeEntered = entered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.businessFailureObserved = result.businessFailureObserved;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.eventDispatchObservationRecorded = result.eventDispatchObservationRecorded;
            facts.eventDispatchFailureObserved = result.eventDispatchFailureObserved;
            facts.hypothesisObservationRecorded = result.v4TextGraph && result.v4TextGraph->identityValid &&
                result.v4TextGraph->hypothesisObservationRecorded;
            facts.hypothesisNotMetObserved = facts.hypothesisObservationRecorded && result.hypothesisNotMetObserved;
            facts.observationUnavailable = result.observationUnavailable;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            facts.obligations.emplace_back("v4_detailed_execution_evidence_persistence_unimplemented");
            RecordDiagnosticCapacity(facts, result.v4DiagnosticBudget, result.v4BackgroundDiagnosticBudget);
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV4Outcome>(
                new v3_scheduler::ScheduledV4Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; }
    }
    static std::unique_ptr<v3_scheduler::ScheduledV4Outcome> RunFile(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget,
        behavior::OwnedProfileDesktopRoot& root, const V4SingleFileFixture& fixture,
        HANDLE sourceRoot, const FILE_ID_INFO& sourceRootId, GridCell landing,
        const std::vector<ole_driver::Input>& inputs)
    {
        if (!attempt.TryBeginExecution(v3_scheduler::FamilyVersion::v4)) return {};
        const auto end = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(end)> guard(&attempt, end);
        bool consumed = false, entered = false;
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size()) return {};
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || replay.unexecutedRecord->kind != "graph_hypothesis" ||
                !replay.unexecutedRecord->coordinate || !replay.unexecutedRecord->graph || replay.unexecutedRecord->nativeGraph)
                return {};
            const auto& record = *replay.unexecutedRecord;
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate ||
                decoded.family->a.kind != V4SourceKind::file_only) return {};
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready) return {};
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.resourceUnavailable || raw.id != record.candidateId) return {};
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget)) return {};
            consumed = true; entered = true;
            result = ingress_adapter::PrepareV4DesktopSingleFileCopy(identity, verified, record.graph->present,
                record.graph->graph, root, fixture, sourceRoot, sourceRootId, std::move(landing), inputs);
        }
        catch (const std::exception& error)
        { result.observationUnavailable = true; result.productionEntryUnknown = entered; result.failure = error.what(); }
        catch (...) { result.observationUnavailable = true; result.productionEntryUnknown = entered; }
        if (!consumed) return {};
        try
        {
            v3_scheduler::OutcomeFacts facts; facts.executionId = std::string(executionId);
            try
            {
                facts.evidenceBytes = EncodeEvidence(result, preState.families.at(familyIndex).cases.at(caseIndex),
                    executionId, checkpointByteBudget);
                facts.evidenceVersion = "v4_attempt_partial_observations_v132";
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_file_evidence_encoding_unavailable"); }
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            facts.failure = std::move(result.failure); facts.receiptConsumed = consumed; facts.bridgeEntered = entered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.eventDispatchObservationRecorded = result.eventDispatchObservationRecorded;
            facts.eventDispatchFailureObserved = result.eventDispatchFailureObserved;
            facts.businessFailureObserved = result.businessFailureObserved; facts.observationUnavailable = result.observationUnavailable;
            facts.hypothesisObservationRecorded = result.v4FileGraph && result.v4FileGraph->identityValid &&
                result.v4FileGraph->hypothesisObservationRecorded;
            facts.hypothesisNotMetObserved = facts.hypothesisObservationRecorded && result.hypothesisNotMetObserved;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            facts.obligations.emplace_back("v4_file_full_execution_evidence_unimplemented");
            RecordDiagnosticCapacity(facts, result.v4DiagnosticBudget, result.v4BackgroundDiagnosticBudget);
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV4Outcome>(
                new v3_scheduler::ScheduledV4Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; }
    }
    static std::unique_ptr<v3_scheduler::ScheduledV4Outcome> RunDirectory(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget,
        behavior::OwnedProfileDesktopRoot& root, const directory_fixture::FixtureObservation& fixture,
        const std::shared_ptr<directory_fixture::DirectoryCopyLease>& lease, GridCell landing,
        const std::vector<ole_driver::Input>& inputs)
    {
        if (!attempt.TryBeginExecution(v3_scheduler::FamilyVersion::v4)) return {};
        const auto end = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(end)> guard(&attempt, end);
        bool consumed = false, entered = false;
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size()) return {};
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || replay.unexecutedRecord->kind != "graph_hypothesis" ||
                !replay.unexecutedRecord->coordinate || !replay.unexecutedRecord->graph || replay.unexecutedRecord->nativeGraph)
                return {};
            const auto& record = *replay.unexecutedRecord;
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate ||
                decoded.family->a.kind != V4SourceKind::file_only || decoded.family->a.fileCount.kind != V3CountKind::exact ||
                decoded.family->a.fileCount.decimal != "1") return {};
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
                Value(verified.Axes(), verified.Coordinate(), "V4.file.member0_kind") != "directory") return {};
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.resourceUnavailable || raw.id != record.candidateId) return {};
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget)) return {};
            consumed = true; entered = true;
            result = ingress_adapter::PrepareV4DesktopDirectoryCopy(identity, verified, *decoded.family, record.graph->present,
                record.graph->graph, root, fixture, lease, std::move(landing), inputs);
        }
        catch (const std::exception& error)
        { result.observationUnavailable = true; result.productionEntryUnknown = entered; result.failure = error.what(); }
        catch (...) { result.observationUnavailable = true; result.productionEntryUnknown = entered; }
        if (!consumed) return {};
        try
        {
            v3_scheduler::OutcomeFacts facts; facts.executionId = std::string(executionId);
            try
            {
                facts.evidenceBytes = EncodeEvidence(result, preState.families.at(familyIndex).cases.at(caseIndex),
                    executionId, checkpointByteBudget);
                facts.evidenceVersion = "v4_attempt_partial_observations_v132";
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_directory_evidence_encoding_unavailable"); }
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            facts.failure = std::move(result.failure); facts.receiptConsumed = consumed; facts.bridgeEntered = entered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.eventDispatchObservationRecorded = result.eventDispatchObservationRecorded;
            facts.eventDispatchFailureObserved = result.eventDispatchFailureObserved;
            facts.businessFailureObserved = result.businessFailureObserved; facts.observationUnavailable = result.observationUnavailable;
            facts.hypothesisObservationRecorded = result.v4DirectoryGraph && result.v4DirectoryGraph->identityValid &&
                result.v4DirectoryGraph->hypothesisObservationRecorded;
            facts.hypothesisNotMetObserved = facts.hypothesisObservationRecorded && result.hypothesisNotMetObserved;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            facts.obligations.emplace_back("v4_directory_full_execution_evidence_unimplemented");
            RecordDiagnosticCapacity(facts, result.v4DiagnosticBudget, result.v4BackgroundDiagnosticBudget);
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV4Outcome>(
                new v3_scheduler::ScheduledV4Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; }
    }
    static std::unique_ptr<v3_scheduler::ScheduledV4Outcome> RunBatch(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget,
        behavior::OwnedProfileDesktopRoot& root, const std::shared_ptr<V4IndependentFilePayload>& payload,
        const V4IndependentShellSource& binding, const std::shared_ptr<V4IndependentShellOwner>& owner,
        GridCell landing, const std::vector<ole_driver::Input>& inputs,
        const std::shared_ptr<const V4PinnedCopyLease>& pinnedCopyLease,
        const std::shared_ptr<const V4SharedMoveLease>& sharedMoveLease)
    {
        if (!attempt.TryBeginExecution(v3_scheduler::FamilyVersion::v4)) return {};
        const auto end = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(end)> guard(&attempt, end);
        bool consumed = false, entered = false;
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size()) return {};
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || replay.unexecutedRecord->kind != "graph_hypothesis" ||
                !replay.unexecutedRecord->coordinate || !replay.unexecutedRecord->graph || replay.unexecutedRecord->nativeGraph)
                return {};
            const auto& record = *replay.unexecutedRecord;
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate ||
                decoded.family->a.kind != V4SourceKind::file_only || decoded.family->a.fileCount.kind != V3CountKind::exact ||
                decoded.family->a.fileCount.decimal == "0") return {};
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready) return {};
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.resourceUnavailable || raw.id != record.candidateId) return {};
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget)) return {};
            consumed = true; entered = true;
            if (decoded.family->a.fileCount.decimal == "1" &&
                Value(verified.Axes(), verified.Coordinate(), "G04.termination") == "drop" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.release") == "none" &&
                Value(verified.Axes(), verified.Coordinate(), "T03") == "copy_move" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E01") == "free_desktop" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "none")
                result = ingress_adapter::PrepareV4DesktopSingleFileMoveCapable(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, sharedMoveLease);
            else if (decoded.family->a.fileCount.decimal == "1" &&
                Value(verified.Axes(), verified.Coordinate(), "G04.termination") == "leave" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E01") == "free_desktop" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "none")
                result = ingress_adapter::PrepareV4DesktopSingleFileLeave(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, pinnedCopyLease);
            else if (decoded.family->a.fileCount.decimal == "1" &&
                Value(verified.Axes(), verified.Coordinate(), "G04.termination") == "drop" &&
                Value(verified.Axes(), verified.Coordinate(), "T03") == "none" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E01") == "free_desktop" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "none")
                result = ingress_adapter::PrepareV4DesktopSingleFileReject(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, pinnedCopyLease);
            else if (decoded.family->a.fileCount.decimal == "1" &&
                Value(verified.Axes(), verified.Coordinate(), "G04.termination") == "leave" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "collection")
                result = ingress_adapter::PrepareV4CollectionSingleFileLeave(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, pinnedCopyLease);
            else if (decoded.family->a.fileCount.decimal == "1" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "collection")
                result = ingress_adapter::PrepareV4CollectionSingleFileCopy(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, pinnedCopyLease);
            else if (decoded.family->a.fileCount.decimal == "1" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "desktop_files")
                result = ingress_adapter::PrepareV4DesktopFilesSingleFileCopy(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, pinnedCopyLease);
            else if (decoded.family->a.fileCount.decimal == "1" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "folder_mapping")
                result = ingress_adapter::PrepareV4FolderMappingSingleFileCopy(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, pinnedCopyLease);
            else if (decoded.family->a.fileCount.decimal == "1" &&
                (Value(verified.Axes(), verified.Coordinate(), "target.E02") == "collection_group" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E02") == "file_group"))
                result = ingress_adapter::PrepareV4GroupSingleFileCopy(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, inputs, pinnedCopyLease);
            else if (Less("1", decoded.family->a.fileCount.decimal) ||
                (decoded.family->a.fileCount.decimal == "1" &&
                 Value(verified.Axes(), verified.Coordinate(), "target.E01") == "free_desktop" &&
                 Value(verified.Axes(), verified.Coordinate(), "target.E02") == "none"))
                result = ingress_adapter::PrepareV4DesktopBatchCopy(identity, verified, record.graph->present,
                    record.graph->graph, root, payload, binding, owner, std::move(landing), inputs, pinnedCopyLease);
            else
                result.obligations.emplace_back("v4_batch_single_target_unimplemented");
        }
        catch (const std::exception& error)
        { result.observationUnavailable = true; result.productionEntryUnknown = entered; result.failure = error.what(); }
        catch (...) { result.observationUnavailable = true; result.productionEntryUnknown = entered; }
        if (!consumed) return {};
        try
        {
            v3_scheduler::OutcomeFacts facts; facts.executionId = std::string(executionId);
            try
            {
                facts.evidenceBytes = EncodeEvidence(result, preState.families.at(familyIndex).cases.at(caseIndex),
                    executionId, checkpointByteBudget);
                facts.evidenceVersion = "v4_attempt_partial_observations_v132";
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_batch_evidence_encoding_unavailable"); }
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            facts.failure = std::move(result.failure); facts.receiptConsumed = consumed; facts.bridgeEntered = entered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.eventDispatchObservationRecorded = result.eventDispatchObservationRecorded;
            facts.eventDispatchFailureObserved = result.eventDispatchFailureObserved;
            facts.businessFailureObserved = result.businessFailureObserved; facts.observationUnavailable = result.observationUnavailable;
            facts.hypothesisObservationRecorded = result.v4FileGraph && result.v4FileGraph->identityValid &&
                result.v4FileGraph->hypothesisObservationRecorded;
            facts.hypothesisNotMetObserved = facts.hypothesisObservationRecorded && result.hypothesisNotMetObserved;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            facts.obligations.emplace_back("v4_batch_full_execution_evidence_unimplemented");
            RecordDiagnosticCapacity(facts, result.v4DiagnosticBudget, result.v4BackgroundDiagnosticBudget);
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV4Outcome>(
                new v3_scheduler::ScheduledV4Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; }
    }
    static std::unique_ptr<v3_scheduler::ScheduledV4Outcome> RunMixed(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget,
        behavior::OwnedProfileDesktopRoot& root, const std::shared_ptr<V4IndependentFilePayload>& payload,
        const V4IndependentShellSource& fileBinding, const std::shared_ptr<V4IndependentShellOwner>& fileOwner,
        GridCell landing, const std::vector<ole_driver::Input>& inputs,
        std::size_t contentByteBudget, std::size_t virtualBudget)
    {
        if (!attempt.TryBeginExecution(v3_scheduler::FamilyVersion::v4)) return {};
        const auto end = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(end)> guard(&attempt, end);
        bool consumed = false, entered = false;
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size()) return {};
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || replay.unexecutedRecord->kind != "graph_hypothesis" ||
                !replay.unexecutedRecord->coordinate || !replay.unexecutedRecord->graph || replay.unexecutedRecord->nativeGraph)
                return {};
            const auto& record = *replay.unexecutedRecord;
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate ||
                decoded.family->a.kind != V4SourceKind::file_and_content || decoded.family->a.fileCount.kind != V3CountKind::exact ||
                decoded.family->a.fileCount.decimal == "0") return {};
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready) return {};
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.resourceUnavailable || raw.id != record.candidateId) return {};
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget)) return {};
            consumed = true; entered = true;
            result = ingress_adapter::PrepareV4MixedFileCopy(identity, verified, record.graph->present,
                record.graph->graph, root, payload, fileBinding, fileOwner, std::move(landing), inputs,
                contentByteBudget, virtualBudget);
        }
        catch (const std::exception& error)
        { result.observationUnavailable = true; result.productionEntryUnknown = entered; result.failure = error.what(); }
        catch (...) { result.observationUnavailable = true; result.productionEntryUnknown = entered; }
        if (!consumed) return {};
        try
        {
            v3_scheduler::OutcomeFacts facts; facts.executionId = std::string(executionId);
            try
            {
                facts.evidenceBytes = EncodeEvidence(result, preState.families.at(familyIndex).cases.at(caseIndex),
                    executionId, checkpointByteBudget);
                facts.evidenceVersion = "v4_attempt_partial_observations_v132";
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_mixed_evidence_encoding_unavailable"); }
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            if (result.v4MixedInput && result.v4MixedInput->resourceUnavailable &&
                !result.productionStarted && !result.productionEntryUnknown)
                facts.status = "resource_unavailable";
            facts.failure = std::move(result.failure); facts.receiptConsumed = consumed; facts.bridgeEntered = entered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.eventDispatchObservationRecorded = result.eventDispatchObservationRecorded;
            facts.eventDispatchFailureObserved = result.eventDispatchFailureObserved;
            facts.businessFailureObserved = result.businessFailureObserved; facts.observationUnavailable = result.observationUnavailable;
            facts.hypothesisObservationRecorded = result.v4MixedGraph && result.v4MixedGraph->identityValid &&
                result.v4MixedGraph->hypothesisObservationRecorded;
            facts.hypothesisNotMetObserved = facts.hypothesisObservationRecorded && result.hypothesisNotMetObserved;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            facts.obligations.emplace_back("v4_mixed_full_execution_evidence_unimplemented");
            RecordDiagnosticCapacity(facts, result.v4DiagnosticBudget, result.v4BackgroundDiagnosticBudget);
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV4Outcome>(
                new v3_scheduler::ScheduledV4Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; }
    }
    static std::unique_ptr<v3_scheduler::ScheduledV4Outcome> RunVirtual(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget,
        behavior::OwnedProfileDesktopRoot& root, GridCell landing, const std::vector<ole_driver::Input>& inputs,
        std::size_t contentByteBudget, std::size_t virtualBudget)
    {
        if (!attempt.TryBeginExecution(v3_scheduler::FamilyVersion::v4)) return {};
        const auto end = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(end)> guard(&attempt, end);
        bool consumed = false, entered = false;
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size()) return {};
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || replay.unexecutedRecord->kind != "graph_hypothesis" ||
                !replay.unexecutedRecord->coordinate || !replay.unexecutedRecord->graph || replay.unexecutedRecord->nativeGraph)
                return {};
            const auto& record = *replay.unexecutedRecord;
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate ||
                decoded.family->a.kind != V4SourceKind::content_only) return {};
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
                Value(verified.Axes(), verified.Coordinate(), "T02.virtual_files") != "yes") return {};
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.resourceUnavailable || raw.id != record.candidateId) return {};
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget)) return {};
            consumed = true; entered = true;
            result = ingress_adapter::PrepareV4VirtualCopy(identity, verified, record.graph->present, record.graph->graph,
                root, std::move(landing), inputs, contentByteBudget, virtualBudget);
        }
        catch (const std::exception& error)
        { result.observationUnavailable = true; result.productionEntryUnknown = entered; result.failure = error.what(); }
        catch (...) { result.observationUnavailable = true; result.productionEntryUnknown = entered; }
        if (!consumed) return {};
        try
        {
            v3_scheduler::OutcomeFacts facts; facts.executionId = std::string(executionId);
            try
            {
                facts.evidenceBytes = EncodeEvidence(result, preState.families.at(familyIndex).cases.at(caseIndex),
                    executionId, checkpointByteBudget);
                facts.evidenceVersion = "v4_attempt_partial_observations_v132";
            }
            catch (...) { result.observationUnavailable = true; result.obligations.emplace_back("v4_virtual_evidence_encoding_unavailable"); }
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            if (!result.productionStarted && !result.productionEntryUnknown &&
                ((result.v4VirtualManifest && result.v4VirtualManifest->resourceUnavailable) ||
                 (result.v4ConfiguredInput && result.v4ConfiguredInput->resourceUnavailable))) facts.status = "resource_unavailable";
            facts.failure = std::move(result.failure); facts.receiptConsumed = consumed; facts.bridgeEntered = entered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.eventDispatchObservationRecorded = result.eventDispatchObservationRecorded;
            facts.eventDispatchFailureObserved = result.eventDispatchFailureObserved;
            facts.businessFailureObserved = result.businessFailureObserved; facts.observationUnavailable = result.observationUnavailable;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            facts.obligations.emplace_back("v4_virtual_full_execution_evidence_unimplemented");
            RecordDiagnosticCapacity(facts, result.v4DiagnosticBudget, result.v4BackgroundDiagnosticBudget);
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV4Outcome>(
                new v3_scheduler::ScheduledV4Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; }
    }
    static std::unique_ptr<v3_scheduler::ScheduledV4Outcome> RunNative(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget,
        behavior::OwnedProfileDesktopRoot& root, const V4SingleFileFixture& fixture,
        GridCell landing, POINT targetScreen, const std::vector<ole_driver::NativePointInput>& points)
    {
        if (!attempt.TryBeginExecution(v3_scheduler::FamilyVersion::v4)) return {};
        const auto end = [](v3_scheduler::InFlightReceipt* active) noexcept { active->EndExecution(); };
        std::unique_ptr<v3_scheduler::InFlightReceipt, decltype(end)> guard(&attempt, end);
        bool consumed = false, entered = false;
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size()) return {};
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || replay.unexecutedRecord->kind != "graph_hypothesis" ||
                !replay.unexecutedRecord->coordinate || !replay.unexecutedRecord->nativeGraph || replay.unexecutedRecord->graph)
                return {};
            const auto& record = *replay.unexecutedRecord;
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate) return {};
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready) return {};
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.resourceUnavailable || raw.id != record.candidateId) return {};
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget)) return {};
            consumed = true; entered = true;
            if (Value(verified.Axes(), verified.Coordinate(), "source.E02") == "none")
                result = ingress_adapter::PrepareV4NativeDesktopMove(identity, verified, record.nativeGraph->present,
                    record.nativeGraph->graph, root, fixture, std::move(landing), targetScreen, points);
            else if (Value(verified.Axes(), verified.Coordinate(), "source.E02") == "collection" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "collection")
                result = ingress_adapter::PrepareV4NativeCollectionReorder(identity, verified, record.nativeGraph->present,
                    record.nativeGraph->graph, root, fixture, points);
            else if (Value(verified.Axes(), verified.Coordinate(), "source.E02") == "collection")
                result = ingress_adapter::PrepareV4NativeCollectionMove(identity, verified, record.nativeGraph->present,
                    record.nativeGraph->graph, root, fixture, std::move(landing), targetScreen, points);
            else if (Value(verified.Axes(), verified.Coordinate(), "source.E02") == "desktop_files")
                result = ingress_adapter::PrepareV4NativeCategoryMove(identity, verified, record.nativeGraph->present,
                    record.nativeGraph->graph, root, fixture, std::move(landing), targetScreen, points);
            else result.obligations.emplace_back("v4_native_source_unimplemented");
        }
        catch (const std::exception& error)
        { result.observationUnavailable = true; result.productionEntryUnknown = entered; result.failure = error.what(); }
        catch (...)
        { result.observationUnavailable = true; result.productionEntryUnknown = entered; }
        if (!consumed) return {};
        try
        {
            v3_scheduler::OutcomeFacts facts;
            facts.executionId = std::string(executionId);
            try
            {
                facts.evidenceBytes = EncodeNativeEvidence(result, preState.families.at(familyIndex).cases.at(caseIndex),
                    executionId, checkpointByteBudget);
                facts.evidenceVersion = "v4_native_partial_observations_v43";
            }
            catch (...)
            {
                result.observationUnavailable = true;
                result.obligations.emplace_back("v4_native_evidence_encoding_unavailable");
            }
            switch (result.status)
            {
            case ingress_adapter::BridgeStatus::unimplemented: facts.status = "unimplemented"; break;
            case ingress_adapter::BridgeStatus::production_failed: facts.status = "production_failed"; break;
            case ingress_adapter::BridgeStatus::production_observed: facts.status = "production_observed"; break;
            }
            facts.failure = std::move(result.failure); facts.receiptConsumed = consumed; facts.bridgeEntered = entered;
            facts.productionStarted = result.productionStarted; facts.productionEntryUnknown = result.productionEntryUnknown;
            facts.eventOracleFailureObserved = result.eventOracleFailureObserved;
            facts.businessFailureObserved = result.businessFailureObserved; facts.observationUnavailable = result.observationUnavailable;
            facts.hypothesisObservationRecorded = result.v4NativeGraph && result.v4NativeGraph->identityValid &&
                result.v4NativeGraph->hypothesisObservationRecorded;
            facts.hypothesisNotMetObserved = facts.hypothesisObservationRecorded && result.hypothesisNotMetObserved;
            facts.obligations = std::move(result.obligations); facts.notApplicable = std::move(result.notApplicable);
            facts.graphUnresolved = std::move(result.graphUnresolved);
            facts.postDispatchObligations = std::move(result.postDispatchObligations);
            facts.unclassifiedObligations = std::move(result.unclassifiedObligations);
            facts.obligations.emplace_back("v4_native_detailed_execution_evidence_persistence_unimplemented");
            RecordDiagnosticCapacity(facts, result.v4DiagnosticBudget, result.v4BackgroundDiagnosticBudget);
            auto outcome = std::unique_ptr<v3_scheduler::ScheduledV4Outcome>(
                new v3_scheduler::ScheduledV4Outcome(attempt, std::move(facts)));
            return outcome->Valid() ? std::move(outcome) : nullptr;
        }
        catch (...) { return {}; }
    }
private:
    static ingress_adapter::BridgeResult RunBridge(
        const v3_scheduler::State& preState, std::size_t familyIndex, std::size_t caseIndex,
        v3_scheduler::InFlightReceipt& attempt, std::string_view executionId,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t checkpointByteBudget, bool& consumed, bool& entered)
    {
        ingress_adapter::BridgeResult result;
        try
        {
            if (familyIndex >= preState.families.size())
            { result.observationUnavailable = true; result.obligations.emplace_back("v4_scheduled_family_index_unavailable"); return result; }
            const auto& family = preState.families[familyIndex];
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord)
            {
                result.observationUnavailable = true; result.obligations = replay.errors;
                result.obligations.emplace_back("v4_scheduled_case_replay_unavailable"); return result;
            }
            const auto& record = *replay.unexecutedRecord;
            if (record.kind != "graph_hypothesis" || !record.coordinate || !record.graph)
            {
                result.obligations = record.unresolved;
                result.obligations.emplace_back("v4_scheduled_case_graph_or_mapping_unimplemented"); return result;
            }
            const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
            if (decoded.status != V4FamilyCoordinateStatus::ready || !decoded.family || decoded.coordinate != family.coordinate)
            { result.observationUnavailable = true; result.obligations.emplace_back("v4_scheduled_family_unavailable"); return result; }
            const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
            { result.observationUnavailable = true; result.obligations.emplace_back("v4_scheduled_family_or_coordinate_unavailable"); return result; }
            const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
            if (!raw.errors.empty() || raw.id != record.candidateId)
            { result.observationUnavailable = true; result.obligations.emplace_back("v4_scheduled_candidate_identity_mismatch"); return result; }
            if (!attempt.Consume(preState, familyIndex, caseIndex, executionId, checkpointByteBudget))
            { result.observationUnavailable = true; result.obligations.emplace_back("v4_scheduled_inflight_receipt_unavailable"); return result; }
            consumed = true; entered = true;
            return ingress_adapter::PrepareV4DesktopSingleText(identity, verified, &record.graph->present, &record.graph->graph);
        }
        catch (const std::exception& error)
        {
            result.observationUnavailable = true; result.productionEntryUnknown = entered; result.failure = error.what();
            result.obligations.emplace_back("v4_scheduled_adapter_observation_unavailable"); return result;
        }
        catch (...)
        {
            result.observationUnavailable = true; result.productionEntryUnknown = entered;
            result.obligations.emplace_back("v4_scheduled_adapter_unknown_observation_unavailable"); return result;
        }
    }
};

// Translate only controllable program fields. Points are observed fixture
// coordinates indexed by the original program, not inferred target outcomes.
// The adapter must still bind each point to the candidate and live host.
struct V4ModelOleInputs
{
    bool prepared = false, resourceUnavailable = false;
    std::string candidateId;
    std::vector<ole_driver::Input> inputs;
    std::vector<std::string> unresolved;
};
V4ModelOleInputs PrepareV4ModelOleInputs(const V4FamilyValidatedContext& verified,
    const event_graph::V4FamilyIdentityContext& identity, const std::vector<std::optional<POINTL>>& points)
{
    V4ModelOleInputs result;
    try
    {
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || identity.Status() != V4FamilyStatus::schema_only ||
            verified.FamilyBinding() != identity.FamilyBinding() || verified.Axes().size() != identity.Axes().size())
        { result.unresolved.emplace_back("v4_program_authoritative_context_unavailable"); return result; }
        for (std::size_t i = 0; i < identity.Axes().size(); ++i)
        {
            const auto& a = verified.Axes()[i]; const auto& b = identity.Axes()[i];
            if (a.id != b.id || a.role != b.role || a.values != b.values || a.label != b.label)
            { result.unresolved.emplace_back("v4_program_schema_mismatch"); return result; }
        }
        const auto raw = identity.RawCandidateIdentity(verified.Coordinate());
        if (raw.resourceUnavailable) result.resourceUnavailable = true;
        if (raw.resourceUnavailable || !raw.errors.empty() || raw.id.empty())
        { result.unresolved.emplace_back("v4_program_candidate_unavailable"); return result; }
        const auto& axes = verified.Axes(); const auto& candidate = verified.Coordinate();
        if (Value(axes, candidate, "T01") != "external_ingress" || verified.TaskBActive())
        { result.unresolved.emplace_back("v4_program_source_or_second_task_unimplemented"); return result; }
        const auto program = verified.TransferProgram();
        result.unresolved.insert(result.unresolved.end(), program.unresolved.begin(), program.unresolved.end());
        if (points.size() != program.steps.size())
        { result.unresolved.emplace_back("v4_program_point_shape_unavailable"); return result; }
        const auto allowed = ole_driver::EncodeAllowedEffect(Value(axes, candidate, "T03"),
            Value(axes, candidate, "T03.extra_protocol_bits"));
        if (!allowed) { result.unresolved.emplace_back("v4_program_effect_unavailable"); return result; }
        bool entered = false, terminated = false;
        for (std::size_t i = 0; i < program.steps.size(); ++i)
        {
            const auto& step = program.steps[i]; const auto kind = ole_driver::ProgramKind(step.kind);
            if (!kind)
            {
                if (points[i]) { result.unresolved.emplace_back("v4_program_non_call_point_invalid"); return result; }
                if (step.kind != "configure_v4_source_input" && step.kind != "observe_completion_or_rejection" &&
                    step.kind != "assert_business_results")
                    result.unresolved.push_back("v4_program_non_call_stage_unimplemented:" + step.kind);
                continue;
            }
            if (terminated || (*kind == ole_driver::Kind::enter && entered) || (*kind != ole_driver::Kind::enter && !entered))
            { result.unresolved.emplace_back("v4_program_session_order_unavailable"); return result; }
            ole_driver::Input input; input.programIndex = i; input.kind = *kind; input.modelParameter = step.parameter;
            if (*kind == ole_driver::Kind::leave)
            {
                if (points[i]) { result.unresolved.emplace_back("v4_program_leave_point_invalid"); return result; }
            }
            else
            {
                const auto [buttonsAxis, modifiersAxis] = ole_driver::PhaseAxes(step.kind);
                const auto buttons = Value(axes, candidate, buttonsAxis), modifiers = Value(axes, candidate, modifiersAxis);
                const auto parameter = ole_driver::ParseKeyParameter(step.parameter);
                const auto encoded = ole_driver::EncodeKeyState(buttons, modifiers);
                if (!points[i] || !parameter || parameter->first != buttons || parameter->second != modifiers ||
                    !encoded.valid || encoded.altX1Ambiguous)
                { result.unresolved.emplace_back("v4_program_key_or_point_unavailable"); return result; }
                input.keyState = encoded.value; input.allowedEffect = *allowed; input.screenPoint = *points[i];
            }
            entered |= *kind == ole_driver::Kind::enter;
            terminated |= *kind == ole_driver::Kind::drop || *kind == ole_driver::Kind::leave;
            result.inputs.push_back(std::move(input));
        }
        if (!entered || !terminated) { result.unresolved.emplace_back("v4_program_terminal_unavailable"); return result; }
        result.candidateId = raw.id; result.prepared = true;
        result.unresolved.emplace_back("v4_program_geometry_and_host_binding_unverified");
        return result;
    }
    catch (const std::bad_alloc&) { result.prepared = false; result.resourceUnavailable = true; return result; }
    catch (const std::length_error&) { result.prepared = false; result.resourceUnavailable = true; return result; }
    catch (...) { result.prepared = false; result.unresolved.emplace_back("v4_program_observation_unavailable"); return result; }
}

// One foreground process owns this in-memory head. Publication/readback is
// not a cross-process CAS or a crash-durability certificate.
class ForegroundV4Runner final
{
public:
    struct NativeInputs
    {
        behavior::OwnedProfileDesktopRoot* root = nullptr;
        const V4SingleFileFixture* fixture = nullptr;
        GridCell landing;
        POINT targetScreen{};
        std::vector<ole_driver::NativePointInput> points;
    };
    struct FileInputs
    {
        behavior::OwnedProfileDesktopRoot* root = nullptr;
        const V4SingleFileFixture* fixture = nullptr;
        HANDLE sourceRoot = INVALID_HANDLE_VALUE;
        FILE_ID_INFO sourceRootId{};
        GridCell landing;
        std::vector<ole_driver::Input> inputs;
    };
    struct BatchInputs
    {
        behavior::OwnedProfileDesktopRoot* root = nullptr;
        std::shared_ptr<behavior::OwnedProfileDesktopRoot> ownedRoot;
        std::shared_ptr<V4IndependentFilePayload> payload;
        const V4IndependentShellSource* binding = nullptr;
        std::shared_ptr<V4IndependentShellOwner> owner;
        std::shared_ptr<const V4PinnedCopyLease> pinnedCopyLease;
        std::shared_ptr<const V4SharedMoveLease> sharedMoveLease;
        bool prepareSingleFromModel = false;
        std::shared_ptr<V4OwnedChildDirectoryAttempt> sourceRoot;
        std::shared_ptr<const V4OwnedFileFamilySeed> familySeed;
        POINT proposedClient{150, 150};
        POINT collectionClient{350, 250};
        GridCell landing;
        std::vector<ole_driver::Input> inputs;
        std::size_t contentByteBudget = 0, virtualBudget = 0;
    };
    struct DirectoryInputs
    {
        behavior::OwnedProfileDesktopRoot* root = nullptr;
        const directory_fixture::FixtureObservation* fixture = nullptr;
        std::shared_ptr<directory_fixture::DirectoryCopyLease> lease;
        GridCell landing;
        std::vector<ole_driver::Input> inputs;
    };
    struct VirtualInputs
    {
        behavior::OwnedProfileDesktopRoot* root = nullptr;
        GridCell landing;
        std::vector<ole_driver::Input> inputs;
        std::size_t contentByteBudget = 0, virtualBudget = 0;
    };
    struct StepResult
    {
        std::string disposition, caseId, candidateId, graphId, detail, outcomeStatus;
        std::optional<std::size_t> familyIndex, caseIndex;
        bool productionStarted = false, productionEntryUnknown = false;
        bool eventFailure = false, businessFailure = false, observationUnavailable = false;
        bool eventDispatchObservationRecorded = false, eventDispatchFailure = false;
        bool hypothesisObservationRecorded = false, hypothesisNotMet = false;
        bool familyScanExhausted = false;
        bool fixturePreparationMayHaveSideEffects = false;
        std::filesystem::path fixtureRoot;
        std::shared_ptr<const V4IndependentFilePayload> fixturePayload;
        std::optional<V4FileCopyHandoff> fixtureHandoff;
        std::optional<v3_scheduler::PublishedCheckpoint> proposalPublication, attemptPublication, outcomePublication;
    };
    struct AttemptHandoffs
    {
        std::function<bool(const v3_scheduler::InFlightReceipt&)> beforeExecution;
        std::function<bool(const v3_scheduler::CommittedV4Receipt&)> afterCommit;
    };
    static std::unique_ptr<ForegroundV4Runner> Start(v3_scheduler::CheckpointStore& store,
        std::string binding, std::size_t byteBudget, v3_scheduler::PublishedCheckpoint& publication)
    {
        publication = {};
        std::unique_ptr<ForegroundV4Runner> runner;
        try
        {
            if (binding.empty()) return {};
            v3_scheduler::State initial;
            initial.binding = binding; initial.familyVersion = v3_scheduler::FamilyVersion::v4;
            const auto migration = v3_scheduler::PrepareV4TypedMigration(initial, binding, byteBudget);
            if (!migration.proposed) return {};
            runner.reset(new ForegroundV4Runner(store, std::move(binding), byteBudget));
            const bool published = runner->PublishChecked(*migration.proposed);
            publication = std::move(runner->lastPublication_);
            if (!published) return {};
            return runner;
        }
        catch (...) { if (runner) publication = std::move(runner->lastPublication_); return {}; }
    }
    StepResult Step(std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t workBudget,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t caseTokenBudget,
        std::string_view executionId, const NativeInputs* native = nullptr, const FileInputs* file = nullptr,
        const BatchInputs* batch = nullptr, const DirectoryInputs* directory = nullptr,
        const VirtualInputs* virtualFiles = nullptr,
        std::optional<std::pair<std::size_t, std::size_t>> selected = std::nullopt,
        bool allowExecution = false, const AttemptHandoffs* handoffs = nullptr);
    StepResult AddTextSeed(std::size_t familyIndex, const Candidate& candidate, const std::vector<bool>& present,
        std::string_view graphId, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget);
    StepResult AddStructuredCase(std::string_view caseId, const V4CoordinateBudget& coordinateBudget,
        const V4AxisBudget& axisBudget, std::size_t tokenBudget, std::size_t replayWorkBudget);
    StepResult AddOwnedSingleFileCase(const V4OwnedFileFamilySeed& seed,
        const std::vector<std::pair<std::string, std::string>>& choices, const std::vector<bool>& present,
        std::string_view graphId, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t tokenBudget, std::size_t replayWorkBudget);
    StepResult RegisterOwnedFileFamily(const V4OwnedFileFamilySeed& seed, const V4CoordinateBudget& coordinateBudget);
    StepResult VisitOwnedFileFamily(const V4OwnedFileFamilySeed& seed, std::size_t familyIndex,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget, std::size_t workBudget);
    StepResult PrepareGlobalCycle(std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t workBudget,
        const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
        std::size_t replayWorkBudget, std::size_t tokenBudget, std::size_t caseByteBudget);
    std::optional<v3_scheduler::State> Snapshot()
    {
        try { if (healthy_ && CheckHead()) return state_; }
        catch (...) { }
        healthy_ = false;
        return std::nullopt;
    }
private:
    ForegroundV4Runner(v3_scheduler::CheckpointStore& store, std::string binding, std::size_t budget)
        : store_(store), binding_(std::move(binding)), byteBudget_(budget) {}
    // These are narrowly bounded production attempts, not coverage verdicts.
    // Each adapter independently checks the live source, host and input plan;
    // unbound axes and graph observations remain unresolved after dispatch.
    bool AdapterCapabilityAvailable(const v3_scheduler::V4RoundProposal& round,
        const V4FamilyDescriptor& family, const event_graph::V4FamilyCandidateRecord& record,
        const V4AxisBudget& budget, const BatchInputs* batch, const NativeInputs* native) const
    {
        if (!record.coordinate || family.b || family.a.kind != V4SourceKind::file_only ||
            family.a.selectionCount.kind != V3CountKind::exact || family.a.selectionCount.decimal != "1" ||
            family.a.fileCount.kind != V3CountKind::exact || family.a.fileCount.decimal != "1") return false;
        const auto verified = V4FamilyValidatedContext::Create(family, budget, *record.coordinate);
        if (verified.Status() != V4CoordinateStatus::coordinate_ready || verified.TaskBActive()) return false;
        const auto value = [&](std::string_view id) { return Value(verified.Axes(), verified.Coordinate(), id); };
        if (round.action == v3_scheduler::V4RoundAction::native_candidate)
        {
            if (!record.nativeGraph || record.graph || !native || !native->root || !native->root->StillOwned() ||
                !native->root->profile || !native->fixture || !native->fixture->inputPrepared ||
                !native->fixture->created || !native->fixture->fileIdentityObserved ||
                native->fixture->candidateId != record.candidateId ||
                !behavior::OwnedProfileDesktopRoot::SameId(native->fixture->rootId, native->root->desktopId) ||
                native->fixture->root != native->root->profile->desktop ||
                native->fixture->file.parent_path() != native->fixture->root) return false;
            if (native->fixture->relativeBinding && !native->fixture->relativeBinding->StillBound()) return false;
            return value("T01") == "internal_native" && value("V4.source.kind") == "file_only" &&
                value("V4.source.file_count") == "one" && value("V4.file.member0_kind") == "file" &&
                value("V4.file.member0_identity") == "unique" && value("source.E01") == "free_desktop" &&
                value("source.E02") == "none" && value("source.E03") == "desktop_icon" &&
                value("target.E01") == "free_desktop" && value("target.E02") == "none" &&
                value("H01.region") == "empty_slot" && value("G01.button") == "left" &&
                value("G02.press") == "none" && value("G02.release") == "none" &&
                value("G02.release_buttons") == "none" && value("G04.termination") == "drop";
        }
        if (round.action != v3_scheduler::V4RoundAction::ole_candidate || !record.graph || record.nativeGraph ||
            !batch || !batch->prepareSingleFromModel || !batch->ownedRoot || batch->ownedRoot.get() != batch->root ||
            !batch->root || !batch->root->StillOwned() || !batch->sourceRoot || !batch->sourceRoot->available ||
            !batch->sourceRoot->created || !batch->sourceRoot->identityObserved || !batch->familySeed ||
            !batch->familySeed->StillOwned()) return false;
        if (value("V4.source.kind") != "file_only" || value("V4.source.file_count") != "one" ||
            value("V4.file.member0_kind") != "file" || value("V4.file.member0_identity") != "unique" ||
            value("T01") != "external_ingress" || value("T02.file_paths") != "yes" ||
            value("T02.shell_object") != "no" || value("T02.virtual_files") != "no" ||
            value("T02.virtual_completeness") != "na" || value("T02.image") != "no" ||
            value("T02.text") != "no" || value("T02.url") != "no" || value("T02.url_kind") != "na" ||
            value("T02.invalid_format") != "no" || value("T03") != "copy" ||
            value("T03.extra_protocol_bits") != "none" || value("T04.capability") != "enabled" ||
            value("G02.enter_buttons") != "left" || value("G02.enter") != "none" ||
            value("G02.release_buttons") != "none" || value("G02.release") != "none" ||
            value("G04.termination") != "drop" || value("X03.task_count") != "one" ||
            value("target.E01") != "free_desktop" || value("target.E02") != "none" || value("H01.region") != "empty_slot")
            return false;
        const auto program = verified.TransferProgram();
        std::vector<std::string_view> actions;
        for (const auto& step : program.steps)
            if (step.kind != "configure_v4_source_input") actions.push_back(step.kind);
        if (actions != std::vector<std::string_view>{"external_enter", "release",
                "observe_completion_or_rejection", "assert_business_results"}) return false;
        for (const auto& axis : verified.Axes())
        {
            const std::string_view id = axis.id;
            if (!id.starts_with("V4.format.")) continue;
            const auto actual = value(id);
            if (id.ends_with(".offer") && actual != (id == "V4.format.cf_hdrop.offer" ? "yes" : "no")) return false;
            if (id.find(".medium.") != std::string_view::npos &&
                actual != (id == "V4.format.cf_hdrop.medium.hglobal" ? "yes" : "no")) return false;
            if (id.ends_with(".unlisted_payloads") && actual != "reject_unlisted") return false;
        }
        return true;
    }
    bool CheckHead() const
    {
        const auto read = store_.Read(headName_, headId_, binding_, byteBudget_);
        if (read.status != v3_scheduler::CodecStatus::decoded || read.recoveredInFlight) return false;
        const auto actual = v3_scheduler::EncodeCheckpoint(read.state, binding_, byteBudget_);
        const auto expected = v3_scheduler::EncodeCheckpoint(state_, binding_, byteBudget_);
        return actual.status == v3_scheduler::CodecStatus::encoded && expected.status == v3_scheduler::CodecStatus::encoded &&
            actual.bytes == expected.bytes;
    }
    bool PublishChecked(const v3_scheduler::State& proposed)
    {
        lastPublication_ = {};
        const auto expected = v3_scheduler::EncodeCheckpoint(proposed, binding_, byteBudget_);
        if (expected.status != v3_scheduler::CodecStatus::encoded) return false;
        lastPublication_ = store_.Publish(proposed, binding_, byteBudget_);
        if (lastPublication_.status != v3_scheduler::StoreStatus::published_durability_unverified ||
            !lastPublication_.fileIdentityCaptured || !lastPublication_.renameReturnedSuccess) return false;
        const auto read = store_.Read(lastPublication_.fileName, lastPublication_.fileId, binding_, byteBudget_);
        if (read.status != v3_scheduler::CodecStatus::decoded || read.recoveredInFlight) return false;
        const auto actual = v3_scheduler::EncodeCheckpoint(read.state, binding_, byteBudget_);
        if (actual.status != v3_scheduler::CodecStatus::encoded || actual.bytes != expected.bytes) return false;
        headName_ = lastPublication_.fileName; headId_ = lastPublication_.fileId; state_ = read.state; healthy_ = true;
        return true;
    }
    v3_scheduler::CheckpointStore& store_;
    std::string binding_;
    std::size_t byteBudget_ = 0;
    std::wstring headName_;
    FILE_ID_INFO headId_{};
    v3_scheduler::PublishedCheckpoint lastPublication_;
    v3_scheduler::State state_;
    bool healthy_ = false;
};
static_assert(std::is_nothrow_move_constructible_v<v3_scheduler::PublishedCheckpoint> &&
    std::is_nothrow_move_assignable_v<v3_scheduler::PublishedCheckpoint>);

ForegroundV4Runner::StepResult ForegroundV4Runner::Step(
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t workBudget,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t replayWorkBudget, std::size_t caseTokenBudget, std::string_view executionId,
    const NativeInputs* native, const FileInputs* file, const BatchInputs* batch, const DirectoryInputs* directory,
    const VirtualInputs* virtualFiles,
    std::optional<std::pair<std::size_t, std::size_t>> selected, bool allowExecution, const AttemptHandoffs* handoffs)
{
    StepResult result;
    bool proposalStarted = false;
    lastPublication_ = {};
    try
    {
        if (!healthy_ || !CheckHead()) { healthy_ = false; result.disposition = "head_unavailable"; return result; }
        const auto previousFamilyCount = state_.families.size();
        v3_scheduler::V4RoundProposal round;
        if (selected)
        {
            const auto [familyIndex, caseIndex] = *selected;
            if (familyIndex >= state_.families.size() || caseIndex >= state_.families[familyIndex].cases.size())
            { result.disposition = "selected_case_unavailable"; return result; }
            const auto& family = state_.families[familyIndex];
            const auto& saved = family.cases[caseIndex];
            if ((saved.state != v3_scheduler::WorkState::pending && saved.state != v3_scheduler::WorkState::resource_unavailable) ||
                !saved.executionId.empty())
            { result.disposition = "selected_case_not_pending"; return result; }
            const auto replay = v3_scheduler::ReplayV4Case(family, caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
            if (!replay.unexecutedRecord || !replay.errors.empty())
            {
                result.disposition = replay.status == v3_scheduler::V4CaseReplay::Status::resource_unavailable
                    ? "retry_resource_unavailable" : "retry_unavailable";
                if (!replay.errors.empty()) result.detail = replay.errors.front();
                return result;
            }
            const auto& record = *replay.unexecutedRecord;
            if (record.kind != "graph_hypothesis" || !record.coordinate ||
                static_cast<bool>(record.graph) == static_cast<bool>(record.nativeGraph))
            { result.disposition = "selected_case_graph_unavailable"; return result; }
            round.visit.tick.proposed = state_; round.familyIndex = familyIndex; round.caseIndex = caseIndex;
            round.candidateId = record.candidateId;
            if (record.graph) { round.action = v3_scheduler::V4RoundAction::ole_candidate; round.graphId = record.graph->graph.id; }
            else { round.action = v3_scheduler::V4RoundAction::native_candidate; round.graphId = record.nativeGraph->graph.id; }
        }
        else
            round = v3_scheduler::PrepareV4Round(state_, binding_, decimalDigitBudget, memberBudget,
                workBudget, coordinateBudget, axisBudget, replayWorkBudget);
        if (round.action == v3_scheduler::V4RoundAction::invalid)
        { result.disposition = "round_unavailable"; if (!round.errors.empty()) result.detail = round.errors.front(); return result; }
        proposalStarted = true;
        const bool proposalPublished = PublishChecked(round.visit.tick.proposed);
        result.proposalPublication.emplace(std::move(lastPublication_));
        if (!proposalPublished)
        { healthy_ = false; result.disposition = "publication_unavailable"; return result; }
        result.candidateId = round.candidateId; result.graphId = round.graphId;
        result.familyIndex = round.familyIndex; result.caseIndex = round.caseIndex;
        if (!result.familyIndex && round.visit.tick.familyIndex) result.familyIndex = round.visit.tick.familyIndex;
        if (!result.familyIndex && state_.families.size() > previousFamilyCount &&
            state_.families.size() - previousFamilyCount == 1)
            result.familyIndex = previousFamilyCount;
        if (round.action == v3_scheduler::V4RoundAction::publish_only)
        { result.disposition = "published_unexecuted"; return result; }
        if (round.action == v3_scheduler::V4RoundAction::retry_unavailable)
        {
            result.disposition = round.replayStatus == v3_scheduler::V4CaseReplay::Status::resource_unavailable
                ? "retry_resource_unavailable" : "retry_unavailable";
            if (!round.errors.empty()) result.detail = round.errors.front();
            return result;
        }
        if (!round.familyIndex || !round.caseIndex)
        { result.disposition = "attempt_input_unavailable"; return result; }
        const auto& family = state_.families.at(*round.familyIndex);
        const auto replay = v3_scheduler::ReplayV4Case(family, *round.caseIndex, coordinateBudget, axisBudget, replayWorkBudget);
        const auto decoded = NextV4FamilyCoordinate(family.coordinate, coordinateBudget);
        if (replay.status == v3_scheduler::V4CaseReplay::Status::resource_unavailable ||
            decoded.status == V4FamilyCoordinateStatus::resource_unavailable)
        { result.disposition = "case_identity_resource_unavailable"; return result; }
        if (!replay.unexecutedRecord || !replay.unexecutedRecord->coordinate || !decoded.family ||
            decoded.status != V4FamilyCoordinateStatus::ready)
        { result.disposition = "case_identity_unavailable"; return result; }
        const auto prefix = EncodeV4StructuredPrefix(*decoded.family, caseTokenBudget);
        if (!prefix.encoded)
        { result.disposition = prefix.resourceUnavailable ? "case_identity_resource_unavailable" : "case_identity_unavailable"; return result; }
        const auto& record = *replay.unexecutedRecord;
        if (static_cast<bool>(record.graph) == static_cast<bool>(record.nativeGraph) ||
            (round.action == v3_scheduler::V4RoundAction::ole_candidate) != static_cast<bool>(record.graph))
        { result.disposition = "case_identity_unavailable"; return result; }
        const auto& presence = record.graph ? record.graph->present : record.nativeGraph->present;
        const auto mode = record.graph ? std::string_view("ole") : std::string_view("native");
        const auto id = v3_scheduler::MakeV4StructuredCaseId(prefix.tokens, *record.coordinate, presence,
            mode, result.graphId, coordinateBudget, axisBudget, caseTokenBudget, byteBudget_);
        if (id.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || id.candidateId != result.candidateId || id.graphId != result.graphId)
        { result.disposition = id.status == v3_scheduler::V4StructuredCaseStatus::resource_unavailable
                ? "case_identity_resource_unavailable" : "case_identity_unavailable"; return result; }
        result.caseId = id.caseId;
        if (executionId.empty())
        { result.disposition = "attempt_input_unavailable"; return result; }
        // Do not materialize or reserve a candidate while execution is known
        // to be unavailable: repeating a pending case must not collide with
        // files or source owners left by an earlier preparation.
        if (!allowExecution || !AdapterCapabilityAvailable(round, *decoded.family, record, axisBudget, batch, native))
        { result.disposition = "adapter_unimplemented_pending"; return result; }
        // The current native runner accepts a single-file fixture only. A
        // selection preparation program or any other member count needs its
        // own same-case two-session bridge; do not consume the attempt or
        // silently drive it through the single-file path.
        if (round.action == v3_scheduler::V4RoundAction::native_candidate &&
            (decoded.family->b || decoded.family->a.kind != V4SourceKind::file_only ||
             decoded.family->a.selectionCount.kind != V3CountKind::exact ||
             decoded.family->a.selectionCount.decimal != "1" ||
             decoded.family->a.fileCount.kind != V3CountKind::exact ||
             decoded.family->a.fileCount.decimal != "1" ||
             decoded.family->a.preparation.mode != V4SelectionPreparationMode::none ||
             !decoded.family->a.preparation.steps.empty() ||
             !decoded.family->a.preparation.outsideIdentities.empty()))
        { result.disposition = "native_multi_or_preparation_adapter_unimplemented_pending"; return result; }
        if (!handoffs || !handoffs->beforeExecution || !handoffs->afterCommit)
        { result.disposition = "attempt_handoff_unavailable_pending"; return result; }
        std::optional<v3_scheduler::AttemptResult> attempt;
        const auto beginAttempt = [&] {
            if (attempt) return attempt->receipt != nullptr;
            attempt.emplace(store_.BeginV4Attempt(headName_, headId_, binding_, *round.familyIndex, *round.caseIndex,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_));
            result.attemptPublication.emplace(std::move(attempt->checkpoint));
            if (!attempt->receipt)
            { healthy_ = false; result.disposition = "attempt_unavailable"; result.detail = attempt->reason; return false; }
            // Once this record exists, even fixture preparation failures are
            // unknown on recovery until explicitly reconciled.
            healthy_ = false;
            if (!handoffs->beforeExecution(*attempt->receipt))
            { result.disposition = "attempt_handoff_unknown"; return false; }
            return true;
        };
        const bool fileOle = round.action == v3_scheduler::V4RoundAction::ole_candidate &&
            decoded.family->a.kind == V4SourceKind::file_only;
        const bool mixedOle = round.action == v3_scheduler::V4RoundAction::ole_candidate &&
            decoded.family->a.kind == V4SourceKind::file_and_content;
        bool virtualOle = false;
        if (round.action == v3_scheduler::V4RoundAction::ole_candidate && decoded.family->a.kind == V4SourceKind::content_only)
        {
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (verified.Status() != V4CoordinateStatus::coordinate_ready)
            { result.disposition = "content_member_mapping_unavailable"; return result; }
            virtualOle = Value(verified.Axes(), verified.Coordinate(), "T02.virtual_files") == "yes";
        }
        if ((fileOle || mixedOle) &&
            (decoded.family->a.fileCount.kind != V3CountKind::exact || decoded.family->a.fileCount.decimal == "0"))
        { result.disposition = "file_count_mapping_unimplemented_pending"; return result; }
        const bool batchOle = fileOle && Less("1", decoded.family->a.fileCount.decimal);
        bool directoryOle = false, collectionOle = false, desktopFilesOle = false, mappingOle = false, groupOle = false, desktopBatchOle = false, moveOle = false;
        BatchInputs generatedBatch;
        const BatchInputs* effectiveBatch = batch;
        std::optional<ingress_adapter::V4PinnedSingleFileSourcePreparation> generatedSource;
        if (fileOle || mixedOle)
        {
            const auto verified = V4FamilyValidatedContext::Create(*decoded.family, axisBudget, *record.coordinate);
            if (verified.Status() != V4CoordinateStatus::coordinate_ready)
            { result.disposition = "file_member_mapping_unavailable"; return result; }
            const auto count = V4MaterializeCount(decoded.family->a.fileCount, axisBudget.fileMembers, false);
            if (count.status != V4AxisStatus::schema_only)
            { result.disposition = "file_member_resource_unavailable"; return result; }
            for (std::size_t i = 0; i < count.value; ++i)
            {
                const auto kind = Value(verified.Axes(), verified.Coordinate(), "V4.file.member" + std::to_string(i) + "_kind");
                if (kind == "directory" && fileOle && !batchOle) directoryOle = true;
                else if (kind != "file")
                { result.disposition = kind == "directory" ? "batch_directory_adapter_unimplemented_pending" : "file_member_mapping_unimplemented_pending"; return result; }
            }
            collectionOle = fileOle && !batchOle && !directoryOle &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "collection";
            desktopFilesOle = fileOle && !batchOle && !directoryOle &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "desktop_files";
            mappingOle = fileOle && !batchOle && !directoryOle &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "folder_mapping";
            groupOle = fileOle && !batchOle && !directoryOle &&
                (Value(verified.Axes(), verified.Coordinate(), "target.E02") == "collection_group" ||
                 Value(verified.Axes(), verified.Coordinate(), "target.E02") == "file_group");
            desktopBatchOle = fileOle && !batchOle && !directoryOle && !collectionOle && !groupOle && batch &&
                Value(verified.Axes(), verified.Coordinate(), "target.E01") == "free_desktop" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E02") == "none";
            const bool moveRequested = desktopBatchOle && Value(verified.Axes(), verified.Coordinate(), "G04.termination") == "drop" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.release") == "none" &&
                Value(verified.Axes(), verified.Coordinate(), "T03") == "copy_move";
            moveOle = moveRequested && Value(verified.Axes(), verified.Coordinate(), "V4.source.kind") == "file_only" &&
                Value(verified.Axes(), verified.Coordinate(), "V4.source.file_count") == "one" &&
                Value(verified.Axes(), verified.Coordinate(), "V4.file.member0_kind") == "file" &&
                Value(verified.Axes(), verified.Coordinate(), "V4.file.member0_identity") == "unique" &&
                Value(verified.Axes(), verified.Coordinate(), "T01") == "external_ingress" &&
                Value(verified.Axes(), verified.Coordinate(), "T02.file_paths") == "yes" &&
                Value(verified.Axes(), verified.Coordinate(), "T04.capability") == "enabled" &&
                Value(verified.Axes(), verified.Coordinate(), "H01.region") == "empty_slot" &&
                Value(verified.Axes(), verified.Coordinate(), "target.E19") == "named" &&
                Value(verified.Axes(), verified.Coordinate(), "H06.page_record") == "saved" &&
                Value(verified.Axes(), verified.Coordinate(), "H06.page_dimensions") == "valid" &&
                Value(verified.Axes(), verified.Coordinate(), "H07.occupancy") == "partial" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.release_buttons") == "none" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.enter_buttons") == "left" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.enter") == "none" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.hover_first") == "na" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.hover_first_buttons") == "na" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.hover_later") == "na" &&
                Value(verified.Axes(), verified.Coordinate(), "G02.hover_later_buttons") == "na" &&
                Value(verified.Axes(), verified.Coordinate(), "T03.extra_protocol_bits") == "none";
            if (moveRequested && !moveOle)
            { result.disposition = "move_candidate_preflight_unimplemented_pending"; return result; }
            if ((desktopBatchOle || collectionOle || desktopFilesOle || mappingOle || groupOle) && batch && batch->prepareSingleFromModel)
            {
                generatedBatch = *batch;
                const auto parentMatches = [&] {
                    if (!generatedBatch.root || !generatedBatch.sourceRoot) return false;
                    const HANDLE parent = CreateFileW(generatedBatch.sourceRoot->physicalPath.parent_path().c_str(),
                        FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
                    if (parent == INVALID_HANDLE_VALUE) return false;
                    FILE_ID_INFO id{}; FILE_ATTRIBUTE_TAG_INFO tag{};
                    const bool matches = GetFileInformationByHandleEx(parent, FileIdInfo, &id, sizeof(id)) &&
                        GetFileInformationByHandleEx(parent, FileAttributeTagInfo, &tag, sizeof(tag)) &&
                        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                        behavior::OwnedProfileDesktopRoot::SameId(id, generatedBatch.root->rootId) && generatedBatch.root->StillOwned();
                    CloseHandle(parent); return matches;
                };
                if (!generatedBatch.ownedRoot || generatedBatch.ownedRoot.get() != generatedBatch.root ||
                    !generatedBatch.root->StillOwned() || !generatedBatch.root->profile ||
                    !generatedBatch.root->profile->app || !generatedBatch.sourceRoot || !generatedBatch.sourceRoot->available ||
                    !generatedBatch.sourceRoot->created || !generatedBatch.sourceRoot->identityObserved ||
                    !generatedBatch.familySeed || !generatedBatch.familySeed->StillOwned() ||
                    generatedBatch.familySeed->RootPath() != generatedBatch.sourceRoot->physicalPath ||
                    !behavior::OwnedProfileDesktopRoot::SameId(generatedBatch.familySeed->RootId(), generatedBatch.sourceRoot->identity) ||
                    generatedBatch.familySeed->Coordinate() != family.coordinate ||
                    !parentMatches())
                { result.disposition = "single_copy_source_root_unavailable"; return result; }
                const auto profileOwner = generatedBatch.root->profile;
                auto& profile = *profileOwner;
                if (behavior::Access::productionDispatch != 1)
                    behavior::Access::EnableProductionDispatch(*profile.app, false);
                if (!behavior::Access::fixtureControlWindow) behavior::Access::EnableFixtureControlWindow(*profile.app);
                const auto identity = event_graph::V4FamilyIdentityContext::Create(*decoded.family, axisBudget);
                if (identity.Status() != V4FamilyStatus::schema_only ||
                    identity.FamilyBinding() != generatedBatch.familySeed->Binding())
                { result.disposition = "single_copy_family_unavailable"; return result; }
                const auto collectionSetting = Value(verified.Axes(), verified.Coordinate(), "C07");
                if (collectionSetting != "on" && collectionSetting != "off")
                { result.disposition = "single_copy_collection_input_unavailable"; return result; }
                auto holder = std::make_shared<std::optional<V4IndependentFilePayload>>();
                if (!beginAttempt()) return result;
                if (!generatedBatch.root->StillOwned() || generatedBatch.root->profile.get() != &profile || !profile.app ||
                    !generatedBatch.familySeed->StillOwned() || !generatedBatch.sourceRoot->available ||
                    !generatedBatch.sourceRoot->created || !generatedBatch.sourceRoot->identityObserved ||
                    generatedBatch.familySeed->RootPath() != generatedBatch.sourceRoot->physicalPath ||
                    !behavior::OwnedProfileDesktopRoot::SameId(generatedBatch.familySeed->RootId(), generatedBatch.sourceRoot->identity) ||
                    generatedBatch.familySeed->Coordinate() != family.coordinate ||
                    generatedBatch.familySeed->Binding() != identity.FamilyBinding() || !parentMatches() ||
                    behavior::Access::asyncOwner != profile.app)
                { result.disposition = "single_copy_fixture_revalidation_unknown"; return result; }
                result.fixtureRoot = generatedBatch.sourceRoot->physicalPath;
                result.fixturePreparationMayHaveSideEffects = true;
                // A failed partial preparation needs explicit reconciliation;
                // do not silently retry it in this runner instance.
                healthy_ = false;
                behavior::Access::SetFixtureProgramCollectionEnabled(*profile.app, collectionSetting == "on");
                holder->emplace(PrepareV4IndependentFilePayload(verified, identity, generatedBatch.sourceRoot->physicalPath,
                    generatedBatch.familySeed->CreationRoot(), generatedBatch.familySeed->RootId(), axisBudget.fileMembers, axisBudget.payloadBytes));
                generatedBatch.payload = std::shared_ptr<V4IndependentFilePayload>(holder, &holder->value());
                result.fixturePayload = generatedBatch.payload;
                if (generatedBatch.payload->status != V4IndependentFileStatus::input_prepared)
                { result.disposition = "single_copy_fixture_reconciliation_required";
                  result.detail = std::string(generatedBatch.payload->reason); return result; }
                generatedSource.emplace(ingress_adapter::PrepareV4PinnedSingleFileSource(identity, verified,
                    *generatedBatch.root, generatedBatch.payload));
                if (generatedSource->handoff) result.fixtureHandoff = *generatedSource->handoff;
                if (!generatedSource->prepared || !generatedSource->binding || !generatedSource->handoff ||
                    (moveOle ? !generatedSource->handoff->sharedMoveLease : !generatedSource->handoff->pinnedLease) || !generatedSource->owner)
                { result.disposition = "single_copy_fixture_reconciliation_required";
                  if (!generatedSource->unresolved.empty()) result.detail = generatedSource->unresolved.front(); return result; }
                generatedBatch.binding = &*generatedSource->binding; generatedBatch.owner = generatedSource->owner;
                generatedBatch.pinnedCopyLease = generatedSource->handoff->pinnedLease;
                generatedBatch.sharedMoveLease = generatedSource->handoff->sharedMoveLease;
                const bool inactiveCollectionTab = groupOle &&
                    Value(verified.Axes(), verified.Coordinate(), "target.E02") == "collection_group" &&
                    Value(verified.Axes(), verified.Coordinate(), "target.E03") == "group_label" &&
                    Value(verified.Axes(), verified.Coordinate(), "H01.region") == "tab" &&
                    Value(verified.Axes(), verified.Coordinate(), "target.E08") == "first_child" &&
                    Value(verified.Axes(), verified.Coordinate(), "target.E08.active_child_type") == "collection" &&
                    Value(verified.Axes(), verified.Coordinate(), "target.E08.hit_relation") == "inactive_valid" &&
                    Value(verified.Axes(), verified.Coordinate(), "target.E08.hit_position") == "last_child";
                if (inactiveCollectionTab)
                {
                    // The tab exists only after the isolated group is seeded;
                    // its bridge must derive the model events from that live
                    // tab geometry and verify the actual production hit.
                    generatedBatch.inputs.clear();
                }
                else if (collectionOle || desktopFilesOle || mappingOle || groupOle)
                {
                    const auto screen = behavior::Access::ExternalPoint(generatedBatch.collectionClient);
                    const auto program = verified.TransferProgram();
                    std::vector<std::optional<POINTL>> points(program.steps.size());
                    for (std::size_t i = 0; i < program.steps.size(); ++i)
                    {
                        const auto kind = ole_driver::ProgramKind(program.steps[i].kind);
                        if (kind && *kind != ole_driver::Kind::leave) points[i] = screen;
                    }
                    auto input = PrepareV4ModelOleInputs(verified, identity, points);
                    if (!input.prepared || input.resourceUnavailable || input.candidateId != result.candidateId)
                    { result.disposition = "single_copy_fixture_reconciliation_required";
                      if (!input.unresolved.empty()) result.detail = input.unresolved.front(); return result; }
                    generatedBatch.inputs = std::move(input.inputs);
                }
                else
                {
                    const auto input = ingress_adapter::PrepareV4PinnedSingleFileDesktopInputs(identity, verified,
                        *generatedBatch.root, generatedBatch.proposedClient);
                    if (!input.prepared || input.resourceUnavailable || input.candidateId != result.candidateId)
                    { result.disposition = "single_copy_fixture_reconciliation_required";
                      if (!input.unresolved.empty()) result.detail = input.unresolved.front(); return result; }
                    generatedBatch.landing = input.landing; generatedBatch.inputs = input.inputs;
                }
                effectiveBatch = &generatedBatch;
            }
        }
        if (directoryOle && (!directory || !directory->root || !directory->fixture || !directory->lease))
        { result.disposition = "directory_fixture_unavailable"; return result; }
        if ((batchOle || collectionOle || desktopFilesOle || mappingOle || groupOle || desktopBatchOle) &&
            (!effectiveBatch || !effectiveBatch->root || !effectiveBatch->payload || !effectiveBatch->binding || !effectiveBatch->owner))
        { result.disposition = "batch_fixture_unavailable"; return result; }
        if ((desktopBatchOle || collectionOle || desktopFilesOle || mappingOle || groupOle) &&
            (moveOle ? !effectiveBatch->sharedMoveLease : !effectiveBatch->pinnedCopyLease))
        { result.disposition = "single_copy_lease_unavailable"; return result; }
        if (mixedOle && (!batch || !batch->root || !batch->payload || !batch->binding || !batch->owner))
        { result.disposition = "mixed_fixture_unavailable"; return result; }
        if (virtualOle && (!virtualFiles || !virtualFiles->root))
        { result.disposition = "virtual_fixture_unavailable"; return result; }
        if (fileOle && !batchOle && !directoryOle && !collectionOle && !desktopFilesOle && !mappingOle && !groupOle && !desktopBatchOle &&
            (!file || !file->root || !file->fixture || !file->sourceRoot || file->sourceRoot == INVALID_HANDLE_VALUE))
        { result.disposition = "file_fixture_unavailable"; return result; }
        if (round.action == v3_scheduler::V4RoundAction::ole_candidate && !fileOle && !mixedOle &&
            decoded.family->a.kind != V4SourceKind::content_only)
        { result.disposition = "ole_source_mapping_unimplemented_pending"; return result; }
        if (round.action == v3_scheduler::V4RoundAction::native_candidate &&
            (!native || !native->root || !native->fixture))
        { result.disposition = "native_fixture_unavailable"; return result; }
        if (!beginAttempt()) return result;
        std::unique_ptr<v3_scheduler::ScheduledV4Outcome> outcome;
        if (virtualOle)
            outcome = ScheduledV4Executor::RunVirtual(state_, *round.familyIndex, *round.caseIndex, *attempt->receipt,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_,
                *virtualFiles->root, virtualFiles->landing, virtualFiles->inputs,
                virtualFiles->contentByteBudget, virtualFiles->virtualBudget);
        else if (mixedOle)
            outcome = ScheduledV4Executor::RunMixed(state_, *round.familyIndex, *round.caseIndex, *attempt->receipt,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_,
                *batch->root, batch->payload, *batch->binding, batch->owner, batch->landing, batch->inputs,
                batch->contentByteBudget, batch->virtualBudget);
        else if (directoryOle)
            outcome = ScheduledV4Executor::RunDirectory(state_, *round.familyIndex, *round.caseIndex, *attempt->receipt,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_,
                *directory->root, *directory->fixture, directory->lease, directory->landing, directory->inputs);
        else if (batchOle || collectionOle || desktopFilesOle || mappingOle || groupOle || desktopBatchOle)
            outcome = ScheduledV4Executor::RunBatch(state_, *round.familyIndex, *round.caseIndex, *attempt->receipt,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_,
                *effectiveBatch->root, effectiveBatch->payload, *effectiveBatch->binding, effectiveBatch->owner,
                effectiveBatch->landing, effectiveBatch->inputs, effectiveBatch->pinnedCopyLease, effectiveBatch->sharedMoveLease);
        else if (fileOle)
            outcome = ScheduledV4Executor::RunFile(state_, *round.familyIndex, *round.caseIndex, *attempt->receipt,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_,
                *file->root, *file->fixture, file->sourceRoot, file->sourceRootId, file->landing, file->inputs);
        else if (round.action == v3_scheduler::V4RoundAction::ole_candidate)
            outcome = ScheduledV4Executor::Run(state_, *round.familyIndex, *round.caseIndex, *attempt->receipt,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_);
        else if (round.action == v3_scheduler::V4RoundAction::native_candidate)
            outcome = ScheduledV4Executor::RunNative(state_, *round.familyIndex, *round.caseIndex, *attempt->receipt,
                executionId, coordinateBudget, axisBudget, replayWorkBudget, byteBudget_,
                *native->root, *native->fixture, native->landing, native->targetScreen, native->points);
        if (!outcome || !outcome->Valid()) { result.disposition = "outcome_unknown"; return result; }
        const auto& facts = outcome->Facts();
        result.productionStarted = facts.productionStarted; result.productionEntryUnknown = facts.productionEntryUnknown;
        result.eventFailure = facts.eventOracleFailureObserved; result.businessFailure = facts.businessFailureObserved;
        result.eventDispatchObservationRecorded = facts.eventDispatchObservationRecorded;
        result.eventDispatchFailure = facts.eventDispatchFailureObserved;
        result.hypothesisObservationRecorded = facts.hypothesisObservationRecorded;
        result.hypothesisNotMet = facts.hypothesisNotMetObserved;
        result.observationUnavailable = facts.observationUnavailable; result.detail = facts.failure;
        result.outcomeStatus = facts.status;
        auto committed = store_.CommitOutcome(*attempt->receipt, *outcome, byteBudget_);
        result.outcomePublication.emplace(std::move(committed.checkpoint));
        if (!committed.resultingState || result.outcomePublication->status != v3_scheduler::StoreStatus::published_durability_unverified)
        { result.disposition = "outcome_commit_unavailable"; return result; }
        if (!committed.committedV4 || !handoffs->afterCommit(*committed.committedV4))
        { result.disposition = "outcome_handoff_unknown"; return result; }
        headName_ = result.outcomePublication->fileName; headId_ = result.outcomePublication->fileId;
        state_ = *committed.resultingState; healthy_ = CheckHead();
        result.disposition = healthy_ ? "outcome_recorded_unresolved" : "outcome_readback_unavailable";
        return result;
    }
    catch (...)
    {
        healthy_ = false;
        if (proposalStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
        result.disposition = "foreground_runner_unavailable";
        return result;
    }
}

ForegroundV4Runner::StepResult ForegroundV4Runner::AddTextSeed(
    std::size_t familyIndex, const Candidate& candidate, const std::vector<bool>& present,
    std::string_view graphId, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget)
{
    StepResult result;
    bool proposalStarted = false;
    lastPublication_ = {};
    try
    {
        if (!healthy_ || !CheckHead()) { healthy_ = false; result.disposition = "head_unavailable"; return result; }
        const auto seed = v3_scheduler::PrepareV4DirectSeed(state_, binding_, familyIndex, candidate, present,
            graphId, coordinateBudget, axisBudget, byteBudget_);
        if (!seed.proposed || !seed.caseIndex)
        { result.disposition = "seed_unavailable"; result.detail = seed.reason; return result; }
        proposalStarted = true;
        const bool published = PublishChecked(*seed.proposed);
        result.proposalPublication.emplace(std::move(lastPublication_));
        if (!published) { healthy_ = false; result.disposition = "seed_publication_unavailable"; return result; }
        result.familyIndex = familyIndex; result.caseIndex = *seed.caseIndex;
        result.candidateId = state_.families.at(familyIndex).cases.at(*seed.caseIndex).candidateId;
        result.graphId = state_.families.at(familyIndex).cases.at(*seed.caseIndex).graphId;
        result.disposition = "seed_published_unexecuted";
        return result;
    }
    catch (...)
    {
        healthy_ = false;
        if (proposalStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
        result.disposition = "seed_unavailable";
        return result;
    }
}

ForegroundV4Runner::StepResult ForegroundV4Runner::AddStructuredCase(
    std::string_view caseId, const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t tokenBudget, std::size_t replayWorkBudget)
{
    StepResult result;
    bool publicationStarted = false;
    lastPublication_ = {};
    try
    {
        if (!healthy_ || !CheckHead()) { healthy_ = false; result.disposition = "head_unavailable"; return result; }
        const auto proposal = v3_scheduler::PrepareV4StructuredCase(state_, binding_, caseId, coordinateBudget,
            axisBudget, tokenBudget, byteBudget_, byteBudget_, replayWorkBudget);
        result.detail = proposal.reason;
        result.familyIndex = proposal.familyIndex; result.caseIndex = proposal.caseIndex;
        if (proposal.status == v3_scheduler::V4StructuredCaseProposalStatus::already_recorded)
        { result.disposition = "case_already_recorded"; result.caseId = caseId; return result; }
        if (proposal.status != v3_scheduler::V4StructuredCaseProposalStatus::proposed || !proposal.proposed ||
            !proposal.familyIndex || !proposal.caseIndex)
        {
            result.disposition = proposal.status == v3_scheduler::V4StructuredCaseProposalStatus::resource_unavailable
                ? "case_registration_resource_unavailable" : "case_registration_unavailable";
            return result;
        }
        publicationStarted = true;
        const bool published = PublishChecked(*proposal.proposed);
        result.proposalPublication.emplace(std::move(lastPublication_));
        if (!published) { healthy_ = false; result.disposition = "case_publication_unavailable"; return result; }
        const auto& recorded = state_.families.at(*proposal.familyIndex).cases.at(*proposal.caseIndex);
        result.caseId = caseId; result.candidateId = recorded.candidateId; result.graphId = recorded.graphId;
        result.disposition = "case_published_unexecuted";
        return result;
    }
    catch (...)
    {
        healthy_ = false;
        if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
        result.disposition = "case_registration_unavailable";
        return result;
    }
}

ForegroundV4Runner::StepResult ForegroundV4Runner::PrepareGlobalCycle(
    std::size_t decimalDigitBudget, std::size_t memberBudget, std::size_t workBudget,
    const V4CoordinateBudget& coordinateBudget, const V4AxisBudget& axisBudget,
    std::size_t replayWorkBudget, std::size_t tokenBudget, std::size_t caseByteBudget)
{
    StepResult result;
    bool publicationStarted = false;
    try
    {
        if (!healthy_ || !CheckHead()) { healthy_ = false; result.disposition = "head_unavailable"; return result; }
        const auto cycle = v3_scheduler::PrepareV4ForegroundCycle(state_, binding_, decimalDigitBudget, memberBudget,
            workBudget, coordinateBudget, axisBudget, replayWorkBudget, tokenBudget, caseByteBudget, byteBudget_);
        if (!cycle.round)
        { result.disposition = cycle.status == v3_scheduler::V4ForegroundCycleStatus::resource_unavailable
                ? "global_cycle_resource_unavailable" : "global_cycle_unavailable";
          result.detail = std::string(cycle.reason); return result; }
        publicationStarted = true;
        const bool published = PublishChecked(cycle.round->visit.tick.proposed);
        result.proposalPublication.emplace(std::move(lastPublication_));
        if (!published) { healthy_ = false; result.disposition = "global_cycle_publication_unavailable"; return result; }
        result.familyIndex = cycle.round->familyIndex; result.caseIndex = cycle.round->caseIndex;
        result.candidateId = cycle.round->candidateId; result.graphId = cycle.round->graphId;
        result.detail = std::string(cycle.reason);
        if (cycle.status == v3_scheduler::V4ForegroundCycleStatus::owned_file_template && !cycle.templateCaseId.empty())
        { result.caseId = cycle.templateCaseId; result.disposition = "global_cycle_template_published_unexecuted"; }
        else result.disposition = cycle.status == v3_scheduler::V4ForegroundCycleStatus::resource_unavailable
            ? "global_cycle_published_resource_pending" : "global_cycle_published_unexecuted";
    }
    catch (const std::bad_alloc&)
    {
        healthy_ = false;
        if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
        result.disposition = publicationStarted ? "global_cycle_publication_unavailable" : "global_cycle_resource_unavailable";
    }
    catch (const std::length_error&)
    {
        healthy_ = false;
        if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
        result.disposition = publicationStarted ? "global_cycle_publication_unavailable" : "global_cycle_resource_unavailable";
    }
    catch (...)
    {
        healthy_ = false;
        if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
        result.disposition = publicationStarted ? "global_cycle_publication_unavailable" : "global_cycle_unavailable";
    }
    return result;
}

ForegroundV4Runner::StepResult ForegroundV4Runner::RegisterOwnedFileFamily(
    const V4OwnedFileFamilySeed& seed, const V4CoordinateBudget& coordinateBudget)
{
    StepResult result;
    bool publicationStarted = false;
    try
    {
        if (!healthy_ || !CheckHead()) { healthy_ = false; result.disposition = "head_unavailable"; return result; }
        const auto registration = v3_scheduler::PrepareV4OwnedFamilyRegistration(state_, binding_, seed, coordinateBudget, byteBudget_);
        result.familyIndex = registration.familyIndex;
        if (registration.status == v3_scheduler::V4OwnedFamilyStatus::already_registered && registration.familyIndex)
        { result.disposition = "owned_family_already_registered"; return result; }
        if (registration.status != v3_scheduler::V4OwnedFamilyStatus::proposed || !registration.proposed || !registration.familyIndex)
        { result.disposition = registration.status == v3_scheduler::V4OwnedFamilyStatus::resource_unavailable
                ? "owned_family_resource_unavailable" : "owned_family_unavailable";
          result.detail = std::string(registration.reason); return result; }
        publicationStarted = true;
        const bool published = PublishChecked(*registration.proposed);
        result.proposalPublication.emplace(std::move(lastPublication_));
        if (!published || !seed.StillOwned())
        { healthy_ = false; result.disposition = "owned_family_publication_unavailable"; return result; }
        result.disposition = "owned_family_registered_unexecuted";
    }
    catch (const std::bad_alloc&)
    { healthy_ = false;
      if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
      result.disposition = publicationStarted ? "owned_family_publication_unavailable" : "owned_family_resource_unavailable"; }
    catch (const std::length_error&)
    { healthy_ = false;
      if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
      result.disposition = publicationStarted ? "owned_family_publication_unavailable" : "owned_family_resource_unavailable"; }
    catch (...)
    { healthy_ = false;
      if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
      result.disposition = "owned_family_unavailable"; }
    return result;
}

ForegroundV4Runner::StepResult ForegroundV4Runner::VisitOwnedFileFamily(
    const V4OwnedFileFamilySeed& seed, std::size_t familyIndex, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t workBudget)
{
    StepResult result;
    bool publicationStarted = false;
    try
    {
        if (!healthy_ || !CheckHead()) { healthy_ = false; result.disposition = "head_unavailable"; return result; }
        if (familyIndex >= state_.families.size()) { result.disposition = "owned_family_unavailable"; return result; }
        const auto previousCount = state_.families[familyIndex].cases.size();
        const auto visit = v3_scheduler::PrepareV4OwnedFamilyVisit(state_, binding_, seed, familyIndex,
            coordinateBudget, axisBudget, workBudget, byteBudget_);
        if (visit.status != v3_scheduler::V4OwnedFamilyStatus::proposed || !visit.proposal)
        { result.disposition = visit.status == v3_scheduler::V4OwnedFamilyStatus::resource_unavailable
                ? "owned_visit_resource_unavailable" : "owned_visit_unavailable";
          result.detail = std::string(visit.reason); return result; }
        publicationStarted = true;
        const bool published = PublishChecked(visit.proposal->tick.proposed);
        result.proposalPublication.emplace(std::move(lastPublication_));
        if (!published || !seed.StillOwned())
        { healthy_ = false; result.disposition = "owned_visit_publication_unavailable"; return result; }
        result.familyIndex = familyIndex;
        const auto& family = state_.families.at(familyIndex);
        result.familyScanExhausted = family.scanExhausted;
        if (family.cases.size() > previousCount && family.cases.size() - previousCount == 1)
        {
            result.caseIndex = previousCount;
            result.candidateId = family.cases.back().candidateId; result.graphId = family.cases.back().graphId;
        }
        else if (family.cases.size() != previousCount)
        { healthy_ = false; result.disposition = "owned_visit_case_count_unavailable"; return result; }
        result.disposition = "owned_visit_published_unexecuted";
    }
    catch (const std::bad_alloc&)
    { healthy_ = false;
      if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
      result.disposition = publicationStarted ? "owned_visit_publication_unavailable" : "owned_visit_resource_unavailable"; }
    catch (const std::length_error&)
    { healthy_ = false;
      if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
      result.disposition = publicationStarted ? "owned_visit_publication_unavailable" : "owned_visit_resource_unavailable"; }
    catch (...)
    { healthy_ = false;
      if (publicationStarted && !result.proposalPublication) result.proposalPublication.emplace(std::move(lastPublication_));
      result.disposition = "owned_visit_unavailable"; }
    return result;
}

ForegroundV4Runner::StepResult ForegroundV4Runner::AddOwnedSingleFileCase(
    const V4OwnedFileFamilySeed& seed, const std::vector<std::pair<std::string, std::string>>& choices,
    const std::vector<bool>& present, std::string_view graphId, const V4CoordinateBudget& coordinateBudget,
    const V4AxisBudget& axisBudget, std::size_t tokenBudget, std::size_t replayWorkBudget)
{
    StepResult result;
    try
    {
        if (!seed.StillOwned()) { result.disposition = "owned_seed_unavailable"; return result; }
        const auto candidate = PrepareV4OwnedFileCandidate(seed, axisBudget, choices);
        if (!candidate.ready || candidate.resourceUnavailable)
        {
            result.disposition = candidate.resourceUnavailable ? "owned_candidate_resource_unavailable" : "owned_candidate_unavailable";
            if (!candidate.unresolved.empty()) result.detail = candidate.unresolved.front();
            return result;
        }
        const auto prefix = EncodeV4StructuredPrefix(seed.Family(), tokenBudget);
        if (!prefix.encoded)
        { result.disposition = prefix.resourceUnavailable ? "owned_case_resource_unavailable" : "owned_case_unavailable"; return result; }
        const auto id = v3_scheduler::MakeV4StructuredCaseId(prefix.tokens, candidate.candidate, present,
            "ole", graphId, coordinateBudget, axisBudget, tokenBudget, byteBudget_);
        if (id.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || id.candidateId != candidate.candidateId ||
            !seed.StillOwned())
        { result.disposition = id.status == v3_scheduler::V4StructuredCaseStatus::resource_unavailable
                ? "owned_case_resource_unavailable" : "owned_case_unavailable"; return result; }
        return AddStructuredCase(id.caseId, coordinateBudget, axisBudget, tokenBudget, replayWorkBudget);
    }
    catch (const std::bad_alloc&)
    { result.disposition = "owned_case_resource_unavailable"; return result; }
    catch (const std::length_error&)
    { result.disposition = "owned_case_resource_unavailable"; return result; }
    catch (...)
    { result.disposition = "owned_case_unavailable"; return result; }
}

bool V4ProfileBoundToChildIsolation(const behavior::OwnedProfileDesktopRoot& root,
    const virtual_staging_fixture::ChildIsolation& isolation)
{
    if (!root.StillOwned() || !root.profile || !isolation.StillOwned()) return false;
    const auto expected = isolation.RootIdentity();
    if (!expected) return false;
    const auto parentPath = root.profile->root.parent_path();
    HANDLE parent = CreateFileW(parentPath.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (parent == INVALID_HANDLE_VALUE) return false;
    FILE_ID_INFO actual{}; FILE_ATTRIBUTE_TAG_INFO tag{};
    const bool matched = GetFileInformationByHandleEx(parent, FileIdInfo, &actual, sizeof(actual)) &&
        GetFileInformationByHandleEx(parent, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        behavior::OwnedProfileDesktopRoot::SameId(actual, *expected) && isolation.StillOwned() && root.StillOwned();
    CloseHandle(parent);
    return matched;
}

int OwnedFileChildWork(virtual_child_protocol::Child& child,
    virtual_staging_fixture::ChildIsolation& isolation, std::string_view nonce)
{
    const auto pending = [&](std::string_view reason) { return child.PublishPending(isolation, reason) ? 2 : 3; };
    bool mayHaveDispatched = false;
    ForegroundV4Runner::BatchInputs* retainedInput = nullptr;
    try
    {
        const auto& budgets = child.WorkBudgets();
        const auto templateCase = v3_scheduler::ReplayV4StructuredCaseId(child.CaseId(), budgets.coordinates,
            budgets.axes, budgets.tokenBudget, budgets.caseByteBudget);
        if (templateCase.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || !templateCase.family ||
            !templateCase.oleGraph || templateCase.family->b || templateCase.family->a.kind != V4SourceKind::file_only ||
            templateCase.family->a.fileCount.kind != V3CountKind::exact || templateCase.family->a.fileCount.decimal != "1")
            return pending("v4_owned_template_unavailable");
        auto uniqueRoot = behavior::CreateV4OwnedProfileDesktopRoot();
        std::shared_ptr<behavior::OwnedProfileDesktopRoot> root(std::move(uniqueRoot));
        if (!root || !root->StillOwned() || !root->profile || !root->profile->app)
            return pending("v4_owned_profile_unavailable");
        // The isolated profile starts with fixture dispatch only. Route both
        // completion windows through the real host procedures before any
        // generated drag is prepared or submitted.
        behavior::Access::EnableProductionDispatch(*root->profile->app, false);
        behavior::Access::EnableFixtureControlWindow(*root->profile->app);
        auto sourceRoot = std::make_shared<V4OwnedChildDirectoryAttempt>();
        auto sourceCreation = CreateV4OwnedChildDirectory(root->root, root->rootId, L"source");
        sourceRoot->handle = std::exchange(sourceCreation.handle, INVALID_HANDLE_VALUE);
        sourceRoot->identity = sourceCreation.identity; sourceRoot->physicalPath = std::move(sourceCreation.physicalPath);
        sourceRoot->attempted = sourceCreation.attempted; sourceRoot->created = sourceCreation.created;
        sourceRoot->identityObserved = sourceCreation.identityObserved; sourceRoot->available = sourceCreation.available;
        sourceRoot->reason = sourceCreation.reason;
        if (!sourceRoot->available || !sourceRoot->created || !sourceRoot->identityObserved)
            return pending(sourceRoot->attempted ? "v4_owned_source_root_reconciliation_required" : "v4_owned_source_root_unavailable");
        auto draft = templateCase.family->a; draft.filePaths.clear();
        const auto createdSeed = PrepareV4OwnedFileFamilySeed(draft, sourceRoot->physicalPath, child.OwnedLeaf(),
            sourceRoot->handle, sourceRoot->identity, budgets.coordinates, budgets.axes);
        if (!createdSeed.seed || createdSeed.resourceUnavailable)
            return pending(createdSeed.resourceUnavailable ? "v4_owned_family_resource_unavailable" : "v4_owned_family_unavailable");
        ingress_adapter::V4OwnedTemplateCasePage selected;
        event_graph::V4JoinedGraphCursor cursor;
        const auto pageBudget = (std::min)(budgets.coordinates.arithmeticSteps, static_cast<std::size_t>(4096));
        if (!pageBudget) return pending("v4_owned_graph_resource_unavailable");
        for (;;)
        {
            auto page = ingress_adapter::PrepareV4OwnedTemplateCasePage(child.CaseId(), *createdSeed.seed,
                budgets.coordinates, budgets.axes, budgets.tokenBudget, budgets.caseByteBudget, std::move(cursor), pageBudget);
            if (page.matched) { selected = std::move(page); break; }
            if (page.resourceUnavailable) return pending("v4_owned_graph_resource_unavailable");
            if (page.exhausted || page.reason != "v4_owned_template_graph_pending" || !page.cursorAdvanced)
                return pending("v4_owned_graph_selection_unavailable");
            if (!child.ReportOwnedScanProgress(*createdSeed.seed, page.cursorAdvanced, page.workUsed)) return 3;
            cursor = std::move(page.cursor);
        }
        if (selected.caseId.empty()) return pending("v4_owned_graph_selection_unavailable");
        if (!child.OfferOwnedCase(isolation, root->root, root->rootId, *createdSeed.seed, selected.caseId)) return 3;
        const auto until = GetTickCount64() + 30000;
        bool accepted = false;
        while (GetTickCount64() < until)
        {
            if (child.AcceptOwnedCase()) { accepted = true; break; }
            if (child.OwnedCaseBudgetRejected()) return pending("v4_owned_parent_case_budget_unavailable");
            Sleep(10);
        }
        if (!accepted || child.CaseId() != selected.caseId || !createdSeed.seed->StillOwned() || !root->StillOwned()) return 3;
        const auto rootId = isolation.RootIdentity(); const auto rootHandle = isolation.RootHandle();
        if (!rootId || !rootHandle) return pending("v4_owned_checkpoint_root_unavailable");
        v3_scheduler::CheckpointLocator locator; v3_scheduler::CheckpointCreateAttempt createAttempt;
        auto store = v3_scheduler::CheckpointStore::Create(rootHandle, *rootId, locator, createAttempt);
        if (!store) return pending("v4_owned_checkpoint_store_unavailable");
        const std::string binding = std::string("v4_virtual_child_v1:") + child.CaseId();
        v3_scheduler::PublishedCheckpoint initial;
        auto runner = ForegroundV4Runner::Start(*store, binding, virtual_child_protocol::kLimit, initial);
        if (!runner) return pending("v4_owned_checkpoint_initial_unavailable");
        const auto registration = runner->AddStructuredCase(child.CaseId(), budgets.coordinates, budgets.axes,
            budgets.tokenBudget, budgets.coordinates.arithmeticSteps);
        if (registration.disposition != "case_published_unexecuted" || !registration.familyIndex || !registration.caseIndex)
            return pending("v4_owned_case_registration_unavailable");
        ForegroundV4Runner::BatchInputs input;
        input.ownedRoot = root; input.root = root.get(); input.sourceRoot = sourceRoot;
        input.familySeed = createdSeed.seed; input.prepareSingleFromModel = true;
        const auto awaitAcknowledgement = [](const auto& acknowledged) {
            const auto deadline = GetTickCount64() + 30000;
            while (!acknowledged() && GetTickCount64() < deadline) Sleep(10);
            return acknowledged();
        };
        const ForegroundV4Runner::AttemptHandoffs handoffs{
            [&](const v3_scheduler::InFlightReceipt& receipt) {
                return child.OfferAttempt(receipt) && awaitAcknowledgement([&] { return child.AttemptAcknowledged(); });
            },
            [&](const v3_scheduler::CommittedV4Receipt& receipt) {
                return child.OfferTerminal(receipt) && awaitAcknowledgement([&] { return child.TerminalAcknowledged(); });
            }};
        // This permits only the runner's separately checked, isolated
        // single-file copy attempt. It does not certify its input coverage or
        // business result and does not enable the virtual-file entry.
        const bool authorized = child.Valid() && child.IsOwnedFileRequest() && V4ProfileBoundToChildIsolation(*root, isolation) &&
            createdSeed.seed->StillOwned() && child.CaseId() == selected.caseId;
        if (authorized) retainedInput = new ForegroundV4Runner::BatchInputs(input);
        mayHaveDispatched = authorized;
        const auto step = runner->Step(budgets.coordinates.decimalDigits, budgets.axes.logicalMembers,
            budgets.coordinates.arithmeticSteps, budgets.coordinates, budgets.axes, budgets.coordinates.arithmeticSteps,
            budgets.tokenBudget, nonce, nullptr, nullptr, &input, nullptr, nullptr,
            std::make_pair(*registration.familyIndex, *registration.caseIndex), authorized, &handoffs);
        if (step.productionStarted || step.productionEntryUnknown || step.attemptPublication)
        {
            const int status = child.TerminalAcknowledged() && child.PublishCheckpointReport(isolation) ? 2 : 3;
            // Keep the isolated host and source alive through process exit;
            // an idle observation alone does not prove that no work arrives later.
            ExitProcess(static_cast<UINT>(status));
        }
        mayHaveDispatched = false;
        delete retainedInput; retainedInput = nullptr;
        if (step.caseId != child.CaseId()) return pending("v4_owned_case_identity_unavailable");
        return pending(step.disposition);
    }
    catch (...)
    {
        if (mayHaveDispatched) ExitProcess(3);
        delete retainedInput;
        return pending("v4_owned_preparation_exception");
    }
}

// Drive one original native case in its own isolated child. The result stays
// unavailable until the adapter has completed the event and business oracle.
int NativeChildWork(virtual_child_protocol::Child& child,
    virtual_staging_fixture::ChildIsolation& isolation, std::string_view nonce,
    const v3_scheduler::V4StructuredCaseReplay& replay)
{
    const auto pending = [&](std::string_view reason) { return child.PublishPending(isolation, reason) ? 2 : 3; };
    bool mayHaveDispatched = false;
    struct RetainedInput
    {
        std::unique_ptr<behavior::OwnedProfileDesktopRoot> root;
        V4SingleFileFixture fixture;
    };
    RetainedInput* retainedInput = nullptr;
    try
    {
        const auto& budgets = child.WorkBudgets();
        if (replay.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || !replay.family || !replay.candidate ||
            !replay.nativeGraph || replay.oleGraph || replay.caseId != child.CaseId() || replay.family->b)
            return pending("v4_native_child_case_unavailable");
        const auto identity = event_graph::V4FamilyIdentityContext::Create(*replay.family, budgets.axes);
        const auto verified = V4FamilyValidatedContext::Create(*replay.family, budgets.axes, *replay.candidate);
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready)
            return pending("v4_native_child_candidate_unavailable");
        auto root = behavior::CreateV4OwnedProfileDesktopRoot();
        if (!root || !root->StillOwned() || !root->profile || !root->profile->app)
            return pending("v4_native_child_profile_unavailable");
        auto& app = *root->profile->app;
        behavior::Access::EnableProductionDispatch(app, false);
        behavior::Access::EnableFixtureControlWindow(app);
        behavior::Access::EnableNativePointerDispatch(app);
        std::shared_ptr<const V4OwnedRelativePathBinding> relative;
        if (!replay.family->a.filePaths.empty() &&
            replay.family->a.filePaths.front().base == V4PathBase::owned_root_relative)
        {
            const auto binding = ObserveV4OwnedRelativePaths(*replay.family, verified, identity,
                root->profile->desktop, root->desktop, root->desktopId, budgets.axes.fileMembers, budgets.axes.payloadBytes);
            if (!binding.binding || binding.resourceUnavailable)
                return pending(binding.resourceUnavailable ? "v4_native_relative_resource_unavailable" : "v4_native_relative_input_unavailable");
            relative = binding.binding;
        }
        auto fixture = PrepareV4PayloadFixture(verified, identity, root->profile->desktop,
            root->desktop, root->desktopId, relative);
        if (!fixture.inputPrepared || !fixture.created || !fixture.fileIdentityObserved)
            return pending("v4_native_child_fixture_unavailable");
        behavior::Access::PrepareNativeMultiDesktop(app, {fixture.file, root->profile->desktop / L"Sentinel.txt"});
        const GridCell landing{L"__page:1", 3, 0};
        const auto target = behavior::Access::InspectNativeDesktopLanding(app, landing);
        const auto rootId = isolation.RootIdentity(); const auto rootHandle = isolation.RootHandle();
        if (!rootId || !rootHandle) return pending("v4_native_child_checkpoint_root_unavailable");
        v3_scheduler::CheckpointLocator locator; v3_scheduler::CheckpointCreateAttempt createAttempt;
        auto store = v3_scheduler::CheckpointStore::Create(rootHandle, *rootId, locator, createAttempt);
        if (!store) return pending("v4_native_child_checkpoint_unavailable");
        const std::string binding = std::string("v4_virtual_child_v1:") + child.CaseId();
        v3_scheduler::PublishedCheckpoint initial;
        auto runner = ForegroundV4Runner::Start(*store, binding, virtual_child_protocol::kLimit, initial);
        if (!runner) return pending("v4_native_child_runner_unavailable");
        const auto registration = runner->AddStructuredCase(child.CaseId(), budgets.coordinates, budgets.axes,
            budgets.tokenBudget, budgets.coordinates.arithmeticSteps);
        if (registration.disposition != "case_published_unexecuted" || !registration.familyIndex || !registration.caseIndex)
            return pending("v4_native_child_registration_unavailable");
        retainedInput = new RetainedInput{std::move(root), std::move(fixture)};
        ForegroundV4Runner::NativeInputs input{retainedInput->root.get(), &retainedInput->fixture, landing, target.screenCenter, {}};
        const auto awaitAcknowledgement = [](const auto& acknowledged) {
            const auto deadline = GetTickCount64() + 30000;
            while (!acknowledged() && GetTickCount64() < deadline) Sleep(10);
            return acknowledged();
        };
        const ForegroundV4Runner::AttemptHandoffs handoffs{
            [&](const v3_scheduler::InFlightReceipt& receipt) {
                return child.OfferAttempt(receipt) && awaitAcknowledgement([&] { return child.AttemptAcknowledged(); });
            },
            [&](const v3_scheduler::CommittedV4Receipt& receipt) {
                return child.OfferTerminal(receipt) && awaitAcknowledgement([&] { return child.TerminalAcknowledged(); });
            }};
        const bool authorized = child.Valid() && V4ProfileBoundToChildIsolation(*retainedInput->root, isolation) &&
            retainedInput->fixture.candidateId == replay.candidateId && (!relative || relative->StillBound());
        mayHaveDispatched = authorized;
        const auto step = runner->Step(budgets.coordinates.decimalDigits, budgets.axes.logicalMembers,
            budgets.coordinates.arithmeticSteps, budgets.coordinates, budgets.axes, budgets.coordinates.arithmeticSteps,
            budgets.tokenBudget, nonce, &input, nullptr, nullptr, nullptr, nullptr,
            std::make_pair(*registration.familyIndex, *registration.caseIndex), authorized, &handoffs);
        if (step.productionStarted || step.productionEntryUnknown || step.attemptPublication)
        {
            const int status = child.TerminalAcknowledged() && child.PublishCheckpointReport(isolation) ? 2 : 3;
            ExitProcess(static_cast<UINT>(status));
        }
        mayHaveDispatched = false;
        delete retainedInput; retainedInput = nullptr;
        if (step.caseId != child.CaseId()) return pending("v4_native_child_case_identity_unavailable");
        return pending(step.disposition);
    }
    catch (...)
    {
        if (mayHaveDispatched) ExitProcess(3);
        delete retainedInput;
        return pending("v4_native_child_preparation_exception");
    }
}

// This dedicated process enters before any profile, worker or staging use.
// Until same-case execution and checkpoint handoff are wired, report only a
// pending case; process success is never a business result.
int VirtualChildEntry()
{
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    struct Args final { LPWSTR* value; ~Args() { if (value) LocalFree(value); } } args{raw};
    if (!raw || count != 4 || (std::wstring_view(raw[1]) != L"--v4-virtual-staging-child" &&
        std::wstring_view(raw[1]) != L"--v4-owned-file-child")) return 2;
    const bool ownedFile = std::wstring_view(raw[1]) == L"--v4-owned-file-child";
    const std::wstring_view wideNonce(raw[2]);
    if (wideNonce.size() != 32 || std::any_of(wideNonce.begin(), wideNonce.end(), [](wchar_t c) {
        return !((c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'f')); })) return 2;
    std::string nonce(wideNonce.begin(), wideNonce.end());
    const std::wstring_view handleText(raw[3]);
    if (handleText.empty() || (handleText.size() > 1 && handleText.front() == L'0')) return 2;
    std::uintptr_t handleValue = 0;
    for (const auto ch : handleText)
    {
        if (ch < L'0' || ch > L'9') return 2;
        const auto digit = static_cast<std::uintptr_t>(ch - L'0');
        if (handleValue > ((std::numeric_limits<std::uintptr_t>::max)() - digit) / 10) return 2;
        handleValue = handleValue * 10 + digit;
    }
    if (!handleValue || handleValue == reinterpret_cast<std::uintptr_t>(INVALID_HANDLE_VALUE)) return 2;
    auto isolation = virtual_staging_fixture::ChildIsolation::Prepare(wideNonce);
    if (!isolation || !isolation->ready || !isolation->StillOwned()) return 2;
    auto child = virtual_child_protocol::Child::Attach(reinterpret_cast<HANDLE>(handleValue), nonce, *isolation);
    if (!child || !child->Valid() || child->IsOwnedFileRequest() != ownedFile || !child->PublishRoot(*isolation)) return 2;
    const auto deadline = GetTickCount64() + 30000;
    while (!child->RootAcknowledged() && GetTickCount64() < deadline) Sleep(10);
    if (!child->RootAcknowledged()) return 2;
    // Record the independently bound diagnostic configuration attempt. It
    // does not grant permission to dispatch a production operation.
    static_cast<void>(child->ConfigureExternalDiagnostics(*isolation));
    if (ownedFile) return OwnedFileChildWork(*child, *isolation, nonce);
    if (child->IsNativeGraphRequest())
    {
        const auto& budgets = child->WorkBudgets();
        const auto replay = v3_scheduler::ReplayV4StructuredCaseId(child->CaseId(), budgets.coordinates,
            budgets.axes, budgets.tokenBudget, budgets.caseByteBudget);
        return NativeChildWork(*child, *isolation, nonce, replay);
    }
    bool mayHaveDispatched = false;
    const auto pending = [&](std::string_view reason) { return child->PublishPending(*isolation, reason) ? 2 : 3; };
    try
    {
        const auto& budgets = child->WorkBudgets();
        const auto replay = v3_scheduler::ReplayV4StructuredCaseId(child->CaseId(), budgets.coordinates, budgets.axes,
            budgets.tokenBudget, budgets.caseByteBudget);
        if (replay.status != v3_scheduler::V4StructuredCaseStatus::unexecuted || !replay.family || !replay.candidate ||
            !replay.oleGraph || replay.family->a.kind != V4SourceKind::content_only)
            return pending("v4_child_virtual_candidate_unavailable");
        const auto identity = event_graph::V4FamilyIdentityContext::Create(*replay.family, budgets.axes);
        const auto verified = V4FamilyValidatedContext::Create(*replay.family, budgets.axes, *replay.candidate);
        if (identity.Status() != V4FamilyStatus::schema_only || verified.Status() != V4CoordinateStatus::coordinate_ready ||
            Value(verified.Axes(), verified.Coordinate(), "T02.virtual_files") != "yes")
            return pending("v4_child_virtual_input_unavailable");
        const auto rootId = isolation->RootIdentity(); const auto rootHandle = isolation->RootHandle();
        if (!rootId || !rootHandle) return pending("v4_child_checkpoint_root_unavailable");
        v3_scheduler::CheckpointLocator locator; v3_scheduler::CheckpointCreateAttempt createAttempt;
        auto store = v3_scheduler::CheckpointStore::Create(rootHandle, *rootId, locator, createAttempt);
        if (!store) return pending("v4_child_checkpoint_store_unavailable");
        const std::string binding = std::string("v4_virtual_child_v1:") + child->CaseId();
        v3_scheduler::PublishedCheckpoint initial;
        auto runner = ForegroundV4Runner::Start(*store, binding, virtual_child_protocol::kLimit, initial);
        if (!runner) return pending("v4_child_checkpoint_initial_unavailable");
        const auto registration = runner->AddStructuredCase(child->CaseId(), budgets.coordinates, budgets.axes,
            budgets.tokenBudget, budgets.coordinates.arithmeticSteps);
        if (registration.disposition != "case_published_unexecuted" || !registration.familyIndex || !registration.caseIndex)
            return pending("v4_child_case_registration_unavailable");
        auto root = behavior::CreateV4OwnedProfileDesktopRoot();
        if (!root || !root->profile || !root->profile->app || !root->StillOwned())
            return pending("v4_child_profile_unavailable");
        behavior::Access::EnableProductionDispatch(*root->profile->app, false);
        behavior::Access::EnableFixtureControlWindow(*root->profile->app);
        const auto geometry = behavior::PrepareV4VirtualDesktopGeometry(*root, verified, identity, POINT{150, 150});
        if (!geometry.prepared || geometry.resourceUnavailable || geometry.candidateId != replay.candidateId)
            return pending("v4_child_geometry_unavailable");
        auto input = PrepareV4ModelOleInputs(verified, identity, geometry.pointsByProgramIndex);
        if (!input.prepared || input.resourceUnavailable || input.candidateId != replay.candidateId)
            return pending("v4_child_ole_input_unavailable");
        ForegroundV4Runner::VirtualInputs virtualInput;
        virtualInput.root = root.get(); virtualInput.landing = geometry.expectedCell; virtualInput.inputs = std::move(input.inputs);
        virtualInput.contentByteBudget = budgets.axes.payloadBytes; virtualInput.virtualBudget = budgets.axes.virtualMembers;
        const auto awaitAcknowledgement = [](const auto& acknowledged) {
            const auto until = GetTickCount64() + 30000;
            while (!acknowledged() && GetTickCount64() < until) Sleep(10);
            return acknowledged();
        };
        const ForegroundV4Runner::AttemptHandoffs handoffs{
            [&](const v3_scheduler::InFlightReceipt& receipt) {
                return child->OfferAttempt(receipt) && awaitAcknowledgement([&] { return child->AttemptAcknowledged(); });
            },
            [&](const v3_scheduler::CommittedV4Receipt& receipt) {
                return child->OfferTerminal(receipt) && awaitAcknowledgement([&] { return child->TerminalAcknowledged(); });
            }};
        const bool authorized = isolation->ExecutionAuthorized();
        mayHaveDispatched = authorized;
        const auto step = runner->Step(budgets.coordinates.decimalDigits, budgets.axes.logicalMembers,
            budgets.coordinates.arithmeticSteps, budgets.coordinates, budgets.axes, budgets.coordinates.arithmeticSteps,
            budgets.tokenBudget, nonce, nullptr, nullptr, nullptr, nullptr, &virtualInput,
            std::make_pair(*registration.familyIndex, *registration.caseIndex), authorized, &handoffs);
        // The protocol does not yet authenticate a sealed outcome. Once an
        // execution is authorized, even an apparently empty result stays
        // unknown rather than being relabeled as an unexecuted case.
        if (authorized || step.productionStarted || step.productionEntryUnknown)
            return child->TerminalAcknowledged() && child->PublishCheckpointReport(*isolation) ? 2 : 3;
        mayHaveDispatched = false;
        if (step.caseId != child->CaseId()) return pending("v4_child_case_identity_unavailable");
        return pending(step.disposition);
    }
    catch (...)
    {
        return mayHaveDispatched ? 3 : pending("v4_child_preparation_exception");
    }
}

// A process exit alone cannot authenticate a product result. The current
// protocol returns only pending/unknown for a selected structured case.
int VirtualParentEntry()
{
    int count = 0;
    LPWSTR* raw = CommandLineToArgvW(GetCommandLineW(), &count);
    struct Args final { LPWSTR* value; ~Args() { if (value) LocalFree(value); } } args{raw};
    if (!raw || count < 3) return 2;
    const bool ownedFile = std::wstring_view(raw[1]) == L"--global-v4-owned-file-case";
    if ((ownedFile && count != 5) || (!ownedFile && (count != 3 ||
        (std::wstring_view(raw[1]) != L"--global-v4-virtual-case" &&
         std::wstring_view(raw[1]) != L"--global-v4-native-case")))) return 2;
    if (ownedFile && std::wstring_view(raw[3]).size() > 256)
    { std::cout << "PENDING owned_file_leaf_transport_budget\n"; return 2; }
    const std::filesystem::path casePath(raw[2]);
    std::error_code error;
    const auto length = std::filesystem::file_size(casePath, error);
    if (error || !length) return 2;
    // The child transport can carry larger case bytes, but this entry still
    // uses the fixed checkpoint and report budgets below. Preserve the input
    // as pending until those budgets and the parent output root are wired.
    if (length > virtual_child_protocol::kLimit)
    { std::cout << "PENDING v4_parent_case_checkpoint_budget_unavailable\n"; return 2; }
    std::ifstream stream(casePath, std::ios::binary);
    if (!stream) return 2;
    std::string caseId(static_cast<std::size_t>(length), '\0');
    stream.read(caseId.data(), static_cast<std::streamsize>(caseId.size()));
    if (!stream || stream.peek() != std::char_traits<char>::eof() || stream.bad()) return 2;
    constexpr char digits[] = "0123456789abcdef";
    const auto wideHex = [&](std::wstring_view text) {
        for (const auto c : text) { const auto u = static_cast<unsigned short>(c);
            const unsigned lo = u & 255, hi = u >> 8;
            std::cout << digits[lo >> 4] << digits[lo & 15] << digits[hi >> 4] << digits[hi & 15]; }
    };
    const auto idHex = [&](const FILE_ID_INFO& id) {
        std::cout << id.VolumeSerialNumber << ':';
        for (const auto byte : id.FileId.Identifier) std::cout << digits[byte >> 4] << digits[byte & 15];
    };
    const auto publicationClues = [&](const v3_scheduler::PublishedCheckpoint& publication) {
        std::cout << "CHECKPOINT_CLUES temporary_attempted=" << publication.temporaryCreateAttempted
            << " temporary_created=" << publication.temporaryCreated << " identity_observed=" << publication.fileIdentityCaptured
            << " rename_attempted=" << publication.renameAttempted << " rename_succeeded=" << publication.renameReturnedSuccess
            << " temporary_utf16le_hex="; wideHex(publication.temporaryName);
        std::cout << " final_utf16le_hex="; wideHex(publication.fileName);
        if (publication.fileIdentityCaptured) { std::cout << " file_id="; idHex(publication.fileId); }
        std::cout << '\n';
    };
    std::cout << "CASE bytes=" << caseId.size();
    if (caseId.size() <= 4096)
    {
        std::cout << " id_hex=";
        for (const unsigned char byte : caseId) std::cout << digits[byte >> 4] << digits[byte & 15];
    }
    else std::cout << " id_hex=omitted_report_budget";
    std::cout << '\n';
    GUID guid{}; if (FAILED(CoCreateGuid(&guid))) return 2;
    const auto* bytes = reinterpret_cast<const unsigned char*>(&guid);
    std::string nonce; nonce.reserve(sizeof(guid) * 2);
    for (std::size_t i = 0; i < sizeof(guid); ++i)
    { nonce.push_back(digits[bytes[i] >> 4]); nonce.push_back(digits[bytes[i] & 15]); }
    const V4CoordinateBudget coordinateBudget{4096, virtual_child_protocol::kLimit, 4096, virtual_child_protocol::kLimit};
    const V4AxisBudget axisBudget{256, 256, 256, 256, 4096, 4096, 4096, 4096,
        virtual_child_protocol::kLimit, 4096, 4096, 4096};
    if (!ownedFile)
    {
        const auto replay = v3_scheduler::ReplayV4StructuredCaseId(caseId, coordinateBudget, axisBudget,
            65536, virtual_child_protocol::kLimit);
        const bool nativeCommand = std::wstring_view(raw[1]) == L"--global-v4-native-case";
        if (replay.status != v3_scheduler::V4StructuredCaseStatus::unexecuted ||
            nativeCommand != static_cast<bool>(replay.nativeGraph) ||
            nativeCommand == static_cast<bool>(replay.oleGraph))
        { std::cout << "PENDING v4_command_case_graph_unavailable\n"; return 2; }
    }
    // The parent stores the complete child checkpoint plus its own metadata.
    // This observation budget is separate from the child transport budget;
    // exceeding either remains resource-unavailable, never a product limit.
    constexpr std::size_t parentCheckpointBudget = 8u * 1024u * 1024u;
    std::optional<V4PinnedDirectoryChain> outputChain;
    std::optional<V4OwnedChildDirectoryAttempt> ledgerRoot;
    std::unique_ptr<v3_scheduler::CheckpointStore> ledgerStore;
    std::unique_ptr<ForegroundV4Runner> ledgerRunner;
    v3_scheduler::CheckpointLocator ledgerLocator;
    std::wstring ledgerHeadName; FILE_ID_INFO ledgerHeadId{};
    std::string ledgerBinding;
    std::optional<v3_scheduler::OwnedAssociationCommitResult> association;
    std::optional<v3_scheduler::OwnedTerminalCommitResult> terminal;
    std::optional<v3_scheduler::V4OwnedTerminalShardResult> terminalShard;
    std::optional<v3_scheduler::V4ShardIndexAppend> shardIndex;
    bool locatorReported = false;
    bool terminalLocatorReported = false;
    bool terminalShardLocatorReported = false;
    bool shardIndexLocatorReported = false;
    bool capacityReported = false;
    if (ownedFile)
    {
        outputChain.emplace(OpenV4PinnedLocalDirectoryChain(std::filesystem::path(raw[4])));
        if (!outputChain->available || !outputChain->ParentId())
        { std::cerr << "UNAVAILABLE owned_output_root\n"; return 2; }
        const std::wstring runName = L"SnowDragLedger-" + std::wstring(nonce.begin(), nonce.end());
        ledgerRoot.emplace(CreateV4OwnedChildDirectory(outputChain->Parent(), *outputChain->ParentId(), runName));
        if (!ledgerRoot->available || !ledgerRoot->created || !ledgerRoot->identityObserved)
        {
            std::cout << "UNKNOWN owned_ledger_root_creation attempted=" << ledgerRoot->attempted
                << " created=" << ledgerRoot->created << " identity_observed=" << ledgerRoot->identityObserved
                << " intended_utf16le_hex="; wideHex(runName);
            if (ledgerRoot->identityObserved) { std::cout << " id="; idHex(ledgerRoot->identity); }
            std::cout << '\n'; return 2;
        }
        std::cout << "LEDGER_ROOT path_utf16le_hex="; wideHex(ledgerRoot->physicalPath.native());
        std::cout << " id="; idHex(ledgerRoot->identity); std::cout << '\n';
        v3_scheduler::CheckpointCreateAttempt createAttempt;
        ledgerStore = v3_scheduler::CheckpointStore::Create(ledgerRoot->handle, ledgerRoot->identity, ledgerLocator, createAttempt);
        if (!ledgerStore)
        {
            std::cout << "UNKNOWN owned_ledger_store_creation attempted=" << createAttempt.directoryCreateAttempted
                << " created=" << createAttempt.directoryCreated << " identity_observed=" << createAttempt.directoryIdentityObserved
                << " name_utf16le_hex="; wideHex(createAttempt.directoryName);
            if (createAttempt.directoryIdentityObserved) { std::cout << " id="; idHex(createAttempt.directoryId); }
            std::cout << '\n'; return 2;
        }
        ledgerBinding = std::string("v4_owned_parent_v1:") + nonce;
        v3_scheduler::PublishedCheckpoint initial;
        ledgerRunner = ForegroundV4Runner::Start(*ledgerStore, ledgerBinding, parentCheckpointBudget, initial);
        if (!ledgerRunner) { publicationClues(initial); std::cerr << "UNKNOWN owned_ledger_initial_publication\n"; return 2; }
        const auto registered = ledgerRunner->AddStructuredCase(caseId, coordinateBudget, axisBudget,
            65536, coordinateBudget.arithmeticSteps);
        if (registered.disposition != "case_published_unexecuted" || !registered.proposalPublication ||
            !registered.proposalPublication->fileIdentityCaptured)
        { if (registered.proposalPublication) publicationClues(*registered.proposalPublication);
          std::cerr << "UNKNOWN owned_template_registration\n"; return 2; }
        ledgerHeadName = registered.proposalPublication->fileName; ledgerHeadId = registered.proposalPublication->fileId;
        // Association publication below advances the store directly. Do not
        // retain a runner that could later publish from this older head.
        ledgerRunner.reset();
    }
    auto parent = virtual_child_protocol::Parent::Start(nonce, caseId, coordinateBudget, axisBudget,
        65536, virtual_child_protocol::kLimit, ownedFile, ownedFile ? std::wstring_view(raw[3]) : std::wstring_view{});
    if (!parent) return 2;
    auto progressDeadline = GetTickCount64() + 45000;
    const auto totalDeadline = GetTickCount64() + 24ull * 60 * 60 * 1000;
    while (GetTickCount64() < progressDeadline && GetTickCount64() < totalDeadline)
    {
        (void)parent->AcknowledgeRoot();
        if (ownedFile) (void)parent->AcknowledgeOwnedCase();
        if (ownedFile && parent->OwnedCasePrepared() && !parent->OwnedCaseVerified() &&
            !parent->OwnedCaseResourceUnavailable())
        {
            const auto proof = parent->OwnedAssociationEvidence();
            if (!proof) { std::cerr << "UNKNOWN owned_association_proof\n"; return 2; }
            if (!association)
            {
                association.emplace(ledgerStore->PublishOwnedAssociation(ledgerHeadName, ledgerHeadId, ledgerBinding, proof,
                    coordinateBudget, axisBudget, 65536, virtual_child_protocol::kLimit,
                    coordinateBudget.arithmeticSteps, parentCheckpointBudget));
                if (!association->committed || !association->resultingState ||
                    !association->checkpoint.fileIdentityCaptured)
                {
                    if (association->resourceUnavailable && !association->checkpoint.temporaryCreateAttempted &&
                        !association->checkpoint.temporaryCreated && !association->checkpoint.renameAttempted &&
                        !association->checkpoint.renameReturnedSuccess && parent->RejectOwnedForResources()) continue;
                    publicationClues(association->checkpoint);
                    std::cerr << "UNKNOWN owned_association_publication\n"; return 2;
                }
                ledgerHeadName = association->checkpoint.fileName; ledgerHeadId = association->checkpoint.fileId;
            }
            if (!locatorReported)
            {
                std::cout << "ASSOCIATION_LEDGER root_utf16le_hex="; wideHex(ledgerRoot->physicalPath.native());
                std::cout << " root_id="; idHex(ledgerRoot->identity);
                std::cout << " store_utf16le_hex="; wideHex(ledgerLocator.directoryName);
                std::cout << " store_id="; idHex(ledgerLocator.directoryId);
                std::cout << " head_utf16le_hex="; wideHex(ledgerHeadName);
                std::cout << " head_id="; idHex(ledgerHeadId);
                std::cout << " binding=" << ledgerBinding << " authentication=unconfirmed exact_rerun_requires_live_root=yes\n";
                std::cout.flush();
                if (!std::cout) { std::cerr << "UNKNOWN owned_locator_output\n"; return 2; }
                locatorReported = true;
            }
            const bool confirmed = parent->ConfirmOwnedCase(*ledgerStore, association->committed, ledgerHeadName, ledgerHeadId,
                ledgerBinding, parentCheckpointBudget, ledgerRoot->handle, ledgerRoot->identity);
            if (!capacityReported && parent->OwnedTerminalCapacity())
            {
                const auto& capacity = *parent->OwnedTerminalCapacity();
                std::cout << "PARENT_TERMINAL_CAPACITY sufficient=" << capacity.sufficient
                    << " resource_unavailable=" << capacity.resourceUnavailable
                    << " maximum_encoded_bytes=" << capacity.maximumEncodedBytes
                    << " reason=" << capacity.reason << " child_evidence_bound=unverified\n";
                capacityReported = true;
            }
            if (confirmed)
                std::cout << "ASSOCIATION_CONFIRMED selection_only=yes\n";
        }
        if (ownedFile && parent->ConsumeOwnedScanProgress()) progressDeadline = GetTickCount64() + 45000;
        (void)parent->AcknowledgeAttempt(coordinateBudget.arithmeticSteps);
        (void)parent->AcknowledgeTerminal(coordinateBudget.arithmeticSteps);
        if (ownedFile && parent->OwnedTerminalPrepared() && !parent->Poll().checkpointTransitionVerified)
        {
            const auto proof = parent->OwnedTerminalEvidence();
            const auto* committedAssociation = parent->OwnedCommittedAssociation();
            if (!proof || !committedAssociation) { std::cerr << "UNKNOWN owned_terminal_proof\n"; return 2; }
            if (!terminal)
            {
                terminal.emplace(ledgerStore->PublishOwnedTerminal(ledgerHeadName, ledgerHeadId, ledgerBinding,
                    *committedAssociation, proof, coordinateBudget, axisBudget, 65536, virtual_child_protocol::kLimit,
                    virtual_child_protocol::kLimit, coordinateBudget.arithmeticSteps, parentCheckpointBudget));
                if (!terminal->committed || !terminal->resultingState || !terminal->checkpoint.fileIdentityCaptured)
                { publicationClues(terminal->checkpoint); std::cerr << "UNKNOWN owned_terminal_publication\n"; return 2; }
                ledgerHeadName = terminal->checkpoint.fileName; ledgerHeadId = terminal->checkpoint.fileId;
            }
            if (!terminalLocatorReported)
            {
                std::cout << "TERMINAL_LEDGER root_utf16le_hex="; wideHex(ledgerRoot->physicalPath.native());
                std::cout << " root_id="; idHex(ledgerRoot->identity);
                std::cout << " store_utf16le_hex="; wideHex(ledgerLocator.directoryName);
                std::cout << " store_id="; idHex(ledgerLocator.directoryId);
                std::cout << " head_utf16le_hex="; wideHex(ledgerHeadName);
                std::cout << " head_id="; idHex(ledgerHeadId);
                std::cout << " binding=" << ledgerBinding << " authentication=unconfirmed\n";
                std::cout.flush();
                if (!std::cout) { std::cerr << "UNKNOWN owned_terminal_locator_output\n"; return 2; }
                terminalLocatorReported = true;
            }
            if (!terminalShard)
            {
                terminalShard.emplace(ledgerStore->PublishOwnedTerminalShard(*terminal->committed, proof,
                    ledgerHeadName, ledgerHeadId, ledgerBinding, parentCheckpointBudget, virtual_child_protocol::kLimit));
                if (!terminalShard->published || !terminalShard->identityReplayed ||
                    !terminalShard->publication.fileIdentityCaptured)
                { publicationClues(terminalShard->publication); std::cerr << "UNKNOWN owned_terminal_shard\n"; return 2; }
            }
            if (!terminalShardLocatorReported)
            {
                std::cout << "TERMINAL_SHARD name_utf16le_hex="; wideHex(terminalShard->publication.fileName);
                std::cout << " id="; idHex(terminalShard->publication.fileId); std::cout << " authentication=unconfirmed\n";
                std::cout.flush();
                if (!std::cout) { std::cerr << "UNKNOWN owned_terminal_shard_locator_output\n"; return 2; }
                terminalShardLocatorReported = true;
            }
            if (!shardIndex)
            {
                const v3_scheduler::V4ShardLocator shard{terminalShard->publication.fileName, terminalShard->publication.fileId};
                shardIndex.emplace(v3_scheduler::AppendV4ShardIndex(*ledgerStore, std::nullopt, shard, "terminal", ledgerBinding,
                    coordinateBudget, axisBudget, 65536, virtual_child_protocol::kLimit, virtual_child_protocol::kLimit,
                    coordinateBudget.arithmeticSteps, parentCheckpointBudget, parentCheckpointBudget));
                if (!shardIndex->published || !shardIndex->head)
                { publicationClues(shardIndex->publication); std::cerr << "UNKNOWN owned_terminal_shard_index\n"; return 2; }
            }
            if (!shardIndexLocatorReported)
            {
                std::cout << "TERMINAL_SHARD_INDEX name_utf16le_hex="; wideHex(shardIndex->head->name);
                std::cout << " id="; idHex(shardIndex->head->fileId); std::cout << " authentication=unconfirmed\n";
                std::cout.flush();
                if (!std::cout) { std::cerr << "UNKNOWN owned_shard_index_locator_output\n"; return 2; }
                shardIndexLocatorReported = true;
            }
            if (parent->ConfirmOwnedTerminal(*ledgerStore, terminal->committed, ledgerHeadName, ledgerHeadId,
                ledgerBinding, parentCheckpointBudget, virtual_child_protocol::kLimit,
                coordinateBudget.arithmeticSteps))
                std::cout << "TERMINAL_CONFIRMED checkpoint_consistency_only=yes\n";
        }
        const auto result = parent->Poll();
        if (!result.started)
        { std::cerr << "UNAVAILABLE child_start error=" << result.error << '\n'; return 2; }
        if (result.error)
        { std::cerr << "UNKNOWN child_wait_error=" << result.error << " pid=" << result.childPid << '\n'; return 2; }
        if (result.exited)
        {
            if (result.ownedDirectoryPinsHeld)
                std::cout << "OWNED_DIRECTORY_PINS held_during_child_exit=yes cleanup_may_be_blocked=yes\n";
            if (ownedFile && parent->OwnedCaseVerified())
            {
                const auto selected = parent->SelectedCaseId();
                std::cout << "OWNED_CASE bytes=" << selected.size();
                if (selected.size() <= 4096)
                { std::cout << " id_hex="; for (const unsigned char byte : selected) std::cout << digits[byte >> 4] << digits[byte & 15]; }
                else std::cout << " id_hex=omitted_report_budget";
                std::cout << " authentication=selection_only\n";
            }
            if (result.reportedPending)
            {
                std::cout << "CHILD_REPORTED_PENDING authenticated=no reason=" << result.pendingReason << '\n';
                if (result.pendingDiagnostics)
                {
                    const auto& diagnostic = *result.pendingDiagnostics; const auto& budget = diagnostic.budget;
                    std::cout << "CHILD_REPORTED_DIAGNOSTICS authenticated=no execution_authority=no"
                        << " configuration_recorded=" << diagnostic.configurationRecorded
                        << " frame_before=" << diagnostic.frameBoundBefore << " frame_after=" << diagnostic.frameBoundAfter
                        << " configured=" << diagnostic.configured << " resource_unavailable=" << diagnostic.resourceUnavailable
                        << " other_recorder_budgets_unresolved=" << diagnostic.otherRecorderBudgetsUnresolved
                        << " event_capacity=" << diagnostic.eventCapacity << " blob_capacity=" << diagnostic.blobCapacity
                        << " staging_handle_capacity=" << diagnostic.stagingHandleCapacity
                        << " committed_bytes=" << diagnostic.committedBytes << " configuration_reason=" << diagnostic.configurationReason
                        << " observed_event_capacity=" << budget.eventCapacity << " observed_blob_capacity=" << budget.blobCapacity
                        << " observed_staging_handle_capacity=" << budget.stagingHandleCapacity
                        << " observed_committed_bytes=" << budget.committedBytes
                        << " recorded_events=" << budget.recordedEvents << " recorded_blob_bytes=" << budget.recordedBlobBytes
                        << " recorded_staging_handles=" << budget.recordedStagingHandles
                        << " capture_claimed=" << budget.captureClaimed << " writers_drained=" << budget.writersDrained
                        << " records_lost=" << budget.recordsLost << " capacity_exceeded=" << budget.capacityExceeded << '\n';
                    std::cout << "CHILD_REPORTED_BACKGROUND_DIAGNOSTICS authenticated=no execution_authority=no"
                        << " attempted=" << diagnostic.backgroundAttempted << " configured=" << diagnostic.backgroundConfigured
                        << " resource_unavailable=" << diagnostic.backgroundResourceUnavailable
                        << " shared_bound_after=" << diagnostic.sharedBoundAfter << " shared_vm_budget=" << diagnostic.sharedVmBudget
                        << " event_capacity=" << diagnostic.backgroundCapacity << " committed_bytes=" << diagnostic.backgroundCommittedBytes
                        << " reason=" << diagnostic.backgroundReason << " observed=" << diagnostic.backgroundBudget.has_value();
                    if (diagnostic.backgroundBudget)
                    {
                        const auto& background = *diagnostic.backgroundBudget;
                        std::cout << " observed_capacity=" << background.eventCapacity << " observed_committed_bytes=" << background.committedBytes
                            << " recorded_events=" << background.recordedEvents << " capture_claimed=" << background.captureClaimed
                            << " writers_drained=" << background.writersDrained << " records_lost=" << background.recordsLost
                            << " capacity_exceeded=" << background.capacityExceeded;
                    }
                    std::cout << '\n';
                }
            }
            else
                std::cout << "UNKNOWN child_outcome report=" << result.reportReceived << " exit=" << result.exitCode << '\n';
            return 2;
        }
        Sleep(10);
    }
    const auto last = parent->Poll();
    std::cout << "UNKNOWN child_timeout child_may_still_be_running=yes pid=" << last.childPid << '\n';
    return 2;
}

EventProgram ProgramVersion(const std::vector<Axis>& axes, const Candidate& candidate,
    SelectionSchemaVersion version)
{
    if (version != SelectionSchemaVersion::legacy_v1 && version != SelectionSchemaVersion::source_members_v2)
        throw std::runtime_error("global_program_version_invalid");
    auto program = version == SelectionSchemaVersion::source_members_v2
        ? BuildEventProgramV2(axes, candidate) : BuildEventProgram(axes, candidate);
    for (std::size_t i = 0; i < axes.size(); ++i)
    {
        if (axes[i].role == Role::Derived) continue;
        const auto& value = candidate.values[i];
        const std::string_view axis = axes[i].id;
        const bool second = axis.substr(0, 7) == "task_b.";
        if (axes[i].role == Role::Hypothesis)
        {
            if (!(second && Value(axes, candidate, "X03.task_count") == "one" && value == "na"))
                program.unresolved.emplace_back("outcome_hypothesis_requires_production_observation:" + axes[i].id + "=" + value);
            continue;
        }
        const auto localAxis = second ? axis.substr(7) : axis;
        const auto transport = Value(axes, candidate, second ? "task_b.T01" : "T01");
        // These describe a fact outside the host's view, rather than a
        // missing value to synthesize. Other unknowns remain obligations.
        const bool unobservedExternalSource = transport == "external_ingress" &&
            ((localAxis == "G01.button" && value == "unknown") ||
             (localAxis == "G02.press" && value == "unknown") ||
             (localAxis == "source.E01" && value == "external_unknown"));
        if (!unobservedExternalSource &&
            (value.find("unknown") != std::string::npos || value.find("unresolved") != std::string::npos))
            program.unresolved.emplace_back("symbolic_input:" + axes[i].id + "=" + value);
    }
    if (version == SelectionSchemaVersion::legacy_v1)
    {
        const auto count = Value(axes, candidate, "P02.resolved_count");
        if (count == "na" || count == "unknown" || count == "still_unknown" ||
            (count == "many" && Value(axes, candidate, "P02.many_count").find("unresolved") != std::string_view::npos))
            program.unresolved.emplace_back("ordered_member_count_not_resolved");
        if (Value(axes, candidate, "X03.task_count") == "two")
        {
            const auto secondCount = Value(axes, candidate, "task_b.P02.resolved_count");
            if (secondCount == "na" || secondCount == "unknown" || secondCount == "still_unknown" ||
                (secondCount == "many" && Value(axes, candidate, "task_b.P02.many_count").find("unresolved") != std::string_view::npos))
                program.unresolved.emplace_back("second_task_ordered_member_count_not_resolved");
        }
    }
    else
    {
        if (SelectionSourceSize(axes, candidate) < 0)
            program.unresolved.emplace_back("ordered_source_member_count_unresolved");
        if (Value(axes, candidate, "X03.task_count") == "two" && SelectionSourceSize(axes, candidate, "task_b.") < 0)
            program.unresolved.emplace_back("second_task_source_member_count_unresolved");
    }
    program.unresolved.emplace_back("full_applicability_and_adapter_mapping_not_closed");
    program.unresolved.emplace_back("event_identity_partition_and_order_generation_not_implemented");
    program.unresolved.emplace_back("derived_results_and_independent_oracles_not_closed");
    return program;
}
EventProgram Program(const std::vector<Axis>& axes, const Candidate& candidate)
{ return ProgramVersion(axes, candidate, SelectionSchemaVersion::legacy_v1); }
void PrintProgram(const EventProgram& program)
{
    for (std::size_t i = 0; i < program.steps.size(); ++i)
        std::cout << "EVENT index=" << i << " kind=" << program.steps[i].kind
            << " parameter=" << program.steps[i].parameter << '\n';
    for (const auto& reason : program.unresolved) std::cout << "UNRESOLVED " << reason << '\n';
}
std::string HexBytes(std::string_view value)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const char character : value)
    {
        const auto byte = static_cast<unsigned char>(character);
        result.push_back(digits[byte >> 4]);
        result.push_back(digits[byte & 15]);
    }
    return result;
}
void PrintPreflight(const ingress_adapter::Preflight& audit, bool verbose)
{
    std::size_t planned = 0, missing = 0, derived = 0;
    for (const auto& axis : audit.axes)
    {
        switch (axis.evidence)
        {
        case ingress_adapter::Evidence::planned_input: ++planned; break;
        case ingress_adapter::Evidence::unimplemented: ++missing; break;
        case ingress_adapter::Evidence::derived_unimplemented: ++derived; break;
        }
        if (verbose)
            std::cout << "AXIS_AUDIT id=" << axis.id << " value=" << axis.value
                << " detail=" << axis.detail << '\n';
    }
    if (verbose)
        for (const auto& obligation : audit.obligations) std::cout << "OBLIGATION " << obligation << '\n';
    std::cout << "PREFLIGHT planned_unverified=" << planned << " input_unimplemented=" << missing
        << " derived_unimplemented=" << derived << " obligations=" << audit.obligations.size()
        << " executable=" << (audit.CanExecute() ? "yes" : "no") << '\n';
}
int Command(int argc, char** argv)
{
    const std::string_view mode = argv[1];
    const auto axes = Model();
    ConditionalSpace space(axes);
    // Replaying a full stable ID does not require computing the enormous
    // ordinal space. In particular this keeps file-based reproductions
    // independent of the cost of counting unrelated candidate branches.
    const bool singleCase = mode == "--global-case" || mode == "--global-case-file" || mode == "--global-case-at" ||
        mode == "--global-graph-replay" || mode == "--global-graph-page" || mode == "--global-graph-joined-page" ||
        mode == "--global-graph-joined-replay" || mode == "--global-unified-page";
    const auto rawCount = singleCase ? std::string{} : RawCount(axes);
    const auto conditionalCount = singleCase ? std::string{} : space.Count();
    const auto prefixExcluded = singleCase ? std::string{} : Subtract(rawCount, conditionalCount);
    std::cout << "CTEST_FULL_OUTPUT\n";
    if (mode == "--global-model")
    {
        behavior::Require(argc == 2, "--global-model takes no arguments");
        for (const auto& axis : axes)
        {
            std::cout << "AXIS " << axis.id << " role=" << RoleName(axis.role)
                << " values=" << axis.values.size() << " label=" << axis.label << " domain=";
            for (std::size_t i = 0; i < axis.values.size(); ++i)
                std::cout << (i ? "," : "") << axis.values[i];
            std::cout << '\n';
        }
        std::size_t missing = 0;
        for (const auto& field : drag_field_inventory::fields)
        {
            const std::string prefix = std::string(field.id) + '.';
            const bool mapped = std::any_of(axes.begin(), axes.end(), [&](const auto& axis) {
                return axis.id == field.id || axis.id.starts_with(prefix);
            });
            if (!mapped) { ++missing; std::cout << "UNMAPPED_FIELD " << field.id << '\n'; }
        }
        std::cout << "GLOBAL axes=" << axes.size() << " historical_fields=" << std::size(drag_field_inventory::fields)
            << " unmapped_fields=" << missing << " raw_domain_product=" << rawCount << " prefix_excluded=" << prefixExcluded
            << " prefix_reason=selection_and_task_count_conditional_domains prefix_candidates=" << conditionalCount
            << " symbolic_unresolved=included event_graph_variants=unimplemented excluded=unknown applicable=unknown status=constraints_and_adapters_incomplete" << std::endl;
        return 2; // Field presence alone does not close applicability or adapters.
    }
    if (mode == "--global-unified-page")
    {
        behavior::Require(argc == 6, "unified page requires legacy or v2, cursor hex file or -, work budget and output limit");
        const std::string_view schemaName = argv[2];
        behavior::Require(schemaName == "legacy" || schemaName == "v2", "unknown unified schema version");
        const auto version = schemaName == "legacy" ? SelectionSchemaVersion::legacy_v1 : SelectionSchemaVersion::source_members_v2;
        const auto model = Model(version);
        const event_graph::Context context(model, version);
        for (const auto& error : context.errors) std::cout << "UNIFIED_SCHEMA_ERROR " << error << '\n';
        behavior::Require(context.errors.empty(), "unified schema unavailable");
        event_graph::UnifiedGraphCursor cursor;
        if (std::string_view(argv[3]) != "-")
        {
            std::ifstream stream(argv[3], std::ios::binary);
            behavior::Require(static_cast<bool>(stream), "open unified cursor input");
            std::string hex;
            behavior::Require(static_cast<bool>(std::getline(stream, hex)), "read unified cursor input");
            if (!hex.empty() && hex.back() == '\r') hex.pop_back();
            behavior::Require(stream.peek() == std::char_traits<char>::eof() && hex.size() % 2 == 0,
                "unified cursor must be one line of even-length hex");
            const auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            std::string bytes; bytes.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size(); i += 2)
            {
                const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
                behavior::Require(high >= 0 && low >= 0, "unified cursor hex must use lowercase hexadecimal");
                bytes.push_back(static_cast<char>((high << 4) | low));
            }
            const auto decoded = event_graph::UnifiedCandidateGraph::ParseCursor(context, bytes);
            for (const auto& error : decoded.errors) std::cout << "UNIFIED_CURSOR_ERROR " << error << '\n';
            behavior::Require(decoded.errors.empty(), "unified cursor invalid or mismatched");
            cursor = decoded.cursor;
        }
        const auto work = std::stoull(Normalize(argv[4])), limit = std::stoull(Normalize(argv[5]));
        behavior::Require(work <= 1000000 && limit > 0 && limit <= 10000, "unified page budget or output limit outside supported bounds");
        const auto page = event_graph::UnifiedCandidateGraph::Enumerate(context, std::move(cursor),
            static_cast<std::size_t>(work), static_cast<std::size_t>(limit));
        for (const auto& error : page.errors) std::cout << "UNIFIED_ERROR " << error << '\n';
        behavior::Require(page.errors.empty(), "unified page unavailable");
        const auto saved = event_graph::UnifiedCandidateGraph::SerializeCursor(context, page.cursor);
        for (const auto& error : saved.errors) std::cout << "UNIFIED_CURSOR_ERROR " << error << '\n';
        behavior::Require(saved.errors.empty(), "unified cursor could not be encoded");
        const auto scan = event_graph::ParseCandidateScanCursor(context, page.cursor.nextCandidate);
        behavior::Require(scan.errors.empty(), "unified candidate scan counters unavailable");
        std::cout << "UNIFIED_BEGIN preview_only=yes schema=" << schemaName << " durable_atomic=no\n";
        for (const auto& item : page.records)
        {
            std::cout << "UNIFIED_PREVIEW unexecuted=yes kind=" << item.kind << " candidate_ordinal=" << item.candidateOrdinal
                << " candidate_id_hex=" << HexBytes(item.candidateVersionedId)
                << " candidate_raw_id_hex=" << HexBytes(item.candidateId) << " graph_ordinal=" << item.graphOrdinal
                << " reason_hex=" << HexBytes(item.reason);
            if (item.graph)
            {
                std::cout << " presence_ordinal=" << item.graph->presenceOrdinal
                    << " active_ordinal=" << item.graph->graph.ordinal << " presence=";
                for (const bool bit : item.graph->present) std::cout << (bit ? '1' : '0');
                std::cout << " graph_id_hex=" << HexBytes(item.graph->graph.id);
            }
            std::cout << '\n';
            for (const auto& reason : item.unresolved)
                std::cout << "UNIFIED_UNRESOLVED candidate_id_hex=" << HexBytes(item.candidateVersionedId)
                    << " graph_ordinal=" << item.graphOrdinal << " reason_hex=" << HexBytes(reason) << '\n';
        }
        for (const auto& prune : page.candidatePruned)
        {
            std::cout << "UNIFIED_CANDIDATE_PREFIX_PRUNE axis_hex=" << HexBytes(prune.axisId)
                << " value_hex=" << HexBytes(prune.value) << " prefix=";
            for (std::size_t i = 0; i < prune.prefix.size(); ++i) std::cout << (i ? "," : "") << prune.prefix[i];
            std::cout << '\n';
        }
        for (const auto& item : page.presencePruned)
        {
            std::cout << "UNIFIED_PRESENCE_PRUNE candidate_ordinal=" << item.candidateOrdinal
                << " candidate_raw_id_hex=" << HexBytes(item.candidateId) << " reason_hex=" << HexBytes(item.prune.reason)
                << " premise_hex=" << HexBytes(item.prune.premise) << " consequence_hex=" << HexBytes(item.prune.consequence)
                << " prefix=";
            for (const auto bit : item.prune.prefix) std::cout << bit;
            std::cout << '\n';
        }
        for (const auto& item : page.activePruned)
        {
            std::cout << "UNIFIED_ACTIVE_PRUNE candidate_ordinal=" << item.candidateOrdinal
                << " candidate_raw_id_hex=" << HexBytes(item.candidateId) << " presence_ordinal=" << item.prune.presenceOrdinal
                << " presence=";
            for (const bool bit : item.prune.present) std::cout << (bit ? '1' : '0');
            std::cout << " scope=" << item.prune.prune.scope << " reason=" << item.prune.prune.reason << " partition=";
            for (std::size_t i = 0; i < item.prune.prune.partition.size(); ++i)
                std::cout << (i ? "," : "") << item.prune.prune.partition[i];
            std::cout << " earlier_hex=" << HexBytes(item.prune.prune.earlierRequirement)
                << " current_hex=" << HexBytes(item.prune.prune.currentRequirement) << '\n';
        }
        for (const auto& [reason, count] : page.cursor.completedActivePrunes)
            std::cout << "UNIFIED_ACTIVE_PRUNE completed_candidates=yes reason=" << reason << " count=" << count << '\n';
        std::cout << "UNIFIED_PAGE preview_only=yes durable_atomic=no schema=" << schemaName
            << " exhausted=" << (page.exhausted ? "yes" : "no") << " paused=" << (page.paused ? "yes" : "no")
            << " work=" << page.workUsed << " next_graph=" << page.cursor.nextGraphOrdinal
            << " structural_excluded=" << page.cursor.structuralExcluded << " mapping_unimplemented=" << page.cursor.mappingUnimplemented
            << " no_graph_hypotheses=" << page.cursor.noGraphHypotheses
            << " completed_presence_prefix_prunes=" << page.cursor.completedPresencePrefixPrunes
            << " candidate_prefix_prunes=" << scan.cursor.rejectedPrefixes
            << " current_candidate_graph_count=" << page.cursor.currentGraphCount
            << " current_candidate_prune_totals_in_cursor=" << (page.cursor.hasCandidate ? "yes" : "no")
            << " cursor_hex=" << HexBytes(saved.encoded) << '\n';
        std::cout << "FAIL UNIMPLEMENTED unified_production_execution" << std::endl;
        return 2;
    }
    if (mode == "--global-graph-joined-replay")
    {
        behavior::Require(argc == 5 || argc == 6,
            "joined graph replay requires candidate ID file, raw graph ID file, exact presence bits and optional legacy or v2");
        const std::string_view schemaName = argc == 6 ? argv[5] : "legacy";
        behavior::Require(schemaName == "legacy" || schemaName == "v2", "unknown joined graph schema version");
        const auto version = schemaName == "legacy" ? SelectionSchemaVersion::legacy_v1 : SelectionSchemaVersion::source_members_v2;
        const auto readFile = [](const char* path) {
            std::ifstream stream(path, std::ios::binary);
            behavior::Require(static_cast<bool>(stream), "open joined graph replay input");
            std::string bytes{std::istreambuf_iterator<char>(stream), {}};
            behavior::Require(!stream.bad(), "read complete joined graph replay input");
            return bytes;
        };
        auto candidateId = readFile(argv[2]);
        if (!candidateId.empty() && candidateId.back() == '\n') candidateId.pop_back();
        if (!candidateId.empty() && candidateId.back() == '\r') candidateId.pop_back();
        const auto replayAxes = Model(version);
        const auto candidate = ParseIdVersion(replayAxes, candidateId, version);
        const auto program = ProgramVersion(replayAxes, candidate, version);
        const event_graph::Context graphContext(replayAxes, version);
        const auto mapping = Value(replayAxes, candidate, "T01") == "internal_native"
            ? event_graph::MapSimpleNative(graphContext, candidate, program)
            : event_graph::MapSimpleExternal(graphContext, candidate, program);
        if (!mapping.structuralExclusion.empty())
        { std::cout << "EXCLUDED reason=" << mapping.structuralExclusion << std::endl; return 2; }
        if (!mapping.graphAvailable)
        {
            for (const auto& reason : mapping.reasons) std::cout << "GRAPH_MAPPING_UNRESOLVED " << reason << '\n';
            std::cout << "FAIL UNIMPLEMENTED graph_mapping" << std::endl; return 2;
        }
        std::vector<bool> present;
        for (const char bit : std::string_view(argv[4]))
        {
            behavior::Require(bit == '0' || bit == '1', "presence bits must contain only 0 and 1");
            present.push_back(bit == '1');
        }
        const auto replay = event_graph::PresenceActiveGraph::Replay(graphContext, candidate, mapping.spec, present, readFile(argv[3]));
        if (!replay.structuralExclusion.empty()) std::cout << "EXCLUDED reason=" << replay.structuralExclusion << '\n';
        for (const auto& error : replay.errors) std::cout << "GRAPH_ERROR " << error << '\n';
        if (!replay.identityValid) { std::cout << "FAIL joined_graph_identity_invalid" << std::endl; return 2; }
        std::cout << "JOINED_GRAPH_IDENTITY_VALID unexecuted=yes schema=" << schemaName << " unresolved=" << replay.variant.unresolved.size() << '\n';
        for (const auto& reason : replay.variant.unresolved) std::cout << "GRAPH_UNRESOLVED " << reason << '\n';
        std::cout << "FAIL UNIMPLEMENTED joined_graph_production_execution" << std::endl;
        return 2;
    }
    if (mode == "--global-graph-joined-page")
    {
        behavior::Require(argc == 6, "joined graph page requires candidate ID file, cursor hex file or -, work budget and output limit");
        const auto line = [](const char* path) {
            std::ifstream stream(path, std::ios::binary);
            behavior::Require(static_cast<bool>(stream), "open joined graph page input");
            std::string value;
            behavior::Require(static_cast<bool>(std::getline(stream, value)), "read joined graph page input");
            if (!value.empty() && value.back() == '\r') value.pop_back();
            behavior::Require(stream.peek() == std::char_traits<char>::eof(), "joined graph input must contain one line");
            return value;
        };
        const auto candidate = ParseId(axes, line(argv[2]));
        const auto program = Program(axes, candidate);
        const event_graph::Context graphContext(axes);
        const auto mapping = Value(axes, candidate, "T01") == "internal_native"
            ? event_graph::MapSimpleNative(graphContext, candidate, program)
            : event_graph::MapSimpleExternal(graphContext, candidate, program);
        if (!mapping.structuralExclusion.empty())
        { std::cout << "EXCLUDED reason=" << mapping.structuralExclusion << std::endl; return 2; }
        if (!mapping.graphAvailable)
        {
            for (const auto& reason : mapping.reasons) std::cout << "GRAPH_MAPPING_UNRESOLVED " << reason << '\n';
            std::cout << "FAIL UNIMPLEMENTED graph_mapping" << std::endl; return 2;
        }
        event_graph::JoinedGraphCursor cursor;
        if (std::string_view(argv[3]) != "-")
        {
            const auto hex = line(argv[3]);
            behavior::Require(hex.size() % 2 == 0, "joined cursor hex length must be even");
            const auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            std::string bytes; bytes.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size(); i += 2)
            {
                const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
                behavior::Require(high >= 0 && low >= 0, "joined cursor hex must use lowercase hexadecimal");
                bytes.push_back(static_cast<char>((high << 4) | low));
            }
            const auto decoded = event_graph::JoinedPresenceGraph::ParseCursor(graphContext, candidate, mapping.spec, bytes);
            for (const auto& error : decoded.errors) std::cout << "GRAPH_CURSOR_ERROR " << error << '\n';
            behavior::Require(decoded.errors.empty() && decoded.structuralExclusion.empty(), "joined graph cursor invalid or mismatched");
            cursor = decoded.cursor;
        }
        const auto work = std::stoull(Normalize(argv[4]));
        const auto limit = std::stoull(Normalize(argv[5]));
        behavior::Require(work <= 1000000 && limit > 0 && limit <= 10000,
            "joined graph page budget or output limit outside supported bounds");
        const auto page = event_graph::JoinedPresenceGraph::Enumerate(graphContext, candidate, mapping.spec,
            std::move(cursor), static_cast<std::size_t>(work), static_cast<std::size_t>(limit));
        for (const auto& error : page.errors) std::cout << "GRAPH_ERROR " << error << '\n';
        if (!page.errors.empty() || !page.structuralExclusion.empty())
        { std::cout << "FAIL joined_graph_page_invalid" << std::endl; return 2; }
        // Validate and encode the complete continuation before publishing any
        // page items. Stdout itself is not an atomic durable transaction.
        const auto saved = event_graph::JoinedPresenceGraph::SerializeCursor(graphContext, candidate, mapping.spec, page.cursor);
        for (const auto& error : saved.errors) std::cout << "GRAPH_CURSOR_ERROR " << error << '\n';
        behavior::Require(saved.errors.empty() && saved.structuralExclusion.empty(), "joined graph cursor could not be encoded");
        const auto outer = event_graph::ParsePresenceCursor(graphContext, candidate, mapping.spec, page.cursor.nextPresence);
        behavior::Require(outer.errors.empty() && outer.structuralExclusion.empty(), "joined graph presence counters unavailable");
        std::map<std::string, std::string> activeCounts;
        if (page.cursor.hasActive)
        {
            const auto active = event_graph::PresenceActiveGraph::ParseCursor(graphContext, candidate, mapping.spec,
                page.cursor.present, page.cursor.activeCursor);
            behavior::Require(active.errors.empty() && active.structuralExclusion.empty(), "joined graph active counters unavailable");
            activeCounts = active.cursor.inner.pruneCounts;
        }
        for (const auto& [reason, count] : outer.cursor.pruneCounts)
            std::cout << "PRESENCE_PRUNE cumulative_prefixes=yes reason=" << reason << " count=" << count << '\n';
        for (const auto& [reason, count] : activeCounts)
            std::cout << "ACTIVE_GRAPH_PRUNE current_assumption=yes reason=" << reason << " count=" << count << '\n';
        for (const auto& item : page.graphs)
        {
            const auto graphHex = HexBytes(item.graph.id);
            std::cout << "JOINED_GRAPH_PREVIEW unexecuted=yes ordinal=" << item.ordinal
                << " presence_ordinal=" << item.presenceOrdinal << " active_ordinal=" << item.graph.ordinal << " presence=";
            for (const bool bit : item.present) std::cout << (bit ? '1' : '0');
            std::cout << " unresolved=" << item.graph.unresolved.size() << " id_hex=" << graphHex << '\n';
            for (const auto& reason : item.graph.unresolved)
                std::cout << "GRAPH_UNRESOLVED id_hex=" << graphHex << " reason_hex=" << HexBytes(reason) << '\n';
        }
        for (const auto& prune : page.presencePruned)
        {
            std::cout << "PRESENCE_PRUNE reason_hex=" << HexBytes(prune.reason) << " premise_hex=" << HexBytes(prune.premise)
                << " consequence_hex=" << HexBytes(prune.consequence) << " prefix=";
            for (const auto bit : prune.prefix) std::cout << bit;
            std::cout << '\n';
        }
        for (const auto& item : page.activePruned)
        {
            std::cout << "ACTIVE_GRAPH_PRUNE presence_ordinal=" << item.presenceOrdinal << " presence=";
            for (const bool bit : item.present) std::cout << (bit ? '1' : '0');
            std::cout << " scope=" << item.prune.scope << " reason=" << item.prune.reason << " partition=";
            for (std::size_t i = 0; i < item.prune.partition.size(); ++i)
                std::cout << (i ? "," : "") << item.prune.partition[i];
            std::cout << " earlier_hex=" << HexBytes(item.prune.earlierRequirement)
                << " current_hex=" << HexBytes(item.prune.currentRequirement) << '\n';
        }
        for (const auto& [reason, count] : page.cursor.completedActivePrunes)
            std::cout << "ACTIVE_GRAPH_PRUNE completed_assumptions=yes reason=" << reason << " count=" << count << '\n';
        std::cout << "JOINED_GRAPH_PAGE preview_only=yes durable_atomic=no exhausted=" << (page.exhausted ? "yes" : "no")
            << " paused=" << (page.paused ? "yes" : "no") << " work=" << page.workUsed
            << " start=" << page.startOrdinal << " next=" << page.cursor.nextOrdinal
            << " cursor_hex=" << HexBytes(saved.encoded) << '\n';
        std::cout << "FAIL UNIMPLEMENTED global_graph_product_and_execution" << std::endl;
        return 2;
    }
    if (mode == "--global-graph-page")
    {
        behavior::Require(argc == 6, "graph page requires candidate ID file, cursor hex file or -, work budget and output limit");
        const auto readLine = [](const char* path) {
            std::ifstream stream(path, std::ios::binary);
            behavior::Require(static_cast<bool>(stream), "open graph page input file");
            std::string value;
            behavior::Require(static_cast<bool>(std::getline(stream, value)), "read graph page input line");
            if (!value.empty() && value.back() == '\r') value.pop_back();
            behavior::Require(stream.peek() == std::char_traits<char>::eof(), "graph page input must contain exactly one line");
            return value;
        };
        const auto candidate = ParseId(axes, readLine(argv[2]));
        const auto program = Program(axes, candidate);
        const event_graph::Context graphContext(axes);
        const auto mapping = Value(axes, candidate, "T01") == "internal_native"
            ? event_graph::MapSimpleNative(graphContext, candidate, program)
            : event_graph::MapSimpleExternal(graphContext, candidate, program);
        if (!mapping.structuralExclusion.empty())
        { std::cout << "EXCLUDED reason=" << mapping.structuralExclusion << std::endl; return 2; }
        if (!mapping.graphAvailable)
        {
            for (const auto& reason : mapping.reasons) std::cout << "GRAPH_MAPPING_UNRESOLVED " << reason << '\n';
            std::cout << "FAIL UNIMPLEMENTED graph_mapping" << std::endl; return 2;
        }
        event_graph::DfsCursor cursor;
        if (std::string_view(argv[3]) != "-")
        {
            const auto hex = readLine(argv[3]);
            behavior::Require(hex.size() % 2 == 0, "cursor hex length must be even");
            const auto nibble = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return -1;
            };
            std::string bytes; bytes.reserve(hex.size() / 2);
            for (std::size_t i = 0; i < hex.size(); i += 2)
            {
                const int high = nibble(hex[i]), low = nibble(hex[i + 1]);
                behavior::Require(high >= 0 && low >= 0, "cursor hex must use lowercase hexadecimal");
                bytes.push_back(static_cast<char>((high << 4) | low));
            }
            const auto decoded = event_graph::ParseDfsCursor(graphContext, candidate, mapping.spec, bytes);
            for (const auto& error : decoded.errors) std::cout << "GRAPH_CURSOR_ERROR " << error << '\n';
            behavior::Require(decoded.errors.empty() && decoded.structuralExclusion.empty(), "graph cursor is invalid or mismatched");
            cursor = decoded.cursor;
        }
        const auto work = std::stoull(Normalize(argv[4]));
        const auto limit = std::stoull(Normalize(argv[5]));
        behavior::Require(work > 0 && work <= 1000000 && limit > 0 && limit <= 10000,
            "graph page budget or output limit outside supported bounds");
        const auto page = event_graph::EnumerateDfs(graphContext, candidate, mapping.spec, std::move(cursor),
            static_cast<std::size_t>(work), static_cast<std::size_t>(limit));
        for (const auto& error : page.errors) std::cout << "GRAPH_ERROR " << error << '\n';
        if (!page.errors.empty() || !page.structuralExclusion.empty())
        { std::cout << "FAIL graph_page_invalid" << std::endl; return 2; }
        for (const auto& graph : page.variants)
        {
            const auto graphHex = HexBytes(graph.id);
            std::cout << "GRAPH_PREVIEW unexecuted=yes ordinal=" << graph.ordinal
                << " unresolved=" << graph.unresolved.size() << " id_hex=" << graphHex << '\n';
            for (const auto& reason : graph.unresolved)
                std::cout << "GRAPH_UNRESOLVED id_hex=" << graphHex << " reason_hex=" << HexBytes(reason) << '\n';
        }
        for (const auto& prune : page.pruned)
        {
            std::cout << "GRAPH_PRUNE_EVENT scope=" << prune.scope << " reason=" << prune.reason
                << " earlier_bytes_hex=" << HexBytes(prune.earlierRequirement)
                << " current_bytes_hex=" << HexBytes(prune.currentRequirement) << " partition=";
            for (std::size_t i = 0; i < prune.partition.size(); ++i)
                std::cout << (i ? "," : "") << prune.partition[i];
            std::cout << '\n';
        }
        for (const auto& [reason, count] : page.cursor.pruneCounts)
            std::cout << "GRAPH_PRUNE cumulative=yes unit_reason=" << reason << " count=" << count << '\n';
        const auto saved = event_graph::SerializeDfsCursor(graphContext, candidate, mapping.spec, page.cursor);
        for (const auto& error : saved.errors) std::cout << "GRAPH_CURSOR_ERROR " << error << '\n';
        behavior::Require(saved.errors.empty() && saved.structuralExclusion.empty(), "graph cursor could not be encoded");
        std::cout << "GRAPH_PAGE preview_only=yes exhausted=" << (page.exhausted ? "yes" : "no")
            << " paused=" << (page.paused ? "yes" : "no") << " work=" << page.workUsed
            << " next=" << page.cursor.nextOrdinal << " cursor_hex=" << HexBytes(saved.encoded) << '\n';
        std::cout << "FAIL UNIMPLEMENTED global_graph_product_and_execution" << std::endl;
        return 2;
    }
    if (mode == "--global-graph-replay")
    {
        behavior::Require(argc == 4, "graph replay requires a candidate ID file and an exact graph ID byte file");
        const auto readFile = [](const char* path) {
            std::ifstream stream(path, std::ios::binary);
            behavior::Require(static_cast<bool>(stream), "read graph replay input file");
            std::string bytes{std::istreambuf_iterator<char>(stream), {}};
            behavior::Require(!stream.bad(), "read complete graph replay input file");
            return bytes;
        };
        auto candidateId = readFile(argv[2]);
        if (!candidateId.empty() && candidateId.back() == '\n') candidateId.pop_back();
        if (!candidateId.empty() && candidateId.back() == '\r') candidateId.pop_back();
        const auto candidate = ParseId(axes, candidateId);
        const auto program = Program(axes, candidate);
        const event_graph::Context graphContext(axes);
        const auto mapping = Value(axes, candidate, "T01") == "internal_native"
            ? event_graph::MapSimpleNative(graphContext, candidate, program)
            : event_graph::MapSimpleExternal(graphContext, candidate, program);
        if (!mapping.structuralExclusion.empty())
        {
            std::cout << "EXCLUDED reason=" << mapping.structuralExclusion << std::endl;
            return 2;
        }
        if (!mapping.graphAvailable)
        {
            for (const auto& reason : mapping.reasons) std::cout << "GRAPH_MAPPING_UNRESOLVED " << reason << '\n';
            std::cout << "FAIL UNIMPLEMENTED graph_mapping" << std::endl;
            return 2;
        }
        const auto graphId = readFile(argv[3]);
        const auto replay = event_graph::ReplayVariant(graphContext, candidate, mapping.spec, graphId);
        if (!replay.structuralExclusion.empty())
            std::cout << "EXCLUDED reason=" << replay.structuralExclusion << '\n';
        for (const auto& error : replay.errors) std::cout << "GRAPH_ERROR " << error << '\n';
        if (!replay.identityValid)
        {
            std::cout << "FAIL graph_identity_invalid" << std::endl;
            return 2;
        }
        std::cout << "GRAPH_IDENTITY_VALID unexecuted=yes unresolved=" << replay.variant.unresolved.size() << '\n';
        const ingress_adapter::Context adapterContext(axes);
        const auto outcome = Value(axes, candidate, "T01") == "internal_native"
            ? ingress_adapter::ExecuteNativeDesktopSingleMove(adapterContext, candidate, program, replay.variant)
            : Value(axes, candidate, "P01.primary") == "text"
                ? ingress_adapter::ExecuteDesktopSingleText(adapterContext, candidate, program, replay.variant)
                : ingress_adapter::ExecuteDesktopSingleCopy(adapterContext, candidate, program, replay.variant);
        for (const auto& obligation : outcome.obligations) std::cout << "OBLIGATION " << obligation << '\n';
        std::cout << "BRIDGE_ENTRY production_started=" << (outcome.productionStarted ? "yes" : "no")
            << " production_entry_unknown=" << (outcome.productionEntryUnknown ? "yes" : "no") << '\n';
        if (outcome.status == ingress_adapter::BridgeStatus::production_failed)
        {
            std::cout << "FAIL PRODUCTION " << outcome.failure << std::endl;
            return 1;
        }
        if (outcome.status == ingress_adapter::BridgeStatus::production_observed)
            std::cout << "PRODUCTION_OBSERVED global_postflight=unimplemented\n";
        if (!outcome.failure.empty()) std::cout << "BRIDGE_DETAIL " << outcome.failure << '\n';
        std::cout << "FAIL UNIMPLEMENTED global_case_postflight" << std::endl;
        return 2;
    }
    if (mode == "--global-case" || mode == "--global-case-file" || mode == "--global-case-at")
    {
        behavior::Require(argc == 3, "global case requires a complete ID, ID file, or conditional ordinal");
        Candidate candidate;
        if (mode == "--global-case-at")
        {
            const auto ordinal = Normalize(argv[2]);
            candidate = ordinal == "0" ? space.First() : space.DecodeAt(ordinal);
        }
        else
        {
            std::string id;
            if (mode == "--global-case-file")
            {
                std::ifstream stream(argv[2], std::ios::binary);
                behavior::Require(static_cast<bool>(stream) && static_cast<bool>(std::getline(stream, id)), "read stable ID file");
                if (!id.empty() && id.back() == '\r') id.pop_back();
                behavior::Require(stream.peek() == std::char_traits<char>::eof(), "stable ID file must contain one line");
            }
            else id = argv[2];
            candidate = ParseId(axes, id);
        }
        const auto reason = StructuralExclusion(axes, candidate);
        if (!reason.empty())
        {
            std::cout << "EXCLUDED ordinal=" << (candidate.ordinal.empty() ? "unknown" : candidate.ordinal)
                << " reason=" << reason << " id=" << Id(axes, candidate) << std::endl;
            return 2;
        }
        std::cout << "MODEL_DERIVED capacity_state=" << CapacityInitialState(axes, candidate)
            << " selection_capacity=" << CapacityRelation(axes, candidate)
            << " task_b_capacity_state=" << SecondTaskCapacityInitialState(axes, candidate)
            << " task_b_selection_capacity=" << SecondTaskCapacityRelation(axes, candidate) << '\n';
        const auto program = Program(axes, candidate);
        PrintProgram(program);
        const event_graph::Context graphContext(axes);
        const auto mapping = Value(axes, candidate, "T01") == "internal_native"
            ? event_graph::MapSimpleNative(graphContext, candidate, program)
            : event_graph::MapSimpleExternal(graphContext, candidate, program);
        std::cout << "GRAPH_MAPPING status=" << mapping.status << " graph_available="
            << (mapping.graphAvailable ? "yes" : "no") << " obligations=" << mapping.reasons.size() << '\n';
        if (mapping.graphAvailable)
        {
            const auto page = event_graph::EnumerateDfs(graphContext, candidate, mapping.spec,
                event_graph::DfsCursor{}, 10000, 1);
            for (const auto& error : page.errors) std::cout << "GRAPH_ERROR " << error << '\n';
            for (const auto& graph : page.variants)
                std::cout << "GRAPH_PREVIEW unexecuted=yes unresolved=" << graph.unresolved.size()
                    << " id=" << graph.id << '\n';
            for (const auto& prune : page.pruned)
            {
                std::cout << "GRAPH_PRUNE_EVENT scope=" << prune.scope << " reason=" << prune.reason
                    << " earlier_bytes_hex=" << HexBytes(prune.earlierRequirement)
                    << " current_bytes_hex=" << HexBytes(prune.currentRequirement) << " partition=";
                for (std::size_t i = 0; i < prune.partition.size(); ++i)
                    std::cout << (i ? "," : "") << prune.partition[i];
                std::cout << '\n';
            }
            for (const auto& [reason, count] : page.cursor.pruneCounts)
                std::cout << "GRAPH_PRUNE partial=yes unit_reason=" << reason << " count=" << count << '\n';
            std::cout << "GRAPH_PAGE preview_only=yes exhausted=" << (page.exhausted ? "yes" : "no")
                << " paused=" << (page.paused ? "yes" : "no") << " work=" << page.workUsed
                << " next=" << page.cursor.nextOrdinal << '\n';
        }
        const ingress_adapter::Context context(axes);
        const auto audit = ingress_adapter::Inspect(context, candidate, program);
        PrintPreflight(audit, true);
        behavior::Require(!audit.CanExecute(), "global executor is not implemented");
        std::cout << "FAIL UNIMPLEMENTED " << Id(axes, candidate)
            << " applicability=unresolved adapter=unimplemented assertion=unimplemented expected=unresolved derived=uncomputed" << std::endl;
        return 2;
    }
    const bool countOnly = mode == "--global-count" || mode == "--global-count-all";
    const bool list = mode == "--global-list" || mode == "--global-list-all";
    const bool run = mode == "--global-run" || mode == "--global-run-all";
    const bool all = mode == "--global-list-all" || mode == "--global-run-all" || mode == "--global-count-all";
    behavior::Require(list || run || countOnly, "unknown global model command");
    const auto adapterContext = run ? std::make_unique<ingress_adapter::Context>(axes) : nullptr;
    behavior::Require(all ? argc == 2 : (argc == 4 || argc == 6 || argc == 7),
        "use START COUNT [SHARD SHARDS [FILTER]], or the explicit -all form");
    std::string ordinal = all ? "0" : Normalize(argv[2]);
    const auto limit = all ? 0ull : std::stoull(Normalize(argv[3]));
    behavior::Require(all || limit > 0, "global range must be nonempty");
    const std::size_t shard = argc >= 6 ? static_cast<std::size_t>(std::stoull(Normalize(argv[4]))) : 0;
    const std::size_t shards = argc >= 6 ? static_cast<std::size_t>(std::stoull(Normalize(argv[5]))) : 1;
    const std::string filter = argc == 7 ? argv[6] : "";
    behavior::Require(shards > 0 && shards <= 1000000000 && shard < shards, "invalid global shard");
    std::string visited = "0", selected = "0", excluded = "0", unimplemented = "0";
    std::map<std::string, std::string> exclusionCounts;
    unsigned long long bounded = 0;
    while (Less(ordinal, conditionalCount) && (all || bounded < limit))
    {
        const auto candidate = space.DecodeAt(ordinal);
        auto quotient = ordinal;
        const bool inShard = Divide(quotient, shards) == shard;
        const auto id = Id(axes, candidate);
        Increment(visited); ++bounded;
        if (inShard && id.find(filter) != std::string::npos)
        {
            Increment(selected);
            const auto reason = StructuralExclusion(axes, candidate);
            if (!reason.empty())
            {
                Increment(excluded);
                auto& reasonCount = exclusionCounts[std::string(reason)];
                if (reasonCount.empty()) reasonCount = "0";
                Increment(reasonCount);
                if (!countOnly) std::cout << "EXCLUDED ordinal=" << ordinal << " reason=" << reason << " id=" << id << '\n';
            }
            else
            {
                Increment(unimplemented);
                if (!countOnly)
                {
                    std::cout << "MODEL_DERIVED capacity_state=" << CapacityInitialState(axes, candidate)
                        << " selection_capacity=" << CapacityRelation(axes, candidate)
                        << " task_b_capacity_state=" << SecondTaskCapacityInitialState(axes, candidate)
                        << " task_b_selection_capacity=" << SecondTaskCapacityRelation(axes, candidate) << '\n';
                    const auto program = Program(axes, candidate);
                    PrintProgram(program);
                    if (run)
                    {
                        const auto audit = ingress_adapter::Inspect(*adapterContext, candidate, program);
                        PrintPreflight(audit, false);
                        behavior::Require(!audit.CanExecute(), "global executor is not implemented");
                    }
                }
                if (!countOnly) std::cout << (run ? "FAIL UNIMPLEMENTED" : "UNEXECUTED") << " ordinal=" << ordinal
                    << " applicability=unresolved adapter=unimplemented assertion=unimplemented expected=unresolved derived=uncomputed id=" << id << '\n';
            }
        }
        Increment(ordinal);
    }
    for (const auto& [reason, count] : exclusionCounts)
        std::cout << "EXCLUSION reason=" << reason << " count=" << count << '\n';
    std::cout << "GLOBAL_RESULT raw_domain_product=" << rawCount << " prefix_excluded=" << prefixExcluded
        << " prefix_reason=selection_and_task_count_conditional_domains prefix_candidates=" << conditionalCount
        << " visited=" << visited << " selected=" << selected
        << " excluded=" << excluded << " pass=0 fail=" << (run ? unimplemented : "0")
        << " unexecuted=" << unimplemented << " remaining_prefix_from=" << ordinal
        << " empty_selection=" << (selected == "0" ? "yes" : "no")
        << " symbolic_unresolved=included event_graph_variants=unimplemented global_excluded=unknown global_applicable=unknown" << std::endl;
    return run ? 2 : 0; // No all-global execution may pass while coverage is incomplete.
}
}

namespace drag_matrix
{
int RunChild(const std::string& id)
{
    std::array<wchar_t, 32768> executable{};
    const DWORD length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
    behavior::Require(length > 0 && length < executable.size(), "locate matrix executable");
    std::wstring command = L"\"" + std::wstring(executable.data(), length) + L"\" --matrix-case ";
    command.append(id.begin(), id.end());
    STARTUPINFOW startup{}; startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.data(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &startup, &process))
        throw std::runtime_error("cannot start isolated matrix case " + id);
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 60000);
    if (wait != WAIT_OBJECT_0)
    {
        TerminateProcess(process.hProcess, 124);
        WaitForSingleObject(process.hProcess, 5000);
    }
    DWORD code = 1;
    if (!GetExitCodeProcess(process.hProcess, &code)) code = 1;
    CloseHandle(process.hProcess);
    return static_cast<int>(code);
}
int Command(int argc, char** argv)
{
    const std::string_view mode = argv[1];
    if (mode == "--matrix-inventory")
    {
        behavior::Require(argc == 2, "--matrix-inventory takes no arguments");
        std::cout << "CTEST_FULL_OUTPUT\n";
        for (const auto& field : drag_field_inventory::fields)
        {
            constexpr std::string_view partial[]{"source.E01", "target.E01", "source.E02", "target.E02", "target.E03",
                "P01", "P02", "P03", "G01", "G02", "T01", "T02", "T03", "T04", "X01"};
            const bool mapped = std::find(std::begin(partial), std::end(partial), field.id) != std::end(partial);
            std::cout << field.id << '\t' << field.kind << '\t' << (mapped ? "partial" : "unmapped") << '\t'
                << field.label << '\t' << field.historicalValues << '\n';
        }
        std::cout << "INVENTORY fields=" << std::size(drag_field_inventory::fields)
            << " note=partial-is-not-full-domain-coverage" << std::endl;
        return 0;
    }
    const auto cases = Enumerate();
    std::vector<std::string> identities;
    identities.reserve(cases.size());
    for (const auto& test : cases) identities.push_back(Id(test));
    std::sort(identities.begin(), identities.end());
    behavior::Require(std::adjacent_find(identities.begin(), identities.end()) == identities.end(),
        "matrix case IDs must be unique");
    if (mode == "--matrix-case")
    {
        behavior::Require(argc == 3, "--matrix-case requires an exact ID");
        const auto it = std::find_if(cases.begin(), cases.end(), [&](const auto& c) { return Id(c) == argv[2]; });
        behavior::Require(it != cases.end(), "unknown matrix ID");
        behavior::scene = argv[2];
        if (it->target == "desktop") behavior::FileCase(*it);
        else behavior::MatrixContainer(*it);
        NativeDragIntegrationAccess::ThrowAsyncException();
        std::cout << "PASS " << behavior::scene << std::endl;
        return 0;
    }
    behavior::Require(mode == "--matrix-list" || mode == "--matrix-run" || mode == "--matrix-run-slice", "unknown matrix command");
    behavior::Require(argc <= 5 && (argc <= 3 || argc == 5), "use [filter [shard-index shard-count]]");
    const std::string filter = argc >= 3 ? argv[2] : "";
    const std::size_t shard = argc == 5 ? std::stoull(argv[3]) : 0;
    const std::size_t shards = argc == 5 ? std::stoull(argv[4]) : 1;
    behavior::Require(shards > 0 && shard < shards, "invalid shard");
    // Preserve every case result in CTest/JUnit, including passing shards.
    std::cout << "CTEST_FULL_OUTPUT" << std::endl;
    std::cout << "MODEL external-ingress targets=" << std::size(targets) << " representation=" << std::size(iconValues)
        << " async=" << std::size(asyncValues)
        << " selection=" << std::size(directoryValues) << " release=" << std::size(gestures)
        << " capabilities=" << std::size(capabilities) << " enter=" << std::size(enterValues)
        << " end=" << std::size(cancelValues) << " raw=" << RawCount()
        << " excluded=" << RawCount() - cases.size() << " applicable=" << cases.size()
        << " excluded-desktop-icon=" << RawCount() / (std::size(targets) * std::size(iconValues))
        << " excluded-no-release=" << (RawCount() - RawCount() / (std::size(targets) * std::size(iconValues))) /
            std::size(cancelValues) / std::size(gestures) * (std::size(gestures) - 1)
        << " reasons=desktop-has-no-widget-icon,leave-has-no-release-event" << std::endl;
    std::size_t selected = 0, passed = 0, failed = 0;
    for (std::size_t index = 0; index < cases.size(); ++index)
    {
        const auto id = Id(cases[index]);
        if (index % shards != shard || id.find(filter) == std::string::npos) continue;
        ++selected;
        if (mode == "--matrix-list") std::cout << id << '\n';
        else
        {
            std::cout << "RUN " << id << std::endl;
            const auto code = RunChild(id);
            if (code == 0) { ++passed; std::cout << "PASS " << id << std::endl; }
            else { ++failed; std::cout << "FAIL " << id << " exit=" << code << std::endl; }
        }
    }
    behavior::Require(selected != 0, "matrix selection is empty");
    std::cout << "RESULT selected=" << selected << " pass=" << passed << " fail=" << failed
        << " skip=0 unexecuted=" << (mode == "--matrix-list" ? selected : 0)
        << " outside-selection=" << cases.size() - selected << std::endl;
    if (mode == "--matrix-run")
    {
        std::cout << "UNEXECUTED global-model: source, payload, relation, gesture, timing, environment and assertion domains are not fully mapped; total applicable paths unknown" << std::endl;
        return 2;
    }
    return failed ? 1 : 0;
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc >= 2 && (std::string_view(argv[1]) == "--v4-virtual-staging-child" ||
            std::string_view(argv[1]) == "--v4-owned-file-child"))
            return drag_global::VirtualChildEntry();
        if (argc >= 2 && (std::string_view(argv[1]) == "--global-v4-virtual-case" ||
            std::string_view(argv[1]) == "--global-v4-native-case" ||
            std::string_view(argv[1]) == "--global-v4-owned-file-case"))
            return drag_global::VirtualParentEntry();
        if (argc >= 2 && std::string_view(argv[1]).starts_with("--global-")) return drag_global::Command(argc, argv);
        if (argc >= 2 && std::string_view(argv[1]).starts_with("--matrix-")) return drag_matrix::Command(argc, argv);
        behavior::Require(argc == 2, "one named behavior is required");
        behavior::scene = argv[1];
        if (behavior::scene == "settings") behavior::Settings();
        else if (behavior::scene == "settings-host") behavior::SettingsHostCommit();
        else if (behavior::scene == "manifest") behavior::BuildManifest();
        else if (behavior::scene == "group-type-contract") behavior::GroupTypeContract();
        else if (behavior::scene == "recovery") behavior::Recovery();
        else if (behavior::scene == "search") behavior::Search();
        else if (behavior::scene == "widget") behavior::WidgetContracts();
        else if (behavior::scene == "dock") behavior::DockLifecycle();
        else if (behavior::scene == "dispatch-desktop" || behavior::scene == "dispatch-control")
            behavior::ProductionMessageWiring(behavior::scene == "dispatch-control");
        else if (behavior::scene == "negative-copy-bytes") behavior::CorruptCopyControl();
        else if (behavior::scene == "interleaved" || behavior::scene == "failed-first")
            behavior::Interleaved(behavior::scene == "failed-first");
        else if (behavior::RunContainerScenario(behavior::scene)) {}
        else behavior::Files(behavior::scene);
        NativeDragIntegrationAccess::ThrowAsyncException();
        std::cout << "PASS " << behavior::scene << '\n'; return 0;
    }
    catch (const std::exception& e) { std::cerr << "FAIL " << e.what() << '\n'; return 1; }
}
