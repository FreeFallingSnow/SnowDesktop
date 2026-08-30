// SPDX-FileCopyrightText: 2026 SnowDesktop contributors
// SPDX-License-Identifier: MIT

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "steam_app_identity.h"
#include "steam_workshop_core.h"
#include "publish_lifecycle.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <system_error>
#include <thread>
#include <utility>

#if SNOWDESKTOP_HAS_STEAMWORKS
#include <steam/steam_api.h>
#endif

namespace snowdesktop::steam_bridge
{
namespace
{
constexpr std::uint64_t kMaximumPackageBytes = 20ull * 1024ull * 1024ull;
constexpr std::uint64_t kMaximumPreviewBytes = 1024ull * 1024ull - 1ull;

std::filesystem::path ExecutableDirectory()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(),
        static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return std::filesystem::path(path).parent_path();
}

std::string WideToUtf8(std::wstring_view value)
{
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr,
        nullptr);
    if (length <= 0) return {};
    std::string result(static_cast<std::size_t>(length), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), length, nullptr,
        nullptr) != length)
        return {};
    return result;
}

void SetError(CoreError& error, int exitCode, std::string code,
    std::string message)
{
    error.exitCode = exitCode;
    error.code = std::move(code);
    error.message = std::move(message);
}

bool ValidateFile(const std::filesystem::path& path, bool preview,
    CoreError& error)
{
    std::error_code ec;
    if (!std::filesystem::is_regular_file(path, ec) || ec)
    {
        SetError(error, kInvalidArguments,
            preview ? "invalid_preview" : "invalid_package",
            preview ? "preview file does not exist" :
                "component package does not exist");
        return false;
    }
    const std::uint64_t size = std::filesystem::file_size(path, ec);
    if (ec || size == 0)
    {
        SetError(error, kInvalidArguments,
            preview ? "invalid_preview" : "invalid_package",
            preview ? "preview file is empty or unreadable" :
                "component package is empty or unreadable");
        return false;
    }
    std::wstring extension = path.extension().wstring();
    std::transform(extension.begin(), extension.end(), extension.begin(),
        towlower);
    if (preview)
    {
        if (extension != L".png" && extension != L".jpg" &&
            extension != L".jpeg" && extension != L".gif")
        {
            SetError(error, kInvalidArguments, "invalid_preview",
                "Steam Workshop previews must be PNG, JPG, or GIF");
            return false;
        }
        if (size > kMaximumPreviewBytes)
        {
            SetError(error, kInvalidArguments, "invalid_preview",
                "Steam Workshop preview must be smaller than 1 MiB");
            return false;
        }
    }
    else if (extension != L".snowwidget" || size > kMaximumPackageBytes)
    {
        SetError(error, kInvalidArguments, "invalid_package",
            extension != L".snowwidget" ?
                "package must be a .snowwidget artifact" :
                "component package exceeds the 20 MiB format limit");
        return false;
    }
    return true;
}

#if SNOWDESKTOP_HAS_STEAMWORKS
std::string ResultName(EResult result)
{
    switch (result)
    {
    case k_EResultOK: return "ok";
    case k_EResultFail: return "fail";
    case k_EResultNoConnection: return "no_connection";
    case k_EResultInvalidParam: return "invalid_param";
    case k_EResultBusy: return "busy";
    case k_EResultInvalidState: return "invalid_state";
    case k_EResultAccessDenied: return "access_denied";
    case k_EResultTimeout: return "timeout";
    case k_EResultServiceUnavailable: return "service_unavailable";
    case k_EResultNotLoggedOn: return "not_logged_on";
    case k_EResultInsufficientPrivilege: return "insufficient_privilege";
    case k_EResultLimitExceeded: return "limit_exceeded";
    case k_EResultRateLimitExceeded: return "rate_limit_exceeded";
    default:
        return "steam_error_" +
            std::to_string(static_cast<int>(result));
    }
}

