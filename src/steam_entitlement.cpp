#include "steam_entitlement.h"

#include "atomic_file.h"
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
#include <charconv>
#include <chrono>
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

BridgeResponse RunBridge(const std::filesystem::path& executable,
    std::stop_token stop)
{
    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &security, 0))
        return {};
    UniqueHandle readPipe(readRaw);
    UniqueHandle writePipe(writeRaw);
    if (!SetHandleInformation(readPipe.Get(), HANDLE_FLAG_INHERIT, 0))
        return {};

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe.Get();
    startup.hStdError = writePipe.Get();
    PROCESS_INFORMATION process{};
    std::wstring commandLine = Quote(executable.wstring()) +
        L" entitlement status";
    const std::wstring workingDirectory = executable.parent_path().wstring();
    std::vector<wchar_t> environment =
        BuildSnowDesktopSteamChildEnvironment();
    if (environment.empty() || !CreateProcessW(executable.c_str(),
            commandLine.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
            environment.data(), workingDirectory.c_str(), &startup, &process))
        return {};

    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    writePipe.Reset();
    std::string output;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(15);
    while (true)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe.Get(), nullptr, 0, nullptr,
                &available, nullptr))
        {
            if (WaitForSingleObject(processHandle.Get(), 0) == WAIT_OBJECT_0)
                break;
            return {};
        }
        if (available > 0)
        {
            std::array<char, 4096> buffer{};
            DWORD read = 0;
            if (!ReadFile(readPipe.Get(), buffer.data(),
                    (std::min)(available,
                        static_cast<DWORD>(buffer.size())), &read, nullptr))
                return {};
            if (output.size() + read > kMaximumBridgeOutputBytes)
            {
                TerminateProcess(processHandle.Get(), kTimedOutExitCode);
                WaitForSingleObject(processHandle.Get(), 5000);
                return {};
            }
            output.append(buffer.data(), read);
            continue;
        }
        if (WaitForSingleObject(processHandle.Get(), 0) == WAIT_OBJECT_0)
            break;
        if (stop.stop_requested())
        {
            TerminateProcess(processHandle.Get(), kCanceledExitCode);
            WaitForSingleObject(processHandle.Get(), 5000);
            return {};
        }
        if (std::chrono::steady_clock::now() >= deadline)
        {
            TerminateProcess(processHandle.Get(), kTimedOutExitCode);
            WaitForSingleObject(processHandle.Get(), 5000);
            return {};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    DWORD exitCode = 0;
    if (!GetExitCodeProcess(processHandle.Get(), &exitCode))
        return {};
    return ParseBridgeResponse(output, exitCode);
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
    const std::filesystem::path& path)
{
    std::string encrypted;
    if (!atomic_file::ReadAll(path, encrypted) || encrypted.empty())
        return std::nullopt;
    DATA_BLOB input{static_cast<DWORD>(encrypted.size()),
        reinterpret_cast<BYTE*>(encrypted.data())};
    DATA_BLOB entropy{static_cast<DWORD>(kDpapiEntropy.size()),
        reinterpret_cast<BYTE*>(const_cast<char*>(kDpapiEntropy.data()))};
    DATA_BLOB plain{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
            CRYPTPROTECT_UI_FORBIDDEN, &plain))
        return std::nullopt;
    const std::string_view decoded(
        reinterpret_cast<const char*>(plain.pbData), plain.cbData);
    const auto result = Deserialize(decoded);
    LocalFree(plain.pbData);
    return result;
}

bool ProtectCache(const std::filesystem::path& path,
    const CacheRecord& record)
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
        return false;
    const std::string_view bytes(
        reinterpret_cast<const char*>(encrypted.pbData), encrypted.cbData);
    const bool saved = atomic_file::WriteAll(path, bytes);
    LocalFree(encrypted.pbData);
    return saved;
}

std::int64_t UnixNow()
{
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
}

BridgeResponse ParseBridgeResponse(
    std::string_view output, std::uint32_t exitCode)
{
    BridgeResponse response;
    const auto root = FinalJsonObject(output);
    if (!root)
        return response;
    if (exitCode != 0)
    {
        response.errorCode = ErrorCode(*root);
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
        return response;
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
        snapshot.bridgeAvailable = IsSafeRegularFile(bridgeExecutable) &&
            IsSafeRegularFile(steamRuntime);
        snapshot.state = snapshot.bridgeAvailable
            ? State::Unregistered : State::BridgeUnavailable;
        const auto cached = UnprotectCache(protectedCache);
        if (snapshot.bridgeAvailable && cached && cached->validUntil > UnixNow())
            snapshot.state = State::Registered;
    }

    std::filesystem::path bridgeExecutable;
    std::filesystem::path steamRuntime;
    std::filesystem::path protectedCache;
    mutable std::mutex mutex;
    Snapshot snapshot;
    std::jthread worker;
    bool stopping = false;

    void Finish(BridgeResponse response, std::function<void()> completed)
    {
        Failure failure = Failure::BridgeError;
        State state = State::RegistrationFailed;
        if (response.outcome == BridgeOutcome::Owned)
        {
            const std::int64_t now = UnixNow();
            CacheRecord record;
            record.steamId = response.steamId;
            record.verifiedAt = now;
            record.validUntil = now + std::chrono::duration_cast<
                std::chrono::seconds>(kRegistrationLifetime).count();
            if (ProtectCache(protectedCache, record))
            {
                state = State::Registered;
                failure = Failure::None;
            }
            else
                failure = Failure::StorageError;
        }
        else if (response.outcome == BridgeOutcome::NotOwned)
        {
            failure = Failure::NotOwned;
            std::error_code ignored;
            std::filesystem::remove(protectedCache, ignored);
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
                ++snapshot.revision;
                notify = true;
            }
        }
        if (notify && completed)
            completed();
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

Snapshot Service::Current() const noexcept
{
    if (!impl_) return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->snapshot;
}

bool Service::IsRegistered() const noexcept
{
    return Current().state == State::Registered;
}

bool Service::StartRegistration(std::function<void()> completed)
{
    if (!impl_) return false;
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping || !impl_->snapshot.bridgeAvailable ||
            impl_->snapshot.state == State::Registered ||
            impl_->snapshot.state == State::Checking)
            return false;
        impl_->snapshot.state = State::Checking;
        impl_->snapshot.failure = Failure::None;
        ++impl_->snapshot.revision;
    }
    if (impl_->worker.joinable())
        impl_->worker.join();
    Impl* const impl = impl_.get();
    impl_->worker = std::jthread(
        [impl, completed = std::move(completed)](std::stop_token stop) mutable {
            BridgeResponse response = RunBridge(impl->bridgeExecutable, stop);
            if (!stop.stop_requested())
                impl->Finish(std::move(response), std::move(completed));
        });
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
