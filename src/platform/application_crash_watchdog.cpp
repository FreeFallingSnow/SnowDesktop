#include "application_crash_watchdog.h"

#include "application_restart_policy.h"
#include "application_hang_watchdog.h"
#include "common/constants.h"

#include <dbghelp.h>

#include <array>
#include <bit>
#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace snowdesktop::application_hang_watchdog
{
namespace
{
constexpr wchar_t kHeartbeat[] = L"SnowDesktop.Health.Heartbeat";
constexpr wchar_t kReceived[] = L"SnowDesktop.Health.ButtonTime";
constexpr wchar_t kEnabled[] = L"SnowDesktop.Health.Enabled";
constexpr wchar_t kBusy[] = L"SnowDesktop.Health.Busy";
constexpr wchar_t kDesktop[] = L"SnowDesktop.Health.Desktop";
constexpr wchar_t kObserverTick[] = L"SnowDesktop.Health.ObserverTick";
constexpr wchar_t kRawAvailable[] = L"SnowDesktop.Health.RawAvailable";
constexpr wchar_t kRawButtons[] = L"SnowDesktop.Health.RawButtons";
constexpr wchar_t kTargetClicks[] = L"SnowDesktop.Health.TargetClicks";
constexpr wchar_t kLastRawTime[] = L"SnowDesktop.Health.LastRawTime";
constexpr wchar_t kLastTargetTime[] = L"SnowDesktop.Health.LastTargetTime";
constexpr wchar_t kLastRejection[] = L"SnowDesktop.Health.LastRejection";
constexpr wchar_t kPendingClicks[] = L"SnowDesktop.Health.PendingClicks";
constexpr wchar_t kRecoveryStage[] = L"SnowDesktop.Health.RecoveryStage";
UiSession* currentUi = nullptr;
HWND currentControl = nullptr;
RecoveryStage pendingRequest = RecoveryStage::None;
DWORD pendingRequestTime = 0;
UINT RecoveryMessage()
{
    static const UINT message = RegisterWindowMessageW(L"SnowDesktop.Health.Recover.v1");
    return message;
}
void Store(HWND window, const wchar_t* key, ULONG_PTR value)
{
    SetPropW(window, key, reinterpret_cast<HANDLE>(value));
}
ULONG_PTR Read(HWND window, const wchar_t* key)
{
    return reinterpret_cast<ULONG_PTR>(GetPropW(window, key));
}
bool IsButtonDown(UINT message)
{
    switch (message)
    {
    case WM_LBUTTONDOWN: case WM_LBUTTONDBLCLK:
    case WM_RBUTTONDOWN: case WM_RBUTTONDBLCLK:
    case WM_MBUTTONDOWN: case WM_MBUTTONDBLCLK:
    case WM_XBUTTONDOWN: case WM_XBUTTONDBLCLK:
    case WM_NCLBUTTONDOWN: case WM_NCLBUTTONDBLCLK:
    case WM_NCRBUTTONDOWN: case WM_NCRBUTTONDBLCLK:
        return true;
    default: return false;
    }
}
bool IsObservedWindow(HWND window, HWND control)
{
    // Publish the actual rendering HWND. The legacy proof-window class name
    // is different from the production desktop class and misses real clicks.
    if (window && reinterpret_cast<HWND>(Read(control, kDesktop)) == window) return true;
    wchar_t name[96]{};
    GetClassNameW(window, name, static_cast<int>(std::size(name)));
    const std::wstring_view type(name);
    return type == kFloatingDockWindowClassName ||
        type == L"SnowDesktop.StatusBar";
}
}

UiSession::UiSession(HWND control) : control_(control)
{
    if (currentUi || !IsWindow(control_)) return;
    currentUi = this;
    currentControl = control_;
    pendingRequest = RecoveryStage::None;
    hook_ = SetWindowsHookExW(WH_GETMESSAGE, ObserveMessage, nullptr, GetCurrentThreadId());
    timer_ = SetTimer(control_, RecoveryMessage(), 1000, Heartbeat);
    // A missing observation hook must disable automatic input repair.
    if (!hook_ || !timer_)
    {
        if (hook_) UnhookWindowsHookEx(hook_);
        if (timer_) KillTimer(control_, timer_);
        hook_ = nullptr;
        timer_ = 0;
        currentUi = nullptr;
        currentControl = nullptr;
        return;
    }
    Heartbeat(control_, 0, 0, 0);
}

UiSession::~UiSession()
{
    if (currentUi != this) return;
    if (hook_) UnhookWindowsHookEx(hook_);
    if (timer_) KillTimer(control_, timer_);
    for (const auto key : {kEnabled, kBusy, kHeartbeat, kReceived, kDesktop})
        RemovePropW(control_, key);
    currentUi = nullptr;
    currentControl = nullptr;
    pendingRequest = RecoveryStage::None;
}

void CALLBACK UiSession::Heartbeat(HWND window, UINT, UINT_PTR, DWORD)
{
    if (window == currentControl)
        Store(window, kHeartbeat, static_cast<ULONG_PTR>(GetTickCount64()));
}

LRESULT CALLBACK UiSession::ObserveMessage(int code, WPARAM wp, LPARAM lp)
{
    if (code >= 0 && wp == PM_REMOVE && currentControl)
    {
        const auto& message = *reinterpret_cast<const MSG*>(lp);
        const ULONGLONG now = GetTickCount64();
        if (now - Read(currentControl, kHeartbeat) >= 500)
            Heartbeat(currentControl, 0, 0, 0);
        if (IsButtonDown(message.message) && IsObservedWindow(message.hwnd, currentControl))
            Store(currentControl, kReceived, message.time);
        if (message.hwnd == currentControl && RecoveryMessage() &&
            message.message == RecoveryMessage() &&
            message.wParam >= static_cast<WPARAM>(RecoveryStage::ResetInput) &&
            message.wParam <= static_cast<WPARAM>(RecoveryStage::RestartApplication))
        {
            pendingRequest = static_cast<RecoveryStage>(message.wParam);
            pendingRequestTime = static_cast<DWORD>(message.lParam);
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void UiSession::Publish(bool enabled, bool busy, HWND desktop)
{
    if (!currentControl) return;
    Store(currentControl, kDesktop, reinterpret_cast<ULONG_PTR>(desktop));
    Store(currentControl, kBusy, busy ? 1 : 0);
    Store(currentControl, kEnabled, enabled ? 1 : 0);
}

RecoveryStage UiSession::TakeRequest()
{
    const auto stage = pendingRequest;
    pendingRequest = RecoveryStage::None;
    // A stale request queued during a long synchronous call is not evidence
    // that input is still broken when that call finally returns.
    return GetTickCount() - pendingRequestTime <= 10000 ? stage : RecoveryStage::None;
}

Sample ReadSample(HWND control, std::uint64_t now)
{
    return {now, Read(control, kHeartbeat), static_cast<DWORD>(Read(control, kReceived)),
        Read(control, kEnabled) != 0, Read(control, kBusy) != 0};
}

bool PostRecoveryRequest(HWND control, RecoveryStage stage)
{
    const UINT message = RecoveryMessage();
    return message && PostMessageW(control, message, static_cast<WPARAM>(stage),
        static_cast<LPARAM>(GetTickCount())) != FALSE;
}

DWORD RebindWindow(HWND child, HWND parent)
{
    if (!IsWindow(child) || !IsWindow(parent) || GetParent(child) != parent)
        return ERROR_INVALID_WINDOW_HANDLE;
    const auto childDpi = GetWindowDpiAwarenessContext(child);
    const auto parentDpi = GetWindowDpiAwarenessContext(parent);
    if (!childDpi || !parentDpi || !AreDpiAwarenessContextsEqual(childDpi, parentDpi))
        return ERROR_INVALID_STATE;
    const LONG_PTR style = GetWindowLongPtrW(child, GWL_STYLE);
    SetLastError(ERROR_SUCCESS);
    if (!SetParent(child, nullptr))
        return GetLastError() ? GetLastError() : ERROR_INVALID_FUNCTION;
    SetWindowLongPtrW(child, GWL_STYLE, (style & ~WS_CHILD) | WS_POPUP);
    // Restore the child style before crossing back into Explorer's desktop.
    SetWindowLongPtrW(child, GWL_STYLE, (style & ~WS_POPUP) | WS_CHILD);
    SetLastError(ERROR_SUCCESS);
    SetParent(child, parent);
    DWORD error = GetParent(child) == parent ? ERROR_SUCCESS : GetLastError();
    if (GetParent(child) != parent)
    {
        // A transient parent change can invalidate the first attachment. Make
        // one best-effort restoration, retaining the original failure result.
        if (!error) error = ERROR_INVALID_WINDOW_HANDLE;
        if (IsWindow(parent)) SetParent(child, parent);
    }
    SetWindowLongPtrW(child, GWL_STYLE, style);
    return error;
}

namespace
{
// External observer: Raw Input is registered in the watchdog process only. It
// does not replace the application's input registration or share its queue.
class Observer
{
public:
    explicit Observer(DWORD process) : process_(process)
    {
        previousDpi_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        WNDCLASSW type{};
        type.lpfnWndProc = WindowProc;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = L"SnowDesktop.Health.Observer";
        RegisterClassW(&type);
        window_ = CreateWindowExW(0, type.lpszClassName, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, this);
        RAWINPUTDEVICE mouse{1, 2, RIDEV_INPUTSINK, window_};
        inputAvailable_ = window_ && RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
    }
    ~Observer()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(control_, &pid);
        if (pid == process_)
            for (const auto key : {kObserverTick, kRawAvailable, kRawButtons,
                kTargetClicks, kLastRawTime, kLastTargetTime, kLastRejection,
                kPendingClicks, kRecoveryStage})
                RemovePropW(control_, key);
        if (inputAvailable_)
        {
            RAWINPUTDEVICE mouse{1, 2, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&mouse, 1, sizeof(mouse));
        }
        if (window_) DestroyWindow(window_);
        if (previousDpi_) SetThreadDpiAwarenessContext(previousDpi_);
    }
    void Pump(HANDLE process)
    {
        MSG message{};
        // Bound work so a flood of raw motion cannot starve exit/hang checks.
        for (unsigned i = 0; i < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i)
        {
            DispatchMessageW(&message);
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return;
        }
        const auto now = GetTickCount64();
        DWORD controlProcess = 0;
        GetWindowThreadProcessId(control_, &controlProcess);
        if (!IsWindow(control_) || controlProcess != process_)
        {
            control_ = nullptr;
            EnumWindows([](HWND window, LPARAM context) -> BOOL {
                auto& self = *reinterpret_cast<Observer*>(context);
                DWORD pid = 0;
                GetWindowThreadProcessId(window, &pid);
                if (pid != self.process_) return TRUE;
                wchar_t name[96]{};
                GetClassNameW(window, name, static_cast<int>(std::size(name)));
                if (std::wstring_view(name) != kControlWindowClassName) return TRUE;
                self.control_ = window;
                return FALSE;
            }, reinterpret_cast<LPARAM>(this));
        }
        auto sample = ReadSample(control_, now);
        // Menus/moves and a locked or switched input desktop are intentional
        // routing changes. Do not reinterpret them as missing button delivery.
        GUITHREADINFO gui{sizeof(gui)};
        const DWORD thread = GetWindowThreadProcessId(control_, nullptr);
        if (!thread || !GetGUIThreadInfo(thread, &gui) ||
            (gui.flags & (GUI_INMENUMODE | GUI_INMOVESIZE | GUI_SYSTEMMENUMODE | GUI_POPUPMENUMODE)))
            sample.busy = true;
        const bool pollGap = lastPoll_ && now > lastPoll_ + 3000;
        lastPoll_ = now;
        if (pollGap) hardHangSince_ = 0;
        if (sample.enabled && sample.heartbeat && now >= sample.heartbeat + 15000)
        {
            if (!hardHangSince_) hardHangSince_ = now;
            if (now >= hardHangSince_ + 1000)
                Report(process, sample, RecoveryStage::None, gui);
        }
        else hardHangSince_ = 0;
        const auto stage = monitor_.Poll(sample);
        if (monitor_.TakeRecoveryConfirmation() && !lastReportPath_.empty())
        {
            AppendEvidence("newButtonReceived=" + std::to_string(sample.receivedButtonTime) +
                " confirmationTick=" + std::to_string(sample.now) +
                " afterRequestedStage=" + std::to_string(static_cast<unsigned>(lastRequestedStage_)) + "\r\n");
        }
        if (stage != RecoveryStage::None)
        {
            Report(process, sample, stage, gui);
            const bool posted = PostRecoveryRequest(control_, stage);
            lastRequestedStage_ = stage;
            AppendEvidence("recoveryRequested=" + std::to_string(static_cast<unsigned>(stage)) +
                " requestTick=" + std::to_string(sample.now) + " posted=" + std::to_string(posted) + "\r\n");
            lastStatus_ = 0;
        }
        PublishStatus(GetTickCount64());
    }

private:
    // Diagnostic codes only; no cursor positions or unrelated input contents
    // are stored. These show which observation gate rejected the latest press.
    enum class Rejection : ULONG_PTR
    {
        None, StaleInput, DisabledOrBusy, NoCursor, OtherWindow,
        NotHostSurface, HitTest, InactiveDesktop
    };
    void PublishStatus(ULONGLONG now)
    {
        if (!control_ || (lastStatus_ && now < lastStatus_ + 1000)) return;
        lastStatus_ = now;
        Store(control_, kObserverTick, static_cast<ULONG_PTR>(now));
        Store(control_, kRawAvailable, inputAvailable_ ? 1 : 0);
        Store(control_, kRawButtons, rawButtons_);
        Store(control_, kTargetClicks, targetClicks_);
        Store(control_, kLastRawTime, lastRawTime_);
        Store(control_, kLastTargetTime, lastTargetTime_);
        Store(control_, kLastRejection, static_cast<ULONG_PTR>(lastRejection_));
        Store(control_, kPendingClicks, monitor_.PendingClicks());
        Store(control_, kRecoveryStage, static_cast<ULONG_PTR>(monitor_.Stage()));
    }
    void AppendEvidence(const std::string& detail)
    {
        if (lastReportPath_.empty()) return;
        HANDLE file = CreateFileW(lastReportPath_.c_str(), FILE_APPEND_DATA,
            FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD written = 0;
        WriteFile(file, detail.data(), static_cast<DWORD>(detail.size()), &written, nullptr);
        CloseHandle(file);
    }
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wp, LPARAM lp)
    {
        if (message == WM_NCCREATE)
            SetWindowLongPtrW(window, GWLP_USERDATA,
                reinterpret_cast<LONG_PTR>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams));
        auto* self = reinterpret_cast<Observer*>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self && message == WM_INPUT) self->OnInput(reinterpret_cast<HRAWINPUT>(lp));
        return DefWindowProcW(window, message, wp, lp);
    }
    void OnInput(HRAWINPUT input)
    {
        RAWINPUT data{};
        UINT size = sizeof(data);
        if (GetRawInputData(input, RID_INPUT, &data, &size, sizeof(RAWINPUTHEADER)) == UINT(-1) ||
            data.header.dwType != RIM_TYPEMOUSE ||
            !(data.data.mouse.usButtonFlags & (RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_RIGHT_BUTTON_DOWN)))
            return;
        const DWORD time = static_cast<DWORD>(GetMessageTime());
        ++rawButtons_;
        lastRawTime_ = time;
        if (GetTickCount() - time > 250) { lastRejection_ = Rejection::StaleInput; return; }
        const auto sample = ReadSample(control_, GetTickCount64());
        if (!sample.enabled || sample.busy) { lastRejection_ = Rejection::DisabledOrBusy; return; }
        POINT point{};
        if (!GetCursorPos(&point)) { lastRejection_ = Rejection::NoCursor; return; }
        const HWND hit = WindowFromPoint(point);
        DWORD pid = 0;
        const DWORD thread = GetWindowThreadProcessId(hit, &pid);
        if (pid != process_ || thread != GetWindowThreadProcessId(control_, nullptr) ||
            !IsWindowEnabled(hit) || !IsWindowVisible(hit))
        { lastRejection_ = Rejection::OtherWindow; return; }
        // Exclude transparent backdrops, Quick Navigation, editors and dialogs.
        if (!IsObservedWindow(hit, control_)) { lastRejection_ = Rejection::NotHostSurface; return; }
        // WindowFromPoint is only a rectangular candidate, not the actual
        // non-client hit test. Transparent bar/desktop regions are not failures.
        DWORD_PTR hitCode = HTNOWHERE;
        if (!SendMessageTimeoutW(hit, WM_NCHITTEST, 0, MAKELPARAM(point.x, point.y),
                SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &hitCode) || hitCode != HTCLIENT)
        { lastRejection_ = Rejection::HitTest; return; }
        HDESK desktop = OpenInputDesktop(0, FALSE, DESKTOP_READOBJECTS);
        if (!desktop) { lastRejection_ = Rejection::InactiveDesktop; return; }
        CloseDesktop(desktop);
        ++targetClicks_;
        lastTargetTime_ = time;
        lastRejection_ = Rejection::None;
        monitor_.ObserveClick(sample.now, time);
    }
    void Report(HANDLE process, const Sample& sample, RecoveryStage stage, const GUITHREADINFO& gui)
    {
        if (reports_ >= 3 || (lastReport_ && sample.now < lastReport_ + 60000)) return;
        lastReport_ = sample.now;
        ++reports_;
        wchar_t folder[32768]{};
        const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", folder, static_cast<DWORD>(std::size(folder)));
        if (!length || length >= std::size(folder)) return;
        const auto directory = std::filesystem::path(folder) / L"SnowDesktop" / L"hang-reports";
        std::error_code error;
        std::filesystem::create_directories(directory, error);
        if (error) return;
        const auto stem = directory / (L"hang-" + std::to_wstring(process_) + L"-" + std::to_wstring(sample.now));
        const auto dumpPath = stem.wstring() + L".dmp";
        DWORD dumpError = ERROR_MOD_NOT_FOUND;
        HMODULE library = LoadLibraryExW(L"dbghelp.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (library)
        {
            using WriteDump = decltype(&MiniDumpWriteDump);
            const auto writeDump = std::bit_cast<WriteDump>(GetProcAddress(library, "MiniDumpWriteDump"));
            HANDLE file = CreateFileW(dumpPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE)
            {
                const BOOL written = writeDump && writeDump(process, process_, file, MiniDumpWithThreadInfo,
                    nullptr, nullptr, nullptr);
                dumpError = written ? ERROR_SUCCESS : GetLastError();
                CloseHandle(file);
            }
            else dumpError = GetLastError();
            FreeLibrary(library);
        }
        const std::string detail = "pid=" + std::to_string(process_) +
            "\r\ntick=" + std::to_string(sample.now) +
            "\r\nheartbeat=" + std::to_string(sample.heartbeat) +
            "\r\nlastButton=" + std::to_string(sample.receivedButtonTime) +
            "\r\ndesktop=" + std::to_string(Read(control_, kDesktop)) +
            "\r\nrawButtons=" + std::to_string(rawButtons_) +
            "\r\ntargetClicks=" + std::to_string(targetClicks_) +
            "\r\nlastRejection=" + std::to_string(static_cast<ULONG_PTR>(lastRejection_)) +
            "\r\nstage=" + std::to_string(static_cast<unsigned>(stage)) +
            "\r\nguiFlags=" + std::to_string(gui.flags) +
            "\r\ncapture=" + std::to_string(reinterpret_cast<ULONG_PTR>(gui.hwndCapture)) +
            "\r\nfocus=" + std::to_string(reinterpret_cast<ULONG_PTR>(gui.hwndFocus)) +
            "\r\ndumpError=" + std::to_string(dumpError) + "\r\n";
        lastReportPath_ = stem.wstring() + L".txt";
        HANDLE file = CreateFileW(lastReportPath_.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE)
        {
            DWORD written = 0;
            WriteFile(file, detail.data(), static_cast<DWORD>(detail.size()), &written, nullptr);
            CloseHandle(file);
        }
    }
    DWORD process_ = 0;
    DPI_AWARENESS_CONTEXT previousDpi_ = nullptr;
    HWND window_ = nullptr, control_ = nullptr;
    bool inputAvailable_ = false;
    Monitor monitor_;
    ULONGLONG lastPoll_ = 0, hardHangSince_ = 0, lastReport_ = 0;
    unsigned reports_ = 0;
    ULONG_PTR rawButtons_ = 0, targetClicks_ = 0;
    DWORD lastRawTime_ = 0, lastTargetTime_ = 0;
    ULONGLONG lastStatus_ = 0;
    Rejection lastRejection_ = Rejection::None;
    RecoveryStage lastRequestedStage_ = RecoveryStage::None;
    std::wstring lastReportPath_;
};
}
}

namespace snowdesktop::application_crash_watchdog
{
WatchResult WatchProcess(HANDLE ownedProcessHandle,
    const PreventRestartCallback& preventRestart,
    const LaunchRestartCallback& launchRestart)
{
    WatchResult result;
    if (!ownedProcessHandle ||
        ownedProcessHandle == INVALID_HANDLE_VALUE)
    {
        result.error = ERROR_INVALID_HANDLE;
        return result;
    }

    DWORD waitResult = WAIT_FAILED;
    DWORD waitError = ERROR_SUCCESS;
    {
        application_hang_watchdog::Observer observer(GetProcessId(ownedProcessHandle));
        for (;;)
        {
            waitResult = MsgWaitForMultipleObjectsEx(1, &ownedProcessHandle,
                500, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (waitResult == WAIT_OBJECT_0 || waitResult == WAIT_FAILED)
            {
                if (waitResult == WAIT_FAILED) waitError = GetLastError();
                break;
            }
            try { observer.Pump(ownedProcessHandle); }
            catch (...) { /* Optional hang diagnostics must not disable crash recovery. */ }
        }
    }
    const bool readExitCode = waitResult == WAIT_OBJECT_0 &&
        GetExitCodeProcess(ownedProcessHandle, &result.exitCode) != FALSE;
    result.error = readExitCode ? ERROR_SUCCESS :
        (waitResult == WAIT_FAILED ? waitError : GetLastError());
    CloseHandle(ownedProcessHandle);
    if (!readExitCode)
        return result;

    result.crashDetected = application_restart_policy::
        IsCrashExitCode(result.exitCode);
    if (!result.crashDetected)
        return result;

    result.restartPrevented = preventRestart && preventRestart();
    if (result.restartPrevented)
        return result;

    result.restartAttempted = true;
    result.restartLaunched =
        launchRestart && launchRestart(GetCurrentProcessId());
    return result;
}

bool StartForCurrentProcess(std::wstring_view executablePath)
{
    if (executablePath.empty())
        return false;

    HANDLE processHandle = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), GetCurrentProcess(),
            GetCurrentProcess(), &processHandle,
            SYNCHRONIZE | PROCESS_QUERY_INFORMATION | PROCESS_VM_READ,
            TRUE, 0))
    {
        return false;
    }

    SIZE_T attributeBytes = 0;
    InitializeProcThreadAttributeList(
        nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0)
    {
        CloseHandle(processHandle);
        return false;
    }
    std::vector<std::byte> attributeStorage(attributeBytes);
    auto* attributes = reinterpret_cast<
        PPROC_THREAD_ATTRIBUTE_LIST>(attributeStorage.data());
    if (!InitializeProcThreadAttributeList(
            attributes, 1, 0, &attributeBytes))
    {
        CloseHandle(processHandle);
        return false;
    }
    if (!UpdateProcThreadAttribute(attributes, 0,
            PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            &processHandle, sizeof(processHandle), nullptr, nullptr))
    {
        DeleteProcThreadAttributeList(attributes);
        CloseHandle(processHandle);
        return false;
    }

    const std::wstring executable(executablePath);
    std::wstring handleArgument(
        application_restart_policy::kWatchProcessHandlePrefix);
    handleArgument += std::to_wstring(
        reinterpret_cast<std::uintptr_t>(processHandle));
    std::wstring commandLine =
        L"\"" + executable + L"\" " + handleArgument;

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.lpAttributeList = attributes;
    PROCESS_INFORMATION processInfo{};
    const bool started = CreateProcessW(
        executable.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
        EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
        nullptr, nullptr, &startup.StartupInfo, &processInfo) != FALSE;
    DeleteProcThreadAttributeList(attributes);
    CloseHandle(processHandle);
    if (!started)
        return false;
    CloseHandle(processInfo.hThread);
    CloseHandle(processInfo.hProcess);
    return true;
}
}
