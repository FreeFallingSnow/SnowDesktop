#pragma once

#include "dock_settings_types.h"
#include "taskbar_hook/taskbar_autohide_trace.h"

#include <array>
#include <string>
#include <vector>

enum class SystemTaskbarBackdropRuntimeState
{
    Disabled,
    Loading,
    Active,
    Unsupported,
    Failed
};

struct SystemTaskbarTargetAppearance
{
    HWND taskbar = nullptr;
    bool enabled = false;
    PersonalizationSettings appearance =
        PersonalizationSettings::DarkPreset();
    bool protectAutoHideActivation = false;
    bool shellPanelVisible = false;
    bool suppressTaskbar = false;
};

void ConfigureSystemTaskbarTargetProtection(
    SystemTaskbarTargetAppearance& target,
    bool suppressionRequested, bool protectActivation, bool hasDock,
    bool allMonitors = false) noexcept;

bool ShouldSuppressSystemTaskbar(const DockSettings& settings,
    bool dockEnabled) noexcept;

bool ShowDockWindowsButton(const DockSettings& settings) noexcept;

bool ShouldProtectAutoHideTaskbar(const DockSettings& settings,
    bool dockEnabled, bool autoHideEnabled) noexcept;

PersonalizationSettings ResolveDockAppearance(const DockSettings& settings, const PersonalizationSettings& global);

void SelectDockAppearance(DockSettings& settings, bool followGlobal,
    int preset, const PersonalizationSettings& global);

void NormalizeDockSettings(DockSettings& settings);

std::wstring GetDockSettingsPath();
bool IsSystemTaskbarAutoHideEnabled();
bool RequestSystemTaskbarAutoHideEnabled(bool enabled);
bool BeginSystemTaskbarMinimizeProtection(HWND source, bool observed = false);
// Worker-only, bounded acknowledgement before sending the Start invocation.
// Renewal and the 500ms closing hold expire independently in Explorer.
// A new begin rearms even when the previous closing hold is still alive.
bool UpdateSystemTaskbarStartMenuProtection(bool active, bool begin = false);
bool IsSystemTaskbarAlignmentCentered();
bool RequestSystemTaskbarAlignmentCentered(bool centered);
bool IsWindowsSystemLightThemeEnabled();
bool RequestWindowsSystemLightThemeEnabled(bool enabled);
bool RestartWindowsExplorer();
PersonalizationSettings MakeTransparentTaskbarAppearance();
SystemTaskbarBackdropRuntimeState GetSystemTaskbarBackdropRuntimeState();
SystemTaskbarBackdropRuntimeState GetSystemTaskbarSuppressionRuntimeState();
bool TakeSystemPowerUserMenuRequest(HWND& foreground);
bool IsClassicSystemTaskbar();
void NotifySystemTaskbarCreated();
LONG DrainSystemTaskbarAutoHideTrace(
    std::array<snowdesktop::taskbar_hook::AutoHideTraceRecord,
        snowdesktop::taskbar_hook::kAutoHideTraceCapacity>& records, LONG& dropped);
bool ApplySystemTaskbarBackdrop(bool hookEnabled, bool defaultEnabled,
    const PersonalizationSettings& defaultAppearance,
    const std::vector<SystemTaskbarTargetAppearance>& targets = {},
    bool appearanceEnabled = true, bool suppressTaskbar = false);
bool LoadDockSettings(const wchar_t* path, DockSettings& settings);
bool SaveDockSettings(const wchar_t* path, const DockSettings& settings);
