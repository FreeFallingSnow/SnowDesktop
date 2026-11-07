#pragma once

#include "dock_tray_types.h"
#include <span>
#include <string_view>

namespace snowdesktop::tray { struct Icon; struct Snapshot; }

namespace snowdesktop::dock_tray
{
const DockTrayRule* TargetRule(std::span<const DockTrayRule> rules, std::wstring_view source,
    std::wstring_view target, std::span<const std::wstring> validatedPayloadPaths = {});

std::vector<tray::Icon> CollectTargets(const tray::Snapshot& snapshot,
    std::span<const DockTrayRule> rules, std::wstring_view executable,
    std::span<const std::wstring> validatedPayloadPaths = {});

bool IsCurrentTarget(const tray::Snapshot& snapshot, const tray::Icon& selected);
}
