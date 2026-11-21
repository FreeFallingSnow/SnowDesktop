#include "app/app.h"

#include "common/constants.h"
#include "common/l10n.h"
#include "common/utils.h"
#include "ui/input/private_pointer.h"
#include "ui/preview/component_preview.h"
#include "widget/runtime/widget_engine.h"

#include <commctrl.h>
#include <objbase.h>
#include <algorithm>
#include <set>
#include <thread>

namespace
{
bool NeedsNativeHelperConsent(const snowdesktop::widget::InstalledPackage& package)
{
    if (package.manifest.nativeHelpers.empty()) return false;
    if (package.permissionState != snowdesktop::widget::PermissionDecisionState::Granted) return true;
    const bool required = std::find(package.manifest.permissions.begin(), package.manifest.permissions.end(),
        "native.execute") != package.manifest.permissions.end();
    return required && std::find(package.grantedPermissions.begin(), package.grantedPermissions.end(),
        "native.execute") == package.grantedPermissions.end();
}

const wchar_t* WidgetPermissionLabel(std::string_view permission)
{
    if (const char* key = snowdesktop::widget::
            WidgetPermissionLabelLocalizationKey(permission))
        return _LW(key);
    static thread_local std::wstring fallback;
    fallback = Utf8ToWide(std::string(permission));
    return fallback.c_str();
}

enum class WidgetConsentChoice
{
    AllowAll,
    AllowRequired,
    Deny,
    Cancel,
};

struct WidgetConsentDialogPresentation
{
    std::wstring title;
    std::wstring instruction;
    std::wstring content;
    std::wstring allowAllLabel;
    std::wstring allowRequiredLabel;
    std::wstring denyLabel;
    bool showRequiredOnly = false;
};

WidgetConsentDialogPresentation BuildWidgetConsentDialogPresentation(
    const snowdesktop::widget::InstalledPackage& package,
    const std::wstring& displayName,
    std::span<const std::string> requiredPermissions,
    std::span<const std::string> optionalPermissions)
{
    WidgetConsentDialogPresentation presentation;
    presentation.title = _LW("app.widget_permission.title");
    presentation.instruction = _LFW(
        "app.widget_permission.instruction", displayName);
    presentation.allowAllLabel = _LW("app.widget_permission.allow");
    presentation.allowRequiredLabel =
        _LW("app.widget_permission.allow_required");
    presentation.denyLabel = _LW("app.widget_permission.deny");
    presentation.showRequiredOnly = !optionalPermissions.empty();
    std::wstring content;
    if (package.builtin)
        content = _LW("app.widget_permission.source_builtin");
    else if (package.development)
        content = _LW("app.widget_permission.source_development");
    else
        content = _LFW("app.widget_permission.source_external",
            Utf8ToWide(package.source.providerId));
    content += L"\n\n";
    if (!requiredPermissions.empty())
    {
        content += _LW("app.widget_permission.permissions_required_heading");
        for (const auto& permission : requiredPermissions)
        {
            content += L"\n  • ";
            content += WidgetPermissionLabel(permission);
        }
    }
    if (!optionalPermissions.empty())
    {
        if (!requiredPermissions.empty()) content += L"\n\n";
        content += _LW("app.widget_permission.permissions_optional_heading");
        for (const auto& permission : optionalPermissions)
        {
            content += L"\n  • ";
            content += WidgetPermissionLabel(permission);
        }
    }
    if (!package.manifest.networkDomains.empty())
    {
        content += L"\n\n";
        content += _LW("app.widget_permission.domains_heading");
        for (const auto& domain : package.manifest.networkDomains)
        {
            content += L"\n  • ";
            content += Utf8ToWide(domain);
        }
    }
    content += L"\n\n";
    content += _LW("app.widget_permission.runtime_notice");
    presentation.content = std::move(content);
    return presentation;
}

WidgetConsentChoice ShowWidgetConsentDialog(
    const WidgetConsentDialogPresentation& presentation,
    HWND resultWindow, std::uint64_t sessionId,
    POINT anchorScreenPoint)
{
    struct CallbackContext
    {
        HWND resultWindow = nullptr;
        std::uint64_t sessionId = 0;
        POINT anchorScreenPoint{};
    } callbackContext{ resultWindow, sessionId, anchorScreenPoint };
    const auto callback = +[](HWND dialogWindow, UINT notification,
        WPARAM, LPARAM, LONG_PTR rawContext) -> HRESULT {
        if (notification != TDN_CREATED) return S_OK;
        const auto* context = reinterpret_cast<const CallbackContext*>(
            rawContext);
        if (context && context->resultWindow)
        {
            PostMessageW(context->resultWindow,
                kWidgetConsentOpenedMessage,
                static_cast<WPARAM>(context->sessionId),
                reinterpret_cast<LPARAM>(dialogWindow));
        }

        // The consent dialog intentionally has no owner so its modal loop
        // cannot disable the desktop UI thread. An ownerless dialog otherwise
        // defaults to the primary monitor and SetForegroundWindow may be
        // rejected for this worker thread. Place it on the monitor where the
        // user clicked and retain topmost until the user closes it.
        int x = 0;
        int y = 0;
        UINT positionFlags = SWP_NOSIZE | SWP_SHOWWINDOW;
        RECT dialogRect{};
        MONITORINFO monitorInfo{ sizeof(monitorInfo) };
        const HMONITOR monitor = MonitorFromPoint(
            context ? context->anchorScreenPoint : POINT{},
            MONITOR_DEFAULTTONEAREST);
        if (GetWindowRect(dialogWindow, &dialogRect) && monitor &&
            GetMonitorInfoW(monitor, &monitorInfo))
        {
            const auto position = snowdesktop::widget_runtime::
                CenterConsentDialogInWorkArea(
                    monitorInfo.rcWork.left, monitorInfo.rcWork.top,
                    monitorInfo.rcWork.right, monitorInfo.rcWork.bottom,
                    dialogRect.right - dialogRect.left,
                    dialogRect.bottom - dialogRect.top);
            x = position.x;
            y = position.y;
        }
        else
        {
            positionFlags |= SWP_NOMOVE;
        }
        ShowWindow(dialogWindow, SW_SHOWNORMAL);
        SetWindowPos(dialogWindow, HWND_TOPMOST, x, y, 0, 0,
            positionFlags);
        snowdesktop::private_pointer::SetInputForeground(dialogWindow);
        return S_OK;
    };
    std::vector<TASKDIALOG_BUTTON> buttons;
    buttons.push_back({ 100, presentation.allowAllLabel.c_str() });
    if (presentation.showRequiredOnly)
        buttons.push_back({ 102,
            presentation.allowRequiredLabel.c_str() });
    buttons.push_back({ 101, presentation.denyLabel.c_str() });
    TASKDIALOGCONFIG dialog{};
    dialog.cbSize = sizeof(dialog);
    dialog.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION |
        TDF_SIZE_TO_CONTENT | TDF_USE_COMMAND_LINKS;
    dialog.dwCommonButtons = TDCBF_CANCEL_BUTTON;
    dialog.pszWindowTitle = presentation.title.c_str();
    dialog.pszMainIcon = TD_SHIELD_ICON;
    dialog.pszMainInstruction = presentation.instruction.c_str();
    dialog.pszContent = presentation.content.c_str();
    dialog.cButtons = static_cast<UINT>(buttons.size());
    dialog.pButtons = buttons.data();
    dialog.nDefaultButton = 101;
    dialog.pfCallback = callback;
    dialog.lpCallbackData = reinterpret_cast<LONG_PTR>(
        &callbackContext);

    int selected = IDCANCEL;
    if (SUCCEEDED(snowdesktop::operation_feedback::ShowTaskDialog(
            &dialog, &selected, nullptr, nullptr)))
    {
        if (selected == 100) return WidgetConsentChoice::AllowAll;
        if (selected == 102) return WidgetConsentChoice::AllowRequired;
        if (selected == 101) return WidgetConsentChoice::Deny;
        return WidgetConsentChoice::Cancel;
    }
    const int fallback = snowdesktop::operation_feedback::ShowMessageBox(nullptr,
        presentation.content.c_str(), presentation.instruction.c_str(),
        MB_ICONWARNING | MB_YESNOCANCEL |
            MB_DEFBUTTON2);
    if (fallback == IDYES) return WidgetConsentChoice::AllowAll;
    if (fallback == IDNO) return WidgetConsentChoice::Deny;
    return WidgetConsentChoice::Cancel;
}

void StartWidgetConsentDialog(HWND resultWindow,
    std::uint64_t sessionId,
    WidgetConsentDialogPresentation presentation,
    POINT anchorScreenPoint)
{
    std::thread([resultWindow, sessionId,
                    presentation = std::move(presentation),
                    anchorScreenPoint]() {
        const HRESULT initialized = CoInitializeEx(
            nullptr, COINIT_APARTMENTTHREADED);
        const WidgetConsentChoice choice =
            ShowWidgetConsentDialog(
                presentation, resultWindow, sessionId,
                anchorScreenPoint);
        PostMessageW(resultWindow, kWidgetConsentResolvedMessage,
            static_cast<WPARAM>(choice),
            static_cast<LPARAM>(sessionId));
        if (SUCCEEDED(initialized)) CoUninitialize();
    }).detach();
}

void ShowWidgetPermissionErrorAsync(
    std::wstring title, std::wstring message)
{
    std::thread([title = std::move(title),
                    message = std::move(message)]() {
        snowdesktop::operation_feedback::ShowMessageBox(nullptr, message.c_str(), title.c_str(),
            MB_OK | MB_ICONERROR);
    }).detach();
}

bool SameStringSet(const std::vector<std::string>& left,
    const std::vector<std::string>& right)
{
    return std::set<std::string>(left.begin(), left.end()) ==
        std::set<std::string>(right.begin(), right.end());
}
}

