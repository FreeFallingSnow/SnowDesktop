#pragma once

#include "personalization_page_actions.h"
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
 * Cached programmatic WinUI controls for the Personalization route.
 *
 * Snapshot publications only patch controls when the personalization-domain
 * revision changes. Continuous controls preview while changing and commit at
 * the interaction boundary; discrete controls preview and commit immediately.
 */
class PersonalizationPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    PersonalizationPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        const winrt::Microsoft::UI::Xaml::Style& navigationStyle = nullptr);
    ~PersonalizationPagePresenter();

    PersonalizationPagePresenter(
        const PersonalizationPagePresenter&) = delete;
    PersonalizationPagePresenter& operator=(
        const PersonalizationPagePresenter&) = delete;

    void SetActions(PersonalizationPageActions actions);

    /** Global, custom-surface, target-surface, and context-menu themes. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        ThemeContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement ThemeManagementContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement DockAppearanceContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement MenuContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement TypographyContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement WindowPinBorderContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement DockAttentionDotContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement QuickNavigationAppearanceContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement WidgetCommonContent() const noexcept;
    /** Compatibility alias for TypographyContent; do not mount both. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        WidgetLayoutContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        WidgetBehaviorContent() const noexcept;
    /** Mount spacing rows once, using the existing layout group surface. */
    void SetLayoutSpacingContent(
        const std::vector<winrt::Microsoft::UI::Xaml::UIElement>& content);
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();

    void Activate() noexcept;
    void Deactivate() noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId, bool reveal = false,
            SettingsFocusPrerequisite* prerequisite = nullptr) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
