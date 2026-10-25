#include "platform/auto_start_rules.h"
#include "platform/auto_start_elevation.h"
#include "platform/auto_start_launch.h"
#include "platform/deployment_context.h"

#include <cstdlib>
#include <iostream>
#include <string_view>

int RunAutoStartManagerTests();
int RunAutoStartLaunchProbe();

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << message << '\n';
}

int primaryChoice = IDCONTINUE;
int fallbackChoice = IDCONTINUE;
bool primaryUnavailable = false;
int fallbackCalls = 0;
HRESULT WINAPI TestStartupTaskDialog(const TASKDIALOGCONFIG* dialog, int* selected, int*, BOOL*)
{
    Check(dialog->cButtons == 2 && dialog->pButtons[0].nButtonID == IDCONTINUE &&
        std::wstring_view(dialog->pButtons[0].pszButtonText) == L"仍然启动" &&
        dialog->pButtons[1].nButtonID == IDCANCEL,
        "startup decisions expose an explicit Start anyway action and cancellation");
    *selected = primaryChoice;
    return primaryUnavailable ? E_FAIL : S_OK;
}
int TestStartupFallback(const wchar_t* content,
    const snowdesktop::auto_start_launch::StartupChoiceLabels& labels) noexcept
{
    ++fallbackCalls;
    namespace launch = snowdesktop::auto_start_launch;
    const auto data = launch::StartupChoiceTemplate(content, labels);
    // Create the actual fallback resource without WS_VISIBLE. This exercises
    // native controls/labels, not a source-text check or the desktop host UI.
    const HWND window = CreateDialogIndirectParamW(GetModuleHandleW(nullptr),
        reinterpret_cast<const DLGTEMPLATE*>(data.data()), nullptr, launch::StartupChoiceWindowProc, 0);
    Check(window != nullptr, "the native startup fallback template creates a hidden dialog");
    if (window)
    {
        wchar_t label[128]{}, cancel[128]{}, details[128]{};
        GetDlgItemTextW(window, IDCONTINUE, label, 128);
        GetDlgItemTextW(window, IDCANCEL, cancel, 128);
        GetDlgItemTextW(window, 100, details, 128);
        Check(std::wstring_view(label) == labels.start && std::wstring_view(cancel) == labels.cancel &&
            !GetDlgItem(window, IDOK) && std::wstring_view(details) == content && !IsWindowVisible(window),
            "fallback preserves the actual Start anyway button, cancellation and full failure details");
        DestroyWindow(window);
    }
    return fallbackChoice;
}
void CheckStartupDialogChoices()
{
    namespace launch = snowdesktop::auto_start_launch;
    const launch::StartupDialogApi api{&TestStartupTaskDialog, &TestStartupFallback};
    const launch::StartupChoiceLabels labels{L"仍然启动", L"取消"};
    Check(launch::ConfirmStartupContinuation(L"startup failure", labels, api) && fallbackCalls == 0,
        "explicit primary-dialog continuation proceeds without another dialog");
    primaryChoice = IDCANCEL;
    Check(!launch::ConfirmStartupContinuation(L"startup failure", labels, api),
        "primary-dialog cancellation cannot proceed");
    primaryUnavailable = true;
    for (const int choice : {IDCONTINUE, IDCANCEL, IDOK, -1})
    {
        fallbackChoice = choice;
        Check(launch::ConfirmStartupContinuation(L"startup failure", labels, api) == (choice == IDCONTINUE),
            "only the explicit fallback continuation action can proceed, including UI errors and unknown results");
    }
    Check(fallbackCalls == 4, "an unavailable TaskDialog invokes the labelled native fallback once per decision");
}

void ShellReportsSuccessWithoutChild(const std::wstring&, const std::wstring&, const std::wstring&) {}
void CheckUnacknowledgedExplorerLaunch()
{
    namespace launch = snowdesktop::auto_start_launch;
    launch::NativePlatform native{&ShellReportsSuccessWithoutChild, 10};
    bool timedOut = false;
    try { (void)native.Relaunch(L"host.exe"); }
    catch (const launch::Failure& failure) { timedOut = failure.code == ERROR_TIMEOUT; }
    Check(timedOut, "a successful Shell request without a child acknowledgement is not reported as successful startup");
}

