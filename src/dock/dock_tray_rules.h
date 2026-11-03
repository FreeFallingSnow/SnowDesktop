#pragma once

#include "dock_tray_types.h"
#include <cstddef>
#include <span>
#include <string_view>

namespace snowdesktop::dock_tray
{
ClickAction ResolveClick(bool normalWindow, std::size_t instances, bool singleClickLaunch, bool doubleGesture);

std::wstring ExecutableName(std::wstring_view path);

bool Equal(std::wstring_view a, std::wstring_view b);

std::wstring NormalizeTarget(std::wstring path, bool matchPath);

bool Matches(const DockTrayRule& rule, std::wstring_view executable);

const DockTrayRule* FindRule(std::span<const DockTrayRule> rules, std::wstring_view executable);

void NormalizeRules(std::vector<DockTrayRule>& rules);
}
