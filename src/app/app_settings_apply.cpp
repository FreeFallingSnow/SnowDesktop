#include "app.h"
#include "../atomic_file.h"
#include "../deployment_context.h"
#include "../http_runtime.h"
#include "../layout_storage.h"
#include "../page_navigation_rules.h"
#include "../settings_update_rules.h"

#include <cwctype>

namespace
{
constexpr wchar_t kAutoStartRunSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kAutoStartRunValue[] = L"SnowDesktop";
constexpr wchar_t kAutoStartApprovalSubKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\"
    L"StartupApproved\\Run";
constexpr wchar_t kSettingsUpdateRequestOwner[] =
    L"snowdesktop.settings.update";

std::wstring CurrentExecutablePath() noexcept
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return {};
    path.resize(length);
    return path;
}

std::wstring RegisteredExecutablePath(std::wstring_view command) noexcept
{
    while (!command.empty() && iswspace(command.front()))
        command.remove_prefix(1);
    if (command.empty()) return {};
    std::wstring path;
    if (command.front() == L'"')
    {
        command.remove_prefix(1);
        const std::size_t end = command.find(L'"');
        if (end == std::wstring_view::npos) return {};
        path.assign(command.substr(0, end));
    }
    else
    {
        const std::size_t end = command.find_first_of(L" \t\r\n");
        path.assign(command.substr(0, end));
    }
    if (path.empty()) return {};
    std::wstring expanded(32768, L'\0');
    const DWORD expandedLength = ExpandEnvironmentStringsW(
        path.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (expandedLength > 0 && expandedLength <= expanded.size())
    {
        expanded.resize(expandedLength - 1);
        path = std::move(expanded);
    }
    return path;
}

bool SameExecutablePath(
    const std::wstring& left, const std::wstring& right) noexcept
{
    if (left.empty() || right.empty()) return false;
    std::error_code error;
    if (std::filesystem::equivalent(left, right, error)) return true;
    error.clear();
    const auto absoluteLeft = std::filesystem::absolute(left, error)
                                  .lexically_normal().wstring();
    if (error) return false;
    const auto absoluteRight = std::filesystem::absolute(right, error)
                                   .lexically_normal().wstring();
    return !error && CompareStringOrdinal(
        absoluteLeft.c_str(), static_cast<int>(absoluteLeft.size()),
        absoluteRight.c_str(), static_cast<int>(absoluteRight.size()),
        TRUE) == CSTR_EQUAL;
}

bool RegistryValueMissing(LONG result) noexcept
{
    return result == ERROR_FILE_NOT_FOUND || result == ERROR_PATH_NOT_FOUND;
}

struct PortableAutoStartRegistration
{
    snowdesktop::PortableAutoStartRegistrationOwner owner =
        snowdesktop::PortableAutoStartRegistrationOwner::Error;
    std::wstring command;
};

PortableAutoStartRegistration QueryPortableAutoStartRegistration() noexcept
{
    using snowdesktop::PortableAutoStartRegistrationOwner;

    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER,
        kAutoStartRunSubKey, 0, KEY_QUERY_VALUE, &key);
    if (RegistryValueMissing(openResult))
        return {PortableAutoStartRegistrationOwner::Missing, {}};
    if (openResult != ERROR_SUCCESS)
        return {PortableAutoStartRegistrationOwner::Error, {}};

    DWORD type = 0;
    DWORD size = 0;
    LONG result = RegQueryValueExW(key, kAutoStartRunValue, nullptr,
        &type, nullptr, &size);
    if (RegistryValueMissing(result))
    {
        RegCloseKey(key);
        return {PortableAutoStartRegistrationOwner::Missing, {}};
    }
    if (result != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ) ||
        size <= sizeof(wchar_t) || size % sizeof(wchar_t) != 0)
    {
        RegCloseKey(key);
        return {PortableAutoStartRegistrationOwner::Error, {}};
    }

    std::wstring command;
    command.resize(size / sizeof(wchar_t));
    result = RegQueryValueExW(key, kAutoStartRunValue, nullptr,
        &type, reinterpret_cast<BYTE*>(command.data()), &size);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS)
        return {PortableAutoStartRegistrationOwner::Error, {}};
    while (!command.empty() && command.back() == L'\0')
        command.pop_back();
    if (command.empty())
        return {PortableAutoStartRegistrationOwner::Error, {}};

    const bool owned = SameExecutablePath(
        RegisteredExecutablePath(command), CurrentExecutablePath());
    return {
        owned ? PortableAutoStartRegistrationOwner::CurrentExecutable
              : PortableAutoStartRegistrationOwner::OtherExecutable,
        std::move(command)};
}

snowdesktop::PortableAutoStartApprovalState
QueryPortableAutoStartApproval() noexcept
{
    using snowdesktop::PortableAutoStartApprovalState;

    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER,
        kAutoStartApprovalSubKey, 0, KEY_QUERY_VALUE, &key);
    if (RegistryValueMissing(openResult))
        return PortableAutoStartApprovalState::Missing;
    if (openResult != ERROR_SUCCESS)
        return PortableAutoStartApprovalState::Error;

    std::array<BYTE, 32> data{};
    DWORD type = 0;
    DWORD size = static_cast<DWORD>(data.size());
    const LONG result = RegQueryValueExW(key, kAutoStartRunValue, nullptr,
        &type, data.data(), &size);
    RegCloseKey(key);
    if (RegistryValueMissing(result))
        return PortableAutoStartApprovalState::Missing;
    if (result != ERROR_SUCCESS || type != REG_BINARY || size == 0)
        return PortableAutoStartApprovalState::Error;
    if (data[0] == 0x02)
        return PortableAutoStartApprovalState::Enabled;
    if (data[0] == 0x03)
        return PortableAutoStartApprovalState::Disabled;
    return PortableAutoStartApprovalState::Error;
}

bool PortableAutoStartApprovalIsActive(
    snowdesktop::PortableAutoStartApprovalState state) noexcept
{
    return state == snowdesktop::PortableAutoStartApprovalState::Missing ||
        state == snowdesktop::PortableAutoStartApprovalState::Enabled;
}

bool ClearPortableAutoStartApproval() noexcept
{
    HKEY key = nullptr;
    const LONG openResult = RegOpenKeyExW(HKEY_CURRENT_USER,
        kAutoStartApprovalSubKey, 0, KEY_SET_VALUE, &key);
    if (RegistryValueMissing(openResult)) return true;
    if (openResult != ERROR_SUCCESS) return false;
    LONG result = RegDeleteValueW(key, kAutoStartRunValue);
    RegCloseKey(key);
    if (RegistryValueMissing(result)) result = ERROR_SUCCESS;
    return result == ERROR_SUCCESS;
}

bool WritePortableAutoStart(bool enabled) noexcept
{
    using snowdesktop::PortableAutoStartRegistrationOwner;

    const PortableAutoStartRegistration existing =
        QueryPortableAutoStartRegistration();
    if (existing.owner == PortableAutoStartRegistrationOwner::Error)
        return false;
    if (existing.owner ==
        PortableAutoStartRegistrationOwner::OtherExecutable)
    {
        return !enabled;
    }
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kAutoStartRunSubKey, 0,
            nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) !=
        ERROR_SUCCESS)
    {
        return false;
    }

    LONG result = ERROR_SUCCESS;
    if (enabled)
    {
        const std::wstring executable = CurrentExecutablePath();
        const std::wstring command = L"\"" + executable + L"\"";
        // Windows limits Run/RunOnce command lines to MAX_PATH characters.
        if (executable.empty() || command.size() >= MAX_PATH)
        {
            RegCloseKey(key);
            return false;
        }
        result = RegSetValueExW(key, kAutoStartRunValue, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(command.c_str()),
            static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
    }
    else
    {
        result = RegDeleteValueW(key, kAutoStartRunValue);
        if (result == ERROR_FILE_NOT_FOUND) result = ERROR_SUCCESS;
    }
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) return false;
    if (!enabled)
    {
        (void)ClearPortableAutoStartApproval();
        return true;
    }
    return ClearPortableAutoStartApproval();
}
} // namespace

// Settings application, desktop passthrough and retained-surface visibility.

snowdesktop::AutoStartQueryResult DesktopApp::QueryAutoStartState()
    const noexcept
{
    using snowdesktop::PortableAutoStartApprovalState;
    using snowdesktop::PortableAutoStartRegistrationOwner;
    using snowdesktop::deployment::PackagedAutoStartState;

    snowdesktop::AutoStartQueryResult result;
    result.packaged = snowdesktop::deployment::IsPackaged();
    const PortableAutoStartRegistration portable =
        QueryPortableAutoStartRegistration();
    result.portableOwner = portable.owner;
    result.portableCommand = portable.command;
    result.portableApproval = QueryPortableAutoStartApproval();

    if (result.packaged)
    {
        const PackagedAutoStartState state =
            snowdesktop::deployment::GetPackagedAutoStartState();
        result.stateKnown = state != PackagedAutoStartState::Unavailable;
        result.enabled = result.stateKnown &&
            snowdesktop::deployment::IsPackagedAutoStartStateEnabled(state);
        return result;
    }

    result.installedPackageEnabled =
        snowdesktop::deployment::IsPackagedAutoStartStateEnabled(
            snowdesktop::deployment::GetInstalledPackagedAutoStartState());
    if (result.portableOwner ==
        PortableAutoStartRegistrationOwner::CurrentExecutable)
    {
        result.stateKnown = result.portableApproval !=
            PortableAutoStartApprovalState::Error;
        result.enabled = result.stateKnown &&
            PortableAutoStartApprovalIsActive(result.portableApproval);
    }
    else
    {
        result.stateKnown = result.portableOwner !=
            PortableAutoStartRegistrationOwner::Error;
        result.enabled = false;
    }
    return result;
}

