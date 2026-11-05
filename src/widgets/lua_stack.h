#pragma once

#include "common/types.h"
#include <string_view>
#include <vector>

namespace snowdesktop::lua_stack
{
bool IsSurface(DesktopWidgetType type) noexcept;
size_t Owner(const std::vector<DesktopWidget>& widgets, std::wstring_view member);
size_t Active(const std::vector<DesktopWidget>& widgets, size_t owner);
std::vector<std::wstring> Members(const DesktopWidget& widget);
bool Supports(const DesktopWidget& widget, GridSpan span) noexcept;
bool CanCombine(const std::vector<DesktopWidget>& widgets, size_t source, size_t target,
    bool checkCachedSizes = true);
bool Combine(std::vector<DesktopWidget>& widgets, size_t source, size_t target,
    std::wstring newId);
void Normalize(std::vector<DesktopWidget>& widgets);
// Dissolution inherits the wrapper's geometry; no member settings/storage are copied.
void Dissolve(std::vector<DesktopWidget>& widgets);
}
