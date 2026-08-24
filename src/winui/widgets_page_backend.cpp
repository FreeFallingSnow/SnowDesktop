#include "pch.h"

#include "widgets_page_backend.h"
#include "widgets_page_backend_state.h"

#include "../utils.h"
#include "../widget_engine.h"
#include "../widget_permission_broker.h"
#include "../steam_workshop_sync.h"
#include "authoring_toolchain.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <cwctype>
#include <deque>
#include <mutex>
#include <set>
#include <span>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace snowdesktop::winui
{
namespace
{
using snowdesktop::widget::InstalledPackage;
using snowdesktop::widget::InvalidPackage;
using snowdesktop::widget::PackageDetails;
using snowdesktop::widget::PackageManifest;
using snowdesktop::widget::PackageSourceInfo;
using snowdesktop::widget::SteamWorkshopInstallFailure;

constexpr std::size_t kMaximumSourceResults = 50;
constexpr int kAllAgentSkillTargetsMask = 0x3F;

WidgetAgentSkillInstallState AgentSkillStateFor(
    snowdesktop::steam_bridge::SkillInstallState state) noexcept
{
    using snowdesktop::steam_bridge::SkillInstallState;
    switch (state)
    {
    case SkillInstallState::NotInstalled:
        return WidgetAgentSkillInstallState::NotInstalled;
    case SkillInstallState::UpdateAvailable:
        return WidgetAgentSkillInstallState::UpdateAvailable;
    case SkillInstallState::Current:
        return WidgetAgentSkillInstallState::Current;
    case SkillInstallState::Unavailable:
    default:
        return WidgetAgentSkillInstallState::Unavailable;
    }
}

WidgetAgentSkillTargetKind AgentSkillKindFor(
    snowdesktop::steam_bridge::AgentSkillTargetKind kind) noexcept
{
    using snowdesktop::steam_bridge::AgentSkillTargetKind;
    switch (kind)
    {
    case AgentSkillTargetKind::Codex:
        return WidgetAgentSkillTargetKind::Codex;
    case AgentSkillTargetKind::ClaudeCode:
        return WidgetAgentSkillTargetKind::ClaudeCode;
    case AgentSkillTargetKind::Cursor:
        return WidgetAgentSkillTargetKind::Cursor;
    case AgentSkillTargetKind::GitHubCopilot:
        return WidgetAgentSkillTargetKind::GitHubCopilot;
    case AgentSkillTargetKind::GeminiCli:
        return WidgetAgentSkillTargetKind::GeminiCli;
    case AgentSkillTargetKind::Shared:
    default:
        return WidgetAgentSkillTargetKind::Shared;
    }
}

int AgentSkillTargetBit(
    snowdesktop::steam_bridge::AgentSkillTargetKind kind) noexcept
{
    return 1 << static_cast<int>(kind);
}

std::wstring FormatAgentSkillCounts(
    std::wstring format, int installed, int removed, int failed)
{
    for (const int value : {installed, removed, failed})
    {
        const std::size_t marker = format.find(L"%d");
        if (marker == std::wstring::npos) break;
        format.replace(marker, 2, std::to_wstring(value));
    }
    return format;
}

std::wstring FormatLocalizedValue(
    std::wstring format, std::wstring_view value)
{
    const std::size_t marker = format.find(L"{0}");
    if (marker != std::wstring::npos)
        format.replace(marker, 3, value.data(), value.size());
    return format;
}

std::wstring TrimInstallReasonValue(std::wstring_view value)
{
    const std::size_t first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring_view::npos)
        return {};
    const std::size_t last = value.find_last_not_of(L" \t\r\n");
    return std::wstring(value.substr(first, last - first + 1));
}

std::vector<WidgetInstallConfirmationReasonSnapshot>
ParseInstallConfirmationReasons(std::wstring_view details)
{
    struct Marker
    {
        std::wstring_view text;
        WidgetInstallConfirmationReasonKind kind;
        bool permission = false;
    };
    constexpr Marker markers[]{
        {L"requests a new required permission: ",
            WidgetInstallConfirmationReasonKind::NewPermission, true},
        {L"requests a new optional permission: ",
            WidgetInstallConfirmationReasonKind::NewPermission, true},
        {L"requests a new permission: ",
            WidgetInstallConfirmationReasonKind::NewPermission, true},
        {L"requests a new network domain: ",
            WidgetInstallConfirmationReasonKind::NewWebsite},
        {L"expands network access to: ",
            WidgetInstallConfirmationReasonKind::NewWebsite},
    };

    std::vector<WidgetInstallConfirmationReasonSnapshot> result;
    const auto appendUnique = [&result](
                                  WidgetInstallConfirmationReasonSnapshot item) {
        const bool duplicate = std::any_of(result.begin(), result.end(),
            [&](const auto& existing) {
                return existing.kind == item.kind &&
                    existing.value == item.value;
            });
        if (!duplicate)
            result.push_back(std::move(item));
    };

    std::size_t offset = 0;
    while (offset < details.size())
    {
        const std::size_t delimiter = details.find(L';', offset);
        const std::wstring_view segment = details.substr(offset,
            delimiter == std::wstring_view::npos
                ? std::wstring_view::npos : delimiter - offset);
        for (const Marker& marker : markers)
        {
            const std::size_t markerOffset = segment.find(marker.text);
            if (markerOffset == std::wstring_view::npos)
                continue;
            WidgetInstallConfirmationReasonSnapshot item;
            item.kind = marker.kind;
            item.value = TrimInstallReasonValue(segment.substr(
                markerOffset + marker.text.size()));
            if (marker.permission && !item.value.empty())
            {
                if (const char* key = snowdesktop::widget::
                        WidgetPermissionLabelLocalizationKey(
                            WideToUtf8(item.value)))
                {
                    item.valueLabelKey = key;
                }
            }
            appendUnique(std::move(item));
            break;
        }
        if (segment.find(
                L"source changes require explicit confirmation") !=
            std::wstring_view::npos)
        {
            WidgetInstallConfirmationReasonSnapshot item;
            item.kind = WidgetInstallConfirmationReasonKind::SourceChange;
            appendUnique(std::move(item));
        }
        if (delimiter == std::wstring_view::npos)
            break;
        offset = delimiter + 1;
    }

    if (result.empty() && !details.empty())
    {
        WidgetInstallConfirmationReasonSnapshot item;
        item.kind = WidgetInstallConfirmationReasonKind::Other;
        result.push_back(std::move(item));
    }
    return result;
}

struct SourceQueryRecord
{
    PackageSourceInfo source;
    std::vector<PackageDetails> results;
    std::string error;
};

struct SourceSearchResult
{
    std::vector<SourceQueryRecord> sources;
    std::string error;
    bool localizeError = false;
    bool cancelled = false;
};

struct SourceSearchWork
{
    std::uint64_t generation = 0;
    std::uint64_t activation = 0;
    std::uint64_t taskId = 0;
    std::uint64_t searchRevision = 0;
    std::wstring query;
    std::string locale;
    std::vector<InstalledPackage> installed;
    std::shared_ptr<std::atomic_bool> cancellation;
    std::function<void(SourceSearchResult)> completion;
};

bool IsBuiltinSource(std::string_view sourceId) noexcept
{
    return sourceId == "builtin";
}

bool IsManagedPackage(const InstalledPackage& package) noexcept
{
    return !package.builtin && !package.development;
}

std::wstring SourceFallbackName(std::string_view providerId)
{
    if (providerId == "builtin") return L"Included with SnowDesktop";
    if (providerId == "local-directory") return L"Local components";
    if (providerId == "static-catalog") return L"Component catalog";
    if (providerId == "steam-workshop")
        return L"Steam Workshop subscriptions";
    if (providerId == "local" || providerId == "local-import")
        return L"Local components";
    return Utf8ToWide(std::string(providerId));
}

bool ContainsPackageId(
    const std::vector<InstalledPackage>& installed,
    std::string_view packageId,
    bool managedOnly)
{
    return std::any_of(installed.begin(), installed.end(),
        [packageId, managedOnly](const InstalledPackage& package) {
            return package.manifest.id == packageId &&
                (!managedOnly || IsManagedPackage(package));
        });
}

const InstalledPackage* ActiveManagedPackage(
    const std::vector<InstalledPackage>& installed,
    std::string_view packageId)
{
    const auto found = std::find_if(installed.begin(), installed.end(),
        [packageId](const InstalledPackage& package) {
            return package.manifest.id == packageId &&
                IsManagedPackage(package) && package.selected;
        });
    return found == installed.end() ? nullptr : &*found;
}

bool SearchTextMatches(const PackageManifest& manifest,
    std::wstring_view query)
{
    if (query.empty()) return true;
    std::wstring searchable = Utf8ToWide(manifest.name + "\n" +
        manifest.description + "\n" + manifest.author + "\n" +
        manifest.id + "\n" + manifest.version);
    std::wstring needle(query);
    std::transform(searchable.begin(), searchable.end(), searchable.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    std::transform(needle.begin(), needle.end(), needle.begin(),
        [](wchar_t value) {
            return static_cast<wchar_t>(std::towlower(value));
        });
    return searchable.find(needle) != std::wstring::npos;
}

SourceSearchResult QuerySources(SourceSearchWork& work)
{
    SourceSearchResult result;
    const auto cancelled = [&]() noexcept {
        return work.cancellation && work.cancellation->load();
    };
    try
    {
        if (cancelled())
        {
            result.cancelled = true;
            return result;
        }
        const auto sources = WidgetEngine::ListWidgetPackageSources();
        result.sources.reserve(sources.size());
        for (const PackageSourceInfo& source : sources)
        {
            if (cancelled())
            {
                result.cancelled = true;
                break;
            }
            SourceQueryRecord record;
            record.source = source;
            if (source.capabilities.query && source.status.available)
            {
                if (IsBuiltinSource(source.providerId))
                {
                    for (const InstalledPackage& package : work.installed)
                    {
                        if (!package.builtin ||
                            record.results.size() >= kMaximumSourceResults)
                        {
                            continue;
                        }
                        PackageManifest manifest =
                            snowdesktop::widget::LocalizePackageManifest(
                                package.manifest, work.locale);
                        if (!SearchTextMatches(manifest, work.query))
                            continue;
                        record.results.push_back({std::move(manifest),
                            package.source,
                            {package.manifest.version}, false});
                    }
                }
                else
                {
                    snowdesktop::widget::PackageQuery query;
                    query.text = WideToUtf8(work.query);
                    query.locale = work.locale.empty()
                        ? "en-US" : work.locale;
                    query.limit = kMaximumSourceResults;
                    record.results = WidgetEngine::QueryWidgetPackageSource(
                        source.providerId, query, record.error);
                }
            }
            if (cancelled())
            {
                result.cancelled = true;
                break;
            }
            result.sources.push_back(std::move(record));
        }
    }
    catch (const std::exception& exception)
    {
        result.error = exception.what();
    }
    catch (...)
    {
        result.localizeError = true;
    }
    return result;
}

class SourceSearchWorker final
{
public:
    SourceSearchWorker() = default;
    ~SourceSearchWorker() { Shutdown(); }

    SourceSearchWorker(const SourceSearchWorker&) = delete;
    SourceSearchWorker& operator=(const SourceSearchWorker&) = delete;

    bool Submit(SourceSearchWork work)
    {
        std::optional<SourceSearchWork> displaced;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return false;
            displaced = std::exchange(pending_, std::nullopt);
            pending_ = std::move(work);
            if (!worker_.joinable())
            {
                worker_ = std::jthread(
                    [this](std::stop_token stopToken) {
                        WorkerMain(stopToken);
                    });
            }
        }
        if (displaced && displaced->completion)
        {
            SourceSearchResult cancelled;
            cancelled.cancelled = true;
            displaced->completion(std::move(cancelled));
        }
        condition_.notify_one();
        return true;
    }

    bool RequestCancel(std::uint64_t taskId)
    {
        std::optional<SourceSearchWork> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (pending_ && pending_->taskId == taskId)
                cancelled = std::exchange(pending_, std::nullopt);
            else if (activeTaskId_ == taskId && activeCancellation_)
                activeCancellation_->store(true);
            else
                return false;
        }
        if (cancelled && cancelled->completion)
        {
            SourceSearchResult result;
            result.cancelled = true;
            cancelled->completion(std::move(result));
        }
        return true;
    }

    void Shutdown() noexcept
    {
        std::optional<SourceSearchWork> cancelled;
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return;
            stopping_ = true;
            cancelled = std::exchange(pending_, std::nullopt);
        }
        if (cancelled && cancelled->completion)
        {
            try
            {
                SourceSearchResult result;
                result.cancelled = true;
                cancelled->completion(std::move(result));
            }
            catch (...)
            {
            }
        }
        worker_.request_stop();
        condition_.notify_all();
        if (worker_.joinable()) worker_.join();
    }

private:
    void WorkerMain(std::stop_token stopToken)
    {
        while (!stopToken.stop_requested())
        {
            std::optional<SourceSearchWork> work;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, [&] {
                    return stopping_ || pending_.has_value() ||
                        stopToken.stop_requested();
                });
                if (stopping_ || stopToken.stop_requested()) return;
                work = std::exchange(pending_, std::nullopt);
                if (work)
                {
                    activeTaskId_ = work->taskId;
                    activeCancellation_ = work->cancellation;
                }
            }
            if (!work) continue;
            SourceSearchResult result = QuerySources(*work);
            {
                std::lock_guard lock(mutex_);
                if (work->cancellation && work->cancellation->load())
                    result.cancelled = true;
                if (activeTaskId_ == work->taskId)
                {
                    activeTaskId_ = 0;
                    activeCancellation_.reset();
                }
            }
            if (work->completion)
            {
                try
                {
                    work->completion(std::move(result));
                }
                catch (...)
                {
                }
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable condition_;
    std::optional<SourceSearchWork> pending_;
    std::uint64_t activeTaskId_ = 0;
    std::shared_ptr<std::atomic_bool> activeCancellation_;
    std::jthread worker_;
    bool stopping_ = false;
};

struct PackageGroup
{
    std::string id;
    const InstalledPackage* builtin = nullptr;
    const InstalledPackage* managed = nullptr;
    const InstalledPackage* development = nullptr;
    std::vector<const InvalidPackage*> invalidBuiltin;
    std::vector<const InvalidPackage*> invalidManaged;
    std::vector<const InvalidPackage*> invalidDevelopment;
    std::vector<const SteamWorkshopInstallFailure*> workshopInstallFailures;
};

std::vector<PackageGroup> GroupPackages(
    const std::vector<InstalledPackage>& packages,
    const std::vector<InvalidPackage>& invalidPackages = {},
    const std::vector<SteamWorkshopInstallFailure>& workshopFailures = {})
{
    std::vector<PackageGroup> groups;
    std::unordered_map<std::string, std::size_t> indexes;
    for (const InstalledPackage& package : packages)
    {
        if (!package.active && !package.selected && !package.development)
            continue;
        const auto [found, inserted] = indexes.emplace(
            package.manifest.id, groups.size());
        if (inserted)
            groups.push_back(PackageGroup{package.manifest.id});
        PackageGroup& group = groups[found->second];
        if (package.development)
        {
            if (!group.development || package.active)
                group.development = &package;
        }
        else if (package.builtin)
        {
            if (!group.builtin || package.active)
                group.builtin = &package;
        }
        else if (package.selected || !group.managed)
        {
            group.managed = &package;
        }
    }
    for (const InvalidPackage& package : invalidPackages)
    {
        std::string groupId = !package.packageId.empty()
            ? package.packageId : package.manifest.id;
        if (groupId.empty())
            groupId = "invalid:" + WideToUtf8(package.root.filename().wstring());
        const auto [found, inserted] = indexes.emplace(groupId, groups.size());
        if (inserted)
            groups.push_back(PackageGroup{groupId});
        PackageGroup& group = groups[found->second];
        if (package.development)
            group.invalidDevelopment.push_back(&package);
        else if (package.builtin)
            group.invalidBuiltin.push_back(&package);
        else
            group.invalidManaged.push_back(&package);
    }
    for (const SteamWorkshopInstallFailure& failure : workshopFailures)
    {
        std::string groupId = !failure.packageId.empty()
            ? failure.packageId : failure.manifest.id;
        if (groupId.empty() && !failure.externalItemId.empty())
            groupId = "workshop:" + failure.externalItemId;
        if (groupId.empty()) continue;
        const auto [found, inserted] = indexes.emplace(groupId, groups.size());
        if (inserted)
            groups.push_back(PackageGroup{groupId});
        groups[found->second].workshopInstallFailures.push_back(&failure);
    }
    return groups;
}

