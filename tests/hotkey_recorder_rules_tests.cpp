#include "winui/hotkey_recorder_rules.h"

#include <iostream>

namespace
{
int failures = 0;

void Check(bool condition, const char* message)
{
    if (condition) return;
    std::cerr << "FAIL: " << message << '\n';
    ++failures;
}
}

int main()
{
    using namespace snowdesktop::winui;

    HotkeyRecorderRules rules;
    rules.Reset({ HotkeyRecorderRules::ModifierControl, 'K' }, 7);
    rules.BeginCapture();
    Check(rules.Active(), "capture starts explicitly");
    Check(rules.KeyDown(HotkeyRecorderRules::KeyControl,
              HotkeyRecorderRules::ModifierControl).action ==
            HotkeyRecorderAction::None,
        "a standalone modifier is ignored");
    Check(rules.KeyDown(HotkeyRecorderRules::KeyLeftShift,
              HotkeyRecorderRules::ModifierShift).action ==
            HotkeyRecorderAction::None,
        "a left/right-specific modifier is ignored");

    const auto candidate = rules.KeyDown('P',
        HotkeyRecorderRules::ModifierControl |
            HotkeyRecorderRules::ModifierShift);
    Check(candidate.action == HotkeyRecorderAction::ProbeAvailability &&
            candidate.chord.virtualKey == 'P' &&
            candidate.chord.modifiers ==
                (HotkeyRecorderRules::ModifierControl |
                 HotkeyRecorderRules::ModifierShift),
        "a physical chord requests an availability probe");
    Check(!rules.ApplyAvailability(6, candidate.requestId, true),
        "a result from an old generation is rejected");
    Check(!rules.ApplyAvailability(7, candidate.requestId - 1, true),
        "a superseded probe result is rejected");
    Check(rules.ApplyAvailability(7, candidate.requestId, false) &&
            rules.Availability() == HotkeyAvailability::Conflict,
        "an unavailable chord enters conflict state");
    Check(rules.KeyDown(HotkeyRecorderRules::KeyEnter, 0).action ==
            HotkeyRecorderAction::None,
        "Enter cannot commit a conflicting chord");

    const auto registered = rules.KeyDown('K',
        HotkeyRecorderRules::ModifierControl);
    Check(registered.action == HotkeyRecorderAction::ProbeAvailability &&
            registered.chord.modifiers ==
                HotkeyRecorderRules::ModifierControl &&
            registered.chord.virtualKey == 'K',
        "a WM_HOTKEY chord follows the same availability path");

    const auto available = rules.KeyDown('O',
        HotkeyRecorderRules::ModifierAlt);
    Check(rules.ApplyAvailability(
            available.generation, available.requestId, true),
        "the current availability result is accepted");
    const auto commit = rules.KeyDown(
        HotkeyRecorderRules::KeyEnter, 0);
    Check(commit.action == HotkeyRecorderAction::Commit &&
            commit.chord == available.chord && !rules.Active(),
        "Enter commits an available chord");

    rules.BeginCapture();
    const auto cancel = rules.KeyDown(
        HotkeyRecorderRules::KeyEscape, 0);
    Check(cancel.action == HotkeyRecorderAction::Cancel &&
            rules.Committed() == commit.chord,
        "Escape cancels and preserves the prior committed chord");

    rules.BeginCapture();
    const auto clear = rules.KeyDown(
        HotkeyRecorderRules::KeyDelete, 0);
    Check(clear.action == HotkeyRecorderAction::Clear &&
            rules.Committed().Empty() && !rules.Active(),
        "Delete clears and commits an empty chord");
    rules.Reset({ HotkeyRecorderRules::ModifierAlt, 'A' }, 8);
    rules.BeginCapture();
    Check(rules.KeyDown(HotkeyRecorderRules::KeyBack, 0).action ==
            HotkeyRecorderAction::Clear && rules.Committed().Empty(),
        "Backspace also clears the committed chord");

    rules.Reset({ HotkeyRecorderRules::ModifierWindows, 'R' }, 20);
    rules.BeginCapture();
    const auto focusCandidate = rules.KeyDown('D',
        HotkeyRecorderRules::ModifierWindows);
    Check(rules.ApplyAvailability(20, focusCandidate.requestId, true),
        "focus test chord becomes available");
    Check(rules.KeyUp('D').action == HotkeyRecorderAction::None &&
            rules.Candidate() == focusCandidate.chord,
        "KeyUp preserves the sampled physical chord for confirmation");
    Check(rules.LoseFocus().action == HotkeyRecorderAction::Commit,
        "losing focus commits an available chord");

    rules.BeginCapture();
    const auto conflicting = rules.KeyDown('X', 0);
    Check(rules.ApplyAvailability(20, conflicting.requestId, false),
        "focus test chord becomes conflicting");
    Check(rules.LoseFocus().action == HotkeyRecorderAction::Cancel &&
            rules.Committed().virtualKey == 'D',
        "losing focus cancels a conflicting chord");

    rules.BeginCapture();
    const auto closing = rules.KeyDown('Z', 0);
    rules.Close();
    Check(!rules.ApplyAvailability(
            closing.generation, closing.requestId, true),
        "closing invalidates in-flight availability results");
    Check(!rules.Active(), "closing terminates capture");

    if (failures == 0)
        std::cout << "All hotkey recorder rule tests passed.\n";
    return failures == 0 ? 0 : 1;
}
