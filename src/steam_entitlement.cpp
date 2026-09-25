#include "steam_entitlement.h"

#include "atomic_file.h"
#include "diagnostic_log.h"
#include "json_value.h"
#include "steam_app_identity.h"
#include "steam_child_environment.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <limits>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

namespace snowdesktop::steam_entitlement
{
namespace
{
constexpr std::size_t kMaximumBridgeOutputBytes = 64 * 1024;
constexpr DWORD kCanceledExitCode = 0x53444501;
constexpr DWORD kTimedOutExitCode = 0x53444502;
constexpr std::array<unsigned char, 4> kCacheMagic{'S', 'D', 'E', '1'};
constexpr std::uint32_t kCacheSchema = 1;
constexpr std::string_view kDpapiEntropy =
    "SnowDesktop Steam entitlement cache v1 AppID 5080330";

std::atomic_uint64_t nextTraceId{0};

std::wstring DiagnosticText(std::string_view text)
{
    // Keep SDK diagnostics on one bounded line and redact account identifiers
    // even when Steam includes them in a human-readable error message.
    std::string safe;
    for (std::size_t index = 0; index < text.size() && safe.size() < 4096;)
    {
        std::size_t end = index;
        while (end < text.size() && text[end] >= '0' && text[end] <= '9') ++end;
        if (end - index >= 17)
        {
            safe += "[redacted-id]";
            index = end;
        }
        else
        {
            const unsigned char character = static_cast<unsigned char>(text[index++]);
            safe.push_back(character < 32 || character == 127 ? ' ' :
                static_cast<char>(character));
        }
    }
    const int length = MultiByteToWideChar(CP_UTF8, 0, safe.data(),
        static_cast<int>(safe.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    if (length > 0) MultiByteToWideChar(CP_UTF8, 0, safe.data(),
        static_cast<int>(safe.size()), wide.data(), length);
    return wide;
}

void Trace(std::uint64_t id, std::wstring_view message, bool failed = false)
{
    const std::wstring line = L"[SteamActivation] id=" + std::to_wstring(id) +
        L" " + std::wstring(message);
    WriteDiagnosticLogEntry(line.c_str(), failed
        ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Info);
}

class UniqueHandle final
{
public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE value) : value_(value) {}
    ~UniqueHandle()
    {
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept
        : value_(std::exchange(other.value_, nullptr)) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this == &other) return *this;
        if (value_ && value_ != INVALID_HANDLE_VALUE)
            CloseHandle(value_);
        value_ = std::exchange(other.value_, nullptr);
        return *this;
    }
    [[nodiscard]] HANDLE Get() const noexcept { return value_; }
    void Reset() noexcept
    {
        UniqueHandle empty;
        *this = std::move(empty);
    }

private:
    HANDLE value_ = nullptr;
};

bool IsSafeRegularFile(const std::filesystem::path& path)
{
    std::error_code error;
    if (!std::filesystem::is_regular_file(path, error) || error)
        return false;
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

std::wstring Quote(std::wstring_view value)
{
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : value)
    {
        if (character == L'\\')
        {
            ++backslashes;
            continue;
        }
        if (character == L'\"')
        {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
            continue;
        }
        result.append(backslashes, L'\\');
        backslashes = 0;
        result.push_back(character);
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

const JsonValue* Field(const JsonValue& object, std::string_view name,
    JsonValue::Type type)
{
    const JsonValue* value = object.Find(name);
    return value && value->type == type ? value : nullptr;
}

std::string ErrorCode(const JsonValue& root)
{
    const JsonValue* error = Field(root, "error", JsonValue::Type::Object);
    if (!error) return {};
    const JsonValue* code = Field(*error, "code", JsonValue::Type::String);
    return code ? code->string : std::string{};
}

bool IsSteamUnavailableCode(std::string_view code)
{
    return code == "steam_init_failed" ||
        code == "steam_not_logged_on" ||
        code == "steam_interface_unavailable";
}

bool ParseSteamId(std::string_view value, std::uint64_t& output)
{
    if (value.empty()) return false;
    const auto parsed = std::from_chars(
        value.data(), value.data() + value.size(), output);
    return parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
        output != 0;
}

std::optional<JsonValue> FinalJsonObject(std::string_view output)
{
    std::size_t end = output.size();
    while (end > 0)
    {
        while (end > 0 &&
            (output[end - 1] == '\r' || output[end - 1] == '\n'))
            --end;
        const std::size_t newline = end == 0
            ? std::string_view::npos : output.rfind('\n', end - 1);
        const std::size_t begin = newline == std::string_view::npos
            ? 0 : newline + 1;
        JsonValue value;
        std::string parseError;
        if (begin < end && ParseJson(output.substr(begin, end - begin),
                value, &parseError) && value.IsObject())
            return value;
        if (begin == 0) break;
        end = begin - 1;
    }
    return std::nullopt;
}

std::optional<std::uint32_t> Uint32Field(
    const JsonValue& object, std::string_view name)
{
    const JsonValue* value = Field(object, name, JsonValue::Type::Number);
    if (!value || !std::isfinite(value->number) || value->number < 0.0 ||
        std::floor(value->number) != value->number ||
        value->number > static_cast<double>(
            std::numeric_limits<std::uint32_t>::max()))
        return std::nullopt;
    return static_cast<std::uint32_t>(value->number);
}

struct BridgeCommandResult
{
    std::uint32_t exitCode = std::numeric_limits<std::uint32_t>::max();
    std::string output;
};

std::optional<BridgeCommandResult> RunBridgeCommand(
    const std::filesystem::path& executable, std::wstring_view arguments,
    std::stop_token stop, std::chrono::seconds timeout, std::uint64_t traceId = 0)
{
    if (traceId == 0) traceId = ++nextTraceId;
    const auto started = std::chrono::steady_clock::now();
    const auto failure = [traceId](const wchar_t* stage, DWORD error) {
        Trace(traceId, std::wstring(L"bridge failed stage=") + stage +
            L" win32_error=" + std::to_wstring(error), true);
        return std::nullopt;
    };
    Trace(traceId, L"bridge begin command=" + std::wstring(arguments) +
        L" path=" + executable.wstring() + L" timeout_s=" +
        std::to_wstring(timeout.count()));
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &security, 0))
        return failure(L"CreatePipe", GetLastError());
    UniqueHandle readPipe(readRaw);
    UniqueHandle writePipe(writeRaw);
    if (!SetHandleInformation(readPipe.Get(), HANDLE_FLAG_INHERIT, 0))
        return failure(L"SetHandleInformation", GetLastError());

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe.Get();
    startup.hStdError = writePipe.Get();
    PROCESS_INFORMATION process{};
    std::wstring commandLine = Quote(executable.wstring()) +
        L" " + std::wstring(arguments);
    const std::wstring workingDirectory = executable.parent_path().wstring();
    std::vector<wchar_t> environment =
        BuildSnowDesktopSteamChildEnvironment();
    if (environment.empty()) return failure(L"environment", GetLastError());
    if (!CreateProcessW(executable.c_str(),
            commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            environment.data(), workingDirectory.c_str(), &startup, &process))
        return failure(L"CreateProcessW", GetLastError());

    Trace(traceId, L"bridge started pid=" + std::to_wstring(process.dwProcessId));

    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    writePipe.Reset();
    BridgeCommandResult result;
    const auto deadline = std::chrono::steady_clock::now() +
        timeout;
    while (true)
    {
        // Observe process completion before inspecting its final output. A
        // child can write and exit between a pipe peek and a process wait.
        const DWORD processState = WaitForSingleObject(processHandle.Get(), 0);
        if (processState == WAIT_FAILED)
            return failure(L"WaitForSingleObject", GetLastError());
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe.Get(), nullptr, 0, nullptr,
                &available, nullptr))
        {
            // Closing stdout can precede process termination. Retain the
            // bytes already read and wait for the final exit code below.
            if (GetLastError() != ERROR_BROKEN_PIPE)
                return failure(L"PeekNamedPipe", GetLastError());
        }
        if (available > 0)
        {
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            if (!ReadFile(readPipe.Get(), buffer.data(),
                    (std::min)(available,
                        static_cast<DWORD>(buffer.size())), &read, nullptr))
                return failure(L"ReadFile", GetLastError());
            if (result.output.size() + read > kMaximumBridgeOutputBytes)
            {
                TerminateProcess(processHandle.Get(), kTimedOutExitCode);
                WaitForSingleObject(processHandle.Get(), 5000);
                return failure(L"output_limit", ERROR_BUFFER_OVERFLOW);
            }
            result.output.append(buffer.data(), read);
            continue;
        }
        if (processState == WAIT_OBJECT_0)
            break;
        if (stop.stop_requested())
        {
            TerminateProcess(processHandle.Get(), kCanceledExitCode);
            WaitForSingleObject(processHandle.Get(), 5000);
            return failure(L"canceled", ERROR_CANCELLED);
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            TerminateProcess(processHandle.Get(), kTimedOutExitCode);
            WaitForSingleObject(processHandle.Get(), 5000);
            return failure(L"timeout", WAIT_TIMEOUT);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle.Get(), &exitCode))
        return failure(L"GetExitCodeProcess", GetLastError());
    result.exitCode = exitCode;
    Trace(traceId, L"bridge finished exit_code=" + std::to_wstring(exitCode) +
        L" output_bytes=" + std::to_wstring(result.output.size()) +
        L" elapsed_ms=" + std::to_wstring(std::chrono::duration_cast<
            std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()),
        exitCode != 0);
    return result;
}