template<typename Result, typename Tick>
bool WaitForCall(SteamAPICall_t call, Result& result,
    std::chrono::milliseconds timeout, Tick&& tick, std::string& error)
{
    if (call == k_uAPICallInvalid)
    {
        error = "Steam rejected the asynchronous operation";
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        SteamAPI_RunCallbacks();
        bool failed = false;
        if (SteamUtils()->IsAPICallCompleted(call, &failed))
        {
            if (failed)
            {
                error = "Steam reported an asynchronous operation failure";
                return false;
            }
            if (!SteamUtils()->GetAPICallResult(call, &result,
                    sizeof(result), result.k_iCallback, &failed) || failed)
            {
                error = "Steam did not return the expected callback result";
                return false;
            }
            return true;
        }
        tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    error = "Steam operation timed out";
    return false;
}

template<typename Result>
bool WaitForCall(SteamAPICall_t call, Result& result,
    std::chrono::milliseconds timeout, std::string& error)
{
    return WaitForCall(call, result, timeout, [] {}, error);
}

std::string QueryText(ISteamUGC& ugc, UGCQueryHandle_t query,
    std::uint32_t index,
    bool (ISteamUGC::*getter)(UGCQueryHandle_t, std::uint32_t, char*,
        std::uint32_t))
{
    std::array<char, 8192> buffer{};
    if (!(ugc.*getter)(query, index, buffer.data(),
            static_cast<std::uint32_t>(buffer.size())))
        return {};
    return buffer.data();
}

std::uint64_t QueryStatistic(ISteamUGC& ugc, UGCQueryHandle_t query,
    std::uint32_t index, EItemStatistic statistic)
{
    std::uint64_t value = 0;
    ugc.GetQueryUGCStatistic(query, index, statistic, &value);
    return value;
}

struct QueryGuard
{
    ISteamUGC* ugc = nullptr;
    UGCQueryHandle_t handle = k_UGCQueryHandleInvalid;
    ~QueryGuard()
    {
        if (ugc && handle != k_UGCQueryHandleInvalid)
            ugc->ReleaseQueryUGCRequest(handle);
    }
};

bool RequireLoggedOn(CoreError& error)
{
    ISteamUser* user = SteamUser();
    if (user && user->BLoggedOn()) return true;
    SetError(error, kSteamInitializationFailed, "steam_not_logged_on",
        "Steam is offline or the current user is not logged on");
    return false;
}

struct UploadDirectory
{
    std::filesystem::path root;
    ~UploadDirectory()
    {
        if (root.empty()) return;
        std::error_code ec;
        std::filesystem::remove(root / L"package.snowwidget", ec);
        ec.clear();
        std::filesystem::remove(root, ec);
    }
};
#endif
}

SteamWorkshopCore::SteamWorkshopCore(std::filesystem::path stagingRoot)
    : stagingRoot_(stagingRoot.empty()
        ? ExecutableDirectory() / L"data" / L"SteamWorkshop" /
            L"staging" / L"uploads"
        : std::move(stagingRoot))
{
    status_.expectedAppId = kSteamAppId;
#if SNOWDESKTOP_HAS_STEAMWORKS
    status_.compiled = true;
#else
    status_.diagnostic = "external Steamworks SDK was not configured";
#endif
}

SteamWorkshopCore::~SteamWorkshopCore()
{
#if SNOWDESKTOP_HAS_STEAMWORKS
    if (status_.initialized) SteamAPI_Shutdown();
#endif
}

