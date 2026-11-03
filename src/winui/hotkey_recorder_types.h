#pragma once

#include "hotkey_recorder_rules.h"

#include <functional>
#include <string>

namespace snowdesktop::winui
{

struct HotkeyRecorderText
{
    std::wstring automationName = L"Keyboard shortcut recorder";
    std::wstring idleHint = L"Select to record a keyboard shortcut";
    std::wstring captureHint =
        L"Press a shortcut";
    std::wstring checking = L"Checking shortcut availability";
    std::wstring available = L"Shortcut is available";
    std::wstring conflict = L"Shortcut conflicts with another command";
    std::wstring cleared = L"Shortcut cleared";
    std::wstring none = L"None";
    std::wstring disabled = L"Disabled";
    std::wstring captureActive = L"Press a shortcut, then select Apply.";
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
    std::wstring apply = L"Apply";
    std::wstring cancel = L"Cancel";
    std::wstring edit = L"Edit";
    std::wstring clear = L"Clear";
    std::wstring restoreDefault = L"Restore default";
};


using HotkeyAvailabilityCompletion =
    std::function<void(bool available, std::wstring conflictMessage)>;

} // namespace snowdesktop::winui
