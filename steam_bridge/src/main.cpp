// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include "component_workshop_publish.h"
#include "package_tool.h"
#include "steam_app_identity.h"
#include "steam_workshop_core.h"
#include "workshop_project.h"

#if SNOWDESKTOP_HAS_STEAMWORKS
#include <steam/steam_api.h>
#endif

namespace
{
constexpr int kSteamworksUnavailable = 3;
constexpr int kSteamInitializationFailed = 4;
constexpr int kSteamOperationFailed = 5;
constexpr int kSteamOperationTimedOut = 6;
constexpr int kInvalidArguments = 64;
constexpr std::uint64_t kMaximumWidgetPackageBytes =
    20ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumWorkshopPreviewBytes =
    1024ull * 1024ull - 1ull;

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0,
        nullptr, nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), result.data(), length,
        nullptr, nullptr) != length)
        return {};
    return result;
}

std::wstring Utf8ToWide(std::string_view value)
{
    if (value.empty()) return {};
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (length <= 0) return {};
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length) != length)
        return {};
    return result;
}

std::string NowIso8601()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    gmtime_s(&utc, &now);
    char buffer[32]{};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

bool OpenUrl(std::string_view url)
{
    const std::wstring wide = Utf8ToWide(url);
    if (wide.empty()) return false;
    const HINSTANCE opened = ShellExecuteW(nullptr, L"open", wide.c_str(),
        nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(opened) > 32;
}

void OpenCommunityItem(std::uint64_t publishedFileId,
    std::string_view webUrl)
{
    if (!OpenUrl(snowdesktop::steam_bridge::SteamCommunityItemClientUrl(
            publishedFileId)))
        OpenUrl(webUrl);
}

void WriteJsonString(std::ostream& output, std::string_view value)
{
    static constexpr char hex[] = "0123456789abcdef";
    output << '"';
    for (const unsigned char character : value)
    {
        switch (character)
        {
        case '\\': output << "\\\\"; break;
        case '"': output << "\\\""; break;
        case '\b': output << "\\b"; break;
        case '\f': output << "\\f"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (character < 0x20)
            {
                output << "\\u00" << hex[character >> 4]
                       << hex[character & 0x0f];
            }
            else
                output << static_cast<char>(character);
            break;
        }
    }
    output << '"';
}

int PrintError(int exitCode, std::string_view code, std::string_view message)
{
    std::cerr << "{\"ok\":false,\"error\":{";
    std::cerr << "\"code\":";
    WriteJsonString(std::cerr, code);
    std::cerr << ",\"message\":";
    WriteJsonString(std::cerr, message);
    std::cerr << "}}\n";
    return exitCode;
}

void PrintUsage()
{
    std::cout
        << "SnowDesktop Steam Bridge " << SNOWDESKTOP_VERSION << '\n'
        << "MIT-licensed SnowDesktop/Steam process boundary\n\n"
        << "Usage:\n"
        << "  SnowDesktopSteamBridge.exe --version\n"
        << "  SnowDesktopSteamBridge.exe configuration\n"
        << "  SnowDesktopSteamBridge.exe status\n"
        << "  SnowDesktopSteamBridge.exe workshop list-subscribed [--details]\n"
        << "  SnowDesktopSteamBridge.exe workshop list-published [--page N]\n"
        << "  SnowDesktopSteamBridge.exe workshop item-details --item ID\n"
        << "  SnowDesktopSteamBridge.exe workshop item-state --item ID\n"
        << "  SnowDesktopSteamBridge.exe workshop install-info --item ID\n"
        << "  SnowDesktopSteamBridge.exe workshop subscribe --item ID"
           " [--timeout-seconds N]\n"
        << "  SnowDesktopSteamBridge.exe workshop unsubscribe --item ID"
           " [--timeout-seconds N]\n"
        << "  SnowDesktopSteamBridge.exe workshop download --item ID"
           " [--high-priority] [--timeout-seconds N]\n"
        << "  SnowDesktopSteamBridge.exe workshop eula-status\n"
        << "  SnowDesktopSteamBridge.exe workshop publish --package FILE"
           " [--item ID] [--preview FILE] [--title TEXT]"
           " [--description TEXT] [--tag TAG] [--metadata JSON]"
           " [--language STEAM_LANGUAGE] [--visibility private|friends|public|unlisted]"
           " [--change-note TEXT] [--timeout-seconds N] [--open-page]\n"
        << "  SnowDesktopSteamBridge.exe workshop component-plan --source DIR"
           " --data-directory DIR [--item ID]"
           " [--text-source package|steam|manual-english]"
           " [--preview-source local|steam] [--tags-source local|steam]"
           " [--preview FILE] [--title TEXT] [--description TEXT]"
           " [--tag TAG|--clear-tags] [--visibility private|friends|public|unlisted]"
           " [--change-note TEXT] [--timeout-seconds N] [--force-content]\n"
        << "  SnowDesktopSteamBridge.exe workshop component-publish --source DIR"
           " [component-plan options] (--confirm-create|--confirm-update)"
           " [--open-page]\n\n"
        << "All successful command output is JSON. Long-running download and"
           " publish commands emit JSON Lines progress events before the final result.\n";
}

int PrintConfiguration()
{
    std::cout << "{\"ok\":true,\"protocolVersion\":1,"
                 "\"version\":";
    WriteJsonString(std::cout, SNOWDESKTOP_VERSION);
    std::cout << ",\"expectedAppId\":" <<
        snowdesktop::steam_bridge::kSteamAppId
              << ",\"windowsDepotId\":" <<
        snowdesktop::steam_bridge::kSteamWindowsDepotId
              << ",\"componentWorkflowProtocolVersion\":1"
              << ",\"steamworksCompiled\":"
              << (SNOWDESKTOP_HAS_STEAMWORKS ? "true" : "false")
              << "}\n";
    return 0;
}

struct ParsedOptions
{
    std::unordered_map<std::wstring, std::wstring> values;
    std::unordered_map<std::wstring, std::vector<std::wstring>> repeated;
    std::set<std::wstring> flags;

    std::optional<std::wstring> Value(std::wstring_view name) const
    {
        const auto found = values.find(std::wstring(name));
        if (found == values.end()) return std::nullopt;
        return found->second;
    }

    std::vector<std::wstring> Values(std::wstring_view name) const
    {
        const auto found = repeated.find(std::wstring(name));
        return found == repeated.end()
            ? std::vector<std::wstring>{} : found->second;
    }

    bool HasFlag(std::wstring_view name) const
    {
        return flags.contains(std::wstring(name));
    }
};

bool ParseOptions(const std::vector<std::wstring>& arguments,
    const std::set<std::wstring>& valueOptions,
    const std::set<std::wstring>& repeatedOptions,
    const std::set<std::wstring>& flagOptions,
    ParsedOptions& output, std::string& error)
{
    output = {};
    for (std::size_t index = 0; index < arguments.size(); ++index)
    {
        const std::wstring& name = arguments[index];
        if (flagOptions.contains(name))
        {
            if (!output.flags.insert(name).second)
            {
                error = "duplicate option: " + WideToUtf8(name);
                return false;
            }
            continue;
        }
        if (!valueOptions.contains(name) && !repeatedOptions.contains(name))
        {
            error = "unknown or positional argument: " + WideToUtf8(name);
            return false;
        }
        if (index + 1 >= arguments.size() ||
            arguments[index + 1].starts_with(L"--"))
        {
            error = "option requires a value: " + WideToUtf8(name);
            return false;
        }
        const std::wstring value = arguments[++index];
        if (repeatedOptions.contains(name))
            output.repeated[name].push_back(value);
        else if (!output.values.emplace(name, value).second)
        {
            error = "duplicate option: " + WideToUtf8(name);
            return false;
        }
    }
    return true;
}

bool ParsePositiveInteger(std::wstring_view text, std::uint64_t& value)
{
    if (text.empty() || !std::all_of(text.begin(), text.end(),
        [](wchar_t character) { return character >= L'0' && character <= L'9'; }))
        return false;
    wchar_t* end = nullptr;
    errno = 0;
    const std::wstring copy(text);
    const unsigned long long parsed = std::wcstoull(copy.c_str(), &end, 10);
    if (errno == ERANGE || end == copy.c_str() || *end != L'\0' || parsed == 0)
        return false;
    value = parsed;
    return true;
}

bool ReadItemId(const ParsedOptions& options, std::uint64_t& itemId,
    std::string& error)
{
    const auto value = options.Value(L"--item");
    if (!value || !ParsePositiveInteger(*value, itemId))
    {
        error = "--item must be a positive Workshop PublishedFileId";
        return false;
    }
    return true;
}

int ReadTimeoutSeconds(const ParsedOptions& options, int defaultValue,
    int maximumValue, std::string& error)
{
    const auto value = options.Value(L"--timeout-seconds");
    if (!value) return defaultValue;
    std::uint64_t parsed = 0;
    if (!ParsePositiveInteger(*value, parsed) ||
        parsed > static_cast<std::uint64_t>(maximumValue))
    {
        error = "--timeout-seconds is outside the supported range";
        return 0;
    }
    return static_cast<int>(parsed);
}

#if SNOWDESKTOP_HAS_STEAMWORKS
const char* ResultName(EResult result)
{
    switch (result)
    {
    case k_EResultOK: return "ok";
    case k_EResultFail: return "fail";
    case k_EResultNoConnection: return "no_connection";
    case k_EResultInvalidParam: return "invalid_param";
    case k_EResultFileNotFound: return "file_not_found";
    case k_EResultBusy: return "busy";
    case k_EResultInvalidState: return "invalid_state";
    case k_EResultAccessDenied: return "access_denied";
    case k_EResultTimeout: return "timeout";
    case k_EResultBanned: return "banned";
    case k_EResultServiceUnavailable: return "service_unavailable";
    case k_EResultNotLoggedOn: return "not_logged_on";
    case k_EResultInsufficientPrivilege: return "insufficient_privilege";
    case k_EResultLimitExceeded: return "limit_exceeded";
    case k_EResultLockingFailed: return "locking_failed";
    case k_EResultIOFailure: return "io_failure";
    case k_EResultCancelled: return "cancelled";
    case k_EResultDataCorruption: return "data_corruption";
    case k_EResultDiskFull: return "disk_full";
    case k_EResultRateLimitExceeded: return "rate_limit_exceeded";
    case k_EResultItemDeleted: return "item_deleted";
    default: return "steam_error";
    }
}

class SteamApiSession final
{
public:
    SteamApiSession()
    {
        SteamErrMsg message{};
        const ESteamAPIInitResult result = SteamAPI_InitEx(&message);
        initialized_ = result == k_ESteamAPIInitResult_OK;
        if (!initialized_)
        {
            errorCode_ = "steam_init_failed";
            error_ = message[0] ? message : "SteamAPI_InitEx failed";
            return;
        }
        ISteamUtils* utils = SteamUtils();
        if (!utils)
        {
            errorCode_ = "steam_interface_unavailable";
            error_ = "ISteamUtils was unavailable after initialization";
            SteamAPI_Shutdown();
            initialized_ = false;
            return;
        }
        appId_ = utils->GetAppID();
        if (!snowdesktop::steam_bridge::IsExpectedSteamAppId(appId_))
        {
            errorCode_ = "steam_app_id_mismatch";
            error_ = snowdesktop::steam_bridge::SteamAppIdMismatchMessage(
                appId_);
            SteamAPI_Shutdown();
            initialized_ = false;
        }
    }

    ~SteamApiSession()
    {
        if (initialized_) SteamAPI_Shutdown();
    }

    SteamApiSession(const SteamApiSession&) = delete;
    SteamApiSession& operator=(const SteamApiSession&) = delete;

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] std::uint32_t AppId() const noexcept { return appId_; }
    [[nodiscard]] const std::string& ErrorCode() const noexcept
    {
        return errorCode_;
    }
    [[nodiscard]] const std::string& Error() const noexcept { return error_; }

private:
    bool initialized_ = false;
    std::uint32_t appId_ = 0;
    std::string errorCode_ = "steam_init_failed";
    std::string error_;
};

