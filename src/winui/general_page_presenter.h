#pragma once

#include "general_page_actions.h"
#include "settings_focus_feedback.h"
#include "home_about_page_model.h"

#include <winrt/Microsoft.UI.Xaml.Controls.h>
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
 * Cached programmatic WinUI controls for the General settings route.
 *
 * The presenter never retains a SettingsSnapshot or SettingsValues copy.
 * Edits carry only their captured scalar value and expected generation; the
 * host action must apply them to the controller's latest domain value.
 */
class GeneralPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;
    using FocusRegistrar = std::function<void(
        std::string focusId,
        const winrt::Microsoft::UI::Xaml::FrameworkElement& element)>;

    GeneralPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        const winrt::Microsoft::UI::Xaml::Style& navigationCardStyle);
    ~GeneralPagePresenter();

    GeneralPagePresenter(const GeneralPagePresenter&) = delete;
    GeneralPagePresenter& operator=(const GeneralPagePresenter&) = delete;

    void SetActions(GeneralPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        Root() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel QuickNavigationContent() const noexcept;
    /** Desktop behavior and interaction settings, rendered on Desktop. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        DesktopBehaviorContent() const noexcept;
    /** Page-navigation shortcuts, rendered on Pages & grid. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        PageNavigationContent() const noexcept;
    /** Legacy Dock shortcut/hotkey block, inserted after Dock enablement. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        DockShortcutContent() const noexcept;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();
    /** Re-query runtime-only state that is not persisted in SettingsSnapshot. */
    void RefreshRuntimeState() noexcept;
    void RegisterFocusTargets(const FocusRegistrar& registrar) const;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId,
            SettingsFocusPrerequisite* prerequisite = nullptr,
            bool reveal = true) const noexcept;

    void ApplyOnboardingStatus(const HomeAboutStatusPatch& patch);
    void Activate(std::string_view focusId = {}) noexcept;
    void Deactivate() noexcept;
    [[nodiscard]] bool IsHotkeyCaptureActive() const noexcept;
    void CaptureRegisteredHotkey(
        std::uint32_t modifiers,
        std::uint32_t virtualKey);
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