bool DesktopApp::QueryAutoStartEnabled() const noexcept
{
    const snowdesktop::AutoStartQueryResult result = QueryAutoStartState();
    return result.stateKnown && result.enabled;
}

snowdesktop::AutoStartApplyResult DesktopApp::ApplyAutoStartEnabled(
    bool enabled)
{
    using snowdesktop::AutoStartApplyResult;
    using snowdesktop::AutoStartApplyStatus;
    using snowdesktop::PortableAutoStartApprovalState;
    using snowdesktop::PortableAutoStartRegistrationOwner;
    using snowdesktop::deployment::PackagedAutoStartState;

    const auto finish = [this](AutoStartApplyStatus status,
                            std::wstring message = {})
    {
        AutoStartApplyResult result;
        result.status = status;
        result.state = QueryAutoStartState();
        result.message = std::move(message);
        return result;
    };

    const snowdesktop::AutoStartQueryResult before = QueryAutoStartState();
    if (before.packaged)
    {
        if (enabled && before.portableOwner !=
                PortableAutoStartRegistrationOwner::Missing)
        {
            if (before.portableOwner ==
                    PortableAutoStartRegistrationOwner::Error ||
                before.portableApproval ==
                    PortableAutoStartApprovalState::Error)
            {
                return finish(AutoStartApplyStatus::StateUnavailable,
                    _LW("app.settings.auto_start_enable_failed"));
            }
            // Keep the legacy ownership rule: the packaged version must not
            // enable its StartupTask while any portable Run entry still owns
            // the shared SnowDesktop registration, even if Task Manager has
            // currently marked that entry disabled.
            return finish(
                AutoStartApplyStatus::PortableRegistrationConflict,
                _LFW("app.settings.auto_start_portable_conflict",
                    before.portableCommand));
        }

        const PackagedAutoStartState state =
            snowdesktop::deployment::SetPackagedAutoStartEnabled(enabled);
        const bool actual =
            snowdesktop::deployment::IsPackagedAutoStartStateEnabled(state);
        if (state != PackagedAutoStartState::Unavailable &&
            actual == enabled)
            return finish(AutoStartApplyStatus::Applied);

        const char* key = nullptr;
        AutoStartApplyStatus status = AutoStartApplyStatus::Failed;
        if (enabled && state == PackagedAutoStartState::DisabledByUser)
        {
            key = "app.settings.auto_start_manual_required";
            status = AutoStartApplyStatus::ManualEnableRequired;
        }
        else if (state == PackagedAutoStartState::DisabledByPolicy ||
            state == PackagedAutoStartState::EnabledByPolicy)
        {
            key = enabled ? "app.settings.auto_start_policy_disabled"
                          : "app.settings.auto_start_policy_enabled";
            status = AutoStartApplyStatus::BlockedByPolicy;
        }
        else
        {
            key = enabled
                ? "app.settings.auto_start_enable_failed"
                : "app.settings.auto_start_disable_failed";
            status = state == PackagedAutoStartState::Unavailable
                ? AutoStartApplyStatus::StateUnavailable
                : AutoStartApplyStatus::Failed;
        }
        return finish(status, _LW(key));
    }

    if (before.portableOwner ==
        PortableAutoStartRegistrationOwner::Error)
    {
        return finish(AutoStartApplyStatus::StateUnavailable,
            _LW(enabled
                ? "app.settings.auto_start_enable_failed"
                : "app.settings.auto_start_disable_failed"));
    }
    if (enabled && before.portableOwner ==
            PortableAutoStartRegistrationOwner::OtherExecutable)
    {
        // Even a Task Manager-disabled entry belongs to the other executable.
        // Never overwrite it merely because it is currently inactive.
        return finish(AutoStartApplyStatus::PortableRegistrationConflict,
            _LFW("app.settings.auto_start_portable_conflict",
                before.portableCommand));
    }
    if (enabled && before.portableApproval ==
            PortableAutoStartApprovalState::Error)
    {
        return finish(AutoStartApplyStatus::StateUnavailable,
            _LW("app.settings.auto_start_enable_failed"));
    }
    if (enabled && before.installedPackageEnabled)
    {
        return finish(AutoStartApplyStatus::InstalledRegistrationConflict,
            _LW("app.settings.auto_start_installed_conflict"));
    }
    if (!enabled && before.portableOwner ==
            PortableAutoStartRegistrationOwner::OtherExecutable)
    {
        return finish(AutoStartApplyStatus::Applied);
    }
    if (!WritePortableAutoStart(enabled))
    {
        return finish(AutoStartApplyStatus::Failed,
            _LW(enabled
                ? "app.settings.auto_start_enable_failed"
                : "app.settings.auto_start_disable_failed"));
    }
    const snowdesktop::AutoStartQueryResult after = QueryAutoStartState();
    if (after.stateKnown && after.enabled == enabled)
        return finish(AutoStartApplyStatus::Applied);
    return finish(
        after.stateKnown ? AutoStartApplyStatus::Failed
                         : AutoStartApplyStatus::StateUnavailable,
        _LW(enabled
            ? "app.settings.auto_start_enable_failed"
            : "app.settings.auto_start_disable_failed"));
}

snowdesktop::SettingsActionResult DesktopApp::StartSettingsUpdateCheck()
{
    using snowdesktop::SettingsActionResult;
    using snowdesktop::winui::SettingsUpdateState;

    if (!snowdesktop::deployment::IsPackaged())
    {
        // The portable build has no update row in the legacy settings UI.
        // Keep this typed action inert if a stale accessibility invocation
        // reaches the host; portable settings must not start GitHub HTTP.
        return SettingsActionResult::Success();
    }

    {
        const std::wstring target =
            snowdesktop::deployment::GetStoreProductPageUri();
        if (target.empty() || reinterpret_cast<INT_PTR>(ShellExecuteW(
                controlHwnd_, L"open", target.c_str(), nullptr, nullptr,
                SW_SHOWNORMAL)) <= 32)
        {
            return SettingsActionResult::Failure(
                _LW("app.settings.update_connect_failed"));
        }
        settingsUpdateState_ = SettingsUpdateState::ManagedByStore;
        PublishSettingsUpdateStatus();
        return SettingsActionResult::Success();
    }
}

void DesktopApp::CancelSettingsUpdateCheck() noexcept
{
    if (settingsUpdateRequestId_ != 0 && settingsUpdateHttpService_)
    {
        (void)settingsUpdateHttpService_->Cancel(
            kSettingsUpdateRequestOwner, settingsUpdateRequestId_);
    }
    settingsUpdateRequestId_ = 0;
    settingsUpdateRequestGeneration_ = 0;
    if (settingsUpdateState_ ==
        snowdesktop::winui::SettingsUpdateState::Checking)
    {
        settingsUpdateState_ =
            snowdesktop::winui::SettingsUpdateState::Unknown;
        settingsUpdateDetailKey_.clear();
        PublishSettingsUpdateStatus();
    }
}

void DesktopApp::PrepareSettingsUpdateSession(std::uint64_t generation)
{
    if (generation == 0 || generation == settingsUpdateSessionGeneration_)
        return;
    CancelSettingsUpdateCheck();
    settingsUpdateSessionGeneration_ = generation;
    settingsUpdateAvailableVersion_.clear();
    settingsUpdateDownloadUrl_.clear();
    settingsUpdateDetailKey_.clear();
    settingsUpdateState_ =
        snowdesktop::winui::SettingsUpdateState::Unknown;
    PublishSettingsUpdateStatus();
}

