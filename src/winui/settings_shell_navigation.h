#pragma once

#include "../settings_route.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace snowdesktop::winui
{

/** Runtime gates for settings pages that must not be discoverable by default. */
struct SettingsShellPageVisibility
{
    bool developerTools = false;
    bool debug = false;

    [[nodiscard]] bool Allows(SettingsPage page) const noexcept;

    friend bool operator==(
        const SettingsShellPageVisibility&,
        const SettingsShellPageVisibility&) = default;
};

/**
 * Toolkit-independent navigation/history and stale-update gate for the XAML
 * settings shell.  Keeping this state outside WinUI makes the navigation
 * contract testable without creating an Application or DispatcherQueue.
 */
class SettingsShellNavigationState final
{
public:
    SettingsShellNavigationState();

    [[nodiscard]] const SettingsRoute& Route() const noexcept;
    [[nodiscard]] std::uint64_t Revision() const noexcept;
    [[nodiscard]] std::uint64_t Generation() const noexcept;
    [[nodiscard]] const SettingsShellPageVisibility& Visibility()
        const noexcept;

    [[nodiscard]] bool CanGoBack() const noexcept;
    [[nodiscard]] bool CanGoForward() const noexcept;
    [[nodiscard]] std::size_t HistorySize() const noexcept;
    [[nodiscard]] std::optional<SettingsRoute> PeekBack() const;

    /**
     * Applies a controller publication.  Updates older than the accepted
     * generation/revision pair are discarded.  A new generation starts a new
     * navigation history so a reopened window cannot return to an old view.
     */
    [[nodiscard]] bool ApplyControllerUpdate(
        const SettingsRoute& route,
        std::uint64_t revision,
        std::uint64_t generation);

    /** Records a user or search-driven navigation request. */
    [[nodiscard]] bool Navigate(const SettingsRoute& route);

    [[nodiscard]] std::optional<SettingsRoute> GoBack();
    [[nodiscard]] std::optional<SettingsRoute> GoForward();

    /**
     * Updates conditional page gates. If the active page becomes hidden, the
     * state returns to that legacy section's stable parent and reports the
     * replacement route.
     */
    [[nodiscard]] std::optional<SettingsRoute> SetVisibility(
        SettingsShellPageVisibility visibility);

    [[nodiscard]] bool IsRouteAvailable(
        const SettingsRoute& route) const noexcept;

private:
    void Push(const SettingsRoute& route);
    void CompactHistory();

    SettingsShellPageVisibility visibility_;
    std::vector<SettingsRoute> history_;
    std::size_t historyIndex_ = 0;
    std::uint64_t revision_ = 0;
    std::uint64_t generation_ = 0;
    bool hasControllerUpdate_ = false;
};

} // namespace snowdesktop::winui