bool SteamWorkshopCore::Initialize(CoreError& error)
{
    std::lock_guard lock(statusMutex_);
#if SNOWDESKTOP_HAS_STEAMWORKS
    if (status_.initialized)
    {
        SteamAPI_RunCallbacks();
        if (ISteamUser* user = SteamUser())
            status_.loggedOn = user->BLoggedOn();
        return true;
    }
    status_.compiled = true;
    status_.appId = 0;
    status_.loggedOn = false;
    status_.steamId.clear();
    SteamErrMsg message{};
    const ESteamAPIInitResult result = SteamAPI_InitEx(&message);
    status_.diagnostic = message;
    if (result != k_ESteamAPIInitResult_OK)
    {
        SetError(error, kSteamInitializationFailed,
            "steam_initialization_failed",
            status_.diagnostic.empty() ? "SteamAPI_InitEx failed" :
                status_.diagnostic);
        return false;
    }
    status_.initialized = true;
    ISteamUtils* utils = SteamUtils();
    ISteamUser* user = SteamUser();
    if (!utils || !user)
    {
        status_.diagnostic =
            "Steam interfaces were unavailable after initialization";
        SteamAPI_Shutdown();
        status_.initialized = false;
        SetError(error, kSteamInitializationFailed,
            "steam_interface_unavailable", status_.diagnostic);
        return false;
    }
    status_.appId = utils->GetAppID();
    if (!IsExpectedSteamAppId(status_.appId))
    {
        status_.diagnostic = SteamAppIdMismatchMessage(status_.appId);
        SteamAPI_Shutdown();
        status_.initialized = false;
        SetError(error, kSteamInitializationFailed,
            "steam_app_id_mismatch", status_.diagnostic);
        return false;
    }
    status_.loggedOn = user->BLoggedOn();
    status_.steamId = std::to_string(
        user->GetSteamID().ConvertToUint64());
    status_.diagnostic = status_.loggedOn ? std::string{} :
        "Steam initialized, but the current user is offline";
    return true;
#else
    status_.diagnostic = "external Steamworks SDK was not configured";
    SetError(error, kSteamworksUnavailable, "steamworks_unavailable",
        "Configure SNOWDESKTOP_STEAMWORKS_SDK_ROOT with an external Steamworks SDK and rebuild");
    return false;
#endif
}

SteamStatus SteamWorkshopCore::Status() const
{
    std::lock_guard lock(statusMutex_);
    return status_;
}