void DesktopApp::PollSettingsUpdateCheck()
{
    if (!settingsUpdateHttpService_) return;

    const auto snapshot = settingsController_
        ? settingsController_->Snapshot() : nullptr;
    if (settingsUpdateRequestId_ != 0 &&
        (!snapshot || !snapshot->sessionActive ||
            snapshot->generation != settingsUpdateRequestGeneration_))
    {
        CancelSettingsUpdateCheck();
    }

    for (HttpResponse& response : settingsUpdateHttpService_->Drain())
    {
        if (response.id != settingsUpdateRequestId_) continue;
        const std::uint64_t generation = settingsUpdateRequestGeneration_;
        settingsUpdateRequestId_ = 0;
        settingsUpdateRequestGeneration_ = 0;
        if (!snapshot || !snapshot->sessionActive ||
            snapshot->generation != generation)
        {
            continue;
        }

        settingsUpdateDetailKey_.clear();
        if (!response.error.empty() || response.status < 200 ||
            response.status >= 300)
        {
            settingsUpdateState_ =
                snowdesktop::winui::SettingsUpdateState::Failed;
            settingsUpdateDetailKey_ =
                "app.settings.update_receive_failed";
        }
        else if (response.body.empty())
        {
            settingsUpdateState_ =
                snowdesktop::winui::SettingsUpdateState::Failed;
            settingsUpdateDetailKey_ =
                "app.settings.update_empty_response";
        }
        else
        {
            const auto release =
                snowdesktop::settings_update_rules::ParseGitHubRelease(
                    response.body, SNOWDESKTOP_VERSION);
            if (!release.parsed)
            {
                settingsUpdateState_ =
                    snowdesktop::winui::SettingsUpdateState::Failed;
                settingsUpdateDetailKey_ =
                    "app.settings.update_parse_failed";
            }
            else
            {
                settingsUpdateState_ = release.updateAvailable
                    ? snowdesktop::winui::SettingsUpdateState::UpdateAvailable
                    : snowdesktop::winui::SettingsUpdateState::UpToDate;
                settingsUpdateAvailableVersion_ =
                    Utf8ToWide(release.version);
                settingsUpdateDownloadUrl_ = release.updateAvailable
                    ? Utf8ToWide(release.downloadUrl)
                    : std::wstring{};
            }
        }
        PublishSettingsUpdateStatus();
    }
}

void DesktopApp::PublishSettingsUpdateStatus()
{
    const auto snapshot = settingsController_
        ? settingsController_->Snapshot() : nullptr;
    if (!snapshot || !snapshot->sessionActive || !settingsWindow_) return;
    snowdesktop::winui::HomeAboutStatusPatch patch;
    patch.generation = snapshot->generation;
    settingsUpdateStatusRevision_ = std::max(
        settingsUpdateStatusRevision_ + 1, snapshot->revision + 1);
    patch.revision = settingsUpdateStatusRevision_;
    patch.packaged = snowdesktop::deployment::IsPackaged();
    patch.updateState = settingsUpdateState_;
    patch.availableVersion = settingsUpdateAvailableVersion_;
    patch.updateDetail = settingsUpdateDetailKey_.empty()
        ? std::wstring{} : _LW(settingsUpdateDetailKey_.c_str());
    patch.animationDiagnosticsEnabled =
        uiAnimationScheduler_.DiagnosticsEnabled();
    patch.animationDiagnosticsStatus =
        BuildAnimationDiagnosticsStatus();
    (void)settingsWindow_->PublishHomeAboutStatus(std::move(patch));
}

std::wstring DesktopApp::BuildAnimationDiagnosticsStatus() const
{
    if (!uiAnimationScheduler_.DiagnosticsEnabled())
        return {};
    const auto metrics = uiAnimationScheduler_.Metrics();
    const auto decimal = [](double value, int precision) {
        wchar_t text[64]{};
        swprintf_s(text, L"%.*f", precision, value);
        return std::wstring(text);
    };
    return _LFW(
        "app.settings.animation_diagnostics_status",
        decimal(metrics.targetRefreshHz, 1),
        decimal(metrics.effectiveRefreshHz, 1),
        std::to_wstring(static_cast<unsigned long long>(
            metrics.requestedFrames)),
        std::to_wstring(static_cast<unsigned long long>(
            metrics.deliveredFrames)),
        std::to_wstring(static_cast<unsigned long long>(
            metrics.skippedFrames)),
        std::to_wstring(static_cast<unsigned long long>(
            metrics.activeAnimations)),
        std::to_wstring(static_cast<unsigned long long>(
            metrics.activeTimers)),
        decimal(metrics.frameIntervalP50Ms, 2),
        decimal(metrics.frameIntervalP95Ms, 2),
        decimal(metrics.frameIntervalP99Ms, 2),
        decimal(metrics.uiWorkP50Ms, 2),
        decimal(metrics.uiWorkP95Ms, 2),
        decimal(metrics.uiWorkP99Ms, 2),
        decimal(metrics.commitP50Ms, 2),
        decimal(metrics.commitP95Ms, 2),
        decimal(metrics.commitP99Ms, 2));
}

