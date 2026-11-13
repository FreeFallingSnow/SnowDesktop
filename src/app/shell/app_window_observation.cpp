#include "app/app.h"
#include "app/dock/persistent_dock_host.h"
#include "diagnostics/diagnostic_log.h"
#include "dock/dock_appbar.h"
#include "platform/window_observation.h"
#include <dwmapi.h>

void DesktopApp::RefreshWindowObservations()
{
    if (windowObservationInProgress_) return;
    windowObservationInProgress_ = true;
    struct Reset { bool& active; ~Reset() { active = false; } } reset{windowObservationInProgress_};
    const DWORD revision = windowObservationRevision_.load(std::memory_order_relaxed);
    const LONG left = virtualLeft_, top = virtualTop_;
    const HWND desktop = hwnd_;
    std::vector<RECT> pages;
    pages.reserve(gridPages_.size());
    for (const auto& page : gridPages_)
    {
        RECT screen = page.bounds;
        OffsetRect(&screen, left, top);
        pages.push_back(screen);
    }
    const HWND desktopSurface = desktopWindows_.defView ? desktopWindows_.defView : desktopWindows_.host;
    const HWND desktopRoot = desktopSurface ? GetAncestor(desktopSurface, GA_ROOT) : nullptr;
    auto snapshot = snowdesktop::window_observation::Observe(pages, desktopRoot,
        customDesktopVisible_ && desktop ? GetAncestor(desktop, GA_ROOT) : nullptr);
    if (desktop != hwnd_ || left != virtualLeft_ || top != virtualTop_) return;

    const bool sceneChanged = snapshot.monitors.size() != observedMonitorWindows_.size() ||
        std::any_of(snapshot.monitors.begin(), snapshot.monitors.end(), [this](const auto& entry) {
            const auto previous = observedMonitorWindows_.find(entry.first);
            return previous == observedMonitorWindows_.end() ||
                previous->second.visible != entry.second.visible ||
                previous->second.maximized != entry.second.maximized ||
                previous->second.fullscreen != entry.second.fullscreen;
        });
    const bool fullscreenChanged = sceneChanged ||
        std::any_of(snapshot.monitors.begin(), snapshot.monitors.end(), [this](const auto& entry) {
            return observedMonitorWindows_.at(entry.first).fullscreenSource != entry.second.fullscreenSource;
        });
    if (sceneChanged) ++monitorSceneRevision_;
    if (sceneChanged && desktop && generalSettings_.dockEnabled &&
        dockSettings_.UsesReservedSpaceBackdrop() && dockAppBars_)
    {
        const auto maximized = [](const auto& monitors, HMONITOR monitor) {
            const auto found = monitors.find(monitor);
            return found != monitors.end() && found->second.maximized;
        };
        for (const auto& page : pages)
        {
            const auto monitor = MonitorFromRect(&page, MONITOR_DEFAULTTONULL);
            if (!monitor || maximized(observedMonitorWindows_, monitor) ==
                maximized(snapshot.monitors, monitor))
                continue;
            if (auto area = dockAppBars_->ApprovedArea(monitor))
            {
                OffsetRect(&*area, -left, -top);
                InvalidateRect(desktop, &*area, FALSE);
            }
        }
    }
    if (fullscreenChanged)
    {
        for (const auto& host : persistentDockHosts_)
        {
            if (!host || !host->active) continue;
            const auto observed = snapshot.monitors.find(host->monitor);
            if (observed == snapshot.monitors.end() || !observed->second.fullscreenSource.has_value()) continue;
            const auto previous = ObservedFullscreenSource(host->monitor);
            const HWND source = *observed->second.fullscreenSource;
            if (previous == observed->second.fullscreenSource) continue;
            host->desktopLayerRestorePending = true;
            if (!source && previous.value_or(nullptr) && host->hwnd &&
                snowdesktop::diagnostics::IsLogEnabled(DiagnosticLogModule::Dock))
            {
                MONITORINFO info{sizeof(info)};
                const bool behindDesktop = GetMonitorInfoW(host->monitor, &info) &&
                    snowdesktop::window_observation::IsBehindDesktop(host->hwnd, desktopRoot, info.rcMonitor);
                wchar_t message[320]{};
                swprintf_s(message, L"Dock fullscreen cleared monitor=%p dock=%p foreground=%p guard=%d "
                    L"visible=%d iconic=%d behindDesktop=%d shouldShow=%d",
                    host->monitor, host->hwnd, GetForegroundWindow(), systemShowDesktopLayerGuardActive_,
                    IsWindowVisible(host->hwnd), IsIconic(host->hwnd), behindDesktop, ShouldShowPersistentDockHost(*host));
                WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, message);
            }
        }
    }
    if (fullscreenChanged && snowdesktop::diagnostics::IsLogEnabled(DiagnosticLogModule::System))
    {
        for (const auto& [monitor, state] : snapshot.monitors)
        {
            const auto previous = ObservedFullscreenSource(monitor);
            if (previous == state.fullscreenSource) continue;
            const HWND oldSource = previous.value_or(nullptr);
            DWORD cloaked = 0;
            const bool cloakKnown = oldSource &&
                SUCCEEDED(DwmGetWindowAttribute(oldSource, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)));
            MONITORINFO info{sizeof(info)};
            const bool oldBehindDesktop = oldSource && GetMonitorInfoW(monitor, &info) &&
                snowdesktop::window_observation::IsBehindDesktop(oldSource, desktopRoot, info.rcMonitor);
            wchar_t message[416]{};
            swprintf_s(message, L"Fullscreen source change monitor=%p old=%p new=%p known=%d "
                L"oldVisible=%d oldIconic=%d oldCloakKnown=%d oldCloaked=%lu oldBehindDesktop=%d "
                L"desktop=%p foreground=%p showDesktop=%d",
                monitor, oldSource, state.fullscreenSource.value_or(nullptr),
                state.fullscreenSource.has_value(), oldSource && IsWindowVisible(oldSource),
                oldSource && IsIconic(oldSource), cloakKnown, cloaked, oldBehindDesktop, desktopRoot, GetForegroundWindow(),
                systemShowDesktopLayerGuardActive_);
            WriteModuleDiagnosticLogEntry(DiagnosticLogModule::System, message);
        }
    }
    observedMonitorWindows_ = std::move(snapshot.monitors);
    {
        std::scoped_lock lock(windowPlacementMutex_);
        observedWindowPlacements_ = std::move(snapshot.placements);
    }
    observedWindowRevision_ = revision;
    windowObservationTick_ = GetTickCount();
    for (auto& bounds : snapshot.desktopCoverage)
        OffsetRect(&bounds, -left, -top);
    UpdateDesktopWidgetCoverage(std::move(snapshot.desktopCoverage));
    if (statusBar_ && fullscreenChanged) statusBar_->NotifyWindowObservations();
    // Explorer can hide/minimize the Dock during Show Desktop even when
    // another application receives foreground focus during the transition.
    if (generalSettings_.dockEnabled && !dockSettings_.showOnlyWhenSummoned &&
        !desktopStartupPresentationPending_)
    {
        for (const auto& host : persistentDockHosts_)
        {
            if (!host || !host->active || !host->hwnd || !host->container ||
                host->container->IsMergedWithStatusBar() ||
                (!IsIconic(host->hwnd) && IsWindowVisible(host->hwnd)) || !ShouldShowPersistentDockHost(*host)) continue;
            const auto fullscreen = ObservedFullscreenSource(host->monitor);
            if (!fullscreen.has_value() || RecheckFullscreenSource(host->monitor)) continue;
            UpdatePersistentDockHostVisibility(*host);
            if (snowdesktop::diagnostics::IsLogEnabled(DiagnosticLogModule::Dock) &&
                IsWindowVisible(host->hwnd) && !IsIconic(host->hwnd))
            {
                wchar_t message[256]{};
                swprintf_s(message, L"Dock Show Desktop restored monitor=%p dock=%p foreground=%p guard=%d topmost=%d",
                    host->monitor, host->hwnd, GetForegroundWindow(), systemShowDesktopLayerGuardActive_,
                    (GetWindowLongPtrW(host->hwnd, GWL_EXSTYLE) & WS_EX_TOPMOST) != 0);
                WriteModuleDiagnosticLogEntry(DiagnosticLogModule::Dock, message);
            }
        }
    }
    if (sceneChanged && generalSettings_.dockEnabled &&
        (dockSettings_.appWindow.enabled || dockSettings_.maximizedWindow.enabled))
    {
        InvalidateDragStaticScene();
        ApplyPersistentDockHostAppearance();
        InvalidateDockRects();
    }
}

std::optional<HWND> DesktopApp::ObservedFullscreenSource(HMONITOR monitor) const
{
    const auto found = observedMonitorWindows_.find(monitor);
    return found == observedMonitorWindows_.end() ? std::nullopt : found->second.fullscreenSource;
}

std::optional<snowdesktop::fullscreen::WindowSnapshot> DesktopApp::RecheckFullscreenSource(
    HMONITOR monitor) const
{
    const HWND desktopSurface = desktopWindows_.defView ? desktopWindows_.defView : desktopWindows_.host;
    return snowdesktop::window_observation::RecheckFullscreenSource(
        monitor, ObservedFullscreenSource(monitor).value_or(nullptr),
        desktopSurface ? GetAncestor(desktopSurface, GA_ROOT) : nullptr);
}
