#include "widget_native_task_executor.h"
#include "widget/packages/widget_package.h"
#include "common/json_value.h"

#include <windows.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstddef>
#include <mutex>
#include <thread>
#include <utility>

namespace snowdesktop::widget_runtime
{
namespace
{
constexpr std::size_t MaximumMessage = 16384;
struct Handle
{
    HANDLE value = nullptr;
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    explicit operator bool() const { return value && value != INVALID_HANDLE_VALUE; }
};
struct DirectoryWatch
{
    Handle directory, changed{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
    OVERLAPPED overlap{};
    alignas(DWORD) std::array<unsigned char, 8192> buffer{};
    BY_HANDLE_FILE_INFORMATION identity{};
    bool armed = false;
    ~DirectoryWatch()
    {
        if (armed)
        {
            CancelIoEx(directory.value, &overlap);
            DWORD ignored = 0;
            GetOverlappedResult(directory.value, &overlap, &ignored, TRUE);
        }
    }
    bool Begin(const std::filesystem::path& root)
    {
        directory.value = CreateFileW(root.c_str(), FILE_LIST_DIRECTORY | FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_OVERLAPPED, nullptr);
        if (!directory || !changed || !GetFileInformationByHandle(directory.value, &identity) ||
            !(identity.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
            (identity.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        overlap.hEvent = changed.value;
        armed = ReadDirectoryChangesW(directory.value, buffer.data(), static_cast<DWORD>(buffer.size()), TRUE,
            FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE |
                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_ATTRIBUTES | FILE_NOTIFY_CHANGE_SECURITY,
            nullptr, &overlap, nullptr) != FALSE;
        return armed;
    }
    bool SameDirectory(const std::filesystem::path& root) const
    {
        Handle current(CreateFileW(root.c_str(), FILE_READ_ATTRIBUTES,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION info{};
        return current && GetFileInformationByHandle(current.value, &info) &&
            !(info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
            info.dwVolumeSerialNumber == identity.dwVolumeSerialNumber &&
            info.nFileIndexHigh == identity.nFileIndexHigh && info.nFileIndexLow == identity.nFileIndexLow;
    }
    std::string ChangedPath()
    {
        DWORD bytes = 0;
        const bool complete = GetOverlappedResult(directory.value, &overlap, &bytes, FALSE) != FALSE;
        if (complete || GetLastError() != ERROR_IO_INCOMPLETE) armed = false;
        if (!complete || bytes < offsetof(FILE_NOTIFY_INFORMATION, FileName)) return {};
        const auto* change = reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data());
        if (change->FileNameLength == 0 || change->FileNameLength % sizeof(wchar_t) != 0 ||
            change->FileNameLength > bytes - offsetof(FILE_NOTIFY_INFORMATION, FileName)) return {};
        const int count = static_cast<int>(change->FileNameLength / sizeof(wchar_t));
        const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            change->FileName, count, nullptr, 0, nullptr, nullptr);
        if (size <= 0 || size > 1024) return {};
        std::string path(static_cast<std::size_t>(size), '\0');
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, change->FileName,
            count, path.data(), size, nullptr, nullptr);
        std::replace(path.begin(), path.end(), '\\', '/');
        if (path.front() == '/' || path.find(':') != std::string::npos ||
            path == ".." || path.starts_with("../") || path.find("/../") != std::string::npos)
            return {};
        return path;
    }
};
bool Identifier(std::string_view value)
{
    return !value.empty() && value.size() <= 64 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        });
}
std::wstring Wide(const std::string& value)
{
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring output(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), output.data(), size);
    return output;
}
struct Pipe
{
    Handle host, child;
    bool Open(bool inbound)
    {
        GUID guid{};
        if (FAILED(CoCreateGuid(&guid))) return false;
        wchar_t guidText[40]{};
        if (!StringFromGUID2(guid, guidText, 40)) return false;
        const std::wstring name = L"\\\\.\\pipe\\SnowDesktop.Native." + std::wstring(guidText);
        host.value = CreateNamedPipeW(name.c_str(),
            (inbound ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) |
                FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
            1, 65536, 65536, 0, nullptr);
        if (!host) return false;
        SECURITY_ATTRIBUTES attributes{ sizeof(attributes), nullptr, TRUE };
        child.value = CreateFileW(name.c_str(), inbound ? GENERIC_WRITE : GENERIC_READ,
            0, &attributes, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (!child) return false;
        OVERLAPPED overlap{};
        Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!event) return false;
        overlap.hEvent = event.value;
        if (ConnectNamedPipe(host.value, &overlap)) return true;
        const auto error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) return true;
        if (error == ERROR_IO_PENDING)
        {
            CancelIoEx(host.value, &overlap);
            DWORD ignored = 0;
            GetOverlappedResult(host.value, &overlap, &ignored, TRUE);
        }
        return false;
    }
};
bool Transfer(HANDLE pipe, bool writing, void* buffer, DWORD count,
    DWORD& transferred, HANDLE cancel, ULONGLONG deadline)
{
    if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0 || GetTickCount64() >= deadline) return false;
    Handle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event) return false;
    OVERLAPPED overlap{};
    overlap.hEvent = event.value;
    const BOOL done = writing ? WriteFile(pipe, buffer, count, &transferred, &overlap)
        : ReadFile(pipe, buffer, count, &transferred, &overlap);
    if (done) return true;
    if (GetLastError() != ERROR_IO_PENDING) return false;
    HANDLE waits[]{ cancel, event.value };
    const auto now = GetTickCount64();
    const DWORD remaining = static_cast<DWORD>(deadline > now ? deadline - now : 0);
    if (WaitForMultipleObjects(2, waits, FALSE, remaining) == WAIT_OBJECT_0 + 1)
        return GetOverlappedResult(pipe, &overlap, &transferred, FALSE) != FALSE;
    CancelIoEx(pipe, &overlap);
    // Keep both the OVERLAPPED and its buffer alive until pipe I/O is drained.
    GetOverlappedResult(pipe, &overlap, &transferred, TRUE);
    return false;
}
bool Send(HANDLE pipe, std::string text, HANDLE cancel, ULONGLONG deadline)
{
    text += '\n';
    std::size_t offset = 0;
    while (offset < text.size())
    {
        DWORD sent = 0;
        if (!Transfer(pipe, true, text.data() + offset,
                static_cast<DWORD>(text.size() - offset), sent, cancel, deadline) || !sent) return false;
        offset += sent;
    }
    return true;
}
bool StringField(const JsonValue& value, const char* key, std::string_view expected)
{
    const auto* field = value.Find(key);
    return field && field->IsString() && field->string == expected;
}
std::string Diagnostic(std::string_view reason, std::string_view phase,
    std::string_view path = {})
{
    // Reason/phase are host constants. The optional path is a bounded relative
    // notification name, never helper data or an absolute user directory.
    std::string output = "{\"reason\":\"" + std::string(reason) +
        "\",\"phase\":\"" + std::string(phase) + "\"";
    if (!path.empty())
    {
        output += ",\"relativePath\":\"";
        for (const unsigned char c : path)
        {
            if (c == '"' || c == '\\') { output += '\\'; output += static_cast<char>(c); }
            else if (c < 0x20) output += '?';
            else output += static_cast<char>(c);
        }
        output += '"';
    }
    return output + '}';
}
bool Frame(const std::string& line, JsonValue& output, std::string& reason)
{
    reason = "invalidEnvelope";
    if (line.empty()) { reason = "emptyFrame"; return false; }
    if (line.size() > MaximumMessage) { reason = "frameTooLarge"; return false; }
    if (Wide(line).empty()) { reason = "invalidUtf8"; return false; }
    if (!ParseJson(line, output)) { reason = "invalidJson"; return false; }
    const auto* version = output.Find("version");
    if (!output.IsObject() || !version || !version->IsNumber() || version->number != 1) return false;
    std::size_t nodes = 0;
    const auto bounded = [&](const auto& self, const JsonValue& value, unsigned depth) -> bool {
        if (depth > 16 || ++nodes > 2048) return false;
        for (const auto& item : value.array) if (!self(self, item, depth + 1)) return false;
        for (const auto& [key, item] : value.object)
            if (key.find('\0') != std::string::npos || !self(self, item, depth + 1)) return false;
        return true;
    };
    if (!bounded(bounded, output, 0)) { reason = "jsonLimit"; return false; }
    return true;
}
}

