#pragma once

#include "../settings_controller.h"

#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace snowdesktop::winui
{

/** Commands emitted by the cached Personalization settings presenter. */
struct PersonalizationPageActions
{
    using Edit = std::function<void(PersonalizationSettings&)>;
    using GeneralEdit = std::function<void(GeneralSettings&)>;

    /**
     * Applies an edit to the controller's latest PersonalizationSettings.
     * The presenter deliberately sends no stale settings copy.
     */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        Edit edit)> update;
    /** Theme choices stored in GeneralSettings by the legacy surface. */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        GeneralEdit edit)> updateGeneral;
};

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
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~PersonalizationPagePresenter();

    PersonalizationPagePresenter(
        const PersonalizationPagePresenter&) = delete;
    PersonalizationPagePresenter& operator=(
        const PersonalizationPagePresenter&) = delete;

    void SetActions(PersonalizationPageActions actions);

    /** Global, target-surface, and context-menu theme controls. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        ThemeContent() const noexcept;
    /** Shared widget surface and layout controls. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        WidgetAppearanceContent() const noexcept;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();

    void Activate() noexcept;
    void Deactivate() noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
