#include "pch.h"

#include "hotkey_recorder.h"

#include <winrt/Microsoft.UI.Xaml.Automation.h>
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

std::uint32_t ResolveRoutedVirtualKey(
    const muxi::KeyRoutedEventArgs& args) noexcept
{
    const auto routedVirtualKey =
        static_cast<std::uint32_t>(args.Key());
    if (routedVirtualKey != HotkeyRecorderRules::KeyProcess)
        return routedVirtualKey;

    const auto keyStatus = args.KeyStatus();
    UINT scanCode = keyStatus.ScanCode;
    if (scanCode == 0)
        return 0;
    if (keyStatus.IsExtendedKey)
        scanCode |= 0xE000u;
    const UINT scanCodeVirtualKey = MapVirtualKeyExW(
        scanCode,
        MAPVK_VSC_TO_VK_EX,
        GetKeyboardLayout(0));
    return HotkeyRecorderRules::ResolveCapturedVirtualKey(
        routedVirtualKey, scanCodeVirtualKey);
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
    std::vector<std::wstring> parts;
    if ((chord.modifiers & HotkeyRecorderRules::ModifierControl) != 0)
        parts.push_back(text.control);
    if ((chord.modifiers & HotkeyRecorderRules::ModifierAlt) != 0)
        parts.push_back(text.alt);
    if ((chord.modifiers & HotkeyRecorderRules::ModifierShift) != 0)
        parts.push_back(text.shift);
    if ((chord.modifiers & HotkeyRecorderRules::ModifierWindows) != 0)
        parts.push_back(text.windows);
    if (!chord.Empty())
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
    const KeycapPalette& palette,
    bool large)
{
    muxc::TextBlock text{};
    text.Text(label);
    text.FontSize(large ? 16.0 : 12.0);
    text.FontWeight(winrt::Windows::UI::Text::FontWeights::SemiBold());
    text.Foreground(palette.foreground);
    text.TextAlignment(mux::TextAlignment::Center);
    text.VerticalAlignment(mux::VerticalAlignment::Center);

    muxc::Border surface{};
    surface.MinWidth(large ? 48.0 : 30.0);
    surface.MinHeight(large ? 50.0 : 26.0);
    surface.Padding(large
            ? mux::Thickness{20.0, 12.0, 20.0, 12.0}
            : mux::Thickness{8.0, 3.0, 8.0, 3.0});
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
    bool capturing,
    bool large,
    const std::wstring& emptyText)
{
    const auto parts = ChordParts(chord, text);
    if (parts.empty())
    {
        muxc::TextBlock empty{};
        empty.Text(emptyText);
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
        panel.Children().Append(CreateKeycap(parts[index], palette, large));
    }
    return panel;
}

[[nodiscard]] mux::UIElement CreateEditButtonContent(
    HotkeyChord chord,
    const HotkeyRecorderText& text,
    const muxc::UserControl& root)
{
    muxc::StackPanel panel{};
    panel.Orientation(muxc::Orientation::Horizontal);
    panel.Spacing(8.0);
    panel.VerticalAlignment(mux::VerticalAlignment::Center);
    panel.Children().Append(CreateChordVisual(
        chord, text, root, false, false, text.none));

    muxc::FontIcon edit{};
    edit.Glyph(L"\xE70F");
    edit.FontSize(12.0);
    edit.VerticalAlignment(mux::VerticalAlignment::Center);
    muxa::AutomationProperties::SetAccessibilityView(
        edit, muxa::Peers::AccessibilityView::Raw);
    panel.Children().Append(edit);
    return panel;
}

[[nodiscard]] mux::UIElement CreateDialogActionContent(
    const wchar_t* glyph,
    const std::wstring& label)
{
    muxc::StackPanel panel{};
    panel.Orientation(muxc::Orientation::Horizontal);
    panel.Spacing(4.0);
    muxc::FontIcon icon{};
    icon.Glyph(glyph);
    icon.FontSize(12.0);
    muxc::TextBlock text{};
    text.Text(label);
    text.VerticalAlignment(mux::VerticalAlignment::Center);
    panel.Children().Append(icon);
    panel.Children().Append(text);
    return panel;
}
} // namespace

struct HotkeyRecorderState
{
    muxc::UserControl root;
    muxc::StackPanel panel;
    muxc::Button button;
    muxc::TextBlock status;
    muxc::ContentDialog dialog;
    muxc::StackPanel dialogContent;
    muxc::TextBlock dialogInstruction;
    muxc::ContentControl captureSurface;
    muxc::TextBlock dialogStatus;
    muxc::StackPanel dialogActions;
    muxc::HyperlinkButton restoreDefault;
    muxc::HyperlinkButton clear;
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
    bool dialogOpen = false;
    HotkeyChord defaultValue;
    bool hasDefaultValue = false;

