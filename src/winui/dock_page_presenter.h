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

/** Commands emitted by the cached Dock and taskbar settings presenter. */
struct DockPageActions
{
    using GeneralEdit = std::function<void(GeneralSettings&)>;
    using DockEdit = std::function<void(DockSettings&)>;
    using ConfirmationCompletion = std::function<void(bool confirmed)>;

    /** Applies an edit to the controller's latest GeneralSettings value. */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        GeneralEdit edit)> updateGeneral;

    /** Applies an edit to the controller's latest DockSettings value. */
    std::function<void(
        std::uint64_t generation,
        SettingsUpdateMode mode,
        DockEdit edit)> updateDock;

    /** Routes non-setting work through SettingsHostActions. */
    std::function<void(
        std::uint64_t generation,
        SettingsHostActions::Request request)> invokeHost;

    /**
     * Shows the host-owned ContentDialog used for dangerous actions.
     * The callback may complete asynchronously on the settings dispatcher.
     */
    std::function<void(
        std::uint64_t generation,
        std::wstring title,
        std::wstring message,
        ConfirmationCompletion completion)> confirm;
};

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
    /** Dock enable row, placed before GeneralPagePresenter's hotkey block. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DockEnableContent() const noexcept;
    /** Remaining non-taskbar Dock controls, owned by General. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        DockContent() const noexcept;
    /** Taskbar behavior, default appearance, scenario overrides and shell UI. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::UIElement
        TaskbarContent() const noexcept;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();

    void Activate() noexcept;
    /** Activate the taskbar surface and refresh Windows-owned runtime state. */
    void ActivateTaskbar() noexcept;
    void Deactivate() noexcept;
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget(std::string_view focusId) const noexcept;
    void Close() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace snowdesktop::winui
