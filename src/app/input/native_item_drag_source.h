#pragma once

#include "core/dock.h"

#include <utility>
#include <vector>

namespace snowdesktop::native_item_drag_source
{
// This is the native pointer dispatcher's source boundary. Keep eligibility
// and the selected runtime items together so tests cannot silently supply a
// payload that the actual pointer-start route would refuse to capture.
inline bool CanStart(const Item* pressed)
{
    return pressed && !dynamic_cast<const DockRunningItem*>(pressed);
}
struct Captured
{
    Container* source = nullptr;
    std::vector<Item*> items;
};
template<class ReadSelected>
Captured Capture(Item* pressed, bool pressedDockEntryWithoutSelection, ReadSelected&& selected)
{
    Captured result;
    if (!CanStart(pressed)) return result;
    result.source = pressed->GetContainer();
    result.items = std::forward<ReadSelected>(selected)(result.source);
    if (result.items.empty() && pressedDockEntryWithoutSelection) result.items.push_back(pressed);
    return result;
}
}
