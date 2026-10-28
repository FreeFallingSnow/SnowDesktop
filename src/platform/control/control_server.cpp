#include "control_server.h"
#include <sddl.h>
#include <algorithm>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace snowdesktop::control
{
namespace
{
constexpr UINT DispatchMessageId = WM_APP + 1;
constexpr wchar_t WindowClass[] = L"SnowDesktop.ApplicationControl.v1";
struct Identity { std::wstring sid; DWORD session = 0, integrity = 0; };

bool ReadIdentity(HANDLE process, Identity& identity)
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(process, TOKEN_QUERY, &token)) return false;
    const auto read = [&](TOKEN_INFORMATION_CLASS type, std::vector<unsigned char>& bytes) {
        DWORD needed = 0;
        GetTokenInformation(token, type, nullptr, 0, &needed);
        if (!needed) return false;
        bytes.resize(needed);
        return GetTokenInformation(token, type, bytes.data(), needed, &needed) != FALSE;
    };
    std::vector<unsigned char> user, integrity;
    DWORD bytes = sizeof(identity.session);
    bool ok = read(TokenUser, user) && read(TokenIntegrityLevel, integrity) &&
        GetTokenInformation(token, TokenSessionId, &identity.session, bytes, &bytes);
    if (ok)
    {
        LPWSTR sid = nullptr;
        ok = ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid) != FALSE;
        if (ok) { identity.sid = sid; LocalFree(sid); }
        const auto label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
        if (!IsValidSid(label) || !*GetSidSubAuthorityCount(label)) ok = false;
        else identity.integrity = *GetSidSubAuthority(label, *GetSidSubAuthorityCount(label) - 1);
    }
    CloseHandle(token);
    return ok;
}

bool SamePeer(HANDLE pipe, const Identity& owner)
{
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid)) return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return false;
    Identity peer;
    const bool ok = ReadIdentity(process, peer) && peer.sid == owner.sid &&
        peer.session == owner.session && peer.integrity == owner.integrity;
    CloseHandle(process);
    return ok;
}

bool AwaitIo(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout, DWORD& transferred)
{
    HANDLE handles[]{stop, operation.hEvent};
    if (WaitForMultipleObjects(2, handles, FALSE, timeout) == WAIT_OBJECT_0 + 1)
        return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
    CancelIoEx(pipe, &operation);
    (void)GetOverlappedResult(pipe, &operation, &transferred, TRUE);
    return false;
}

bool Transfer(HANDLE pipe, void* data, DWORD length, bool writing, HANDLE stop, DWORD timeout)
{
    auto* bytes = static_cast<unsigned char*>(data);
    const ULONGLONG deadline = GetTickCount64() + timeout;
    DWORD done = 0;
    while (done < length)
    {
        if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) return false;
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        DWORD transferred = 0;
        bool ok = (writing ? WriteFile(pipe, bytes + done, length - done, &transferred, &operation)
            : ReadFile(pipe, bytes + done, length - done, &transferred, &operation)) != FALSE;
        if (!ok && GetLastError() == ERROR_IO_PENDING)
        {
            const auto now = GetTickCount64();
            ok = AwaitIo(pipe, operation, stop,
                now < deadline ? static_cast<DWORD>(deadline - now) : 0, transferred);
        }
        CloseHandle(operation.hEvent);
        if (!ok || !transferred) return false;
        done += transferred;
    }
    return true;
}

bool Utf8(std::string_view text)
{
    const bool bom = text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xef &&
        static_cast<unsigned char>(text[1]) == 0xbb && static_cast<unsigned char>(text[2]) == 0xbf;
    return !text.empty() && !bom && MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        text.data(), static_cast<int>(text.size()), nullptr, 0) > 0;
}
}

std::wstring EndpointName()
{
    Identity identity;
    if (!ReadIdentity(GetCurrentProcess(), identity)) return {};
    return L"\\\\.\\pipe\\SnowDesktop.Control.v1." + std::to_wstring(identity.session) + L"." + identity.sid;
}

struct Server::Impl
{
    struct Pending
    {
        enum class Phase { Queued, Executing, Complete, Cancelled };
        std::mutex mutex;
        Phase phase = Phase::Queued;
        std::string request, response;
        HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        ~Pending() { if (done) CloseHandle(done); }
    };
    HANDLE stop = nullptr, pipe = INVALID_HANDLE_VALUE;
    HWND window = nullptr;
    DWORD timeout = 5000;
    std::atomic<DWORD> error{ERROR_SUCCESS};
    std::atomic<bool> running{false};
    Identity identity;
    Handler handler;
    std::thread worker;
    std::mutex queueMutex;
    std::deque<std::shared_ptr<Pending>> queue;

