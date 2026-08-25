#include "pch.h"

#include "hotkey_recorder.h"

#include <winrt/Microsoft.UI.Xaml.Automation.Peers.h>
#include <winrt/Microsoft.UI.Xaml.Media.h>
#include <winrt/Windows.System.h>

#include <array>
#include <utility>
#include <vector>

namespace snowdesktop::winui
{
namespace mux = winrt::Microsoft::UI::Xaml;
namespace muxa = winrt::Microsoft::UI::Xaml::Automation;
namespace muxc = winrt::Microsoft::UI::Xaml::Controls;
namespace muxi = winrt::Microsoft::UI::Xaml::Input;
namespace muxm = winrt::Microsoft::UI::Xaml::Media;
namespace mud = winrt::Microsoft::UI::Dispatching;

namespace
{
std::uint32_t SamplePhysicalModifiers() noexcept
{
    std::uint32_t modifiers = 0;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LCONTROL) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RCONTROL) & 0x8000) != 0)
        modifiers |= HotkeyRecorderRules::ModifierControl;
    if ((GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LMENU) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RMENU) & 0x8000) != 0)
        modifiers |= HotkeyRecorderRules::ModifierAlt;
    if ((GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_LSHIFT) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RSHIFT) & 0x8000) != 0)
        modifiers |= HotkeyRecorderRules::ModifierShift;
    if ((GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0)
        modifiers |= HotkeyRecorderRules::ModifierWindows;
    return modifiers;
}

std::wstring VirtualKeyName(std::uint32_t virtualKey)
{
    switch (virtualKey)
    {
    case VK_SPACE: return L"Space";
    case VK_TAB: return L"Tab";
    case VK_PRIOR: return L"Page Up";
    case VK_NEXT: return L"Page Down";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_LEFT: return L"Left";
    case VK_UP: return L"Up";
    case VK_RIGHT: return L"Right";
    case VK_DOWN: return L"Down";
    default: break;
    }

    UINT scanCode = MapVirtualKeyW(virtualKey, MAPVK_VK_TO_VSC);
    if (virtualKey == VK_INSERT || virtualKey == VK_DELETE ||
        virtualKey == VK_HOME || virtualKey == VK_END ||
        virtualKey == VK_PRIOR || virtualKey == VK_NEXT ||
        virtualKey == VK_LEFT || virtualKey == VK_RIGHT ||
        virtualKey == VK_UP || virtualKey == VK_DOWN)
        scanCode |= 0x0100;
    wchar_t name[96]{};
    if (GetKeyNameTextW(static_cast<LONG>(scanCode << 16),
            name, static_cast<int>(std::size(name))) > 0)
        return name;

    wchar_t fallback[24]{};
    swprintf_s(fallback, L"VK 0x%02X", virtualKey);
    return fallback;
}

std::vector<std::wstring> ChordParts(
    HotkeyChord chord,
    const HotkeyRecorderText& text)
{
    if (chord.Empty()) return {};
    std::vector<std::wstring> parts;
    if ((chord.modifiers & HotkeyRecorderRules::ModifierControl) != 0)
        parts.push_back(text.control);
    if ((chord.modifiers & HotkeyRecorderRules::ModifierAlt) != 0)
        parts.push_back(text.alt);
    if ((chord.modifiers & HotkeyRecorderRules::ModifierShift) != 0)
        parts.push_back(text.shift);
    if ((chord.modifiers & HotkeyRecorderRules::ModifierWindows) != 0)
        parts.push_back(text.windows);
    parts.push_back(VirtualKeyName(chord.virtualKey));
    return parts;
}

std::wstring FormatChord(
    HotkeyChord chord,
    const HotkeyRecorderText& text)
{
    const auto parts = ChordParts(chord, text);
    if (parts.empty()) return text.none;
    std::wstring result;
    for (const auto& part : parts)
    {
        if (!result.empty()) result += L" + ";
        result += part;
    }
    return result;
}

