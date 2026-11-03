#pragma once

#include "desktop_page_actions.h"
#include "settings_focus_feedback.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::winui
{

/**
 * Cached, programmatic WinUI controls for the Desktop settings route.
 *
 * Continuous editors publish Preview while changing and
 * PreviewAndCommit when their pointer/keyboard interaction ends. Category
 * edits use the host's existing immediate preview-and-commit transaction.
 */
class DesktopPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    DesktopPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        std::function<void(SettingsRoute)> navigate = {});
    ~DesktopPagePresenter();

    DesktopPagePresenter(const DesktopPagePresenter&) = delete;
    DesktopPagePresenter& operator=(const DesktopPagePresenter&) = delete;

    void SetActions(DesktopPageActions actions);
    void SetIconEditorMode();
    void SetIconThemeInheritance(bool followGlobalTheme);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        Content() const noexcept;
    /** Spacing rows mounted in order in the existing global layout group. */
    [[nodiscard]] std::vector<winrt::Microsoft::UI::Xaml::UIElement>
        LayoutSpacingContent() const;
    /** Icon rendering, typography, and shortcut-arrow controls. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        DesktopIconsContent() const noexcept;
    /** Icon-beautification preset and advanced appearance controls. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        IconBeautificationContent() const noexcept;
    /** Category count and automatic-classification surface. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        CategoryContent() const noexcept;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();

    /** Return a stable control for search-result navigation and focus. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement FocusTarget(
        std::string_view focusId, bool reveal = false,
        SettingsFocusPrerequisite* prerequisite = nullptr) const noexcept;

    void Activate() noexcept;
    /** Track category-page visits independently of other pages sharing this
     * presenter. Changing pages hides the add form without discarding drafts. */
    void SetCategoryPageActive(bool active) noexcept;
    void Deactivate() noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
