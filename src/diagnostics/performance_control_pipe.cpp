#include "performance_control_pipe.h"
#include "performance_capture.h"
#include "performance_trace.h"

#include <windows.h>
#include <sddl.h>
#include <array>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace snowdesktop::performance
{
namespace
{
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
struct Identity { std::wstring sid; DWORD session = 0, integrity = 0; };
bool ReadIdentity(HANDLE process, Identity& result)
{
    Handle token{ nullptr };
    if (!OpenProcessToken(process, TOKEN_QUERY, &token.value)) return false;
    DWORD size = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    std::vector<std::byte> user(size);
    if (!GetTokenInformation(token.value, TokenUser, user.data(), size, &size)) return false;
    wchar_t* text = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<const TOKEN_USER*>(user.data())->User.Sid, &text)) return false;
    try { result.sid = text; }
    catch (...) { LocalFree(text); throw; }
    LocalFree(text);
    if (!GetTokenInformation(token.value, TokenSessionId, &result.session, sizeof(result.session), &size)) return false;
    size = 0;
    GetTokenInformation(token.value, TokenIntegrityLevel, nullptr, 0, &size);
    if (!size || size > 65536) return false;
    std::vector<std::byte> integrity(size);
    if (!GetTokenInformation(token.value, TokenIntegrityLevel, integrity.data(), size, &size)) return false;
    const auto sid = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data())->Label.Sid;
    if (!IsValidSid(sid) || !*GetSidSubAuthorityCount(sid)) return false;
    result.integrity = *GetSidSubAuthority(sid, *GetSidSubAuthorityCount(sid) - 1);
    return true;
}
bool SamePrincipal(HANDLE pipe, const Identity& server)
{
    ULONG pid = 0;
    if (!GetNamedPipeClientProcessId(pipe, &pid) || !pid) return false;
    Handle process{ OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) };
    Identity client;
    return process.value && ReadIdentity(process.value, client) && client.sid == server.sid &&
        client.session == server.session && client.integrity == server.integrity;
}
struct PipeState
{
    std::mutex mutex;
    std::thread worker;
    HANDLE stop = nullptr;
    std::atomic<bool> alive{ false };
    ~PipeState()
    {
        if (stop) SetEvent(stop);
        if (worker.joinable()) worker.join();
        if (stop) CloseHandle(stop);
    }
};
PipeState pipeState;