snowdesktop::SettingsActionResult DesktopApp::CommitLayoutRestore(
    snowdesktop::winui::LayoutRestorePayload payload)
{
    using snowdesktop::SettingsActionResult;

    if (!settingsController_ || exitRequested_ || reloading_ ||
        shellFileOperationInFlight_ > 0 || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive())
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.busy"));
    }

    // Capture edits made while the worker validated the backup before the
    // live-file transaction begins.
    const SettingsActionResult flushed = settingsController_->FlushAll();
    if (!flushed.Succeeded())
        return flushed;

    std::string validationError;
    if (!snowdesktop::layout_storage::ValidateDocument(
            payload.layoutDocument, &validationError))
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    const std::filesystem::path layoutPath = GetLayoutPath();
    const std::filesystem::path storagePath =
        GetDataFilePath(L"SnowDesktop.storage.json");
    std::string previousLayout;
    if (!snowdesktop::atomic_file::ReadAll(
            layoutPath, previousLayout, &validationError) ||
        !snowdesktop::layout_storage::ValidateDocument(
            previousLayout, &validationError))
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    std::optional<std::string> previousStorage;
    const bool storageExisted =
        GetFileAttributesW(storagePath.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (storageExisted)
    {
        std::string contents;
        if (!snowdesktop::atomic_file::ReadAll(
                storagePath, contents, &validationError))
        {
            return SettingsActionResult::Failure(
                _LW("settings.backup.restoreLayout.commitFailed"));
        }
        previousStorage = std::move(contents);
    }

    std::string commitError;
    if (!snowdesktop::layout_storage::SaveDocument(
            layoutPath, payload.layoutDocument, &commitError))
    {
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    if (payload.storageDocument &&
        !snowdesktop::atomic_file::WriteAll(
            storagePath, *payload.storageDocument, {}, &commitError))
    {
        std::string rollbackError;
        const bool layoutRolledBack =
            snowdesktop::layout_storage::SaveDocument(
                layoutPath, previousLayout, &rollbackError);
        bool storageRolledBack = true;
        if (previousStorage)
        {
            storageRolledBack = snowdesktop::atomic_file::WriteAll(
                storagePath, *previousStorage, {}, &rollbackError);
        }
        else if (!storageExisted)
        {
            std::error_code removeError;
            std::filesystem::remove(storagePath, removeError);
            storageRolledBack = !removeError;
        }
        if (!layoutRolledBack || !storageRolledBack)
        {
            WriteDiagnosticLogEntry(L"Layout restore rollback failed",
                DiagnosticLogLevel::Error);
        }
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }

    // ReloadItems reads both restored documents and writes only the newly
    // reconstructed model. Synchronizing the mirrors prevents a later close
    // from persisting the pre-restore desktop values.
    ReloadItems(true);
    snowdesktop::DesktopDisplaySettings desktop;
    desktop.dockEnabled = generalSettings_.dockEnabled;
    desktop.iconSpacingScale = iconSpacingScale_;
    desktop.itemIconSizeScale = itemIconSizeScale_;
    desktop.itemFontSizeCu = itemFontSizeCu_;
    desktop.listItemFontSizeCu = listItemFontSizeCu_;
    desktop.itemFontWeight = static_cast<int>(itemFontWeight_);
    desktop.shortcutArrowMode = shortcutArrowMode_;
    desktop.iconBeautify = iconBeautifySettings_;
    const bool generalSynchronized =
        settingsController_->SynchronizeGeneral(generalSettings_);
    const bool desktopSynchronized =
        settingsController_->SynchronizeDesktop(std::move(desktop));
    if (!generalSynchronized || !desktopSynchronized)
    {
        WriteDiagnosticLogEntry(
            L"Layout restored but settings mirror synchronization failed",
            DiagnosticLogLevel::Error);
        return SettingsActionResult::Failure(
            _LW("settings.backup.restoreLayout.commitFailed"));
    }
    return SettingsActionResult::Success();
}

class DesktopApp::SettingsHostActionsAdapter final
    : public snowdesktop::SettingsHostActions
{
public:
    explicit SettingsHostActionsAdapter(DesktopApp& app) : app_(app) {}

    snowdesktop::SettingsActionResult OnSettingsPreview(
        const snowdesktop::SettingsSnapshot& snapshot,
        snowdesktop::SettingsDomain domains) override
    {
        using snowdesktop::HasSettingsDomain;
        using snowdesktop::SettingsDomain;

        if (HasSettingsDomain(domains, SettingsDomain::Personalization))
        {
            app_.personalizationSettings_ = snapshot.values.personalization;
            app_.ApplyQuickNavigationAppearance();
            app_.ApplyCollectionPopupAppearance();
            app_.ApplyPersistentDockHostAppearance();
            if (app_.dockSettings_.systemTaskbarFollowPersonalization)
                app_.RefreshSystemTaskbarAppearance(false);
            app_.InvalidateAllWidgetSlots();
            if (app_.hwnd_)
                InvalidateRect(app_.hwnd_, nullptr, FALSE);
        }
        if (HasSettingsDomain(domains, SettingsDomain::Dock))
        {
            // Auto-hide and alignment are committed through the Windows Shell
            // request queue.  Keep their last committed mirrors intact while
            // previewing the remaining Dock appearance and layout values so
            // the commit path can detect a requested system-state change.
            const bool committedTaskbarAutoHide =
                app_.dockSettings_.systemTaskbarAutoHide;
            const int committedTaskbarAlignment =
                app_.dockSettings_.systemTaskbarAlignment;
            app_.dockSettings_ = snapshot.values.dock;
            NormalizeDockSettings(app_.dockSettings_);
            app_.dockSettings_.systemTaskbarAutoHide =
                committedTaskbarAutoHide;
            app_.dockSettings_.systemTaskbarAlignment =
                committedTaskbarAlignment;
            app_.ApplyFloatingDockHotkey();
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            app_.InvalidateDragStaticScene();
            // Taskbar color, opacity, blur, and dynamic-rule sliders are
            // continuous controls.  Apply their coalesced preview immediately
            // while keeping the Windows-owned auto-hide/alignment values at
            // their last committed state above.
            app_.RefreshSystemTaskbarAppearance(false);
            if (app_.hwnd_)
                InvalidateRect(app_.hwnd_, nullptr, TRUE);
        }
        if (HasSettingsDomain(domains, SettingsDomain::Desktop))
        {
            app_.PreviewIconSpacing(
                snapshot.values.desktop.iconSpacingScale);
            app_.PreviewItemIconSize(
                snapshot.values.desktop.itemIconSizeScale);
            app_.PreviewItemFontSize(
                snapshot.values.desktop.itemFontSizeCu);
            app_.PreviewListItemFontSize(
                snapshot.values.desktop.listItemFontSizeCu);
            app_.PreviewItemFontWeight(static_cast<DWRITE_FONT_WEIGHT>(
                snapshot.values.desktop.itemFontWeight));
            app_.SetIconBeautifySettings(
                snapshot.values.desktop.iconBeautify,
                snowdesktop::IconBeautifyUpdateKind::Preview);
        }
        return snowdesktop::SettingsActionResult::Success(domains);
    }

    snowdesktop::SettingsActionResult OnSettingsCommitted(
        const snowdesktop::SettingsSnapshot& snapshot,
        snowdesktop::SettingsDomain domains) override
    {
        using snowdesktop::HasSettingsDomain;
        using snowdesktop::SettingsDomain;

        // Shortcut edits only need to update the in-memory mirror and the
        // relevant registration. Running the full domain pipeline here would
        // rebuild desktop layout and erase the desktop host even though no
        // desktop pixels changed. That erase is visible through the Settings
        // backdrop as a brief black frame when the recorder closes.
        if (domains == SettingsDomain::Navigation &&
            snowdesktop::settings_update_rules::
                IsNavigationShortcutOnlyCommit(
                    app_.navigationSettings_, snapshot.values.navigation))
        {
            app_.navigationSettings_ = snapshot.values.navigation;
            app_.ApplyNavigationHotkey();
            return snowdesktop::SettingsActionResult::Success(domains);
        }
        if (domains == SettingsDomain::General &&
            snowdesktop::settings_update_rules::IsGeneralShortcutOnlyCommit(
                app_.generalSettings_, snapshot.values.general))
        {
            app_.generalSettings_ = snapshot.values.general;
            app_.ApplyDesktopPassthroughHotkey();
            return snowdesktop::SettingsActionResult::Success(domains);
        }
        if (domains == SettingsDomain::Dock &&
            snowdesktop::settings_update_rules::
                IsFloatingDockShortcutOnlyCommit(
                    app_.dockSettings_, snapshot.values.dock))
        {
            app_.dockSettings_ = snapshot.values.dock;
            NormalizeDockSettings(app_.dockSettings_);
            app_.ApplyFloatingDockHotkey();
            return snowdesktop::SettingsActionResult::Success(domains);
        }

        DockSettings requestedDockSettings = snapshot.values.dock;
        NormalizeDockSettings(requestedDockSettings);
        if (HasSettingsDomain(domains, SettingsDomain::Dock))
        {
            const bool autoHideChanged =
                app_.dockSettings_.systemTaskbarAutoHide !=
                    requestedDockSettings.systemTaskbarAutoHide;
            const bool alignmentChanged =
                app_.dockSettings_.systemTaskbarAlignment !=
                    requestedDockSettings.systemTaskbarAlignment;

            // Queue system-owned changes before mutating the application
            // mirror.  A rejected request leaves the Dock domain pending so
            // SettingsController can surface the error and retry safely.
            if (autoHideChanged &&
                !RequestSystemTaskbarAutoHideEnabled(
                    requestedDockSettings.systemTaskbarAutoHide))
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("settings.taskbar.autoHide.queueFailed"),
                    SettingsDomain::Dock);
            }
            if (alignmentChanged &&
                !RequestSystemTaskbarAlignmentCentered(
                    requestedDockSettings.systemTaskbarAlignment == 1))
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("settings.taskbar.alignment.queueFailed"),
                    SettingsDomain::Dock);
            }
        }

        if (HasSettingsDomain(domains, SettingsDomain::Personalization))
        {
            app_.personalizationSettings_ = snapshot.values.personalization;
            app_.ApplyQuickNavigationAppearance();
            app_.ApplyCollectionPopupAppearance();
            app_.ApplyPersistentDockHostAppearance();
            app_.RefreshSystemTaskbarAppearance(false);
            app_.InvalidateAllWidgetSlots();
        }
        if (HasSettingsDomain(domains, SettingsDomain::Dock))
        {
            app_.dockSettings_ = requestedDockSettings;
            app_.ApplyFloatingDockHotkey();
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            app_.SaveLayoutSlots();
            app_.InvalidateDragStaticScene();
            app_.RefreshSystemTaskbarAppearance(true);
        }
        if (HasSettingsDomain(domains, SettingsDomain::Navigation))
        {
            app_.navigationSettings_ = snapshot.values.navigation;
            app_.ApplyNavigationHotkey();
        }
        if (HasSettingsDomain(domains, SettingsDomain::General))
        {
            const bool dockEnabledChanged =
                app_.generalSettings_.dockEnabled !=
                    snapshot.values.general.dockEnabled;
            const bool languageChanged = std::strcmp(
                app_.generalSettings_.language,
                snapshot.values.general.language) != 0;
            app_.generalSettings_ = snapshot.values.general;
            Locale::Instance().SetLanguage(app_.generalSettings_.language);
            app_.SetSoftwareDesktopEnabled(
                app_.generalSettings_.softwareDesktopEnabled, false);
            app_.ApplyDesktopPassthroughHotkey();
            app_.ApplyFloatingDockHotkey();
            if (dockEnabledChanged)
            {
                if (app_.settingsController_)
                {
                    auto desktop = snapshot.values.desktop;
                    desktop.dockEnabled =
                        app_.generalSettings_.dockEnabled;
                    (void)app_.settingsController_->SynchronizeDesktop(
                        std::move(desktop));
                }
                app_.UpdateLayoutWorkArea();
                if (!app_.generalSettings_.dockEnabled)
                    app_.RestoreDockEntriesToDesktop();
                app_.LayoutItems();
                app_.SaveLayoutSlots();
                app_.InvalidateDragStaticScene();
            }
            app_.ApplyQuickNavigationAppearance();
            app_.ApplyCollectionPopupAppearance();
            if (languageChanged)
                app_.ApplyLanguageChange();
        }
        if (HasSettingsDomain(domains, SettingsDomain::Category))
        {
            app_.categorySettings_ = snapshot.values.category;
            NormalizeCategorySettings(app_.categorySettings_);
            for (auto& container : app_.containers_)
            {
                if (auto* categories =
                        dynamic_cast<FileCategories*>(container.get()))
                    categories->InvalidateCategoryCache();
                else if (auto* mapping =
                             dynamic_cast<FolderMapping*>(container.get()))
                    mapping->InvalidateFilterCache();
                else if (auto* group =
                             dynamic_cast<FileGroup*>(container.get()))
                    group->InvalidateHostedView();
            }
        }
        if (HasSettingsDomain(domains, SettingsDomain::Desktop))
        {
            const auto& desktop = snapshot.values.desktop;
            app_.SetIconSpacing(desktop.iconSpacingScale);
            app_.SetItemIconSize(desktop.itemIconSizeScale);
            app_.SetItemFontSize(desktop.itemFontSizeCu);
            app_.SetListItemFontSize(desktop.listItemFontSizeCu);
            app_.SetItemFontWeight(static_cast<DWRITE_FONT_WEIGHT>(
                desktop.itemFontWeight));
            app_.SetShortcutArrowMode(desktop.shortcutArrowMode);
            app_.SetIconBeautifySettings(
                desktop.iconBeautify,
                snowdesktop::IconBeautifyUpdateKind::Commit);
        }
        if (app_.hwnd_)
            InvalidateRect(app_.hwnd_, nullptr, TRUE);
        return snowdesktop::SettingsActionResult::Success(domains);
    }

    snowdesktop::SettingsActionResult OnSettingsRouteChanged(
        const snowdesktop::SettingsRoute& route) override
    {
        // Windows owns these taskbar values. Reconcile them whenever the
        // already-open settings window enters the Taskbar route; reopening the
        // window is not the only path that can expose this page.
        if (route.page == snowdesktop::SettingsPage::Taskbar)
            app_.SyncSystemTaskbarSettingsFromWindows();
        return snowdesktop::SettingsActionResult::Success();
    }

    snowdesktop::SettingsActionResult Invoke(
        const Request& request) override
    {
        switch (request.action)
        {
        case Action::ApplyLanguage:
            app_.ApplyLanguageChange();
            break;
        case Action::RegisterHotkeys:
            app_.ApplyNavigationHotkey();
            app_.ApplyDesktopPassthroughHotkey();
            app_.ApplyFloatingDockHotkey();
            break;
        case Action::ApplyDock:
            app_.ApplyFloatingDockHotkey();
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            break;
        case Action::ApplyTaskbar:
            app_.RefreshSystemTaskbarAppearance(true);
            break;
        case Action::ApplyDesktopLayout:
            app_.UpdateLayoutWorkArea();
            app_.LayoutItems();
            app_.SaveLayoutSlots();
            break;
        case Action::ApplyCategories:
            if (app_.hwnd_)
                InvalidateRect(app_.hwnd_, nullptr, FALSE);
            break;
        case Action::RefreshDesktop:
            app_.ReloadItems();
            break;
        case Action::RefreshWidgets:
            if (app_.widgetEngine_)
            {
                for (const auto& widget : app_.widgets_)
                {
                    if (widget.type == DesktopWidgetType::LuaScript)
                        app_.widgetEngine_->ReloadWidget(widget.id);
                }
            }
            break;
        case Action::AddWidgetToDesktop:
        {
            const size_t previousCount = app_.widgets_.size();
            app_.AddLuaWidgetAt(POINT{ -32000, -32000 }, request.value);
            if (app_.widgets_.size() == previousCount)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The widget could not be added to the desktop.");
            }
            break;
        }
        case Action::ReloadWidgetInstance:
            if (!app_.widgetEngine_ ||
                !app_.widgetEngine_->ReloadWidget(request.widgetInstanceId))
            {
                return snowdesktop::SettingsActionResult::Failure(
                    L"The widget instance could not be reloaded.");
            }
            break;
        case Action::RestartExplorer:
            if (!RestartWindowsExplorer())
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("app.interact.restart_explorer_fail"));
            }
            break;
        case Action::RestartApplication:
            if (!app_.RequestRestart())
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("app.run.restart_failed"));
            }
            break;
        case Action::ExitApplication:
            app_.RequestExit();
            break;
        case Action::OpenDataDirectory:
        {
            const std::wstring path = GetDataDirectoryPath();
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open", path.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("settings.backup.error.openLocation"));
            }
            break;
        }
        case Action::SetAutoStartEnabled:
        {
            const snowdesktop::AutoStartApplyResult result =
                app_.ApplyAutoStartEnabled(request.boolValue);

            // The registry/MSIX StartupTask is authoritative. Publish the
            // actual state even when Windows rejected the requested value so
            // the toggle never remains optimistically out of sync and the
            // General JSON domain never becomes dirty for a system setting.
            app_.generalSettings_.autoStartEnabled =
                result.state.stateKnown && result.state.enabled;
            if (app_.settingsController_)
            {
                (void)app_.settingsController_->SynchronizeGeneral(
                    app_.generalSettings_);
            }
            if (!result.Succeeded())
            {
                return snowdesktop::SettingsActionResult::Failure(
                    result.message);
            }
            break;
        }
        case Action::OpenStartupAppsSettings:
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open", L"ms-settings:startupapps",
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("app.settings.auto_start_enable_failed"));
            }
            break;
        case Action::CheckForUpdates:
            return app_.StartSettingsUpdateCheck();
        case Action::CancelUpdateCheck:
            app_.CancelSettingsUpdateCheck();
            break;
        case Action::OpenProject:
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open",
                    L"https://github.com/FreeFallingSnow/SnowDesktop",
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("settings.about.link.openFailed"));
            }
            break;
        case Action::OpenLicense:
        case Action::OpenThirdPartyNotices:
        {
            const wchar_t* filename = request.action == Action::OpenLicense
                ? L"LICENSE" : L"THIRD_PARTY_NOTICES.md";
            std::filesystem::path target =
                std::filesystem::path(GetExecutableDirectoryPath()) /
                filename;
            if (!std::filesystem::exists(target))
            {
                target = request.action == Action::OpenLicense
                    ? L"https://github.com/FreeFallingSnow/"
                      L"SnowDesktop/blob/main/LICENSE"
                    : L"https://github.com/FreeFallingSnow/"
                      L"SnowDesktop/blob/main/THIRD_PARTY_NOTICES.md";
            }
            if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                    app_.controlHwnd_, L"open", target.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    _LW("settings.about.link.openFailed"));
            }
            break;
        }
        case Action::SetAnimationDiagnostics:
            app_.uiAnimationScheduler_.SetDiagnosticsEnabled(
                request.boolValue);
            app_.PublishSettingsUpdateStatus();
            break;
        case Action::TriggerCrashTest:
            TriggerCrashForTesting();
            break;
        case Action::ProbeHotkeyAvailability:
            if (request.hotkeyTarget == HotkeyTarget::None)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    std::wstring(_LW("app.settings.hotkey_status_in_use")) + L" — " +
                        _LW("app.settings.hotkey_conflict_system"));
            }
            const HotkeyProbeResult hotkeyProbe = ProbeHotkeyAvailability(
                request.hotkeyTarget,
                request.modifiers,
                request.virtualKey);
            if (hotkeyProbe.available)
            {
                return snowdesktop::SettingsActionResult::Success();
            }
            if (hotkeyProbe.conflictTarget != HotkeyTarget::None)
            {
                return snowdesktop::SettingsActionResult::Failure(
                    std::wstring(_LW("app.settings.hotkey_status_conflict")) + L" — " +
                    _LFW("app.settings.hotkey_conflict_with",
                        _LW(HotkeyTargetLabelKey(
                            hotkeyProbe.conflictTarget))));
            }
            return snowdesktop::SettingsActionResult::Failure(
                std::wstring(_LW("app.settings.hotkey_status_in_use")) + L" — " +
                    _LW("app.settings.hotkey_conflict_system"));
        }
        return snowdesktop::SettingsActionResult::Success();
    }