    winrt::event_token clickToken{};
    winrt::event_token keyDownToken{};
    winrt::event_token keyUpToken{};
    winrt::event_token dialogOpenedToken{};
    winrt::event_token restoreDefaultToken{};
    winrt::event_token clearToken{};
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
    const HotkeyChord committed = state->rules.Committed();
    state->button.Content(CreateEditButtonContent(
        committed, state->text, state->root));

    std::wstring inlineStatus;
    if (!state->enabled)
    {
        inlineStatus = state->text.disabled;
    }
    else if (state->committedAvailability == HotkeyAvailability::Checking)
    {
        inlineStatus = state->text.checking;
    }
    else if (state->committedAvailability == HotkeyAvailability::Conflict)
    {
        inlineStatus = L"⚠ " +
            (state->committedConflictMessage.empty()
                ? StatusWithDetails(
                    state->text.inUseStatus, state->text.systemConflict)
                : state->committedConflictMessage);
    }
    else if (!committed.Empty() && committed.modifiers == 0 &&
        !state->localDesktopHotkey)
    {
        inlineStatus = L"⚠ " + StatusWithDetails(
            state->text.noModifierStatus, state->text.noModifierWarning);
    }

    state->status.Text(inlineStatus);
    state->status.Visibility(inlineStatus.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);

    const HotkeyChord candidate = state->rules.Active()
        ? state->rules.Candidate() : committed;
    state->captureSurface.Content(CreateChordVisual(candidate,
        state->text, state->root, true, true, state->text.captureHint));

    std::wstring dialogStatus;
    if (state->rules.Active())
    {
        switch (state->rules.Availability())
        {
        case HotkeyAvailability::Checking:
            dialogStatus = state->text.checking;
            break;
        case HotkeyAvailability::Available:
            dialogStatus = L"✓ " + StatusWithDetails(
                state->text.availableStatus, state->text.available);
            break;
        case HotkeyAvailability::Conflict:
            dialogStatus = L"⚠ " + (state->conflictMessage.empty()
                ? StatusWithDetails(
                    state->text.inUseStatus, state->text.systemConflict)
                : state->conflictMessage);
            break;
        default:
            dialogStatus = state->text.captureHint;
            break;
        }
    }
    state->dialogStatus.Text(dialogStatus);
    state->dialogStatus.Visibility(dialogStatus.empty()
            ? mux::Visibility::Collapsed
            : mux::Visibility::Visible);
    state->dialog.IsPrimaryButtonEnabled(state->rules.Active() &&
        !state->rules.Candidate().Empty() &&
        state->rules.Availability() == HotkeyAvailability::Available);
    state->restoreDefault.Visibility(state->hasDefaultValue
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);
    state->clear.Visibility(state->rules.Active() && !candidate.Empty()
            ? mux::Visibility::Visible
            : mux::Visibility::Collapsed);

