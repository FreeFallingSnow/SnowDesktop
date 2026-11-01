#include "file_preview.h"
#include "common/json_value.h"

#include <sddl.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <fstream>
#include <mutex>
#include <thread>
#include <utility>

namespace snowdesktop::file_preview
{
namespace
{
using Clock = std::chrono::steady_clock;
constexpr auto refreshInterval = std::chrono::seconds(2);

struct Handle
{
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

std::string ReadSettings(const std::filesystem::path& path)
{
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) return {};
    const auto size = stream.tellg();
    if (size <= 0 || size > 1024 * 1024) return {};
    std::string text(static_cast<size_t>(size), '\0');
    stream.seekg(0);
    if (!stream.read(text.data(), static_cast<std::streamsize>(text.size()))) return {};
    return text;
}

std::filesystem::path KnownFolder(REFKNOWNFOLDERID folder)
{
    PWSTR path = nullptr;
    if (FAILED(SHGetKnownFolderPath(folder, KF_FLAG_DONT_VERIFY, nullptr, &path))) return {};
    const std::filesystem::path result(path);
    CoTaskMemFree(path);
    return result;
}

std::wstring CurrentUserPipe()
{
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value)) return {};
    DWORD bytes = 0;
    GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
    if (!bytes) return {};
    std::vector<BYTE> storage(bytes);
    if (!GetTokenInformation(token.value, TokenUser, storage.data(), bytes, &bytes)) return {};
    LPWSTR sid = nullptr;
    if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(storage.data())->User.Sid, &sid)) return {};
    const std::wstring result = L"\\\\.\\pipe\\QuickLook.App.Pipe." + std::wstring(sid);
    LocalFree(sid);
    return result;
}

std::filesystem::path ProcessPath(DWORD pid)
{
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
    if (!process.value) return {};
    std::wstring path(32768, L'\0');
    DWORD size = static_cast<DWORD>(path.size());
    if (!QueryFullProcessImageNameW(process.value, 0, path.data(), &size)) return {};
    path.resize(size);
    return path;
}

PeekInstallation ReadInstallation(const std::filesystem::path& path)
{
    PeekInstallation installation{path};
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), nullptr);
    if (!size || size > 1024 * 1024) return installation;
    std::vector<BYTE> data(size);
    if (!GetFileVersionInfoW(path.c_str(), 0, size, data.data())) return installation;
    VS_FIXEDFILEINFO* info = nullptr;
    UINT infoSize = 0;
    if (VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &infoSize) &&
        info && infoSize >= sizeof(*info))
    {
        installation.major = HIWORD(info->dwFileVersionMS);
        installation.minor = LOWORD(info->dwFileVersionMS);
    }
    return installation;
}

bool ReadBoolean(const JsonValue* parent, const char* name, bool& value)
{
    const auto* field = parent ? parent->Find(name) : nullptr;
    if (!field || !field->IsBoolean()) return false;
    value = field->boolean;
    return true;
}