bool ParseNativeRequestOptions(const std::unordered_map<std::string, std::string>& arguments,
    NativeRequestOptions& options)
{
    options = {};
    for (const auto& [key, value] : arguments)
    {
        if (key == "helper") options.helper = value;
        else if (key == "operation") options.operation = value;
        else if (key == "payload") options.payload = value;
        else if (key == "timeoutMs")
        {
            const auto converted = std::from_chars(value.data(), value.data() + value.size(), options.timeoutMs);
            if (converted.ec != std::errc{} || converted.ptr != value.data() + value.size()) return false;
        }
        else return false;
    }
    JsonValue payload;
    const bool helperId = !options.helper.empty() && options.helper.size() <= 64 &&
        options.helper.front() >= 'a' && options.helper.front() <= 'z' &&
        std::all_of(options.helper.begin(), options.helper.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
    const bool valid = helperId && Identifier(options.operation) &&
        options.timeoutMs >= 1000 && options.timeoutMs <= 60000 &&
        options.payload.size() <= 8192 && !Wide(options.payload).empty() &&
        ParseJson(options.payload, payload) && payload.IsObject();
    if (!valid) return false;
    // JSON Lines framing permits pretty-printed input; compact whitespace
    // outside strings before putting it into a single protocol frame.
    std::string compact;
    bool quoted = false, escaped = false;
    for (const char c : options.payload)
    {
        if (quoted || (c != ' ' && c != '\t' && c != '\r' && c != '\n')) compact += c;
        if (quoted)
        {
            if (escaped) escaped = false;
            else if (c == '\\') escaped = true;
            else if (c == '"') quoted = false;
        }
        else if (c == '"') quoted = true;
    }
    options.payload = std::move(compact);
    return true;
}

struct WidgetNativeTaskExecutor::State
{
    struct Operation
    {
        Handle cancel{ CreateEventW(nullptr, TRUE, FALSE, nullptr) };
        std::thread thread;
        bool done = false;
        ~Operation() { if (thread.joinable()) thread.join(); }
    };
    std::mutex mutex;
    bool stopped = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<Operation>> operations;
    std::vector<NativeTaskMessage> messages;
    std::function<void()> wake;

    bool Post(NativeTaskMessage message)
    {
        std::lock_guard lock(mutex);
        if (stopped) return false;
        if (!message.final && std::count_if(messages.begin(), messages.end(),
                [&](const auto& queued) { return queued.id == message.id && !queued.final; }) >= 64)
            return false;
        if (message.final)
            if (const auto found = operations.find(message.id); found != operations.end()) found->second->done = true;
        messages.push_back(std::move(message));
        // Callback only posts a host wake; serialized with teardown.
        if (wake) wake();
        return true;
    }

    static NativeTaskMessage Run(std::uint64_t id, const std::filesystem::path& root,
        const std::string& fingerprint, const NativeRequestOptions& options, HANDLE cancel,
        const std::shared_ptr<State>& state)
    {
        using namespace snowdesktop::widget;
        NativeTaskMessage result{ id, true, false, "nativeLaunchFailed", {} };
        auto deadline = GetTickCount64() + static_cast<ULONGLONG>(options.timeoutMs);
        DirectoryWatch directoryWatch;
        if (options.streaming && !directoryWatch.Begin(root))
        { result.error = "nativeContentChanged"; return result; }
        PackageManifest manifest;
        const auto validation = WidgetPackageValidator{}.ValidateDirectory(root, &manifest);
        if (!validation.Ok() || manifest.nativeContentFingerprint.empty() ||
            PackagePermissionScopeFingerprint(manifest) != fingerprint)
        {
            result.error = "nativeContentChanged";
            const bool hashMismatch = std::any_of(validation.issues.begin(), validation.issues.end(),
                [](const auto& issue) { return issue.code == "package.nativeHash"; });
            result.diagnostic = Diagnostic(hashMismatch ? "helperHashMismatch" :
                (validation.Ok() ? "authorizationContentChanged" : "packageValidationFailed"), "validation");
            return result;
        }
        const auto helper = std::find_if(manifest.nativeHelpers.begin(), manifest.nativeHelpers.end(),
            [&](const auto& value) { return value.id == options.helper; });
        if (helper == manifest.nativeHelpers.end()) { result.error = "unknownHelper"; return result; }
        if (options.streaming && !directoryWatch.SameDirectory(root))
        { result.error = "nativeContentChanged"; return result; }
        const auto executable = std::filesystem::absolute(root / Wide(helper->path));
        // Pin each ancestor against rename/reparse substitution before opening
        // the image. Locking only the EXE would still allow a parent to move.
        std::vector<std::unique_ptr<Handle>> directories;
        const auto pinDirectory = [&](const std::filesystem::path& path) {
            auto handle = std::make_unique<Handle>(CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
            BY_HANDLE_FILE_INFORMATION info{};
            if (!*handle || !GetFileInformationByHandle(handle->value, &info) ||
                !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ||
                (info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            directories.push_back(std::move(handle));
            return true;
        };
        auto directoryPath = executable.root_path();
        if (!pinDirectory(directoryPath)) { result.error = "nativeContentChanged"; return result; }
        for (const auto& part : executable.relative_path().parent_path())
        {
            directoryPath /= part;
            if (!pinDirectory(directoryPath)) { result.error = "nativeContentChanged"; return result; }
        }
        // Pin the image against modification/deletion through process creation.
        Handle image(CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
        BY_HANDLE_FILE_INFORMATION information{};
        if (!image || !GetFileInformationByHandle(image.value, &information) ||
            (information.dwFileAttributes & (FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_DIRECTORY)) ||
            WidgetPackageManager::Sha256File(executable) != helper->sha256)
        { result.error = "nativeContentChanged"; return result; }
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { result.error = "canceled"; return result; }
        if (GetTickCount64() >= deadline) { result.error = "nativeTimeout"; return result; }
        Pipe input, output, diagnostic;
        if (!input.Open(false) || !output.Open(true) || !diagnostic.Open(true)) return result;
        Handle job(CreateJobObjectW(nullptr, nullptr));
        if (!job) return result;
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
        limits.BasicLimitInformation.ActiveProcessLimit = 8;
        if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits))) return result;
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        startup.StartupInfo.wShowWindow = SW_HIDE;
        startup.StartupInfo.hStdInput = input.child.value;
        startup.StartupInfo.hStdOutput = output.child.value;
        startup.StartupInfo.hStdError = diagnostic.child.value;
        SIZE_T attributeBytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &attributeBytes);
        std::vector<unsigned char> attributes(attributeBytes);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attributeBytes)) return result;
        struct AttributeCleanup { LPPROC_THREAD_ATTRIBUTE_LIST value; ~AttributeCleanup() { DeleteProcThreadAttributeList(value); } } cleanup{ startup.lpAttributeList };
        HANDLE inherited[]{ input.child.value, output.child.value, diagnostic.child.value };
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited, sizeof(inherited), nullptr, nullptr)) return result;
        std::wstring command = L"\"" + executable.wstring() + L"\" --snowdesktop-native-v1";
        PROCESS_INFORMATION processInfo{};
        const auto directory = executable.parent_path().wstring();
        if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
                nullptr, directory.c_str(), &startup.StartupInfo, &processInfo)) return result;
        Handle process(processInfo.hProcess), thread(processInfo.hThread);
        if (!AssignProcessToJobObject(job.value, process.value))
        { TerminateProcess(process.value, 1); WaitForSingleObject(process.value, 1000); return result; }
        struct ProcessCleanup
        {
            HANDLE job, process, input;
            ~ProcessCleanup()
            {
                if (WaitForSingleObject(process, 0) != WAIT_OBJECT_0)
                {
                    Handle stopping(CreateEventW(nullptr, TRUE, FALSE, nullptr));
                    if (stopping) (void)Send(input, R"({"version":1,"kind":"stop"})",
                        stopping.value, GetTickCount64() + 200);
                    WaitForSingleObject(process, 200);
                }
                TerminateJobObject(job, 1);
                WaitForSingleObject(process, 1000);
            }
        } processCleanup{ job.value, process.value, input.host.value };
        for (auto* handle : { &input.child, &output.child, &diagnostic.child })
        { CloseHandle(handle->value); handle->value = nullptr; }
        if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { result.error = "canceled"; return result; }
        if (GetTickCount64() >= deadline) { result.error = "nativeTimeout"; return result; }
        if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) return result;
        if (options.streaming)
        {
            // Launch identity is pinned above; a resident process must allow
            // package replacement. Directory notifications retire it on changes.
            directories.clear();
            CloseHandle(image.value);
            image.value = nullptr;
        }
        result.error = "nativeProtocolError";
        const auto handshakeDeadline = std::min(deadline, GetTickCount64() + 3000);
        if (!Send(input.host.value, R"({"version":1,"kind":"hello","protocol":"snowdesktop-native"})", cancel, handshakeDeadline)) return result;
        std::string pending;
        std::size_t totalBytes = 0, diagnostics = 0, events = 0;
        auto windowStarted = GetTickCount64();
        auto nextIdentityCheck = windowStarted;
        bool hello = false, finished = false;
        while (!finished)
        {
            if (WaitForSingleObject(cancel, 0) == WAIT_OBJECT_0) { result.error = "canceled"; return result; }
            const auto now = GetTickCount64();
            if (options.streaming)
            {
                if (WaitForSingleObject(directoryWatch.changed.value, 0) == WAIT_OBJECT_0)
                {
                    result.error = "nativeContentChanged";
                    result.diagnostic = Diagnostic("packageChangeDetected", "watch", directoryWatch.ChangedPath());
                    return result;
                }
                if (now >= nextIdentityCheck && !directoryWatch.SameDirectory(root))
                {
                    result.error = "nativeContentChanged";
                    result.diagnostic = Diagnostic("packageIdentityChanged", "watch");
                    return result;
                }
                if (now >= nextIdentityCheck) nextIdentityCheck = now + 1000;
                if (now - windowStarted >= 1000)
                { windowStarted = now; totalBytes = diagnostics = events = 0; }
            }
            if (GetTickCount64() >= (hello ? deadline : handshakeDeadline))
            { result.error = "nativeTimeout"; return result; }
            for (int channel = 0; channel < 2; ++channel)
            {
                const HANDLE pipe = channel ? diagnostic.host.value : output.host.value;
                DWORD available = 0;
                if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr))
                {
                    if (!channel) { result.error = "nativeExited"; return result; }
                    continue;
                }
                if (!available) continue;
                std::array<char, 4096> buffer{};
                DWORD received = 0;
                if (!Transfer(pipe, false, buffer.data(), std::min(available, static_cast<DWORD>(buffer.size())),
                        received, cancel, deadline)) return result;
                if (channel)
                {
                    diagnostics += received;
                    if (diagnostics > 16384) { result.error = "nativeOutputLimit"; return result; }
                    continue; // Never expose arbitrary diagnostic output to Lua or logs.
                }
                totalBytes += received;
                if (totalBytes > 262144) { result.error = "nativeOutputLimit"; return result; }
                pending.append(buffer.data(), received);
                std::size_t end = 0;
                while (!finished && (end = pending.find('\n')) != std::string::npos)
                {
                    std::string line = pending.substr(0, end);
                    pending.erase(0, end + 1);
                    if (!line.empty() && line.back() == '\r') line.pop_back();
                    if (line.size() > MaximumMessage)
                    { result.error = "nativeOutputLimit"; result.diagnostic = Diagnostic("frameTooLarge", hello ? "response" : "hello"); return result; }
                    JsonValue frame;
                    std::string reason;
                    if (!Frame(line, frame, reason))
                    { result.diagnostic = Diagnostic(reason, hello ? "response" : "hello"); return result; }
                    if (!hello)
                    {
                        if (!StringField(frame, "kind", "hello") || !StringField(frame, "protocol", "snowdesktop-native"))
                        { result.diagnostic = Diagnostic("unexpectedHello", "hello"); return result; }
                        hello = true;
                        const std::string request = "{\"version\":1,\"kind\":\"request\",\"id\":\"1\",\"operation\":\"" +
                            options.operation + "\",\"payload\":" + options.payload +
                            (options.streaming ? ",\"stream\":true}" : "}");
                        if (!Send(input.host.value, request, cancel, deadline)) return result;
                    }
                    else
                    {
                        if (!StringField(frame, "id", "1"))
                        { result.diagnostic = Diagnostic("unexpectedId", "response"); return result; }
                        if (StringField(frame, "kind", "event") && frame.Find("value"))
                        {
                            if (++events > (options.streaming ? 32u : 16u) ||
                                !state->Post({ id, false, true, {}, std::move(line) }))
                            { result.error = "nativeOutputLimit"; return result; }
                            if (options.streaming) deadline = GetTickCount64() + 30000;
                        }
                        else if (options.streaming && StringField(frame, "kind", "heartbeat"))
                        {
                            if (++events > 32) { result.error = "nativeOutputLimit"; return result; }
                            deadline = GetTickCount64() + 30000;
                        }
                        else if (StringField(frame, "kind", "result") && frame.Find("value"))
                        { result.ok = true; result.error.clear(); result.json = std::move(line); finished = true; }
                        else if (StringField(frame, "kind", "error"))
                        { result.error = "nativeHelperError"; result.json = std::move(line); finished = true; }
                        else { result.diagnostic = Diagnostic("unexpectedKind", "response"); return result; }
                    }
                }
                if (pending.size() > MaximumMessage) { result.error = "nativeOutputLimit"; return result; }
            }
            if (!finished) WaitForSingleObject(cancel, options.streaming ? 50 : 10);
        }
        // Give a cooperative helper a brief shutdown window; the job guarantees
        // cleanup of the request's remaining managed descendants on every exit.
        return result;
    }
};