    const std::wstring helpText = inlineStatus.empty()
        ? state->text.idleHint : inlineStatus;
    muxa::AutomationProperties::SetName(
        state->button, state->text.automationName);
    muxa::AutomationProperties::SetHelpText(
        state->button, helpText);
    muxa::AutomationProperties::SetItemStatus(
        state->button, FormatChord(committed, state->text));
    muxa::AutomationProperties::SetName(
        state->status, inlineStatus);
    muxa::AutomationProperties::SetLiveSetting(
        state->status,
        muxa::Peers::AutomationLiveSetting::Polite);
    muxa::AutomationProperties::SetName(
        state->captureSurface, state->text.automationName);
    muxa::AutomationProperties::SetHelpText(
        state->captureSurface, dialogStatus);
    muxa::AutomationProperties::SetItemStatus(
        state->captureSurface, FormatChord(candidate, state->text));
    muxa::AutomationProperties::SetName(
        state->dialogStatus, dialogStatus);
    muxa::AutomationProperties::SetLiveSetting(
        state->dialogStatus,
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

winrt::fire_and_forget ShowShortcutDialog(
    std::shared_ptr<HotkeyRecorderState> state)
{
    if (!state || !state->alive || !state->enabled || state->dialogOpen)
        co_return;

    state->dialogOpen = true;
    state->conflictMessage.clear();
    ApplyTransition(state, state->rules.BeginCapture());
    UpdateVisuals(state);

    try
    {
        state->dialog.XamlRoot(state->root.XamlRoot());
        state->dialog.RequestedTheme(state->root.ActualTheme());
        const auto result = co_await state->dialog.ShowAsync();
        if (!state->alive)
            co_return;

        if (result == muxc::ContentDialogResult::Primary)
        {
            const auto transition = state->rules.Commit();
            if (transition.action == HotkeyRecorderAction::Commit)
                ApplyTransition(state, transition);
            else
                ApplyTransition(state, state->rules.CancelCapture());
        }
        else if (state->rules.Active())
        {
            ApplyTransition(state, state->rules.CancelCapture());
        }
    }
    catch (...)
    {
        if (state->alive && state->rules.Active())
            ApplyTransition(state, state->rules.CancelCapture());
    }

    if (state->alive)
    {
        state->dialogOpen = false;
        UpdateVisuals(state);
    }
}
} // namespace

HotkeyRecorder::HotkeyRecorder()
    : state_(std::make_shared<HotkeyRecorderState>())
{
    state_->dispatcher = mud::DispatcherQueue::GetForCurrentThread();
    state_->panel.Spacing(6.0);
    state_->button.HorizontalAlignment(mux::HorizontalAlignment::Right);
    state_->button.HorizontalContentAlignment(
        mux::HorizontalAlignment::Left);
    state_->button.MinHeight(36.0);
    state_->button.Padding({4.0, 4.0, 4.0, 4.0});
    state_->button.IsTabStop(true);
    state_->button.UseSystemFocusVisuals(true);
    state_->status.TextWrapping(mux::TextWrapping::Wrap);
    state_->status.IsHitTestVisible(false);
    state_->status.TextAlignment(mux::TextAlignment::Right);
    state_->status.HorizontalAlignment(mux::HorizontalAlignment::Right);
    state_->panel.Children().Append(state_->button);
    state_->panel.Children().Append(state_->status);
    state_->root.Content(state_->panel);

    try
    {
        const auto resources = mux::Application::Current().Resources();
        if (const auto style = resources.TryLookup(
                winrt::box_value(L"SubtleButtonStyle")).try_as<mux::Style>())
        {
            state_->button.Style(style);
        }
        if (const auto background = resources.TryLookup(winrt::box_value(
                L"SolidBackgroundFillColorTertiaryBrush")).try_as<muxm::Brush>())
        {
            state_->captureSurface.Background(background);
        }
    }
    catch (...)
    {
        // The built-in styles are optional; default WinUI styling remains
        // fully functional when a resource is unavailable.
    }

    state_->dialog.Title(winrt::box_value(state_->text.automationName));
    state_->dialog.PrimaryButtonText(state_->text.apply);
    state_->dialog.CloseButtonText(state_->text.cancel);
    state_->dialog.DefaultButton(muxc::ContentDialogButton::Primary);

    state_->dialogContent.Spacing(12.0);
    state_->dialogContent.MinWidth(460.0);
    state_->dialogInstruction.Text(state_->text.captureActive);
    state_->dialogInstruction.TextWrapping(mux::TextWrapping::Wrap);
    state_->captureSurface.MinHeight(104.0);
    state_->captureSurface.Padding({16.0, 16.0, 16.0, 16.0});
    state_->captureSurface.CornerRadius({8.0, 8.0, 8.0, 8.0});
    state_->captureSurface.HorizontalContentAlignment(
        mux::HorizontalAlignment::Center);
    state_->captureSurface.VerticalContentAlignment(
        mux::VerticalAlignment::Center);
    state_->captureSurface.IsTabStop(true);
    state_->captureSurface.UseSystemFocusVisuals(true);
    state_->dialogStatus.TextWrapping(mux::TextWrapping::Wrap);
    state_->dialogStatus.TextAlignment(mux::TextAlignment::Center);
    state_->dialogStatus.HorizontalAlignment(
        mux::HorizontalAlignment::Center);
    state_->dialogActions.Orientation(muxc::Orientation::Horizontal);
    state_->dialogActions.Spacing(12.0);
    state_->dialogActions.HorizontalAlignment(
        mux::HorizontalAlignment::Center);
    state_->restoreDefault.Content(CreateDialogActionContent(
        L"\xE777", state_->text.restoreDefault));
    state_->clear.Content(CreateDialogActionContent(
        L"\xE894", state_->text.clear));
    state_->dialogActions.Children().Append(state_->restoreDefault);
    state_->dialogActions.Children().Append(state_->clear);
    state_->dialogContent.Children().Append(state_->dialogInstruction);
    state_->dialogContent.Children().Append(state_->captureSurface);
    state_->dialogContent.Children().Append(state_->dialogActions);
    state_->dialogContent.Children().Append(state_->dialogStatus);
    state_->dialog.Content(state_->dialogContent);

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
            ShowShortcutDialog(state);
        });
    state_->keyDownToken = state_->captureSurface.KeyDown(
        [weak](const auto&, const muxi::KeyRoutedEventArgs& args) {
            const auto state = weak.lock();
            if (!state || !state->alive || !state->rules.Active()) return;
            const auto virtualKey = ResolveRoutedVirtualKey(args);
            if (virtualKey == 0)
            {
                args.Handled(true);
                return;
            }
            if (virtualKey == HotkeyRecorderRules::KeyEnter ||
                virtualKey == HotkeyRecorderRules::KeyEscape ||
                virtualKey == HotkeyRecorderRules::KeyTab)
            {
                return;
            }
            const auto transition = state->rules.KeyDown(
                virtualKey, SamplePhysicalModifiers());
            args.Handled(true);
            ApplyTransition(state, transition);
        });
    state_->keyUpToken = state_->captureSurface.KeyUp(
        [weak](const auto&, const muxi::KeyRoutedEventArgs& args) {
            const auto state = weak.lock();
            if (!state || !state->alive || !state->rules.Active()) return;
            const auto virtualKey = ResolveRoutedVirtualKey(args);
            if (virtualKey == 0)
            {
                args.Handled(true);
                return;
            }
            if (virtualKey == HotkeyRecorderRules::KeyEnter ||
                virtualKey == HotkeyRecorderRules::KeyEscape ||
                virtualKey == HotkeyRecorderRules::KeyTab)
            {
                return;
            }
            (void)state->rules.KeyUp(
                virtualKey);
            args.Handled(true);
        });
    state_->dialogOpenedToken = state_->dialog.Opened(
        [weak](const auto&, const auto&) {
            const auto state = weak.lock();
            if (!state || !state->alive) return;
            (void)state->captureSurface.Focus(
                mux::FocusState::Programmatic);
        });
    state_->restoreDefaultToken = state_->restoreDefault.Click(
        [weak](const auto&, const auto&) {
            const auto state = weak.lock();
            if (!state || !state->alive || !state->rules.Active() ||
                !state->hasDefaultValue)
            {
                return;
            }
            ApplyTransition(state,
                state->rules.Restore(state->defaultValue));
            if (state->dialogOpen)
                state->dialog.Hide();
        });
    state_->clearToken = state_->clear.Click(
        [weak](const auto&, const auto&) {
            const auto state = weak.lock();
            if (!state || !state->alive || !state->rules.Active()) return;
            ApplyTransition(state, state->rules.Clear());
            if (state->dialogOpen)
                state->dialog.Hide();
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
    state_->dialog.Title(winrt::box_value(state_->text.automationName));
    state_->dialog.PrimaryButtonText(state_->text.apply);
    state_->dialog.CloseButtonText(state_->text.cancel);
    state_->dialogInstruction.Text(state_->text.captureActive);
    state_->restoreDefault.Content(CreateDialogActionContent(
        L"\xE777", state_->text.restoreDefault));
    state_->clear.Content(CreateDialogActionContent(
        L"\xE894", state_->text.clear));
    muxa::AutomationProperties::SetName(
        state_->restoreDefault, state_->text.restoreDefault);
    muxa::AutomationProperties::SetName(
        state_->clear, state_->text.clear);
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
    if (!enabled && state_->rules.Active())
        CancelCapture();
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

void HotkeyRecorder::SetDefaultValue(HotkeyChord value)
{
    if (!state_ || !state_->alive) return;
    state_->defaultValue = value;
    state_->hasDefaultValue = true;
    UpdateVisuals(state_);
}

HotkeyChord HotkeyRecorder::Value() const noexcept
{
    return state_ ? state_->rules.Committed() : HotkeyChord{};
}

bool HotkeyRecorder::IsCapturing() const noexcept
{
    return state_ && state_->alive && state_->rules.Active() &&
        state_->captureSurface.FocusState() != mux::FocusState::Unfocused;
}

void HotkeyRecorder::BeginCapture()
{
    if (!state_ || !state_->alive || !state_->enabled) return;
    ShowShortcutDialog(state_);
}

void HotkeyRecorder::CancelCapture()
{
    if (!state_ || !state_->alive || !state_->rules.Active()) return;
    ApplyTransition(state_, state_->rules.CancelCapture());
    if (state_->dialogOpen)
        state_->dialog.Hide();
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
        if (state_->dialogOpen)
            state_->dialog.Hide();
        state_->button.Click(state_->clickToken);
        state_->captureSurface.KeyDown(state_->keyDownToken);
        state_->captureSurface.KeyUp(state_->keyUpToken);
        state_->dialog.Opened(state_->dialogOpenedToken);
        state_->restoreDefault.Click(state_->restoreDefaultToken);
        state_->clear.Click(state_->clearToken);
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
