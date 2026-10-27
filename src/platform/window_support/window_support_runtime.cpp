#include "window_support_runtime.h"
#include "window_support_authorization.h"
#include "window_support_security.h"
#include "window_support_pipe.h"
#include "window_pointer_observer.h"
#include "window_pointer_events.h"
#include "window_operation_failures.h"
#include "platform/operation_feedback.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>
#include <algorithm>

namespace snowdesktop::window_support
{
struct Runtime::Impl
{
    Impl() { status.capabilities.inProcess = Mask(Capability::WindowPin) | Mask(Capability::WindowControl); }
    struct Work { protocol::Request request; Completion completion; };
    struct Configuration { bool enabled; protocol::WindowIdentity owner; std::function<void(DWORD)> completion; };
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    PointerObserver pointer;
    SupportStatus status;
    protocol::Response snapshot;
    std::deque<Work> work, finished;
    std::optional<Configuration> configuration;
    std::deque<std::pair<std::function<void(DWORD)>, DWORD>> configured;
    std::deque<protocol::PointerEvent> remotePointer;
    std::deque<Completion> animationCompletions;
    PointerCallback pointerCallback;
    std::function<void()> authorizationRequest;
    std::function<void()> statusChanged;
    std::optional<SupportStatus> publishedStatus;
    std::optional<protocol::WindowIdentity> pendingPin;
    ULONGLONG pendingPinAt = 0;
    std::optional<protocol::PointerEvent> currentPointer;
    protocol::PinAppearance appearance;
    HWND peekTarget = nullptr;
    bool stopping = false, running = false, polling = false, workerFailed = false;
    std::uint64_t nextSequence = 0;
    std::uint32_t wireVersion = protocol::Version;
    RecentPointerEvents recentPointerEvents;
    WindowOperationFailures failures;
    ULONGLONG lastResponse = 0;

