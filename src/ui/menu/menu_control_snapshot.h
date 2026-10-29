#pragma once

#include <windows.h>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::modern_menu
{
struct ControlSnapshot
{
    std::string id, parent, nativeControlKey;
    HWND window = nullptr;
    RECT bounds{}, clip{}; // Physical screen pixels; excludes shadow/scroll bands.
    std::wstring name, value;
    bool pane = false, textInput = false, enabled = true, checked = false;
    bool hasChildren = false, expanded = false, focused = false, selected = false;
    int depth = 0;
    std::size_t index = 0; // Owner-thread routing only, never retained by callers.
};
// Query/update only from the thread running Show(). No cross-thread HWND/Item
// access and no command execution are provided by this adapter.
std::vector<ControlSnapshot> ActiveControls();
// Exact companion supplied by the active owner-thread menu, never an HWND
// discovered by class name or by scanning unrelated application windows.
HWND ActiveCompanionWindow();
bool SetControlText(std::string_view id, std::wstring_view value);
}