std::optional<PublishedPage> SteamWorkshopCore::ListPublished(
    std::uint32_t page, CoreError& error)
{
    if (!Initialize(error)) return std::nullopt;
    if (page == 0)
    {
        SetError(error, kInvalidArguments, "invalid_page",
            "page must be at least 1");
        return std::nullopt;
    }
#if SNOWDESKTOP_HAS_STEAMWORKS
    if (!RequireLoggedOn(error)) return std::nullopt;
    ISteamUGC* ugc = SteamUGC();
    ISteamUser* user = SteamUser();
    ISteamUtils* utils = SteamUtils();
    if (!ugc || !user || !utils)
    {
        SetError(error, kSteamInitializationFailed,
            "steam_ugc_unavailable", "Steam UGC interfaces are unavailable");
        return std::nullopt;
    }
    const AppId_t appId = static_cast<AppId_t>(kSteamAppId);
    QueryGuard guard;
    guard.ugc = ugc;
    guard.handle = ugc->CreateQueryUserUGCRequest(
        user->GetSteamID().GetAccountID(), k_EUserUGCList_Published,
        k_EUGCMatchingUGCType_Items_ReadyToUse,
        k_EUserUGCListSortOrder_LastUpdatedDesc, appId, appId, page);
    if (guard.handle == k_UGCQueryHandleInvalid)
    {
        SetError(error, kSteamOperationFailed, "query_create_failed",
            "Steam rejected the published-item query");
        return std::nullopt;
    }
    ugc->SetReturnMetadata(guard.handle, true);
    ugc->SetReturnLongDescription(guard.handle, true);
    ugc->SetReturnAdditionalPreviews(guard.handle, true);
    SteamUGCQueryCompleted_t completed{};
    std::string message;
    if (!WaitForCall(ugc->SendQueryUGCRequest(guard.handle), completed,
            std::chrono::seconds(30), message))
    {
        SetError(error, kSteamOperationTimedOut, "query_timed_out", message);
        return std::nullopt;
    }
    if (completed.m_eResult != k_EResultOK)
    {
        SetError(error, kSteamOperationFailed, "query_failed",
            ResultName(completed.m_eResult));
        return std::nullopt;
    }
    PublishedPage output;
    output.page = page;
    output.totalResults = completed.m_unTotalMatchingResults;
    output.totalPages = output.totalResults == 0 ? 0 :
        (output.totalResults + 49u) / 50u;
    output.items.reserve(completed.m_unNumResultsReturned);
    for (std::uint32_t index = 0;
         index < completed.m_unNumResultsReturned; ++index)
    {
        SteamUGCDetails_t details{};
        if (!ugc->GetQueryUGCResult(guard.handle, index, &details)) continue;
        PublishedItem item;
        item.publishedFileId = details.m_nPublishedFileId;
        item.ownerSteamId = details.m_ulSteamIDOwner;
        item.creatorAppId = details.m_nCreatorAppID;
        item.consumerAppId = details.m_nConsumerAppID;
        item.createdAt = details.m_rtimeCreated;
        item.updatedAt = details.m_rtimeUpdated;
        item.visibility = static_cast<int>(details.m_eVisibility);
        item.banned = details.m_bBanned;
        item.acceptedForUse = details.m_bAcceptedForUse;
        item.title = details.m_rgchTitle;
        item.description = details.m_rgchDescription;
        item.fileSize = details.m_nFileSize;
        item.score = details.m_flScore;
        item.metadata = QueryText(*ugc, guard.handle, index,
            &ISteamUGC::GetQueryUGCMetadata);
        item.previewUrl = QueryText(*ugc, guard.handle, index,
            &ISteamUGC::GetQueryUGCPreviewURL);
        item.subscriptions = QueryStatistic(*ugc, guard.handle, index,
            k_EItemStatistic_NumSubscriptions);
        item.favorites = QueryStatistic(*ugc, guard.handle, index,
            k_EItemStatistic_NumFavorites);
        item.views = QueryStatistic(*ugc, guard.handle, index,
            k_EItemStatistic_NumUniqueWebsiteViews);
        item.comments = QueryStatistic(*ugc, guard.handle, index,
            k_EItemStatistic_NumComments);
        const std::uint32_t tagCount =
            ugc->GetQueryUGCNumTags(guard.handle, index);
        if (tagCount > 0)
        {
            for (std::uint32_t tagIndex = 0; tagIndex < tagCount; ++tagIndex)
            {
                std::array<char, 256> tag{};
                if (ugc->GetQueryUGCTag(guard.handle, index, tagIndex,
                        tag.data(), static_cast<std::uint32_t>(tag.size())))
                    item.tags.emplace_back(tag.data());
            }
        }
        output.items.push_back(std::move(item));
    }
    return output;
#else
    (void)page;
    return std::nullopt;
#endif
}

std::optional<WorkshopEulaStatus> SteamWorkshopCore::GetEulaStatus(
    CoreError& error)
{
    if (!Initialize(error)) return std::nullopt;
#if SNOWDESKTOP_HAS_STEAMWORKS
    if (!RequireLoggedOn(error)) return std::nullopt;
    ISteamUGC* ugc = SteamUGC();
    if (!ugc)
    {
        SetError(error, kSteamInitializationFailed,
            "steam_ugc_unavailable", "ISteamUGC is unavailable");
        return std::nullopt;
    }
    WorkshopEULAStatus_t result{};
    std::string message;
    if (!WaitForCall(ugc->GetWorkshopEULAStatus(), result,
            std::chrono::seconds(30), message))
    {
        SetError(error, kSteamOperationTimedOut, "eula_query_failed", message);
        return std::nullopt;
    }
    if (result.m_eResult == k_EResultInvalidParam)
    {
        // GetWorkshopEULAStatus has no caller-supplied parameters. After the
        // App ID has already been validated, InvalidParam means Steam has no
        // app-specific Workshop EULA status available for this application.
        // Publishing callbacks remain authoritative for required agreement
        // actions, so this optional preflight query must not block creators.
        return WorkshopEulaStatus{};
    }
    if (result.m_eResult != k_EResultOK)
    {
        SetError(error, kSteamOperationFailed, "eula_query_failed",
            ResultName(result.m_eResult));
        return std::nullopt;
    }
    return WorkshopEulaStatus{ true, result.m_bAccepted,
        result.m_bNeedsAction, result.m_unVersion, result.m_rtAction };
#else
    return std::nullopt;
#endif
}

