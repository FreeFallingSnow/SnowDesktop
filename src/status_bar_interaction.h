#pragma once
#include "status_bar_view.h"

namespace snowdesktop
{
struct StatusBarTarget
{
    std::string key;
    bool tray = false;
    StatusBarAction action = StatusBarAction::None;
    friend bool operator==(const StatusBarTarget&, const StatusBarTarget&) = default;
};
inline StatusBarTarget StatusBarTargetOf(const StatusBarItem& item)
{
    return {item.icon ? item.icon->key : item.key, item.icon.has_value(), item.action};
}
struct StatusBarRelease
{
    bool accepted = false;
    // An accepted release without an item is a blank-area context click.
    std::optional<std::size_t> item;
};
struct StatusBarInteraction
{
    std::optional<std::size_t> hovered;
    std::optional<std::size_t> focused{0};

    // Pointer hover is cleared when targets change; it must not silently move
    // to a replacement under a stationary pointer. Keyboard focus follows its
    // identity, or becomes empty when that target disappears.
    bool Reconcile(const std::vector<StatusBarItem>& before, const std::vector<StatusBarItem>& after)
    {
        if (before.size() == after.size() && std::equal(before.begin(), before.end(), after.begin(),
            [](const auto& a, const auto& b) { return StatusBarTargetOf(a) == StatusBarTargetOf(b); })) return false;
        const bool initial = before.empty() && focused == 0;
        std::optional<StatusBarTarget> previous;
        if (focused && *focused < before.size()) previous = StatusBarTargetOf(before[*focused]);
        focused.reset();
        if (previous)
            for (std::size_t i = 0; i < after.size(); ++i)
                if (StatusBarTargetOf(after[i]) == *previous) { focused = i; break; }
        if (initial && !after.empty()) focused = 0;
        hovered.reset();
        return true;
    }
    void MoveFocus(const std::vector<StatusBarItem>& items, bool backward)
    {
        if (items.empty()) { focused.reset(); return; }
        auto next = focused && *focused < items.size() ?
            (*focused + (backward ? items.size() - 1 : 1)) % items.size() : backward ? items.size() - 1 : 0;
        for (std::size_t count = 0; count < items.size(); ++count)
        {
            if (items[next].action != StatusBarAction::None && !IsRectEmpty(&items[next].bounds))
            { focused = next; return; }
            next = (next + (backward ? items.size() - 1 : 1)) % items.size();
        }
        focused.reset();
    }
    StatusBarAction Press(const std::vector<StatusBarItem>& items, POINT point, bool right)
    {
        const auto hit = HitTestStatusBarItems(items, point);
        // A no-activate bar cannot rely on popup deactivation. Dismiss at
        // button-down, even if capture/leave cancels the eventual release.
        // Do not arm a click that could reopen a surface after dismissal.
        if (!right && !hit)
        {
            CancelPointer();
            return StatusBarAction::Dismiss;
        }
        auto& press = right ? right_ : left_;
        press.active = true;
        press.target = hit ? std::optional{StatusBarTargetOf(items[*hit])} : std::nullopt;
        return StatusBarAction::None;
    }
    StatusBarRelease Release(const std::vector<StatusBarItem>& items, POINT point, bool right)
    {
        auto& press = right ? right_ : left_;
        const auto saved = press;
        press = {};
        if (!saved.active) return {};
        const auto hit = HitTestStatusBarItems(items, point);
        const auto target = hit ? std::optional{StatusBarTargetOf(items[*hit])} : std::nullopt;
        const bool accepted = saved.target == target;
        if (!right) lastLeft_ = accepted ? target : std::nullopt;
        return accepted ? StatusBarRelease{true, hit} : StatusBarRelease{};
    }
    bool IsDoubleClickTarget(const std::vector<StatusBarItem>& items, POINT point) const
    {
        const auto hit = HitTestStatusBarItems(items, point);
        return hit && lastLeft_ && *lastLeft_ == StatusBarTargetOf(items[*hit]);
    }
    void CancelPointer() { hovered.reset(); left_ = {}; right_ = {}; lastLeft_.reset(); }
private:
    struct Pressed { bool active = false; std::optional<StatusBarTarget> target; } left_, right_;
    std::optional<StatusBarTarget> lastLeft_;
};

struct StatusBarInvocation
{
    StatusBarAction action = StatusBarAction::None;
    bool isTray = false;
    std::string trayKey;
    tray::Activation trayAction = tray::Activation::Keyboard;
    RECT bounds{};
};
inline std::optional<std::size_t> FindStatusBarTrayFocus(
    const std::vector<StatusBarItem>& items, const std::string& key)
{
    std::optional<std::size_t> overflow;
    for (std::size_t i = 0; i < items.size(); ++i)
    {
        const auto& item = items[i];
        if (IsRectEmpty(&item.bounds)) continue;
        if (item.icon && item.icon->key == key) return i;
        if (!item.icon && item.action == StatusBarAction::Tray) overflow = i;
    }
    return overflow;
}
inline std::optional<StatusBarInvocation> ResolveStatusBarInvocation(
    const std::vector<StatusBarItem>& items, std::optional<std::size_t> index, bool context)
{
    if (!index || *index >= items.size()) return {};
    const auto& item = items[*index];
    if (item.action == StatusBarAction::None || IsRectEmpty(&item.bounds)) return {};
    return StatusBarInvocation{context ? StatusBarAction::Menu : item.action,
        item.icon.has_value(), item.icon ? item.icon->key : std::string{},
        context ? tray::Activation::ContextKeyboard : tray::Activation::Keyboard, item.bounds};
}

// Shared native message routing. Leave unhandled keys to Windows: DefWindowProc
// generates Shift+F10 context requests; Apps requests come from keyboard input.
// Callbacks are also used by an isolated HWND test, without a desktop AppBar.
template<class ShowFocus, class Invoke, class Dismiss>
bool DispatchStatusBarKeyboard(UINT message, WPARAM key, LPARAM bits, bool shift,
    StatusBarInteraction& state, const std::vector<StatusBarItem>& items,
    ShowFocus&& showFocus, Invoke&& invoke, Dismiss&& dismiss)
{
    if (message == WM_CONTEXTMENU)
    {
        if (static_cast<DWORD>(bits) != 0xffffffffu) return false;
        showFocus();
        invoke(true);
        return true;
    }
    if (message != WM_KEYDOWN && message != WM_SYSKEYDOWN) return false;
    const bool repeat = (bits & (LPARAM{1} << 30)) != 0;
    if (message == WM_SYSKEYDOWN && (key != VK_F10 || (bits & (LPARAM{1} << 29)))) return false;
    if (key == VK_F10) return shift && repeat;
    if (message != WM_KEYDOWN) return false;
    if (key == VK_RETURN || key == VK_SPACE)
    {
        if (!repeat) { showFocus(); invoke(false); }
        return true;
    }
    if (key == VK_ESCAPE)
    {
        if (!repeat) dismiss();
        return true;
    }
    if (key == VK_RIGHT || key == VK_DOWN || key == VK_TAB || key == VK_LEFT || key == VK_UP)
    {
        state.MoveFocus(items, key == VK_LEFT || key == VK_UP || (key == VK_TAB && shift));
        showFocus();
        return true;
    }
    return false;
}
}