template<typename Result, typename Progress>
bool WaitForCall(SteamAPICall_t call, Result& result,
    std::chrono::seconds timeout, Progress progress, std::string& error)
{
    if (call == k_uAPICallInvalid)
    {
        error = "Steam returned an invalid asynchronous call handle";
        return false;
    }
    ISteamUtils* utils = SteamUtils();
    if (!utils)
    {
        error = "ISteamUtils is unavailable";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        SteamAPI_RunCallbacks();
        bool callFailed = false;
        if (utils->IsAPICallCompleted(call, &callFailed))
        {
            bool resultFailed = false;
            if (callFailed || !utils->GetAPICallResult(call, &result,
                sizeof(result), Result::k_iCallback, &resultFailed) ||
                resultFailed)
            {
                error = "Steam asynchronous call failed";
                return false;
            }
            return true;
        }
        progress();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    error = "Steam operation timed out";
    return false;
}

template<typename Result>
bool WaitForCall(SteamAPICall_t call, Result& result,
    std::chrono::seconds timeout, std::string& error)
{
    return WaitForCall(call, result, timeout, [] {}, error);
}

struct InstallInfo
{
    bool installed = false;
    std::uint64_t sizeOnDisk = 0;
    std::string folder;
    std::uint32_t timestamp = 0;
};

InstallInfo GetInstallInfo(ISteamUGC& ugc, PublishedFileId_t itemId)
{
    InstallInfo result;
    std::array<char, 32768> folder{};
    result.installed = ugc.GetItemInstallInfo(itemId,
        &result.sizeOnDisk, folder.data(),
        static_cast<std::uint32_t>(folder.size()), &result.timestamp);
    if (result.installed) result.folder = folder.data();
    return result;
}

void WriteItemState(std::ostream& output, std::uint32_t state)
{
    output << "\"state\":" << state
           << ",\"subscribed\":"
           << ((state & k_EItemStateSubscribed) ? "true" : "false")
           << ",\"installed\":"
           << ((state & k_EItemStateInstalled) ? "true" : "false")
           << ",\"needsUpdate\":"
           << ((state & k_EItemStateNeedsUpdate) ? "true" : "false")
           << ",\"downloading\":"
           << ((state & k_EItemStateDownloading) ? "true" : "false")
           << ",\"downloadPending\":"
           << ((state & k_EItemStateDownloadPending) ? "true" : "false");
}

struct WorkshopDetails
{
    SteamUGCDetails_t value{};
    std::string metadata;
    std::string previewUrl;
};

bool QueryWorkshopDetails(ISteamUGC& ugc,
    const std::vector<PublishedFileId_t>& itemIds,
    std::unordered_map<std::uint64_t, WorkshopDetails>& output,
    std::string& error)
{
    output.clear();
    constexpr std::size_t batchSize = 100;
    for (std::size_t begin = 0; begin < itemIds.size(); begin += batchSize)
    {
        const std::size_t count = std::min(batchSize, itemIds.size() - begin);
        std::vector<PublishedFileId_t> batch(
            itemIds.begin() + static_cast<std::ptrdiff_t>(begin),
            itemIds.begin() + static_cast<std::ptrdiff_t>(begin + count));
        const UGCQueryHandle_t query = ugc.CreateQueryUGCDetailsRequest(
            batch.data(), static_cast<std::uint32_t>(batch.size()));
        if (query == k_UGCQueryHandleInvalid)
        {
            error = "Steam could not create a Workshop details query";
            return false;
        }
        ugc.SetReturnMetadata(query, true);
        ugc.SetReturnLongDescription(query, true);
        SteamUGCQueryCompleted_t completed{};
        const bool waited = WaitForCall(ugc.SendQueryUGCRequest(query),
            completed, std::chrono::seconds(30), error);
        if (!waited)
        {
            ugc.ReleaseQueryUGCRequest(query);
            return false;
        }
        if (completed.m_eResult != k_EResultOK)
        {
            error = std::string("Workshop details query failed: ") +
                ResultName(completed.m_eResult);
            ugc.ReleaseQueryUGCRequest(query);
            return false;
        }
        for (std::uint32_t index = 0;
            index < completed.m_unNumResultsReturned; ++index)
        {
            WorkshopDetails details;
            if (!ugc.GetQueryUGCResult(query, index, &details.value))
                continue;
            std::array<char, k_cchDeveloperMetadataMax> metadata{};
            if (ugc.GetQueryUGCMetadata(query, index, metadata.data(),
                static_cast<std::uint32_t>(metadata.size())))
                details.metadata = metadata.data();
            std::array<char, k_cchPublishedFileURLMax> preview{};
            if (ugc.GetQueryUGCPreviewURL(query, index, preview.data(),
                static_cast<std::uint32_t>(preview.size())))
                details.previewUrl = preview.data();
            output[static_cast<std::uint64_t>(
                details.value.m_nPublishedFileId)] = std::move(details);
        }
        ugc.ReleaseQueryUGCRequest(query);
    }
    return true;
}

void WriteWorkshopDetails(std::ostream& output,
    const WorkshopDetails& details)
{
    const auto& value = details.value;
    output << ",\"details\":{";
    output << "\"result\":" << static_cast<int>(value.m_eResult)
           << ",\"title\":";
    WriteJsonString(output, value.m_rgchTitle);
    output << ",\"description\":";
    WriteJsonString(output, value.m_rgchDescription);
    output << ",\"ownerSteamId\":\""
           << value.m_ulSteamIDOwner << "\""
           << ",\"creatorAppId\":" << value.m_nCreatorAppID
           << ",\"consumerAppId\":" << value.m_nConsumerAppID
           << ",\"created\":" << value.m_rtimeCreated
           << ",\"updated\":" << value.m_rtimeUpdated
           << ",\"visibility\":" << static_cast<int>(value.m_eVisibility)
           << ",\"banned\":" << (value.m_bBanned ? "true" : "false")
           << ",\"tags\":";
    WriteJsonString(output, value.m_rgchTags);
    output << ",\"fileSize\":" << value.m_ulTotalFilesSize
           << ",\"metadata\":";
    WriteJsonString(output, details.metadata);
    output << ",\"previewUrl\":";
    WriteJsonString(output, details.previewUrl);
    output << '}';
}

void WriteInstallInfo(std::ostream& output, const InstallInfo& info)
{
    output << "\"available\":" << (info.installed ? "true" : "false")
           << ",\"sizeOnDisk\":" << info.sizeOnDisk
           << ",\"folder\":";
    WriteJsonString(output, info.folder);
    output << ",\"timestamp\":" << info.timestamp;
}

int PrintInitializationFailure(const SteamApiSession& steam)
{
    return PrintError(kSteamInitializationFailed, steam.ErrorCode(),
        steam.Error().empty()
            ? "Launch the bridge through Steam and keep the Steam client running"
            : steam.Error());
}

int PrintStatus()
{
    SteamApiSession steam;
    if (!steam.IsInitialized()) return PrintInitializationFailure(steam);
    ISteamUtils* utils = SteamUtils();
    ISteamUser* user = SteamUser();
    if (!utils || !user)
        return PrintError(kSteamInitializationFailed,
            "steam_interface_unavailable",
            "Steam interfaces were unavailable after initialization");
    std::cout << "{\"ok\":true,\"protocolVersion\":1,"
                 "\"steamworksCompiled\":true,\"initialized\":true,"
                 "\"expectedAppId\":"
              << snowdesktop::steam_bridge::kSteamAppId
              << ",\"appId\":" << steam.AppId()
              << ",\"loggedOn\":" << (user->BLoggedOn() ? "true" : "false")
              << ",\"steamId\":\""
              << user->GetSteamID().ConvertToUint64() << "\"}\n";
    return 0;
}

int ListSubscribedWorkshopItems(const ParsedOptions& options)
{
    SteamApiSession steam;
    if (!steam.IsInitialized()) return PrintInitializationFailure(steam);
    ISteamUGC* ugc = SteamUGC();
    ISteamUtils* utils = SteamUtils();
    ISteamUser* user = SteamUser();
    if (!ugc || !utils || !user)
        return PrintError(kSteamInitializationFailed,
            "steam_ugc_unavailable", "Steam UGC interfaces are unavailable");
    if (!user->BLoggedOn())
        return PrintError(kSteamInitializationFailed,
            "steam_not_logged_on",
            "Steam is offline; subscription state is not authoritative");

    const std::uint32_t count = ugc->GetNumSubscribedItems();
    std::vector<PublishedFileId_t> itemIds(count);
    const std::uint32_t returned = itemIds.empty() ? 0 :
        ugc->GetSubscribedItems(itemIds.data(), count);
    itemIds.resize(returned);

    std::unordered_map<std::uint64_t, WorkshopDetails> details;
    std::string error;
    if (options.HasFlag(L"--details") &&
        !QueryWorkshopDetails(*ugc, itemIds, details, error))
        return PrintError(kSteamOperationFailed,
            "workshop_query_failed", error);

    std::cout << "{\"ok\":true,\"protocolVersion\":1,\"appId\":"
              << utils->GetAppID() << ",\"items\":[";
    for (std::size_t index = 0; index < itemIds.size(); ++index)
    {
        if (index != 0) std::cout << ',';
        const PublishedFileId_t itemId = itemIds[index];
        const std::uint32_t state = ugc->GetItemState(itemId);
        const InstallInfo install = GetInstallInfo(*ugc, itemId);
        std::uint64_t downloaded = 0;
        std::uint64_t total = 0;
        const bool hasDownload = ugc->GetItemDownloadInfo(
            itemId, &downloaded, &total);
        std::cout << "{\"publishedFileId\":\""
                  << static_cast<std::uint64_t>(itemId) << "\",";
        WriteItemState(std::cout, state);
        std::cout << ",\"downloadInfo\":{\"available\":"
                  << (hasDownload ? "true" : "false")
                  << ",\"downloaded\":" << downloaded
                  << ",\"total\":" << total << '}';
        std::cout << ",\"installInfo\":{";
        WriteInstallInfo(std::cout, install);
        std::cout << '}';
        const auto found = details.find(
            static_cast<std::uint64_t>(itemId));
        if (found != details.end())
            WriteWorkshopDetails(std::cout, found->second);
        std::cout << '}';
    }
    std::cout << "]}\n";
    return 0;
}

int PrintItemDetails(const ParsedOptions& options)
{
    std::uint64_t rawItemId = 0;
    std::string error;
    if (!ReadItemId(options, rawItemId, error))
        return PrintError(kInvalidArguments, "invalid_arguments", error);
    SteamApiSession steam;
    if (!steam.IsInitialized()) return PrintInitializationFailure(steam);
    ISteamUGC* ugc = SteamUGC();
    if (!ugc)
        return PrintError(kSteamInitializationFailed,
            "steam_ugc_unavailable", "ISteamUGC is unavailable");
    const PublishedFileId_t itemId =
        static_cast<PublishedFileId_t>(rawItemId);
    std::unordered_map<std::uint64_t, WorkshopDetails> details;
    if (!QueryWorkshopDetails(*ugc, { itemId }, details, error))
        return PrintError(kSteamOperationFailed,
            "workshop_query_failed", error);
    const auto found = details.find(rawItemId);
    if (found == details.end())
        return PrintError(kSteamOperationFailed,
            "workshop_item_not_found", "Steam returned no details for the item");
    std::cout << "{\"ok\":true,\"publishedFileId\":\""
              << rawItemId << "\"";
    WriteWorkshopDetails(std::cout, found->second);
    std::cout << "}\n";
    return 0;
}

int PrintItemState(const ParsedOptions& options)
{
    std::uint64_t rawItemId = 0;
    std::string error;
    if (!ReadItemId(options, rawItemId, error))
        return PrintError(kInvalidArguments, "invalid_arguments", error);
    SteamApiSession steam;
    if (!steam.IsInitialized()) return PrintInitializationFailure(steam);
    ISteamUGC* ugc = SteamUGC();
    if (!ugc)
        return PrintError(kSteamInitializationFailed,
            "steam_ugc_unavailable", "ISteamUGC is unavailable");
    const PublishedFileId_t itemId =
        static_cast<PublishedFileId_t>(rawItemId);
    const std::uint32_t state = ugc->GetItemState(itemId);
    std::uint64_t downloaded = 0;
    std::uint64_t total = 0;
    const bool hasDownload = ugc->GetItemDownloadInfo(
        itemId, &downloaded, &total);
    std::cout << "{\"ok\":true,\"publishedFileId\":\""
              << rawItemId << "\",";
    WriteItemState(std::cout, state);
    std::cout << ",\"downloadInfo\":{\"available\":"
              << (hasDownload ? "true" : "false")
              << ",\"downloaded\":" << downloaded
              << ",\"total\":" << total << "}}\n";
    return 0;
}

int PrintItemInstallInfo(const ParsedOptions& options)
{
    std::uint64_t rawItemId = 0;
    std::string error;
    if (!ReadItemId(options, rawItemId, error))
        return PrintError(kInvalidArguments, "invalid_arguments", error);
    SteamApiSession steam;
    if (!steam.IsInitialized()) return PrintInitializationFailure(steam);
    ISteamUGC* ugc = SteamUGC();
    if (!ugc)
        return PrintError(kSteamInitializationFailed,
            "steam_ugc_unavailable", "ISteamUGC is unavailable");
    const InstallInfo info = GetInstallInfo(*ugc,
        static_cast<PublishedFileId_t>(rawItemId));
    std::cout << "{\"ok\":true,\"publishedFileId\":\""
              << rawItemId << "\",\"installInfo\":{";
    WriteInstallInfo(std::cout, info);
    std::cout << "}}\n";
    return 0;
}

class DownloadWaiter final
{
public:
    DownloadWaiter(AppId_t appId, PublishedFileId_t itemId)
        : appId_(appId), itemId_(itemId),
          callback_(this, &DownloadWaiter::OnDownloadResult)
    {
    }

    bool Completed() const { return completed_; }
    EResult Result() const { return result_; }

private:
    void OnDownloadResult(DownloadItemResult_t* value)
    {
        if (!value || value->m_unAppID != appId_ ||
            value->m_nPublishedFileId != itemId_)
            return;
        completed_ = true;
        result_ = value->m_eResult;
    }

    AppId_t appId_;
    PublishedFileId_t itemId_;
    bool completed_ = false;
    EResult result_ = k_EResultPending;
    CCallback<DownloadWaiter, DownloadItemResult_t> callback_;
};

int DownloadWorkshopItem(const ParsedOptions& options)
{
    std::uint64_t rawItemId = 0;
    std::string error;
    if (!ReadItemId(options, rawItemId, error))
        return PrintError(kInvalidArguments, "invalid_arguments", error);
    const int timeoutSeconds = ReadTimeoutSeconds(
        options, 600, 3600, error);
    if (timeoutSeconds == 0)
        return PrintError(kInvalidArguments, "invalid_arguments", error);

    SteamApiSession steam;
    if (!steam.IsInitialized()) return PrintInitializationFailure(steam);
    ISteamUGC* ugc = SteamUGC();
    ISteamUtils* utils = SteamUtils();
    if (!ugc || !utils)
        return PrintError(kSteamInitializationFailed,
            "steam_ugc_unavailable", "Steam UGC interfaces are unavailable");
    const PublishedFileId_t itemId =
        static_cast<PublishedFileId_t>(rawItemId);
    DownloadWaiter waiter(utils->GetAppID(), itemId);
    if (!ugc->DownloadItem(itemId, options.HasFlag(L"--high-priority")))
        return PrintError(kSteamOperationFailed,
            "download_not_started",
            "Steam rejected the Workshop download request");

    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(timeoutSeconds);
    auto nextProgress = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() < deadline)
    {
        SteamAPI_RunCallbacks();
        const std::uint32_t state = ugc->GetItemState(itemId);
        if (waiter.Completed() && waiter.Result() != k_EResultOK)
            return PrintError(kSteamOperationFailed,
                "download_failed", ResultName(waiter.Result()));
        if ((state & k_EItemStateInstalled) &&
            !(state & (k_EItemStateNeedsUpdate |
                       k_EItemStateDownloading |
                       k_EItemStateDownloadPending)))
            break;
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextProgress)
        {
            std::uint64_t downloaded = 0;
            std::uint64_t total = 0;
            const bool available = ugc->GetItemDownloadInfo(
                itemId, &downloaded, &total);
            std::cout << "{\"event\":\"download-progress\","
                         "\"publishedFileId\":\"" << rawItemId
                      << "\",\"available\":"
                      << (available ? "true" : "false")
                      << ",\"downloaded\":" << downloaded
                      << ",\"total\":" << total
                      << ",\"state\":" << state << "}\n" << std::flush;
            nextProgress = now + std::chrono::milliseconds(250);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const std::uint32_t finalState = ugc->GetItemState(itemId);
    if (!(finalState & k_EItemStateInstalled) ||
        (finalState & (k_EItemStateNeedsUpdate |
                      k_EItemStateDownloading |
                      k_EItemStateDownloadPending)))
        return PrintError(kSteamOperationTimedOut,
            "download_timed_out", "Workshop download timed out");

    const InstallInfo install = GetInstallInfo(*ugc, itemId);
    if (!install.installed)
        return PrintError(kSteamOperationFailed,
            "install_info_unavailable",
            "Steam completed the download but did not expose its install folder");
    std::cout << "{\"ok\":true,\"publishedFileId\":\""
              << rawItemId << "\",\"installInfo\":{";
    WriteInstallInfo(std::cout, install);
    std::cout << "}}\n";
    return 0;
}

int PrintWorkshopEulaStatus()
{
    SteamApiSession steam;
    if (!steam.IsInitialized()) return PrintInitializationFailure(steam);
    ISteamUGC* ugc = SteamUGC();
    if (!ugc)
        return PrintError(kSteamInitializationFailed,
            "steam_ugc_unavailable", "ISteamUGC is unavailable");
    WorkshopEULAStatus_t status{};
    std::string error;
    if (!WaitForCall(ugc->GetWorkshopEULAStatus(), status,
        std::chrono::seconds(30), error))
        return PrintError(kSteamOperationFailed, "eula_query_failed", error);
    if (status.m_eResult == k_EResultInvalidParam)
    {
        std::cout << "{\"ok\":true,\"available\":false}\n";
        return 0;
    }
    if (status.m_eResult != k_EResultOK)
        return PrintError(kSteamOperationFailed, "eula_query_failed",
            ResultName(status.m_eResult));
    std::cout << "{\"ok\":true,\"available\":true,\"appId\":"
              << status.m_nAppID
              << ",\"version\":" << status.m_unVersion
              << ",\"actionTime\":" << status.m_rtAction
              << ",\"accepted\":"
              << (status.m_bAccepted ? "true" : "false")
              << ",\"needsAction\":"
              << (status.m_bNeedsAction ? "true" : "false") << "}\n";
    return 0;
}

std::optional<ERemoteStoragePublishedFileVisibility> ParseVisibility(
    const std::optional<std::wstring>& value, std::string& error)
{
    if (!value) return std::nullopt;
    if (*value == L"public")
        return k_ERemoteStoragePublishedFileVisibilityPublic;
    if (*value == L"friends")
        return k_ERemoteStoragePublishedFileVisibilityFriendsOnly;
    if (*value == L"private")
        return k_ERemoteStoragePublishedFileVisibilityPrivate;
    if (*value == L"unlisted")
        return k_ERemoteStoragePublishedFileVisibilityUnlisted;
    error = "--visibility must be private, friends, public, or unlisted";
    return std::nullopt;
}

bool ApplyComponentSources(const ParsedOptions& options,
    snowdesktop::steam_bridge::WorkshopProject& project,
    std::string& error)
{
    using namespace snowdesktop::steam_bridge;
    if (const auto source = options.Value(L"--text-source"))
    {
        if (*source == L"package")
            project.publishPreferences.textSource =
                WorkshopTextSource::Package;
        else if (*source == L"steam")
            project.publishPreferences.textSource = WorkshopTextSource::Steam;
        else if (*source == L"manual-english")
            project.publishPreferences.textSource =
                WorkshopTextSource::ManualEnglish;
        else
        {
            error = "--text-source must be package, steam, or manual-english";
            return false;
        }
    }
    const auto parseAssetSource = [&](std::wstring_view option,
        WorkshopAssetSource& destination)
    {
        const auto source = options.Value(option);
        if (!source) return true;
        if (*source == L"local") destination = WorkshopAssetSource::Local;
        else if (*source == L"steam")
            destination = WorkshopAssetSource::Steam;
        else
        {
            error = WideToUtf8(option) + " must be local or steam";
            return false;
        }
        return true;
    };
    if (!parseAssetSource(L"--preview-source",
            project.publishPreferences.previewSource) ||
        !parseAssetSource(L"--tags-source",
            project.publishPreferences.tagsSource))
        return false;
    if (const auto title = options.Value(L"--title"))
        project.publishPreferences.manualEnglishTitle = WideToUtf8(*title);
    if (const auto description = options.Value(L"--description"))
        project.publishPreferences.manualEnglishDescription =
            WideToUtf8(*description);
    if ((options.Value(L"--title") || options.Value(L"--description")) &&
        project.publishPreferences.textSource !=
            WorkshopTextSource::ManualEnglish)
    {
        error = "--title and --description require --text-source manual-english";
        return false;
    }
    if (const auto preview = options.Value(L"--preview"))
        project.primaryPreview = *preview;
    if (options.HasFlag(L"--clear-tags") &&
        options.repeated.contains(L"--tag"))
    {
        error = "--clear-tags cannot be combined with --tag";
        return false;
    }
    if (options.HasFlag(L"--clear-tags")) project.tags.clear();
    else if (options.repeated.contains(L"--tag"))
    {
        project.tags.clear();
        for (const auto& tag : options.Values(L"--tag"))
            project.tags.push_back(WideToUtf8(tag));
    }
    return true;
}

void PrintComponentPlan(
    const snowdesktop::steam_bridge::WorkshopProject& project,
    const snowdesktop::steam_bridge::ComponentPublishPlan& plan,
    bool registered, bool event)
{
    using namespace snowdesktop::steam_bridge;
    std::cout << '{';
    if (event) std::cout << "\"event\":\"component-plan\",";
    std::cout << "\"ok\":true,\"protocolVersion\":1,\"registered\":"
              << (registered ? "true" : "false")
              << ",\"localId\":";
    WriteJsonString(std::cout, project.localId);
    std::cout << ",\"sourceDirectory\":";
    WriteJsonString(std::cout,
        WideToUtf8(project.sourceDirectory.wstring()));
    std::cout << ",\"action\":";
    WriteJsonString(std::cout, ComponentPublishActionName(plan.action));
    std::cout << ",\"confirmationRequired\":";
    WriteJsonString(std::cout,
        plan.action == ComponentPublishAction::Create ?
            "confirm-create" : "confirm-update");
    std::cout << ",\"publishedFileId\":";
    if (plan.publishedFileId)
        WriteJsonString(std::cout, std::to_string(*plan.publishedFileId));
    else std::cout << "null";
    std::cout << ",\"package\":{\"id\":";
    WriteJsonString(std::cout, plan.packageId);
    std::cout << ",\"version\":";
    WriteJsonString(std::cout, plan.version);
    std::cout << ",\"sha256\":";
    WriteJsonString(std::cout, plan.sha256);
    std::cout << ",\"uploadContent\":"
              << (plan.updateContent ? "true" : "false") << '}';
    std::cout << ",\"listing\":{\"source\":";
    WriteJsonString(std::cout,
        WorkshopTextSourceName(project.publishPreferences.textSource));
    std::cout << ",\"localizations\":[";
    for (std::size_t index = 0; index < plan.localizations.size(); ++index)
    {
        if (index) std::cout << ',';
        const auto& localized = plan.localizations[index];
        std::cout << "{\"language\":";
        WriteJsonString(std::cout, localized.language);
        std::cout << ",\"title\":";
        WriteJsonString(std::cout, localized.title);
        std::cout << ",\"description\":";
        WriteJsonString(std::cout, localized.description);
        std::cout << '}';
    }
    std::cout << "]},\"preview\":{\"source\":";
    WriteJsonString(std::cout,
        WorkshopAssetSourceName(project.publishPreferences.previewSource));
    std::cout << ",\"path\":";
    if (plan.preview)
        WriteJsonString(std::cout, WideToUtf8(plan.preview->wstring()));
    else std::cout << "null";
    std::cout << "},\"tags\":{\"source\":";
    WriteJsonString(std::cout,
        WorkshopAssetSourceName(project.publishPreferences.tagsSource));
    std::cout << ",\"values\":";
    if (!plan.tags) std::cout << "null";
    else
    {
        std::cout << '[';
        for (std::size_t index = 0; index < plan.tags->size(); ++index)
        {
            if (index) std::cout << ',';
            WriteJsonString(std::cout, (*plan.tags)[index]);
        }
        std::cout << ']';
    }
    std::cout << "},\"visibility\":";
    if (plan.action == ComponentPublishAction::Create)
        WriteJsonString(std::cout, "private");
    else if (plan.visibility)
        std::cout << *plan.visibility;
    else std::cout << "null";
    std::cout << "}\n";
}

int PrintComponentPublishError(int exitCode, std::string_view code,
    std::string_view message, std::optional<std::uint64_t> item,
    std::string_view language)
{
    std::cerr << "{\"ok\":false,\"error\":{\"code\":";
    WriteJsonString(std::cerr, code);
    std::cerr << ",\"message\":";
    WriteJsonString(std::cerr, message);
    std::cerr << "},\"publishedFileId\":";
    if (item) WriteJsonString(std::cerr, std::to_string(*item));
    else std::cerr << "null";
    std::cerr << ",\"failedLanguage\":";
    if (language.empty()) std::cerr << "null";
    else WriteJsonString(std::cerr, language);
    std::cerr << "}\n";
    return exitCode;
}

int RunComponentWorkshopCommand(const ParsedOptions& options, bool execute)
{
    using namespace snowdesktop::steam_bridge;
    const auto source = options.Value(L"--source");
    const auto dataDirectoryValue = options.Value(L"--data-directory");
    if (!source || !dataDirectoryValue)
        return PrintError(kInvalidArguments, "invalid_arguments",
            "component workflow requires --source and --data-directory");
    const std::filesystem::path dataDirectory = *dataDirectoryValue;
    const auto managerRoot = WorkshopManagerDataRoot(dataDirectory);
    ProjectStore store(managerRoot);
    std::string message;
    if (!store.Load(message))
        return PrintError(kSteamOperationFailed, "project_store_failed",
            message);
    const std::size_t projectCount = store.Projects().size();
    WorkshopProject* project = nullptr;
    if (!store.AddDirectory(*source, project, message) || !project)
        return PrintError(kInvalidArguments, "invalid_component_source",
            message);
    const bool registered = store.Projects().size() == projectCount;
    if (const auto item = options.Value(L"--item"))
    {
        std::uint64_t parsed = 0;
        if (!ParsePositiveInteger(*item, parsed))
            return PrintError(kInvalidArguments, "invalid_arguments",
                "--item must be a positive Workshop PublishedFileId");
        if (project->publishedFileId && *project->publishedFileId != parsed)
            return PrintError(kInvalidArguments, "project_item_mismatch",
                "--item does not match the local project binding");
        project->publishedFileId = parsed;
        if (!registered)
        {
            project->publishPreferences.textSource =
                WorkshopTextSource::Steam;
            project->publishPreferences.previewSource =
                WorkshopAssetSource::Steam;
            project->publishPreferences.tagsSource =
                WorkshopAssetSource::Steam;
        }
    }
    if (!ApplyComponentSources(options, *project, message))
        return PrintError(kInvalidArguments, "invalid_arguments", message);

    PackageTool packageTool({}, managerRoot / L"staging" / L"packages");
    WidgetInspection inspection;
    PackagedWidget artifact;
    if (!packageTool.Inspect(project->sourceDirectory,
            inspection, message) ||
        !packageTool.Pack(project->sourceDirectory,
            inspection, artifact, message))
        return PrintError(kInvalidArguments, "component_validation_failed",
            message);

    ComponentPublishOptions publishOptions;
    publishOptions.forceContent = options.HasFlag(L"--force-content");
    if (const auto note = options.Value(L"--change-note"))
        publishOptions.changeNote = WideToUtf8(*note);
    const int timeout = ReadTimeoutSeconds(options, 1800, 7200, message);
    if (timeout == 0)
        return PrintError(kInvalidArguments, "invalid_arguments", message);
    publishOptions.timeout = std::chrono::seconds(timeout);
    if (const auto visibilityValue = options.Value(L"--visibility"))
    {
        const auto visibility = ParseVisibility(visibilityValue, message);
        if (!visibility)
            return PrintError(kInvalidArguments, "invalid_arguments",
                message);
        publishOptions.visibility = static_cast<int>(*visibility);
    }

    ComponentPublishPlan plan;
    if (!BuildComponentPublishPlan(*project, inspection, artifact,
            publishOptions, plan, message))
        return PrintError(kInvalidArguments, "publish_plan_failed", message);
    if (plan.action == ComponentPublishAction::Create &&
        publishOptions.visibility && *publishOptions.visibility !=
            static_cast<int>(
                k_ERemoteStoragePublishedFileVisibilityPrivate))
        return PrintError(kInvalidArguments, "invalid_visibility",
            "new component items are always created private");
    if (!execute)
    {
        PrintComponentPlan(*project, plan, registered, false);
        return 0;
    }

    const bool confirmCreate = options.HasFlag(L"--confirm-create");
    const bool confirmUpdate = options.HasFlag(L"--confirm-update");
    const bool creating = plan.action == ComponentPublishAction::Create;
    if (confirmCreate == confirmUpdate || creating != confirmCreate)
    {
        return PrintError(kInvalidArguments, "confirmation_required",
            creating ?
                "review component-plan, then pass --confirm-create" :
                "review component-plan, then pass --confirm-update");
    }
    project->packageId = inspection.packageId;
    if (!store.Save(message))
        return PrintError(kSteamOperationFailed, "project_store_failed",
            message);

    PrintComponentPlan(*project, plan, registered, true);
    SteamWorkshopCore steam(managerRoot / L"staging" / L"uploads");
    ComponentPublishResult result;
    CoreError coreError;
    std::string progressStoreError;
    const bool published = ExecuteComponentPublishPlan(plan, steam,
        [&](const ComponentPublishProgress& progress)
        {
            if (progress.steam.stage == PublishStage::Created)
            {
                project->publishedFileId =
                    progress.steam.publishedFileId;
                if (!store.Save(progressStoreError) &&
                    progressStoreError.empty())
                    progressStoreError =
                        "cannot persist the created Workshop item";
            }
            std::cout << "{\"event\":\"component-publish-progress\","
                         "\"submissionIndex\":"
                      << progress.submissionIndex
                      << ",\"submissionTotal\":"
                      << progress.submissionTotal
                      << ",\"language\":";
            if (progress.language.empty()) std::cout << "null";
            else WriteJsonString(std::cout, progress.language);
            std::cout << ",\"stage\":";
            WriteJsonString(std::cout,
                PublishStageName(progress.steam.stage));
            std::cout << ",\"publishedFileId\":";
            if (progress.steam.publishedFileId)
                WriteJsonString(std::cout,
                    std::to_string(progress.steam.publishedFileId));
            else std::cout << "null";
            std::cout << ",\"status\":" << progress.steam.steamStatus
                      << ",\"processed\":" << progress.steam.processed
                      << ",\"total\":" << progress.steam.total
                      << ",\"submitStarted\":"
                      << (progress.steam.submitStarted ? "true" : "false")
                      << "}\n" << std::flush;
        }, result, coreError);
    if (result.publishedFileId)
        project->publishedFileId = result.publishedFileId;
    if (result.baseSubmitted)
    {
        project->packageId = plan.packageId;
        project->lastPublishedVersion = plan.version;
        project->lastPublishedSha256 = plan.sha256;
        project->lastPublishedAt = NowIso8601();
    }
    std::string saveError;
    if (!store.Save(saveError) && progressStoreError.empty())
        progressStoreError = saveError;
    if (!published)
    {
        const std::optional<std::uint64_t> item =
            project->publishedFileId;
        return PrintComponentPublishError(coreError.exitCode,
            coreError.code, coreError.message, item,
            result.failedLanguage);
    }
    if (!progressStoreError.empty())
        return PrintComponentPublishError(kSteamOperationFailed,
            "project_store_failed", progressStoreError,
            project->publishedFileId, {});
    if (options.HasFlag(L"--open-page"))
        OpenCommunityItem(result.publishedFileId, result.communityUrl);
    std::cout << "{\"ok\":true,\"protocolVersion\":1,"
                 "\"action\":";
    WriteJsonString(std::cout, ComponentPublishActionName(plan.action));
    std::cout << ",\"publishedFileId\":";
    WriteJsonString(std::cout, std::to_string(result.publishedFileId));
    std::cout << ",\"created\":"
              << (result.created ? "true" : "false")
              << ",\"contentUploaded\":"
              << (result.contentUploaded ? "true" : "false")
              << ",\"localizedSubmissions\":"
              << result.localizedSubmissions
              << ",\"needsLegalAgreement\":"
              << (result.needsLegalAgreement ? "true" : "false")
              << ",\"packageId\":";
    WriteJsonString(std::cout, plan.packageId);
    std::cout << ",\"version\":";
    WriteJsonString(std::cout, plan.version);
    std::cout << ",\"sha256\":";
    WriteJsonString(std::cout, plan.sha256);
    std::cout << ",\"communityUrl\":";
    WriteJsonString(std::cout, result.communityUrl);
    std::cout << "}\n";
    return 0;
}

int ListPublishedWorkshopItems(const ParsedOptions& options)
{
    std::uint64_t rawPage = 1;
    if (const auto value = options.Value(L"--page");
        value && !ParsePositiveInteger(*value, rawPage))
        return PrintError(kInvalidArguments, "invalid_page",
            "--page must be a positive integer");
    if (rawPage > std::numeric_limits<std::uint32_t>::max())
        return PrintError(kInvalidArguments, "invalid_page",
            "--page is too large");
    snowdesktop::steam_bridge::SteamWorkshopCore core;
    snowdesktop::steam_bridge::CoreError error;
    const auto result = core.ListPublished(
        static_cast<std::uint32_t>(rawPage), error);
    if (!result) return PrintError(error.exitCode, error.code, error.message);
    std::cout << "{\"ok\":true,\"protocolVersion\":1,\"page\":"
              << result->page << ",\"totalPages\":" << result->totalPages
              << ",\"totalResults\":" << result->totalResults
              << ",\"items\":[";
    for (std::size_t index = 0; index < result->items.size(); ++index)
    {
        if (index) std::cout << ',';
        const auto& item = result->items[index];
        std::cout << "{\"publishedFileId\":\""
                  << item.publishedFileId << "\",\"ownerSteamId\":\""
                  << item.ownerSteamId << "\",\"creatorAppId\":"
                  << item.creatorAppId << ",\"consumerAppId\":"
                  << item.consumerAppId << ",\"createdAt\":"
                  << item.createdAt << ",\"updatedAt\":" << item.updatedAt
                  << ",\"visibility\":" << item.visibility
                  << ",\"banned\":" << (item.banned ? "true" : "false")
                  << ",\"acceptedForUse\":"
                  << (item.acceptedForUse ? "true" : "false")
                  << ",\"title\":";
        WriteJsonString(std::cout, item.title);
        std::cout << ",\"description\":";
        WriteJsonString(std::cout, item.description);
        std::cout << ",\"metadata\":";
        WriteJsonString(std::cout, item.metadata);
        std::cout << ",\"previewUrl\":";
        WriteJsonString(std::cout, item.previewUrl);
        std::cout << ",\"fileSize\":" << item.fileSize
                  << ",\"score\":" << item.score
                  << ",\"statistics\":{\"subscriptions\":"
                  << item.subscriptions << ",\"favorites\":"
                  << item.favorites << ",\"views\":" << item.views
                  << ",\"comments\":" << item.comments
                  << "},\"tags\":[";
        for (std::size_t tag = 0; tag < item.tags.size(); ++tag)
        {
            if (tag) std::cout << ',';
            WriteJsonString(std::cout, item.tags[tag]);
        }
        std::cout << "]}";
    }
    std::cout << "]}\n";
    return 0;
}

int PublishWorkshopItemWithCore(const ParsedOptions& options)
{
    std::string message;
    const auto package = options.Value(L"--package");
    if (!package)
        return PrintError(kInvalidArguments, "invalid_arguments",
            "workshop publish requires --package");
    snowdesktop::steam_bridge::PublishRequest request;
    request.package = *package;
    if (const auto item = options.Value(L"--item"))
    {
        std::uint64_t value = 0;
        if (!ParsePositiveInteger(*item, value))
            return PrintError(kInvalidArguments, "invalid_arguments",
                "--item must be a positive Workshop PublishedFileId");
        request.publishedFileId = value;
    }
    if (const auto preview = options.Value(L"--preview"))
        request.preview = *preview;
    if (const auto title = options.Value(L"--title"))
        request.title = WideToUtf8(*title);
    if (const auto description = options.Value(L"--description"))
        request.description = WideToUtf8(*description);
    if (const auto metadata = options.Value(L"--metadata"))
        request.metadata = WideToUtf8(*metadata);
    else request.metadata =
        "{\"format\":\"snowdesktop-widget\",\"artifact\":\"package.snowwidget\",\"protocolVersion\":1}";
    if (const auto language = options.Value(L"--language"))
        request.language = WideToUtf8(*language);
    if (options.repeated.contains(L"--tag"))
    {
        request.tags = std::vector<std::string>{};
        for (const auto& tag : options.Values(L"--tag"))
            request.tags->push_back(WideToUtf8(tag));
    }
    if (const auto visibilityValue = options.Value(L"--visibility"))
    {
        const auto visibility = ParseVisibility(visibilityValue, message);
        if (!visibility)
            return PrintError(kInvalidArguments, "invalid_arguments", message);
        request.visibility = static_cast<int>(*visibility);
    }
    if (const auto note = options.Value(L"--change-note"))
        request.changeNote = WideToUtf8(*note);
    const int timeout = ReadTimeoutSeconds(options, 1800, 7200, message);
    if (timeout == 0)
        return PrintError(kInvalidArguments, "invalid_arguments", message);
    request.timeout = std::chrono::seconds(timeout);
    snowdesktop::steam_bridge::SteamWorkshopCore core;
    snowdesktop::steam_bridge::CoreError error;
    const auto result = core.Publish(request,
        [](const snowdesktop::steam_bridge::PublishProgress& progress)
        {
            std::cout << "{\"event\":\"publish-progress\",\"stage\":";
            WriteJsonString(std::cout,
                snowdesktop::steam_bridge::PublishStageName(progress.stage));
            std::cout << ",\"publishedFileId\":\""
                      << progress.publishedFileId << "\",\"status\":"
                      << progress.steamStatus << ",\"processed\":"
                      << progress.processed << ",\"total\":"
                      << progress.total << ",\"submitStarted\":"
                      << (progress.submitStarted ? "true" : "false")
                      << "}\n" << std::flush;
        }, error);
    if (!result) return PrintError(error.exitCode, error.code, error.message);
    if (options.HasFlag(L"--open-page"))
    {
        OpenCommunityItem(result->publishedFileId, result->communityUrl);
    }
    std::cout << "{\"ok\":true,\"publishedFileId\":\""
              << result->publishedFileId << "\",\"created\":"
              << (result->created ? "true" : "false")
              << ",\"needsLegalAgreement\":"
              << (result->needsLegalAgreement ? "true" : "false")
              << ",\"communityUrl\":";
    WriteJsonString(std::cout, result->communityUrl);
    std::cout << "}\n";
    return 0;
}

int SetWorkshopSubscription(const ParsedOptions& options, bool subscribed)
{
    std::uint64_t publishedFileId = 0;
    std::string message;
    if (!ReadItemId(options, publishedFileId, message))
        return PrintError(kInvalidArguments, "invalid_arguments", message);
    const int timeout = ReadTimeoutSeconds(options, 30, 300, message);
    if (timeout == 0)
        return PrintError(kInvalidArguments, "invalid_arguments", message);

    snowdesktop::steam_bridge::SteamWorkshopCore core;
    snowdesktop::steam_bridge::CoreError error;
    if (!core.SetSubscribed(publishedFileId, subscribed,
            std::chrono::seconds(timeout), error))
        return PrintError(error.exitCode, error.code, error.message);

    std::cout << "{\"ok\":true,\"publishedFileId\":\""
              << publishedFileId << "\",\"subscribed\":"
              << (subscribed ? "true" : "false") << "}\n";
    return 0;
}

int RunWorkshopCommand(const std::wstring& command,
    const std::vector<std::wstring>& arguments)
{
    ParsedOptions options;
    std::string error;
    if (command == L"list-subscribed")
    {
        if (!ParseOptions(arguments, {}, {}, { L"--details" },
            options, error))
            return PrintError(kInvalidArguments,
                "invalid_arguments", error);
        return ListSubscribedWorkshopItems(options);
    }
    if (command == L"list-published")
    {
        if (!ParseOptions(arguments, { L"--page" }, {}, {}, options, error))
            return PrintError(kInvalidArguments, "invalid_arguments", error);
        return ListPublishedWorkshopItems(options);
    }
    if (command == L"item-details" || command == L"item-state" ||
        command == L"install-info")
    {
        if (!ParseOptions(arguments, { L"--item" }, {}, {},
            options, error))
            return PrintError(kInvalidArguments,
                "invalid_arguments", error);
        if (command == L"item-details") return PrintItemDetails(options);
        if (command == L"item-state") return PrintItemState(options);
        return PrintItemInstallInfo(options);
    }
    if (command == L"download")
    {
        if (!ParseOptions(arguments,
            { L"--item", L"--timeout-seconds" }, {},
            { L"--high-priority" }, options, error))
            return PrintError(kInvalidArguments,
                "invalid_arguments", error);
        return DownloadWorkshopItem(options);
    }
    if (command == L"subscribe" || command == L"unsubscribe")
    {
        if (!ParseOptions(arguments,
            { L"--item", L"--timeout-seconds" }, {}, {}, options, error))
            return PrintError(kInvalidArguments,
                "invalid_arguments", error);
        return SetWorkshopSubscription(options, command == L"subscribe");
    }
    if (command == L"eula-status")
    {
        if (!arguments.empty())
            return PrintError(kInvalidArguments, "invalid_arguments",
                "workshop eula-status accepts no options");
        return PrintWorkshopEulaStatus();
    }
    if (command == L"component-plan" || command == L"component-publish")
    {
        const bool execute = command == L"component-publish";
        std::set<std::wstring> flags = {
            L"--clear-tags", L"--force-content"
        };
        if (execute)
        {
            flags.insert(L"--confirm-create");
            flags.insert(L"--confirm-update");
            flags.insert(L"--open-page");
        }
        if (!ParseOptions(arguments,
            { L"--source", L"--data-directory", L"--item",
              L"--text-source", L"--preview-source",
              L"--tags-source", L"--preview", L"--title",
              L"--description", L"--visibility", L"--change-note",
              L"--timeout-seconds" },
            { L"--tag" }, flags, options, error))
            return PrintError(kInvalidArguments,
                "invalid_arguments", error);
        return RunComponentWorkshopCommand(options, execute);
    }
    if (command == L"publish")
    {
        if (!ParseOptions(arguments,
            { L"--package", L"--item", L"--preview", L"--title",
              L"--description", L"--metadata", L"--language",
              L"--visibility", L"--change-note", L"--timeout-seconds" },
            { L"--tag" }, { L"--open-page" }, options, error))
            return PrintError(kInvalidArguments,
                "invalid_arguments", error);
        return PublishWorkshopItemWithCore(options);
    }
    return PrintError(kInvalidArguments, "unknown_command",
        "unknown or incomplete Workshop command");
}
#else
int PrintStatus()
{
    std::cout
        << "{\"ok\":false,\"protocolVersion\":1,"
           "\"steamworksCompiled\":false,\"initialized\":false,"
           "\"expectedAppId\":"
        << snowdesktop::steam_bridge::kSteamAppId << ','
        << "\"reason\":\"external Steamworks SDK was not configured\"}\n";
    return kSteamworksUnavailable;
}

int RunWorkshopCommand(const std::wstring&,
    const std::vector<std::wstring>&)
{
    return PrintError(kSteamworksUnavailable, "steamworks_unavailable",
        "Configure SNOWDESKTOP_STEAMWORKS_SDK_ROOT with an external Steamworks SDK and rebuild");
}
#endif
}

int wmain(int argc, wchar_t* argv[])
{
    if (argc == 1)
    {
        PrintUsage();
        return 0;
    }

    const std::wstring_view command = argv[1];
    if (command == L"--help" || command == L"-h")
    {
        PrintUsage();
        return 0;
    }
    if (command == L"--version")
    {
        std::cout << SNOWDESKTOP_VERSION << '\n';
        return 0;
    }
    if (command == L"configuration" && argc == 2)
        return PrintConfiguration();
    if (command == L"status" && argc == 2)
        return PrintStatus();
    if (command == L"workshop" && argc >= 3)
    {
        std::vector<std::wstring> arguments;
        arguments.reserve(static_cast<std::size_t>(argc - 3));
        for (int index = 3; index < argc; ++index)
            arguments.emplace_back(argv[index]);
        return RunWorkshopCommand(argv[2], arguments);
    }

    PrintUsage();
    return PrintError(kInvalidArguments, "unknown_command",
        "unknown or incomplete command");
}