BridgeResponse RunBridge(const std::filesystem::path& executable,
    std::stop_token stop, std::uint64_t traceId)
{
    const auto result = RunBridgeCommand(executable, L"entitlement status",
        stop, std::chrono::seconds(15), traceId);
    auto response = result
        ? ParseBridgeResponse(result->output, result->exitCode)
        : BridgeResponse{};
    if (result)
    {
        const auto root = FinalJsonObject(result->output);
        if (root)
        {
            const auto appId = Uint32Field(*root, "appId");
            const auto boolean = [&root](const char* key) {
                const auto value = Field(*root, key, JsonValue::Type::Boolean);
                return value ? (value->boolean ? L"true" : L"false") : L"absent";
            };
            Trace(traceId, L"ownership fields app_id=" +
                (appId ? std::to_wstring(*appId) : L"absent") +
                L" logged_on=" + boolean("loggedOn") +
                L" owned=" + boolean("owned"));
        }
    }
    const wchar_t* outcome = L"failed";
    switch (response.outcome)
    {
    case BridgeOutcome::Owned: outcome = L"owned"; break;
    case BridgeOutcome::NotOwned: outcome = L"not_owned"; break;
    case BridgeOutcome::SteamUnavailable: outcome = L"steam_unavailable"; break;
    case BridgeOutcome::Failed: break;
    }
    Trace(traceId, std::wstring(L"ownership response outcome=") + outcome +
        L" connection_problem=" +
        std::to_wstring(static_cast<int>(response.connectionProblem)) +
        L" steam_init_result=" + (response.steamInitResult
            ? std::to_wstring(*response.steamInitResult) : L"absent") +
        L" detail=" + DiagnosticText(response.errorDetail),
        response.outcome != BridgeOutcome::Owned);
    return response;
}