const InstalledPackage* DisplayPackage(const PackageGroup& group) noexcept
{
    if (group.development && group.development->active)
        return group.development;
    if (group.managed && group.managed->selected && group.managed->active)
        return group.managed;
    if (group.builtin && group.builtin->active)
        return group.builtin;
    if (group.managed && group.managed->active)
        return group.managed;
    if (group.development) return group.development;
    if (group.managed) return group.managed;
    return group.builtin;
}

std::string WorkshopExternalItemIdFor(
    const PackageGroup& group,
    const std::unordered_map<std::string, std::string>& associations)
{
    if (group.managed &&
        group.managed->source.providerId == "steam-workshop" &&
        !snowdesktop::widget::SteamPublishedFileId(
            group.managed->source.externalItemId).empty())
    {
        return group.managed->source.externalItemId;
    }
    for (const InvalidPackage* package : group.invalidManaged)
    {
        if (package && package->source.providerId == "steam-workshop" &&
            !snowdesktop::widget::SteamPublishedFileId(
                package->source.externalItemId).empty())
        {
            return package->source.externalItemId;
        }
    }
    if (!group.workshopInstallFailures.empty() &&
        group.workshopInstallFailures.front())
    {
        return group.workshopInstallFailures.front()->externalItemId;
    }
    const auto associated = associations.find(group.id);
    if (associated != associations.end() &&
        !snowdesktop::widget::SteamPublishedFileId(
            associated->second).empty())
    {
        return associated->second;
    }
    return {};
}

bool IsGranted(std::span<const std::string> granted,
    std::string_view permission)
{
    return std::find(granted.begin(), granted.end(), permission) !=
        granted.end();
}

using PackageFileIdentity =
    widgets_page_backend_detail::ReviewedPackageFileIdentity;

bool ReadFileIdentity(HANDLE handle, PackageFileIdentity& identity) noexcept
{
    FILE_ID_INFO information{};
    if (handle == INVALID_HANDLE_VALUE)
    {
        return false;
    }
    if (GetFileInformationByHandleEx(handle, FileIdInfo,
            &information, sizeof(information)))
    {
        identity.volumeSerialNumber = information.VolumeSerialNumber;
        std::memcpy(identity.fileId.data(), information.FileId.Identifier,
            identity.fileId.size());
        return true;
    }

    // Some removable filesystems do not implement FileIdInfo but do expose
    // the stable volume/index pair through the legacy handle information.
    BY_HANDLE_FILE_INFORMATION fallback{};
    if (!GetFileInformationByHandle(handle, &fallback)) return false;
    identity = {};
    identity.volumeSerialNumber = fallback.dwVolumeSerialNumber;
    const std::uint64_t fileIndex =
        (static_cast<std::uint64_t>(fallback.nFileIndexHigh) << 32) |
        fallback.nFileIndexLow;
    std::memcpy(identity.fileId.data(), &fileIndex, sizeof(fileIndex));
    return fileIndex != 0;
}

bool IsSafeDiskObject(HANDLE handle, bool directory) noexcept
{
    FILE_ATTRIBUTE_TAG_INFO information{};
    return handle != INVALID_HANDLE_VALUE &&
        GetFileType(handle) == FILE_TYPE_DISK &&
        GetFileInformationByHandleEx(handle, FileAttributeTagInfo,
            &information, sizeof(information)) &&
        (information.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        ((information.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) ==
            directory;
}

class ScopedPackageIdentityLock final
{
public:
    explicit ScopedPackageIdentityLock(
        const std::filesystem::path& path,
        const std::filesystem::path& trustedRoot,
        const std::optional<PackageFileIdentity>& expected = {},
        bool directory = false)
        : directory_(directory)
    {
        std::error_code error;
        path_ = std::filesystem::absolute(path, error).lexically_normal();
        if (error) return;
        const std::filesystem::path root =
            std::filesystem::absolute(trustedRoot, error).lexically_normal();
        if (error || path_.parent_path() != root) return;

        // Lock every lexical ancestor from the volume root down. Omitting
        // FILE_SHARE_DELETE prevents an ancestor from being renamed while a
        // validator or the package manager reopens the reviewed path.
        std::vector<std::filesystem::path> ancestors;
        for (std::filesystem::path current = path_.parent_path();
             !current.empty();)
        {
            ancestors.push_back(current);
            const std::filesystem::path parent = current.parent_path();
            if (parent == current) break;
            current = parent;
        }
        std::reverse(ancestors.begin(), ancestors.end());
        for (const std::filesystem::path& ancestor : ancestors)
        {
            HANDLE handle = CreateFileW(ancestor.c_str(), FILE_READ_ATTRIBUTES,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
                nullptr);
            if (!IsSafeDiskObject(handle, true))
            {
                if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
                Reset();
                return;
            }
            ancestorHandles_.push_back(handle);
        }

        handle_ = OpenPath();
        if (!IsSafeDiskObject(handle_, directory_) ||
            !ReadFileIdentity(handle_, identity_) ||
            (expected && identity_ != *expected))
        {
            Reset();
        }
    }

    ~ScopedPackageIdentityLock() { Reset(); }

    ScopedPackageIdentityLock(const ScopedPackageIdentityLock&) = delete;
    ScopedPackageIdentityLock& operator=(
        const ScopedPackageIdentityLock&) = delete;

    [[nodiscard]] bool Acquired() const noexcept
    {
        return handle_ != INVALID_HANDLE_VALUE;
    }

    [[nodiscard]] const PackageFileIdentity& Identity() const noexcept
    {
        return identity_;
    }

    /** Verify both the held object and a fresh path open at every IO boundary. */
    [[nodiscard]] bool MatchesPathIdentity() const noexcept
    {
        if (!Acquired() || !AncestorsRemainSafe()) return false;
        PackageFileIdentity heldIdentity;
        if (!ReadFileIdentity(handle_, heldIdentity) ||
            heldIdentity != identity_)
        {
            return false;
        }
        HANDLE reopened = OpenPath();
        PackageFileIdentity reopenedIdentity;
        const bool matches = IsSafeDiskObject(reopened, directory_) &&
            ReadFileIdentity(reopened, reopenedIdentity) &&
            reopenedIdentity == identity_;
        if (reopened != INVALID_HANDLE_VALUE) CloseHandle(reopened);
        return matches;
    }

private:
    [[nodiscard]] HANDLE OpenPath() const noexcept
    {
        return CreateFileW(path_.c_str(),
            directory_ ? FILE_READ_ATTRIBUTES : GENERIC_READ,
            FILE_SHARE_READ,
            nullptr, OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT |
                (directory_ ? FILE_FLAG_BACKUP_SEMANTICS
                            : FILE_FLAG_SEQUENTIAL_SCAN),
            nullptr);
    }

    [[nodiscard]] bool AncestorsRemainSafe() const noexcept
    {
        return !ancestorHandles_.empty() &&
            std::all_of(ancestorHandles_.begin(), ancestorHandles_.end(),
                [](HANDLE handle) { return IsSafeDiskObject(handle, true); });
    }

    void Reset() noexcept
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        for (HANDLE handle : ancestorHandles_)
            if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        ancestorHandles_.clear();
    }

    std::filesystem::path path_;
    bool directory_ = false;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::vector<HANDLE> ancestorHandles_;
    PackageFileIdentity identity_;
};

void RemoveAbandonedSettingsReviewPackages(
    const std::filesystem::path& stagingRoot) noexcept
{
    std::error_code error;
    std::filesystem::directory_iterator item(stagingRoot, error);
    const std::filesystem::directory_iterator end;
    while (!error && item != end)
    {
        const std::filesystem::path path = item->path();
        const std::wstring filename = path.filename().wstring();
        if (filename.starts_with(L"settings-review-") &&
            path.extension() == L".snowwidget")
        {
            std::error_code removeError;
            std::filesystem::remove(path, removeError);
        }
        item.increment(error);
    }
}

} // namespace

WidgetsPageHostOperationResult WidgetsPageHostOperationResult::Success(
    bool changed, std::wstring message)
{
    return {true, changed, std::move(message)};
}

WidgetsPageHostOperationResult WidgetsPageHostOperationResult::Failure(
    std::wstring message)
{
    return {false, false, std::move(message)};
}

namespace widgets_page_backend_detail
{
WidgetSourceKind SourceKindFor(std::string_view providerId) noexcept
{
    if (providerId == "builtin") return WidgetSourceKind::BuiltIn;
    if (providerId == "local" || providerId == "local-import")
        return WidgetSourceKind::Local;
    if (providerId == "static-catalog")
        return WidgetSourceKind::Catalog;
    if (providerId == "steam-workshop")
        return WidgetSourceKind::SteamWorkshop;
    if (providerId == "local-directory")
        return WidgetSourceKind::Development;
    return WidgetSourceKind::Other;
}

std::string SourceNameKeyFor(std::string_view providerId)
{
    switch (SourceKindFor(providerId))
    {
    case WidgetSourceKind::BuiltIn:
        return "app.settings.widgets_source_builtin";
    case WidgetSourceKind::Local:
    case WidgetSourceKind::Development:
        return "app.settings.widgets_source_local";
    case WidgetSourceKind::Catalog:
        return "app.settings.widgets_source_catalog";
    case WidgetSourceKind::SteamWorkshop:
        return "app.settings.widgets_source_steam";
    case WidgetSourceKind::Other:
    default:
        return {};
    }
}

WidgetPackagePermissionState PermissionStateFor(
    snowdesktop::widget::PermissionDecisionState state,
    snowdesktop::widget::PermissionRuntimeBlock runtimeBlock) noexcept
{
    using snowdesktop::widget::PermissionDecisionState;
    using snowdesktop::widget::PermissionRuntimeBlock;
    if (runtimeBlock == PermissionRuntimeBlock::MissingRequired)
        return WidgetPackagePermissionState::MissingRequired;
    switch (state)
    {
    case PermissionDecisionState::LegacyImplicit:
        return WidgetPackagePermissionState::LegacyImplicit;
    case PermissionDecisionState::Pending:
        return WidgetPackagePermissionState::Pending;
    case PermissionDecisionState::Granted:
        return WidgetPackagePermissionState::Granted;
    case PermissionDecisionState::Denied:
        return WidgetPackagePermissionState::Denied;
    default:
        return WidgetPackagePermissionState::Pending;
    }
}

WidgetPermissionRisk PermissionRiskFor(
    snowdesktop::widget::PermissionRiskClass risk) noexcept
{
    using snowdesktop::widget::PermissionRiskClass;
    switch (risk)
    {
    case PermissionRiskClass::Basic:
        return WidgetPermissionRisk::Basic;
    case PermissionRiskClass::SystemStatus:
        return WidgetPermissionRisk::SystemStatus;
    case PermissionRiskClass::PersonalData:
        return WidgetPermissionRisk::PersonalData;
    case PermissionRiskClass::ExternalCommunication:
        return WidgetPermissionRisk::ExternalCommunication;
    case PermissionRiskClass::ElevatedRead:
        return WidgetPermissionRisk::ElevatedRead;
    case PermissionRiskClass::Modification:
        return WidgetPermissionRisk::Modification;
    case PermissionRiskClass::UserScoped:
        return WidgetPermissionRisk::UserScoped;
    case PermissionRiskClass::Sensor:
        return WidgetPermissionRisk::Sensor;
    case PermissionRiskClass::Unknown:
    default:
        return WidgetPermissionRisk::Unknown;
    }
}

bool InstallFailureNeedsConfirmation(std::wstring_view message) noexcept
{
    return message.find(L"source changes require explicit confirmation") !=
            std::wstring_view::npos ||
        message.find(L"requests a new required permission") !=
            std::wstring_view::npos ||
        message.find(L"requests a new optional permission") !=
            std::wstring_view::npos ||
        message.find(L"requests a new network domain") !=
            std::wstring_view::npos;
}

bool CompletionIdentityMatches(
    std::uint64_t expectedGeneration,
    std::uint64_t expectedActivation,
    std::uint64_t expectedTaskId,
    std::uint64_t currentGeneration,
    std::uint64_t currentActivation,
    std::uint64_t currentTaskId) noexcept
{
    return expectedGeneration != 0 && expectedTaskId != 0 &&
        expectedGeneration == currentGeneration &&
        expectedActivation == currentActivation &&
        expectedTaskId == currentTaskId;
}
} // namespace widgets_page_backend_detail