/**
 * @brief 在右键菜单位置添加 Lua 脚本组件。
 * @param screenPoint 屏幕坐标点。
 * @param packageId 组件包 ID。
 * @return 是否已在桌面建立实例。
 */
bool DesktopApp::AddLuaWidgetAt(POINT screenPoint, const std::wstring& packageId)
{
    if (packageId.empty()) return false;
    const auto package = WidgetEngine::GetWidgetPackage(packageId);
    if (!package) return false;

    const auto createWidget = [&] {
        int defaultColumns = 1;
        int defaultRows = 1;
        WidgetEngine::GetWidgetDefaultSpan(
            packageId, defaultColumns, defaultRows);
        snowdesktop::component_preview::ApplySettings settings;
        settings.kind =
            snowdesktop::component_preview::ApplyKind::LuaScript;
        settings.packageId = packageId;
        settings.columns = defaultColumns;
        settings.rows = defaultRows;
        return ApplyWidgetPreviewSettings(screenPoint, settings);
    };
    const bool needsNativeConsent = NeedsNativeHelperConsent(*package);
    const bool alreadyGranted = package->permissionState ==
        snowdesktop::widget::PermissionDecisionState::Granted;
    if (needsNativeConsent ||
        (!alreadyGranted &&
            (!snowdesktop::widget::PermissionsRequiringConsent(
                 package->manifest.permissions).empty() ||
             !snowdesktop::widget::PermissionsRequiringConsent(
                 package->manifest.optionalPermissions).empty())))
    {
        if (!needsNativeConsent && pendingLuaWidgetConsent_) return false;
        const std::wstring createdId = createWidget();
        if (createdId.empty()) return false;
        BeginLuaWidgetConsent(screenPoint, packageId, createdId);
        return true;
    }
    if (!alreadyGranted)
    {
        std::string error;
        const bool applied = widgetEngine_
            ? widgetEngine_->ApplyWidgetPermissionDecision(
                packageId,
                snowdesktop::widget::PermissionDecisionState::Granted,
                snowdesktop::widget::DeclaredPermissions(
                    package->manifest),
                package->manifest.networkDomains,
                error)
            : WidgetEngine::SetWidgetPermissionDecision(
                packageId,
                snowdesktop::widget::PermissionDecisionState::Granted,
                snowdesktop::widget::DeclaredPermissions(
                    package->manifest),
                package->manifest.networkDomains,
                error);
        if (!applied)
        {
            ShowWidgetPermissionErrorAsync(
                _LW("app.widget_permission.title"),
                _LFW("app.widget_permission.save_failed",
                    Utf8ToWide(error)));
            return false;
        }
    }
    return !createWidget().empty();
}