private:
    struct HotkeyProbeResult
    {
        bool available = false;
        HotkeyTarget conflictTarget = HotkeyTarget::None;
    };

    static const char* HotkeyTargetLabelKey(HotkeyTarget target) noexcept
    {
        switch (target)
        {
        case HotkeyTarget::QuickNavigation:
            return "app.settings.quick_navigation";
        case HotkeyTarget::DesktopPassthrough:
            return "app.settings.desktop_passthrough_hotkey";
        case HotkeyTarget::FloatingDock:
            return "app.settings.dock_bar";
        case HotkeyTarget::PagePrevious:
            return "app.settings.page_navigation_previous";
        case HotkeyTarget::PageNext:
            return "app.settings.page_navigation_next";
        case HotkeyTarget::None:
        default:
            return "app.settings.hotkey";
        }
    }

    HotkeyProbeResult ProbeHotkeyAvailability(
        HotkeyTarget target,
        UINT modifiers,
        UINT virtualKey) const
    {
        if (virtualKey == 0)
            return {};

        const UINT normalizedModifiers = modifiers &
            (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN);
        const auto matches = [normalizedModifiers, virtualKey](
            UINT configuredModifiers,
            UINT configuredVirtualKey) {
            return normalizedModifiers ==
                    (configuredModifiers &
                        (MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN)) &&
                virtualKey == configuredVirtualKey;
        };

        const auto conflictsWith = [target, &matches](
            HotkeyTarget configuredTarget,
            bool enabled,
            UINT configuredModifiers,
            UINT configuredVirtualKey) {
            return target != configuredTarget && enabled &&
                matches(configuredModifiers, configuredVirtualKey);
        };
        if (conflictsWith(HotkeyTarget::QuickNavigation,
                app_.navigationSettings_.enabled,
                app_.navigationSettings_.modifiers,
                app_.navigationSettings_.virtualKey))
            return { false, HotkeyTarget::QuickNavigation };
        if (conflictsWith(HotkeyTarget::DesktopPassthrough,
                app_.generalSettings_.desktopPassthroughHotkeyEnabled,
                app_.generalSettings_.desktopPassthroughHotkeyModifiers,
                app_.generalSettings_.desktopPassthroughHotkeyVirtualKey))
            return { false, HotkeyTarget::DesktopPassthrough };
        if (conflictsWith(HotkeyTarget::FloatingDock,
                app_.generalSettings_.dockEnabled &&
                    app_.dockSettings_.floatingShortcutMode,
                app_.dockSettings_.floatingHotkeyModifiers,
                app_.dockSettings_.floatingHotkeyVirtualKey))
            return { false, HotkeyTarget::FloatingDock };
        if (conflictsWith(HotkeyTarget::PagePrevious,
                app_.generalSettings_.pageNavigationKeyboardEnabled,
                app_.generalSettings_.pageNavigationPreviousModifiers,
                app_.generalSettings_.pageNavigationPreviousVirtualKey))
            return { false, HotkeyTarget::PagePrevious };
        if (conflictsWith(HotkeyTarget::PageNext,
                app_.generalSettings_.pageNavigationKeyboardEnabled,
                app_.generalSettings_.pageNavigationNextModifiers,
                app_.generalSettings_.pageNavigationNextVirtualKey))
            return { false, HotkeyTarget::PageNext };

        if ((target == HotkeyTarget::PagePrevious ||
                target == HotkeyTarget::PageNext) &&
            snowdesktop::page_navigation_rules::
                IsReservedDesktopSingleKey(
                    normalizedModifiers, virtualKey))
        {
            return {};
        }
        if (target == HotkeyTarget::PagePrevious ||
            target == HotkeyTarget::PageNext)
        {
            // Page navigation is dispatched inside SnowDesktop's desktop
            // input path. Once reserved keys and application conflicts have
            // been rejected it must not be tested with RegisterHotKey.
            return { true, HotkeyTarget::None };
        }

        switch (target)
        {
        case HotkeyTarget::QuickNavigation:
            if (app_.navigationSettings_.enabled &&
                matches(app_.navigationSettings_.modifiers,
                    app_.navigationSettings_.virtualKey))
            {
                return { app_.navigationHotkeyRegistered_,
                    HotkeyTarget::None };
            }
            break;
        case HotkeyTarget::DesktopPassthrough:
            if (app_.generalSettings_.desktopPassthroughHotkeyEnabled &&
                app_.customDesktopVisible_ &&
                matches(app_.generalSettings_.
                        desktopPassthroughHotkeyModifiers,
                    app_.generalSettings_.
                        desktopPassthroughHotkeyVirtualKey))
            {
                return { app_.desktopPassthroughHotkeyRegistered_,
                    HotkeyTarget::None };
            }
            break;
        case HotkeyTarget::FloatingDock:
            if (app_.generalSettings_.dockEnabled &&
                app_.dockSettings_.floatingShortcutMode &&
                matches(app_.dockSettings_.floatingHotkeyModifiers,
                    app_.dockSettings_.floatingHotkeyVirtualKey))
            {
                return { app_.floatingDockHotkeyRegistered_,
                    HotkeyTarget::None };
            }
            break;
        case HotkeyTarget::PagePrevious:
        case HotkeyTarget::PageNext:
            return { true, HotkeyTarget::None };
        case HotkeyTarget::None:
            return {};
        }

        HWND probeWindow =
            app_.controlHwnd_ && IsWindow(app_.controlHwnd_)
                ? app_.controlHwnd_
                : (app_.inputHwnd_ && IsWindow(app_.inputHwnd_)
                    ? app_.inputHwnd_ : app_.hwnd_);
        if (!probeWindow || !IsWindow(probeWindow))
            return {};

        const BOOL registered = RegisterHotKey(
            probeWindow,
            kSettingsHotkeyProbeId,
            normalizedModifiers | MOD_NOREPEAT,
            virtualKey);
        if (!registered)
            return {};

        UnregisterHotKey(probeWindow, kSettingsHotkeyProbeId);
        return { true, HotkeyTarget::None };
    }

    DesktopApp& app_;
};