std::wstring QuoteArgument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    size_t slashes = 0;
    for (const wchar_t ch : argument)
    {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(slashes * (ch == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (ch == L'\"') result.push_back(L'\\');
        result.push_back(ch);
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

bool SendQuickLook(const Provider& provider, const std::wstring& path,
    const PublishSession& publishSession)
{
    if (!WaitNamedPipeW(provider.endpoint.c_str(), 75)) return false;
    Handle pipe{CreateFileW(provider.endpoint.c_str(), GENERIC_WRITE, 0, nullptr,
        OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr)};
    if (pipe.value == INVALID_HANDLE_VALUE) return false;

    // Querying availability must never connect and disconnect an empty client:
    // QuickLook expects a full UTF-8 line on every accepted connection.
    const std::wstring line = L"QuickLook.App.PipeMessages.Toggle|" + path + L"|\n";
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
        static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return false;
    std::string bytes(static_cast<size_t>(count), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, line.data(),
            static_cast<int>(line.size()), bytes.data(), count, nullptr, nullptr)) return false;

    ULONG server = 0;
    if (GetNamedPipeServerProcessId(pipe.value, &server))
    {
        publishSession(IdentifyProcess(server));
        AllowSetForegroundWindow(server);
    }
    Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
    if (!event.value) return false;
    OVERLAPPED operation{};
    operation.hEvent = event.value;
    DWORD written = 0;
    if (!WriteFile(pipe.value, bytes.data(), static_cast<DWORD>(bytes.size()), &written, &operation))
    {
        if (GetLastError() != ERROR_IO_PENDING) return false;
        if (WaitForSingleObject(event.value, 200) != WAIT_OBJECT_0)
        {
            CancelIoEx(pipe.value, &operation);
            // Keep the buffer/OVERLAPPED alive until cancellation is delivered.
            GetOverlappedResult(pipe.value, &operation, &written, TRUE);
            return false;
        }
        if (!GetOverlappedResult(pipe.value, &operation, &written, FALSE)) return false;
    }
    return written == bytes.size();
}
}

bool AllowsPreview(const InputContext& context)
{
    return !context.textInput && !context.overlay && !context.gesture;
}

bool IsFileSystemPath(std::wstring_view path)
{
    if (path.empty() || path.size() > 32700 ||
        path.find_first_of(L"\r\n|\"") != std::wstring_view::npos ||
        path.find(L'\0') != std::wstring_view::npos) return false;
    return (path.size() >= 3 && ((path[0] >= L'A' && path[0] <= L'Z') ||
            (path[0] >= L'a' && path[0] <= L'z')) && path[1] == L':' &&
            (path[2] == L'\\' || path[2] == L'/')) ||
        (path.size() > 3 && path[0] == L'\\' && path[1] == L'\\');
}

std::optional<Shortcut> ParsePeekShortcut(
    std::string_view generalSettings, std::string_view peekSettings)
{
    JsonValue general, peek;
    if (!ParseJson(generalSettings, general) || !ParseJson(peekSettings, peek)) return {};
    bool enabled = false;
    if (!ReadBoolean(general.Find("enabled"), "Peek", enabled) || !enabled) return {};
    const auto* properties = peek.Find("properties");
    if (!properties) return {};
    bool space = false;
    ReadBoolean(properties->Find("EnableSpaceToActivate"), "value", space);
    if (space) return Shortcut{};
    const auto* key = properties->Find("ActivationShortcut");
    if (key && key->Find("value")) key = key->Find("value");
    const auto* code = key ? key->Find("code") : nullptr;
    if (!code || !code->IsNumber() || code->number < 0 || code->number > 254 ||
        std::floor(code->number) != code->number) return {};
    Shortcut shortcut{static_cast<UINT>(code->number), 0};
    for (const auto& [name, modifier] : {std::pair{"ctrl", MOD_CONTROL},
            {"alt", MOD_ALT}, {"shift", MOD_SHIFT}, {"win", MOD_WIN}})
    {
        bool pressed = false;
        if (!ReadBoolean(key, name, pressed)) return {};
        if (pressed) shortcut.modifiers |= modifier;
    }
    // Mirror Peek's fallback when Space mode is disabled or its key is unset.
    if (!shortcut.key || (shortcut.key == VK_SPACE && !shortcut.modifiers))
        return Shortcut{VK_SPACE, MOD_CONTROL};
    if (shortcut.key == VK_CONTROL || shortcut.key == VK_MENU || shortcut.key == VK_SHIFT ||
        shortcut.key == VK_LWIN || shortcut.key == VK_RWIN) return {};
    return shortcut;
}

std::optional<Provider> MatchProvider(const Availability& availability, Shortcut pressed)
{
    // QuickLook wins a shared Space binding. Other Peek combinations still work.
    for (const Kind kind : {Kind::QuickLook, Kind::Peek})
        for (const auto& provider : availability.providers)
            if (provider.kind == kind && provider.shortcut == pressed && !provider.endpoint.empty())
                return provider;
    return {};
}

Environment ReadEnvironment()
{
    Environment result;
    result.quickLookPipe = CurrentUserPipe();
    const auto local = KnownFolder(FOLDERID_LocalAppData);
    if (!local.empty()) result.powerToysSettings = local / L"Microsoft/PowerToys";
    std::vector<std::filesystem::path> executables;
    Handle processes{CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)};
    DWORD session = 0;
    const bool sessionKnown = ProcessIdToSessionId(GetCurrentProcessId(), &session) != FALSE;
    PROCESSENTRY32W process{};
    process.dwSize = sizeof(process);
    if (processes.value != INVALID_HANDLE_VALUE && Process32FirstW(processes.value, &process))
    {
        do
        {
            DWORD candidateSession = 0;
            if (!sessionKnown || !ProcessIdToSessionId(process.th32ProcessID, &candidateSession) ||
                candidateSession != session) continue;
            const bool runner = _wcsicmp(process.szExeFile, L"PowerToys.exe") == 0;
            const bool viewer = _wcsicmp(process.szExeFile, L"PowerToys.Peek.UI.exe") == 0;
            if (!runner && !viewer) continue;
            if (runner) result.powerToysRunning = true;
            const auto path = ProcessPath(process.th32ProcessID);
            if (!path.empty()) executables.push_back(viewer ? path :
                path.parent_path() / L"WinUI3Apps/PowerToys.Peek.UI.exe");
        } while (Process32NextW(processes.value, &process));
    }
    if (!result.powerToysRunning) return result;
    if (!local.empty()) executables.push_back(local / L"PowerToys/WinUI3Apps/PowerToys.Peek.UI.exe");
    const auto programFiles = KnownFolder(FOLDERID_ProgramFiles);
    if (!programFiles.empty()) executables.push_back(programFiles / L"PowerToys/WinUI3Apps/PowerToys.Peek.UI.exe");
    for (const auto& executable : executables)
    {
        if (std::any_of(result.peekInstallations.begin(), result.peekInstallations.end(),
                [&](const auto& item) { return item.executable == executable; })) continue;
        auto installation = ReadInstallation(executable);
        if (installation.major || installation.minor >= 96)
            result.peekInstallations.push_back(std::move(installation));
    }
    return result;
}

