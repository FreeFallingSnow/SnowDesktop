#pragma once

#include <string>

enum class DockFullscreenPolicy
{
    Allow = 0,
    BlockGestures = 1,
    FullProtection = 2
};

enum class DockRevealSource
{
    AssociatedSurface,
    Gesture,
    Hotkey,
    WindowAction
};

struct DockFullscreenException
{
    bool operator==(const DockFullscreenException&) const = default;
    std::wstring executable;
    DockFullscreenPolicy policy = DockFullscreenPolicy::Allow;
};

struct DockFullscreenApplication
{
    bool operator==(const DockFullscreenApplication&) const = default;
    std::wstring name, executable;
};
