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
    Check(HotkeyRecorderRules::ResolveCapturedVirtualKey(
              HotkeyRecorderRules::KeyProcess, 0x20) == 0x20,
        "an IME process key resolves to the physical Space virtual key");
    Check(HotkeyRecorderRules::ResolveCapturedVirtualKey(
              HotkeyRecorderRules::KeyProcess, 0) == 0,
        "an unrecoverable IME process key is ignored");
    Check(HotkeyRecorderRules::ResolveCapturedVirtualKey('K', 'L') == 'K',
        "ordinary routed keys do not use the scan-code fallback");
    rules.Reset({ HotkeyRecorderRules::ModifierControl, 'K' }, 7);
    const auto initial = rules.BeginCapture();
    Check(rules.Active(), "capture starts explicitly");
    Check(initial.action == HotkeyRecorderAction::ProbeAvailability &&
            initial.chord == rules.Committed(),
        "opening the dialog rechecks the saved shortcut");
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
            HotkeyRecorderAction::None && rules.Active(),
        "Enter is reserved for the dialog and does not mutate the chord");

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
    Check(rules.KeyDown(HotkeyRecorderRules::KeyTab, 0).action ==
            HotkeyRecorderAction::None && rules.Active(),
        "Tab is reserved for dialog focus navigation");
    const auto commit = rules.Commit();
    Check(commit.action == HotkeyRecorderAction::Commit &&
            commit.chord == available.chord && !rules.Active(),
        "the explicit dialog action commits an available chord");

    (void)rules.BeginCapture();
    Check(rules.KeyDown(HotkeyRecorderRules::KeyEscape, 0).action ==
            HotkeyRecorderAction::None && rules.Active(),
        "Escape is reserved for ContentDialog cancellation");
    const auto cancel = rules.CancelCapture();
    Check(cancel.action == HotkeyRecorderAction::Cancel &&
            rules.Committed() == commit.chord,
        "explicit cancellation preserves the prior committed chord");

    (void)rules.BeginCapture();
    const auto deleteCandidate = rules.KeyDown(
        HotkeyRecorderRules::KeyDelete,
        HotkeyRecorderRules::ModifierControl);
    Check(deleteCandidate.action == HotkeyRecorderAction::ProbeAvailability &&
            deleteCandidate.chord.virtualKey ==
                HotkeyRecorderRules::KeyDelete,
        "Delete can be recorded as an action key");
    const auto clear = rules.Clear();
    Check(clear.action == HotkeyRecorderAction::Clear &&
            rules.Committed().Empty() && !rules.Active(),
        "the explicit Clear action commits an empty chord");
    rules.Reset({ HotkeyRecorderRules::ModifierAlt, 'A' }, 8);
    (void)rules.BeginCapture();
    Check(rules.KeyDown(HotkeyRecorderRules::KeyBack, 0).action ==
            HotkeyRecorderAction::ProbeAvailability &&
            rules.Committed().virtualKey == 'A',
        "Backspace is recorded without clearing the saved shortcut");
    const auto restored = rules.Restore(
        {HotkeyRecorderRules::ModifierAlt, 'R'});
    Check(restored.action == HotkeyRecorderAction::Commit &&
            restored.chord.virtualKey == 'R' && !rules.Active(),
        "Restore Default commits the supplied default shortcut");

    rules.Reset({ HotkeyRecorderRules::ModifierWindows, 'R' }, 20);
    (void)rules.BeginCapture();
    const auto focusCandidate = rules.KeyDown('D',
        HotkeyRecorderRules::ModifierWindows);
    Check(rules.ApplyAvailability(20, focusCandidate.requestId, true),
        "focus test chord becomes available");
    Check(rules.KeyUp('D').action == HotkeyRecorderAction::None &&
            rules.Candidate() == focusCandidate.chord,
        "KeyUp preserves the sampled physical chord for confirmation");
    Check(rules.LoseFocus().action == HotkeyRecorderAction::None &&
            rules.Active(),
        "moving focus inside the dialog does not auto-commit");
    Check(rules.Commit().action == HotkeyRecorderAction::Commit,
        "the dialog can commit after focus navigation");

    (void)rules.BeginCapture();
    const auto conflicting = rules.KeyDown('X', 0);
    Check(rules.ApplyAvailability(20, conflicting.requestId, false),
        "focus test chord becomes conflicting");
    Check(rules.LoseFocus().action == HotkeyRecorderAction::None &&
            rules.Active(),
        "focus changes do not dismiss a conflicting chord");
    Check(rules.CancelCapture().action == HotkeyRecorderAction::Cancel &&
            rules.Committed().virtualKey == 'D',
        "dialog cancellation restores the saved chord after a conflict");

    (void)rules.BeginCapture();
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
