#pragma once
#include "status_bar_view.h"

namespace snowdesktop
{
struct StatusBarTrayDrop
{
    bool pinned = true;
    std::string before;
    RECT indicator{};
};
// Resolve against rendered hit rectangles. Pinned icons share the "tray"
// presentation key with overflow; only the icon-less button means unpin.
inline std::optional<StatusBarTrayDrop> ResolveStatusBarTrayDrop(
    const std::vector<StatusBarItem>& items, POINT point, LONG inset, LONG stroke, RECT viewport = {})
{
    StatusBarTrayDrop result;
    const auto overflow=std::find_if(items.begin(),items.end(),[&](const auto& item) {
        return !item.icon&&item.key=="tray"&&PtInRect(&item.bounds,point);
    });
    if (!IsRectEmpty(&viewport) && !PtInRect(&viewport, point) && overflow == items.end()) return {};
    for(const auto& item:items) if(item.icon &&
        (!IsRectEmpty(&item.bounds) || (!IsRectEmpty(&viewport) && !IsRectEmpty(&item.contentBounds))))
    {
        const auto bounds = IsRectEmpty(&item.contentBounds) ? item.bounds : item.contentBounds;
        result.indicator={bounds.right-stroke/2,bounds.top+inset,bounds.right+stroke-stroke/2,bounds.bottom-inset};
        if(point.x<(bounds.left+bounds.right)/2)
        {result.before=item.icon->key;result.indicator.left=bounds.left-stroke/2;result.indicator.right=result.indicator.left+stroke;break;}
    }
    if (!IsRectEmpty(&viewport) && !IsRectEmpty(&result.indicator))
    {
        result.indicator.left = std::clamp(result.indicator.left, viewport.left, std::max(viewport.left, viewport.right - stroke));
        result.indicator.right = std::min(viewport.right, result.indicator.left + stroke);
    }
    if(overflow!=items.end()) {result.pinned=false;result.indicator=overflow->bounds;}
    else if(IsRectEmpty(&result.indicator))
        for(const auto& item:items)if(!item.icon&&(item.key=="tray"||item.key=="controlCenter")&&!IsRectEmpty(&item.bounds))
        {result.indicator={item.bounds.left-stroke,item.bounds.top+inset,item.bounds.left,item.bounds.bottom-inset};break;}
    if(IsRectEmpty(&result.indicator))return {};
    return result;
}
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
            if (items[next].action != StatusBarAction::None && (!IsRectEmpty(&items[next].bounds) ||
                (items[next].icon && !IsRectEmpty(&items[next].contentBounds))))
            { focused = next; return; }
            next = (next + (backward ? items.size() - 1 : 1)) % items.size();
        }
        focused.reset();
    }
    // Show desktop does not open a popup. A matching pointer hover can retain
    // its tooltip through both button-down and invocation, without a new delay.
    void PrepareActionHover(const std::vector<StatusBarItem>& items, std::optional<std::size_t> index,
        bool context, const std::function<void()>& clearHover) const
    {
        const bool retain = !context && index && hovered == index && *index < items.size() &&
            !items[*index].icon && items[*index].action == StatusBarAction::ShowDesktop;
        if (!retain && clearHover) clearHover();
    }
    StatusBarAction Press(const std::vector<StatusBarItem>& items, POINT point, bool right, bool doubleClick = false,
        const std::function<void()>& pointerPressed = {}, const std::function<void()>& clearHover = {})
    {
        // Notify before any blank-area, double-click or native tray early return.
        // These no-activate surfaces cannot clear host selection via focus loss.
        if (pointerPressed) pointerPressed();
        const auto hit = HitTestStatusBarItems(items, point);
        PrepareActionHover(items, hit, right, clearHover);
        // A Task View double-click is one opening request. Do not re-arm its
        // second release while Shell may be taking over the foreground.
        if (!right && doubleClick && hit && !items[*hit].icon &&
            items[*hit].action == StatusBarAction::TaskView)
        {
            left_ = {};
            return StatusBarAction::None;
        }
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
    auto action = item.action;
    if (context)
    {
        using Action = StatusBarAction;
        switch (item.action)
        {
        case Action::InputMethod: action = Action::InputMethodMenu; break;
        case Action::InputMethodPanel: case Action::SystemInputMethodPanel: action = Action::MenuInputMethod; break;
        case Action::Calendar: case Action::SystemCalendar: action = Action::MenuTime; break;
        case Action::Notifications: action = Action::MenuNotifications; break;
        case Action::Tray: action = Action::MenuTray; break;
        case Action::Network: action = Action::MenuNetwork; break;
        case Action::Audio: action = Action::MenuAudio; break;
        case Action::Power: action = Action::MenuPower; break;
        case Action::ControlCenter: case Action::SystemControlCenter: action = Action::MenuControlCenter; break;
        case Action::QuickSearch: action = Action::MenuQuickSearch; break;
        case Action::SystemMenu: action = Action::MenuSystem; break;
        case Action::TaskView: action = Action::MenuTaskView; break;
        case Action::Cpu: action = Action::MenuCpu; break;
        case Action::Memory: action = Action::MenuMemory; break;
        case Action::Gpu: action = Action::MenuGpu; break;
        case Action::Traffic: action = Action::MenuTraffic; break;
        default: action = Action::Menu; break;
        }
    }
    return StatusBarInvocation{action,
        item.icon.has_value(), item.icon ? item.icon->key : std::string{},
        context ? tray::Activation::ContextKeyboard : tray::Activation::Keyboard, item.bounds};
}

