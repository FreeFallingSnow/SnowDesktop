#include "steam_workshop_source.h"

#include "data_paths.h"
#include "json_value.h"

#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string_view>
#include <thread>

namespace snowdesktop::widget
{
namespace
{
constexpr std::size_t kMaximumBridgeOutputBytes = 16u * 1024u * 1024u;
constexpr UINT kBridgeFailureExitCode = 5;
constexpr UINT kBridgeTimeoutExitCode = 6;

struct UniqueHandle
{
    HANDLE value = nullptr;

    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) : value(handle) {}
    ~UniqueHandle()
    {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : value(other.value)
    {
        other.value = nullptr;
    }
    UniqueHandle& operator=(UniqueHandle&& other) noexcept
    {
        if (this == &other) return *this;
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = other.value;
        other.value = nullptr;
        return *this;
    }
};

std::wstring QuoteArgument(std::wstring_view argument)
{
    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument)
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

std::wstring BuildCommandLine(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments)
{
    std::wstring commandLine = QuoteArgument(executable.wstring());
    for (const auto& argument : arguments)
    {
        commandLine.push_back(L' ');
        commandLine += QuoteArgument(argument);
    }
    return commandLine;
}

const JsonValue* JsonField(const JsonValue& object, std::string_view key,
    JsonValue::Type type)
{
    const JsonValue* value = object.Find(key);
    return value && value->type == type ? value : nullptr;
}

std::optional<std::string> JsonString(const JsonValue& object,
    std::string_view key)
{
    const JsonValue* value = JsonField(object, key, JsonValue::Type::String);
    return value ? std::optional<std::string>(value->string) : std::nullopt;
}

std::optional<bool> JsonBoolean(const JsonValue& object,
    std::string_view key)
{
    const JsonValue* value = JsonField(object, key, JsonValue::Type::Boolean);
    return value ? std::optional<bool>(value->boolean) : std::nullopt;
}

std::optional<std::uint32_t> JsonUint32(const JsonValue& object,
    std::string_view key)
{
    const JsonValue* value = JsonField(
        object, key, JsonValue::Type::Number);
    if (!value || !std::isfinite(value->number) ||
        value->number < 0.0 || std::floor(value->number) != value->number ||
        value->number > static_cast<double>(
            std::numeric_limits<std::uint32_t>::max()))
        return std::nullopt;
    return static_cast<std::uint32_t>(value->number);
}

bool DigitsOnly(std::string_view value)
{
    return !value.empty() && std::all_of(value.begin(), value.end(),
        [](unsigned char character) { return std::isdigit(character) != 0; });
}

bool SplitExternalItemId(const std::string& externalItemId,
    std::string& publishedFileId, std::string& ownerSteamId)
{
    const std::size_t separator = externalItemId.find('@');
    publishedFileId = externalItemId.substr(0, separator);
    ownerSteamId = separator == std::string::npos
        ? std::string{} : externalItemId.substr(separator + 1);
    return DigitsOnly(publishedFileId) &&
        (ownerSteamId.empty() || DigitsOnly(ownerSteamId));
}

std::string BoundExternalItemId(std::string_view publishedFileId,
    std::string_view ownerSteamId)
{
    if (ownerSteamId.empty()) return std::string(publishedFileId);
    return std::string(publishedFileId) + '@' + std::string(ownerSteamId);
}

bool HasReparsePoint(const std::filesystem::path& path)
{
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

bool ContainsReparsePoint(const std::filesystem::path& absolutePath)
{
    std::filesystem::path current = absolutePath.root_path();
    for (const auto& component : absolutePath.relative_path())
    {
        current /= component;
        const DWORD attributes = GetFileAttributesW(current.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES ||
            (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
            return true;
    }
    return false;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char character)
        {
            return static_cast<char>(std::tolower(character));
        });
    return value;
}

bool QueryMatches(const PackageManifest& manifest, const PackageQuery& query)
{
    if (query.text.empty()) return true;
    const std::string needle = LowerAscii(query.text);
    for (const std::string* field : {
        &manifest.name, &manifest.description, &manifest.author,
        &manifest.slug })
        if (LowerAscii(*field).find(needle) != std::string::npos)
            return true;
    return false;
}

std::optional<std::wstring> Utf8ToWide(std::string_view value)
{
    if (value.empty()) return std::wstring{};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return std::nullopt;
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()),
        result.data(), length) != length)
        return std::nullopt;
    return result;
}
}

