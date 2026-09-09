#pragma once

#include <windows.h>
#include <chrono>
#include <string>
#include <vector>

namespace snowdesktop::windows_desktop_layout
{
struct ItemPosition
{
    std::wstring parsingName;
    POINT screenPosition{};
};

struct Snapshot
{
    HRESULT status = E_FAIL;
    POINT spacing{};
    int iconSize = 0;
    std::vector<ItemPosition> items;

    bool Available() const noexcept
    {
        return SUCCEEDED(status) && spacing.x > 0 && spacing.y > 0;
    }
};

// Read Explorer's desktop view on a dedicated STA. A slow Shell cannot block
// initialization beyond this budget, and at most one capture remains in flight.
// This never changes native icon positions, visibility or view settings.
Snapshot Capture(std::chrono::milliseconds budget = std::chrono::milliseconds(2500));
}