[[nodiscard]] bool IsHighContrastEnabled() noexcept
{
    HIGHCONTRASTW state{};
    state.cbSize = sizeof(state);
    return SystemParametersInfoW(
               SPI_GETHIGHCONTRAST, sizeof(state), &state, 0) != FALSE &&
        (state.dwFlags & HCF_HIGHCONTRASTON) != 0;
}

[[nodiscard]] muxm::SolidColorBrush MakeBrush(
    std::uint8_t red,
    std::uint8_t green,
    std::uint8_t blue)
{
    return muxm::SolidColorBrush(
        winrt::Windows::UI::Color{0xFF, red, green, blue});
}

[[nodiscard]] muxm::SolidColorBrush MakeSystemBrush(int colorIndex)
{
    const COLORREF color = GetSysColor(colorIndex);
    return MakeBrush(
        static_cast<std::uint8_t>(GetRValue(color)),
        static_cast<std::uint8_t>(GetGValue(color)),
        static_cast<std::uint8_t>(GetBValue(color)));
}

struct KeycapPalette
{
    muxm::Brush surface{nullptr};
    muxm::Brush depth{nullptr};
    muxm::Brush stroke{nullptr};
    muxm::Brush foreground{nullptr};
};

[[nodiscard]] KeycapPalette ResolveKeycapPalette(
    const muxc::UserControl& root,
    bool capturing)
{
    if (IsHighContrastEnabled())
    {
        return {
            MakeSystemBrush(COLOR_BTNFACE),
            MakeSystemBrush(COLOR_BTNSHADOW),
            MakeSystemBrush(capturing ? COLOR_HIGHLIGHT : COLOR_BTNTEXT),
            MakeSystemBrush(COLOR_BTNTEXT),
        };
    }

    const bool dark = root && root.ActualTheme() == mux::ElementTheme::Dark;
    if (dark)
    {
        return {
            MakeBrush(0x3B, 0x3B, 0x3B),
            MakeBrush(0x18, 0x18, 0x18),
            capturing ? MakeBrush(0x60, 0xCD, 0xFF)
                      : MakeBrush(0x70, 0x70, 0x70),
            MakeBrush(0xFF, 0xFF, 0xFF),
        };
    }
    return {
        MakeBrush(0xFC, 0xFC, 0xFC),
        MakeBrush(0x9A, 0x9A, 0x9A),
        capturing ? MakeBrush(0x00, 0x67, 0xC0)
                  : MakeBrush(0xBC, 0xBC, 0xBC),
        MakeBrush(0x1A, 0x1A, 0x1A),
    };
}

[[nodiscard]] mux::UIElement CreateKeycap(
    const std::wstring& label,
    const KeycapPalette& palette)
{
    muxc::TextBlock text{};
    text.Text(label);
    text.FontSize(12.0);
    text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    text.Foreground(palette.foreground);
    text.TextAlignment(mux::TextAlignment::Center);
    text.VerticalAlignment(mux::VerticalAlignment::Center);

    muxc::Border surface{};
    surface.MinWidth(30.0);
    surface.MinHeight(26.0);
    surface.Padding({8.0, 3.0, 8.0, 3.0});
    surface.Background(palette.surface);
    surface.BorderBrush(palette.stroke);
    surface.BorderThickness({1.0, 1.0, 1.0, 1.0});
    surface.CornerRadius({4.0, 4.0, 4.0, 4.0});
    surface.Child(text);

    muxc::Border depth{};
    depth.Background(palette.depth);
    depth.Padding({0.0, 0.0, 0.0, 2.0});
    depth.CornerRadius({5.0, 5.0, 5.0, 5.0});
    depth.Child(surface);
    return depth;
}