void CheckAutoStartLaunchRules()
{
    namespace launch = snowdesktop::auto_start_launch;
    Check(launch::IsPreviewHostCommand(L"host.exe --widget-author-preview request") &&
        launch::IsPreviewHostCommand(L"host.exe --native-component-preview request") &&
        !launch::IsPreviewHostCommand(L"host.exe --data-directory=--widget-author-preview") &&
        !launch::IsPreviewHostCommand(L"host.exe profile --native-component-preview"),
        "only dedicated preview entry points retain their independent tool argument protocol");
    struct Platform
    {
        int launches = 0;
        int tokenChecks = 0;
        int explorerChecks = 0;
        int prompts = 0;
        bool elevated = false;
        bool explorerElevated = false;
        bool continueStartup = false;
        DWORD tokenError = 0;
        DWORD explorerError = 0;
        DWORD error = 0;
        DWORD promptedError = 0;
        std::wstring forwarded;
        bool IsElevated()
        {
            ++tokenChecks;
            if (tokenError) throw launch::Failure{tokenError, L"test token query"};
            return elevated;
        }
        bool IsExplorerElevated()
        {
            ++explorerChecks;
            if (explorerError) throw launch::Failure{explorerError, L"test Explorer token query"};
            return explorerElevated;
        }
        bool ConfirmContinue(const launch::Failure& failure)
        {
            ++prompts;
            promptedError = failure.code;
            return continueStartup;
        }
        DWORD Relaunch(std::wstring_view command)
        {
            ++launches;
            forwarded = command;
            if (error) throw launch::Failure{error, L"test Explorer launch"};
            return 0;
        }
    };
    for (const auto owner : {L"portable", L"packaged", L"steam"})
    {
        Platform platform;
        const std::wstring command = std::wstring(L"\"C:\\Snow Desktop\\host.exe\" ") +
            L"--snowdesktop-autostart-owner=" + owner + L" --data-directory=\"D:\\用户数据\"";
        const auto result = launch::Normalize(command, platform);
        Check(result.handled && result.error == ERROR_SUCCESS &&
            platform.launches == 1 && platform.forwarded == command,
            "all login owners delegate once to Explorer with their launch inputs");
    }
    for (const auto command : {L"host.exe", L"host.exe --snowdesktop-startup-elevated=123",
        L"host.exe --snowdesktop-autostart",
        L"host.exe --snowdesktop-autostart-owner=unknown",
        L"host.exe --data-directory=--snowdesktop-autostart-owner=steam"})
    {
        Platform platform;
        Check(!launch::Normalize(command, platform).handled && platform.launches == 0,
            "normal launches, privileged helpers and token substrings keep their existing entry point");
    }
    {
        Platform platform;
        platform.error = ERROR_ACCESS_DENIED;
        const auto result = launch::Normalize(L"host.exe --snowdesktop-autostart-owner=steam", platform);
        Check(result.handled && result.error == ERROR_CANCELLED && platform.launches == 1 &&
            platform.prompts == 1 && platform.promptedError == ERROR_ACCESS_DENIED,
            "an Explorer launch failure offers a decision and cancellation does not retry");
    }
    // Steam/manual launches carry no login owner. The actual token, including
    // a background/recovery launch or an unrecognized owner, decides routing.
    for (const auto command : {L"host.exe", L"host.exe --snowdesktop-autostart",
        L"host.exe --snowdesktop-autostart-owner=portable",
        L"host.exe --snowdesktop-autostart-owner=packaged",
        L"host.exe --snowdesktop-autostart-owner=steam",
        L"host.exe --snowdesktop-application-recovery", L"host.exe --wait-for-pid=42",
        L"host.exe --snowdesktop-autostart-owner=unknown",
        L"host.exe --data-directory=--snowdesktop-autostart-owner=steam"})
    {
        Platform platform;
        platform.elevated = true;
        const auto result = launch::Normalize(command, platform);
        Check(result.handled && result.error == ERROR_SUCCESS && platform.tokenChecks == 1 &&
            platform.launches == 1 && platform.forwarded == command,
            "every elevated desktop launch delegates regardless of source, background intent or owner arguments");
        platform.error = ERROR_ACCESS_DENIED;
        const auto denied = launch::Normalize(command, platform);
        Check(denied.handled && denied.error == ERROR_CANCELLED && platform.launches == 2 &&
            platform.prompts == 1 && platform.promptedError == ERROR_ACCESS_DENIED,
            "failed ordinary launch handoff prompts before any elevated fallback");
        platform.continueStartup = true;
        const auto accepted = launch::Normalize(command, platform);
        Check(!accepted.handled && accepted.error == ERROR_ACCESS_DENIED &&
            accepted.currentPrivilegesAccepted && platform.prompts == 2 && platform.launches == 3,
            "accepting a failed handoff continues in the current process with explicit per-launch consent");
    }
    for (const auto command : {L"host.exe --snowdesktop-explorer-relaunch",
        L"host.exe --snowdesktop-autostart --snowdesktop-explorer-relaunch"})
    {
        Platform platform;
        Check(!launch::Normalize(command, platform).handled && platform.tokenChecks == 1 &&
            platform.launches == 0, "an ordinary Explorer child continues without another handoff");
        platform.elevated = true;
        const auto result = launch::Normalize(command, platform);
        Check(result.handled && result.error == ERROR_CANCELLED && platform.tokenChecks == 2 &&
            platform.launches == 0 && platform.prompts == 1 &&
            platform.promptedError == ERROR_ELEVATION_REQUIRED,
            "a still-elevated child offers a decision instead of looping or exiting silently");
        platform.continueStartup = true;
        const auto accepted = launch::Normalize(command, platform);
        Check(!accepted.handled && accepted.currentPrivilegesAccepted && platform.launches == 0 &&
            platform.prompts == 2, "an elevated Explorer child can continue only after confirmation");
    }
    for (const auto command : {L"host.exe", L"host.exe --snowdesktop-autostart-owner=steam",
        L"host.exe --snowdesktop-autostart", L"host.exe --wait-for-pid=42",
        L"host.exe --snowdesktop-autostart-owner=portable", L"host.exe --snowdesktop-autostart-owner=packaged",
        L"host.exe --snowdesktop-application-recovery", L"host.exe --snowdesktop-explorer-relaunch",
        L"host.exe --snowdesktop-autostart --snowdesktop-explorer-relaunch"})
    {
        for (const bool elevated : {false, true})
        {
            Platform platform;
            platform.elevated = elevated;
            platform.explorerElevated = true;
            const auto result = launch::Normalize(command, platform);
            Check(!result.handled && result.error == ERROR_SUCCESS && platform.launches == 0 &&
                platform.prompts == 0 && (!elevated || platform.explorerChecks == 1),
                "a high-privilege Explorer preserves current startup without a handoff or a warning");
        }
    }
    {
        Platform platform;
        platform.elevated = true;
        platform.explorerError = ERROR_ACCESS_DENIED;
        Check(launch::Normalize(L"host.exe", platform).handled && platform.prompts == 1 &&
            platform.promptedError == ERROR_ACCESS_DENIED && platform.launches == 0,
            "an unreadable Explorer token requires a decision instead of assuming desktop privileges");
        platform.continueStartup = true;
        Check(!launch::Normalize(L"host.exe", platform).handled && platform.prompts == 2 &&
            platform.launches == 0, "Explorer token inspection failure can continue with explicit consent");
    }
    {
        Platform platform;
        platform.tokenError = ERROR_ACCESS_DENIED;
        const auto result = launch::Normalize(L"host.exe", platform);
        Check(result.handled && result.error == ERROR_CANCELLED && platform.launches == 0 &&
            platform.prompts == 1 && platform.promptedError == ERROR_ACCESS_DENIED,
            "unknown privileges offer a decision before desktop initialization");
        Check(!launch::Normalize(L"host.exe --shell-open-helper 123 456", platform, false).handled &&
            platform.tokenChecks == 1 && platform.launches == 0 && platform.prompts == 1,
            "inherited-channel helper dispatch retains its handles and independent validation");
        platform.continueStartup = true;
        const auto accepted = launch::Normalize(L"host.exe", platform);
        Check(!accepted.handled && accepted.currentPrivilegesAccepted && platform.prompts == 2,
            "token-query failure can continue only after confirmation");
    }
    {
        Platform platform;
        platform.tokenError = ERROR_ACCESS_DENIED;
        const auto accepted = launch::Normalize(L"host.exe", platform, true, true);
        Check(!accepted.handled && accepted.currentPrivilegesAccepted && platform.tokenChecks == 0 &&
            platform.explorerChecks == 0 && platform.prompts == 0 && platform.launches == 0,
            "the launcher's PID-bound consent avoids a second decision in its own runtime child");
        Check(!launch::Normalize(L"host.exe --shell-open-helper", platform, false, true).currentPrivilegesAccepted,
            "desktop startup consent never changes helper dispatch");
    }
}
}
int main(int argc, char** argv)
{
    if (argc > 1 && std::string_view(argv[1]) == "--autostart-permission-probe")
        return RunAutoStartLaunchProbe();
    if (const auto result = snowdesktop::auto_start::TryRunElevationCommand()) return *result;
    if (argc > 1 && std::string_view(argv[1]) == "--scheduler")
        return RunAutoStartManagerTests();
    // A test task must remain harmless if a logon happens during the test.
    if (argc > 1) return 0;
    CheckStartupDialogChoices();
    CheckUnacknowledgedExplorerLaunch();
    CheckAutoStartLaunchRules();
    using snowdesktop::BuildPortableAutoStartApprovalPayload;
    using snowdesktop::AutoStartOwnershipNotice;
    using snowdesktop::ClassifyAutoStartOwnershipNotice;
    using snowdesktop::DecodePortableAutoStartApprovalState;
    using snowdesktop::HasActivePortableAutoStart;
    using snowdesktop::IsPortableAutoStartApprovalActive;
    using snowdesktop::LegacyAutoStartState;
    using snowdesktop::PortableAutoStartApprovalState;
    using snowdesktop::PortableAutoStartRegistrationOwner;
    using snowdesktop::SelectAutoStartMigration;
    using snowdesktop::UnifiedAutoStartOwner;
    using snowdesktop::UnifiedAutoStartTaskState;
    using snowdesktop::deployment::PackagedAutoStartState;
    using snowdesktop::deployment::ShouldFinalizePackagedAutoStartUserEnable;

    Check(ShouldFinalizePackagedAutoStartUserEnable(
              PackagedAutoStartState::DisabledByUser,
              PackagedAutoStartState::Disabled),
        "a user-unblocked packaged task is finalized through the public API");
    Check(!ShouldFinalizePackagedAutoStartUserEnable(
               PackagedAutoStartState::Unavailable,
               PackagedAutoStartState::Disabled) &&
            !ShouldFinalizePackagedAutoStartUserEnable(
               PackagedAutoStartState::Enabled,
               PackagedAutoStartState::Disabled) &&
            !ShouldFinalizePackagedAutoStartUserEnable(
               PackagedAutoStartState::DisabledByUser,
               PackagedAutoStartState::DisabledByUser),
        "initial, app-disabled, and still-user-disabled tasks are not auto-enabled");

    Check(DecodePortableAutoStartApprovalState(0x00) ==
            PortableAutoStartApprovalState::Disabled,
        "Task Manager's zeroed marker is treated as disabled");
    Check(DecodePortableAutoStartApprovalState(0x01) ==
            PortableAutoStartApprovalState::Disabled,
        "Windows Settings marker 0x01 is treated as disabled");
    Check(DecodePortableAutoStartApprovalState(0x03) ==
            PortableAutoStartApprovalState::Disabled,
        "legacy marker 0x03 remains disabled");
    Check(DecodePortableAutoStartApprovalState(0x02) ==
            PortableAutoStartApprovalState::Enabled,
        "marker 0x02 remains enabled");
    Check(DecodePortableAutoStartApprovalState(0x7f) ==
            PortableAutoStartApprovalState::Error,
        "unknown markers remain unavailable instead of being guessed");

    constexpr auto enabledPayload =
        BuildPortableAutoStartApprovalPayload(true);
    Check(enabledPayload[0] == 0x02 && enabledPayload[4] == 0x00 &&
            enabledPayload[11] == 0x00,
        "app-side enablement writes the classic enabled approval payload");
    constexpr auto disabledPayload =
        BuildPortableAutoStartApprovalPayload(
            false, 0x0807060504030201ULL);
    Check(disabledPayload[0] == 0x03 &&
            disabledPayload[4] == 0x01 &&
            disabledPayload[5] == 0x02 &&
            disabledPayload[11] == 0x08,
        "app-side disablement writes the classic marker and FILETIME payload");

    Check(IsPortableAutoStartApprovalActive(
              PortableAutoStartApprovalState::Missing),
        "a Run registration without an approval override is active");
    Check(IsPortableAutoStartApprovalActive(
              PortableAutoStartApprovalState::Enabled),
        "an enabled approval state is active");
    Check(!IsPortableAutoStartApprovalActive(
               PortableAutoStartApprovalState::Disabled),
        "a disabled approval state is inactive");
    Check(!IsPortableAutoStartApprovalActive(
               PortableAutoStartApprovalState::Error),
        "an unavailable approval state is not reported as active");

    Check(HasActivePortableAutoStart(
              PortableAutoStartRegistrationOwner::OtherExecutable,
              PortableAutoStartApprovalState::Enabled),
        "an enabled registration owned by another executable is active");
    Check(!HasActivePortableAutoStart(
               PortableAutoStartRegistrationOwner::OtherExecutable,
               PortableAutoStartApprovalState::Disabled),
        "a disabled registration owned by another executable is not active");
    Check(!HasActivePortableAutoStart(
               PortableAutoStartRegistrationOwner::Missing,
               PortableAutoStartApprovalState::Enabled),
        "an approval entry without a Run registration is not active");
    Check(!HasActivePortableAutoStart(
               PortableAutoStartRegistrationOwner::Error,
               PortableAutoStartApprovalState::Enabled),
        "an unreadable Run registration is not reported as active");

    Check(ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Portable) ==
            AutoStartOwnershipNotice::OtherVersion &&
            ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Steam) ==
            AutoStartOwnershipNotice::OtherVersion,
        "Steam and portable startup targets owned by another deployment show the path-aware notice");
    Check(ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Packaged) ==
            AutoStartOwnershipNotice::InstalledVersion,
        "an active packaged startup target shows the installed-version notice");
    Check(ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, true,
              UnifiedAutoStartOwner::Steam) ==
            AutoStartOwnershipNotice::None &&
            ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Disabled, false,
              UnifiedAutoStartOwner::Portable) ==
            AutoStartOwnershipNotice::None &&
            ClassifyAutoStartOwnershipNotice(false,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Packaged) ==
            AutoStartOwnershipNotice::None &&
            ClassifyAutoStartOwnershipNotice(true,
              UnifiedAutoStartTaskState::Enabled, false,
              UnifiedAutoStartOwner::Unknown) ==
            AutoStartOwnershipNotice::None,
        "current, disabled, unreadable, and unknown startup targets do not show ownership notices");

    constexpr auto noLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Portable,
        LegacyAutoStartState::Missing,
        LegacyAutoStartState::Disabled);
    Check(noLegacy.canMigrate && !noLegacy.enableUnifiedTask &&
            noLegacy.owner == UnifiedAutoStartOwner::Portable,
        "a clean machine creates a disabled task owned by the current deployment");
    constexpr auto portableLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Packaged,
        LegacyAutoStartState::Enabled,
        LegacyAutoStartState::Disabled);
    Check(portableLegacy.canMigrate &&
            portableLegacy.enableUnifiedTask &&
            portableLegacy.owner == UnifiedAutoStartOwner::Portable,
        "a sole active portable entry keeps portable ownership during migration");
    constexpr auto packagedLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Portable,
        LegacyAutoStartState::Disabled,
        LegacyAutoStartState::Enabled);
    Check(packagedLegacy.canMigrate &&
            packagedLegacy.enableUnifiedTask &&
            packagedLegacy.owner == UnifiedAutoStartOwner::Packaged,
        "a sole active packaged entry keeps packaged ownership during migration");
    constexpr auto duplicateLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Portable,
        LegacyAutoStartState::Enabled,
        LegacyAutoStartState::Enabled);
    Check(duplicateLegacy.canMigrate &&
            duplicateLegacy.enableUnifiedTask &&
            duplicateLegacy.owner == UnifiedAutoStartOwner::Portable,
        "the explicitly running deployment wins when both legacy entries are active");
    constexpr auto unknownLegacy = SelectAutoStartMigration(
        UnifiedAutoStartOwner::Packaged,
        LegacyAutoStartState::Unavailable,
        LegacyAutoStartState::Enabled);
    Check(!unknownLegacy.canMigrate,
        "migration does not change startup when either legacy source is unreadable");

    if (failures != 0) return EXIT_FAILURE;
    std::cout << "Auto-start rule checks passed\n";
    return EXIT_SUCCESS;
}