// Mouse context requests use hit geometry; keyboard requests use focus below.
// Tray icons already receive their native right-button sequence. The IME is a
// built-in control with its own system context surface. Other buttons retain
// their identity so the bar menu can offer their own quick settings.
template<class Invoke>
bool DispatchStatusBarPointerContextMenu(const std::vector<StatusBarItem>& items,
    POINT client, Invoke&& invoke)
{
    const auto hit = HitTestStatusBarItems(items, client);
    if (!hit) return false;
    if (items[*hit].icon) return true;
    invoke(*hit);
    return true;
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

// UI-thread preparation only. Native actions still execute synchronously;
// nested drawing requests are collected until their latest model is prepared.
class StatusBarPresentationPreparation
{
public:
    bool Active() const { return active_; }
    bool Defer() { if (!active_) return false; pending_ = true; return true; }
    void Consume() { pending_ = false; }
    bool Pending() const { return pending_; }
    template<class Update> decltype(auto) Run(Update&& update)
    {
        struct Restore
        {
            bool& value;
            bool previous;
            ~Restore() { value = previous; }
        } restore{active_, active_};
        active_ = true;
        return update();
    }
private:
    bool active_ = false;
    bool pending_ = false;
};

// Snapshot the pixels of interaction feedback, rather than only the logical
// keyboard flag: Windows activation can change whether that flag is visible.
struct StatusBarInteractionVisualState
{
    std::optional<std::size_t> hovered, focused;
    std::optional<RECT> indicator;
};

inline StatusBarInteractionVisualState StatusBarInteractionVisual(
    const StatusBarInteraction& interaction, bool keyboardVisible, bool hasFocus,
    const std::optional<RECT>& indicator)
{
    return {interaction.hovered, keyboardVisible && hasFocus ? interaction.focused : std::nullopt, indicator};
}

inline void AddStatusBarInteractionDamage(std::vector<RECT>& regions, RECT bounds)
{
    if (IsRectEmpty(&bounds)) return;
    for (auto i = regions.begin(); i != regions.end();)
    {
        RECT overlap{};
        if (IntersectRect(&overlap, &bounds, &*i))
        {
            UnionRect(&bounds, &bounds, &*i);
            regions.erase(i);
            i = regions.begin();
        }
        else ++i;
    }
    regions.push_back(bounds);
}
inline std::vector<RECT> StatusBarInteractionDamageRegions(const std::vector<StatusBarItem>& items,
    const StatusBarInteractionVisualState& before, const StatusBarInteractionVisualState& after,
    UINT width, UINT height)
{
    std::vector<RECT> damage;
    const RECT client{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    const auto add = [&](RECT bounds) {
        RECT clipped{};
        if (IntersectRect(&clipped, &bounds, &client)) AddStatusBarInteractionDamage(damage, clipped);
    };
    const auto item = [&](std::optional<std::size_t> index) {
        if (index && *index < items.size() && items[*index].action != StatusBarAction::ShowDesktop)
            add(items[*index].bounds);
    };
    if (before.hovered != after.hovered) { item(before.hovered); item(after.hovered); }
    if (before.focused != after.focused) { item(before.focused); item(after.focused); }
    const bool sameIndicator = before.indicator.has_value() == after.indicator.has_value() &&
        (!before.indicator || EqualRect(&*before.indicator, &*after.indicator));
    if (!sameIndicator)
        for (const auto& indicator : {before.indicator, after.indicator}) if (indicator)
        {
            auto bounds = *indicator;
            InflateRect(&bounds, 2, 2); // Existing rounded marker stroke and AA.
            add(bounds);
        }
    return damage;
}
inline RECT StatusBarInteractionDamage(const std::vector<StatusBarItem>& items,
    const StatusBarInteractionVisualState& before, const StatusBarInteractionVisualState& after,
    UINT width, UINT height)
{
    RECT damage{};
    for (const auto& region : StatusBarInteractionDamageRegions(items, before, after, width, height))
        UnionRect(&damage, &damage, &region);
    return damage;
}

// Preparation owns its new geometry; never overwrite it with the old surface's
// cells. Compare a copy of the retained layout instead. A changed extent,
// viewport, identity or degraded measurement requires the complete frame.
inline bool RetainStatusBarPreparedLayout(const std::vector<StatusBarItem>& items,
    const std::vector<StatusBarItem>& before, const StatusBarTrayScroll& oldScroll,
    const StatusBarTrayScroll& scroll, bool merged, bool reusable)
{
    if (!reusable || !EqualRect(&oldScroll.viewport, &scroll.viewport) ||
        oldScroll.contentWidth != scroll.contentWidth) return false;
    auto retained = items;
    if (!RestoreStatusBarContentLayout(retained, before, merged, reusable)) return false;
    if (oldScroll.offset != scroll.offset &&
        !UpdateStatusBarTrayScrollLayout(retained, oldScroll.offset, scroll, reusable)) return false;
    for (std::size_t i = 0; i < items.size(); ++i)
        if (!EqualRect(&retained[i].bounds, &items[i].bounds) ||
            !EqualRect(&retained[i].contentBounds, &items[i].contentBounds)) return false;
    return true;
}

// Production sequence shared with the offscreen regression. Prepare samples,
// reconciles, lays out and publishes geometry without presenting. As with the
// former void Paint, a drawing failure may retain geometry and does not cancel
// native restoration. Prepare/Present own their error reporting. Present gets
// no native callback, so surface fallback cannot replay restoration/Leave.
// The result records native restoration.
template<class Prepare, class Restore, class ClearHover, class Present>
bool DispatchStatusBarTrayFocusReturn(const std::string& key, bool keyboard,
    std::vector<StatusBarItem>& items, StatusBarTrayScroll& scroll,
    StatusBarInteraction& interaction, bool& keyboardVisible,
    StatusBarPresentationPreparation& preparation, Prepare&& prepare,
    Restore&& restore, ClearHover&& clearHover, Present&& present)
{
    const auto refresh = [&] {
        // Consume only synchronous requests already made by preparation,
        // matching the old nested Paint/Replay boundary. No messages are
        // pumped and no native action is repeated by this observation loop.
        do { preparation.Consume(); prepare(); } while (preparation.Pending());
    };
    bool restored = false;
    preparation.Run([&] {
        refresh();
        bool revealed = false;
        for (const auto& item : items)
            if (item.icon && item.icon->key == key)
            {
                revealed = scroll.Reveal(item.contentBounds);
                break;
            }
        if (revealed) refresh();
        const auto index = FindStatusBarTrayFocus(items, key);
        if (!index) return;
        const auto target = StatusBarTargetOf(items[*index]);
        const bool granted = restore();
        // WM_ACTIVATE used to sample/arrange inside RestoreFocus. Keep that
        // observation and its fallback recovery opportunity before the final
        // focus/Leave sample, while collecting its pixels for one presentation.
        if (preparation.Pending()) refresh();
        if (!granted) return;
        restored = true;
        interaction.focused.reset();
        for (std::size_t i = 0; i < items.size(); ++i)
            if (StatusBarTargetOf(items[i]) == target) { interaction.focused = i; break; }
        keyboardVisible = keyboard;
        clearHover();
        refresh(); // The old final focus Paint sampled again too.
    });
    // Even a denied restoration can synchronously request a content refresh.
    if (preparation.Pending())
        preparation.Run(refresh);
    present();
    return restored;
}
}
