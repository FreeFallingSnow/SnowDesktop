#include "settings_ipc_channel.h"
#include "platform/pending_window_message.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>

namespace snowdesktop::settings_ipc
{
namespace
{
constexpr UINT DispatchMessageId = WM_APP + 0x681;
constexpr UINT_PTR AsyncTimerId = 1;
constexpr std::uint32_t Magic = 0x53444950; // SDIP
// Font settings extend the snapshot; restart state queries the host's active font.
constexpr std::uint32_t Version = 31;
constexpr std::size_t HeaderSize = 24;
constexpr std::size_t MaximumQueuedBytes = MaximumFrameBytes * 2;
constexpr std::size_t MaximumQueuedItems = 1024;
enum class Kind : std::uint32_t { Request = 1, Reply = 2, Error = 3 };

void Release(HANDLE& handle) noexcept
{
    if (handle && handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    handle = nullptr;
}
bool Transfer(HANDLE pipe, void* data, std::size_t size, bool writing)
{
    auto* position = static_cast<std::byte*>(data);
    while (size)
    {
        DWORD transferred = 0;
        const DWORD count = static_cast<DWORD>(size);
        const BOOL ok = writing
            ? WriteFile(pipe, position, count, &transferred, nullptr)
            : ReadFile(pipe, position, count, &transferred, nullptr);
        if (!ok || !transferred) return false;
        position += transferred;
        size -= transferred;
    }
    return true;
}
}

struct Channel::Impl
{
    struct Gate
    {
        std::mutex mutex;
        Channel* owner = nullptr;
    };
    std::shared_ptr<Gate> gate = std::make_shared<Gate>();
    struct Frame
    {
        Kind kind{};
        std::uint64_t id = 0;
        Bytes data;
    };
    DWORD ownerThread = GetCurrentThreadId();
    HWND window = nullptr;
    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    HANDLE peerProcess = nullptr;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::thread reader;
    std::thread writer;
    std::atomic<bool> connected = false;
    std::mutex mutex;
    std::condition_variable writeReady;
    std::deque<Frame> incoming;
    std::deque<Bytes> outgoing;
    std::size_t outgoingBytes = 0;
    std::deque<std::function<void()>> tasks;
    std::size_t queuedBytes = 0;
    std::unordered_map<std::string, Handler> handlers;
    std::unordered_map<std::string, DeferredHandler> deferredHandlers;
    std::uint64_t connectionEpoch = 0;
    std::unordered_map<std::uint64_t, Frame> replies;
    std::unordered_set<std::uint64_t> pending;
    struct AsyncRequest
    {
        Completion completion;
        std::uint64_t deadline;
        std::string name;
    };
    std::unordered_map<std::uint64_t, AsyncRequest> asynchronous;
    std::uint64_t nextId = 0;
    bool disconnectDelivered = true;
    std::function<void()> disconnected;

