#pragma once

#include "../settings_controller.h"
#include "home_about_page_model.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{

struct HomeAboutPageActions
{
    std::function<void(const SettingsRoute& route)> navigate;
    std::function<void(
        std::uint64_t generation,
        HomeAboutCommand command)> invoke;
};

/**
 * Cached native WinUI presentation for the Home and About routes.
 *
 * The presenter performs no file, network or shell work. ProgressRing and
 * InfoBar state is driven exclusively by SettingsSnapshot/status publications
 * from the shell host, so closing the settings window cannot leave a private
 * task publishing into stale controls.
 */
class HomeAboutPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    HomeAboutPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        const winrt::Microsoft::UI::Xaml::Style& navigationCardStyle);
    ~HomeAboutPagePresenter();

    HomeAboutPagePresenter(const HomeAboutPagePresenter&) = delete;
    HomeAboutPagePresenter& operator=(
        const HomeAboutPagePresenter&) = delete;

    void SetActions(HomeAboutPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        HomeContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        AboutContent() const noexcept;

    void ApplySnapshot(const SettingsSnapshot& snapshot);
    /** Returns false for a stale generation or non-newer status patch. */
    [[nodiscard]] bool ApplyStatusPatch(
        const HomeAboutStatusPatch& patch);
    void RefreshLocalizedText();

    void Activate(SettingsPage page) noexcept;
    void Deactivate() noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(
            SettingsPage page,
            std::string_view focusId) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