struct WidgetsPageBackend::Impl final
    : std::enable_shared_from_this<WidgetsPageBackend::Impl>
{
    struct LocalPackageSnapshot
    {
        std::filesystem::path path;
        PackageManifest manifest;
        std::string sha256;
        PackageFileIdentity identity;

        ~LocalPackageSnapshot()
        {
            if (path.empty()) return;
            std::error_code error;
            std::filesystem::remove(path, error);
        }
    };

    struct PendingInstall
    {
        enum class Kind
        {
            LocalPath,
            SourceItem,
        };

        Kind kind = Kind::LocalPath;
        std::filesystem::path path;
        std::string packageId;
        std::string sourceId;
        std::string externalItemId;
        std::string version;
        std::shared_ptr<LocalPackageSnapshot> localSnapshot;
        /** Non-empty only for an immutable export of a development tree. */
        std::string developmentPackageId;
        bool developmentOverrideWasActive = false;
        bool allowSourceChange = false;
        bool allowPermissionExpansion = false;
    };

    explicit Impl(WidgetEngine& value, WidgetsPageBackendOptions valueOptions)
        : engine(value), options(std::move(valueOptions)),
          ownerThreadId(GetCurrentThreadId())
    {
        RemoveAbandonedSettingsReviewPackages(
            WidgetEngine::GetWidgetPackagePaths().staging);
        state = std::make_shared<WidgetsPageSnapshot>();
    }

    WidgetEngine& engine;
    WidgetsPageBackendOptions options;
    DWORD ownerThreadId = 0;
    SourceSearchWorker sourceWorker;

    std::shared_ptr<WidgetsPageSnapshot> state;
    std::vector<InstalledPackage> packages;
    std::vector<InvalidPackage> invalidPackages;
    std::vector<SteamWorkshopInstallFailure> workshopInstallFailures;
    std::vector<snowdesktop::steam_bridge::SkillInstallStatus>
        agentSkillStatuses;
    std::uint64_t generation = 0;
    std::uint64_t activation = 0;
    std::uint64_t revision = 0;
    std::uint64_t requestedSearchRevision = 0;
    std::wstring requestedSearchQuery;
    std::uint64_t nextTaskId = 0;
    std::uint64_t activeTaskId = 0;
    std::uint64_t pickerRequestId = 0;
    std::uint64_t confirmationRequestId = 0;
    widgets_page_backend_detail::OutstandingOperationLedger
        outstandingOperations;
    bool active = false;
    std::atomic_bool closed{false};
    bool awaitingPicker = false;
    bool awaitingConfirmation = false;
    bool deferredSourceDiscovery = false;

    [[nodiscard]] bool OnOwnerThread() const noexcept
    {
        return ownerThreadId != 0 && ownerThreadId == GetCurrentThreadId();
    }

    [[nodiscard]] std::uint64_t NextTaskId() noexcept
    {
        ++nextTaskId;
        if (nextTaskId == 0) ++nextTaskId;
        return nextTaskId;
    }

    [[nodiscard]] std::wstring L(
        std::string_view key,
        std::wstring_view fallback = {}) const
    {
        if (options.localize)
        {
            try
            {
                std::wstring value = options.localize(key);
                if (!value.empty()) return value;
            }
            catch (...)
            {
            }
        }
        return std::wstring(fallback);
    }

    [[nodiscard]] std::string Locale() const
    {
        if (options.locale)
        {
            try
            {
                std::string value = options.locale();
                if (!value.empty()) return value;
            }
            catch (...)
            {
            }
        }
        return "en-US";
    }

    int CaptureAgentSkillTargetMask() const noexcept
    {
        int mask = kAllAgentSkillTargetsMask;
        if (options.agentSkillTargetMask)
        {
            try
            {
                mask = options.agentSkillTargetMask();
            }
            catch (...)
            {
            }
        }
        return std::clamp(mask, 0, kAllAgentSkillTargetsMask);
    }

    void CaptureAgentSkillState()
    {
        agentSkillStatuses.clear();
        state->agentSkills.clear();
        state->agentSkillStatusError.clear();

        const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
        const auto bundledSkill =
            packagePaths.builtin / L"snowdesktop-lua-widget";
        const auto bundledCli = bundledSkill / L"bin" / L"snowwidget.exe";
        std::error_code workspaceError;
        std::filesystem::create_directories(
            packagePaths.development, workspaceError);
        state->developmentWorkspace = packagePaths.development.wstring();
        state->componentCliPath = bundledCli.wstring();
        state->developerPublisherAvailable = false;
        if (options.canPublishDevelopmentPackage)
        {
            try
            {
                state->developerPublisherAvailable =
                    options.canPublishDevelopmentPackage();
            }
            catch (...)
            {
            }
        }

        state->agentSkillTargetMask = CaptureAgentSkillTargetMask();
        for (auto target :
            snowdesktop::steam_bridge::DefaultAgentSkillTargets())
        {
            std::string error;
            auto status = snowdesktop::steam_bridge::InspectAgentSkill(
                bundledSkill, bundledCli, std::move(target), error);
            if (!error.empty())
            {
                if (!state->agentSkillStatusError.empty())
                    state->agentSkillStatusError += L"\n";
                state->agentSkillStatusError +=
                    Utf8ToWide(status.agent.id + ": " + error);
            }

            const int bit = AgentSkillTargetBit(status.agent.kind);
            const bool selected =
                (state->agentSkillTargetMask & bit) != 0;
            using snowdesktop::steam_bridge::SkillInstallState;
            bool installed = status.state == SkillInstallState::Current ||
                status.state == SkillInstallState::UpdateAvailable;
            if (!installed)
            {
                std::error_code pathError;
                installed = std::filesystem::exists(
                    status.target, pathError) && !pathError;
            }
            state->agentSkills.push_back({
                AgentSkillKindFor(status.agent.kind), status.agent.id,
                status.target.wstring(), AgentSkillStateFor(status.state),
                selected, installed});
            agentSkillStatuses.push_back(std::move(status));
        }
    }

    void SetFeedback(WidgetsPageFeedbackSeverity severity,
        std::wstring message, std::string titleKey = {})
    {
        state->feedback.severity = severity;
        state->feedback.titleKey = std::move(titleKey);
        state->feedback.title.clear();
        state->feedback.message = std::move(message);
    }

    void ClearFeedback()
    {
        state->feedback = {};
    }

    void Publish()
    {
        if (closed || !active) return;
        state->generation = generation;
        state->revision = ++revision;
        const auto publication =
            std::make_shared<const WidgetsPageSnapshot>(*state);
        if (options.snapshotChanged)
        {
            try
            {
                options.snapshotChanged(publication);
            }
            catch (...)
            {
            }
        }
    }

    void MarshalToOwner(std::function<void()> callback)
    {
        if (!callback || closed) return;
        if (OnOwnerThread())
        {
            callback();
            return;
        }
        if (!options.dispatchToOwner) return;
        try
        {
            (void)options.dispatchToOwner(std::move(callback));
        }
        catch (...)
        {
        }
    }

    std::vector<WidgetsPageHostInstance> CaptureInstances()
    {
        std::vector<WidgetsPageHostInstance> result;
        if (options.instances)
        {
            try
            {
                result = options.instances();
            }
            catch (...)
            {
                result.clear();
            }
        }

        std::unordered_map<std::wstring, const LuaWidget*> live;
        for (const LuaWidget& widget : engine.GetWidgets())
        {
            if (widget.preview || widget.widgetId.empty() ||
                widget.packageId.empty())
            {
                continue;
            }
            live[widget.widgetId] = &widget;
        }

        if (result.empty() && !options.instances)
        {
            result.reserve(live.size());
            for (const auto& [instanceId, widget] : live)
            {
                result.push_back({instanceId,
                    Utf8ToWide(widget->packageId),
                    Utf8ToWide(widget->name),
                    widget->valid && widget->runtimeToken != 0});
            }
        }
        else
        {
            for (WidgetsPageHostInstance& instance : result)
            {
                const auto found = live.find(instance.instanceId);
                if (found == live.end()) continue;
                if (instance.packageId.empty())
                    instance.packageId =
                        Utf8ToWide(found->second->packageId);
                if (instance.displayName.empty())
                    instance.displayName =
                        Utf8ToWide(found->second->name);
                instance.settingsAvailable =
                    instance.settingsAvailable ||
                    (found->second->valid &&
                        found->second->runtimeToken != 0);
            }
        }

        std::erase_if(result, [](const WidgetsPageHostInstance& instance) {
            return instance.instanceId.empty() || instance.packageId.empty();
        });
        std::sort(result.begin(), result.end(),
            [](const WidgetsPageHostInstance& left,
               const WidgetsPageHostInstance& right) {
                if (left.packageId != right.packageId)
                    return left.packageId < right.packageId;
                return left.instanceId < right.instanceId;
            });
        result.erase(std::unique(result.begin(), result.end(),
            [](const WidgetsPageHostInstance& left,
               const WidgetsPageHostInstance& right) {
                return left.instanceId == right.instanceId;
            }), result.end());
        return result;
    }

    std::wstring SourceDisplayName(std::string_view providerId) const
    {
        const std::string key =
            widgets_page_backend_detail::SourceNameKeyFor(providerId);
        return key.empty()
            ? SourceFallbackName(providerId)
            : L(key, SourceFallbackName(providerId));
    }

    InstalledWidgetPackageSnapshot ConvertPackage(
        const PackageGroup& group,
        const InstalledPackage* display,
        const std::vector<WidgetsPageHostInstance>& instances,
        const std::unordered_map<std::string, std::string>&
            workshopAssociations) const
    {
        const auto firstInvalid = [&]() -> const InvalidPackage* {
            const auto select = [](const auto& candidates)
                -> const InvalidPackage* {
                const auto selected = std::find_if(candidates.begin(),
                    candidates.end(), [](const InvalidPackage* candidate) {
                        return candidate && candidate->selected;
                    });
                return selected != candidates.end() ? *selected
                    : candidates.empty() ? nullptr : candidates.front();
            };
            if (const InvalidPackage* value =
                    select(group.invalidDevelopment)) return value;
            if (const InvalidPackage* value =
                    select(group.invalidManaged)) return value;
            return select(group.invalidBuiltin);
        }();
        const SteamWorkshopInstallFailure* firstFailure =
            group.workshopInstallFailures.empty()
            ? nullptr : group.workshopInstallFailures.front();
        const PackageManifest* rawManifest = display
            ? &display->manifest
            : firstInvalid ? &firstInvalid->manifest
            : firstFailure ? &firstFailure->manifest : nullptr;
        const PackageManifest manifest = rawManifest
            ? snowdesktop::widget::LocalizePackageManifest(
                *rawManifest, Locale())
            : PackageManifest{};

        InstalledWidgetPackageSnapshot snapshot;
        snapshot.packageId = Utf8ToWide(group.id);
        snapshot.name = Utf8ToWide(manifest.name);
        snapshot.description = Utf8ToWide(manifest.description);
        snapshot.version = Utf8ToWide(manifest.version);
        snapshot.author = Utf8ToWide(manifest.author);
        const std::string sourceId = display
            ? display->source.providerId
            : firstInvalid ? firstInvalid->source.providerId
            : firstFailure ? "steam-workshop" : std::string{};
        snapshot.sourceId = Utf8ToWide(sourceId);
        snapshot.sourceName = SourceDisplayName(sourceId);
        snapshot.sourceExternalItemId = display
            ? Utf8ToWide(display->source.externalItemId)
            : firstInvalid
                ? Utf8ToWide(firstInvalid->source.externalItemId)
                : firstFailure
                    ? Utf8ToWide(firstFailure->externalItemId)
                    : std::wstring{};
        const bool hasUserSource = group.managed || group.development ||
            !group.invalidManaged.empty() ||
            !group.invalidDevelopment.empty() ||
            !group.workshopInstallFailures.empty();
        snapshot.builtIn = display
            ? display->builtin
            : !hasUserSource && !group.invalidBuiltin.empty();
        snapshot.development = display
            ? display->development
            : !group.invalidDevelopment.empty();
        snapshot.valid = display != nullptr;
        snapshot.enabled = group.managed
            ? group.managed->enabled : display ? display->enabled : false;
        snapshot.active = display && display->active;
        snapshot.canEnable = group.managed != nullptr;
        snapshot.canUninstall = group.managed != nullptr ||
            !group.invalidManaged.empty();
        // Adding a component creates a persisted desktop instance. Invalid
        // packages remain recoverable management rows, but are not launchable.
        snapshot.showAddToDesktop = display &&
            static_cast<bool>(options.addPackageToDesktop);
        snapshot.canAddToDesktop = snapshot.showAddToDesktop &&
            display->active && display->enabled;
        snapshot.canUseDevelopmentOverride = display &&
            group.development != nullptr;
        snapshot.developmentOverrideActive = display && group.development &&
            group.development->active;
        const std::string workshopExternalItemId =
            WorkshopExternalItemIdFor(group, workshopAssociations);
        snapshot.workshopExternalItemId =
            Utf8ToWide(workshopExternalItemId);

        const auto appendInvalid = [&](const auto& candidates) {
            for (const InvalidPackage* invalid : candidates)
            {
                if (!invalid) continue;
                InvalidWidgetPackageSourceSnapshot item;
                item.sourceId = Utf8ToWide(invalid->source.providerId);
                item.sourceName = SourceDisplayName(
                    invalid->source.providerId);
                item.version = Utf8ToWide(invalid->manifest.version);
                item.rootName = invalid->root.filename().wstring();
                item.builtIn = invalid->builtin;
                item.development = invalid->development;
                item.selected = invalid->selected;
                for (const auto& issue : invalid->report.issues)
                {
                    item.issues.push_back({Utf8ToWide(issue.code),
                        Utf8ToWide(issue.message)});
                }
                snapshot.invalidSources.push_back(std::move(item));
            }
        };
        appendInvalid(group.invalidManaged);
        appendInvalid(group.invalidDevelopment);
        appendInvalid(group.invalidBuiltin);
        for (const SteamWorkshopInstallFailure* failure :
             group.workshopInstallFailures)
        {
            if (!failure) continue;
            snapshot.workshopInstallFailures.push_back({
                L"steam-workshop",
                Utf8ToWide(failure->externalItemId),
                Utf8ToWide(failure->manifest.version),
                Utf8ToWide(failure->error)});
        }

        if (!display)
            return snapshot;

        const bool managedDevelopmentVersionExists = group.development &&
            std::any_of(packages.begin(), packages.end(),
                [&](const InstalledPackage& package) {
                    return package.manifest.id == group.id &&
                        IsManagedPackage(package) &&
                        package.manifest.version ==
                            group.development->manifest.version;
                });
        snapshot.canCreateDevelopmentProject = group.managed &&
            !group.development && !workshopExternalItemId.empty();
        snapshot.canInstallDevelopmentSnapshot = group.development &&
            !managedDevelopmentVersionExists;
        bool publisherAvailable = group.development &&
            static_cast<bool>(options.publishDevelopmentPackage);
        if (publisherAvailable && options.canPublishDevelopmentPackage)
        {
            try
            {
                publisherAvailable = options.canPublishDevelopmentPackage();
            }
            catch (...)
            {
                publisherAvailable = false;
            }
        }
        snapshot.canPublishDevelopmentPackage = publisherAvailable;

        if (group.managed)
        {
            std::vector<const InstalledPackage*> restorable;
            for (const InstalledPackage& package : packages)
            {
                if (package.manifest.id == group.id &&
                    IsManagedPackage(package) && !package.selected)
                    restorable.push_back(&package);
            }
            std::sort(restorable.begin(), restorable.end(),
                [](const InstalledPackage* left,
                   const InstalledPackage* right) {
                    return snowdesktop::widget::WidgetPackageValidator::
                        IsNewerSemVer(left->manifest.version,
                            right->manifest.version);
                });
            for (const InstalledPackage* package : restorable)
            {
                const std::wstring version =
                    Utf8ToWide(package->manifest.version);
                if (!version.empty() &&
                    std::none_of(snapshot.restorableVersions.begin(),
                        snapshot.restorableVersions.end(),
                        [&](const WidgetRestorableVersionSnapshot& item) {
                            return item.version == version;
                        }))
                {
                    snapshot.restorableVersions.push_back({version});
                }
            }
        }

        const auto grant = snowdesktop::widget::WidgetPermissionBroker::
            Evaluate(display->permissionState,
                display->manifest.permissions,
                display->manifest.optionalPermissions,
                display->manifest.networkDomains,
                display->grantedPermissions,
                display->grantedNetworkDomains);
        snapshot.permissionState = widgets_page_backend_detail::
            PermissionStateFor(display->permissionState, grant.runtimeBlock);
        snapshot.permissionScopeFingerprint = Utf8ToWide(
            snowdesktop::widget::WidgetPermissionBroker::ScopeFingerprint(
                display->manifest.permissions,
                display->manifest.optionalPermissions,
                display->manifest.networkDomains));
        const auto declared =
            snowdesktop::widget::DeclaredPermissions(display->manifest);
        using snowdesktop::widget::PermissionDecisionState;
        snapshot.canRevokePermissions =
            (display->permissionState == PermissionDecisionState::Granted ||
             display->permissionState ==
                 PermissionDecisionState::LegacyImplicit) &&
            !snowdesktop::widget::PermissionsRequiringConsent(declared).empty();
        snapshot.grantedNetworkDomains.reserve(grant.networkDomains.size());
        snapshot.declaredNetworkDomains.reserve(
            display->manifest.networkDomains.size());
        for (const std::string& domain : display->manifest.networkDomains)
            snapshot.declaredNetworkDomains.push_back(Utf8ToWide(domain));
        for (const std::string& domain : grant.networkDomains)
            snapshot.grantedNetworkDomains.push_back(Utf8ToWide(domain));

        std::unordered_set<std::string> required(
            display->manifest.permissions.begin(),
            display->manifest.permissions.end());
        for (const std::string& permission : declared)
        {
            WidgetPermissionSnapshot item;
            item.id = Utf8ToWide(permission);
            if (const char* key = snowdesktop::widget::
                    WidgetPermissionLabelLocalizationKey(permission))
                item.labelKey = key;
            else
                item.label = Utf8ToWide(permission);
            item.risk = widgets_page_backend_detail::PermissionRiskFor(
                snowdesktop::widget::ClassifyPermissionRisk(permission));
            item.required = required.contains(permission);
            item.requiresConsent =
                snowdesktop::widget::PermissionRequiresConsent(permission);
            item.granted = IsGranted(grant.permissions, permission);
            snapshot.permissions.push_back(std::move(item));
        }

        for (const WidgetsPageHostInstance& instance : instances)
        {
            if (instance.packageId != snapshot.packageId) continue;
            snapshot.instances.push_back({instance.instanceId,
                instance.displayName.empty()
                    ? instance.instanceId : instance.displayName,
                instance.settingsAvailable});
        }
        return snapshot;
    }

    void UpdateCatalogInstallationFlags()
    {
        for (WidgetSourceGroupSnapshot& source : state->sources)
        {
            const bool builtin = source.kind == WidgetSourceKind::BuiltIn;
            for (WidgetCatalogItemSnapshot& item : source.results)
            {
                const std::string packageId = WideToUtf8(item.packageId);
                if (builtin)
                {
                    item.installed = ContainsPackageId(
                        packages, packageId, false);
                    item.updateAvailable = false;
                    item.installAllowed = false;
                    continue;
                }
                const InstalledPackage* managed =
                    ActiveManagedPackage(packages, packageId);
                item.installed = managed != nullptr;
                item.updateAvailable = managed &&
                    snowdesktop::widget::WidgetPackageValidator::
                        IsNewerSemVer(WideToUtf8(item.version),
                            managed->manifest.version);
                item.installAllowed = source.supportsInstall;
            }
        }
    }

    bool CaptureInstalledState()
    {
        if (!OnOwnerThread()) return false;
        try
        {
            packages = WidgetEngine::ListWidgetPackages();
            invalidPackages = WidgetEngine::ListInvalidWidgetPackages();
            workshopInstallFailures =
                WidgetEngine::CachedSteamWorkshopInstallFailures();
            const auto instances = CaptureInstances();
            const auto workshopAssociations =
                WidgetEngine::CachedSteamWorkshopPackageAssociations();
            state->developerOverridesVisible =
                options.developerOverridesVisible &&
                options.developerOverridesVisible();
            CaptureAgentSkillState();
            std::vector<InstalledWidgetPackageSnapshot> converted;
            const auto groups = GroupPackages(
                packages, invalidPackages, workshopInstallFailures);
            converted.reserve(groups.size());
            for (const PackageGroup& group : groups)
            {
                const InstalledPackage* display = DisplayPackage(group);
                if (display &&
                    !snowdesktop::widget::IsExecutablePackageContract(
                        display->manifest))
                    display = nullptr;
                if (!display && group.invalidBuiltin.empty() &&
                    group.invalidManaged.empty() &&
                    group.invalidDevelopment.empty() &&
                    group.workshopInstallFailures.empty())
                    continue;
                converted.push_back(ConvertPackage(group, display,
                    instances, workshopAssociations));
            }
            std::sort(converted.begin(), converted.end(),
                [](const InstalledWidgetPackageSnapshot& left,
                   const InstalledWidgetPackageSnapshot& right) {
                    if (left.name != right.name) return left.name < right.name;
                    return left.packageId < right.packageId;
            });
            state->installed = std::move(converted);

            state->diagnostics.clear();
            state->errors.clear();
            const bool diagnosticsVisible = options.diagnosticsVisible &&
                options.diagnosticsVisible();
            if (diagnosticsVisible)
            {
                std::unordered_map<std::wstring,
                    const WidgetsPageHostInstance*> hostInstances;
                hostInstances.reserve(instances.size());
                for (const auto& instance : instances)
                    hostInstances.emplace(instance.instanceId, &instance);

                for (const WidgetErrorEntry& error :
                     engine.GetWidgetErrors())
                {
                    state->errors.push_back({Utf8ToWide(error.key),
                        Utf8ToWide(error.message)});
                }

                const auto convertViewNodes = [](const auto& source) {
                    std::vector<WidgetRuntimeViewNodeSnapshot> result;
                    result.reserve(source.size());
                    for (const auto& node : source)
                    {
                        result.push_back({Utf8ToWide(std::string(
                                snowdesktop::widget_runtime::
                                    ViewNodeTypeName(node.type))),
                            Utf8ToWide(node.key),
                            Utf8ToWide(node.debugName),
                            Utf8ToWide(node.testId), node.depth,
                            node.frame.x, node.frame.y,
                            node.frame.width, node.frame.height});
                    }
                    return result;
                };

                for (const WidgetDiagnosticEntry& diagnostic :
                     engine.GetWidgetDiagnostics())
                {
                    const auto host = hostInstances.find(
                        diagnostic.widgetId);

                    WidgetRuntimeDiagnosticSnapshot item;
                    item.instanceId = diagnostic.widgetId;
                    item.displayName = host == hostInstances.end() ||
                            host->second->displayName.empty()
                        ? Utf8ToWide(diagnostic.name)
                        : host->second->displayName;
                    item.packageId = Utf8ToWide(diagnostic.packageId);
                    item.scriptPath = diagnostic.scriptPath;
                    item.valid = diagnostic.valid;
                    item.hasManifest = diagnostic.hasManifest;
                    item.lastError = Utf8ToWide(diagnostic.lastError);
                    item.memoryBytes = diagnostic.memoryBytes;
                    item.memoryLimit = diagnostic.memoryLimit;
                    item.lastCallbackMs = diagnostic.lastCallbackMs;
                    item.executionQuotaExceeded =
                        diagnostic.executionQuotaExceeded;
                    item.memoryQuotaExceeded = diagnostic.memoryQuotaExceeded;
                    item.circuitOpen = diagnostic.circuitOpen;
                    for (const std::string& permission :
                         diagnostic.permissions)
                    {
                        item.permissions.push_back(Utf8ToWide(permission));
                    }
                    item.auxiliarySurface =
                        Utf8ToWide(diagnostic.auxiliarySurface);
                    item.desktopViewNodes =
                        convertViewNodes(diagnostic.desktopViewNodes);
                    item.auxiliaryViewNodes =
                        convertViewNodes(diagnostic.auxiliaryViewNodes);
                    for (std::size_t index = 0;
                         index < diagnostic.logs.size(); ++index)
                    {
                        item.recentLogs.push_back({
                            Utf8ToWide(diagnostic.logs[index].level),
                            Utf8ToWide(diagnostic.logs[index].message)});
                    }
                    state->diagnostics.push_back(std::move(item));
                }
                std::sort(state->diagnostics.begin(),
                    state->diagnostics.end(),
                    [](const WidgetRuntimeDiagnosticSnapshot& left,
                       const WidgetRuntimeDiagnosticSnapshot& right) {
                        return left.instanceId < right.instanceId;
                    });
            }
            UpdateCatalogInstallationFlags();
            return true;
        }
        catch (const std::exception& exception)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                Utf8ToWide(exception.what()), "settings.status.error");
        }
        catch (...)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_state_read",
                    L"Component state could not be read."),
                "settings.status.error");
        }
        return false;
    }

    std::vector<WidgetSourceGroupSnapshot> ConvertSources(
        const SourceSearchResult& result) const
    {
        std::vector<WidgetSourceGroupSnapshot> converted;
        converted.reserve(result.sources.size());
        for (const SourceQueryRecord& record : result.sources)
        {
            WidgetSourceGroupSnapshot source;
            source.sourceId = Utf8ToWide(record.source.providerId);
            source.kind = widgets_page_backend_detail::SourceKindFor(
                record.source.providerId);
            source.nameKey = widgets_page_backend_detail::SourceNameKeyFor(
                record.source.providerId);
            source.name = SourceDisplayName(record.source.providerId);
            source.available = record.source.status.available &&
                record.error.empty();
            const std::string status = record.error.empty()
                ? record.source.status.message : record.error;
            source.status = Utf8ToWide(status);
            source.supportsSearch = record.source.capabilities.query;
            source.supportsInstall = source.kind != WidgetSourceKind::BuiltIn &&
                record.source.capabilities.query &&
                record.source.capabilities.details;
            source.workshop =
                source.kind == WidgetSourceKind::SteamWorkshop;
            source.supportsSynchronization =
                static_cast<bool>(options.synchronizeSource) &&
                static_cast<bool>(options.canSynchronizeSource) &&
                options.canSynchronizeSource(record.source.providerId);

            for (const PackageDetails& details : record.results)
            {
                if (!snowdesktop::widget::IsExecutablePackageContract(
                        details.manifest) || details.withdrawn)
                {
                    continue;
                }
                WidgetCatalogItemSnapshot item;
                item.sourceId = source.sourceId;
                item.externalItemId =
                    Utf8ToWide(details.source.externalItemId);
                item.packageId = Utf8ToWide(details.manifest.id);
                item.name = Utf8ToWide(details.manifest.name);
                item.description = Utf8ToWide(
                    details.manifest.description);
                item.version = Utf8ToWide(details.manifest.version);
                item.author = Utf8ToWide(details.manifest.author);
                item.installAllowed = source.supportsInstall;
                source.results.push_back(std::move(item));
            }
            converted.push_back(std::move(source));
        }
        return converted;
    }

    void CompleteSearch(const SourceSearchWork& work,
        SourceSearchResult result)
    {
        const widgets_page_backend_detail::OutstandingOperationIdentity
            operation{work.generation, work.activation, work.taskId,
                widgets_page_backend_detail::OutstandingOperationKind::Search};
        if (!outstandingOperations.Complete(operation)) return;
        if (closed || !active ||
            !widgets_page_backend_detail::CompletionIdentityMatches(
                work.generation, work.activation, work.taskId,
                generation, activation, activeTaskId) ||
            work.searchRevision != requestedSearchRevision)
        {
            MaybeStartDeferredSourceDiscovery();
            return;
        }

        if (result.cancelled)
        {
            activeTaskId = 0;
            state->task = {};
            Publish();
            MaybeStartDeferredSourceDiscovery();
            return;
        }

        activeTaskId = 0;
        state->task = {};
        if (result.localizeError || !result.error.empty())
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                result.localizeError
                    ? L("app.settings.widgets_error_source_query",
                          L"Component source search failed.")
                    : Utf8ToWide(result.error),
                "settings.status.error");
        }
        state->sources = ConvertSources(result);
        state->searchRevision = work.searchRevision;
        state->searchQuery = work.query;
        UpdateCatalogInstallationFlags();
        Publish();
        MaybeStartDeferredSourceDiscovery();
    }

    bool StartSearch(std::uint64_t searchRevision,
        std::wstring query, bool internalRefresh = false)
    {
        if (!active || closed || !OnOwnerThread()) return false;
        if (!options.dispatchToOwner)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_search_dispatcher",
                    L"Component source search requires an owner-thread "
                    L"dispatcher."),
                "settings.status.error");
            Publish();
            return false;
        }
        if (state->task.kind != WidgetsPageTaskKind::None &&
            state->task.kind != WidgetsPageTaskKind::Searching)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Warning,
                L("app.settings.widgets_error_operation_busy",
                    L"Another component operation is still running."));
            Publish();
            return false;
        }
        if (outstandingOperations.Busy() && activeTaskId == 0)
        {
            if (internalRefresh)
                deferredSourceDiscovery = true;
            else
                (void)ReportBusy();
            return false;
        }
        if (!internalRefresh &&
            searchRevision <= requestedSearchRevision &&
            !(searchRevision == 0 && requestedSearchRevision == 0))
        {
            return false;
        }

        requestedSearchRevision = searchRevision;
        requestedSearchQuery = query;
        const std::uint64_t taskId = NextTaskId();
        activeTaskId = taskId;
        const widgets_page_backend_detail::OutstandingOperationIdentity
            operation{generation, activation, taskId,
                widgets_page_backend_detail::OutstandingOperationKind::Search};
        if (!outstandingOperations.Begin(operation))
        {
            activeTaskId = 0;
            return false;
        }
        state->task.taskId = taskId;
        state->task.kind = WidgetsPageTaskKind::Searching;
        state->task.status = L("app.settings.widgets_search", L"Searching");
        state->task.cancellable = true;
        state->task.progress.reset();
        deferredSourceDiscovery = false;
        ClearFeedback();
        Publish();

        SourceSearchWork work;
        work.generation = generation;
        work.activation = activation;
        work.taskId = taskId;
        work.searchRevision = searchRevision;
        work.query = std::move(query);
        work.locale = Locale();
        work.installed = packages;
        work.cancellation = std::make_shared<std::atomic_bool>(false);
        const std::weak_ptr<Impl> weak = weak_from_this();
        work.completion = [weak,
            workGeneration = work.generation,
            workActivation = work.activation,
            workTaskId = work.taskId,
            workSearchRevision = work.searchRevision,
            workQuery = work.query](SourceSearchResult result) mutable {
            const auto self = weak.lock();
            if (!self) return;
            SourceSearchWork identity;
            identity.generation = workGeneration;
            identity.activation = workActivation;
            identity.taskId = workTaskId;
            identity.searchRevision = workSearchRevision;
            identity.query = std::move(workQuery);
            self->MarshalToOwner(
                [weak, identity = std::move(identity),
                    result = std::move(result)]() mutable {
                    if (const auto owner = weak.lock())
                        owner->CompleteSearch(identity, std::move(result));
                });
        };
        if (sourceWorker.Submit(std::move(work))) return true;

        (void)outstandingOperations.Complete(operation);
        if (activeTaskId == taskId)
        {
            activeTaskId = 0;
            state->task = {};
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_search_start",
                    L"Component source search could not be started."),
                "settings.status.error");
            Publish();
        }
        return false;
    }

    void MaybeStartDeferredSourceDiscovery()
    {
        if (!deferredSourceDiscovery || closed || !active ||
            outstandingOperations.Busy() ||
            state->task.kind != WidgetsPageTaskKind::None)
        {
            return;
        }
        deferredSourceDiscovery = false;
        (void)StartSearch(requestedSearchRevision,
            requestedSearchQuery, true);
    }

    [[nodiscard]] bool BusyForMutation() const noexcept
    {
        return awaitingPicker || awaitingConfirmation ||
            state->task.kind != WidgetsPageTaskKind::None ||
            outstandingOperations.Busy();
    }

    bool ReportBusy()
    {
        SetFeedback(WidgetsPageFeedbackSeverity::Warning,
            L("app.settings.widgets_error_operation_busy",
                L"Another component operation is still running."));
        Publish();
        return false;
    }

    const InstalledWidgetPackageSnapshot* FindSnapshotPackage(
        std::wstring_view packageId) const noexcept
    {
        const auto found = std::find_if(state->installed.begin(),
            state->installed.end(), [packageId](const auto& package) {
                return package.packageId == packageId;
            });
        return found == state->installed.end() ? nullptr : &*found;
    }

    const InstalledPackage* FindDisplayPackage(
        std::string_view packageId) const noexcept
    {
        const auto groups = GroupPackages(packages);
        const auto group = std::find_if(groups.begin(), groups.end(),
            [packageId](const PackageGroup& candidate) {
                return candidate.id == packageId;
            });
        return group == groups.end() ? nullptr : DisplayPackage(*group);
    }

    const InstalledPackage* FindManagedPackage(
        std::string_view packageId) const noexcept
    {
        return ActiveManagedPackage(packages, packageId);
    }

    [[nodiscard]] bool HasInvalidManagedPackage(
        std::string_view packageId) const noexcept
    {
        return std::any_of(invalidPackages.begin(), invalidPackages.end(),
            [packageId](const InvalidPackage& package) {
                const std::string_view id = !package.packageId.empty()
                    ? std::string_view(package.packageId)
                    : std::string_view(package.manifest.id);
                return !package.builtin && !package.development &&
                    id == packageId;
            });
    }

    const InstalledPackage* FindDevelopmentPackage(
        std::string_view packageId) const noexcept
    {
        const auto found = std::find_if(packages.begin(), packages.end(),
            [packageId](const InstalledPackage& package) {
                return package.development &&
                    package.manifest.id == packageId;
            });
        return found == packages.end() ? nullptr : &*found;
    }

    void NotifyHostStateChanged()
    {
        if (!options.hostStateChanged) return;
        try
        {
            options.hostStateChanged();
        }
        catch (...)
        {
        }
    }

    void BeginTask(WidgetsPageTaskKind kind, std::wstring packageId = {},
        std::wstring sourceId = {}, bool cancellable = false)
    {
        activeTaskId = NextTaskId();
        state->task = {};
        state->task.taskId = activeTaskId;
        state->task.kind = kind;
        state->task.packageId = std::move(packageId);
        state->task.sourceId = std::move(sourceId);
        state->task.cancellable = cancellable;
        ClearFeedback();
        Publish();
    }

    void FinishTask(WidgetsPageHostOperationResult result,
        std::string_view successKey = {})
    {
        activeTaskId = 0;
        state->task = {};
        if (result.succeeded)
        {
            std::wstring message = std::move(result.message);
            if (message.empty() && !successKey.empty())
                message = L(successKey);
            SetFeedback(WidgetsPageFeedbackSeverity::Success,
                std::move(message));
            NotifyHostStateChanged();
            (void)CaptureInstalledState();
        }
        else
        {
            if (result.message.empty())
                result.message = L(
                    "app.settings.widgets_error_operation_failed",
                    L"The component operation failed.");
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(result.message), "settings.status.error");
        }
        Publish();
    }

    void CompleteHostAsyncOperation(
        const widgets_page_backend_detail::OutstandingOperationIdentity&
            operation,
        WidgetsPageHostOperationResult result,
        std::string_view successKey,
        bool refreshSources)
    {
        if (!outstandingOperations.Complete(operation)) return;
        if (closed || !active ||
            !widgets_page_backend_detail::CompletionIdentityMatches(
                operation.generation, operation.activation,
                operation.taskId, generation, activation, activeTaskId))
        {
            MaybeStartDeferredSourceDiscovery();
            return;
        }
        const bool succeeded = result.succeeded;
        FinishTask(std::move(result), successKey);
        if (succeeded && refreshSources && active && !closed)
            (void)StartSearch(requestedSearchRevision,
                requestedSearchQuery, true);
        MaybeStartDeferredSourceDiscovery();
    }

    WidgetsPageHostOperationResult ExecuteInstall(
        const PendingInstall& install)
    {
        std::wstring error;
        bool installed = false;
        if (install.kind == PendingInstall::Kind::LocalPath)
        {
            const auto identityChanged = [&](std::wstring message = {}) {
                if (message.empty())
                {
                    message = L("settings.widgets.install.identityChanged",
                        L"The selected component package changed after review. "
                        L"Choose the package again.");
                }
                return WidgetsPageHostOperationResult::Failure(
                    std::move(message));
            };
            if (!install.localSnapshot ||
                install.path != install.localSnapshot->path)
            {
                return identityChanged();
            }
            // Keep the reviewed staging object read-only and non-replaceable
            // until the package manager has consumed it. Every validation,
            // hash and extraction below therefore observes the same bytes.
            const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
            const ScopedPackageIdentityLock packageLock(install.path,
                packagePaths.staging, install.localSnapshot->identity);
            if (!packageLock.Acquired() ||
                !packageLock.MatchesPathIdentity())
            {
                return identityChanged();
            }
            const std::string actualSha256 =
                snowdesktop::widget::WidgetPackageManager::Sha256File(
                    install.path);
            if (!packageLock.MatchesPathIdentity())
            {
                return identityChanged();
            }
            snowdesktop::widget::WidgetPackageManager validator(
                packagePaths);
            PackageManifest currentManifest;
            if (!packageLock.MatchesPathIdentity())
            {
                return identityChanged();
            }
            const auto report = validator.ValidateArchive(
                install.path, &currentManifest);
            if (!packageLock.MatchesPathIdentity() || actualSha256.empty() ||
                actualSha256 != install.localSnapshot->sha256 ||
                !report.Ok() ||
                currentManifest.id != install.localSnapshot->manifest.id ||
                currentManifest.version !=
                    install.localSnapshot->manifest.version)
            {
                return identityChanged();
            }
            if (!packageLock.MatchesPathIdentity())
            {
                return identityChanged();
            }
            installed = engine.InstallAndVerifyWidgetPackage(
                install.path.wstring(), error, install.allowSourceChange,
                install.allowPermissionExpansion);
            if (!packageLock.MatchesPathIdentity())
            {
                return identityChanged(
                    L("app.settings.widgets_error_install_identity_changed",
                        L"The selected component package changed while it was "
                        L"being installed. Choose the package again."));
            }
        }
        else
        {
            installed = engine.InstallAndVerifyWidgetPackageFromSource(
                install.sourceId, install.externalItemId, install.version,
                error, install.allowSourceChange,
                install.allowPermissionExpansion);
        }
        return installed
            ? WidgetsPageHostOperationResult::Success()
            : WidgetsPageHostOperationResult::Failure(std::move(error));
    }

    void RequestInstallConfirmation(PendingInstall install,
        std::wstring reason)
    {
        activeTaskId = 0;
        state->task = {};
        if (!options.confirmInstall)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_install_confirmation",
                    L"The install confirmation could not be shown."),
                "settings.status.error");
            Publish();
            return;
        }

        WidgetInstallConfirmationRequest dialogRequest;
        dialogRequest.reasons = ParseInstallConfirmationReasons(reason);
        dialogRequest.technicalDetails = reason;
        const bool sourceChange = std::any_of(
            dialogRequest.reasons.begin(), dialogRequest.reasons.end(),
            [](const auto& item) {
                return item.kind ==
                    WidgetInstallConfirmationReasonKind::SourceChange;
            });
        const bool permissionExpansion = std::any_of(
            dialogRequest.reasons.begin(), dialogRequest.reasons.end(),
            [](const auto& item) {
                return item.kind ==
                        WidgetInstallConfirmationReasonKind::NewPermission ||
                    item.kind ==
                        WidgetInstallConfirmationReasonKind::NewWebsite;
            });
        install.allowSourceChange =
            install.allowSourceChange || sourceChange;
        install.allowPermissionExpansion =
            install.allowPermissionExpansion || permissionExpansion;

        dialogRequest.packageId = Utf8ToWide(install.packageId);
        dialogRequest.version = Utf8ToWide(install.version);
        dialogRequest.sourceId = Utf8ToWide(install.sourceId);
        dialogRequest.externalItemId =
            Utf8ToWide(install.externalItemId);
        if (install.localSnapshot)
        {
            const PackageManifest localized =
                snowdesktop::widget::LocalizePackageManifest(
                    install.localSnapshot->manifest, Locale());
            dialogRequest.packageName = localized.name.empty()
                ? Utf8ToWide(install.localSnapshot->manifest.id)
                : Utf8ToWide(localized.name);
            dialogRequest.packageId =
                Utf8ToWide(install.localSnapshot->manifest.id);
            dialogRequest.version =
                Utf8ToWide(install.localSnapshot->manifest.version);
            dialogRequest.sha256 = Utf8ToWide(
                install.localSnapshot->sha256);
        }
        else if (const InstalledWidgetPackageSnapshot* package =
                     FindSnapshotPackage(dialogRequest.packageId))
        {
            dialogRequest.packageName = package->name;
        }

        awaitingConfirmation = true;
        confirmationRequestId = NextTaskId();
        SetFeedback(WidgetsPageFeedbackSeverity::Warning,
            L("app.settings.widgets_install_confirm",
                L"Confirm the component installation to continue."));
        Publish();
        const std::uint64_t requestGeneration = generation;
        const std::uint64_t requestActivation = activation;
        const std::uint64_t requestId = confirmationRequestId;
        const std::weak_ptr<Impl> weak = weak_from_this();
        try
        {
            options.confirmInstall(requestGeneration,
                std::move(dialogRequest),
                [weak, requestGeneration, requestActivation, requestId,
                    install = std::move(install)](bool confirmed) mutable {
                    const auto self = weak.lock();
                    if (!self) return;
                    self->MarshalToOwner(
                        [weak, requestGeneration, requestActivation,
                            requestId, confirmed,
                            install = std::move(install)]() mutable {
                            const auto owner = weak.lock();
                            if (!owner || owner->closed || !owner->active ||
                                !owner->awaitingConfirmation ||
                                owner->confirmationRequestId != requestId ||
                                owner->generation != requestGeneration ||
                                owner->activation != requestActivation)
                            {
                                return;
                            }
                            owner->awaitingConfirmation = false;
                            owner->confirmationRequestId = 0;
                            if (!confirmed)
                            {
                                owner->ClearFeedback();
                                owner->Publish();
                                return;
                            }
                            owner->RunInstall(std::move(install));
                        });
                });
        }
        catch (...)
        {
            awaitingConfirmation = false;
            confirmationRequestId = 0;
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_install_confirmation",
                    L"The install confirmation could not be shown."),
                "settings.status.error");
            Publish();
        }
    }

    std::shared_ptr<LocalPackageSnapshot> StageLocalPackage(
        const std::filesystem::path& selected, std::wstring& error)
    {
        snowdesktop::widget::WidgetPackageValidator archiveValidator;
        const auto archiveReport = archiveValidator.ValidateArchive(selected);
        if (!archiveReport.Ok())
        {
            error = Utf8ToWide(archiveReport.ToJson());
            return {};
        }

        const auto stagingRoot =
            WidgetEngine::GetWidgetPackagePaths().staging;
        std::error_code filesystemError;
        std::filesystem::create_directories(stagingRoot, filesystemError);
        if (filesystemError)
        {
            error = FormatLocalizedValue(L(
                "app.settings.widgets_error_package_copy_review",
                L"The selected package could not be copied for review: {0}"),
                Utf8ToWide(filesystemError.message()));
            return {};
        }
        auto snapshot = std::make_shared<LocalPackageSnapshot>();
        snapshot->path = stagingRoot /
            Utf8ToWide("settings-review-" +
                snowdesktop::widget::WidgetPackageManager::GenerateUuid() +
                ".snowwidget");
        std::filesystem::copy_file(selected, snapshot->path,
            std::filesystem::copy_options::none, filesystemError);
        if (filesystemError)
        {
            error = FormatLocalizedValue(L(
                "app.settings.widgets_error_package_copy_review",
                L"The selected package could not be copied for review: {0}"),
                Utf8ToWide(filesystemError.message()));
            return {};
        }

        // Bind the manifest and fingerprint to one immutable staging object.
        // The same lock discipline is repeated during confirmed installation.
        const ScopedPackageIdentityLock packageLock(
            snapshot->path, stagingRoot);
        if (!packageLock.Acquired() ||
            !packageLock.MatchesPathIdentity())
        {
            error = L("app.settings.widgets_error_package_lock_review",
                L"The selected package could not be locked for review.");
            return {};
        }
        snapshot->identity = packageLock.Identity();

        snowdesktop::widget::WidgetPackageManager validator(
            WidgetEngine::GetWidgetPackagePaths());
        if (!packageLock.MatchesPathIdentity())
        {
            error = L("app.settings.widgets_error_package_identity_changed",
                L"The selected package identity changed during review.");
            return {};
        }
        const auto report = validator.ValidateArchive(
            snapshot->path, &snapshot->manifest);
        if (!packageLock.MatchesPathIdentity())
        {
            error = L("app.settings.widgets_error_package_identity_changed",
                L"The selected package identity changed during review.");
            return {};
        }
        snapshot->sha256 =
            snowdesktop::widget::WidgetPackageManager::Sha256File(
                snapshot->path);
        if (!packageLock.MatchesPathIdentity() || !report.Ok() ||
            snapshot->sha256.empty() ||
            snapshot->manifest.id.empty() ||
            snapshot->manifest.version.empty())
        {
            error = report.Ok()
                ? L("app.settings.widgets_error_package_identity_read",
                      L"The selected package identity could not be read.")
                : Utf8ToWide(report.ToJson());
            return {};
        }
        return snapshot;
    }

    std::shared_ptr<LocalPackageSnapshot> StageDevelopmentPackage(
        const InstalledPackage& development, std::wstring& error)
    {
        if (!development.development || development.manifest.id.empty() ||
            development.manifest.version.empty())
        {
            error = L("app.settings.widgets_error_development_unavailable",
                L"The development component is unavailable.");
            return {};
        }

        const auto packagePaths = WidgetEngine::GetWidgetPackagePaths();
        std::error_code filesystemError;
        std::filesystem::create_directories(
            packagePaths.staging, filesystemError);
        if (filesystemError)
        {
            error = FormatLocalizedValue(L(
                "app.settings.widgets_error_development_prepare",
                L"The development component could not be prepared: {0}"),
                Utf8ToWide(filesystemError.message()));
            return {};
        }

        // Bind the selected development directory while exporting it. The
        // resulting archive, rather than the mutable tree, becomes the sole
        // reviewed input to confirmation and installation.
        const ScopedPackageIdentityLock sourceLock(development.root,
            packagePaths.development, std::nullopt, true);
        if (!sourceLock.Acquired() || !sourceLock.MatchesPathIdentity())
        {
            error = L(
                "app.settings.widgets_error_development_directory_changed",
                L"The development component directory is unsafe or changed "
                L"before it could be prepared.");
            return {};
        }

        auto snapshot = std::make_shared<LocalPackageSnapshot>();
        snapshot->path = packagePaths.staging /
            Utf8ToWide("settings-review-development-" +
                snowdesktop::widget::WidgetPackageManager::GenerateUuid() +
                ".snowwidget");
        snowdesktop::widget::WidgetPackageManager manager(packagePaths);
        snowdesktop::widget::PackageArtifact artifact;
        snowdesktop::widget::ValidationReport exportReport;
        std::string exportError;
        if (!manager.ExportDirectory(development.root, snapshot->path,
                artifact, exportReport, exportError) ||
            !sourceLock.MatchesPathIdentity())
        {
            error = exportError.empty()
                ? L("app.settings.widgets_error_development_export_changed",
                      L"The development component changed while it was "
                      L"exported.")
                : Utf8ToWide(exportError);
            return {};
        }

        const ScopedPackageIdentityLock archiveLock(
            snapshot->path, packagePaths.staging);
        if (!archiveLock.Acquired() ||
            !archiveLock.MatchesPathIdentity())
        {
            error = L(
                "app.settings.widgets_error_development_snapshot_lock",
                L"The prepared development snapshot could not be locked for "
                L"review.");
            return {};
        }
        snapshot->identity = archiveLock.Identity();
        const auto archiveReport = manager.ValidateArchive(
            snapshot->path, &snapshot->manifest);
        if (!archiveLock.MatchesPathIdentity())
        {
            error = L(
                "app.settings.widgets_error_development_snapshot_changed",
                L"The prepared development snapshot changed during review.");
            return {};
        }
        snapshot->sha256 =
            snowdesktop::widget::WidgetPackageManager::Sha256File(
                snapshot->path);
        if (!archiveLock.MatchesPathIdentity() || !archiveReport.Ok() ||
            snapshot->sha256.empty() ||
            snapshot->manifest.id != development.manifest.id ||
            snapshot->manifest.version != development.manifest.version ||
            artifact.packageId != development.manifest.id ||
            artifact.version != development.manifest.version ||
            artifact.sha256 != snapshot->sha256)
        {
            error = L(
                "app.settings.widgets_error_development_snapshot_mismatch",
                L"The prepared development snapshot did not match the "
                L"selected development component.");
            return {};
        }
        return snapshot;
    }

    void RunInstall(PendingInstall install)
    {
        BeginTask(WidgetsPageTaskKind::Installing,
            Utf8ToWide(install.packageId), Utf8ToWide(install.sourceId));
        WidgetsPageHostOperationResult result;
        try
        {
            result = ExecuteInstall(install);
        }
        catch (const std::exception& exception)
        {
            result = WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(exception.what()));
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("app.settings.widgets_error_install_failed",
                    L"The component could not be installed."));
        }

        const bool packageInstalled = result.succeeded;
        if (result.succeeded && !install.developmentPackageId.empty())
        {
            std::string overrideError;
            if (!WidgetEngine::SetWidgetDevelopmentOverride(
                    install.developmentPackageId, false, overrideError))
            {
                result = WidgetsPageHostOperationResult::Failure(
                    Utf8ToWide(overrideError));
            }
            else
            {
                const auto activePackage = WidgetEngine::GetWidgetPackage(
                    Utf8ToWide(install.developmentPackageId));
                const auto blocked = [&]() {
                    if (!activePackage || !activePackage->enabled)
                        return true;
                    const auto grant = snowdesktop::widget::
                        WidgetPermissionBroker::Evaluate(
                            activePackage->permissionState,
                            activePackage->manifest.permissions,
                            activePackage->manifest.optionalPermissions,
                            activePackage->manifest.networkDomains,
                            activePackage->grantedPermissions,
                            activePackage->grantedNetworkDomains);
                    return grant.runtimeBlock != snowdesktop::widget::
                        PermissionRuntimeBlock::None;
                }();
                std::vector<std::wstring> reloadFailures;
                std::vector<std::wstring> instances;
                for (const LuaWidget& widget : engine.GetWidgets())
                {
                    if (!widget.preview && widget.packageId ==
                            install.developmentPackageId)
                    {
                        instances.push_back(widget.widgetId);
                    }
                }
                for (const std::wstring& instance : instances)
                {
                    if (blocked)
                        engine.UnloadWidget(instance);
                    else if (!engine.ReloadWidget(instance))
                        reloadFailures.push_back(instance);
                }
                if (!reloadFailures.empty())
                {
                    std::wstring message = L(
                        "app.settings.widgets_error_development_reload",
                        L"The development snapshot was installed, but one or "
                        L"more component instances could not be reloaded.");
                    if (install.developmentOverrideWasActive)
                    {
                        std::string rollbackError;
                        if (WidgetEngine::SetWidgetDevelopmentOverride(
                                install.developmentPackageId, true,
                                rollbackError))
                        {
                            for (const std::wstring& instance : instances)
                                (void)engine.ReloadWidget(instance);
                            message += L" " + L(
                                "app.settings.widgets_error_development_override_restored",
                                L"The previous development override was "
                                L"restored.");
                        }
                        else
                        {
                            message += L" " + FormatLocalizedValue(L(
                                "app.settings.widgets_error_development_override_restore_failed",
                                L"Restoring the previous development override "
                                L"also failed: {0}"),
                                Utf8ToWide(rollbackError));
                        }
                    }
                    result = WidgetsPageHostOperationResult::Failure(
                        std::move(message));
                }
            }
        }

        const bool needsSourceConfirmation = !install.allowSourceChange &&
            result.message.find(
                L"source changes require explicit confirmation") !=
                std::wstring::npos;
        const bool needsPermissionConfirmation =
            !install.allowPermissionExpansion &&
            (result.message.find(L"requests a new required permission") !=
                    std::wstring::npos ||
                result.message.find(L"requests a new optional permission") !=
                    std::wstring::npos ||
                result.message.find(L"requests a new network domain") !=
                    std::wstring::npos);
        if (!result.succeeded &&
            (needsSourceConfirmation || needsPermissionConfirmation) &&
            widgets_page_backend_detail::InstallFailureNeedsConfirmation(
                result.message))
        {
            RequestInstallConfirmation(
                std::move(install), std::move(result.message));
            return;
        }
        // Installation can succeed before a development override/reload
        // follow-up reports an error. Publish the changed package table even
        // on that partial-failure path so the immutable UI snapshot is not
        // stale.
        if (packageInstalled && !result.succeeded)
        {
            NotifyHostStateChanged();
            (void)CaptureInstalledState();
        }
        FinishTask(std::move(result),
            install.developmentPackageId.empty()
                ? "app.settings.widgets_install_ok"
                : "app.settings.widgets_install_managed_ok");
    }

    bool BrowseInstallPackage()
    {
        if (BusyForMutation()) return ReportBusy();
        if (!options.pickPackage)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_package_picker_unavailable",
                    L"The package picker is unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }
        awaitingPicker = true;
        pickerRequestId = NextTaskId();
        const std::uint64_t requestGeneration = generation;
        const std::uint64_t requestActivation = activation;
        const std::uint64_t requestId = pickerRequestId;
        const std::weak_ptr<Impl> weak = weak_from_this();
        try
        {
            options.pickPackage(requestGeneration,
                [weak, requestGeneration, requestActivation, requestId](
                    std::optional<std::filesystem::path> selected) mutable {
                    const auto self = weak.lock();
                    if (!self) return;
                    self->MarshalToOwner(
                        [weak, requestGeneration, requestActivation,
                            requestId, selected = std::move(selected)]() mutable {
                            const auto owner = weak.lock();
                            if (!owner || owner->closed || !owner->active ||
                                !owner->awaitingPicker ||
                                owner->pickerRequestId != requestId ||
                                owner->generation != requestGeneration ||
                                owner->activation != requestActivation)
                            {
                                return;
                            }
                            owner->awaitingPicker = false;
                            owner->pickerRequestId = 0;
                            if (!selected) return;
                            PendingInstall install;
                            install.kind = PendingInstall::Kind::LocalPath;
                            std::wstring error;
                            install.localSnapshot = owner->StageLocalPackage(
                                *selected, error);
                            if (!install.localSnapshot)
                            {
                                owner->SetFeedback(
                                    WidgetsPageFeedbackSeverity::Error,
                                    std::move(error),
                                    "settings.status.error");
                                owner->Publish();
                                return;
                            }
                            install.path = install.localSnapshot->path;
                            install.packageId =
                                install.localSnapshot->manifest.id;
                            install.version =
                                install.localSnapshot->manifest.version;
                            owner->RequestInstallConfirmation(
                                std::move(install), {});
                        });
                });
            return true;
        }
        catch (...)
        {
            awaitingPicker = false;
            pickerRequestId = 0;
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_package_picker_open",
                    L"The package picker could not be opened."),
                "settings.status.error");
            Publish();
            return false;
        }
    }

    bool InstallCatalogItem(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const auto source = std::find_if(state->sources.begin(),
            state->sources.end(), [&](const WidgetSourceGroupSnapshot& item) {
                return item.sourceId == request.sourceId;
            });
        if (source == state->sources.end() || !source->available ||
            !source->supportsInstall)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_source_unavailable",
                    L"The selected component source is unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }
        const auto item = std::find_if(source->results.begin(),
            source->results.end(), [&](const WidgetCatalogItemSnapshot& value) {
                return value.packageId == request.packageId &&
                    value.externalItemId == request.externalItemId &&
                    value.version == request.version;
            });
        if (item == source->results.end() || !item->installAllowed)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_result_stale",
                    L"The selected component result is stale or unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }

        PendingInstall install;
        install.kind = PendingInstall::Kind::SourceItem;
        install.packageId = WideToUtf8(item->packageId);
        install.sourceId = WideToUtf8(item->sourceId);
        install.externalItemId = WideToUtf8(item->externalItemId);
        install.version = WideToUtf8(item->version);
        if (install.packageId.empty() || install.sourceId.empty() ||
            install.externalItemId.empty())
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_identity_invalid",
                    L"The selected component identity is invalid."),
                "settings.status.error");
            Publish();
            return false;
        }
        RunInstall(std::move(install));
        return true;
    }

    bool RetryWorkshopInstall(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const InstalledWidgetPackageSnapshot* package =
            FindSnapshotPackage(request.packageId);
        if (!package)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_workshop_install_failed",
                    L"Installation failed"),
                "settings.status.error");
            Publish();
            return false;
        }
        const auto failure = std::find_if(
            package->workshopInstallFailures.begin(),
            package->workshopInstallFailures.end(),
            [&](const WidgetWorkshopInstallFailureSnapshot& candidate) {
                return candidate.sourceId == request.sourceId &&
                    candidate.externalItemId == request.externalItemId &&
                    candidate.version == request.version;
            });
        if (failure == package->workshopInstallFailures.end() ||
            failure->sourceId != L"steam-workshop" ||
            failure->externalItemId.empty())
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_workshop_install_failed",
                    L"Installation failed"),
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::Installing, request.packageId,
            failure->sourceId);
        std::wstring error;
        const bool installed = engine.InstallAndVerifyWidgetPackageFromSource(
            WideToUtf8(failure->sourceId),
            WideToUtf8(failure->externalItemId),
            WideToUtf8(failure->version), error, false, false);
        FinishTask(installed
                ? WidgetsPageHostOperationResult::Success()
                : WidgetsPageHostOperationResult::Failure(std::move(error)),
            "app.settings.widgets_install_ok");
        return installed;
    }

    bool SetPackageEnabled(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const std::string packageId = WideToUtf8(request.packageId);
        const InstalledPackage* managed = FindManagedPackage(packageId);
        if (!managed)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_toggle_unavailable",
                    L"This component cannot be enabled or disabled."),
                "settings.status.error");
            Publish();
            return false;
        }
        std::string error;
        if (!WidgetEngine::SetWidgetPackageEnabled(
                packageId, request.enabled, error))
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                Utf8ToWide(error), "settings.status.error");
            Publish();
            return false;
        }
        if (!request.enabled)
        {
            std::vector<std::wstring> instances;
            for (const LuaWidget& widget : engine.GetWidgets())
                if (!widget.preview && widget.packageId == packageId)
                    instances.push_back(widget.widgetId);
            for (const std::wstring& instance : instances)
                engine.UnloadWidget(instance);
        }
        NotifyHostStateChanged();
        (void)CaptureInstalledState();
        SetFeedback(WidgetsPageFeedbackSeverity::Success,
            L(request.enabled ? "app.settings.widgets_enabled_ok"
                              : "app.settings.widgets_disabled_ok"));
        Publish();
        return true;
    }

    bool SetPermissionDecision(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const std::string packageId = WideToUtf8(request.packageId);
        const InstalledPackage* package = FindDisplayPackage(packageId);
        if (!package)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_package_unavailable",
                    L"The component package is unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }

        const std::string expectedVersion = WideToUtf8(request.version);
        const std::string expectedSourceId = WideToUtf8(request.sourceId);
        const std::string expectedExternalItemId =
            WideToUtf8(request.externalItemId);
        const std::string expectedScopeFingerprint =
            WideToUtf8(request.scopeFingerprint);
        const std::string currentScopeFingerprint =
            snowdesktop::widget::WidgetPermissionBroker::ScopeFingerprint(
                package->manifest.permissions,
                package->manifest.optionalPermissions,
                package->manifest.networkDomains);
        if (expectedVersion.empty() || expectedSourceId.empty() ||
            expectedScopeFingerprint.empty() ||
            package->manifest.version != expectedVersion ||
            package->source.providerId != expectedSourceId ||
            package->source.externalItemId != expectedExternalItemId ||
            currentScopeFingerprint != expectedScopeFingerprint)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Warning,
                L("app.settings.widgets_permissions_scope_changed",
                    L"The component or its requested access changed. Reopen "
                    L"the permission editor and review it again."));
            (void)CaptureInstalledState();
            Publish();
            return false;
        }

        using snowdesktop::widget::PermissionDecisionState;
        PermissionDecisionState stateValue;
        switch (request.permissionState)
        {
        case WidgetPackagePermissionState::Granted:
            stateValue = PermissionDecisionState::Granted;
            break;
        case WidgetPackagePermissionState::Denied:
            stateValue = PermissionDecisionState::Denied;
            break;
        default:
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_permission_invalid",
                    L"The requested permission decision is invalid."),
                "settings.status.error");
            Publish();
            return false;
        }

        std::unordered_set<std::string> requestedPermissions;
        for (const std::wstring& permission : request.grantedPermissions)
            requestedPermissions.insert(WideToUtf8(permission));
        std::vector<std::string> grantedPermissions;
        if (stateValue == PermissionDecisionState::Granted)
        {
            for (const std::string& permission :
                snowdesktop::widget::DeclaredPermissions(package->manifest))
            {
                if (requestedPermissions.contains(permission))
                    grantedPermissions.push_back(permission);
            }
        }

        std::vector<std::string> grantedDomains;
        const bool grantsDeclaredNetwork =
            IsGranted(grantedPermissions, "network.http") ||
            IsGranted(grantedPermissions, "network.internet");
        if (grantsDeclaredNetwork)
        {
            std::unordered_set<std::string> requestedDomains;
            for (const std::wstring& domain :
                request.grantedNetworkDomains)
            {
                requestedDomains.insert(WideToUtf8(domain));
            }
            // The current presenter exposes network domains as a declared
            // package scope rather than individual toggles. A newly granted
            // network permission therefore grants the full declared list,
            // matching the legacy settings behavior.
            for (const std::string& domain : package->manifest.networkDomains)
            {
                if (requestedDomains.contains(domain))
                {
                    grantedDomains.push_back(domain);
                }
            }
        }

        BeginTask(WidgetsPageTaskKind::ApplyingPermissions,
            request.packageId);
        std::string error;
        const bool applied = engine.ApplyWidgetPermissionDecision(
            request.packageId, stateValue, grantedPermissions,
            grantedDomains, error);
        FinishTask(applied
                ? WidgetsPageHostOperationResult::Success()
                : WidgetsPageHostOperationResult::Failure(Utf8ToWide(error)),
            stateValue == PermissionDecisionState::Denied
                ? "app.settings.widgets_permissions_revoked_ok"
                : "app.settings.widgets_permissions_updated_ok");
        return applied;
    }

    bool SetDevelopmentOverride(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        if (!options.developerOverridesVisible ||
            !options.developerOverridesVisible())
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_development_disabled",
                    L"Development overrides are disabled."),
                "settings.status.error");
            Publish();
            return false;
        }
        const std::string packageId = WideToUtf8(request.packageId);
        const InstalledWidgetPackageSnapshot* snapshotPackage =
            FindSnapshotPackage(request.packageId);
        if (!snapshotPackage)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_package_unavailable",
                    L"The component package is unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }
        const bool previous = snapshotPackage->developmentOverrideActive;
        if (previous == request.enabled)
            return true;
        const bool developmentExists = std::any_of(
            packages.begin(), packages.end(), [&](const InstalledPackage& item) {
                return item.development && item.manifest.id == packageId;
            });
        if (request.enabled && !developmentExists)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_development_unavailable",
                    L"The development package is unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }

        std::vector<std::wstring> instances;
        for (const LuaWidget& widget : engine.GetWidgets())
            if (!widget.preview && widget.packageId == packageId)
                instances.push_back(widget.widgetId);

        BeginTask(WidgetsPageTaskKind::ApplyingDevelopmentOverride,
            request.packageId);
        std::string error;
        if (!WidgetEngine::SetWidgetDevelopmentOverride(
                packageId, request.enabled, error))
        {
            FinishTask(WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(error)));
            return false;
        }

        const auto activePackage =
            WidgetEngine::GetWidgetPackage(request.packageId);
        if (activePackage)
        {
            const auto grant = snowdesktop::widget::WidgetPermissionBroker::
                Evaluate(activePackage->permissionState,
                    activePackage->manifest.permissions,
                    activePackage->manifest.optionalPermissions,
                    activePackage->manifest.networkDomains,
                    activePackage->grantedPermissions,
                    activePackage->grantedNetworkDomains);
            if (!activePackage->enabled || grant.runtimeBlock !=
                    snowdesktop::widget::PermissionRuntimeBlock::None)
            {
                // A newly selected source can legitimately require fresh
                // consent. Keep the override, unload old source runtimes and
                // let the host expose the pending/denied state instead of
                // treating a permission-blocked reload as toggle failure.
                for (const std::wstring& instance : instances)
                    engine.UnloadWidget(instance);
                FinishTask(WidgetsPageHostOperationResult::Success(),
                    request.enabled
                        ? "app.settings.widgets_development_activated_ok"
                        : "app.settings.widgets_development_deactivated_ok");
                return true;
            }
        }

        std::vector<std::wstring> reloadFailures;
        for (const std::wstring& instance : instances)
        {
            if (!engine.ReloadWidget(instance))
                reloadFailures.push_back(instance);
        }
        if (!reloadFailures.empty())
        {
            std::string rollbackError;
            const bool overrideRolledBack =
                WidgetEngine::SetWidgetDevelopmentOverride(
                    packageId, previous, rollbackError);
            std::vector<std::wstring> rollbackReloadFailures;
            if (overrideRolledBack)
            {
                for (const std::wstring& instance : instances)
                {
                    if (!engine.ReloadWidget(instance))
                        rollbackReloadFailures.push_back(instance);
                }
            }

            std::wstring message =
                L("app.settings.widgets_error_override_reload_rolled_back",
                    L"One or more component instances could not be reloaded; "
                    L"the development override was rolled back.");
            if (!overrideRolledBack)
            {
                message = FormatLocalizedValue(L(
                    "app.settings.widgets_error_override_rollback_failed",
                    L"One or more component instances could not be reloaded, "
                    L"and the development override rollback also failed: {0}"),
                    Utf8ToWide(rollbackError));
            }
            else if (!rollbackReloadFailures.empty())
            {
                message += L" " + L(
                    "app.settings.widgets_error_instances_need_restart",
                    L"Some instances could not be restored and need an "
                    L"application restart.");
            }
            NotifyHostStateChanged();
            FinishTask(WidgetsPageHostOperationResult::Failure(
                std::move(message)));
            return false;
        }
        FinishTask(WidgetsPageHostOperationResult::Success(),
            request.enabled
                ? "app.settings.widgets_development_activated_ok"
                : "app.settings.widgets_development_deactivated_ok");
        return true;
    }

    bool CreateDevelopmentProject(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const InstalledWidgetPackageSnapshot* snapshotPackage =
            FindSnapshotPackage(request.packageId);
        const std::string packageId = WideToUtf8(request.packageId);
        if (!snapshotPackage ||
            !snapshotPackage->canCreateDevelopmentProject ||
            !FindManagedPackage(packageId) ||
            FindDevelopmentPackage(packageId))
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_development_create",
                    L"A development project cannot be created for this "
                    L"component."),
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::ApplyingDevelopmentOverride,
            request.packageId);
        std::filesystem::path projectRoot;
        std::string error;
        if (!WidgetEngine::CreateWidgetDevelopmentProject(
                packageId, projectRoot, error))
        {
            FinishTask(WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(error)));
            return false;
        }

        // Publish the completed mutation before the host reveals the folder
        // or navigates to Developer Tools. That callback may synchronously
        // change routes and reactivate this backend, so no page state may be
        // touched after it runs.
        activeTaskId = 0;
        state->task = {};
        NotifyHostStateChanged();
        (void)CaptureInstalledState();
        SetFeedback(WidgetsPageFeedbackSeverity::Success,
            L("app.settings.widgets_create_development_ok",
                L"Development project created."));
        Publish();
        if (options.developmentProjectCreated)
        {
            try
            {
                (void)options.developmentProjectCreated(projectRoot);
            }
            catch (...)
            {
            }
        }
        return true;
    }

    bool InstallDevelopmentSnapshot(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const InstalledWidgetPackageSnapshot* snapshotPackage =
            FindSnapshotPackage(request.packageId);
        const std::string packageId = WideToUtf8(request.packageId);
        const InstalledPackage* development =
            FindDevelopmentPackage(packageId);
        if (!snapshotPackage ||
            !snapshotPackage->canInstallDevelopmentSnapshot ||
            !development)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_development_install_managed",
                    L"The development component cannot be installed as a "
                    L"managed snapshot."),
                "settings.status.error");
            Publish();
            return false;
        }

        std::wstring error;
        std::shared_ptr<LocalPackageSnapshot> localSnapshot =
            StageDevelopmentPackage(*development, error);
        if (!localSnapshot)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(error), "settings.status.error");
            Publish();
            return false;
        }

        PendingInstall install;
        install.kind = PendingInstall::Kind::LocalPath;
        install.path = localSnapshot->path;
        install.packageId = localSnapshot->manifest.id;
        install.version = localSnapshot->manifest.version;
        install.localSnapshot = std::move(localSnapshot);
        install.developmentPackageId = packageId;
        install.developmentOverrideWasActive =
            snapshotPackage->developmentOverrideActive;
        RequestInstallConfirmation(std::move(install), {});
        return true;
    }

    bool RollbackPackage(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const InstalledWidgetPackageSnapshot* snapshotPackage =
            FindSnapshotPackage(request.packageId);
        const std::string packageId = WideToUtf8(request.packageId);
        const std::string requestedVersion = WideToUtf8(request.version);
        const InstalledPackage* managed = FindManagedPackage(packageId);
        const bool advertisedVersion = snapshotPackage &&
            std::any_of(snapshotPackage->restorableVersions.begin(),
                snapshotPackage->restorableVersions.end(),
                [&](const WidgetRestorableVersionSnapshot& version) {
                    return version.version == request.version;
                });
        if (!snapshotPackage || !managed || requestedVersion.empty() ||
            !advertisedVersion)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_version_stale",
                    L"The requested component version is stale or "
                    L"unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }

        const std::string previousVersion = managed->manifest.version;
        std::vector<std::wstring> instances;
        for (const LuaWidget& widget : engine.GetWidgets())
        {
            if (!widget.preview && widget.packageId == packageId)
                instances.push_back(widget.widgetId);
        }

        BeginTask(WidgetsPageTaskKind::Installing, request.packageId);
        std::string error;
        if (!WidgetEngine::RollbackWidgetPackage(
                packageId, requestedVersion, error))
        {
            FinishTask(WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(error)));
            return false;
        }

        const auto activePackage =
            WidgetEngine::GetWidgetPackage(request.packageId);
        bool blocked = !activePackage || !activePackage->enabled;
        if (activePackage && activePackage->enabled)
        {
            const auto grant = snowdesktop::widget::WidgetPermissionBroker::
                Evaluate(activePackage->permissionState,
                    activePackage->manifest.permissions,
                    activePackage->manifest.optionalPermissions,
                    activePackage->manifest.networkDomains,
                    activePackage->grantedPermissions,
                    activePackage->grantedNetworkDomains);
            blocked = grant.runtimeBlock != snowdesktop::widget::
                PermissionRuntimeBlock::None;
        }

        std::vector<std::wstring> reloadFailures;
        for (const std::wstring& instance : instances)
        {
            if (blocked)
                engine.UnloadWidget(instance);
            else if (!engine.ReloadWidget(instance))
                reloadFailures.push_back(instance);
        }
        if (!reloadFailures.empty())
        {
            std::string rollbackError;
            const bool restored = WidgetEngine::RollbackWidgetPackage(
                packageId, previousVersion, rollbackError);
            if (restored)
            {
                for (const std::wstring& instance : instances)
                    (void)engine.ReloadWidget(instance);
            }
            NotifyHostStateChanged();
            (void)CaptureInstalledState();
            std::wstring message = restored
                ? L("app.settings.widgets_error_version_reload_restored",
                      L"The selected version could not reload every instance; "
                      L"the previous package version was restored.")
                : FormatLocalizedValue(L(
                      "app.settings.widgets_error_version_restore_failed",
                      L"The selected version could not reload every instance, "
                      L"and restoring the previous version also failed: {0}"),
                      Utf8ToWide(rollbackError));
            FinishTask(WidgetsPageHostOperationResult::Failure(
                std::move(message)));
            return false;
        }

        FinishTask(WidgetsPageHostOperationResult::Success(),
            "app.settings.widgets_rollback_ok");
        return true;
    }

    bool PublishDevelopmentPackage(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const InstalledWidgetPackageSnapshot* snapshotPackage =
            FindSnapshotPackage(request.packageId);
        const InstalledPackage* development =
            FindDevelopmentPackage(WideToUtf8(request.packageId));
        if (!snapshotPackage ||
            !snapshotPackage->canPublishDevelopmentPackage ||
            !development || !options.publishDevelopmentPackage)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_publisher_unavailable",
                    L"The Workshop Creator Manager is unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::SynchronizingWorkshop,
            request.packageId);
        WidgetsPageHostOperationResult result;
        try
        {
            result = options.publishDevelopmentPackage(development->root);
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("app.settings.widgets_publisher_launch_failed",
                    L"The Workshop Creator Manager could not be opened."));
        }
        // Launching the external manager does not itself mutate a package.
        if (result.succeeded) result.changed = false;
        const bool succeeded = result.succeeded;
        FinishTask(std::move(result));
        return succeeded;
    }

    bool RefreshAgentSkills()
    {
        if (BusyForMutation()) return ReportBusy();
        CaptureAgentSkillState();
        state->developerActionStatus = L(
            "app.settings.widgets_skill_check_complete",
            L"Agent Skill status refreshed.");
        Publish();
        return true;
    }

    bool SetAgentSkillTargetSelection(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const int mask = std::clamp(request.agentSkillTargetMask,
            0, kAllAgentSkillTargetsMask);
        if (!options.setAgentSkillTargetMask)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_skill_selection_save",
                    L"The Agent Skill selection cannot be saved."),
                "settings.status.error");
            Publish();
            return false;
        }
        bool applied = false;
        try
        {
            applied = options.setAgentSkillTargetMask(mask);
        }
        catch (...)
        {
        }
        if (!applied)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_skill_selection_save",
                    L"The Agent Skill selection could not be saved."),
                "settings.status.error");
            Publish();
            return false;
        }
        CaptureAgentSkillState();
        Publish();
        return true;
    }

    bool ApplyAgentSkillSelection()
    {
        if (BusyForMutation()) return ReportBusy();
        CaptureAgentSkillState();
        int installed = 0;
        int removed = 0;
        int failed = 0;
        std::string firstError;
        for (const auto& status : agentSkillStatuses)
        {
            const int bit = AgentSkillTargetBit(status.agent.kind);
            const bool selected =
                (state->agentSkillTargetMask & bit) != 0;
            using snowdesktop::steam_bridge::SkillInstallState;
            bool targetInstalled =
                status.state == SkillInstallState::Current ||
                status.state == SkillInstallState::UpdateAvailable;
            if (!targetInstalled)
            {
                std::error_code pathError;
                targetInstalled = std::filesystem::exists(
                    status.target, pathError) && !pathError;
            }
            const bool needsInstall =
                status.state == SkillInstallState::NotInstalled ||
                status.state == SkillInstallState::UpdateAvailable;
            if (!(selected && needsInstall) &&
                !(!selected && targetInstalled))
            {
                continue;
            }

            std::string error;
            const bool succeeded = selected
                ? snowdesktop::steam_bridge::InstallOrUpdateAgentSkill(
                    status, error)
                : snowdesktop::steam_bridge::UninstallAgentSkill(
                    status, error);
            if (succeeded)
            {
                selected ? ++installed : ++removed;
            }
            else
            {
                ++failed;
                if (firstError.empty()) firstError = std::move(error);
            }
        }
        CaptureAgentSkillState();
        state->developerActionStatus = FormatAgentSkillCounts(
            L(failed == 0
                    ? "app.settings.widgets_skill_sync_success"
                    : "app.settings.widgets_skill_sync_partial",
                failed == 0
                    ? L"Installed or updated %d target(s) and uninstalled "
                        L"%d target(s). Start a new assistant session to use "
                        L"the update."
                    : L"Installed or updated %d target(s), uninstalled %d "
                        L"target(s), and failed on %d target(s)."),
            installed, removed, failed);
        if (!firstError.empty())
            state->developerActionStatus += L"\n" + Utf8ToWide(firstError);
        Publish();
        return failed == 0;
    }

    bool OpenDevelopmentFolder()
    {
        if (!options.openDevelopmentFolder)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_development_folder_open",
                    L"The development components folder cannot be opened."),
                "settings.status.error");
            Publish();
            return false;
        }
        WidgetsPageHostOperationResult result;
        try
        {
            result = options.openDevelopmentFolder();
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("app.settings.widgets_error_development_folder_open",
                    L"The development components folder cannot be opened."));
        }
        if (!result.succeeded)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(result.message), "settings.status.error");
            Publish();
        }
        return result.succeeded;
    }

    bool PublishDevelopmentWorkspace()
    {
        if (!state->developerPublisherAvailable ||
            !options.publishDevelopmentPackage)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_publisher_launch_failed",
                    L"The Workshop Creator Manager could not be opened."),
                "settings.status.error");
            Publish();
            return false;
        }
        WidgetsPageHostOperationResult result;
        try
        {
            result = options.publishDevelopmentPackage(
                std::filesystem::path{});
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("app.settings.widgets_publisher_launch_failed",
                    L"The Workshop Creator Manager could not be opened."));
        }
        if (!result.succeeded)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(result.message), "settings.status.error");
            Publish();
        }
        return result.succeeded;
    }

    bool ClearWidgetErrors()
    {
        engine.ClearWidgetErrors();
        (void)CaptureInstalledState();
        Publish();
        return true;
    }

    bool OpenWorkshopItem(const WidgetsPageRequest& request)
    {
        const InstalledWidgetPackageSnapshot* package =
            FindSnapshotPackage(request.packageId);
        const bool installedIdentity = package && request.version.empty() &&
            request.sourceId == L"steam-workshop" &&
            !package->workshopExternalItemId.empty() &&
            package->workshopExternalItemId == request.externalItemId;
        const bool failureIdentity = package && std::any_of(
            package->workshopInstallFailures.begin(),
            package->workshopInstallFailures.end(),
            [&](const WidgetWorkshopInstallFailureSnapshot& failure) {
                return failure.sourceId == L"steam-workshop" &&
                    failure.sourceId == request.sourceId &&
                    failure.externalItemId == request.externalItemId &&
                    failure.version == request.version;
            });
        if ((!installedIdentity && !failureIdentity) ||
            request.externalItemId.empty() || !options.openWorkshopItem)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_workshop_item_stale",
                    L"The Workshop item is stale or unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }

        WidgetsPageHostOperationResult result;
        try
        {
            result = options.openWorkshopItem(
                WideToUtf8(request.externalItemId));
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("settings.widgets.workshop.openFailed",
                    L"The Workshop item could not be opened."));
        }
        if (!result.succeeded)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(result.message), "settings.status.error");
            Publish();
        }
        return result.succeeded;
    }

    bool UninstallPackage(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const std::string packageId = WideToUtf8(request.packageId);
        if (!FindManagedPackage(packageId) &&
            !HasInvalidManagedPackage(packageId))
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_uninstall_unavailable",
                    L"This component cannot be uninstalled."),
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::Uninstalling, request.packageId);
        const auto source =
            WidgetEngine::GetWidgetPackageSource(request.packageId);
        std::string workshopExternalItemId;
        if (source && source->providerId == "steam-workshop")
            workshopExternalItemId = source->externalItemId;
        else if (const InstalledWidgetPackageSnapshot* snapshot =
                     FindSnapshotPackage(request.packageId);
                 snapshot && !snapshot->workshopExternalItemId.empty())
        {
            workshopExternalItemId =
                WideToUtf8(snapshot->workshopExternalItemId);
        }
        if (!workshopExternalItemId.empty())
        {
            if (!options.unsubscribeWorkshop)
            {
                FinishTask(WidgetsPageHostOperationResult::Failure(
                    L("app.settings.widgets_error_unsubscribe_unavailable",
                        L"Steam Workshop unsubscription is unavailable.")));
                return false;
            }
            const std::uint64_t taskId = activeTaskId;
            const std::uint64_t taskGeneration = generation;
            const std::uint64_t taskActivation = activation;
            const widgets_page_backend_detail::OutstandingOperationIdentity
                operation{taskGeneration, taskActivation, taskId,
                    widgets_page_backend_detail::OutstandingOperationKind::
                        WorkshopUnsubscribe};
            if (!outstandingOperations.Begin(operation))
            {
                FinishTask(WidgetsPageHostOperationResult::Failure(
                    L("app.settings.widgets_error_operation_tracking",
                        L"The component operation could not be tracked.")));
                return false;
            }
            const std::weak_ptr<Impl> weak = weak_from_this();
            try
            {
                options.unsubscribeWorkshop(taskGeneration, taskId,
                    workshopExternalItemId,
                    [weak, operation](
                        WidgetsPageHostOperationResult result) mutable {
                        const auto self = weak.lock();
                        if (!self) return;
                        self->MarshalToOwner(
                            [weak, operation,
                                result = std::move(result)]() mutable {
                                const auto owner = weak.lock();
                                if (!owner) return;
                                owner->CompleteHostAsyncOperation(operation,
                                    std::move(result),
                                    "app.settings.widgets_uninstall_workshop_ok",
                                    false);
                            });
                    });
                return true;
            }
            catch (...)
            {
                (void)outstandingOperations.Complete(operation);
                FinishTask(WidgetsPageHostOperationResult::Failure(
                    L("app.settings.widgets_error_unsubscribe_failed",
                        L"Steam Workshop could not be unsubscribed.")));
                return false;
            }
        }

        std::vector<std::wstring> instances;
        for (const LuaWidget& widget : engine.GetWidgets())
            if (!widget.preview && widget.packageId == packageId)
                instances.push_back(widget.widgetId);
        for (const std::wstring& instance : instances)
            engine.UnloadWidget(instance);

        std::string error;
        if (!WidgetEngine::UninstallWidgetPackage(packageId, error))
        {
            // UnloadWidget removes runtime records, so ReloadWidget cannot
            // resurrect them directly. Ask DesktopApp to reconcile its
            // persisted instance table against the still-installed package.
            NotifyHostStateChanged();
            FinishTask(WidgetsPageHostOperationResult::Failure(
                Utf8ToWide(error)));
            return false;
        }
        engine.RevokeFilesystemHandlesForPackage(packageId);
        FinishTask(WidgetsPageHostOperationResult::Success(),
            "app.settings.widgets_uninstall_ok");
        return true;
    }

    bool OpenWorkshop(const WidgetsPageRequest& request)
    {
        if (!options.openWorkshop)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_workshop_unavailable",
                    L"Steam Workshop is unavailable."),
                "settings.status.error");
            Publish();
            return false;
        }
        WidgetsPageHostOperationResult result;
        try
        {
            const std::string sourceId = request.sourceId.empty()
                ? "steam-workshop" : WideToUtf8(request.sourceId);
            result = options.openWorkshop(sourceId);
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("settings.widgets.workshop.openFailed",
                    L"Steam Workshop could not be opened."));
        }
        if (!result.succeeded)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                std::move(result.message), "settings.status.error");
            Publish();
        }
        return result.succeeded;
    }

    bool AddPackageToDesktop(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        const auto* package = FindSnapshotPackage(request.packageId);
        const auto* activePackage =
            FindDisplayPackage(WideToUtf8(request.packageId));
        if (!package || !package->canAddToDesktop ||
            !activePackage || !activePackage->active ||
            !activePackage->enabled ||
            !options.addPackageToDesktop)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_add_to_desktop_failed",
                    L"The component could not be added to the desktop."),
                "settings.status.error");
            Publish();
            return false;
        }
        BeginTask(WidgetsPageTaskKind::AddingToDesktop, request.packageId);
        WidgetsPageHostOperationResult result;
        try
        {
            result = options.addPackageToDesktop(request.packageId);
        }
        catch (...)
        {
            result = WidgetsPageHostOperationResult::Failure(
                L("app.settings.widgets_add_to_desktop_failed",
                    L"The component could not be added to the desktop."));
        }
        const bool succeeded = result.succeeded;
        FinishTask(std::move(result),
            "app.settings.widgets_add_to_desktop_ok");
        return succeeded;
    }

    bool SynchronizeSource(const WidgetsPageRequest& request)
    {
        if (BusyForMutation()) return ReportBusy();
        if (!options.synchronizeSource || !options.canSynchronizeSource)
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_source_sync_unavailable",
                    L"This component source cannot be synchronized."),
                "settings.status.error");
            Publish();
            return false;
        }
        const std::string sourceId = WideToUtf8(request.sourceId);
        const auto source = std::find_if(state->sources.begin(),
            state->sources.end(), [&](const WidgetSourceGroupSnapshot& item) {
                return item.sourceId == request.sourceId &&
                    item.supportsSynchronization && item.available;
            });
        if (source == state->sources.end() ||
            !options.canSynchronizeSource(sourceId))
        {
            SetFeedback(WidgetsPageFeedbackSeverity::Error,
                L("app.settings.widgets_error_source_sync_unavailable",
                    L"This component source cannot be synchronized."),
                "settings.status.error");
            Publish();
            return false;
        }

        BeginTask(WidgetsPageTaskKind::SynchronizingWorkshop, {},
            request.sourceId, true);
        const std::uint64_t taskId = activeTaskId;
        const std::uint64_t taskGeneration = generation;
        const std::uint64_t taskActivation = activation;
        const widgets_page_backend_detail::OutstandingOperationIdentity
            operation{taskGeneration, taskActivation, taskId,
                widgets_page_backend_detail::OutstandingOperationKind::
                    SourceSynchronization};
        if (!outstandingOperations.Begin(operation))
        {
            FinishTask(WidgetsPageHostOperationResult::Failure(
                L("app.settings.widgets_error_operation_tracking",
                    L"The component operation could not be tracked.")));
            return false;
        }
        const std::weak_ptr<Impl> weak = weak_from_this();
        try
        {
            options.synchronizeSource(taskGeneration, taskId, sourceId,
                [weak, operation](
                    WidgetsPageHostOperationResult result) mutable {
                    const auto self = weak.lock();
                    if (!self) return;
                    self->MarshalToOwner(
                        [weak, operation,
                            result = std::move(result)]() mutable {
                            if (const auto owner = weak.lock())
                                owner->CompleteHostAsyncOperation(operation,
                                    std::move(result), std::string_view{},
                                    true);
                        });
                });
            return true;
        }
        catch (...)
        {
            (void)outstandingOperations.Complete(operation);
            FinishTask(WidgetsPageHostOperationResult::Failure(
                L("settings.widgets.source.syncFailed",
                    L"The component source could not be synchronized.")));
            return false;
        }
    }

    bool CancelTask(const WidgetsPageRequest& request)
    {
        if (request.taskId == 0 || request.taskId != activeTaskId ||
            !state->task.cancellable)
        {
            return false;
        }
        const std::uint64_t cancelledTask = activeTaskId;
        if (state->task.kind == WidgetsPageTaskKind::Searching)
        {
            if (!sourceWorker.RequestCancel(cancelledTask))
                return false;
            if (!outstandingOperations.Contains(cancelledTask))
                return true;
            // A provider call already in progress may not offer a cooperative
            // cancellation API. Keep the mutation gate closed until its
            // terminal completion instead of allowing overlapping package IO.
            state->task.cancellable = false;
            state->task.status = L(
                "settings.backup.cancelling", L"Canceling…");
            Publish();
            return true;
        }
        const widgets_page_backend_detail::OutstandingOperationIdentity
            sourceSynchronization{
                generation, activation, cancelledTask,
                widgets_page_backend_detail::OutstandingOperationKind::
                    SourceSynchronization};
        if (outstandingOperations.Contains(sourceSynchronization))
        {
            // Steam does not expose a physical query abort. Detach only the
            // visible page task; the exact ledger entry remains busy until
            // the authoritative callback applies host state and arrives here.
            activeTaskId = 0;
            state->task = {};
            Publish();
            return true;
        }
        else if (options.cancelAsyncOperation)
        {
            try
            {
                options.cancelAsyncOperation(generation, cancelledTask);
            }
            catch (...)
            {
            }
        }
        if (outstandingOperations.Contains(cancelledTask))
        {
            // Host cancellation is a request, not a terminal completion.
            // Keep package mutation blocked until the original callback
            // releases the exact generation/activation/task identity.
            state->task.cancellable = false;
            state->task.status = L(
                "settings.backup.cancelling", L"Canceling…");
            Publish();
            return true;
        }
        if (activeTaskId != cancelledTask)
            return true;
        activeTaskId = 0;
        state->task = {};
        Publish();
        return true;
    }

    bool Activate(std::uint64_t value, bool discoverSources)
    {
        if (closed || !OnOwnerThread() || value == 0) return false;
        ++activation;
        active = true;
        if (generation != value)
        {
            generation = value;
            revision = 0;
            requestedSearchRevision = 0;
            requestedSearchQuery.clear();
            state = std::make_shared<WidgetsPageSnapshot>();
            state->generation = generation;
        }
        activeTaskId = 0;
        awaitingPicker = false;
        awaitingConfirmation = false;
        pickerRequestId = 0;
        confirmationRequestId = 0;
        state->task = {};
        ClearFeedback();
        (void)CaptureInstalledState();
        Publish();
        deferredSourceDiscovery = discoverSources &&
            outstandingOperations.Busy();
        if (discoverSources && !deferredSourceDiscovery)
            (void)StartSearch(0, {}, true);
        return true;
    }

    void Deactivate() noexcept
    {
        if (!OnOwnerThread()) return;
        ++activation;
        active = false;
        activeTaskId = 0;
        awaitingPicker = false;
        awaitingConfirmation = false;
        pickerRequestId = 0;
        confirmationRequestId = 0;
        state->task = {};
        state->diagnostics.clear();
        deferredSourceDiscovery = false;
        for (const std::uint64_t taskId : outstandingOperations.Tasks(
                 widgets_page_backend_detail::OutstandingOperationKind::Search))
        {
            (void)sourceWorker.RequestCancel(taskId);
        }
    }

    bool Refresh()
    {
        if (closed || !active || !OnOwnerThread()) return false;
        const bool captured = CaptureInstalledState();
        Publish();
        return captured;
    }

    bool Invoke(std::uint64_t requestGeneration,
        WidgetsPageRequest request)
    {
        if (closed || !active || !OnOwnerThread() ||
            requestGeneration != generation)
        {
            return false;
        }
        switch (request.command)
        {
        case WidgetsPageCommand::Refresh:
            return Refresh();
        case WidgetsPageCommand::BrowseInstallPackage:
            return BrowseInstallPackage();
        case WidgetsPageCommand::SearchSources:
            return StartSearch(request.searchRevision,
                std::move(request.query));
        case WidgetsPageCommand::CancelTask:
            return CancelTask(request);
        case WidgetsPageCommand::InstallCatalogItem:
            return InstallCatalogItem(request);
        case WidgetsPageCommand::RetryWorkshopInstall:
            return RetryWorkshopInstall(request);
        case WidgetsPageCommand::SetPackageEnabled:
            return SetPackageEnabled(request);
        case WidgetsPageCommand::UninstallPackage:
            return UninstallPackage(request);
        case WidgetsPageCommand::SetPermissionDecision:
            return SetPermissionDecision(request);
        case WidgetsPageCommand::SetDevelopmentOverride:
            return SetDevelopmentOverride(request);
        case WidgetsPageCommand::CreateDevelopmentProject:
            return CreateDevelopmentProject(request);
        case WidgetsPageCommand::InstallDevelopmentSnapshot:
            return InstallDevelopmentSnapshot(request);
        case WidgetsPageCommand::RollbackPackage:
            return RollbackPackage(request);
        case WidgetsPageCommand::PublishDevelopmentPackage:
            return PublishDevelopmentPackage(request);
        case WidgetsPageCommand::OpenWorkshop:
            return OpenWorkshop(request);
        case WidgetsPageCommand::OpenWorkshopItem:
            return OpenWorkshopItem(request);
        case WidgetsPageCommand::SynchronizeSource:
            return SynchronizeSource(request);
        case WidgetsPageCommand::AddPackageToDesktop:
            return AddPackageToDesktop(request);
        case WidgetsPageCommand::RefreshAgentSkills:
            return RefreshAgentSkills();
        case WidgetsPageCommand::ApplyAgentSkillSelection:
            return ApplyAgentSkillSelection();
        case WidgetsPageCommand::SetAgentSkillTargetSelection:
            return SetAgentSkillTargetSelection(request);
        case WidgetsPageCommand::OpenDevelopmentFolder:
            return OpenDevelopmentFolder();
        case WidgetsPageCommand::PublishDevelopmentWorkspace:
            return PublishDevelopmentWorkspace();
        case WidgetsPageCommand::ClearWidgetErrors:
            return ClearWidgetErrors();
        default:
            return false;
        }
    }

    void Close() noexcept
    {
        if (closed.exchange(true)) return;
        active = false;
        ++activation;
        activeTaskId = 0;
        awaitingPicker = false;
        awaitingConfirmation = false;
        options.snapshotChanged = {};
        sourceWorker.Shutdown();
        outstandingOperations.Clear();
    }
};