[[nodiscard]] mux::UIElement CreateChordVisual(
    HotkeyChord chord,
    const HotkeyRecorderText& text,
    const muxc::UserControl& root,
    bool capturing)
{
    const auto parts = ChordParts(chord, text);
    if (parts.empty())
    {
        muxc::TextBlock empty{};
        empty.Text(text.none);
        empty.Opacity(IsHighContrastEnabled() ? 1.0 : 0.72);
        empty.VerticalAlignment(mux::VerticalAlignment::Center);
        return empty;
    }

    const KeycapPalette palette = ResolveKeycapPalette(root, capturing);
    muxc::StackPanel panel{};
    panel.Orientation(muxc::Orientation::Horizontal);
    panel.Spacing(6.0);
    panel.VerticalAlignment(mux::VerticalAlignment::Center);
    for (std::size_t index = 0; index < parts.size(); ++index)
    {
        if (index != 0)
        {
            muxc::TextBlock plus{};
            plus.Text(L"+");
            plus.Opacity(IsHighContrastEnabled() ? 1.0 : 0.68);
            plus.VerticalAlignment(mux::VerticalAlignment::Center);
            panel.Children().Append(plus);
        }
        panel.Children().Append(CreateKeycap(parts[index], palette));
    }
    return panel;
}
} // namespace

struct HotkeyRecorderState
{
    muxc::UserControl root;
    muxc::StackPanel panel;
    muxc::Button button;
    muxc::TextBlock status;
    mud::DispatcherQueue dispatcher{ nullptr };
    HotkeyRecorderRules rules;
    HotkeyRecorderText text;
    HotkeyRecorder::AvailabilityProbe availabilityProbe;
    HotkeyRecorder::CommittedCallback committed;
    HotkeyRecorder::CancelledCallback cancelled;
    std::wstring conflictMessage;
    std::wstring committedConflictMessage;
    HotkeyAvailability committedAvailability = HotkeyAvailability::Unknown;
    std::uint64_t committedRequestId = 0;
    bool enabled = true;
    bool localDesktopHotkey = false;
    bool alive = true;
    bool lastActionCleared = false;

    winrt::event_token clickToken{};
    winrt::event_token keyDownToken{};
    winrt::event_token keyUpToken{};
    winrt::event_token lostFocusToken{};
    winrt::event_token themeChangedToken{};
};