WidgetNativeTaskExecutor::WidgetNativeTaskExecutor() : state_(std::make_shared<State>()) {}
WidgetNativeTaskExecutor::~WidgetNativeTaskExecutor()
{
    std::unordered_map<std::uint64_t, std::shared_ptr<State::Operation>> operations;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopped = true;
        state_->wake = {};
        operations.swap(state_->operations);
        for (const auto& [id, operation] : operations) { (void)id; SetEvent(operation->cancel.value); }
    }
    // Join outside the state lock; workers must be able to finish teardown.
    for (const auto& [id, operation] : operations) { (void)id; if (operation->thread.joinable()) operation->thread.join(); }
}
void WidgetNativeTaskExecutor::SetCompletionCallback(std::function<void()> callback)
{ std::lock_guard lock(state_->mutex); state_->wake = std::move(callback); }
bool WidgetNativeTaskExecutor::Start(std::uint64_t id, std::filesystem::path root,
    std::string authorizedFingerprint, NativeRequestOptions options)
{
    NativeRequestOptions validated;
    if (!ParseNativeRequestOptions({{"helper", options.helper}, {"operation", options.operation},
            {"payload", options.payload}, {"timeoutMs", std::to_string(options.timeoutMs)}}, validated) ||
        !id || authorizedFingerprint.empty()) return false;
    auto operation = std::make_shared<State::Operation>();
    validated.streaming = options.streaming;
    if (!operation->cancel) return false;
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || state_->operations.size() >= 8 || state_->operations.contains(id)) return false;
    state_->operations.emplace(id, operation);
    try
    {
        operation->thread = std::thread([state = state_, operation, id, root = std::move(root),
            fingerprint = std::move(authorizedFingerprint), options = std::move(validated)] {
            NativeTaskMessage result{ id, true, false, "nativeLaunchFailed", {} };
            try { result = State::Run(id, root, fingerprint, options, operation->cancel.value, state); }
            catch (...) { /* All OS resources are scoped before reporting failure. */ }
            if (WaitForSingleObject(operation->cancel.value, 0) == WAIT_OBJECT_0)
            { result.ok = false; result.error = "canceled"; result.json.clear(); result.diagnostic.clear(); }
            state->Post(std::move(result));
        });
    }
    catch (...) { state_->operations.erase(id); return false; }
    return true;
}
bool WidgetNativeTaskExecutor::Cancel(std::uint64_t id)
{
    std::lock_guard lock(state_->mutex);
    const auto found = state_->operations.find(id);
    return found != state_->operations.end() && SetEvent(found->second->cancel.value) != FALSE;
}
std::vector<NativeTaskMessage> WidgetNativeTaskExecutor::DrainMessages()
{
    std::vector<std::shared_ptr<State::Operation>> finished;
    std::vector<NativeTaskMessage> messages;
    {
        std::lock_guard lock(state_->mutex);
        messages.swap(state_->messages);
        for (auto it = state_->operations.begin(); it != state_->operations.end();)
            if (it->second->done) { finished.push_back(it->second); it = state_->operations.erase(it); }
            else ++it;
    }
    // No worker callback can outlive a drained operation.
    for (const auto& operation : finished) if (operation->thread.joinable()) operation->thread.join();
    return messages;
}
}
