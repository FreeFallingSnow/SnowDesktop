#pragma once

// Explicit opt-in acceptance against an already running host. Default CTest
// still uses its isolated protocol/transport fixtures and never user data.
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>
#include <cwctype>

namespace control_live_settings
{
using namespace snowdesktop::control;
inline void Require(bool value, const char* message)
{ if (!value) throw std::runtime_error(message); }
inline std::wstring Wide(const char* text)
{
    const std::string value(text);
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    Require(count > 0, "Invalid UTF-8 command line path");
    std::wstring output(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), output.data(), count);
    return output;
}
struct Handle
{
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
inline std::string Sha256(const std::wstring& path)
{
    BCRYPT_ALG_HANDLE algorithm{};
    Require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0, "SHA256 provider unavailable");
    BCRYPT_HASH_HANDLE hash{};
    const auto status = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0);
    if (status < 0) { BCryptCloseAlgorithmProvider(algorithm, 0); throw std::runtime_error("Cannot create SHA256 context"); }
    std::string result;
    try
    {
        Handle file{CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        Require(file.value != INVALID_HANDLE_VALUE, "Cannot inspect the running executable");
        unsigned char buffer[65536]; DWORD count = 0;
        while (true)
        {
            Require(ReadFile(file.value, buffer, sizeof(buffer), &count, nullptr) != FALSE, "Cannot hash the running executable");
            if (!count) break;
            Require(BCryptHashData(hash, buffer, count, 0) >= 0, "Executable hash update failed");
        }
        unsigned char digest[32];
        Require(BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0, "Executable hash finalization failed");
        const char* digits = "0123456789abcdef";
        for (const auto byte : digest) { result += digits[byte >> 4]; result += digits[byte & 15]; }
    }
    catch (...) { BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0); throw; }
    BCryptDestroyHash(hash); BCryptCloseAlgorithmProvider(algorithm, 0);
    return result;
}
inline bool Transfer(HANDLE pipe, void* bytes, DWORD size, bool write)
{
    DWORD done = 0;
    const auto deadline = GetTickCount64() + 12000;
    while (done < size)
    {
        Handle event{CreateEventW(nullptr, TRUE, FALSE, nullptr)};
        if (!event.value) return false;
        OVERLAPPED overlapped{}; overlapped.hEvent = event.value;
        DWORD transferred = 0;
        auto* position = static_cast<unsigned char*>(bytes) + done;
        bool completed = (write ? WriteFile(pipe, position, size - done, &transferred, &overlapped) :
            ReadFile(pipe, position, size - done, &transferred, &overlapped)) != FALSE;
        if (!completed && GetLastError() == ERROR_IO_PENDING)
        {
            const auto now = GetTickCount64();
            if (now >= deadline || WaitForSingleObject(event.value, static_cast<DWORD>(deadline - now)) != WAIT_OBJECT_0)
            { CancelIoEx(pipe, &overlapped); GetOverlappedResult(pipe, &overlapped, &transferred, TRUE); return false; }
            completed = GetOverlappedResult(pipe, &overlapped, &transferred, FALSE) != FALSE;
        }
        if (!completed || !transferred) return false;
        done += transferred;
    }
    return true;
}
class Client
{
public:
    std::wstring expectedPath;
    std::string expectedHash;
    DWORD authenticatedPid = 0;
    unsigned serial = 0;
    Client(const char* path, const char* hash) : expectedPath(std::filesystem::absolute(Wide(path)).lexically_normal().wstring()), expectedHash(hash)
    {
        std::transform(expectedHash.begin(), expectedHash.end(), expectedHash.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        Require(expectedHash.size() == 64, "Expected SHA256 must have 64 hexadecimal characters");
    }
    Value Call(std::string_view method, Value params = Object(), bool allowError = false)
    {
        const auto endpoint = EndpointName();
        Handle pipe;
        const auto deadline = GetTickCount64() + 3000;
        do
        {
            pipe.value = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
            if (pipe.value != INVALID_HANDLE_VALUE) break;
            if (GetLastError() != ERROR_PIPE_BUSY) break;
            WaitNamedPipeW(endpoint.c_str(), 25);
        } while (GetTickCount64() < deadline);
        Require(pipe.value != INVALID_HANDLE_VALUE, "Running host control endpoint unavailable");
        ULONG pid = 0;
        Require(GetNamedPipeServerProcessId(pipe.value, &pid) != FALSE, "Cannot authenticate control server PID");
        Handle process{OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)};
        wchar_t image[32768]{}; DWORD length = static_cast<DWORD>(std::size(image));
        Require(process.value && QueryFullProcessImageNameW(process.value, 0, image, &length), "Cannot authenticate host image");
        const auto actual = std::filesystem::path(std::wstring(image, length)).lexically_normal().wstring();
        Require(CompareStringOrdinal(actual.c_str(), -1, expectedPath.c_str(), -1, TRUE) == CSTR_EQUAL, "Unexpected application executable");
        if (!authenticatedPid)
        {
            Require(Sha256(actual) == expectedHash, "Running host SHA256 differs from accepted build");
            authenticatedPid = pid;
            std::cout << "HOST pid=" << pid << " sha256=" << expectedHash << '\n';
        }
        Require(pid == authenticatedPid, "Host identity changed during acceptance");
        const auto id = "live-settings-" + std::to_string(++serial);
        const auto request = Serialize(Object({{"jsonrpc", String("2.0")}, {"id", String(id)},
            {"method", String(std::string(method))}, {"params", std::move(params)}}));
        DWORD size = static_cast<DWORD>(request.size());
        Require(Transfer(pipe.value, &size, sizeof(size), true) &&
            Transfer(pipe.value, const_cast<char*>(request.data()), size, true), "Cannot submit runtime request");
        Require(Transfer(pipe.value, &size, sizeof(size), false) && size <= MaximumFrameBytes, "Invalid runtime response frame");
        std::string response(size, '\0');
        Require(Transfer(pipe.value, response.data(), size, false), "Runtime response did not complete");
        unsigned char acknowledge = 6;
        Require(Transfer(pipe.value, &acknowledge, 1, true), "Cannot acknowledge runtime response");
        Value parsed; Require(ParseJson(response, parsed), "Runtime response is not JSON");
        Require(parsed.Find("id") && parsed.Find("id")->string == id, "Runtime response correlation mismatch");
        if (const auto* error = parsed.Find("error"))
        {
            if (!allowError) throw std::runtime_error("Runtime RPC rejected " + std::string(method) + ": " + Serialize(*error));
            return parsed;
        }
        Require(parsed.Find("result"), "Runtime response has no result");
        return allowError ? parsed : *parsed.Find("result");
    }
};
inline Value Target(const char* kind) { return Object({{"target", Object({{"kind", String(kind)}})}}); }
inline Value ReadJsonFile(const std::filesystem::path& path)
{
    std::ifstream input(path, std::ios::binary);
    Require(input.good(), "Persisted personalization file is unavailable");
    std::stringstream text; text << input.rdbuf(); Value result;
    Require(ParseJson(text.str(), result), "Persisted personalization file is invalid JSON");
    return result;
}
inline const Value& Required(const Value& value, const char* key)
{ const auto* found = value.Find(key); Require(found != nullptr, "Required settings result field missing"); return *found; }
inline std::string ErrorCode(const Value& response)
{ return Required(Required(Required(response, "error"), "data"), "code").string; }
}