template<typename Integer>
void AppendLittleEndian(std::string& output, Integer value)
{
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned encoded = static_cast<Unsigned>(value);
    for (std::size_t index = 0; index < sizeof(Integer); ++index)
    {
        output.push_back(static_cast<char>(encoded & 0xff));
        encoded >>= 8;
    }
}

template<typename Integer>
bool ReadLittleEndian(std::string_view input, std::size_t& offset,
    Integer& output)
{
    if (offset + sizeof(Integer) > input.size()) return false;
    using Unsigned = std::make_unsigned_t<Integer>;
    Unsigned decoded = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index)
    {
        decoded |= static_cast<Unsigned>(
            static_cast<unsigned char>(input[offset + index])) <<
            (index * 8);
    }
    offset += sizeof(Integer);
    output = static_cast<Integer>(decoded);
    return true;
}

struct CacheRecord
{
    std::uint64_t steamId = 0;
    std::int64_t verifiedAt = 0;
    std::int64_t validUntil = 0;
};

std::string Serialize(const CacheRecord& record)
{
    std::string bytes(kCacheMagic.begin(), kCacheMagic.end());
    AppendLittleEndian(bytes, kCacheSchema);
    AppendLittleEndian(bytes,
        static_cast<std::uint32_t>(kSnowDesktopSteamAppId));
    AppendLittleEndian(bytes, record.steamId);
    AppendLittleEndian(bytes, record.verifiedAt);
    AppendLittleEndian(bytes, record.validUntil);
    return bytes;
}