bool SteamWorkshopCore::SetSubscribed(std::uint64_t publishedFileId,
    bool subscribed, std::chrono::seconds timeout, CoreError& error)
{
    if (publishedFileId == 0)
    {
        SetError(error, kInvalidArguments, "invalid_item",
            "PublishedFileId must be positive");
        return false;
    }
    if (timeout <= std::chrono::seconds::zero())
    {
        SetError(error, kInvalidArguments, "invalid_timeout",
            "timeout must be positive");
        return false;
    }
    if (!Initialize(error)) return false;
#if SNOWDESKTOP_HAS_STEAMWORKS
    if (!RequireLoggedOn(error)) return false;
    ISteamUGC* ugc = SteamUGC();
    if (!ugc)
    {
        SetError(error, kSteamInitializationFailed,
            "steam_ugc_unavailable", "ISteamUGC is unavailable");
        return false;
    }

    std::string message;
    PublishedFileId_t itemId =
        static_cast<PublishedFileId_t>(publishedFileId);
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    auto remaining = [&]()
    {
        const auto value = std::chrono::duration_cast<
            std::chrono::milliseconds>(deadline -
                std::chrono::steady_clock::now());
        return std::max(value, std::chrono::milliseconds::zero());
    };

    QueryGuard guard;
    guard.ugc = ugc;
    guard.handle = ugc->CreateQueryUGCDetailsRequest(&itemId, 1);
    if (guard.handle == k_UGCQueryHandleInvalid)
    {
        SetError(error, kSteamOperationFailed, "item_query_create_failed",
            "Steam rejected the Workshop item identity query");
        return false;
    }
    SteamUGCQueryCompleted_t queryCompleted{};
    if (remaining() == std::chrono::milliseconds::zero() ||
        !WaitForCall(ugc->SendQueryUGCRequest(guard.handle), queryCompleted,
            remaining(), message))
    {
        SetError(error,
            message == "Steam operation timed out" || message.empty()
                ? kSteamOperationTimedOut : kSteamOperationFailed,
            "item_query_failed", message.empty()
                ? "Steam operation timed out" : message);
        return false;
    }
    SteamUGCDetails_t details{};
    if (queryCompleted.m_eResult != k_EResultOK ||
        queryCompleted.m_unNumResultsReturned != 1 ||
        !ugc->GetQueryUGCResult(guard.handle, 0, &details) ||
        details.m_eResult != k_EResultOK ||
        details.m_nPublishedFileId != itemId)
    {
        SetError(error, kSteamOperationFailed, "item_query_failed",
            queryCompleted.m_eResult == k_EResultOK
                ? "Steam did not return the requested Workshop item"
                : ResultName(queryCompleted.m_eResult));
        return false;
    }
    if (!IsExpectedSteamAppId(details.m_nConsumerAppID))
    {
        SetError(error, kInvalidArguments, "item_app_id_mismatch",
            "Workshop item does not belong to SnowDesktop");
        return false;
    }

    EResult resultCode = k_EResultFail;
    PublishedFileId_t resultItemId = 0;
    bool completed = false;
    if (subscribed)
    {
        RemoteStorageSubscribePublishedFileResult_t result{};
        completed = remaining() != std::chrono::milliseconds::zero() &&
            WaitForCall(ugc->SubscribeItem(itemId), result,
                remaining(), message);
        resultCode = result.m_eResult;
        resultItemId = result.m_nPublishedFileId;
    }
    else
    {
        RemoteStorageUnsubscribePublishedFileResult_t result{};
        completed = remaining() != std::chrono::milliseconds::zero() &&
            WaitForCall(ugc->UnsubscribeItem(itemId), result,
                remaining(), message);
        resultCode = result.m_eResult;
        resultItemId = result.m_nPublishedFileId;
    }
    const char* operation = subscribed ? "subscribe" : "unsubscribe";
    if (!completed)
    {
        if (message.empty()) message = "Steam operation timed out";
        SetError(error,
            message == "Steam operation timed out" ?
                kSteamOperationTimedOut : kSteamOperationFailed,
            std::string(operation) + "_failed", message);
        return false;
    }
    if (resultCode != k_EResultOK)
    {
        SetError(error, kSteamOperationFailed,
            std::string(operation) + "_failed", ResultName(resultCode));
        return false;
    }
    if (resultItemId != itemId)
    {
        SetError(error, kSteamOperationFailed,
            std::string(operation) + "_failed",
            "Steam returned a result for a different Workshop item");
        return false;
    }
    return true;
#else
    (void)subscribed;
    (void)timeout;
    return false;
#endif
}