    void Disconnect(DWORD error)
    {
        std::lock_guard guard(mutex);
        status.capabilities.helper = 0;
        status.capabilities.helperState = error ? ProviderState::Failed : ProviderState::Absent;
        status.error = error;
        ++status.capabilities.generation;
        snapshot = {};
        remotePointer.clear();
        for (auto& completion : animationCompletions)
        {
            protocol::Response failure;
            failure.error = error ? error : ERROR_NOT_CONNECTED;
            finished.push_back({{}, [completion = std::move(completion), failure](const auto&) { completion(failure); }});
        }
        animationCompletions.clear();
        for (auto& request : work)
        {
            protocol::Response response;
            response.sequence = request.request.sequence;
            response.error = error ? error : ERROR_NOT_CONNECTED;
            const auto command = request.request.command;
            finished.push_back({{}, [completion = std::move(request.completion), response, command](const auto&) {
                if (command == protocol::Command::TogglePin || command == protocol::Command::CloseWindow ||
                    command == protocol::Command::MinimizeWindow || command == protocol::Command::StartMinimize)
                    operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, response.error});
                if (completion) completion(response);
            }});
        }
        work.clear();
    }
    HANDLE Connect()
    {
        const auto name = HelperPipeName();
        if (name.empty()) return INVALID_HANDLE_VALUE;
        const HANDLE connection = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
            OPEN_EXISTING, FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
        if (connection == INVALID_HANDLE_VALUE) return connection;
        ULONG processId = 0;
        const HANDLE process = GetNamedPipeServerProcessId(connection, &processId)
            ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId) : nullptr;
        const bool valid = process && IsProtectedHelper(process);
        if (process) CloseHandle(process);
        if (!valid) { CloseHandle(connection); SetLastError(ERROR_ACCESS_DENIED); return INVALID_HANDLE_VALUE; }
        DWORD mode = PIPE_READMODE_MESSAGE;
        if (!SetNamedPipeHandleState(connection, &mode, nullptr, nullptr))
        { const DWORD error = GetLastError(); CloseHandle(connection); SetLastError(error); return INVALID_HANDLE_VALUE; }
        return connection;
    }
    bool Exchange(HANDLE connection, protocol::Request& request, protocol::Response& response)
    {
        request.sequence = ++nextSequence;
        request.version = wireVersion;
        request.hostProcess = GetCurrentProcessId();
        if (!pipe::Transfer(connection, &request, sizeof(request), true) ||
            !pipe::Transfer(connection, &response, sizeof(response), false)) return false;
        if (response.magic != protocol::Magic || response.version != request.version ||
            response.sequence != request.sequence || response.pointerCount > response.pointerEvents.size())
        { SetLastError(ERROR_REVISION_MISMATCH); return false; }
        return true;
    }
    void Publish(const protocol::Response& response)
    {
        if (response.asynchronousError)
            operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, response.asynchronousError});
        std::lock_guard guard(mutex);
        if (status.capabilities.helperState != ProviderState::Ready)
        {
            protocol::Request request;
            request.command = protocol::Command::PinAppearance;
            request.appearance = appearance;
            work.push_front({request, {}});
        }
        if (status.capabilities.helper != response.capabilities || status.capabilities.helperState != ProviderState::Ready)
            ++status.capabilities.generation;
        status.capabilities.helper = response.capabilities;
        status.capabilities.helperState = ProviderState::Ready;
        status.error = response.error;
        snapshot = response;
        if (!response.animationActive)
        {
            for (auto& completion : animationCompletions)
                finished.push_back({{}, [completion = std::move(completion), response](const auto&) { completion(response); }});
            animationCompletions.clear();
        }
        lastResponse = GetTickCount64();
        for (std::size_t i = 0; i < response.pointerCount; ++i)
        {
            if (remotePointer.size() == 128) remotePointer.pop_front();
            remotePointer.push_back(response.pointerEvents[i]);
        }
    }
    void Run()
    {
        HANDLE connection = INVALID_HANDLE_VALUE;
        auto authorization = QueryAuthorization();
        {
            std::lock_guard guard(mutex);
            if (!configuration)
            {
                status.authorization = authorization.state;
                status.error = authorization.error;
                status.updateAvailable = authorization.updateAvailable;
            }
        }
        ULONGLONG nextAttempt = 0;
        bool alternateVersionTried = false;
        for (;;)
        {
            std::optional<Configuration> configure;
            bool elevated = false;
            AuthorizationState authorized{};
            {
                std::unique_lock lock(mutex);
                wake.wait_for(lock, std::chrono::milliseconds(25), [&] { return stopping || configuration || !work.empty(); });
                if (stopping) break;
                configure = std::exchange(configuration, {});
                elevated = status.hostElevated;
                authorized = status.authorization;
            }
            if (configure)
            {
                HANDLE previous = nullptr;
                if (connection != INVALID_HANDLE_VALUE)
                {
                    ULONG process = 0;
                    if (GetNamedPipeServerProcessId(connection, &process)) previous = OpenProcess(SYNCHRONIZE, FALSE, process);
                    CloseHandle(connection);
                    connection = INVALID_HANDLE_VALUE;
                }
                // Reconfiguration intentionally retires the previous provider.
                // It is not a connection failure (2250) to show beside progress.
                Disconnect(ERROR_SUCCESS);
                // A previous owner is allowed to undo its own pins/cloak and
                // leave before explicitly reconfiguring the authorization.
                if (previous) { WaitForSingleObject(previous, 5000); CloseHandle(previous); }
                const DWORD error = !protocol::Current(configure->owner) ? ERROR_CANCELLED : elevated
                    ? static_cast<DWORD>(RunAuthorizationCommand(configure->enabled, GetCurrentProcessId()))
                    : ConfigureAuthorization(configure->enabled, protocol::Window(configure->owner), [this] {
                        std::lock_guard guard(mutex);
                        status.authorization = AuthorizationState::Configuring;
                    });
                authorization = QueryAuthorization();
                {
                    std::lock_guard guard(mutex);
                    status.authorization = authorization.state;
                    status.updateAvailable = authorization.updateAvailable;
                    status.error = error == ERROR_CANCELLED ? ERROR_SUCCESS : error ? error : authorization.error;
                    if (error || !configure->enabled) pendingPin.reset();
                    configured.emplace_back(std::move(configure->completion), error);
                }
                nextAttempt = 0;
                continue;
            }
            // Elevation is a provider lifecycle optimization only. Feature
            // consumers still resolve actual per-operation capabilities.
            if (elevated || authorized != AuthorizationState::Registered) continue;
            if (connection == INVALID_HANDLE_VALUE)
            {
                if (GetTickCount64() < nextAttempt) continue;
                {
                    std::lock_guard guard(mutex);
                    status.capabilities.helperState = ProviderState::Starting;
                    ++status.capabilities.generation;
                }
                connection = Connect();
                DWORD error = ERROR_SUCCESS;
                if (connection == INVALID_HANDLE_VALUE)
                {
                    error = StartAuthorizedHelper();
                    const ULONGLONG deadline = GetTickCount64() + 4000;
                    while (!error && GetTickCount64() < deadline)
                    {
                        { std::lock_guard guard(mutex); if (stopping) break; }
                        connection = Connect();
                        if (connection != INVALID_HANDLE_VALUE) break;
                        std::this_thread::sleep_for(std::chrono::milliseconds(40));
                    }
                }
                protocol::Request hello;
                protocol::Response response;
                if (connection == INVALID_HANDLE_VALUE || !Exchange(connection, hello, response) || !response.succeeded)
                {
                    if (!error) error = GetLastError();
                    if (connection != INVALID_HANDLE_VALUE) CloseHandle(connection);
                    connection = INVALID_HANDLE_VALUE;
                    Disconnect(error ? error : ERROR_NOT_CONNECTED);
                    const bool versionProbe = !alternateVersionTried && (error == ERROR_BROKEN_PIPE ||
                        error == ERROR_NO_DATA || error == ERROR_REVISION_MISMATCH || error == ERROR_INVALID_DATA);
                    if (versionProbe)
                    {
                        wireVersion = wireVersion == 2 ? 1 : 2;
                        alternateVersionTried = true;
                    }
                    nextAttempt = GetTickCount64() + (versionProbe ? 200 : 10000);
                    continue;
                }
                alternateVersionTried = false;
                Publish(response);
            }
            Work current;
            {
                std::lock_guard guard(mutex);
                if (!work.empty()) { current = std::move(work.front()); work.pop_front(); }
                else current.request.command = protocol::Command::Poll;
            }
            protocol::Response response;
            if (!Exchange(connection, current.request, response))
            {
                const DWORD error = GetLastError();
                CloseHandle(connection);
                connection = INVALID_HANDLE_VALUE;
                Disconnect(error);
                response.error = error;
            }
            else Publish(response);
            if (response.error && protocol::Current(current.request.target))
            {
                const auto command = current.request.command;
                const auto capability = command == protocol::Command::TogglePin ? Capability::WindowPin :
                    command == protocol::Command::StartMinimize || command == protocol::Command::StartRestore ? Capability::MinimizeAnimation :
                    command == protocol::Command::CloseWindow || command == protocol::Command::MinimizeWindow || command == protocol::Command::RestoreWindow
                        ? Capability::WindowControl : Capability::None;
                std::lock_guard guard(mutex);
                failures.Record(current.request.target, capability, Provider::Helper, response.error, GetTickCount64());
            }
            if (response.error && response.error != ERROR_BUSY && current.request.command != protocol::Command::Poll)
                operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, response.error});
            if (current.completion)
            {
                std::lock_guard guard(mutex);
                if (current.request.command == protocol::Command::StartRestore && response.succeeded && response.animationActive)
                    animationCompletions.push_back(std::move(current.completion));
                else finished.push_back({{}, [completion = std::move(current.completion), response](const auto&) { completion(response); }});
            }
        }
        if (connection != INVALID_HANDLE_VALUE) CloseHandle(connection);
        Disconnect(ERROR_SUCCESS);
    }
};
Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() { Stop(); }
void Runtime::Start()
{
    if (impl_->running) return;
    TokenIdentity identity;
    const bool elevated = ReadTokenIdentity(GetCurrentProcess(), identity) && identity.elevated &&
        identity.integrity >= SECURITY_MANDATORY_HIGH_RID;
    const bool pointer = impl_->pointer.Start();
    {
        std::lock_guard guard(impl_->mutex);
        impl_->stopping = false;
        impl_->workerFailed = false;
        impl_->running = true;
        impl_->status.hostElevated = elevated;
        impl_->status.capabilities.inProcess = (impl_->status.capabilities.inProcess & Mask(Capability::MinimizeAnimation)) |
            Mask(Capability::WindowPin) | Mask(Capability::WindowControl) |
            (pointer ? Mask(Capability::PointerObservation) : 0u);
        ++impl_->status.capabilities.generation;
    }
    try { impl_->worker = std::thread([state = impl_.get()] {
        try { state->Run(); }
        catch (...)
        {
            state->Disconnect(ERROR_GEN_FAILURE);
            std::lock_guard guard(state->mutex);
            if (AuthorizationPending(state->status.authorization))
                state->status.authorization = AuthorizationState::Invalid;
            state->configuration.reset();
            state->pendingPin.reset();
            state->workerFailed = true;
        }
    }); }
    catch (...)
    {
        impl_->pointer.Stop();
        impl_->Disconnect(ERROR_NOT_ENOUGH_MEMORY);
        std::lock_guard guard(impl_->mutex);
        impl_->workerFailed = true;
        impl_->status.authorization = AuthorizationState::Invalid;
    }
}
void Runtime::Stop()
{
    {
        std::lock_guard guard(impl_->mutex);
        if (!impl_->running) return;
        impl_->stopping = true;
    }
    impl_->wake.notify_one();
    if (impl_->worker.joinable()) impl_->worker.join();
    impl_->pointer.Stop();
    std::lock_guard guard(impl_->mutex);
    impl_->running = false;
    impl_->status.capabilities.inProcess = 0;
    impl_->pointerCallback = {};
    impl_->authorizationRequest = {};
    impl_->statusChanged = {};
    impl_->publishedStatus.reset();
    impl_->pendingPin.reset();
    impl_->finished.clear();
    impl_->configured.clear();
}
SupportStatus Runtime::Status() const
{
    std::lock_guard guard(impl_->mutex);
    auto status = impl_->status;
    if (impl_->running && !impl_->pointer.Available()) status.capabilities.inProcess &= ~Mask(Capability::PointerObservation);
    if (status.capabilities.helperState == ProviderState::Ready && GetTickCount64() - impl_->lastResponse > 2500)
    { status.capabilities.helper = 0; status.capabilities.helperState = ProviderState::Failed; }
    return status;
}
CapabilityRoute Runtime::Resolve(Capability capability, HWND target) const
{
    const auto status = Status();
    OperationContext context;
    context.targetCurrent = capability == Capability::PointerObservation || !target || TargetInCurrentSession(target);
    const HWND subject = target ? target : GetForegroundWindow();
    // Installing a local observer establishes its backend, not coverage of
    // every target input queue. Target eligibility belongs to the provider;
    // feature consumers still depend only on the resolved capability.
    context.inProcessEligible = !subject || NativeTargetAccessible(subject);
    if (capability == Capability::PointerObservation && status.capabilities.helperState == ProviderState::Ready &&
        Contains(status.capabilities.helper, capability)) context.inProcessEligible = false;
    if (target && capability != Capability::PointerObservation)
    {
        const auto identity = protocol::Capture(target);
        std::lock_guard guard(impl_->mutex);
        if (impl_->failures.Blocked(identity, capability, Provider::InProcess, GetTickCount64())) context.inProcessEligible = false;
        if (impl_->failures.Blocked(identity, capability, Provider::Helper, GetTickCount64())) context.helperEligible = false;
    }
    return ResolveCapability(status.capabilities, capability, context);
}
std::optional<std::uint32_t> Runtime::PointerButtons() const
{
    if (impl_->currentPointer)
        return impl_->currentPointer->buttons;
    const auto route = Resolve(Capability::PointerObservation);
    if (!route.Available()) return {};
    if (route.provider == Provider::Helper)
    { std::lock_guard guard(impl_->mutex); return impl_->snapshot.buttons; }
    // A local low-level hook is an event source, not an authoritative current
    // button state: another input queue may swallow its matching button-up.
    // Keep native polling on the original live User32 sampler.
    return {};
}
std::uint32_t Runtime::PointerPresses() const
{
    if (!impl_->currentPointer) return 0;
    switch (impl_->currentPointer->message)
    {
    case WM_LBUTTONDOWN: return 1;
    case WM_RBUTTONDOWN: return 2;
    case WM_MBUTTONDOWN: return 4;
    default: return 0;
    }
}
bool Runtime::PointerPosition(POINT& point) const
{
    if (impl_->currentPointer) { point = impl_->currentPointer->point; return true; }
    return GetCursorPos(&point) != FALSE;
}
void Runtime::Poll()
{
    if (impl_->polling) return;
    impl_->polling = true;
    struct PollScope { bool& value; ~PollScope() { value = false; } } scope{impl_->polling};
    std::deque<Impl::Work> finished;
    std::deque<std::pair<std::function<void(DWORD)>, DWORD>> configured;
    std::deque<protocol::PointerEvent> pointer;
    PointerCallback callback;
    {
        std::lock_guard guard(impl_->mutex);
        finished.swap(impl_->finished);
        configured.swap(impl_->configured);
        pointer.swap(impl_->remotePointer);
        callback = impl_->pointerCallback;
    }
    protocol::Response local;
    impl_->pointer.Collect(local);
    for (std::size_t i = 0; i < local.pointerCount; ++i) pointer.push_back(local.pointerEvents[i]);
    std::stable_sort(pointer.begin(), pointer.end(), [](const auto& a, const auto& b) {
        return static_cast<LONG>(a.time - b.time) < 0;
    });
    for (const auto& event : pointer)
    {
        // Both providers can observe an ordinary click during handoff. Deliver
        // it once, and discard delayed clicks rather than closing a new popup.
        if (!impl_->recentPointerEvents.Accept(event, GetTickCount())) continue;
        impl_->currentPointer = event;
        try { if (callback) callback(event); }
        catch (...) { operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, ERROR_GEN_FAILURE}); }
        impl_->currentPointer.reset();
    }
    for (auto& item : finished) if (item.completion) item.completion({});
    for (auto& item : configured) if (item.first) item.first(item.second);
    const auto currentStatus = Status();
    std::function<void()> changed;
    {
        std::lock_guard guard(impl_->mutex);
        if (impl_->publishedStatus != currentStatus)
        {
            impl_->publishedStatus = currentStatus;
            changed = impl_->statusChanged;
        }
    }
    try { if (changed) changed(); }
    catch (...) { operation_feedback::Report({"settings.general.windowSupport.operationFailed", {}, ERROR_GEN_FAILURE}); }
    std::optional<protocol::WindowIdentity> pin;
    {
        std::lock_guard guard(impl_->mutex);
        if (impl_->pendingPin && GetTickCount64() - impl_->pendingPinAt > 120000) impl_->pendingPin.reset();
        if (impl_->pendingPin && impl_->status.capabilities.helperState == ProviderState::Ready &&
            Contains(impl_->status.capabilities.helper, Capability::WindowPin)) pin = std::exchange(impl_->pendingPin, {});
    }
    if (pin && protocol::Current(*pin))
    {
        protocol::Request request;
        request.command = protocol::Command::TogglePin;
        request.target = *pin;
        Submit(request);
    }
}
void Runtime::SetPointerCallback(PointerCallback callback)
{ std::lock_guard guard(impl_->mutex); impl_->pointerCallback = std::move(callback); }
void Runtime::SetPinAppearance(protocol::PinAppearance appearance)
{
    { std::lock_guard guard(impl_->mutex); impl_->appearance = appearance; }
    protocol::Request request;
    request.command = protocol::Command::PinAppearance;
    request.appearance = appearance;
    Submit(request);
}
void Runtime::SetPinPeek(HWND target)
{
    { std::lock_guard guard(impl_->mutex); if (impl_->peekTarget == target) return; impl_->peekTarget = target; }
    protocol::Request request;
    request.command = protocol::Command::PeekTarget;
    request.target = protocol::Capture(target);
    Submit(request);
}
void Runtime::SetNativeCapability(Capability capability, bool available)
{
    std::lock_guard guard(impl_->mutex);
    if (available) impl_->status.capabilities.inProcess |= Mask(capability);
    else impl_->status.capabilities.inProcess &= ~Mask(capability);
    ++impl_->status.capabilities.generation;
}
void Runtime::RecordFailure(Capability capability, HWND target, Provider provider, DWORD error)
{
    if (!protocol::Current(protocol::Capture(target))) return;
    std::lock_guard guard(impl_->mutex);
    impl_->failures.Record(protocol::Capture(target), capability, provider, error, GetTickCount64());
    ++impl_->status.capabilities.generation;
}
void Runtime::SetAuthorizationRequestHandler(std::function<void()> handler)
{ std::lock_guard guard(impl_->mutex); impl_->authorizationRequest = std::move(handler); }
bool Runtime::RequestPinAuthorization(HWND target)
{
    std::function<void()> handler;
    {
        std::lock_guard guard(impl_->mutex);
        if (!impl_->running || impl_->status.hostElevated || !impl_->authorizationRequest || !TargetInCurrentSession(target)) return false;
        if (impl_->status.authorization == AuthorizationState::Registered && impl_->status.capabilities.helperState == ProviderState::Ready) return false;
        impl_->pendingPin = protocol::Capture(target);
        impl_->pendingPinAt = GetTickCount64();
        handler = impl_->authorizationRequest;
    }
    handler();
    return true;
}
bool Runtime::Submit(protocol::Request request, Completion completion)
{
    std::lock_guard guard(impl_->mutex);
    if (!impl_->running || impl_->status.hostElevated ||
        impl_->status.capabilities.helperState != ProviderState::Ready || impl_->work.size() >= 128) return false;
    if (request.command == protocol::Command::ConfigureTargets && !completion)
        for (auto& queued : impl_->work)
            if (queued.request.command == request.command && !queued.completion)
            { queued.request = std::move(request); return true; }
    impl_->work.push_back({std::move(request), std::move(completion)});
    impl_->wake.notify_one();
    return true;
}
void Runtime::Configure(bool enabled, HWND owner, std::function<void(DWORD)> completion)
{
    std::lock_guard guard(impl_->mutex);
    if (!impl_->running || impl_->workerFailed)
    {
        impl_->configured.emplace_back(std::move(completion), ERROR_PROCESS_ABORTED);
        return;
    }
    if (impl_->configuration || AuthorizationPending(impl_->status.authorization)) return;
    impl_->status.authorization = impl_->status.hostElevated ? AuthorizationState::Configuring : AuthorizationState::AwaitingConsent;
    impl_->status.error = ERROR_SUCCESS;
    impl_->configuration = Impl::Configuration{enabled, protocol::Capture(owner), std::move(completion)};
    impl_->wake.notify_one();
}
protocol::Response Runtime::HelperSnapshot() const
{ std::lock_guard guard(impl_->mutex); return impl_->snapshot; }
void Runtime::SetStatusChangedHandler(std::function<void()> handler)
{
    std::lock_guard guard(impl_->mutex);
    impl_->statusChanged = std::move(handler);
    impl_->publishedStatus.reset();
}
Runtime& WindowSupport() { static Runtime runtime; return runtime; }
}
