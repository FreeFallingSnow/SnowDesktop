#pragma once

#include "../settings_controller.h"
#include "hotkey_recorder.h"

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

struct SettingsLanguageOption
{
    std::string code;
    std::wstring label;
};

enum class GeneralStartupConflictKind : std::uint8_t
{
    None,
    PortableVersionOwnsStartup,
    InstalledVersionOwnsStartup,
};

/** Runtime-only startup ownership projected by the host. */
struct GeneralStartupConflict
{
    GeneralStartupConflictKind kind = GeneralStartupConflictKind::None;
    std::wstring ownerCommand;
};

/** Commands emitted by the cached General settings presenter. */
struct GeneralPageActions
{
    using GeneralEdit = std::function<void(GeneralSettings&)>;
    using NavigationEdit = std::function<void(NavigationSettings&)>;
    using DockEdit = std::function<void(DockSettings&)>;

    std::function<void(std::uint64_t generation, GeneralEdit edit)>
        commitGeneral;
    std::function<void(std::uint64_t generation, NavigationEdit edit)>
        commitNavigation;
    std::function<void(std::uint64_t generation, DockEdit edit)>
        commitDock;

    /** Auto-start is a Windows/MSIX projection, not a GeneralSettings edit. */
    std::function<void(std::uint64_t generation, bool enabled)>
        setAutoStart;
    std::function<GeneralStartupConflict()> queryStartupConflict;

    std::function<void(
        SettingsHostActions::HotkeyTarget target,
        HotkeyChord chord,
        std::uint64_t generation,
        std::uint64_t requestId,
        HotkeyRecorder::AvailabilityCompletion completion)>
        probeHotkey;

    std::function<std::vector<SettingsLanguageOption>()> languageCatalog;
};

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
        const winrt::Microsoft::UI::Xaml::Style& cardStyle);
    ~GeneralPagePresenter();

    GeneralPagePresenter(const GeneralPagePresenter&) = delete;
    GeneralPagePresenter& operator=(const GeneralPagePresenter&) = delete;

    void SetActions(GeneralPageActions actions);

    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        Root() const noexcept;
    /** Desktop behavior and interaction settings, rendered on Desktop. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        DesktopBehaviorContent() const noexcept;
    /** Legacy Dock shortcut/hotkey block, inserted after Dock enablement. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::StackPanel
        DockShortcutContent() const noexcept;
    void ApplySnapshot(const SettingsSnapshot& snapshot);
    void RefreshLocalizedText();
    /** Re-query runtime-only state that is not persisted in SettingsSnapshot. */
    void RefreshRuntimeState() noexcept;
    void RegisterFocusTargets(const FocusRegistrar& registrar) const;

    void Activate() noexcept;
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
