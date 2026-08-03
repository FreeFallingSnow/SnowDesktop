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

#include "steam_workshop_core.h"

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
        << "  SnowDesktopSteamBridge.exe status\n"
        << "  SnowDesktopSteamBridge.exe workshop list-subscribed [--details]\n"
        << "  SnowDesktopSteamBridge.exe workshop list-published [--page N]\n"
        << "  SnowDesktopSteamBridge.exe workshop item-details --item ID\n"
        << "  SnowDesktopSteamBridge.exe workshop item-state --item ID\n"
        << "  SnowDesktopSteamBridge.exe workshop install-info --item ID\n"
        << "  SnowDesktopSteamBridge.exe workshop download --item ID"
           " [--high-priority] [--timeout-seconds N]\n"
        << "  SnowDesktopSteamBridge.exe workshop eula-status\n"
        << "  SnowDesktopSteamBridge.exe workshop publish --package FILE"
           " [--item ID] [--preview FILE] [--title TEXT]"
           " [--description TEXT] [--tag TAG] [--metadata JSON]"
           " [--language STEAM_LANGUAGE] [--visibility private|friends|public|unlisted]"
           " [--change-note TEXT] [--timeout-seconds N] [--open-page]\n\n"
        << "All successful command output is JSON. Long-running download and"
           " publish commands emit JSON Lines progress events before the final result.\n";
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
            error_ = message[0] ? message : "SteamAPI_InitEx failed";
    }

    ~SteamApiSession()
    {
        if (initialized_) SteamAPI_Shutdown();
    }

    SteamApiSession(const SteamApiSession&) = delete;
    SteamApiSession& operator=(const SteamApiSession&) = delete;

    [[nodiscard]] bool IsInitialized() const noexcept { return initialized_; }
    [[nodiscard]] const std::string& Error() const noexcept { return error_; }

private:
    bool initialized_ = false;
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
    return PrintError(kSteamInitializationFailed, "steam_init_failed",
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
                 "\"appId\":" << utils->GetAppID()
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
    if (!ugc || !utils)
        return PrintError(kSteamInitializationFailed,
            "steam_ugc_unavailable", "Steam UGC interfaces are unavailable");

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
        std::cout << "{\"publishedFileId\":\""
                  << static_cast<std::uint64_t>(itemId) << "\",";
        WriteItemState(std::cout, state);
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
    if (status.m_eResult != k_EResultOK)
        return PrintError(kSteamOperationFailed, "eula_query_failed",
            ResultName(status.m_eResult));
    std::cout << "{\"ok\":true,\"appId\":" << status.m_nAppID
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
        const auto url = Utf8ToWide(result->communityUrl);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr,
            SW_SHOWNORMAL);
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
    if (command == L"eula-status")
    {
        if (!arguments.empty())
            return PrintError(kInvalidArguments, "invalid_arguments",
                "workshop eula-status accepts no options");
        return PrintWorkshopEulaStatus();
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
           "\"reason\":\"external Steamworks SDK was not configured\"}\n";
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