namespace
{
std::wstring StatusWithDetails(
    const std::wstring& status,
    const std::wstring& details)
{
    if (details.empty()) return status;
    return status + L" — " + details;
}

void UpdateVisuals(const std::shared_ptr<HotkeyRecorderState>& state)
{
    const HotkeyChord displayed = state->rules.Active()
        ? state->rules.Candidate() : state->rules.Committed();
    state->button.Content(CreateChordVisual(
        displayed, state->text, state->root, state->rules.Active()));

    std::wstring statusText;
    if (state->rules.Active())
    {
        switch (state->rules.Availability())
        {
        case HotkeyAvailability::Checking:
            statusText = StatusWithDetails(
                state->text.checking, state->text.captureActive);
            break;
        case HotkeyAvailability::Available:
            statusText = L"✓ " + StatusWithDetails(
                state->text.availableStatus, state->text.captureActive);
            break;
        case HotkeyAvailability::Conflict:
            statusText = L"⚠ " + (state->conflictMessage.empty()
                ? StatusWithDetails(
                    state->text.inUseStatus, state->text.systemConflict)
                : state->conflictMessage);
            break;
        default:
            statusText = state->text.captureHint;
            break;
        }
    }
    else
    {
        if (!state->enabled)
        {
            statusText = state->text.disabled;
        }
        else if (displayed.Empty())
        {
            statusText = L"⚠ " + StatusWithDetails(
                state->text.none, state->text.notSetWarning);
        }
        else if (state->committedAvailability ==
            HotkeyAvailability::Checking)
        {
            statusText = state->text.checking;
        }
        else if (state->committedAvailability ==
            HotkeyAvailability::Conflict)
        {
            statusText = L"⚠ " +
                (state->committedConflictMessage.empty()
                    ? StatusWithDetails(
                        state->text.inUseStatus, state->text.systemConflict)
                    : state->committedConflictMessage);
        }
        else if (displayed.modifiers == 0 &&
            !state->localDesktopHotkey)
        {
            statusText = L"⚠ " + StatusWithDetails(
                state->text.noModifierStatus,
                state->text.noModifierWarning);
        }
        else if (state->committedAvailability ==
            HotkeyAvailability::Available)
        {
            statusText = L"✓ " + StatusWithDetails(
                state->text.availableStatus, state->text.available);
        }
        else
        {
            statusText = state->text.idleHint;
        }
    }
    state->status.Text(statusText);
    muxa::AutomationProperties::SetName(
        state->button, state->text.automationName);
    muxa::AutomationProperties::SetHelpText(
        state->button, statusText);
    muxa::AutomationProperties::SetItemStatus(
        state->button, FormatChord(displayed, state->text));
    muxa::AutomationProperties::SetName(
        state->status, statusText);
    muxa::AutomationProperties::SetLiveSetting(
        state->status,
        muxa::Peers::AutomationLiveSetting::Polite);
}

void ProbeCommittedAvailability(
    const std::shared_ptr<HotkeyRecorderState>& state)
{
    const std::uint64_t requestId = ++state->committedRequestId;
    state->committedConflictMessage.clear();
    const HotkeyChord chord = state->rules.Committed();
    const std::uint64_t generation = state->rules.Generation();
    if (!state->enabled || chord.Empty())
    {
        state->committedAvailability = HotkeyAvailability::Unknown;
        UpdateVisuals(state);
        return;
    }

    state->committedAvailability = HotkeyAvailability::Checking;
    UpdateVisuals(state);
    const std::weak_ptr<HotkeyRecorderState> weak = state;
    auto completion = [weak, chord, generation, requestId](
                          bool available, std::wstring message) mutable {
        const auto current = weak.lock();
        if (!current || !current->alive)
            return;
        auto apply = [weak, chord, generation, requestId, available,
                         message = std::move(message)]() mutable {
            const auto target = weak.lock();
            if (!target || !target->alive || !target->enabled ||
                target->committedRequestId != requestId ||
                target->rules.Generation() != generation ||
                target->rules.Committed() != chord)
            {
                return;
            }
            target->committedAvailability = available
                ? HotkeyAvailability::Available
                : HotkeyAvailability::Conflict;
            target->committedConflictMessage = std::move(message);
            UpdateVisuals(target);
        };
        if (!current->dispatcher || current->dispatcher.HasThreadAccess())
            apply();
        else
            (void)current->dispatcher.TryEnqueue(std::move(apply));
    };

    if (state->availabilityProbe)
    {
        try
        {
            state->availabilityProbe(
                chord, generation, requestId, completion);
        }
        catch (...)
        {
            completion(false, StatusWithDetails(
                state->text.inUseStatus, state->text.systemConflict));
        }
    }
    else
    {
        completion(true, {});
    }
}

void ApplyTransition(
    const std::shared_ptr<HotkeyRecorderState>& state,
    const HotkeyRecorderTransition& transition)
{
    switch (transition.action)
    {
    case HotkeyRecorderAction::ProbeAvailability:
    {
        state->lastActionCleared = false;
        state->conflictMessage.clear();
        UpdateVisuals(state);
        const std::weak_ptr<HotkeyRecorderState> weak = state;
        auto completion = [weak,
            generation = transition.generation,
            requestId = transition.requestId](
                bool available, std::wstring message) mutable {
            const auto current = weak.lock();
            if (!current || !current->alive)
                return;
            auto apply = [weak, generation, requestId, available,
                message = std::move(message)]() mutable {
                const auto target = weak.lock();
                if (!target || !target->alive ||
                    !target->rules.ApplyAvailability(
                        generation, requestId, available))
                    return;
                target->conflictMessage = std::move(message);
                UpdateVisuals(target);
            };
            if (!current->dispatcher || current->dispatcher.HasThreadAccess())
                apply();
            else
                (void)current->dispatcher.TryEnqueue(std::move(apply));
        };
        if (state->availabilityProbe)
        {
            try
            {
                state->availabilityProbe(
                    transition.chord,
                    transition.generation,
                    transition.requestId,
                    completion);
            }
            catch (...)
            {
                completion(false, StatusWithDetails(
                    state->text.inUseStatus, state->text.systemConflict));
            }
        }
        else
        {
            completion(true, {});
        }
        return;
    }
    case HotkeyRecorderAction::Commit:
    case HotkeyRecorderAction::Clear:
        state->lastActionCleared =
            transition.action == HotkeyRecorderAction::Clear;
        ProbeCommittedAvailability(state);
        if (state->committed)
            state->committed(transition.chord);
        return;
    case HotkeyRecorderAction::Cancel:
        UpdateVisuals(state);
        if (state->cancelled)
            state->cancelled();
        return;
    case HotkeyRecorderAction::None:
        return;
    }
}
} // namespace