bool Await(HANDLE pipe, OVERLAPPED& operation, HANDLE stop, DWORD timeout, DWORD& transferred)
{
    const std::array handles{ stop, operation.hEvent };
    const auto wait = WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, timeout);
    if (wait != WAIT_OBJECT_0 + 1)
    {
        CancelIoEx(pipe, &operation);
        // The OVERLAPPED and buffer must outlive cancellation completion.
        GetOverlappedResult(pipe, &operation, &transferred, TRUE);
        return false;
    }
    return GetOverlappedResult(pipe, &operation, &transferred, FALSE) != FALSE;
}
bool Transfer(HANDLE pipe, void* bytes, DWORD length, bool write, HANDLE stop, ULONGLONG deadline)
{
    DWORD offset = 0;
    while (offset < length)
    {
        const auto now = GetTickCount64();
        if (now >= deadline || WaitForSingleObject(stop, 0) == WAIT_OBJECT_0) return false;
        Handle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        if (!event.value) return false;
        OVERLAPPED operation{}; operation.hEvent = event.value;
        DWORD transferred = 0;
        auto* buffer = static_cast<std::byte*>(bytes) + offset;
        const bool ready = write ? WriteFile(pipe, buffer, length - offset, &transferred, &operation) != FALSE
            : ReadFile(pipe, buffer, length - offset, &transferred, &operation) != FALSE;
        if (!ready && (GetLastError() != ERROR_IO_PENDING ||
                !Await(pipe, operation, stop, static_cast<DWORD>(deadline - now), transferred))) return false;
        if (!transferred) return false;
        offset += transferred;
    }
    return true;
}
void Serve(HANDLE pipe, HANDLE stop, Identity identity) noexcept
{
    Handle ownedPipe{ pipe };
    try
    {
        while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0)
        {
            Handle event{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
            if (!event.value) break;
            OVERLAPPED operation{}; operation.hEvent = event.value;
            DWORD unused = 0;
            const bool connected = ConnectNamedPipe(pipe, &operation) != FALSE;
            if (!connected)
            {
                const auto error = GetLastError();
                if (error != ERROR_PIPE_CONNECTED && (error != ERROR_IO_PENDING || !Await(pipe, operation, stop, INFINITE, unused))) break;
            }
            if (!SamePrincipal(pipe, identity)) { DisconnectNamedPipe(pipe); continue; }
            const auto deadline = GetTickCount64() + 3000;
            DWORD length = 0;
            if (!Transfer(pipe, &length, sizeof(length), false, stop, deadline) || !length || length > 16384)
            { DisconnectNamedPipe(pipe); continue; }
            std::string request(length, '\0');
            if (!Transfer(pipe, request.data(), length, false, stop, deadline))
            { DisconnectNamedPipe(pipe); continue; }
            std::string response;
            { Scope scope("profiler", "control.request"); response = ProcessControlRequest(request); }
            if (response.size() > 65536) response = "{\"protocolVersion\":3,\"state\":\"failed\",\"errorCode\":\"controlResponseLimit\"}";
            length = static_cast<DWORD>(response.size());
            const auto writeDeadline = GetTickCount64() + 3000;
            if (Transfer(pipe, &length, sizeof(length), true, stop, writeDeadline) &&
                Transfer(pipe, response.data(), length, true, stop, writeDeadline))
            {
                // An acknowledged frame avoids an unbounded FlushFileBuffers
                // and prevents disconnecting before the client reads the reply.
                unsigned char acknowledgement = 0;
                (void)Transfer(pipe, &acknowledgement, 1, false, stop, GetTickCount64() + 1000);
            }
            DisconnectNamedPipe(pipe);
        }
    }
    catch (...) { /* Endpoint failure cannot affect desktop work. */ }
    pipeState.alive.store(false);
}
}

bool EnsureControlPipe() noexcept
{
    std::lock_guard lock(pipeState.mutex);
    if (pipeState.alive.load()) return true;
    if (pipeState.worker.joinable()) pipeState.worker.join();
    if (pipeState.stop) { CloseHandle(pipeState.stop); pipeState.stop = nullptr; }
    try
    {
        Identity identity;
        if (!ReadIdentity(GetCurrentProcess(), identity)) return false;
        const auto descriptor = L"D:P(A;;GA;;;SY)(A;;GA;;;" + identity.sid + L")";
        PSECURITY_DESCRIPTOR security = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(descriptor.c_str(), SDDL_REVISION_1, &security, nullptr)) return false;
        SECURITY_ATTRIBUTES attributes{ sizeof(attributes), security, FALSE };
        const auto name = L"\\\\.\\pipe\\SnowDesktop.Performance.v3." + std::to_wstring(GetCurrentProcessId());
        const auto pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65540, 16388, 0, &attributes);
        LocalFree(security);
        if (pipe == INVALID_HANDLE_VALUE) return false;
        pipeState.stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!pipeState.stop) { CloseHandle(pipe); return false; }
        pipeState.alive.store(true);
        try { pipeState.worker = std::thread(Serve, pipe, pipeState.stop, std::move(identity)); }
        catch (...) { pipeState.alive.store(false); CloseHandle(pipe); CloseHandle(pipeState.stop); pipeState.stop = nullptr; return false; }
        return true;
    }
    catch (...) { return false; }
}
void StopControlPipe() noexcept
{
    std::lock_guard lock(pipeState.mutex);
    if (pipeState.stop) SetEvent(pipeState.stop);
    if (pipeState.worker.joinable()) pipeState.worker.join();
    if (pipeState.stop) { CloseHandle(pipeState.stop); pipeState.stop = nullptr; }
    pipeState.alive.store(false);
}
}
