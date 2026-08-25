#include "settings_shell_navigation.h"

#include <algorithm>

namespace snowdesktop::winui
{
namespace
{
constexpr std::size_t kMaximumHistoryEntries = 64;
}

bool SettingsShellPageVisibility::Allows(SettingsPage page) const noexcept
{
    switch (page)
    {
    case SettingsPage::DeveloperTools:
        return developerTools;
    case SettingsPage::Debug:
        return debug;
    case SettingsPage::Home:
    case SettingsPage::General:
    case SettingsPage::Personalization:
    case SettingsPage::Desktop:
    case SettingsPage::DockAndTaskbar:
    case SettingsPage::Widgets:
    case SettingsPage::WidgetSettings:
    case SettingsPage::BackupAndData:
    case SettingsPage::About:
    case SettingsPage::Dock:
    case SettingsPage::Taskbar:
    case SettingsPage::DesktopCategories:
        return true;
    default:
        return false;
    }
}

SettingsShellNavigationState::SettingsShellNavigationState()
{
    history_.push_back(SettingsRoute::ForPage(SettingsPage::General));
}

const SettingsRoute& SettingsShellNavigationState::Route() const noexcept
{
    return history_[historyIndex_];
}

std::uint64_t SettingsShellNavigationState::Revision() const noexcept
{
    return revision_;
}

std::uint64_t SettingsShellNavigationState::Generation() const noexcept
{
    return generation_;
}

const SettingsShellPageVisibility&
SettingsShellNavigationState::Visibility() const noexcept
{
    return visibility_;
}

bool SettingsShellNavigationState::CanGoBack() const noexcept
{
    return historyIndex_ > 0;
}

bool SettingsShellNavigationState::CanGoForward() const noexcept
{
    return historyIndex_ + 1 < history_.size();
}

std::size_t SettingsShellNavigationState::HistorySize() const noexcept
{
    return history_.size();
}

bool SettingsShellNavigationState::ApplyControllerUpdate(
    const SettingsRoute& route,
    std::uint64_t revision,
    std::uint64_t generation)
{
    const SettingsRoute canonicalRoute = CanonicalizeSettingsRoute(route);
    if (!IsRouteAvailable(canonicalRoute))
        return false;

    if (hasControllerUpdate_)
    {
        if (generation < generation_ ||
            (generation == generation_ && revision <= revision_))
        {
            return false;
        }
    }

    const bool newGeneration =
        !hasControllerUpdate_ || generation != generation_;
    generation_ = generation;
    revision_ = revision;
    hasControllerUpdate_ = true;

    if (newGeneration)
    {
        history_.assign(1, canonicalRoute);
        historyIndex_ = 0;
    }
    else if (Route() != canonicalRoute)
    {
        if (historyIndex_ > 0 &&
            history_[historyIndex_ - 1] == canonicalRoute)
            --historyIndex_;
        else if (historyIndex_ + 1 < history_.size() &&
            history_[historyIndex_ + 1] == canonicalRoute)
            ++historyIndex_;
        else
            Push(canonicalRoute);
    }
    return true;
}

std::optional<SettingsRoute> SettingsShellNavigationState::PeekBack() const
{
    return CanGoBack()
        ? std::optional<SettingsRoute>(history_[historyIndex_ - 1])
        : std::nullopt;
}

bool SettingsShellNavigationState::Navigate(const SettingsRoute& route)
{
    const SettingsRoute canonicalRoute = CanonicalizeSettingsRoute(route);
    if (!IsRouteAvailable(canonicalRoute) || Route() == canonicalRoute)
        return false;
    Push(canonicalRoute);
    return true;
}

std::optional<SettingsRoute> SettingsShellNavigationState::GoBack()
{
    if (!CanGoBack())
        return std::nullopt;
    --historyIndex_;
    return Route();
}

std::optional<SettingsRoute> SettingsShellNavigationState::GoForward()
{
    if (!CanGoForward())
        return std::nullopt;
    ++historyIndex_;
    return Route();
}

std::optional<SettingsRoute> SettingsShellNavigationState::SetVisibility(
    SettingsShellPageVisibility visibility)
{
    if (visibility_ == visibility)
        return std::nullopt;

    const SettingsRoute active = Route();
    visibility_ = visibility;
    if (!IsRouteAvailable(active))
    {
        SettingsPage fallbackPage = SettingsPage::General;
        if (active.page == SettingsPage::DeveloperTools)
            fallbackPage = SettingsPage::Widgets;
        else if (active.page == SettingsPage::Debug)
            fallbackPage = SettingsPage::About;
        history_.assign(1, SettingsRoute::ForPage(fallbackPage));
        historyIndex_ = 0;
        return Route();
    }

    CompactHistory();
    return std::nullopt;
}

bool SettingsShellNavigationState::IsRouteAvailable(
    const SettingsRoute& route) const noexcept
{
    return route.IsValid() && visibility_.Allows(route.page);
}

void SettingsShellNavigationState::Push(const SettingsRoute& route)
{
    if (historyIndex_ + 1 < history_.size())
    {
        history_.erase(
            history_.begin() + static_cast<std::ptrdiff_t>(historyIndex_ + 1),
            history_.end());
    }
    history_.push_back(route);
    historyIndex_ = history_.size() - 1;

    if (history_.size() > kMaximumHistoryEntries)
    {
        const std::size_t removeCount =
            history_.size() - kMaximumHistoryEntries;
        history_.erase(
            history_.begin(),
            history_.begin() + static_cast<std::ptrdiff_t>(removeCount));
        historyIndex_ -= removeCount;
    }
}

void SettingsShellNavigationState::CompactHistory()
{
    const SettingsRoute active = Route();
    std::vector<SettingsRoute> compacted;
    compacted.reserve(history_.size());
    std::size_t activeIndex = 0;

    for (std::size_t index = 0; index < history_.size(); ++index)
    {
        if (!IsRouteAvailable(history_[index]))
            continue;
        if (!compacted.empty() && compacted.back() == history_[index])
            continue;
        if (index == historyIndex_)
            activeIndex = compacted.size();
        compacted.push_back(history_[index]);
    }

    if (compacted.empty())
    {
        compacted.push_back(SettingsRoute::ForPage(SettingsPage::General));
        activeIndex = 0;
    }
    else
    {
        const auto activeIt =
            std::find(compacted.begin(), compacted.end(), active);
        if (activeIt != compacted.end())
        {
            activeIndex = static_cast<std::size_t>(
                std::distance(compacted.begin(), activeIt));
        }
    }

    history_ = std::move(compacted);
    historyIndex_ = std::min(activeIndex, history_.size() - 1);
}

} // namespace snowdesktop::winui