void DesktopApp::InitializeSettingsController()
{
    settingsHostActions_ =
        std::make_unique<SettingsHostActionsAdapter>(*this);
    settingsController_ = std::make_unique<snowdesktop::SettingsController>(
        snowdesktop::CreateNativeSettingsStore(),
        settingsHostActions_.get());

    const snowdesktop::SettingsActionResult result =
        settingsController_->Initialize();
    const auto snapshot = settingsController_->Snapshot();
    if (snapshot && snapshot->initialized)
    {
        personalizationSettings_ = snapshot->values.personalization;
        dockSettings_ = snapshot->values.dock;
        navigationSettings_ = snapshot->values.navigation;
        generalSettings_ = snapshot->values.general;
        categorySettings_ = snapshot->values.category;
        generalSettings_.autoStartEnabled = QueryAutoStartEnabled();
        (void)settingsController_->SynchronizeGeneral(generalSettings_);
    }
    settingsUpdateState_ =
        snowdesktop::winui::SettingsUpdateState::Unknown;
    if (!result.Succeeded())
    {
        std::wstring message =
            L"SettingsController initialized with recoverable load errors";
        if (!result.message.empty())
        {
            message += L": ";
            message += result.message;
        }
        WriteDiagnosticLogEntry(message.c_str());
    }
}

void DesktopApp::ShowSettingsWindow(snowdesktop::SettingsRoute route)
{
    settingsWindowOpenRequest_.Request(std::move(route));
    TryShowPendingSettingsWindow();
}

bool DesktopApp::IsSettingsApplicationWindow(HWND window) const noexcept
{
    if (!window || !settingsWindow_)
        return false;
    const HWND settings = settingsWindow_->Window();
    if (!settings || !IsWindow(settings))
        return false;

    HWND root = GetAncestor(window, GA_ROOT);
    if (!root)
        root = window;
    HWND rootOwner = GetAncestor(window, GA_ROOTOWNER);
    if (!rootOwner)
        rootOwner = root;
    return root == settings || rootOwner == settings;
}

void DesktopApp::TryShowPendingSettingsWindow()
{
    if (!settingsWindowOpenRequest_.Pending() ||
        !startupInitializationComplete_)
        return;

    const snowdesktop::SettingsRoute route =
        settingsWindowOpenRequest_.Route();
    const bool shown = settingsWindow_ && settingsWindow_->Open(route);
    if (shown)
    {
        settingsWindowOpenRequest_.MarkShown();
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kSettingsWindowRetryTimerId);
        RefreshDockRunningWindows();
        WriteDiagnosticLogEntry(L"SettingsWindow shown");
        return;
    }

    if (settingsWindowOpenRequest_.RecordFailure(
            kSettingsWindowMaximumAutomaticRetries) &&
        controlHwnd_ && IsWindow(controlHwnd_))
    {
        if (SetTimer(controlHwnd_, kSettingsWindowRetryTimerId,
                kSettingsWindowRetryIntervalMs, nullptr) != 0)
        {
            WriteDiagnosticLogEntry(
                L"SettingsWindow show failed; retry scheduled");
            return;
        }
        wchar_t message[192]{};
        swprintf_s(message,
            L"SettingsWindow retry timer failed (error=%lu)",
            GetLastError());
        WriteDiagnosticLogEntry(message);
    }

    WriteDiagnosticLogEntry(
        L"SettingsWindow show failed; request remains pending");
}

/**
 * @brief 加载导航设置并应用热键注册
 */
void DesktopApp::LoadNavigationSettingsAndApply()
{
    NavigationSettings settings;
    LoadNavigationSettings(GetNavigationSettingsPath().c_str(), settings);
    navigationSettings_ = settings;
    ApplyNavigationHotkey();
}

bool DesktopApp::IsDesktopPassthroughHotkeyDown() const
{
    const auto keyDown = [](int virtualKey) {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    };
    if (!keyDown(static_cast<int>(
            generalSettings_.desktopPassthroughHotkeyVirtualKey)))
        return false;

    const UINT modifiers =
        generalSettings_.desktopPassthroughHotkeyModifiers;
    if ((modifiers & MOD_CONTROL) != 0 && !keyDown(VK_CONTROL))
        return false;
    if ((modifiers & MOD_ALT) != 0 && !keyDown(VK_MENU))
        return false;
    if ((modifiers & MOD_SHIFT) != 0 && !keyDown(VK_SHIFT))
        return false;
    if ((modifiers & MOD_WIN) != 0 &&
        !keyDown(VK_LWIN) && !keyDown(VK_RWIN))
        return false;
    return true;
}

bool DesktopApp::IsDesktopPassthroughPointerDown() const
{
    constexpr int pointerKeys[] = {
        VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
        VK_XBUTTON1, VK_XBUTTON2
    };
    for (const int virtualKey : pointerKeys)
    {
        if ((GetAsyncKeyState(virtualKey) & 0x8000) != 0)
            return true;
    }
    return false;
}

void DesktopApp::EndDesktopPassthroughHold(
    bool restoreDesktop)
{
    if (desktopPassthroughHotkeyHwnd_ &&
        IsWindow(desktopPassthroughHotkeyHwnd_))
    {
        KillTimer(desktopPassthroughHotkeyHwnd_,
            kDesktopPassthroughHoldTimerId);
    }

    if (!desktopPassthroughHoldActive_)
        return;
    desktopPassthroughHoldActive_ = false;

    if (!restoreDesktop || !customDesktopVisible_ ||
        !hwnd_ || !IsWindow(hwnd_))
        return;

    ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
    desktopBackdropCompositor_.SetVisible(true);
    ReconcileDesktopHoverState(
        snowdesktop::desktop_hover_rules::
            ReconcileMode::AllowImmediateActivation);
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
}

