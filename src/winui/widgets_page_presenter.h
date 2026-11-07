#pragma once

#include "widgets_page_model.h"

#include <winrt/Microsoft.UI.Xaml.h>
#include <memory>

namespace snowdesktop::winui
{/**
 * Cached programmatic WinUI controls for the Widgets route.
 *
 * This class performs no package-manager IO, file picking, shell launch,
 * Workshop access or background work. It renders immutable host snapshots and
 * emits typed actions carrying the current settings-session generation.
 */
class WidgetsPagePresenter final
{
public:
    using LocalizeCallback =
        std::function<std::wstring(std::string_view key)>;

    WidgetsPagePresenter(
        LocalizeCallback localize,
        const winrt::Microsoft::UI::Xaml::Style& cardStyle,
        const winrt::Microsoft::UI::Xaml::DataTemplate& fluentGlyphTemplate,
        const winrt::Microsoft::UI::Xaml::DataTemplate&
            fontAwesomeGlyphTemplate);
    ~WidgetsPagePresenter();

    WidgetsPagePresenter(const WidgetsPagePresenter&) = delete;
    WidgetsPagePresenter& operator=(const WidgetsPagePresenter&) = delete;
    WidgetsPagePresenter(WidgetsPagePresenter&&) = delete;
    WidgetsPagePresenter& operator=(WidgetsPagePresenter&&) = delete;

    void SetActions(WidgetsPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        Content() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DeveloperToolsContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        AgentOperationsContent() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        AgentOperationsFocusTarget() const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DebugContent() const noexcept;

    /** Establishes the generation accepted by subsequent snapshots/actions. */
    void Activate(std::uint64_t generation) noexcept;
    void Deactivate() noexcept;

    /** Returns false when the publication is stale or belongs to another session. */
    [[nodiscard]] bool ApplySnapshot(const WidgetsPageSnapshot& snapshot);
    void RefreshLocalizedText();

    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const noexcept;
    /** Reveals only the requested package's access controls; never edits access. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusPackagePermissions(std::wstring_view packageId);
    [[nodiscard]] bool CanOpenPackagePermissions(std::wstring_view packageId) const noexcept;
    void OpenPackagePermissions(std::wstring_view packageId);
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        DeveloperToolsFocusTarget(std::string_view focusId) const noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        DebugFocusTarget(std::string_view focusId) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
