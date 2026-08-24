#pragma once

#include "hotkey_recorder_rules.h"

#include <winrt/Microsoft.UI.Dispatching.h>
#include <winrt/Microsoft.UI.Xaml.Controls.h>
#include <winrt/Microsoft.UI.Xaml.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace snowdesktop::winui
{

struct HotkeyRecorderState;

struct HotkeyRecorderText
{
    std::wstring automationName = L"Keyboard shortcut recorder";
    std::wstring idleHint = L"Select to record a keyboard shortcut";
    std::wstring captureHint =
        L"Press a shortcut. Enter saves, Escape cancels, Backspace or Delete clears.";
    std::wstring checking = L"Checking shortcut availability";
    std::wstring available = L"Shortcut is available";
    std::wstring conflict = L"Shortcut conflicts with another command";
    std::wstring cleared = L"Shortcut cleared";
    std::wstring none = L"None";
    std::wstring disabled = L"Disabled";
    std::wstring captureActive = L"Shortcut recording is active";
    std::wstring notSetWarning = L"No shortcut is configured";
    std::wstring availableStatus = L"Available";
    std::wstring inUseStatus = L"In use";
    std::wstring noModifierStatus = L"No modifier";
    std::wstring noModifierWarning =
        L"No modifier is used, which may interfere with normal typing";
    std::wstring systemConflict =
        L"Already used by Windows or another application";
    std::wstring control = L"Ctrl";
    std::wstring alt = L"Alt";
    std::wstring shift = L"Shift";
    std::wstring windows = L"Win";
};

/**
 * Programmatic WinUI 3 hotkey recorder suitable for insertion into a page.
 *
 * The visible element is a UserControl containing a theme-aware Button and a
 * live status TextBlock; it is deliberately not a TextBox. Availability may
 * complete asynchronously and is guarded by both generation and request id.
 */
class HotkeyRecorder final
{
public:
    using AvailabilityCompletion =
        std::function<void(bool available, std::wstring conflictMessage)>;
    using AvailabilityProbe = std::function<void(
        HotkeyChord chord,
        std::uint64_t generation,
        std::uint64_t requestId,
        AvailabilityCompletion completion)>;
    using CommittedCallback = std::function<void(HotkeyChord chord)>;
    using CancelledCallback = std::function<void()>;

    HotkeyRecorder();
    ~HotkeyRecorder();

    HotkeyRecorder(const HotkeyRecorder&) = delete;
    HotkeyRecorder& operator=(const HotkeyRecorder&) = delete;

    [[nodiscard]] winrt::Microsoft::UI::Xaml::Controls::UserControl
        Root() const noexcept;
    /** Actual keyboard-focus target used by search-result navigation. */
    [[nodiscard]] winrt::Microsoft::UI::Xaml::FrameworkElement
        FocusTarget() const noexcept;

    void SetText(HotkeyRecorderText text);
    void SetAvailabilityProbe(AvailabilityProbe probe);
    void SetCommittedCallback(CommittedCallback callback);
    void SetCancelledCallback(CancelledCallback callback);

    /**
     * Sets the legacy enable/local-desktop context used to continuously
     * validate the saved chord while the recorder is not capturing.
     */
    void SetValidationContext(bool enabled, bool localDesktopHotkey);

    /** Rebinds the control to an immutable settings generation. */
    void SetValue(HotkeyChord value, std::uint64_t generation);
    [[nodiscard]] HotkeyChord Value() const noexcept;
    [[nodiscard]] bool IsCapturing() const noexcept;

    void BeginCapture();
    void CancelCapture();
    /** Accepts a chord intercepted by the host's WM_HOTKEY handler. */
    void CaptureRegisteredHotkey(
        std::uint32_t modifiers,
        std::uint32_t virtualKey);
    void Close() noexcept;

private:
    std::shared_ptr<HotkeyRecorderState> state_;
};

} // namespace snowdesktop::winui