std::optional<CacheRecord> Deserialize(std::string_view bytes)
{
    if (bytes.size() < kCacheMagic.size() || !std::equal(
            kCacheMagic.begin(), kCacheMagic.end(), bytes.begin()))
        return std::nullopt;
    std::size_t offset = kCacheMagic.size();
    std::uint32_t schema = 0;
    std::uint32_t appId = 0;
    CacheRecord record;
    if (!ReadLittleEndian(bytes, offset, schema) ||
        !ReadLittleEndian(bytes, offset, appId) ||
        !ReadLittleEndian(bytes, offset, record.steamId) ||
        !ReadLittleEndian(bytes, offset, record.verifiedAt) ||
        !ReadLittleEndian(bytes, offset, record.validUntil) ||
        offset != bytes.size() || schema != kCacheSchema ||
        appId != kSnowDesktopSteamAppId || record.steamId == 0 ||
        record.verifiedAt <= 0 || record.validUntil <= record.verifiedAt)
        return std::nullopt;
    return record;
}

std::optional<CacheRecord> UnprotectCache(
    const std::filesystem::path& path, std::uint64_t traceId)
{
    std::string encrypted;
    std::string readError;
    if (!atomic_file::ReadAll(path, encrypted, &readError) || encrypted.empty())
    {
        Trace(traceId, L"cache read unavailable path=" + path.wstring() +
            L" detail=" + DiagnosticText(readError));
        return std::nullopt;
    }
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
        reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB entropy{static_cast<DWORD>(kDpapiEntropy.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(kDpapiEntropy.data()))};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &plain))
    {
        Trace(traceId, L"cache decrypt failed win32_error=" +
            std::to_wstring(GetLastError()), true);
        return std::nullopt;
    }
    const std::string_view decoded(
        reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    const auto result = Deserialize(decoded);
    LocalFree(plain.pbData);
    Trace(traceId, L"cache schema valid=" + std::to_wstring(result.has_value()), !result);
    return result;
}

bool ProtectCache(const std::filesystem::path& path,
    const CacheRecord& record, std::uint64_t traceId)
{
    std::string plain = Serialize(record);
    DATA_BLOB input{static_cast<DWORD>(plain.size()),
        reinterpret_cast<BYTE*>(plain.data())};
    DATA_BLOB entropy{static_cast<DWORD>(kDpapiEntropy.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(kDpapiEntropy.data()))};
    DATA_BLOB encrypted{};
    if (!CryptProtectData(&input, L"SnowDesktop Steam entitlement",
            &entropy, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
            &encrypted))
    {
        Trace(traceId, L"cache protect failed win32_error=" +
            std::to_wstring(GetLastError()), true);
        return false;
    }
    const std::string_view bytes(
        reinterpret_cast<const char*>(encrypted.pbData), encrypted.cbData);
    std::string error;
    const bool saved = atomic_file::WriteAll(path, bytes, {}, &error);
    Trace(traceId, L"cache save success=" + std::to_wstring(saved) +
        L" path=" + path.wstring() + L" detail=" + DiagnosticText(error), !saved);
    LocalFree(encrypted.pbData);
    return saved;
}

std::int64_t UnixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

SteamBridgeConfiguration ParseSteamBridgeConfiguration(
    std::string_view output, std::uint32_t exitCode)
{
    SteamBridgeConfiguration configuration;
    if (exitCode != 0) return configuration;
    const auto root = FinalJsonObject(output);
    if (!root) return configuration;

    const JsonValue* ok = Field(*root, "ok", JsonValue::Type::Boolean);
    const JsonValue* version = Field(
        *root, "version", JsonValue::Type::String);
    const JsonValue* steamworksCompiled = Field(
        *root, "steamworksCompiled", JsonValue::Type::Boolean);
    const auto protocolVersion = Uint32Field(*root, "protocolVersion");
    const auto expectedAppId = Uint32Field(*root, "expectedAppId");
    if (!ok || !ok->boolean || !version || version->string.empty() ||
        !steamworksCompiled || !protocolVersion || !expectedAppId)
        return configuration;

    configuration.valid = true;
    configuration.version = version->string;
    configuration.protocolVersion = *protocolVersion;
    configuration.expectedAppId = *expectedAppId;
    configuration.steamworksCompiled = steamworksCompiled->boolean;
    return configuration;
}

