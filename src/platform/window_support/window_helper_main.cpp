#include "window_support_authorization.h"
#include "window_support_pipe.h"
#include "window_support_protocol.h"
#include "window_support_security.h"
#include "window_support_host_identity.h"
#include "window_pointer_observer.h"
#include "dock/dock_window_pin.h"
#include "dock/dock_window_helper_animation.h"
#include "theme/personalization.h"
#include "platform/operation_feedback.h"

#include <sddl.h>
#include <shellapi.h>
#include <algorithm>
#include <array>
#include <cwchar>
#include <fstream>
#include <memory>

namespace snowdesktop::window_support
{
namespace
{
snowdesktop::UiAnimationScheduler* animationScheduler = nullptr;
struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Security
{
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, FALSE};
    explicit Security(const std::wstring& sid)
    {
        const auto text = L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;" + sid + L")S:(ML;;NW;;;ME)";
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(text.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
            return;
        attributes.lpSecurityDescriptor = descriptor;
    }
    ~Security() { if (descriptor) LocalFree(descriptor); }
};
bool PumpWait(HANDLE event, HANDLE owner, DWORD timeout)
{
    const ULONGLONG deadline = GetTickCount64() + timeout;
    for (;;)
    {
        HANDLE handles[]{event, nullptr, nullptr};
        DWORD count = 1;
        const DWORD ownerIndex = owner ? count : MAXDWORD;
        if (owner) handles[count++] = owner;
        const DWORD animationIndex = animationScheduler && animationScheduler->WaitHandle() ? count : MAXDWORD;
        if (animationIndex != MAXDWORD) handles[count++] = animationScheduler->WaitHandle();
        const auto now = GetTickCount64();
        if (now >= deadline) { SetLastError(ERROR_TIMEOUT); return false; }
        const DWORD remaining = static_cast<DWORD>((std::min)(deadline - now, 1000ULL));
        const DWORD result = MsgWaitForMultipleObjects(count, handles, FALSE, remaining, QS_ALLINPUT);
        if (result == WAIT_OBJECT_0) return true;
        if (owner && result == WAIT_OBJECT_0 + ownerIndex) { SetLastError(ERROR_PROCESS_ABORTED); return false; }
        if (animationIndex != MAXDWORD && result == WAIT_OBJECT_0 + animationIndex) animationScheduler->DispatchDue();
        if (result == WAIT_FAILED) return false;
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            if (message.message == WM_QUIT) { SetLastError(ERROR_CANCELLED); return false; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
}
bool Transfer(HANDLE connection, void* data, DWORD size, bool write, HANDLE owner)
{
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD bytes = 0;
    BOOL result = write ? WriteFile(connection, data, size, &bytes, &operation)
                        : ReadFile(connection, data, size, &bytes, &operation);
    DWORD error = result ? ERROR_SUCCESS : GetLastError();
    if (!result && error == ERROR_IO_PENDING)
    {
        if (!PumpWait(event.value, owner, 5000))
        {
            error = GetLastError();
            CancelIoEx(connection, &operation);
            GetOverlappedResult(connection, &operation, &bytes, TRUE);
        }
        else
        {
            result = GetOverlappedResult(connection, &operation, &bytes, FALSE);
            error = result ? ERROR_SUCCESS : GetLastError();
        }
    }
    if (error || bytes != size) { SetLastError(error ? error : ERROR_INVALID_DATA); return false; }
    return true;
}
bool AcceptedHost(HANDLE connection, DWORD expectedProcess, Handle& owner)
{
    ULONG process = 0;
    if (!GetNamedPipeClientProcessId(connection, &process) || process != expectedProcess) return false;
    owner.value = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, process);
    if (!owner.value || !SameInteractiveUser(owner.value)) return false;
    const auto path = std::filesystem::path(ProcessImage(owner.value));
    if (_wcsicmp(path.filename().c_str(), L"SnowDesktop.exe") != 0) return false;
    const auto identityPath = HelperDirectory() / L"host-path.bin";
    const auto bytes = std::filesystem::file_size(identityPath);
    if (!bytes || bytes > 65534 || bytes % sizeof(wchar_t)) return false;
    std::wstring expected(static_cast<std::size_t>(bytes / sizeof(wchar_t)), L'\0');
    std::ifstream identity(identityPath, std::ios::binary);
    identity.read(reinterpret_cast<char*>(expected.data()), static_cast<std::streamsize>(bytes));
    return identity && AuthorizedHostMatches(expected, owner.value);
}
struct Engine
{
    PointerObserver pointer;
    DockWindowPin pins;
    DockWindowHelperAnimation animation;
    std::atomic<DWORD> pendingError{ERROR_SUCCESS};
    CapabilityMask Capabilities() const noexcept
    {
        return Mask(Capability::WindowPin) | Mask(Capability::WindowControl) |
            (pointer.Available() ? Mask(Capability::PointerObservation) : 0u) |
            (animation.Available() ? Mask(Capability::MinimizeAnimation) : 0u);
    }
    protocol::Response Execute(const protocol::Request& request)
    {
        protocol::Response response;
        response.sequence = request.sequence;
        response.capabilities = Capabilities();
        const HWND window = protocol::Window(request.target);
        const bool targeted = request.command == protocol::Command::TogglePin ||
            request.command == protocol::Command::CloseWindow || request.command == protocol::Command::MinimizeWindow ||
            request.command == protocol::Command::RestoreWindow || request.command == protocol::Command::StartMinimize ||
            request.command == protocol::Command::StartRestore;
        if (targeted && (!protocol::Current(request.target) || !TargetInCurrentSession(window)))
        { response.error = ERROR_INVALID_WINDOW_HANDLE; return response; }
        SetLastError(ERROR_SUCCESS);
        switch (request.command)
        {
        case protocol::Command::Hello: case protocol::Command::Poll:
            response.succeeded = TRUE;
            break;
        case protocol::Command::TogglePin:
            response.succeeded = pins.Toggle(window);
            break;
        case protocol::Command::PinAppearance:
        {
            PersonalizationSettings appearance;
            appearance.windowPinBorderEnabled = request.appearance.enabled != 0;
            appearance.windowPinBorderWidth = request.appearance.width;
            appearance.windowPinBorderR = request.appearance.red;
            appearance.windowPinBorderG = request.appearance.green;
            appearance.windowPinBorderB = request.appearance.blue;
            appearance.windowPinBorderOpacity = request.appearance.opacity;
            pins.SetAppearance(appearance);
            response.succeeded = TRUE;
            break;
        }
        case protocol::Command::PeekTarget:
            pins.SetPeekTarget(protocol::Current(request.target) ? window : nullptr);
            response.succeeded = TRUE;
            break;
        case protocol::Command::ClearPins:
            pins.Clear();
            response.succeeded = TRUE;
            break;
        case protocol::Command::CloseWindow:
            response.succeeded = PostMessageW(window, WM_CLOSE, 0, 0);
            break;
        case protocol::Command::MinimizeWindow:
            response.succeeded = PostMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0);
            break;
        case protocol::Command::RestoreWindow:
            response.succeeded = ShowWindowAsync(window, IsIconic(window) ? SW_RESTORE : SW_SHOW);
            // Foreground ownership is independent of access to show/restore.
            // The host keeps its existing activation/readback policy.
            break;
        case protocol::Command::ConfigureTargets:
            response.succeeded = animation.Configure(request);
            break;
        case protocol::Command::StartMinimize: case protocol::Command::StartRestore:
            if (request.animation.identity.window != request.target.window ||
                request.animation.identity.process != request.target.process || request.animation.identity.thread != request.target.thread)
            { response.error = ERROR_INVALID_DATA; break; }
            response.succeeded = animation.Start(request.animation, request.command == protocol::Command::StartRestore);
            if (!response.succeeded)
            {
                // Once accepted by the helper, preserve the user's native
                // minimize/restore intent even if the shared image is unavailable.
                response.succeeded = request.command == protocol::Command::StartMinimize
                    ? PostMessageW(window, WM_SYSCOMMAND, SC_MINIMIZE, 0)
                    : ShowWindowAsync(window, SW_RESTORE);
            }
            break;
        case protocol::Command::CancelAnimation: case protocol::Command::CompleteAnimation:
            animation.Cancel(request.command == protocol::Command::CompleteAnimation);
            response.succeeded = TRUE;
            break;
        default:
            response.error = ERROR_NOT_SUPPORTED;
            break;
        }
        if (!response.succeeded && !response.error)
        { response.error = GetLastError(); if (!response.error) response.error = ERROR_GEN_FAILURE; }
        pointer.Collect(response);
        pins.Refresh();
        animation.Snapshot(response);
        const auto asynchronous = pendingError.exchange(ERROR_SUCCESS);
        if (asynchronous != response.error) response.asynchronousError = asynchronous;
        return response;
    }
};
int RunHelper()
{
    if (!IsProtectedHelper(GetCurrentProcess())) return ERROR_ACCESS_DENIED;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    const auto name = HelperPipeName();
    if (name.empty()) return ERROR_INVALID_SID;
    const auto mutexName = L"Local\\SnowDesktop.WindowSupport." + CurrentUserSid();
    Handle instance{CreateMutexW(nullptr, FALSE, mutexName.c_str())};
    if (!instance.value) return static_cast<int>(GetLastError());
    if (GetLastError() == ERROR_ALREADY_EXISTS) return ERROR_ALREADY_EXISTS;
    Security security(CurrentUserSid());
    if (!security.descriptor) return static_cast<int>(GetLastError());
    Handle connection{CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, sizeof(protocol::Response), sizeof(protocol::Request), 0, &security.attributes)};
    if (connection.value == INVALID_HANDLE_VALUE) return static_cast<int>(GetLastError());
    Handle connected{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!connected.value) return static_cast<int>(GetLastError());
    OVERLAPPED operation{};
    operation.hEvent = connected.value;
    const BOOL immediate = ConnectNamedPipe(connection.value, &operation);
    const DWORD error = immediate ? ERROR_SUCCESS : GetLastError();
    if (error == ERROR_IO_PENDING)
    {
        if (!PumpWait(connected.value, nullptr, 15000))
        {
            CancelIoEx(connection.value, &operation);
            DWORD transferred = 0;
            GetOverlappedResult(connection.value, &operation, &transferred, TRUE);
            return ERROR_TIMEOUT;
        }
    }
    else if (error != ERROR_PIPE_CONNECTED && error != ERROR_SUCCESS) return static_cast<int>(error);
    protocol::Request request;
    if (!Transfer(connection.value, &request, sizeof(request), false, nullptr) ||
        request.magic != protocol::Magic || request.version != protocol::Version || request.command != protocol::Command::Hello)
        return ERROR_INVALID_DATA;
    Handle host;
    if (!AcceptedHost(connection.value, request.hostProcess, host)) return ERROR_ACCESS_DENIED;
    const DWORD hostProcess = request.hostProcess;
    std::uint64_t lastSequence = 0;
    Engine engine;
    operation_feedback::SetReporter([&engine](const operation_feedback::Failure& failure) {
        DWORD empty = ERROR_SUCCESS;
        engine.pendingError.compare_exchange_strong(empty, failure.error);
    });
    struct ReporterScope { ~ReporterScope() { operation_feedback::SetReporter({}); } } reporterScope;
    engine.animation.Initialize();
    animationScheduler = &engine.animation.Scheduler();
    struct SchedulerScope { ~SchedulerScope() { animationScheduler = nullptr; } } schedulerScope;
    engine.pointer.Start();
    for (;;)
    {
        if (request.magic != protocol::Magic || request.version != protocol::Version ||
            request.hostProcess != hostProcess || request.sequence <= lastSequence ||
            request.targetCount > request.targets.size()) break;
        lastSequence = request.sequence;
        if (request.command == protocol::Command::Disconnect) break;
        auto response = engine.Execute(request);
        if (!Transfer(connection.value, &response, sizeof(response), true, host.value) ||
            !Transfer(connection.value, &request, sizeof(request), false, host.value)) break;
    }
    engine.pointer.Stop();
    engine.animation.Cancel(true);
    engine.pins.Clear();
    // Preserve the pin manager's asynchronous retirement until its queued
    // native undo has had a bounded opportunity to finish.
    Handle cleanup{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (cleanup.value) PumpWait(cleanup.value, nullptr, 1000);
    return ERROR_SUCCESS;
}
}
}
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int)
{
    using namespace snowdesktop::window_support;
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int count = 0;
    LPWSTR* args = CommandLineToArgvW(GetCommandLineW(), &count);
    if (!args) return static_cast<int>(GetLastError());
    int result = ERROR_INVALID_PARAMETER;
    try
    {
        if (count == 1) result = RunHelper();
        else if (count == 3 && (wcscmp(args[1], L"--authorize") == 0 || wcscmp(args[1], L"--revoke") == 0))
        {
            wchar_t* end = nullptr;
            const auto process = wcstoul(args[2], &end, 10);
            if (process && end && !*end)
                result = RunAuthorizationCommand(wcscmp(args[1], L"--authorize") == 0, process);
        }
    }
    catch (const std::bad_alloc&) { result = ERROR_OUTOFMEMORY; }
    catch (...) { result = ERROR_GEN_FAILURE; }
    LocalFree(args);
    if (SUCCEEDED(com)) CoUninitialize();
    return result;
}