HotkeyRecorder::HotkeyRecorder()
    : state_(std::make_shared<HotkeyRecorderState>())
{
    state_->dispatcher = mud::DispatcherQueue::GetForCurrentThread();
    state_->panel.Spacing(6.0);
    state_->button.HorizontalAlignment(mux::HorizontalAlignment::Stretch);
    state_->button.HorizontalContentAlignment(
        mux::HorizontalAlignment::Left);
    state_->button.MinHeight(44.0);
    state_->button.Padding({10.0, 7.0, 10.0, 7.0});
    state_->button.IsTabStop(true);
    state_->button.UseSystemFocusVisuals(true);
    state_->status.TextWrapping(mux::TextWrapping::Wrap);
    state_->status.IsHitTestVisible(false);
    state_->panel.Children().Append(state_->button);
    state_->panel.Children().Append(state_->status);
    state_->root.Content(state_->panel);

    const std::weak_ptr<HotkeyRecorderState> weak = state_;
    state_->themeChangedToken = state_->root.ActualThemeChanged(
        [weak](const auto&, const auto&) {
            const auto state = weak.lock();
            if (state && state->alive)
                UpdateVisuals(state);
        });
    state_->clickToken = state_->button.Click(
        [weak](const auto&, const auto&) {
            const auto state = weak.lock();
            if (!state || !state->alive || !state->enabled) return;
            state->rules.BeginCapture();
            state->conflictMessage.clear();
            UpdateVisuals(state);
            (void)state->button.Focus(mux::FocusState::Programmatic);
        });
    state_->keyDownToken = state_->button.KeyDown(
        [weak](const auto&, const muxi::KeyRoutedEventArgs& args) {
            const auto state = weak.lock();
            if (!state || !state->alive || !state->rules.Active()) return;
            const auto virtualKey =
                static_cast<std::uint32_t>(args.OriginalKey());
            const auto transition = state->rules.KeyDown(
                virtualKey, SamplePhysicalModifiers());
            args.Handled(true);
            ApplyTransition(state, transition);
        });
    state_->keyUpToken = state_->button.KeyUp(
        [weak](const auto&, const muxi::KeyRoutedEventArgs& args) {
            const auto state = weak.lock();
            if (!state || !state->alive || !state->rules.Active()) return;
            (void)state->rules.KeyUp(
                static_cast<std::uint32_t>(args.OriginalKey()));
            args.Handled(true);
        });
    state_->lostFocusToken = state_->button.LostFocus(
        [weak](const auto&, const mux::RoutedEventArgs&) {
            const auto state = weak.lock();
            if (!state || !state->alive) return;
            ApplyTransition(state, state->rules.LoseFocus());
        });

    state_->rules.Reset({}, 0);
    UpdateVisuals(state_);
}

