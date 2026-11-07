#pragma once

#include "dock_page_actions.h"
#include "settings_focus_feedback.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{

/**
 * Cached programmatic WinUI controls for the DockAndTaskbar route.
 *
 * Snapshot publications patch only the General and Dock domains whose
 * revision changed. Continuous controls preview while editing and commit at
 * an interaction boundary; discrete controls preview and commit immediately.
 */
class DockPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    DockPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~DockPagePresenter();

    DockPagePresenter(const DockPagePresenter&) = delete;
    DockPagePresenter& operator=(const DockPagePresenter&) = delete;

    void SetActions(DockPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        Content() const noexcept;
    /** Separate appearance and animation groups, placed after Dock behavior. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DockAppearanceContent() const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement DockThemeContent() const;
    /** Reparents the shared taskbar appearance group, including scenario rules. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement TaskbarAppearanceContent() const;
    void SetSupplementalContent(
        const winrt::Microsoft::UI::Xaml::UIElement& appearance,
        const winrt::Microsoft::UI::Xaml::UIElement& animation,
        const winrt::Microsoft::UI::Xaml::UIElement& shortcuts);
    /** Dock enable row. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DockEnableContent() const noexcept;
    /** Remaining non-taskbar Dock cards. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DockContent() const noexcept;
    /** Taskbar behavior, default appearance, scenario overrides and shell UI. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        TaskbarContent() const;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();

    void Activate() noexcept;
    /** Activate the taskbar surface and refresh Windows-owned runtime state. */
    void ActivateTaskbar() noexcept;
    void Deactivate() noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId,
            SettingsFocusPrerequisite* prerequisite = nullptr) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