    static LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp)
    {
        auto* self = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_NCCREATE)
        {
            self = static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        }
        if (self && message == DispatchMessageId) { self->Drain(); return 0; }
        return DefWindowProcW(hwnd, message, wp, lp);
    }

    void Drain() noexcept
    {
        std::deque<std::shared_ptr<Pending>> ready;
        { std::lock_guard lock(queueMutex); ready.swap(queue); }
        for (const auto& request : ready)
        {
            {
                std::lock_guard lock(request->mutex);
                if (request->phase != Pending::Phase::Queued) continue;
                if (WaitForSingleObject(stop, 0) == WAIT_OBJECT_0)
                { request->phase = Pending::Phase::Cancelled; SetEvent(request->done); continue; }
                request->phase = Pending::Phase::Executing;
            }
            auto response = Dispatch(request->request, handler);
            { std::lock_guard lock(request->mutex); request->response = std::move(response); request->phase = Pending::Phase::Complete; }
            SetEvent(request->done);
        }
    }

    bool Connect()
    {
        OVERLAPPED operation{};
        operation.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!operation.hEvent) return false;
        bool connected = ConnectNamedPipe(pipe, &operation) != FALSE;
        const DWORD status = connected ? ERROR_SUCCESS : GetLastError();
        if (status == ERROR_PIPE_CONNECTED) connected = true;
        else if (status == ERROR_IO_PENDING)
        {
            DWORD transferred = 0;
            connected = AwaitIo(pipe, operation, stop, INFINITE, transferred);
        }
        CloseHandle(operation.hEvent);
        return connected;
    }

    void Run() noexcept
    {
        try
        {
            while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0)
            {
                if (!Connect()) break;
                if (SamePeer(pipe, identity)) Serve();
                DisconnectNamedPipe(pipe);
            }
        }
        catch (...) { error.store(ERROR_UNHANDLED_EXCEPTION); }
        running.store(false);
    }

    void Serve()
    {
        // One request per connection. A client that stalls loses its slot.
        DWORD length = 0;
        if (!Transfer(pipe, &length, sizeof(length), false, stop, timeout) || !length || length > MaximumFrameBytes) return;
        auto request = std::make_shared<Pending>();
        if (!request->done) return;
        request->request.resize(length);
        if (!Transfer(pipe, request->request.data(), length, false, stop, timeout) || !Utf8(request->request)) return;
        {
            std::lock_guard lock(queueMutex);
            // Expired work is retired by the UI thread; never retain an
            // unbounded backlog while it is inside a modal Shell operation.
            if (queue.size() >= 16) return;
            queue.push_back(request);
        }
        if (!PostMessageW(window, DispatchMessageId, 0, 0))
        { std::lock_guard lock(request->mutex); request->phase = Pending::Phase::Cancelled; return; }
        HANDLE handles[]{stop, request->done};
        if (WaitForMultipleObjects(2, handles, FALSE, timeout) != WAIT_OBJECT_0 + 1)
        {
            // A queued timeout can still be cancelled without side effects.
            // Once executing, connection loss cannot undo an applied action.
            std::lock_guard lock(request->mutex);
            if (request->phase == Pending::Phase::Queued) request->phase = Pending::Phase::Cancelled;
            return;
        }
        std::string response;
        { std::lock_guard lock(request->mutex); if (request->phase != Pending::Phase::Complete) return; response = request->response; }
        length = static_cast<DWORD>(response.size());
        if (Transfer(pipe, &length, sizeof(length), true, stop, timeout) &&
            Transfer(pipe, response.data(), length, true, stop, timeout))
        {
            // DisconnectNamedPipe discards unread buffered bytes. An ACK
            // confirms the caller consumed the response before disconnect.
            unsigned char acknowledgement = 0;
            (void)Transfer(pipe, &acknowledgement, 1, false, stop, timeout);
        }
    }
};

Server::Server() : impl_(std::make_unique<Impl>()) {}
Server::~Server() { Stop(); }
bool Server::Running() const noexcept { return impl_->running.load(); }
DWORD Server::LastError() const noexcept { return impl_->error.load(); }

bool Server::Start(Handler handler, std::wstring endpoint, DWORD timeoutMilliseconds)
{
    if (impl_->window) { impl_->error.store(ERROR_ALREADY_INITIALIZED); return false; }
    if (!handler || !timeoutMilliseconds || !ReadIdentity(GetCurrentProcess(), impl_->identity))
    { impl_->error.store(ERROR_INVALID_PARAMETER); return false; }
    if (endpoint.empty()) endpoint = EndpointName();
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto security = L"D:P(A;;GA;;;" + impl_->identity.sid + L")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(security.c_str(), SDDL_REVISION_1, &descriptor, nullptr))
    { impl_->error.store(GetLastError()); return false; }
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), descriptor, FALSE};
    impl_->pipe = CreateNamedPipeW(endpoint.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, &attributes);
    const DWORD pipeError = GetLastError();
    LocalFree(descriptor);
    if (impl_->pipe == INVALID_HANDLE_VALUE) { impl_->error.store(pipeError); return false; }
    impl_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!impl_->stop) { impl_->error.store(GetLastError()); Stop(); return false; }
    WNDCLASSW wc{}; wc.hInstance = GetModuleHandleW(nullptr); wc.lpfnWndProc = Impl::WindowProc; wc.lpszClassName = WindowClass;
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    { impl_->error.store(GetLastError()); Stop(); return false; }
    impl_->window = CreateWindowExW(0, WindowClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, impl_.get());
    if (!impl_->window) { impl_->error.store(GetLastError()); Stop(); return false; }
    impl_->handler = std::move(handler);
    impl_->timeout = timeoutMilliseconds;
    impl_->error.store(ERROR_SUCCESS);
    impl_->running.store(true);
    try { impl_->worker = std::thread([self = impl_.get()] { self->Run(); }); }
    catch (...) { impl_->error.store(ERROR_NOT_ENOUGH_MEMORY); Stop(); return false; }
    return true;
}

void Server::Stop() noexcept
{
    if (impl_->stop) SetEvent(impl_->stop);
    if (impl_->pipe != INVALID_HANDLE_VALUE) CancelIoEx(impl_->pipe, nullptr);
    if (impl_->worker.joinable()) impl_->worker.join();
    if (impl_->window) { DestroyWindow(impl_->window); impl_->window = nullptr; }
    { std::lock_guard lock(impl_->queueMutex); impl_->queue.clear(); }
    if (impl_->pipe != INVALID_HANDLE_VALUE) { CloseHandle(impl_->pipe); impl_->pipe = INVALID_HANDLE_VALUE; }
    if (impl_->stop) { CloseHandle(impl_->stop); impl_->stop = nullptr; }
    impl_->handler = {};
    impl_->running.store(false);
}
}