void DesktopApp::BeginDesktopPassthroughHold()
{
    if (desktopPassthroughHoldActive_ ||
        !desktopPassthroughHotkeyRegistered_ ||
        !generalSettings_.desktopPassthroughHotkeyEnabled ||
        !customDesktopVisible_ ||
        !hwnd_ || !IsWindow(hwnd_) ||
        !desktopPassthroughHotkeyHwnd_ ||
        !IsWindow(desktopPassthroughHotkeyHwnd_))
        return;

    // Hiding in the middle of a desktop drag would prevent SnowDesktop from
    // receiving the matching button-up event and leave its interaction state
    // latched. The shortcut can be pressed again after the current gesture.
    if (IsDesktopPassthroughPointerDown() ||
        mouseDown_ || marqueeActive_ ||
        dragSession_.IsActive() ||
        dragDropController_.IsTransportActive() ||
        GetCapture() != nullptr)
        return;

    if (SetTimer(desktopPassthroughHotkeyHwnd_,
            kDesktopPassthroughHoldTimerId,
            kDesktopPassthroughHoldIntervalMs,
            nullptr) == 0)
        return;

    if (quickNavigationOpen_)
    {
        CloseQuickNavigation();
        FinalizeCloseQuickNavigation();
    }
    HideDockWindowPreview();
    HideDragHintWindow();

    desktopPassthroughHoldActive_ = true;
    CloseAllFloatingDocksThen(
        [this]() {
            // The hotkey may have been released while the compositor hand-off
            // was pending. In that case the desktop must remain visible.
            if (!desktopPassthroughHoldActive_ ||
                !hwnd_ || !IsWindow(hwnd_))
                return;
            if (widgetEngine_)
                widgetEngine_->SetAllWidgetDesktopVisible(false);
            desktopBackdropCompositor_.SetVisible(false);
            ShowWindow(hwnd_, SW_HIDE);
        });
}

void DesktopApp::UnregisterDesktopPassthroughHotkey()
{
    EndDesktopPassthroughHold();
    if (desktopPassthroughHotkeyRegistered_ &&
        desktopPassthroughHotkeyHwnd_)
    {
        UnregisterHotKey(desktopPassthroughHotkeyHwnd_,
            kDesktopPassthroughHotkeyId);
    }
    desktopPassthroughHotkeyRegistered_ = false;
    desktopPassthroughHotkeyHwnd_ = nullptr;
}

void DesktopApp::ApplyDesktopPassthroughHotkey()
{
    UnregisterDesktopPassthroughHotkey();
    if (!generalSettings_.desktopPassthroughHotkeyEnabled ||
        !customDesktopVisible_ ||
        generalSettings_.desktopPassthroughHotkeyVirtualKey == 0)
        return;

    HWND target =
        controlHwnd_ && IsWindow(controlHwnd_)
            ? controlHwnd_
            : (inputHwnd_ && IsWindow(inputHwnd_)
                ? inputHwnd_ : hwnd_);
    if (!target)
        return;

    const UINT modifiers =
        generalSettings_.desktopPassthroughHotkeyModifiers |
        MOD_NOREPEAT;
    desktopPassthroughHotkeyRegistered_ =
        RegisterHotKey(target, kDesktopPassthroughHotkeyId,
            modifiers,
            generalSettings_.desktopPassthroughHotkeyVirtualKey) != FALSE;
    if (desktopPassthroughHotkeyRegistered_)
    {
        desktopPassthroughHotkeyHwnd_ = target;
        WriteDiagnosticLogEntry(
            L"Desktop passthrough hold hotkey registered");
    }
    else
    {
        WriteDiagnosticLogEntry(
            L"Desktop passthrough hold hotkey registration failed");
    }
}

void DesktopApp::LoadGeneralSettingsAndApply()
{
    const bool dockEnabled = generalSettings_.dockEnabled;
    const bool demoModeEnabled = generalSettings_.demoModeEnabled;
    GeneralSettings settings;
    LoadGeneralSettings(GetGeneralSettingsPath().c_str(), settings);
    generalSettings_ = settings;
    if (std::strcmp(generalSettings_.language, "system") != 0 &&
        !Locale::Instance().HasLanguage(generalSettings_.language))
    {
        std::strncpy(generalSettings_.language, "system",
            sizeof(generalSettings_.language) - 1);
        generalSettings_.language[sizeof(generalSettings_.language) - 1] = '\0';
    }
    Locale::Instance().SetLanguage(generalSettings_.language);
    generalSettings_.dockEnabled = dockEnabled;
    generalSettings_.quickNavTheme =
        NormalizeFourThemeSelection(generalSettings_.quickNavTheme);
    generalSettings_.collectionPopupTheme =
        NormalizeFourThemeSelection(
            generalSettings_.collectionPopupTheme);
    SetSoftwareDesktopEnabled(generalSettings_.softwareDesktopEnabled, false);
    ApplyQuickNavigationAppearance();
    ApplyCollectionPopupAppearance();
    if (demoModeEnabled != generalSettings_.demoModeEnabled)
    {
        InvalidateDragStaticScene();
        InvalidateDockContainers();
        if (hwnd_ && IsWindow(hwnd_))
            InvalidateRect(hwnd_, nullptr, TRUE);
        InvalidateFloatingDockWindow(false);
    }
}

void DesktopApp::ApplyQuickNavigationAppearance()
{
    const PersonalizationSettings globalAppearance = CurrentPersonalization();
    const int presetId = globalAppearance.backgroundPreset == kAppearancePresetCustom
        ? AppearancePresetFromFourThemeSelection(
            generalSettings_.quickNavTheme)
        : globalAppearance.backgroundPreset;
    const PersonalizationSettings appearance =
        MakeQuickNavigationAppearancePreset(presetId);

    const float luminance = appearance.widgetBgR * 0.2126f +
        appearance.widgetBgG * 0.7152f + appearance.widgetBgB * 0.0722f;
    quickNavLightTheme_ = (presetId == kAppearancePresetLight ||
        presetId == kAppearancePresetAcrylicLight) ||
        luminance >= 0.55f;
    quickNavGlassTheme_ = appearance.glassEnabled;
    quickNavBlurRadius_ = std::clamp(appearance.glassBlurRadius, 4.0f, 48.0f);
    quickNavAppearance_ = appearance;
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        UpdateQuickNavigationBackdrop();
}

void DesktopApp::ApplyCollectionPopupAppearance()
{
    const PersonalizationSettings globalAppearance = CurrentPersonalization();

    const int selection =
        globalAppearance.backgroundPreset == kAppearancePresetCustom
        ? NormalizeFourThemeSelection(
            generalSettings_.collectionPopupTheme)
        : FourThemeSelectionFromAppearancePreset(
            NormalizeAppearancePresetId(
                globalAppearance.backgroundPreset));
    const int presetId =
        AppearancePresetFromFourThemeSelection(selection);
    collectionPopupAppearance_ =
        MakeQuickNavigationAppearancePreset(presetId);
    collectionPopupLightTheme_ =
        collectionPopupAppearance_.contentTheme == 1;
    collectionPopupGlassTheme_ =
        collectionPopupAppearance_.glassEnabled;
    collectionPopupBlurRadius_ = std::clamp(
        collectionPopupAppearance_.glassBlurRadius,
        4.0f, 48.0f);

    UpdateCollectionPopupBackdrop();
    if (GetOpenPopupWidget())
        InvalidateFloatingPopupWindow(true);
}

