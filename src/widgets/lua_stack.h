#pragma once

#include "common/types.h"
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace snowdesktop::lua_stack
{
bool IsSurface(DesktopWidgetType type) noexcept;
size_t Owner(const std::vector<DesktopWidget>& widgets, std::wstring_view member);
size_t Active(const std::vector<DesktopWidget>& widgets, size_t owner);
// Return the actual desktop surface only for a currently displayed Lua instance.
size_t SurfaceOwner(const std::vector<DesktopWidget>& widgets, std::wstring_view instance);
std::vector<std::wstring> Members(const DesktopWidget& widget);
bool Supports(const DesktopWidget& widget, GridSpan span) noexcept;
bool CanCombine(const std::vector<DesktopWidget>& widgets, size_t source, size_t target,
    bool checkCachedSizes = true);
bool Combine(std::vector<DesktopWidget>& widgets, size_t source, size_t target,
    std::wstring newId);
void Normalize(std::vector<DesktopWidget>& widgets);
// Dissolution inherits the wrapper's geometry; no member settings/storage are copied.
void Dissolve(std::vector<DesktopWidget>& widgets);

// A drag keeps membership intact until the host has a valid placement plan.
// Resolve identities again on every update/commit; indices can change on reload.
class MemberDrag
{
public:
    bool Begin(const std::vector<DesktopWidget>& widgets, size_t owner);
    bool Armed() const noexcept { return !memberId_.empty(); }
    void Reset() { ownerId_.clear(); memberId_.clear(); }
    size_t OwnerIndex(const std::vector<DesktopWidget>& widgets) const;
    size_t MemberIndex(const std::vector<DesktopWidget>& widgets) const;
    bool Detach(std::vector<DesktopWidget>& widgets, GridCell landing) const;

private:
    std::wstring ownerId_;
    std::wstring memberId_;
};

class WheelGesture
{
public:
    static constexpr std::uint64_t QuietMilliseconds = 300;
    // +1 is the next member, -1 the previous; a burst can produce one step.
    int Consume(std::wstring_view stack, int delta, std::uint64_t now,
        bool controlOwnsWheel = false);
    void Hold(std::wstring_view stack, std::uint64_t now);
private:
    std::wstring stack_;
    std::uint64_t lastEvent_ = 0;
    int remainder_ = 0;
    bool seen_ = false;
    bool switched_ = false;
};
}