void DesktopApp::BeginLuaWidgetConsent(POINT screenPoint,
    const std::wstring& packageId,
    const std::wstring& targetWidgetId)
{
    if (targetWidgetId.empty()) return;
    const auto package = WidgetEngine::GetWidgetPackage(packageId);
    if (package && NeedsNativeHelperConsent(*package))
    {
        auto route = snowdesktop::SettingsRoute::ForPage(
            snowdesktop::SettingsPage::Widgets, "widgets.permissions");
        route.itemKey = packageId;
        ShowSettingsWindow(std::move(route));
        return;
    }
    if (pendingLuaWidgetConsent_)
    {
        const HWND dialogWindow =
            pendingLuaWidgetConsent_->dialogWindow;
        constexpr ULONGLONG ConsentOpeningTimeoutMs = 3000;
        const ULONGLONG elapsed = GetTickCount64() -
            pendingLuaWidgetConsent_->startedAt;
        const auto sessionAction = snowdesktop::widget_runtime::
            ConsentSessionActionFor(true, dialogWindow != nullptr,
                dialogWindow && IsWindow(dialogWindow), elapsed,
                ConsentOpeningTimeoutMs);
        if (sessionAction == snowdesktop::widget_runtime::
                WidgetConsentSessionAction::ActivateWindow)
        {
            ShowWindow(dialogWindow, SW_SHOWNORMAL);
            SetWindowPos(dialogWindow, HWND_TOPMOST, 0, 0, 0, 0,
                SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
            snowdesktop::private_pointer::SetInputForeground(dialogWindow);
            return;
        }
        if (sessionAction == snowdesktop::widget_runtime::
                WidgetConsentSessionAction::WaitForWindow)
            return;

        // The worker failed to publish a live window or a former dialog was
        // destroyed without delivering its result. Retire that session so an
        // explicit retry click cannot be swallowed indefinitely.
        pendingLuaWidgetConsent_.reset();
    }
    if (!package) return;
    const auto requiredConsentPermissions =
        snowdesktop::widget::PermissionsRequiringConsent(
            package->manifest.permissions);
    const auto optionalConsentPermissions =
        snowdesktop::widget::PermissionsRequiringConsent(
            package->manifest.optionalPermissions);
    if (requiredConsentPermissions.empty() &&
        optionalConsentPermissions.empty()) return;

    const std::wstring displayName =
        WidgetEngine::GetWidgetDisplayName(packageId);
    WidgetConsentDialogPresentation presentation =
        BuildWidgetConsentDialogPresentation(*package,
            displayName.empty() ? packageId : displayName,
            requiredConsentPermissions,
            optionalConsentPermissions);
    std::uint64_t sessionId = ++nextLuaWidgetConsentSessionId_;
    if (sessionId == 0)
        sessionId = ++nextLuaWidgetConsentSessionId_;
    pendingLuaWidgetConsent_ = PendingLuaWidgetConsent{
        sessionId, screenPoint, packageId, targetWidgetId,
        package->source, package->manifest.permissions,
        package->manifest.optionalPermissions,
        package->manifest.networkDomains, GetTickCount64(), nullptr };
    StartWidgetConsentDialog(
        hwnd_, sessionId, std::move(presentation), screenPoint);
}

void DesktopApp::NotifyLuaWidgetConsentDialogOpened(
    WPARAM rawSessionId, LPARAM rawDialogWindow)
{
    const std::uint64_t sessionId =
        static_cast<std::uint64_t>(rawSessionId);
    const HWND dialogWindow = reinterpret_cast<HWND>(rawDialogWindow);
    if (!pendingLuaWidgetConsent_ ||
        pendingLuaWidgetConsent_->sessionId != sessionId ||
        !dialogWindow || !IsWindow(dialogWindow))
        return;
    pendingLuaWidgetConsent_->dialogWindow = dialogWindow;
}

void DesktopApp::CompleteLuaWidgetConsent(
    WPARAM rawChoice, LPARAM rawSessionId)
{
    const std::uint64_t sessionId =
        static_cast<std::uint64_t>(rawSessionId);
    if (!pendingLuaWidgetConsent_ ||
        pendingLuaWidgetConsent_->sessionId != sessionId)
        return;
    PendingLuaWidgetConsent pending =
        std::move(*pendingLuaWidgetConsent_);
    pendingLuaWidgetConsent_.reset();

    const WidgetConsentChoice choice =
        static_cast<WidgetConsentChoice>(rawChoice);
    if (choice == WidgetConsentChoice::Cancel) return;
    const auto package = WidgetEngine::GetWidgetPackage(
        pending.packageId);
    if (!package) return;
    if (package->source != pending.source ||
        !SameStringSet(package->manifest.permissions,
            pending.requestedPermissions) ||
        !SameStringSet(package->manifest.optionalPermissions,
            pending.requestedOptionalPermissions) ||
        !SameStringSet(package->manifest.networkDomains,
            pending.requestedNetworkDomains))
    {
        BeginLuaWidgetConsent(pending.screenPoint,
            pending.packageId, pending.targetWidgetId);
        return;
    }

    const bool grant = choice == WidgetConsentChoice::AllowAll ||
        choice == WidgetConsentChoice::AllowRequired;
    const auto decision = grant
        ? snowdesktop::widget::PermissionDecisionState::Granted
        : snowdesktop::widget::PermissionDecisionState::Denied;
    std::vector<std::string> grantedPermissions;
    std::vector<std::string> grantedNetworkDomains;
    if (grant)
    {
        grantedPermissions = pending.requestedPermissions;
        for (const auto& permission :
            pending.requestedOptionalPermissions)
        {
            if (choice == WidgetConsentChoice::AllowAll ||
                !snowdesktop::widget::PermissionRequiresConsent(
                    permission))
            {
                grantedPermissions.push_back(permission);
            }
        }
        if (std::find(grantedPermissions.begin(),
                grantedPermissions.end(), "network.http") !=
                grantedPermissions.end() ||
            std::find(grantedPermissions.begin(),
                grantedPermissions.end(), "network.internet") !=
                grantedPermissions.end())
        {
            grantedNetworkDomains =
                pending.requestedNetworkDomains;
        }
    }
    std::string error;
    const bool applied = widgetEngine_
        ? widgetEngine_->ApplyWidgetPermissionDecision(
            pending.packageId, decision,
            grantedPermissions, grantedNetworkDomains, error)
        : WidgetEngine::SetWidgetPermissionDecision(
            pending.packageId, decision,
            grantedPermissions, grantedNetworkDomains, error);
    if (!applied)
    {
        ShowWidgetPermissionErrorAsync(
            _LW("app.widget_permission.title"),
            _LFW("app.widget_permission.save_failed",
                Utf8ToWide(error)));
        return;
    }
    if (grant && widgetEngine_)
    {
        (void)widgetEngine_->RetryWidget(
            pending.targetWidgetId, pending.packageId);
    }
    InvalidateRect(hwnd_, nullptr, FALSE);
}