struct SteamWorkshopSource::BridgeResult
{
    DWORD exitCode = std::numeric_limits<DWORD>::max();
    std::string output;
    JsonValue finalObject;
};

SteamWorkshopSource::SteamWorkshopSource()
    : SteamWorkshopSource(std::filesystem::path(
        GetExecutableDirectoryPath()) / L"SnowDesktopSteamBridge.exe")
{
}

SteamWorkshopSource::SteamWorkshopSource(
    std::filesystem::path bridgeExecutable)
    : bridgeExecutable_(std::move(bridgeExecutable))
{
}

std::string SteamWorkshopSource::ProviderId() const
{
    return "steam-workshop";
}

ProviderCapabilities SteamWorkshopSource::Capabilities() const
{
    return { true, true, false, true, false, true };
}

bool SteamWorkshopSource::RunBridge(
    const std::vector<std::wstring>& arguments, int timeoutSeconds,
    BridgeResult& result, std::string& error) const
{
    result = {};
    std::error_code filesystemError;
    if (!std::filesystem::is_regular_file(
        bridgeExecutable_, filesystemError))
    {
        error = "SnowDesktopSteamBridge.exe is missing";
        return false;
    }
    if (HasReparsePoint(bridgeExecutable_))
    {
        error = "Steam bridge executable cannot be a reparse point";
        return false;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE readRaw = nullptr;
    HANDLE writeRaw = nullptr;
    if (!CreatePipe(&readRaw, &writeRaw, &security, 0))
    {
        error = "cannot create the Steam bridge output pipe";
        return false;
    }
    UniqueHandle readPipe(readRaw);
    UniqueHandle writePipe(writeRaw);
    if (!SetHandleInformation(readPipe.value, HANDLE_FLAG_INHERIT, 0))
    {
        error = "cannot protect the Steam bridge output pipe";
        return false;
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = writePipe.value;
    startup.hStdError = writePipe.value;
    PROCESS_INFORMATION process{};
    std::wstring commandLine = BuildCommandLine(
        bridgeExecutable_, arguments);
    const std::wstring workingDirectory =
        bridgeExecutable_.parent_path().wstring();
    if (!CreateProcessW(bridgeExecutable_.c_str(), commandLine.data(),
        nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
        nullptr, workingDirectory.c_str(), &startup, &process))
    {
        error = "cannot start SnowDesktopSteamBridge.exe";
        return false;
    }
    UniqueHandle processHandle(process.hProcess);
    UniqueHandle threadHandle(process.hThread);
    writePipe = UniqueHandle{};

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(std::max(1, timeoutSeconds));
    while (true)
    {
        DWORD available = 0;
        if (!PeekNamedPipe(readPipe.value, nullptr, 0, nullptr,
            &available, nullptr))
        {
            if (WaitForSingleObject(processHandle.value, 0) == WAIT_OBJECT_0)
                break;
            error = "cannot read Steam bridge output";
            return false;
        }
        if (available > 0)
        {
            std::array<char, 8192> buffer{};
            DWORD read = 0;
            if (!ReadFile(readPipe.value, buffer.data(),
                std::min<DWORD>(available,
                    static_cast<DWORD>(buffer.size())), &read, nullptr))
            {
                error = "cannot read Steam bridge output";
                return false;
            }
            if (result.output.size() + read > kMaximumBridgeOutputBytes)
            {
                TerminateProcess(processHandle.value, kBridgeFailureExitCode);
                WaitForSingleObject(processHandle.value, 5000);
                error = "Steam bridge output exceeded its safety limit";
                return false;
            }
            result.output.append(buffer.data(), read);
            continue;
        }
        if (WaitForSingleObject(processHandle.value, 0) == WAIT_OBJECT_0)
            break;
        if (std::chrono::steady_clock::now() >= deadline)
        {
            TerminateProcess(processHandle.value, kBridgeTimeoutExitCode);
            WaitForSingleObject(processHandle.value, 5000);
            error = "Steam bridge operation timed out";
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!GetExitCodeProcess(processHandle.value, &result.exitCode))
    {
        error = "cannot read Steam bridge exit code";
        return false;
    }

    std::size_t end = result.output.size();
    bool parsed = false;
    while (end > 0)
    {
        while (end > 0 && (result.output[end - 1] == '\r' ||
            result.output[end - 1] == '\n')) --end;
        const std::size_t newline = end == 0 ? std::string::npos :
            result.output.rfind('\n', end - 1);
        const std::size_t begin = newline == std::string::npos
            ? 0 : newline + 1;
        const std::string_view line(
            result.output.data() + begin, end - begin);
        std::string parseError;
        if (!line.empty() && ParseJson(
            line, result.finalObject, &parseError) &&
            result.finalObject.IsObject())
        {
            parsed = true;
            break;
        }
        if (begin == 0) break;
        end = begin - 1;
    }
    if (!parsed)
    {
        error = "Steam bridge returned no valid JSON result";
        return false;
    }
    if (result.exitCode == 0 &&
        JsonBoolean(result.finalObject, "ok").value_or(false))
        return true;

    if (const JsonValue* errorObject = JsonField(
        result.finalObject, "error", JsonValue::Type::Object))
        if (const auto message = JsonString(*errorObject, "message"))
            error = *message;
    if (error.empty())
        if (const auto reason = JsonString(result.finalObject, "reason"))
            error = *reason;
    if (error.empty())
        error = "Steam bridge exited with code " +
            std::to_string(result.exitCode);
    return false;
}

ProviderStatus SteamWorkshopSource::Status()
{
    const auto now = std::chrono::steady_clock::now();
    if (statusCheckedAt_.time_since_epoch().count() != 0 &&
        now - statusCheckedAt_ < std::chrono::minutes(5))
        return cachedStatus_;
    BridgeResult result;
    std::string error;
    if (RunBridge({ L"status" }, 5, result, error) &&
        JsonUint32(result.finalObject, "protocolVersion") == 1u &&
        JsonBoolean(result.finalObject, "initialized").value_or(false) &&
        JsonBoolean(result.finalObject, "loggedOn").value_or(false) &&
        JsonUint32(result.finalObject, "appId").value_or(0u) != 0u)
        cachedStatus_ = { true, "Steam Workshop subscriptions are available" };
    else
    {
        if (error.empty())
            error = "Steam bridge is not logged on or uses an incompatible protocol";
        cachedStatus_ = { false, std::move(error) };
    }
    statusCheckedAt_ = now;
    return cachedStatus_;
}

std::optional<SteamWorkshopSource::ResolvedItem>
SteamWorkshopSource::ResolveInstalledFolder(
    const std::string& publishedFileId, const std::string& ownerSteamId,
    const std::filesystem::path& folder, std::string& error) const
{
    std::error_code filesystemError;
    const auto absoluteFolder = std::filesystem::absolute(
        folder, filesystemError);
    if (filesystemError || absoluteFolder.empty() ||
        ContainsReparsePoint(absoluteFolder))
    {
        error = "Steam Workshop install folder is unsafe or unavailable";
        return std::nullopt;
    }
    const auto canonicalFolder = std::filesystem::canonical(
        absoluteFolder, filesystemError);
    if (filesystemError ||
        !std::filesystem::is_directory(canonicalFolder, filesystemError) ||
        HasReparsePoint(canonicalFolder))
    {
        error = "Steam Workshop install folder is unsafe or unavailable";
        return std::nullopt;
    }

    std::filesystem::path artifact;
    std::size_t entryCount = 0;
    for (std::filesystem::directory_iterator iterator(
        canonicalFolder, std::filesystem::directory_options::none,
        filesystemError), end;
        !filesystemError && iterator != end; iterator.increment(filesystemError))
    {
        ++entryCount;
        const auto& entry = *iterator;
        std::error_code entryError;
        if (_wcsicmp(entry.path().filename().c_str(),
                L"package.snowwidget") == 0 &&
            entry.is_regular_file(entryError) && !entryError &&
            !entry.is_symlink(entryError) && !entryError &&
            !HasReparsePoint(entry.path()))
            artifact = entry.path();
    }
    if (filesystemError || entryCount != 1 || artifact.empty())
    {
        error = "Workshop content must contain exactly one package.snowwidget file";
        return std::nullopt;
    }

    PackageManifest manifest;
    const ValidationReport validation =
        validator_.ValidateArchive(artifact, &manifest);
    if (!validation.Ok())
    {
        error = "Workshop component package failed validation: " +
            validation.ToJson();
        return std::nullopt;
    }
    PackageDetails details;
    details.manifest = std::move(manifest);
    details.source = { ProviderId(),
        BoundExternalItemId(publishedFileId, ownerSteamId) };
    details.versions.push_back(details.manifest.version);
    return ResolvedItem{ std::move(details), artifact };
}

std::optional<SteamWorkshopSource::ResolvedItem>
SteamWorkshopSource::ResolveCurrent(const std::string& externalItemId,
    bool verifyOwner, std::string& error) const
{
    std::string publishedFileId;
    std::string expectedOwner;
    if (!SplitExternalItemId(
        externalItemId, publishedFileId, expectedOwner))
    {
        error = "invalid Steam Workshop external item identity";
        return std::nullopt;
    }

    BridgeResult listResult;
    if (!RunBridge({ L"workshop", L"list-subscribed", L"--details" },
        40, listResult, error))
        return std::nullopt;
    const JsonValue* items = JsonField(
        listResult.finalObject, "items", JsonValue::Type::Array);
    const auto appId = JsonUint32(listResult.finalObject, "appId");
    if (!items ||
        JsonUint32(listResult.finalObject, "protocolVersion") != 1u ||
        !appId || *appId == 0u)
    {
        error = "Steam bridge returned an incompatible subscription result";
        return std::nullopt;
    }
    for (const JsonValue& item : items->array)
    {
        if (!item.IsObject() ||
            JsonString(item, "publishedFileId").value_or("") !=
                publishedFileId)
            continue;
        const JsonValue* details = JsonField(
            item, "details", JsonValue::Type::Object);
        if (!details || JsonUint32(*details, "result") != 1u ||
            JsonUint32(*details, "consumerAppId") != appId ||
            JsonBoolean(*details, "banned").value_or(true))
        {
            error = "Steam Workshop item is banned or unavailable";
            return std::nullopt;
        }
        const std::string ownerSteamId =
            JsonString(*details, "ownerSteamId").value_or("");
        if (!DigitsOnly(ownerSteamId) ||
            (verifyOwner && !expectedOwner.empty() &&
                ownerSteamId != expectedOwner))
        {
            error = "Steam Workshop item owner identity changed";
            return std::nullopt;
        }
        if (JsonBoolean(item, "needsUpdate").value_or(true) ||
            JsonBoolean(item, "downloading").value_or(false) ||
            JsonBoolean(item, "downloadPending").value_or(false))
        {
            error = "Steam Workshop item is still downloading or needs an update";
            return std::nullopt;
        }
        const JsonValue* installInfo = JsonField(
            item, "installInfo", JsonValue::Type::Object);
        const auto folder = installInfo
            ? JsonString(*installInfo, "folder") : std::nullopt;
        const auto wideFolder = folder ? Utf8ToWide(*folder) : std::nullopt;
        if (!installInfo ||
            !JsonBoolean(*installInfo, "available").value_or(false) ||
            !wideFolder || wideFolder->empty())
        {
            error = "Steam Workshop item has not finished installing";
            return std::nullopt;
        }
        return ResolveInstalledFolder(
            publishedFileId, ownerSteamId, *wideFolder, error);
    }
    error = "Steam Workshop item is not subscribed";
    return std::nullopt;
}

std::vector<PackageDetails> SteamWorkshopSource::Query(
    const PackageQuery& query, std::string& error)
{
    BridgeResult result;
    if (!RunBridge({ L"workshop", L"list-subscribed", L"--details" },
        40, result, error))
        return {};
    const JsonValue* items = JsonField(
        result.finalObject, "items", JsonValue::Type::Array);
    const auto appId = JsonUint32(result.finalObject, "appId");
    if (!items || JsonUint32(result.finalObject, "protocolVersion") != 1u ||
        !appId || *appId == 0u)
    {
        error = "Steam bridge returned an incompatible subscription result";
        return {};
    }

    std::vector<PackageDetails> matches;
    std::size_t matched = 0;
    for (const JsonValue& item : items->array)
    {
        if (!item.IsObject()) continue;
        const auto publishedFileId = JsonString(item, "publishedFileId");
        const JsonValue* details = JsonField(
            item, "details", JsonValue::Type::Object);
        if (!publishedFileId || !DigitsOnly(*publishedFileId) ||
            !details || JsonUint32(*details, "result") != 1u ||
            JsonUint32(*details, "consumerAppId") != appId ||
            JsonBoolean(*details, "banned").value_or(true))
            continue;
        const auto ownerSteamId = JsonString(*details, "ownerSteamId");
        if (!ownerSteamId || !DigitsOnly(*ownerSteamId)) continue;
        if (JsonBoolean(item, "needsUpdate").value_or(true) ||
            JsonBoolean(item, "downloading").value_or(false) ||
            JsonBoolean(item, "downloadPending").value_or(false))
            continue;
        const JsonValue* installInfo = JsonField(
            item, "installInfo", JsonValue::Type::Object);
        const auto folder = installInfo
            ? JsonString(*installInfo, "folder") : std::nullopt;
        const auto wideFolder = folder ? Utf8ToWide(*folder) : std::nullopt;
        if (!installInfo ||
            !JsonBoolean(*installInfo, "available").value_or(false) ||
            !wideFolder || wideFolder->empty())
            continue;
        std::string itemError;
        auto resolved = ResolveInstalledFolder(
            *publishedFileId, *ownerSteamId, *wideFolder, itemError);
        if (!resolved) continue;
        resolved->details.manifest = LocalizePackageManifest(
            std::move(resolved->details.manifest), query.locale);
        if (!QueryMatches(resolved->details.manifest, query)) continue;
        if (matched++ < query.offset) continue;
        if (matches.size() >= query.limit) break;
        matches.push_back(std::move(resolved->details));
    }
    error.clear();
    return matches;
}

std::optional<PackageDetails> SteamWorkshopSource::GetDetails(
    const std::string& externalItemId, std::string& error)
{
    auto resolved = ResolveCurrent(externalItemId, true, error);
    if (!resolved) return std::nullopt;
    return std::move(resolved->details);
}

std::optional<PackageArtifact> SteamWorkshopSource::Materialize(
    const std::string& externalItemId, const std::string& version,
    const std::filesystem::path& destination, std::string& error)
{
    auto resolved = ResolveCurrent(externalItemId, true, error);
    if (!resolved) return std::nullopt;
    if (resolved->details.manifest.version != version)
    {
        error = "Steam Workshop currently exposes a different package version";
        return std::nullopt;
    }
    std::error_code filesystemError;
    std::filesystem::copy_file(resolved->artifact, destination,
        std::filesystem::copy_options::overwrite_existing, filesystemError);
    if (filesystemError)
    {
        error = "cannot copy the Workshop artifact into staging: " +
            filesystemError.message();
        return std::nullopt;
    }
    PackageManifest copiedManifest;
    const ValidationReport copiedValidation =
        validator_.ValidateArchive(destination, &copiedManifest);
    if (!copiedValidation.Ok() ||
        copiedManifest.id != resolved->details.manifest.id ||
        copiedManifest.version != version)
    {
        std::filesystem::remove(destination, filesystemError);
        error = "staged Workshop artifact failed validation: " +
            copiedValidation.ToJson();
        return std::nullopt;
    }
    const std::string sha256 =
        WidgetPackageManager::Sha256File(destination);
    if (sha256.empty())
    {
        std::filesystem::remove(destination, filesystemError);
        error = "cannot hash the staged Workshop artifact";
        return std::nullopt;
    }
    return PackageArtifact{ destination,
        resolved->details.manifest.id, version, sha256 };
}

std::vector<PackageUpdate> SteamWorkshopSource::CheckUpdates(
    const std::vector<PackageVersionRef>& installed, std::string& error)
{
    PackageQuery query;
    query.limit = std::numeric_limits<std::size_t>::max();
    const auto available = Query(query, error);
    if (!error.empty()) return {};
    std::vector<PackageUpdate> updates;
    for (const auto& current : installed)
    {
        const auto found = std::find_if(available.begin(), available.end(),
            [&](const PackageDetails& item)
            {
                return item.manifest.id == current.packageId &&
                    WidgetPackageValidator::IsNewerSemVer(
                        item.manifest.version, current.version);
            });
        if (found != available.end())
            updates.push_back({ current, *found });
    }
    return updates;
}
}