std::optional<PublishResult> SteamWorkshopCore::Publish(
    const PublishRequest& request, const PublishProgressCallback& progress,
    CoreError& error)
{
    if (!ValidateFile(request.package, false, error)) return std::nullopt;
    if (request.preview && !ValidateFile(*request.preview, true, error))
        return std::nullopt;
    const bool creating = !request.publishedFileId.has_value();
    PublishLifecycle lifecycle;
    lifecycle.Begin(creating);
    if (creating && request.title.empty())
    {
        SetError(error, kInvalidArguments, "title_required",
            "creating a Workshop item requires an initial title");
        return std::nullopt;
    }
    if (creating && !request.preview)
    {
        SetError(error, kInvalidArguments, "preview_required",
            "creating a Workshop item requires a primary preview");
        return std::nullopt;
    }
    if (request.title.size() >= 129 ||
        (request.description && request.description->size() >= 8000) ||
        (request.metadata && request.metadata->size() >= 5000))
    {
        SetError(error, kInvalidArguments, "metadata_too_large",
            "title, description, or metadata exceeds Steam Workshop limits");
        return std::nullopt;
    }
    if (progress) progress(PublishProgress{ PublishStage::Preparing });
    if (!Initialize(error)) return std::nullopt;
#if SNOWDESKTOP_HAS_STEAMWORKS
    if (!RequireLoggedOn(error)) return std::nullopt;
    std::error_code ec;
    UploadDirectory upload;
    std::filesystem::create_directories(stagingRoot_, ec);
    const DWORD stagingAttributes = GetFileAttributesW(stagingRoot_.c_str());
    if (ec || stagingRoot_.empty() ||
        stagingAttributes == INVALID_FILE_ATTRIBUTES ||
        (stagingAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (stagingAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    {
        SetError(error, kSteamOperationFailed, "staging_failed",
            "cannot prepare the data Workshop upload directory");
        return std::nullopt;
    }
    upload.root = stagingRoot_ /
        (L"upload-" + std::to_wstring(GetCurrentProcessId()) +
         L"-" + std::to_wstring(std::chrono::steady_clock::now()
             .time_since_epoch().count()));
    std::filesystem::create_directory(upload.root, ec);
    if (ec)
    {
        SetError(error, kSteamOperationFailed, "staging_failed",
            "cannot create the temporary Workshop upload directory");
        return std::nullopt;
    }
    std::filesystem::copy_file(request.package,
        upload.root / L"package.snowwidget",
        std::filesystem::copy_options::none, ec);
    if (ec)
    {
        SetError(error, kSteamOperationFailed, "staging_failed",
            "cannot stage package.snowwidget");
        return std::nullopt;
    }
    ISteamUGC* ugc = SteamUGC();
    ISteamUtils* utils = SteamUtils();
    if (!ugc || !utils)
    {
        SetError(error, kSteamInitializationFailed,
            "steam_ugc_unavailable", "Steam UGC interfaces are unavailable");
        return std::nullopt;
    }
    const AppId_t appId = static_cast<AppId_t>(kSteamAppId);
    PublishedFileId_t itemId = request.publishedFileId.value_or(0);
    bool needsAgreement = false;
    std::string message;
    if (creating)
    {
        CreateItemResult_t created{};
        if (!WaitForCall(ugc->CreateItem(appId, k_EWorkshopFileTypeCommunity),
                created, std::chrono::seconds(60), message))
        {
            SetError(error, kSteamOperationFailed, "create_item_failed", message);
            return std::nullopt;
        }
        if (created.m_eResult != k_EResultOK)
        {
            SetError(error, kSteamOperationFailed, "create_item_failed",
                ResultName(created.m_eResult));
            return std::nullopt;
        }
        itemId = created.m_nPublishedFileId;
        lifecycle.ItemCreated(static_cast<std::uint64_t>(itemId));
        needsAgreement = created.m_bUserNeedsToAcceptWorkshopLegalAgreement;
        if (progress)
            progress(PublishProgress{ PublishStage::Created,
                static_cast<std::uint64_t>(itemId) });
    }
    else lifecycle.BindExisting(static_cast<std::uint64_t>(itemId));
    const UGCUpdateHandle_t update = ugc->StartItemUpdate(appId, itemId);
    if (update == k_UGCUpdateHandleInvalid)
    {
        SetError(error, kSteamOperationFailed, "update_start_failed",
            "Steam returned an invalid Workshop update handle");
        return std::nullopt;
    }
    const std::string contentPath = WideToUtf8(upload.root.wstring());
    if (!ugc->SetItemContent(update, contentPath.c_str()))
    {
        SetError(error, kSteamOperationFailed, "content_rejected",
            "Steam rejected the Workshop content directory");
        return std::nullopt;
    }
    if (request.preview)
    {
        const std::string path = WideToUtf8(request.preview->wstring());
        if (!ugc->SetItemPreview(update, path.c_str()))
        {
            SetError(error, kSteamOperationFailed, "preview_rejected",
                "Steam rejected the Workshop preview file");
            return std::nullopt;
        }
    }
    if (creating)
    {
        if (!ugc->SetItemTitle(update, request.title.c_str()) ||
            !ugc->SetItemVisibility(update,
                k_ERemoteStoragePublishedFileVisibilityPrivate))
        {
            SetError(error, kSteamOperationFailed, "initial_fields_rejected",
                "Steam rejected the initial title or private visibility");
            return std::nullopt;
        }
        if (request.description && !ugc->SetItemDescription(update,
                request.description->c_str()))
        {
            SetError(error, kSteamOperationFailed, "description_rejected",
                "Steam rejected the initial description");
            return std::nullopt;
        }
    }
    if (request.language && !ugc->SetItemUpdateLanguage(update,
            request.language->c_str()))
    {
        SetError(error, kSteamOperationFailed, "language_rejected",
            "Steam rejected the Workshop language");
        return std::nullopt;
    }
    if (request.metadata && !ugc->SetItemMetadata(update,
            request.metadata->c_str()))
    {
        SetError(error, kSteamOperationFailed, "metadata_rejected",
            "Steam rejected the Workshop metadata");
        return std::nullopt;
    }
    if (!creating && request.visibility && !ugc->SetItemVisibility(update,
            static_cast<ERemoteStoragePublishedFileVisibility>(
                *request.visibility)))
    {
        SetError(error, kSteamOperationFailed, "visibility_rejected",
            "Steam rejected the Workshop visibility");
        return std::nullopt;
    }
    if (request.tags)
    {
        if (request.tags->size() > 100)
        {
            SetError(error, kInvalidArguments, "invalid_tags",
                "at most 100 Workshop tags are allowed");
            return std::nullopt;
        }
        for (const auto& tag : *request.tags)
        {
            if (tag.empty() || tag.size() > 255)
            {
                SetError(error, kInvalidArguments, "invalid_tags",
                    "Workshop tags must contain 1 to 255 UTF-8 bytes");
                return std::nullopt;
            }
        }
        std::vector<const char*> pointers;
        for (const auto& tag : *request.tags) pointers.push_back(tag.c_str());
        SteamParamStringArray_t values{ pointers.data(),
            static_cast<std::int32_t>(pointers.size()) };
        if (!ugc->SetItemTags(update, &values))
        {
            SetError(error, kSteamOperationFailed, "tags_rejected",
                "Steam rejected the Workshop tags");
            return std::nullopt;
        }
    }
    SubmitItemUpdateResult_t submitted{};
    auto nextProgress = std::chrono::steady_clock::now();
    const auto call = ugc->SubmitItemUpdate(update,
        request.changeNote.empty() ? nullptr : request.changeNote.c_str());
    lifecycle.SubmitStarted();
    if (progress)
        progress(PublishProgress{ PublishStage::CommittingChanges,
            static_cast<std::uint64_t>(itemId), 0, 0, 0, true });
    const bool completed = WaitForCall(call, submitted, request.timeout, [&]
    {
        if (!progress || std::chrono::steady_clock::now() < nextProgress) return;
        std::uint64_t processed = 0;
        std::uint64_t total = 0;
        const EItemUpdateStatus status = ugc->GetItemUpdateProgress(
            update, &processed, &total);
        PublishStage stage = PublishStage::CommittingChanges;
        if (status == k_EItemUpdateStatusUploadingContent)
            stage = PublishStage::UploadingContent;
        else if (status == k_EItemUpdateStatusUploadingPreviewFile)
            stage = PublishStage::UploadingPreview;
        progress(PublishProgress{ stage, static_cast<std::uint64_t>(itemId),
            processed, total, static_cast<int>(status), true });
        nextProgress = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(250);
    }, message);
    if (!completed)
    {
        SetError(error, kSteamOperationTimedOut, "publish_timed_out", message);
        return std::nullopt;
    }
    if (submitted.m_eResult != k_EResultOK)
    {
        SetError(error, kSteamOperationFailed, "publish_failed",
            ResultName(submitted.m_eResult));
        return std::nullopt;
    }
    needsAgreement = needsAgreement ||
        submitted.m_bUserNeedsToAcceptWorkshopLegalAgreement;
    PublishResult result;
    result.created = creating;
    result.publishedFileId = itemId;
    result.needsLegalAgreement = needsAgreement;
    result.communityUrl = CommunityItemUrl(itemId);
    lifecycle.Succeed();
    if (progress)
        progress(PublishProgress{ PublishStage::Completed,
            result.publishedFileId, 0, 0, 0, true });
    return result;
#else
    (void)progress;
    return std::nullopt;
#endif
}

std::string PublishStageName(PublishStage stage)
{
    switch (stage)
    {
    case PublishStage::Preparing: return "preparing";
    case PublishStage::Created: return "created";
    case PublishStage::UploadingContent: return "uploading-content";
    case PublishStage::UploadingPreview: return "uploading-preview";
    case PublishStage::CommittingChanges: return "committing-changes";
    case PublishStage::Completed: return "completed";
    }
    return "unknown";
}

std::string CommunityItemUrl(std::uint64_t publishedFileId)
{
    return "https://steamcommunity.com/sharedfiles/filedetails/?id=" +
        std::to_string(publishedFileId);
}

}
