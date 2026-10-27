#include "app/app.h"
#include "dock_platform_helpers.h"
#include "dock/dock_minimize_protocol.h"
#include "platform/deployment_context.h"
#include "settings/animation_settings.h"
#include "platform/window_support/window_support_runtime.h"

void DesktopApp::UpdateDockExternalMinimizeTargets()
{
    const bool protectTaskbar = (snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Taskbar) && snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::WindowMinimize)) && ShouldProtectAutoHideTaskbar(dockSettings_, generalSettings_.dockEnabled,
        IsSystemTaskbarAutoHideEnabled());
    const bool moduleAllowed = snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::WindowMinimize);
    const bool animate = moduleAllowed && generalSettings_.dockEnabled && dockWindowTransition_ &&
        snowdesktop::animation::RuntimeAnimationsEnabled() &&
        snowdesktop::animation::RuntimeWindowEffect() != snowdesktop::animation::NoEffect;
    if (!moduleAllowed || (!protectTaskbar && !animate))
    {
        snowdesktop::window_support::protocol::Request clear;
        clear.command = snowdesktop::window_support::protocol::Command::ConfigureTargets;
        snowdesktop::window_support::WindowSupport().Submit(clear);
        dockExternalMinimize_.reset();
        snowdesktop::runtime_modules::SetActive(snowdesktop::runtime_modules::Module::WindowMinimize, 0);
        return;
    }
    // The current target already owns its hook. Process-bitness checks, hook
    // synchronization and stale-lease recovery can wait for the next idle tick.
    if (dockWindowTransition_ && dockWindowTransition_->IsActive()) return;
    const HWND receiver = controlHwnd_ && IsWindow(controlHwnd_) ? controlHwnd_ : hwnd_;
    if (!receiver) return;
    if (!dockExternalMinimize_)
    {
        auto monitor = std::make_unique<DockExternalMinimize>();
        if (!monitor->Start(receiver,
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHook.dll"),
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHost32.exe"),
                snowdesktop::deployment::GetInjectableRuntimeFilePath(L"SnowDesktopDockMinimizeHook32.dll")))
        {
            snowdesktop::runtime_modules::SetError(snowdesktop::runtime_modules::Module::WindowMinimize, GetLastError() ? GetLastError() : ERROR_GEN_FAILURE);
            return;
        }
        snowdesktop::runtime_modules::SetError(snowdesktop::runtime_modules::Module::WindowMinimize, ERROR_SUCCESS);
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
    if (protectTaskbar)
    {
        // Protection also covers applications omitted from the Dock model,
        // including screens without a Dock and configurations without animations.
        EnumWindows([](HWND window, LPARAM value) -> BOOL {
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process != GetCurrentProcessId() && IsDockTaskWindow(window))
                reinterpret_cast<std::vector<HWND>*>(value)->push_back(window);
            return TRUE;
        }, reinterpret_cast<LPARAM>(&windows));
    }
    std::sort(windows.begin(), windows.end(), std::less<HWND>{});
    windows.erase(std::unique(windows.begin(), windows.end()), windows.end());
    namespace support = snowdesktop::window_support;
    namespace wire = support::protocol;
    wire::Request helperTargets;
    helperTargets.command = wire::Command::ConfigureTargets;
    std::vector<HWND> nativeTargets;
    for (const HWND window : windows)
    {
        if (support::WindowSupport().Resolve(support::Capability::MinimizeAnimation, window).provider != support::Provider::Helper)
        { nativeTargets.push_back(window); continue; }
        if (helperTargets.targetCount == helperTargets.targets.size()) continue;
        wire::Target target;
        target.identity = wire::Capture(window);
        target.animate = animate ? 1u : 0u;
        target.effect = static_cast<std::uint32_t>(snowdesktop::animation::RuntimeWindowEffect());
        target.speed = static_cast<std::uint32_t>(snowdesktop::animation::detail::speed.load());
        target.frameLimit = static_cast<std::uint32_t>(snowdesktop::animation::detail::frameLimit.load());
        target.dockPosition = static_cast<std::uint32_t>(snowdesktop::animation::detail::dockPosition.load());
        if (dockWindowTransition_)
            if (const auto resolved = dockWindowTransition_->ResolveMinimizeTarget(window))
            { target.anchor = resolved->anchor; target.keepBelow = reinterpret_cast<UINT_PTR>(resolved->dock); }
        helperTargets.targets[helperTargets.targetCount++] = target;
        if (dockWindowTransition_) dockWindowTransition_->ObserveNativeMinimize(window);
    }
    support::WindowSupport().Submit(helperTargets);
    dockExternalMinimize_->UpdateTargets(nativeTargets);
    const auto registrations = dockExternalMinimize_->RegistrationStatistics();
    const auto cloakRegistrations = dockExternalMinimize_->CloakStatistics();
    const auto active = dockExternalMinimize_->HookCount();
    snowdesktop::runtime_modules::SetActive(snowdesktop::runtime_modules::Module::WindowMinimize, static_cast<unsigned>(active));
    const auto activeCloak = dockExternalMinimize_->CloakHookCount();
    static std::uint64_t reportedAttempts = 0, reportedCloakAttempts = 0;
    static std::size_t reportedActive = 0;
    if (reportedAttempts != registrations.attempts || reportedCloakAttempts != cloakRegistrations.attempts || reportedActive != active)
    {
        wchar_t message[320]{};
        swprintf_s(message, L"[DockHookRegistration] nativeActiveCBT=%zu cloakActive=%zu attempts=%llu failures=%llu lastError=%lu cloakAttempts=%llu cloakFailures=%llu",
            active, activeCloak, registrations.attempts, registrations.failures, registrations.lastError,
            cloakRegistrations.attempts, cloakRegistrations.failures);
        WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, message, registrations.lastError ? DiagnosticLogLevel::Warning : DiagnosticLogLevel::Info);
        reportedAttempts = registrations.attempts; reportedCloakAttempts = cloakRegistrations.attempts; reportedActive = active;
    }
}

bool DesktopApp::HandleDockExternalMinimize(HWND window, DWORD deadline)
{
    if (!snowdesktop::dock_minimize::RequestIsCurrent(deadline, GetTickCount()) ||
        !dockExternalMinimize_ || !dockExternalMinimize_->OwnsTarget(window) ||
        !IsWindowVisible(window) || IsIconic(window)) return false;
    PrepareDockWindowMinimize(window, L"application-minimize");
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
    if (dockSettings_.summonOnAppMinimize)
        if (const HWND promotedDock = KeepDockTopmostForAction(anchor))
            keepBelow = promotedDock;
    DismissDockWindowPreviewUntilLeave();
    CancelDockWindowActivationObservation(window);
    const bool started = dockWindowTransition_->StartExternalMinimize(window, anchor, deadline, keepBelow);
    if (started && !dockSettings_.summonOnAppMinimize)
        KeepDockTopmostForAction(anchor, false);
    return started;
}