    Impl()
    {
        if (!ready) throw ProtocolError("cannot create settings IPC event");
        WNDCLASSW klass{};
        klass.hInstance = GetModuleHandleW(nullptr);
        klass.lpfnWndProc = Procedure;
        klass.lpszClassName = L"SnowDesktop.SettingsIpc.Endpoint";
        if (!RegisterClassW(&klass) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        {
            Release(ready);
            throw ProtocolError("cannot register settings IPC endpoint");
        }
        window = CreateWindowExW(0, klass.lpszClassName, L"", 0,
            0, 0, 0, 0, HWND_MESSAGE, nullptr, klass.hInstance, this);
        if (!window)
        {
            Release(ready);
            throw ProtocolError("cannot create settings IPC endpoint");
        }
    }
    ~Impl()
    {
        Close();
        if (window) DestroyWindow(window);
        Release(ready);
    }
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == DispatchMessageId)
        {
            self->Dispatch();
            return 0;
        }
        if (self && message == WM_TIMER && wp == AsyncTimerId)
        {
            self->CheckAsyncTimeouts();
            return 0;
        }
        return DefWindowProcW(hwnd, message, wp, lp);
    }
    void Signal() noexcept
    {
        SetEvent(ready);
        PostMessageW(window, DispatchMessageId, 0, 0);
    }
    void Fail() noexcept
    {
        {
            std::lock_guard lock(mutex);
            connected = false;
        }
        writeReady.notify_all();
        Signal();
    }
    void Close() noexcept
    {
        ++connectionEpoch;
        KillTimer(window, AsyncTimerId);
        asynchronous.clear();
        {
            // Change the wait predicate under the writer's mutex. An atomic
            // store alone can lose the notification just before it waits.
            std::lock_guard lock(mutex);
            connected = false;
        }
        writeReady.notify_all();
        for (auto* worker : {&reader, &writer})
        {
            if (!worker->joinable()) continue;
            // A worker can pass its connected check immediately before the
            // first cancellation. Repeat until that exact thread has exited.
            while (WaitForSingleObject(worker->native_handle(), 0) == WAIT_TIMEOUT)
            {
                CancelSynchronousIo(worker->native_handle());
                WaitForSingleObject(worker->native_handle(), 10);
            }
            worker->join();
        }
        Release(readPipe);
        Release(writePipe);
        Release(peerProcess);
        std::lock_guard lock(mutex);
        incoming.clear();
        outgoing.clear();
        outgoingBytes = 0;
        queuedBytes = 0;
        replies.clear();
        pending.clear();
        disconnectDelivered = true;
    }
    void ReadLoop() noexcept
    {
        try
        {
            while (connected)
            {
                std::array<std::byte, HeaderSize> header{};
                if (!Transfer(readPipe, header.data(), header.size(), false)) break;
                auto [magic, version, kind, count, id] = Unpack<std::tuple<
                    std::uint32_t, std::uint32_t, Kind, std::uint32_t, std::uint64_t>>(header);
                if (magic != Magic || version != Version || count > MaximumFrameBytes ||
                    (kind != Kind::Request && kind != Kind::Reply && kind != Kind::Error) ||
                    (kind != Kind::Request && id == 0)) break;
                Frame frame{kind, id, Bytes(count)};
                if (!Transfer(readPipe, frame.data.data(), count, false)) break;
                {
                    std::lock_guard lock(mutex);
                    if (incoming.size() >= MaximumQueuedItems ||
                        count > MaximumQueuedBytes - queuedBytes) break;
                    queuedBytes += count;
                    incoming.push_back(std::move(frame));
                }
                Signal();
            }
        }
        catch (...) {}
        Fail();
    }
    void Send(Kind kind, std::uint64_t id, const Bytes& data)
    {
        if (data.size() > MaximumFrameBytes) throw ProtocolError("oversized IPC frame");
        auto header = Pack(Magic, Version, kind, static_cast<std::uint32_t>(data.size()), id);
        header.insert(header.end(), data.begin(), data.end());
        {
            std::unique_lock lock(mutex);
            if (!connected || !writePipe || outgoing.size() >= MaximumQueuedItems ||
                header.size() > MaximumQueuedBytes - outgoingBytes)
            {
                lock.unlock();
                Fail();
                throw ProtocolError("settings process disconnected or stalled");
            }
            outgoingBytes += header.size();
            outgoing.push_back(std::move(header));
        }
        writeReady.notify_one();
    }
    void WriteLoop() noexcept
    {
        try
        {
            while (connected)
            {
                Bytes data;
                {
                    std::unique_lock lock(mutex);
                    writeReady.wait(lock, [this] { return !connected || !outgoing.empty(); });
                    if (!connected) break;
                    data = std::move(outgoing.front());
                    outgoing.pop_front();
                    outgoingBytes -= data.size();
                }
                if (!Transfer(writePipe, data.data(), data.size(), true)) break;
            }
        }
        catch (...) {}
        Fail();
    }
    void FailAsync(std::exception_ptr error) noexcept
    {
        auto requests = std::move(asynchronous);
        asynchronous.clear();
        KillTimer(window, AsyncTimerId);
        for (const auto& [id, request] : requests) pending.erase(id);
        for (auto& [id, request] : requests)
        {
            try { request.completion({}, error); } catch (...) {}
        }
    }
    void CheckAsyncTimeouts() noexcept
    {
        const auto now = GetTickCount64();
        for (const auto& [id, request] : asynchronous)
        {
            if (now < request.deadline) continue;
            const auto error = std::make_exception_ptr(ProtocolError(
                "settings IPC response timed out: " + request.name));
            // A timed-out operation has an unknown outcome. Retire this
            // connection before invoking a callback that might reopen it.
            Fail();
            FailAsync(error);
            Dispatch();
            return;
        }
    }
    void Dispatch() noexcept
    {
        // Pop before invoking. Nested RPC waits are allowed to dispatch the
        // next request, but can never execute the current frame a second time.
        for (;;)
        {
            Frame frame;
            std::function<void()> task;
            {
                std::lock_guard lock(mutex);
                if (!incoming.empty())
                {
                    frame = std::move(incoming.front());
                    incoming.pop_front();
                    queuedBytes -= frame.data.size();
                }
                else if (!tasks.empty())
                {
                    task = std::move(tasks.front());
                    tasks.pop_front();
                }
                else { ResetEvent(ready); break; }
            }
            try
            {
                if (task) { task(); continue; }
                if (frame.kind != Kind::Request)
                {
                    if (!pending.contains(frame.id) || replies.contains(frame.id))
                        throw ProtocolError("unexpected settings IPC reply");
                    if (const auto found = asynchronous.find(frame.id);
                        found != asynchronous.end())
                    {
                        auto completion = std::move(found->second.completion);
                        asynchronous.erase(found);
                        pending.erase(frame.id);
                        if (asynchronous.empty()) KillTimer(window, AsyncTimerId);
                        std::exception_ptr error;
                        if (frame.kind == Kind::Error)
                            error = std::make_exception_ptr(ProtocolError(Unpack<std::string>(frame.data)));
                        completion(std::move(frame.data), error);
                        continue;
                    }
                    replies.emplace(frame.id, std::move(frame));
                    continue;
                }
                Reader input(frame.data);
                std::string name;
                // Byte payloads use a string to keep the collection limit
                // separate from the maximum frame size.
                std::string payload;
                input(name, payload);
                input.Finish();
                if (const auto deferred = deferredHandlers.find(name); deferred != deferredHandlers.end())
                {
                    auto handler = deferred->second;
                    const auto epoch = connectionEpoch;
                    const auto id = frame.id;
                    std::weak_ptr<Gate> weak = gate;
                    auto replied = std::make_shared<std::atomic<bool>>(false);
                    Completion respond = [weak, epoch, id, replied](Bytes reply, std::exception_ptr error) {
                            if (replied->exchange(true) || !id) return;
                            const auto lifetime = weak.lock();
                            if (!lifetime) return;
                            std::lock_guard lock(lifetime->mutex);
                            if (!lifetime->owner) return;
                            auto* endpoint = lifetime->owner->impl_.get();
                            lifetime->owner->Post([endpoint, epoch, id, reply = std::move(reply), error]() {
                                if (!endpoint->connected || endpoint->connectionEpoch != epoch) return;
                                try
                                {
                                    if (error) std::rethrow_exception(error);
                                    endpoint->Send(Kind::Reply, id, reply);
                                }
                                catch (const std::exception& failure)
                                {
                                    try { endpoint->Send(Kind::Error, id, Pack(std::string(failure.what()))); }
                                    catch (...) { endpoint->Fail(); }
                                }
                                catch (...) { endpoint->Fail(); }
                            });
                        };
                    try { handler(std::as_bytes(std::span(payload.data(), payload.size())), respond); }
                    catch (...) { respond({}, std::current_exception()); }
                    continue;
                }
                const auto found = handlers.find(name);
                if (found == handlers.end()) throw ProtocolError("unknown settings IPC operation");
                // A callback may unbind itself; retain the callable by value.
                auto handler = found->second;
                auto result = handler(std::as_bytes(std::span(payload.data(), payload.size())));
                if (frame.id) Send(Kind::Reply, frame.id, result);
            }
            catch (const std::exception& error)
            {
                try
                {
                    if (frame.kind == Kind::Request && frame.id)
                        Send(Kind::Error, frame.id, Pack(std::string(error.what())));
                    else Fail();
                }
                catch (...) { Fail(); }
            }
            catch (...) { Fail(); }
        }
        if (!connected && !disconnectDelivered)
        {
            const auto epoch = connectionEpoch;
            disconnectDelivered = true;
            FailAsync(std::make_exception_ptr(ProtocolError("settings process disconnected")));
            // Completion may have explicitly closed or replaced the connection.
            if (connected || connectionEpoch != epoch) return;
            try { if (disconnected) disconnected(); } catch (...) {}
        }
    }
};

Channel::Channel() : impl_(std::make_unique<Impl>()) { impl_->gate->owner = this; }
Channel::~Channel()
{
    std::lock_guard lock(impl_->gate->mutex);
    impl_->gate->owner = nullptr;
}
void Channel::Open(HANDLE readPipe, HANDLE writePipe, HANDLE peerProcess)
{
    impl_->Close();
    impl_->readPipe = readPipe;
    impl_->writePipe = writePipe;
    impl_->peerProcess = peerProcess;
    if (GetCurrentThreadId() != impl_->ownerThread ||
        GetFileType(readPipe) != FILE_TYPE_PIPE || GetFileType(writePipe) != FILE_TYPE_PIPE ||
        !peerProcess || WaitForSingleObject(peerProcess, 0) != WAIT_TIMEOUT)
    {
        impl_->Close();
        throw ProtocolError("invalid settings IPC connection");
    }
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(writePipe, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(peerProcess, HANDLE_FLAG_INHERIT, 0);
    impl_->connected = true;
    impl_->disconnectDelivered = false;
    try
    {
        impl_->reader = std::thread([this] { impl_->ReadLoop(); });
        impl_->writer = std::thread([this] { impl_->WriteLoop(); });
    }
    catch (...) { impl_->Close(); throw; }
}
void Channel::Close() noexcept { impl_->Close(); }
bool Channel::Connected() const noexcept { return impl_->connected; }
HWND Channel::Window() const noexcept { return impl_->window; }
bool Channel::Post(std::function<void()> task)
{
    if (!task) return false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->tasks.size() >= MaximumQueuedItems) return false;
        impl_->tasks.push_back(std::move(task));
    }
    impl_->Signal();
    return true;
}
std::function<bool(std::function<void()>)> Channel::Poster()
{
    std::weak_ptr<Impl::Gate> weak = impl_->gate;
    return [weak](std::function<void()> task) {
        const auto gate = weak.lock();
        if (!gate) return false;
        std::lock_guard lock(gate->mutex);
        return gate->owner && gate->owner->Post(std::move(task));
    };
}
std::function<HWND()> Channel::WindowProvider()
{
    std::weak_ptr<Impl::Gate> weak = impl_->gate;
    return [weak]() -> HWND {
        const auto gate = weak.lock();
        if (!gate) return nullptr;
        std::lock_guard lock(gate->mutex);
        return gate->owner ? gate->owner->Window() : nullptr;
    };
}
void Channel::SetDisconnected(std::function<void()> callback)
{
    impl_->disconnected = std::move(callback);
}
void Channel::BindRaw(std::string name, Handler handler)
{
    if (name.empty() || name.size() > 128 || !handler)
        throw ProtocolError("invalid settings IPC handler");
    impl_->deferredHandlers.erase(name);
    impl_->handlers.insert_or_assign(std::move(name), std::move(handler));
}
void Channel::BindDeferredRaw(std::string name, DeferredHandler handler)
{
    if (name.empty() || name.size() > 128 || !handler)
        throw ProtocolError("invalid settings IPC handler");
    impl_->handlers.erase(name);
    impl_->deferredHandlers.insert_or_assign(std::move(name), std::move(handler));
}
void Channel::Unbind(std::string_view name)
{
    impl_->handlers.erase(std::string(name));
    impl_->deferredHandlers.erase(std::string(name));
}
void Channel::NotifyRaw(std::string_view name, Bytes arguments)
{
    impl_->Send(Kind::Request, 0, Pack(std::string(name), std::string(
        reinterpret_cast<const char*>(arguments.data()), arguments.size())));
}
void Channel::RequestAsync(std::string_view name, Bytes arguments,
    Completion completion, DWORD timeoutMs)
{
    if (GetCurrentThreadId() != impl_->ownerThread || !completion ||
        impl_->pending.size() >= 32)
        throw ProtocolError("invalid settings IPC call context");
    const auto id = ++impl_->nextId;
    impl_->pending.insert(id);
    try
    {
        impl_->asynchronous.emplace(id, Impl::AsyncRequest{std::move(completion),
            timeoutMs == INFINITE ? UINT64_MAX : GetTickCount64() + timeoutMs, std::string(name)});
        if (impl_->asynchronous.size() == 1 && !SetTimer(impl_->window, AsyncTimerId, 50, nullptr))
            throw ProtocolError("cannot schedule settings IPC timeout");
        impl_->Send(Kind::Request, id, Pack(std::string(name), std::string(
            reinterpret_cast<const char*>(arguments.data()), arguments.size())));
    }
    catch (...)
    {
        impl_->asynchronous.erase(id);
        impl_->pending.erase(id);
        if (impl_->asynchronous.empty()) KillTimer(impl_->window, AsyncTimerId);
        impl_->Fail();
        throw;
    }
}
Bytes Channel::Request(std::string_view name, Bytes arguments, DWORD timeoutMs)
{
    if (GetCurrentThreadId() != impl_->ownerThread || impl_->pending.size() >= 32)
        throw ProtocolError("invalid settings IPC call context");
    const auto id = ++impl_->nextId;
    impl_->pending.insert(id);
    try
    {
        impl_->Send(Kind::Request, id, Pack(std::string(name), std::string(
            reinterpret_cast<const char*>(arguments.data()), arguments.size())));
        const auto deadline = timeoutMs == INFINITE ? UINT64_MAX : GetTickCount64() + timeoutMs;
        for (;;)
        {
            impl_->Dispatch();
            if (const auto found = impl_->replies.find(id); found != impl_->replies.end())
            {
                auto reply = std::move(found->second);
                impl_->replies.erase(found);
                impl_->pending.erase(id);
                if (reply.kind == Kind::Error)
                    throw ProtocolError(Unpack<std::string>(reply.data));
                return std::move(reply.data);
            }
            if (!impl_->connected) throw ProtocolError("settings process disconnected");
            const auto now = GetTickCount64();
            if (now >= deadline) throw ProtocolError("settings IPC response timed out: " + std::string(name));
            HANDLE handles[] = {impl_->ready, impl_->peerProcess};
            const DWORD wait = MsgWaitForMultipleObjectsEx(2, handles,
                timeoutMs == INFINITE ? INFINITE : static_cast<DWORD>(deadline - now), QS_SENDMESSAGE,
                MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0 + 1 || wait == WAIT_FAILED)
                throw ProtocolError("settings process exited");
            MSG message{};
            PendingWindowMessage pending;
            while ((pending = TakePendingWindowMessage(message, impl_->window,
                DispatchMessageId)) == PendingWindowMessage::Ready)
                DispatchMessageW(&message);
            if (pending == PendingWindowMessage::Quit)
                throw ProtocolError("settings IPC interrupted by application exit");
        }
    }
    catch (...)
    {
        impl_->pending.erase(id);
        // A timed-out mutation has an unknown outcome. Disconnect instead of
        // retrying it or accepting a late reply in another settings session.
        impl_->Fail();
        throw;
    }
}
} // namespace snowdesktop::settings_ipc
