#include "app/app.h"
#include "dock/dock_minimize_protocol.h"
#include "platform/deployment_context.h"
#include "settings/animation_settings.h"

void DesktopApp::UpdateDockExternalMinimizeTargets()
{
    if (!generalSettings_.dockEnabled || !dockWindowTransition_ ||
        !snowdesktop::animation::RuntimeAnimationsEnabled() ||
        snowdesktop::animation::RuntimeWindowEffect() == snowdesktop::animation::NoEffect)
    {
        dockExternalMinimize_.reset();
        return;
    }
    // The current target already owns its hook. Process-bitness checks, hook
    // synchronization and stale-lease recovery can wait for the next idle tick.
    if (dockWindowTransition_->IsActive()) return;
    const HWND receiver = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
    if (!receiver) return;
    if (!dockExternalMinimize_)
    {
        auto monitor = std::make_unique<DockExternalMinimize>();
        if (!monitor->Start(receiver,
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHook.dll"),
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHost32.exe"),
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHook32.dll")))
            return;
        dockExternalMinimize_ = std::move(monitor);
    }
    std::vector<HWND> windows;
    for (const auto& [key, state] : dockRunningWindows_)
    {
        (void)key;
        if (!state.running) continue;
        windows.push_back(state.window);
        windows.insert(windows.end(), state.trackedWindows.begin(), state.trackedWindows.end());
    }
    for (const auto& state : dockUnpinnedRunningApps_)
    {
        windows.push_back(state.window);
        windows.insert(windows.end(), state.trackedWindows.begin(), state.trackedWindows.end());
    }
    std::sort(windows.begin(), windows.end(), std::less<HWND>{});
    windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
    dockExternalMinimize_->UpdateTargets(windows);
    const auto registrations = dockExternalMinimize_->RegistrationStatistics();
    const auto cloakRegistrations = dockExternalMinimize_->CloakStatistics();
    const auto active = dockExternalMinimize_->HookCount();
    const auto activeCloak = dockExternalMinimize_->CloakHookCount();
    static std::uint64_t reportedAttempts = 0, reportedCloakAttempts = 0;
    static std::size_t reportedActive = 0;
    if (reportedAttempts != registrations.attempts || reportedCloakAttempts != cloakRegistrations.attempts || reportedActive != active)
    {
        wchar_t message[320]{};
        swprintf_s(message, L"[DockHookRegistration] nativeActiveCBT=%zu cloakActive=%zu attempts=%llu failures=%llu lastError=%lu cloakAttempts=%llu cloakFailures=%llu",
            active, activeCloak, registrations.attempts, registrations.failures, registrations.lastError,
            cloakRegistrations.attempts, cloakRegistrations.failures);
        WriteDiagnosticLogEntry(message, registrations.lastError ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Info);
        reportedAttempts = registrations.attempts; reportedCloakAttempts = cloakRegistrations.attempts; reportedActive = active;
    }
}

bool DesktopApp::HandleDockExternalMinimize(HWND window, DWORD deadline)
{
    if (!snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount()) ||
        !generalSettings_.dockEnabled || reloading_ || dragSession_.HasContext() ||
        graphicsDeviceRecovery_.Pending() || !dockExternalMinimize_ ||
        !dockExternalMinimize_->OwnsTarget(window) || !dockWindowTransition_ ||
        dockWindowTransition_->IsActive() || !IsWindowVisible(window) || IsIconic(window) ||
        !snowdesktop::animation::RuntimeAnimationsEnabled() ||
        snowdesktop::animation::RuntimeWindowEffect() == snowdesktop::animation::NoEffect)
        return false;

    const auto target = dockWindowTransition_->ResolveMinimizeTarget(window);
    if (!target || !snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount())) return false;
    const RECT anchor = target->anchor;
    HWND keepBelow = target->dock && IsWindowVisible(target->dock) ? target->dock : nullptr;
    PrepareDockWindowMinimize(window, L"application-minimize");
    if (const HWND promotedDock = KeepDockTopmostForAction(anchor))
        keepBelow = promotedDock;
    DismissDockWindowPreviewUntilLeave();
    CancelDockWindowActivationObservation(window);
    return dockWindowTransition_->StartExternalMinimize(window, anchor, deadline, keepBelow);
}