bool IsSteamBridgeConfigurationCompatible(
    const SteamBridgeConfiguration& configuration,
    std::string_view expectedVersion) noexcept
{
    return configuration.valid &&
        configuration.version == expectedVersion &&
        configuration.protocolVersion == kSupportedBridgeProtocolVersion &&
        configuration.expectedAppId == kSnowDesktopSteamAppId &&
        configuration.steamworksCompiled;
}

bool IsSteamBridgeExecutableCompatible(
    const std::filesystem::path& executable,
    std::string_view expectedVersion)
{
    const auto traceId = ++nextTraceId;
    if (!IsSafeRegularFile(executable))
    {
        Trace(traceId, L"bridge missing or unsafe path=" + executable.wstring(), true);
        return false;
    }
    const auto result = RunBridgeCommand(executable, L"configuration", {},
        std::chrono::seconds(3), traceId);
    const auto configuration = result ? ParseSteamBridgeConfiguration(
        result->output, result->exitCode) : SteamBridgeConfiguration{};
    const bool compatible = IsSteamBridgeConfigurationCompatible(configuration, expectedVersion);
    Trace(traceId, L"configuration compatible=" + std::to_wstring(compatible) +
        L" valid=" + std::to_wstring(configuration.valid) +
        L" host_version=" + DiagnosticText(expectedVersion) +
        L" bridge_version=" + DiagnosticText(configuration.version) +
        L" protocol=" + std::to_wstring(configuration.protocolVersion) +
        L" app_id=" + std::to_wstring(configuration.expectedAppId) +
        L" sdk=" + std::to_wstring(configuration.steamworksCompiled), !compatible);
    return compatible;
}

BridgeResponse ParseBridgeResponse(
    std::string_view output, std::uint32_t exitCode)
{
    BridgeResponse response;
    const auto root = FinalJsonObject(output);
    if (!root)
    {
        response.errorDetail = "invalid_bridge_response: no JSON object";
        return response;
    }
    if (exitCode != 0)
    {
        response.errorCode = ErrorCode(*root);
        const JsonValue* error = Field(*root, "error", JsonValue::Type::Object);
        response.steamInitResult = error ? Uint32Field(*error, "steamInitResult")
            : std::nullopt;
        response.connectionProblem = steam_bridge::ClassifySteamConnectionProblem(
            response.errorCode, response.steamInitResult);
        response.errorDetail = response.errorCode;
        if (const JsonValue* message = error
                ? Field(*error, "message", JsonValue::Type::String) : nullptr)
            response.errorDetail += ": " + message->string;
        response.outcome = IsSteamUnavailableCode(response.errorCode)
            ? BridgeOutcome::SteamUnavailable : BridgeOutcome::Failed;
        return response;
    }

    const JsonValue* ok = Field(*root, "ok", JsonValue::Type::Boolean);
    const JsonValue* appId = Field(*root, "appId", JsonValue::Type::Number);
    const JsonValue* loggedOn = Field(
        *root, "loggedOn", JsonValue::Type::Boolean);
    const JsonValue* owned = Field(*root, "owned", JsonValue::Type::Boolean);
    const JsonValue* steamId = Field(
        *root, "steamId", JsonValue::Type::String);
    if (!ok || !ok->boolean || !appId ||
        appId->number != static_cast<double>(kSnowDesktopSteamAppId) ||
        !loggedOn || !loggedOn->boolean || !owned || !steamId ||
        !ParseSteamId(steamId->string, response.steamId))
    {
        response.errorDetail = "invalid_bridge_response: invalid ownership fields";
        return response;
    }
    response.outcome = owned->boolean
        ? BridgeOutcome::Owned : BridgeOutcome::NotOwned;
    return response;
}

