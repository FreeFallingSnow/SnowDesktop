#pragma once

#include "dock_fullscreen_types.h"
#include <optional>
#include <string>
#include <vector>

namespace snowdesktop::dock_fullscreen
{
DockFullscreenPolicy Normalize(int value) noexcept;

DockFullscreenPolicy LoadPolicy(
    std::optional<int> current, std::optional<bool> legacy,
    DockFullscreenPolicy fallback = DockFullscreenPolicy::FullProtection) noexcept;

std::wstring NormalizeExecutable(std::wstring path);

bool SameExecutable(const std::wstring& a, const std::wstring& b) noexcept;

void NormalizeExceptions(std::vector<DockFullscreenException>& entries);

DockFullscreenPolicy SelectPolicy(DockFullscreenPolicy fallback,
    const std::vector<DockFullscreenException>& entries, const std::wstring& executable);

bool BlocksReveal(DockFullscreenPolicy policy, DockRevealSource source,
    bool monitorFullscreen) noexcept;

bool StartsKeyboardSession(DockRevealSource source,
    bool foregroundHasHigherInputPrivilege = false,
    bool foregroundFullscreen = false) noexcept;
} // namespace snowdesktop::dock_fullscreen
