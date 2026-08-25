#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace snowdesktop
{

/** Stable destinations understood by every settings presentation backend. */
enum class SettingsPage : std::uint8_t
{
    Home,
    General,
    Personalization,
    Desktop,
    // Retained as a compatibility input. New routes use Dock or Taskbar.
    DockAndTaskbar,
    Widgets,
    WidgetSettings,
    BackupAndData,
    About,
    DeveloperTools,
    Debug,
    // Appended to preserve the numeric values of existing destinations.
    Dock,
    Taskbar,
    DesktopCategories,
};

/**
 * A backend-neutral settings destination.
 *
 * focusId is an internal, stable setting-card identifier.  It lets search
 * results navigate to and focus a setting without encoding control pointers
 * or XAML names in application code.
 */
struct SettingsRoute
{
    SettingsPage page = SettingsPage::General;
    std::wstring widgetInstanceId;
    std::string focusId;

    [[nodiscard]] static SettingsRoute ForPage(
        SettingsPage page,
        std::string focusId = {});
    [[nodiscard]] static SettingsRoute ForWidget(
        std::wstring instanceId,
        std::string focusId = {});

    [[nodiscard]] bool IsValid() const noexcept;

    friend bool operator==(
        const SettingsRoute&,
        const SettingsRoute&) = default;
};

/** Converts legacy compatibility destinations to their canonical pages. */
[[nodiscard]] SettingsRoute CanonicalizeSettingsRoute(SettingsRoute route);

/** Stable, non-localized key suitable for diagnostics and navigation state. */
[[nodiscard]] std::string_view SettingsPageKey(SettingsPage page) noexcept;

} // namespace snowdesktop