inline int LiveControlSettingsChecks(const char* expectedExe, const char* expectedSha)
{
    using namespace control_live_settings;
    Client client(expectedExe, expectedSha);
    bool mustRestore = false, original = false;
    const auto get = [&] { return client.Call("settings.get", Target("personalization")); };
    const auto set = [&](const Value& current, bool value) {
        auto params = Target("personalization"); params.object.emplace("ifRevision", Required(current, "revision"));
        params.object.emplace("changes", Object({{"scrollableTitleBarOnTop", Boolean(value)}}));
        return client.Call("settings.update", std::move(params));
    };
    try
    {
        const auto state = client.Call("app.getState");
        const auto data = std::filesystem::path(Wide(Required(state, "dataDirectory").string.c_str()));
        const auto originalLayout = Required(client.Call("desktop.getLayout"), "layoutRevision").string;
        const auto catalog = client.Call("settings.catalog");
        Require(Required(catalog, "targets").array.size() >= 20, "Direct settings target catalogue is incomplete");
        for (const auto* kind : {"global", "personalization", "dock", "navigation", "general", "category", "desktop",
                "pages", "debug-environments", "fonts", "runtime-services", "packages", "agent-skills", "calendar", "themes", "icon-library"})
        {
            const auto result = client.Call("settings.get", Target(kind), true);
            if (result.Find("error"))
            {
                const auto code = ErrorCode(result);
                Require(code == "settings_unavailable" || code == "feature_unavailable" || code == "not_ready", "Advertised settings target has an unexpected routing failure");
                std::cout << "TARGET " << kind << " unavailable=" << code << '\n';
            }
            else { Require(Required(result, "result").Find("values") != nullptr, "Read target has no named values"); std::cout << "TARGET " << kind << " read=passed\n"; }
        }
        auto before = get(); original = Required(Required(before, "values"), "scrollableTitleBarOnTop").boolean;
        Require(Required(ReadJsonFile(data / L"SnowDesktop.personalization.json"), "scrollableTitleBarOnTop").boolean == original,
            "Initial persisted and host values differ; finish pending edits before acceptance");
        POINT cursorBefore{}, cursorAfter{};
        Require(GetPhysicalCursorPos(&cursorBefore), "Cannot sample system cursor");
        const auto foreground = GetForegroundWindow();
        mustRestore = true;
        const auto updated = set(before, !original);
        Require(Required(updated, "status").string == "applied", "Direct mutation did not apply");
        const auto changed = get();
        Require(Required(Required(changed, "values"), "scrollableTitleBarOnTop").boolean == !original, "Host did not retain requested value");
        Require(Required(ReadJsonFile(data / L"SnowDesktop.personalization.json"), "scrollableTitleBarOnTop").boolean == !original, "Requested setting was not persisted");
        auto stale = Target("personalization"); stale.object.emplace("ifRevision", Required(before, "revision"));
        stale.object.emplace("changes", Object({{"scrollableTitleBarOnTop", Boolean(original)}}));
        Require(ErrorCode(client.Call("settings.update", stale, true)) == "stale_settings", "Stale guard was not enforced by running host");
        auto invalid = Target("personalization"); invalid.object.emplace("ifRevision", Required(changed, "revision"));
        invalid.object.emplace("changes", Object({{"widgetAlpha", Number(-1)}}));
        Require(ErrorCode(client.Call("settings.update", invalid, true)) == "invalid_params", "Invalid alpha was not rejected by running host");
        set(get(), original); mustRestore = false;
        Require(Required(Required(get(), "values"), "scrollableTitleBarOnTop").boolean == original, "Original setting did not restore");
        Require(Required(ReadJsonFile(data / L"SnowDesktop.personalization.json"), "scrollableTitleBarOnTop").boolean == original, "Original persisted setting did not restore");
        Require(GetPhysicalCursorPos(&cursorAfter), "Cannot resample system cursor");
        Require(cursorBefore.x == cursorAfter.x && cursorBefore.y == cursorAfter.y && foreground == GetForegroundWindow(), "Cursor or foreground changed during direct mutation; check concurrent user input");
        Require(Required(client.Call("desktop.getLayout"), "layoutRevision").string == originalLayout, "Direct display setting unexpectedly changed desktop placement");
        std::cout << "LIVE SETTINGS PASSED: catalogue/read, durable reversible direct update, stale/invalid rejection, original value/layout restored, system cursor and foreground unchanged\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "LIVE SETTINGS FAILED: " << error.what() << '\n';
        if (mustRestore)
        {
            try { set(get(), original); std::cerr << "RECOVERY: original display setting restored\n"; }
            catch (const std::exception& restore) { std::cerr << "RECOVERY FAILED: " << restore.what() << '\n'; }
        }
        return 1;
    }
}