struct Service::Impl
{
    Impl(std::filesystem::path bridge, std::filesystem::path runtime,
        std::filesystem::path cache)
        : bridgeExecutable(std::move(bridge)),
          steamRuntime(std::move(runtime)), protectedCache(std::move(cache))
    {
        const auto traceId = ++nextTraceId;
        const bool runtimeAvailable = IsSafeRegularFile(steamRuntime);
        Trace(traceId, L"service initialize host_version=" + DiagnosticText(SNOWDESKTOP_VERSION) +
            L" expected_app_id=" + std::to_wstring(kSnowDesktopSteamAppId) +
            L" runtime_available=" + std::to_wstring(runtimeAvailable) +
            L" runtime=" + steamRuntime.wstring());
        snapshot.bridgeAvailable = runtimeAvailable &&
            IsSteamBridgeExecutableCompatible(
                bridgeExecutable, SNOWDESKTOP_VERSION);
        snapshot.state = snapshot.bridgeAvailable
            ? State::Unregistered : State::BridgeUnavailable;
        const auto cached = UnprotectCache(protectedCache, traceId);
        if (snapshot.bridgeAvailable && cached && cached->validUntil > UnixNow())
        {
            snapshot.state = State::Registered;
            snapshot.registered = true;
            snapshot.validUntil = cached->validUntil;
        }
        Trace(traceId, L"service ready bridge_available=" +
            std::to_wstring(snapshot.bridgeAvailable) + L" cached_registered=" +
            std::to_wstring(snapshot.registered) + L" cache_valid_until=" +
            std::to_wstring(cached ? cached->validUntil : 0) +
            L" now=" + std::to_wstring(UnixNow()));
    }

    std::filesystem::path bridgeExecutable;
    std::filesystem::path steamRuntime;
    std::filesystem::path protectedCache;
    mutable std::mutex mutex;
    Snapshot snapshot;
    std::jthread worker;
    bool stopping = false;

    void Finish(BridgeResponse response, std::function<void()> completed,
        std::uint64_t traceId)
    {
        Failure failure = Failure::BridgeError;
        State state = State::RegistrationFailed;
        bool registered = false;
        std::int64_t validUntil = 0;
        {
            std::lock_guard lock(mutex);
            registered = snapshot.registered;
            validUntil = snapshot.validUntil;
        }
        if (response.outcome == BridgeOutcome::Owned)
        {
            const std::int64_t now = UnixNow();
            CacheRecord record;
            record.steamId = response.steamId;
            record.verifiedAt = now;
            record.validUntil = now + std::chrono::duration_cast<
                std::chrono::seconds>(kRegistrationLifetime).count();
            if (ProtectCache(protectedCache, record, traceId))
            {
                state = State::Registered;
                failure = Failure::None;
                registered = true;
                validUntil = record.validUntil;
            }
            else
                failure = Failure::StorageError;
        }
        else if (response.outcome == BridgeOutcome::NotOwned)
        {
            failure = Failure::NotOwned;
            registered = false;
            validUntil = 0;
            std::error_code ignored;
            std::filesystem::remove(protectedCache, ignored);
            Trace(traceId, L"non-owner cache removal error=" +
                std::to_wstring(ignored.value()), bool(ignored));
        }
        else if (response.outcome == BridgeOutcome::SteamUnavailable)
            failure = Failure::SteamUnavailable;

        bool notify = false;
        {
            std::lock_guard lock(mutex);
            if (!stopping)
            {
                snapshot.state = state;
                snapshot.failure = failure;
                snapshot.connectionProblem = response.connectionProblem;
                snapshot.errorDetail = std::move(response.errorDetail);
                snapshot.registered = registered;
                snapshot.validUntil = validUntil;
                ++snapshot.revision;
                notify = true;
            }
        }
        if (notify && completed)
            completed();
        Trace(traceId, L"registration complete registered=" + std::to_wstring(registered) +
            L" failure=" + std::to_wstring(static_cast<int>(failure)) +
            L" valid_until=" + std::to_wstring(validUntil) +
            L" notified=" + std::to_wstring(notify));
    }
};

