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

/** Commands emitted by the cached Desktop settings presenter. */
struct DesktopPageActions
{
    using DesktopEdit = std::function<void(DesktopDisplaySettings&)>;
    using CategoryEdit = std::function<void(CategorySettings&)>;
    using PersonalizationEdit =
        std::function<void(PersonalizationSettings&)>;

    /**
     * Each edit is applied by the host to the controller's latest domain
     * value.  The presenter intentionally does not send a stale settings
     * object back across the view boundary.
     */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        DesktopEdit edit)> updateDesktop;
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        CategoryEdit edit)> updateCategory;
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        PersonalizationEdit edit)> updatePersonalization;

    /** Persist the Category draft after the explicit Apply action. */
    std::function<void(std::uint64_t generation)> commitCategory;
};

/**
 * Cached, programmatic WinUI controls for the Desktop settings route.
 *
 * Continuous editors publish Preview while changing and
 * PreviewAndCommit when their pointer/keyboard interaction ends. Category
 * rules deliberately remain Draft until the user presses Apply.
 */
class DesktopPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    DesktopPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~DesktopPagePresenter();

    DesktopPagePresenter(const DesktopPagePresenter&) = delete;
    DesktopPagePresenter& operator=(const DesktopPagePresenter&) = delete;

    void SetActions(DesktopPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        Content() const noexcept;
    /** Display and icon-beautification sections owned by Desktop. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        AppearanceContent() const noexcept;
    /** Category layout and automatic-classification surface. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        CategoryContent() const noexcept;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();

    /** Return a stable control for search-result navigation and focus. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement FocusTarget(
        std::string_view focusId) const noexcept;

    void Activate() noexcept;
    void Deactivate() noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