HotkeyRecorder::~HotkeyRecorder()
{
    Close();
}

muxc::UserControl HotkeyRecorder::Root() const noexcept
{
    return state_ ? state_->root : nullptr;
}

mux::FrameworkElement HotkeyRecorder::FocusTarget() const noexcept
{
    return state_ ? state_->button : nullptr;
}

void HotkeyRecorder::SetText(HotkeyRecorderText text)
{
    if (!state_ || !state_->alive) return;
    state_->text = std::move(text);
    ProbeCommittedAvailability(state_);
}

void HotkeyRecorder::SetAvailabilityProbe(AvailabilityProbe probe)
{
    if (state_ && state_->alive)
    {
        state_->availabilityProbe = std::move(probe);
        ProbeCommittedAvailability(state_);
    }
}

void HotkeyRecorder::SetCommittedCallback(CommittedCallback callback)
{
    if (state_ && state_->alive)
        state_->committed = std::move(callback);
}

void HotkeyRecorder::SetCancelledCallback(CancelledCallback callback)
{
    if (state_ && state_->alive)
        state_->cancelled = std::move(callback);
}

void HotkeyRecorder::SetValidationContext(
    bool enabled,
    bool localDesktopHotkey)
{
    if (!state_ || !state_->alive) return;
    state_->enabled = enabled;
    state_->localDesktopHotkey = localDesktopHotkey;
    ProbeCommittedAvailability(state_);
}

void HotkeyRecorder::SetValue(
    HotkeyChord value,
    std::uint64_t generation)
{
    if (!state_ || !state_->alive) return;
    state_->conflictMessage.clear();
    state_->lastActionCleared = false;
    state_->rules.Reset(value, generation);
    ProbeCommittedAvailability(state_);
}

HotkeyChord HotkeyRecorder::Value() const noexcept
{
    return state_ ? state_->rules.Committed() : HotkeyChord{};
}

bool HotkeyRecorder::IsCapturing() const noexcept
{
    return state_ && state_->alive && state_->rules.Active();
}

void HotkeyRecorder::BeginCapture()
{
    if (!state_ || !state_->alive || !state_->enabled) return;
    state_->rules.BeginCapture();
    state_->conflictMessage.clear();
    UpdateVisuals(state_);
    (void)state_->button.Focus(mux::FocusState::Programmatic);
}

void HotkeyRecorder::CancelCapture()
{
    if (!state_ || !state_->alive || !state_->rules.Active()) return;
    ApplyTransition(state_, state_->rules.KeyDown(
        HotkeyRecorderRules::KeyEscape, 0));
}

void HotkeyRecorder::CaptureRegisteredHotkey(
    std::uint32_t modifiers,
    std::uint32_t virtualKey)
{
    if (!state_ || !state_->alive || !state_->rules.Active()) return;
    ApplyTransition(state_, state_->rules.KeyDown(
        virtualKey,
        modifiers & (HotkeyRecorderRules::ModifierAlt |
            HotkeyRecorderRules::ModifierControl |
            HotkeyRecorderRules::ModifierShift |
            HotkeyRecorderRules::ModifierWindows)));
}

void HotkeyRecorder::Close() noexcept
{
    if (!state_ || !state_->alive) return;
    state_->alive = false;
    ++state_->committedRequestId;
    state_->rules.Close();
    try
    {
        state_->button.Click(state_->clickToken);
        state_->button.KeyDown(state_->keyDownToken);
        state_->button.KeyUp(state_->keyUpToken);
        state_->button.LostFocus(state_->lostFocusToken);
        state_->root.ActualThemeChanged(state_->themeChangedToken);
        state_->availabilityProbe = {};
        state_->committed = {};
        state_->cancelled = {};
    }
    catch (...)
    {
    }
}

} // namespace snowdesktop::winui