WidgetsPageBackend::WidgetsPageBackend(
    WidgetEngine& engine, WidgetsPageBackendOptions options)
    : impl_(std::make_shared<Impl>(engine, std::move(options)))
{
}

WidgetsPageBackend::~WidgetsPageBackend()
{
    Close();
}

void WidgetsPageBackend::SetSnapshotChangedCallback(
    WidgetsPageBackendOptions::SnapshotChangedCallback callback)
{
    if (impl_ && !impl_->closed && impl_->OnOwnerThread())
        impl_->options.snapshotChanged = std::move(callback);
}

bool WidgetsPageBackend::Activate(
    std::uint64_t generation, bool discoverSources)
{
    return impl_ && impl_->Activate(generation, discoverSources);
}

void WidgetsPageBackend::Deactivate() noexcept
{
    if (impl_) impl_->Deactivate();
}

bool WidgetsPageBackend::Refresh()
{
    return impl_ && impl_->Refresh();
}

bool WidgetsPageBackend::Invoke(
    std::uint64_t generation, WidgetsPageRequest request)
{
    return impl_ && impl_->Invoke(generation, std::move(request));
}

std::shared_ptr<const WidgetsPageSnapshot>
WidgetsPageBackend::Snapshot() const noexcept
{
    return impl_ ? std::make_shared<const WidgetsPageSnapshot>(
                       *impl_->state)
                 : nullptr;
}

std::uint64_t WidgetsPageBackend::Generation() const noexcept
{
    return impl_ ? impl_->generation : 0;
}

bool WidgetsPageBackend::IsGenerationCurrent(
    std::uint64_t generation) const noexcept
{
    return impl_ && impl_->active && !impl_->closed &&
        impl_->generation == generation;
}

void WidgetsPageBackend::Close() noexcept
{
    if (impl_) impl_->Close();
}

} // namespace snowdesktop::winui