Service::Service(std::filesystem::path bridgeExecutable,
    std::filesystem::path steamRuntime,
    std::filesystem::path protectedCache)
    : impl_(std::make_unique<Impl>(std::move(bridgeExecutable),
          std::move(steamRuntime), std::move(protectedCache)))
{
}

Service::~Service()
{
    Stop();
}

Snapshot Service::Current() const
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot;
}

bool Service::IsRegistered() const noexcept
{
    if (!impl_) return false;
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot.registered;
}

bool Service::StartRegistration(std::function<void()> completed,
    bool revalidateRegistered)
{
    if (!impl_) return false;
    const auto traceId = ++nextTraceId;
    DWORD session = 0;
    const BOOL sessionKnown = ProcessIdToSessionId(GetCurrentProcessId(), &session);
    HANDLE tokenRaw = nullptr;
    const BOOL tokenOpened = OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tokenRaw);
    UniqueHandle token(tokenRaw);
    TOKEN_ELEVATION elevation{};
    DWORD returned = 0;
    const BOOL elevationKnown = tokenOpened && GetTokenInformation(token.Get(),
        TokenElevation, &elevation, sizeof(elevation), &returned);
    Trace(traceId, L"registration requested revalidate=" +
        std::to_wstring(revalidateRegistered) + L" pid=" +
        std::to_wstring(GetCurrentProcessId()) + L" session_known=" +
        std::to_wstring(sessionKnown) + L" session=" + std::to_wstring(session) +
        L" elevated=" + (elevationKnown
            ? std::to_wstring(elevation.TokenIsElevated) : L"unknown"));
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || !impl_->snapshot.bridgeAvailable ||
            (impl_->snapshot.state == State::Registered &&
                !revalidateRegistered) ||
            impl_->snapshot.state == State::Checking)
        {
            Trace(traceId, L"registration skipped stopping=" + std::to_wstring(impl_->stopping) +
                L" bridge_available=" + std::to_wstring(impl_->snapshot.bridgeAvailable) +
                L" state=" + std::to_wstring(static_cast<int>(impl_->snapshot.state)));
            return false;
        }
        impl_->snapshot.state = State::Checking;
        impl_->snapshot.failure = Failure::None;
        impl_->snapshot.connectionProblem = steam_bridge::SteamConnectionProblem::None;
        impl_->snapshot.errorDetail.clear();
        ++impl_->snapshot.revision;
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
    Impl* const impl = impl_.get();
    impl_->worker = std::jthread(
        [impl, traceId, completed = std::move(completed)](std::stop_token stop) mutable {
            BridgeResponse response = RunBridge(impl->bridgeExecutable, stop, traceId);
            if (!stop.stop_requested())
                impl->Finish(std::move(response), std::move(completed), traceId);
        });
    return true;
}

bool Service::ResetRegistration()
{
    if (!impl_) return false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return false;
    }
    // Join before deleting: a check already persisting its result must not
    // recreate the cache after the user has cleared it.
    if (impl_->worker.joinable())
    {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
    std::error_code error;
    std::filesystem::remove(impl_->protectedCache, error);
    Trace(++nextTraceId, L"registration reset path=" + impl_->protectedCache.wstring() +
        L" error=" + std::to_wstring(error.value()), bool(error));
    std::lock_guard lock(impl_->mutex);
    ++impl_->snapshot.revision;
    impl_->snapshot.connectionProblem = steam_bridge::SteamConnectionProblem::None;
    impl_->snapshot.errorDetail.clear();
    if (error)
    {
        impl_->snapshot.state = State::RegistrationFailed;
        impl_->snapshot.failure = Failure::StorageError;
        return false;
    }
    impl_->snapshot.state = impl_->snapshot.bridgeAvailable
        ? State::Unregistered : State::BridgeUnavailable;
    impl_->snapshot.failure = Failure::None;
    impl_->snapshot.registered = false;
    impl_->snapshot.validUntil = 0;
    return true;
}

void Service::Stop() noexcept
{
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->stopping = true;
    }
    if (impl_->worker.joinable())
    {
        impl_->worker.request_stop();
        impl_->worker.join();
    }
}

} // namespace snowdesktop::steam_entitlement