Availability Probe(const Environment& environment)
{
    Availability result;
    if (!environment.quickLookPipe.empty() &&
        WaitNamedPipeW(environment.quickLookPipe.c_str(), 1))
        result.providers.push_back({Kind::QuickLook, {}, environment.quickLookPipe});
    if (!environment.powerToysRunning || environment.powerToysSettings.empty()) return result;
    const auto shortcut = ParsePeekShortcut(
        ReadSettings(environment.powerToysSettings / L"settings.json"),
        ReadSettings(environment.powerToysSettings / L"Peek/settings.json"));
    if (!shortcut) return result;
    for (const auto& installation : environment.peekInstallations)
    {
        if (!installation.major && installation.minor < 96) continue;
        const DWORD attributes = GetFileAttributesW(installation.executable.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        result.providers.push_back({Kind::Peek, *shortcut, installation.executable.wstring()});
        break;
    }
    return result;
}

ProcessIdentity IdentifyProcess(DWORD pid)
{
    Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, pid)};
    FILETIME created{}, exited{}, kernel{}, user{};
    if (!process.value || WaitForSingleObject(process.value, 0) != WAIT_TIMEOUT ||
        !GetProcessTimes(process.value, &created, &exited, &kernel, &user)) return {};
    return {pid, (std::uint64_t{created.dwHighDateTime} << 32) | created.dwLowDateTime};
}

bool Invoke(const Provider& provider, const std::wstring& path,
    const PublishSession& publishSession)
{
    if (!IsFileSystemPath(path) || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return false;
    if (provider.kind == Kind::QuickLook) return SendQuickLook(provider, path, publishSession);
    if (!IsFileSystemPath(provider.endpoint)) return false;
    std::wstring command = QuoteArgument(provider.endpoint) + L" " + QuoteArgument(path);
    if (command.size() >= 32767) return false;
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_SHOWNORMAL;
    PROCESS_INFORMATION info{};
    // Publish the exact process identity before it can activate its first window.
    if (!CreateProcessW(provider.endpoint.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info)) return false;
    Handle process{info.hProcess}, thread{info.hThread};
    publishSession(IdentifyProcess(info.dwProcessId));
    AllowSetForegroundWindow(info.dwProcessId);
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1))
    {
        TerminateProcess(process.value, ERROR_CANCELLED);
        return false;
    }
    return true;
}