void DesktopApp::LoadDockSettingsAndApply()
{
    DockSettings settings;
    LoadDockSettings(GetDockSettingsPath().c_str(), settings);
    NormalizeDockSettings(settings);
    dockSettings_ = settings;
    SyncSystemTaskbarSettingsFromWindows();
    ApplyFloatingDockHotkey();
    systemTaskbarWindowStateChangedTick_.fetch_add(1,
        std::memory_order_relaxed);
    RefreshSystemTaskbarAppearance(true);
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::SyncSystemTaskbarSettingsFromWindows()
{
    // During Explorer restart ABM_GETSTATE returns zero before Shell_TrayWnd
    // exists. Treat that interval as unavailable, not as an external request
    // to disable auto-hide and overwrite the saved software mirror.
    if (!FindWindowW(L"Shell_TrayWnd", nullptr))
        return;

    const bool autoHide = IsSystemTaskbarAutoHideEnabled();
    const int alignment = IsSystemTaskbarAlignmentCentered() ? 1 : 0;
    bool dockDraftPending = false;

    // Auto-hide and alignment belong to Windows, but the rest of Dock can have
    // an unrelated draft. Reconcile only these two fields; the controller
    // protects either field when the user has edited it in this session.
    if (settingsController_)
    {
        const auto snapshot = settingsController_->Snapshot();
        if (snapshot)
        {
            if (snapshot->externalReplacementPending)
                return;
            dockDraftPending = snowdesktop::HasSettingsDomain(
                snapshot->dirtyDomains,
                snowdesktop::SettingsDomain::Dock);
        }
        if (!settingsController_->SynchronizeSystemTaskbarState(
                autoHide, alignment == 1))
            return;
    }

    const bool persistedMirrorChanged =
        dockSettings_.systemTaskbarAutoHide != autoHide ||
        dockSettings_.systemTaskbarAlignment != alignment;
    dockSettings_.systemTaskbarAutoHide = autoHide;
    dockSettings_.systemTaskbarAlignment = alignment;
    // Avoid persisting other in-memory Dock previews ahead of their commit.
    // A later successful Dock commit writes the reconciled system mirror too.
    if (persistedMirrorChanged && !dockDraftPending)
        SaveDockSettings(GetDockSettingsPath().c_str(), dockSettings_);
}

void DesktopApp::LoadCategorySettingsAndApply()
{
    CategorySettings settings = CategorySettings::Defaults();
    LoadCategorySettings(GetCategorySettingsPath().c_str(), settings);
    categorySettings_ = settings;

    for (auto& c : containers_)
    {
        if (auto* fc = dynamic_cast<FileCategories*>(c.get()))
            fc->InvalidateCategoryCache();
        else if (auto* mapping =
                     dynamic_cast<FolderMapping*>(c.get()))
            mapping->InvalidateFilterCache();
        else if (auto* group =
                     dynamic_cast<FileGroup*>(c.get()))
            group->InvalidateHostedView();
    }
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

void DesktopApp::ApplyLanguageChange()
{
    LoadCategorySettingsAndApply();
    const bool widgetRuntimeReloadAllowed = !settingsWindow_ ||
        settingsWindow_->PrepareLanguageChange();
    PublishSettingsUpdateStatus();
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        SetWindowTextW(quickNavigationHwnd_, _LW("app.interact.snow_nav_title"));
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(_LW("app.nav.search_hint")));
    }

    bool titleChanged = false;
    for (auto& widget : widgets_)
    {
        std::wstring defaultTitle;
        switch (widget.type)
        {
        case DesktopWidgetType::Collection:
            defaultTitle = _LW("widget.collection");
            break;
        case DesktopWidgetType::CollectionGroup:
            defaultTitle = _LW("widget.collection_group");
            break;
        case DesktopWidgetType::FileGroup:
            defaultTitle = _LW("widget.file_group");
            break;
        case DesktopWidgetType::FileCategories:
            defaultTitle = _LW("widget.desktop_files");
            break;
        case DesktopWidgetType::Guide:
            defaultTitle = _LW("app.guide.title");
            break;
        case DesktopWidgetType::LuaScript:
            if (widgetEngine_ && !widget.packageId.empty())
            {
                if (widgetRuntimeReloadAllowed)
                {
                    if (!widgetEngine_->ReloadWidget(widget.id))
                    {
                        widgetEngine_->EnsureWidgetLoaded(
                            widget.id, widget.packageId);
                    }
                    widgetEngine_->NotifyLanguageChanged(widget.id);
                }
                const auto& runtimeWidgets = widgetEngine_->GetWidgets();
                auto runtime = std::find_if(runtimeWidgets.begin(), runtimeWidgets.end(),
                    [&](const LuaWidget& loaded) {
                        return loaded.widgetId == widget.id;
                    });
                if (runtime != runtimeWidgets.end())
                    defaultTitle = Utf8ToWide(runtime->name);
            }
            break;
        case DesktopWidgetType::FolderMapping:
        default:
            break;
        }

        if (widget.customTitle.empty() &&
            !defaultTitle.empty() &&
            (widget.type != DesktopWidgetType::LuaScript ||
                widget.scriptTitle.empty()) &&
            widget.title != defaultTitle)
        {
            widget.title = std::move(defaultTitle);
            titleChanged = true;
        }
    }

    if (settingsWindow_)
    {
        settingsWindow_->ApplyLanguageChange(
            widgetRuntimeReloadAllowed);
    }

    if (titleChanged)
        SaveLayoutSlots();
    if (quickNavigationOpen_)
        InvalidateQuickNavigationWindow();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

void DesktopApp::ToggleDesktopIconsVisibility()
{
    desktopIconsHidden_ = !desktopIconsHidden_;
    // The control-window timer also maintains the Explorer taskbar hook and
    // the blurred desktop background. Keep it alive while icons are hidden.
    ClearHiddenHint();

    if (desktopIconsHidden_)
    {
        if (GetOpenPopupWidget() && !IsOpenPopupRetained())
            CloseCollectionPopup();
        if (!luaWidgetPanelRequest_.widgetId.empty())
        {
            const auto source = std::find_if(
                widgets_.begin(), widgets_.end(),
                [&](const DesktopWidget& widget) {
                    return widget.id ==
                        luaWidgetPanelRequest_.widgetId;
                });
            if (source == widgets_.end() ||
                !source->keepWhenDesktopHidden)
            {
                CloseLuaWidgetPanel(
                    luaWidgetPanelRequest_.widgetId,
                    "desktop-hidden");
            }
        }
    }

    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
    UpdatePersistentDockHostVisibility();
}

bool DesktopApp::HasRetainedElements() const
{
    if (dockSettings_.keepWhenDesktopHidden)
    {
        for (const auto& container : containers_)
            if (dynamic_cast<DockContainer*>(container.get()))
                return true;
    }
    for (const auto& widgetData : widgets_)
        if (widgetData.keepWhenDesktopHidden &&
            !IsRectEmptyRect(widgetData.bounds))
            return true;
    return false;
}

bool DesktopApp::IsOpenPopupRetained() const
{
    if (!desktopIconsHidden_)
        return GetOpenPopupWidget() != nullptr;
    if (!GetOpenPopupWidget())
        return false;
    if (dockFolderPopupOpen_ || popupAnchoredToDock_)
        return dockSettings_.keepWhenDesktopHidden ||
            (collectionPopupDockHost_ &&
                IsPersistentDockHostEffectivelyFloating(
                    *collectionPopupDockHost_));
    return popupWidgetIndex_ < widgets_.size() &&
        widgets_[popupWidgetIndex_].keepWhenDesktopHidden;
}

bool DesktopApp::IsRetainedContainer(
    const Container* container) const
{
    if (!container)
        return false;
    if (!desktopIconsHidden_)
        return true;
    if (dynamic_cast<const DockContainer*>(container))
        return dockSettings_.keepWhenDesktopHidden ||
            IsDockContainerEffectivelyFloating(
                static_cast<const DockContainer*>(container));
    if (container == dockFolderPopupContainer_.get())
        return dockSettings_.keepWhenDesktopHidden;
    const auto* widget =
        dynamic_cast<const WidgetContainer*>(container);
    const DesktopWidget* widgetData = widget
        ? widget->GetWidgetData()
        : nullptr;
    if (widgetData && popupAnchoredToDock_ &&
        dockSettings_.keepWhenDesktopHidden &&
        GetOpenPopupWidget() == widgetData)
        return true;
    return widgetData && widgetData->keepWhenDesktopHidden;
}

bool DesktopApp::IsPointOnRetainedElement(POINT pt) const
{
    if (IsOpenPopupRetained() &&
        IsPointInsideOpenPopup(pt))
        return true;
    if (const DockContainer* dock =
            GetDockContainerAtPoint(pt);
        dock &&
        (dockSettings_.keepWhenDesktopHidden ||
            IsDockContainerEffectivelyFloating(dock)))
        return true;
    for (const auto& widgetData : widgets_)
    {
        if (!widgetData.keepWhenDesktopHidden) continue;
        if (luaWidgetPanelRequest_.widgetId == widgetData.id &&
            luaWidgetPanelAnimation_.IsInteractive())
        {
            const RECT panel = GetLuaWidgetPanelRect();
            if (!IsRectEmptyRect(panel) && PtInRect(&panel, pt))
                return true;
        }
        const size_t standalone =
            HitTestStandaloneWidgetIndex(pt);
        if (standalone < widgets_.size() &&
            &widgets_[standalone] == &widgetData)
            return true;
        if (!IsRectEmptyRect(widgetData.bounds) &&
            PtInRect(&widgetData.bounds, pt))
            return true;
        for (const auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgetData) continue;
            const RECT bodyRect = wc->GetBodyRect();
            if (PtInRect(&bodyRect, pt))
                return true;
            break;
        }
    }
    return false;
}

void DesktopApp::ShowHiddenHint()
{
    if (!generalSettings_.doubleClickHideDesktop) return;
    showHiddenHint_ = true;
    hiddenHintStartTick_ = GetTickCount();
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kHiddenHintTimerId, 100, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopApp::ClearHiddenHint()
{
    showHiddenHint_ = false;
    hiddenHintStartTick_ = 0;
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kHiddenHintTimerId);
}

void DesktopApp::ShowWidgetAddedHint()
{
    showWidgetAddedHint_ = true;
    widgetAddedHintStartTick_ = GetTickCount();
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kWidgetAddedHintTimerId, 100, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

void DesktopApp::ClearWidgetAddedHint()
{
    showWidgetAddedHint_ = false;
    widgetAddedHintStartTick_ = 0;
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kWidgetAddedHintTimerId);
}

/**
 * @brief 刷新拖拽目标：根据鼠标位置更新目标容器、槽位和区域
 * @param clientPoint 客户端坐标点
 * @param mods 修饰键状态
 */