struct Service::State
{
    explicit State(Backend services) : backend(std::move(services)) {}
    struct Request { Shortcut shortcut; std::wstring path; std::uint64_t serial; };
    std::mutex mutex;
    std::condition_variable changed;
    Backend backend;
    Availability availability;
    Clock::time_point refreshed{};
    std::optional<Request> pending;
    ProcessIdentity session;
    std::uint64_t serial = 0;
    bool started = false, stopped = false, refresh = true;
};

Service::Service() : Service(Backend{
    [] { return Probe(ReadEnvironment()); }, &Invoke}) {}
Service::Service(Backend backend) : state_(std::make_shared<State>(std::move(backend))) {}
Service::~Service() { Stop(); }
Service::Service(Service&& other) noexcept : state_(std::move(other.state_)) {}
Service& Service::operator=(Service&& other) noexcept
{
    if (this != &other) { Stop(); state_ = std::move(other.state_); }
    return *this;
}

void Service::Start()
{
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    if (state_->started || state_->stopped) return;
    try { std::thread([state = state_] { Run(state); }).detach(); state_->started = true; }
    catch (...) { state_->stopped = true; }
}

void Service::Stop()
{
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    state_->stopped = true;
    state_->availability = {};
    state_->pending.reset();
    state_->session = {};
    state_->changed.notify_all();
}

void Service::Refresh()
{
    if (!state_) return;
    std::lock_guard lock(state_->mutex);
    state_->refresh = true;
    state_->changed.notify_all();
}

Availability Service::Current() const
{
    if (!state_) return {};
    std::lock_guard lock(state_->mutex);
    if (state_->stopped || Clock::now() - state_->refreshed > refreshInterval * 3) return {};
    return state_->availability;
}

bool Service::Matches(Shortcut pressed) const { return MatchProvider(Current(), pressed).has_value(); }

bool Service::Request(Shortcut pressed, std::wstring path, bool repeated)
{
    if (!state_ || !IsFileSystemPath(path)) return false;
    std::lock_guard lock(state_->mutex);
    if (!state_->started || state_->stopped ||
        Clock::now() - state_->refreshed > refreshInterval * 3 ||
        !MatchProvider(state_->availability, pressed)) return false;
    if (repeated) return true;
    state_->pending = State::Request{pressed, std::move(path), ++state_->serial};
    state_->session = {};
    state_->changed.notify_all();
    return true;
}

bool Service::RetainsSelection(HWND window)
{
    if (!state_) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    const auto identity = pid ? IdentifyProcess(pid) : ProcessIdentity{};
    std::lock_guard lock(state_->mutex);
    if (!state_->stopped && state_->session.pid && state_->session == identity) return true;
    state_->session = {};
    state_->pending.reset();
    ++state_->serial;
    return false;
}

void Service::Run(const std::shared_ptr<State>& state)
{
    auto nextRefresh = Clock::now();
    for (;;)
    {
        std::optional<State::Request> request;
        {
            std::unique_lock lock(state->mutex);
            state->changed.wait_until(lock, nextRefresh,
                [&] { return state->stopped || state->refresh || state->pending.has_value(); });
            if (state->stopped) return;
            request = std::move(state->pending);
            state->pending.reset();
            state->refresh = false;
        }
        Availability availability;
        try { if (state->backend.detect) availability = state->backend.detect(); }
        catch (...) { /* An unavailable provider never blocks host input. */ }
        {
            std::lock_guard lock(state->mutex);
            if (state->stopped) return;
            state->availability = availability;
            state->refreshed = Clock::now();
            nextRefresh = state->refreshed + refreshInterval;
            if (!request || request->serial != state->serial) continue;
        }
        // Re-read availability at execution time: a disabled tool or changed
        // shortcut must not be invoked using an obsolete UI cache.
        bool dispatched = false;
        if (const auto provider = MatchProvider(availability, request->shortcut))
        {
            try
            {
                if (state->backend.invoke) dispatched = state->backend.invoke(*provider, request->path,
                    [state, serial = request->serial](ProcessIdentity process) {
                        std::lock_guard lock(state->mutex);
                        if (!state->stopped && state->serial == serial) state->session = process;
                    });
            }
            catch (...) { /* Keep provider failures outside the message loop. */ }
        }
        if (!dispatched)
        {
            std::lock_guard lock(state->mutex);
            if (state->serial == request->serial) state->session = {};
        }
    }
}
}
