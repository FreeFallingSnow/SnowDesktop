#include "app/app.h"
#include "widgets/lua_stack.h"
#include "widget/runtime/widget_engine.h"
#include "common/utils.h"
#include "common/l10n.h"
#include "ui/input/private_pointer.h"
#include "app/layout/grid_geometry.h"

#include <algorithm>
#include <limits>

const DesktopWidget* DesktopApp::ResolveLuaWidgetContent(const DesktopWidget* widget) const
{
    if (!widget) return nullptr;
    if (widget->type == DesktopWidgetType::LuaScript) return widget;
    const size_t index = FindWidgetIndexById(widget->id);
    const size_t active = snowdesktop::lua_stack::Active(widgets_, index);
    return active < widgets_.size() ? &widgets_[active] : nullptr;
}

DesktopWidget* DesktopApp::ResolveLuaWidgetContent(DesktopWidget* widget) const
{
    return const_cast<DesktopWidget*>(ResolveLuaWidgetContent(static_cast<const DesktopWidget*>(widget)));
}

const DesktopWidget& DesktopApp::ResolveLuaWidgetFrameOwner(const DesktopWidget& widget) const
{
    const size_t owner = snowdesktop::lua_stack::Owner(widgets_, widget.id);
    return owner < widgets_.size() ? widgets_[owner] : widget;
}

bool DesktopApp::IsLuaDesktopSurface(const DesktopWidget& widget) const
{
    return snowdesktop::lua_stack::IsSurface(widget.type);
}

bool DesktopApp::CanStackLuaWidgets(size_t source, size_t target) const
{
    if (!snowdesktop::lua_stack::CanCombine(widgets_, source, target, false)) return false;
    const auto& destination = widgets_[target];
    const GridPage* page = FindGridPage(gridPages_, destination.gridCell.pageId);
    if (!page || destination.gridCell.column + destination.gridSpan.columns > page->columns ||
        destination.gridCell.row + destination.gridSpan.rows > page->rows) return false;
    for (const size_t index : {source, target})
        for (const auto& id : snowdesktop::lua_stack::Members(widgets_[index]))
        {
            const size_t child = FindWidgetIndexById(id);
            if (child >= widgets_.size()) return false;
            const auto manifest = WidgetEngine::GetWidgetManifest(widgets_[child].packageId);
            // An absent package must not silently acquire an unrestricted size.
            if (!manifest.hasManifest) return false;
            DesktopWidget limits;
            limits.minGridSpan = {std::max(1, manifest.minColumns), std::max(1, manifest.minRows)};
            limits.maxGridSpan = {manifest.maxColumns, manifest.maxRows};
            if (!snowdesktop::lua_stack::Supports(limits, destination.gridSpan)) return false;
        }
    return true;
}

bool DesktopApp::CommitLuaWidgetStack(size_t source, size_t target)
{
    if (!CanStackLuaWidgets(source, target)) return false;
    HideLuaStackIndicator();
    for (auto& widget : widgets_)
        if (widget.type == DesktopWidgetType::LuaScript) ConfigureWidgetGridLimits(widget);
    if (!snowdesktop::lua_stack::Combine(widgets_, source, target, MakeNewWidgetId())) return false;
    for (auto& widget : widgets_)
        if (widget.type == DesktopWidgetType::LuaStack) ConfigureWidgetGridLimits(widget);
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    keyboardNavInsideWidget_ = false;
    keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    LayoutItems();
    RequestLayoutSave();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool DesktopApp::SelectLuaStackMember(std::wstring stackId, std::wstring memberId)
{
    const size_t index = FindWidgetIndexById(stackId);
    if (index >= widgets_.size() || widgets_[index].type != DesktopWidgetType::LuaStack ||
        (luaWidgetPanelRequest_.modal && !luaWidgetPanelRequest_.widgetId.empty()) ||
        widgetAction_ != WidgetAction::None || dragSession_.HasContext()) return false;
    auto& stack = widgets_[index];
    if (std::find(stack.childWidgetIds.begin(), stack.childWidgetIds.end(), memberId) == stack.childWidgetIds.end()) return false;
    if (stack.activeCategoryId == memberId) return true;
    const auto* old = ResolveLuaWidgetContent(&stack);
    const std::wstring oldId = old ? old->id : L"";
    if (widgetEngine_)
    {
        widgetEngine_->BlurHostInput(false);
        widgetEngine_->CancelInteractionPointerPress("desktop");
        if (!oldId.empty()) widgetEngine_->SetWidgetDesktopVisible(oldId, false);
    }
    if (!oldId.empty() && luaWidgetPanelRequest_.widgetId == oldId)
        CloseLuaWidgetPanel(oldId, "host");
    // Close callbacks may rebuild the model: reacquire both stable identities.
    const size_t current = FindWidgetIndexById(stackId);
    if (current >= widgets_.size() || widgets_[current].type != DesktopWidgetType::LuaStack ||
        std::find(widgets_[current].childWidgetIds.begin(), widgets_[current].childWidgetIds.end(), memberId) ==
            widgets_[current].childWidgetIds.end()) return false;
    widgets_[current].activeCategoryId = memberId;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    RebuildContainersAndItems();
    RequestLayoutSave();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool DesktopApp::ScrollLuaWidgetStack(const std::wstring& stackId, int delta)
{
    const std::wstring stableId(stackId);
    const size_t index = FindWidgetIndexById(stableId);
    if (index >= widgets_.size() || widgets_[index].type != DesktopWidgetType::LuaStack) return false;
    if (luaStackIndicator_.stackId != stableId)
    {
        luaStackIndicator_.wheelRemainder = 0;
        luaStackIndicator_.stackId = stackId;
    }
    luaStackIndicator_.wheelRemainder += delta;
    const int steps = luaStackIndicator_.wheelRemainder / WHEEL_DELTA;
    luaStackIndicator_.wheelRemainder %= WHEEL_DELTA;
    if (!steps) return true;
    const auto members = widgets_[index].childWidgetIds;
    if (members.empty()) return true;
    const auto active = std::find(members.begin(), members.end(), widgets_[index].activeCategoryId);
    const auto count = static_cast<std::int64_t>(members.size());
    const auto position = active == members.end() ? 0 : static_cast<std::int64_t>(active - members.begin());
    const size_t next = static_cast<size_t>(((position - steps) % count + count) % count);
    (void)SelectLuaStackMember(stableId, members[next]);
    UpdateLuaStackIndicator(lastMousePoint_);
    return true;
}

bool DesktopApp::ReleaseLuaStackMember(const std::wstring& stackId)
{
    const size_t index = FindWidgetIndexById(stackId);
    if (index >= widgets_.size() || widgets_[index].type != DesktopWidgetType::LuaStack) return false;
    auto* child = ResolveLuaWidgetContent(&widgets_[index]);
    if (!child) return false;
    const std::wstring childId = child->id;
    const GridSpan span = widgets_[index].gridSpan;
    const GridCell preferred = widgets_[index].gridCell;
    std::unordered_set<std::wstring> occupied;
    for (const auto& widget : widgets_)
        if (!IsGroupedWidget(widget)) MarkGridArea(occupied, widget.gridCell, widget.gridSpan);
    for (const auto& item : items_)
        if (!item.name.empty() && !IsItemInAnyWidget(item)) MarkGridArea(occupied, item.gridCell, item.gridSpan);
    GridCell landing;
    const int start = SlotFromCell(gridPages_, preferred);
    if (!TryFindFreeCell(span, occupied, landing, preferred.pageId, start) &&
        !TryFindFreeCell(span, occupied, landing, L"", 0) &&
        !FindDockReturnCell(occupied, preferred.pageId, start, span, landing)) return false;
    const size_t owner = FindWidgetIndexById(stackId);
    const size_t member = FindWidgetIndexById(childId);
    if (owner >= widgets_.size() || member >= widgets_.size()) return false;
    HideLuaStackIndicator();
    std::erase(widgets_[owner].childWidgetIds, childId);
    widgets_[member].gridCell = landing;
    widgets_[member].gridSpan = span;
    widgets_[member].selected = true;
    snowdesktop::lua_stack::Normalize(widgets_);
    snowdesktop::lua_stack::Dissolve(widgets_);
    LayoutItems();
    RequestLayoutSave();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

bool DesktopApp::IsLuaStackIndicatorVisible() const
{
    if (luaStackIndicator_.stackId.empty() || IsRectEmpty(&luaStackIndicator_.bounds) ||
        !customDesktopVisible_ || desktopPassthroughActive_ || HasActiveContextMenuSession() ||
        widgetAction_ != WidgetAction::None || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() || !luaWidgetPanelRequest_.widgetId.empty() || GetOpenPopupWidget()) return false;
    const size_t index = FindWidgetIndexById(luaStackIndicator_.stackId);
    return index < widgets_.size() && widgets_[index].type == DesktopWidgetType::LuaStack &&
        (!desktopIconsHidden_ || widgets_[index].keepWhenDesktopHidden) && !IsRectEmpty(&widgets_[index].bounds);
}

void DesktopApp::HideLuaStackIndicator()
{
    const bool shown = !IsRectEmpty(&luaStackIndicator_.bounds);
    luaStackIndicator_ = {};
    if (shown) UpdateFloatingPopupWindowBounds();
}

void DesktopApp::UpdateLuaStackIndicator(POINT point)
{
    if (widgetAction_ != WidgetAction::None || dragSession_.HasContext() ||
        dragDropController_.IsTransportActive() || !customDesktopVisible_ ||
        desktopPassthroughActive_ || HasActiveContextMenuSession() ||
        !luaWidgetPanelRequest_.widgetId.empty() || GetOpenPopupWidget())
    {
        HideLuaStackIndicator();
        return;
    }
    size_t target = HitTestStandaloneWidgetIndex(point);
    if (target >= widgets_.size() || widgets_[target].type != DesktopWidgetType::LuaStack)
    {
        if (IsLuaStackIndicatorVisible() &&
            (PtInRect(&luaStackIndicator_.bounds, point) || PtInRect(&luaStackIndicator_.bridge, point)))
            target = FindWidgetIndexById(luaStackIndicator_.stackId);
        else
        {
            if (!luaStackIndicator_.hideAfter && !luaStackIndicator_.stackId.empty())
            {
                luaStackIndicator_.hideAfter = GetTickCount64() + 180;
                uiAnimationScheduler_.ScheduleOnce(180, [this](snowdesktop::UiScheduleToken) {
                    if (!luaStackIndicator_.hideAfter || GetTickCount64() < luaStackIndicator_.hideAfter) return;
                    POINT cursor{};
                    if (snowdesktop::private_pointer::CursorPosition(&cursor) && ScreenToClient(hwnd_, &cursor))
                    {
                        if (PtInRect(&luaStackIndicator_.bounds, cursor) || PtInRect(&luaStackIndicator_.bridge, cursor))
                        { luaStackIndicator_.hideAfter = 0; return; }
                        const size_t hovered = HitTestStandaloneWidgetIndex(cursor);
                        if (hovered < widgets_.size() && widgets_[hovered].id == luaStackIndicator_.stackId)
                        { luaStackIndicator_.hideAfter = 0; return; }
                    }
                    HideLuaStackIndicator();
                });
            }
            return;
        }
    }
    if (target >= widgets_.size()) return;
    const auto& stack = widgets_[target];
    const auto& ids = stack.childWidgetIds;
    if (ids.size() < 2) { HideLuaStackIndicator(); return; }
    const bool newTarget = luaStackIndicator_.stackId != stack.id;
    const RECT oldBounds = luaStackIndicator_.bounds;
    if (newTarget) luaStackIndicator_ = {};
    luaStackIndicator_.stackId = stack.id;
    luaStackIndicator_.hideAfter = 0;
    const float scale = GetWidgetCellScale(stack);
    const auto cu = [&](float value) { return std::max(1, ScaleWidgetCu(value, scale)); };
    const RECT frame = GetStandaloneWidgetFrameRect(stack);
    RECT screenFrame = frame;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenFrame), 2);
    MONITORINFO monitor{sizeof(monitor)};
    if (!GetMonitorInfoW(MonitorFromRect(&screenFrame, MONITOR_DEFAULTTONEAREST), &monitor)) return;
    RECT work = monitor.rcWork;
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&work), 2);
    const int pitch = cu(16), padding = cu(8), width = cu(24), gap = cu(6);
    const size_t visible = std::min(ids.size(), static_cast<size_t>(std::max<LONG>(1, (work.bottom - work.top - padding * 2) / pitch)));
    const int height = static_cast<int>(visible) * pitch + padding * 2;
    int left = frame.right + gap;
    if (left + width > work.right) left = frame.left - gap - width;
    left = std::clamp(left, static_cast<int>(work.left), std::max<int>(work.left, work.right - width));
    const int top = std::clamp<int>((frame.top + frame.bottom - height) / 2, work.top, std::max<int>(work.top, work.bottom - height));
    luaStackIndicator_.bounds = {left, top, left + width, top + height};
    luaStackIndicator_.bridge = {std::min(frame.right, static_cast<LONG>(left + width)), std::max(frame.top, static_cast<LONG>(top)),
        std::max(frame.left, static_cast<LONG>(left)), std::min(frame.bottom, static_cast<LONG>(top + height))};
    luaStackIndicator_.pitch = pitch;
    luaStackIndicator_.padding = padding;
    luaStackIndicator_.visibleCount = visible;
    const auto active = std::find(ids.begin(), ids.end(), stack.activeCategoryId);
    const size_t activeIndex = active == ids.end() ? 0 : static_cast<size_t>(active - ids.begin());
    luaStackIndicator_.first = std::min(activeIndex > visible / 2 ? activeIndex - visible / 2 : 0, ids.size() - visible);
    if (newTarget || !EqualRect(&oldBounds, &luaStackIndicator_.bounds)) UpdateFloatingPopupWindowBounds(true);
    else InvalidateFloatingPopupWindow(false);
}

bool DesktopApp::HandleLuaStackIndicatorPointer(POINT point, bool pressed)
{
    if (!IsLuaStackIndicatorVisible() || !PtInRect(&luaStackIndicator_.bounds, point))
    {
        if (!pressed) luaStackIndicator_.pressedMember.clear();
        return false;
    }
    const size_t index = FindWidgetIndexById(luaStackIndicator_.stackId);
    const int row = (point.y - luaStackIndicator_.bounds.top - luaStackIndicator_.padding) / luaStackIndicator_.pitch;
    const size_t member = luaStackIndicator_.first + static_cast<size_t>(std::max(0, row));
    if (point.y < luaStackIndicator_.bounds.top + luaStackIndicator_.padding || row < 0 ||
        static_cast<size_t>(row) >= luaStackIndicator_.visibleCount || member >= widgets_[index].childWidgetIds.size())
    {
        if (!pressed)
        {
            luaStackIndicator_.pressedMember.clear();
            snowdesktop::private_pointer::ReleasePointerCapture();
        }
        return true;
    }
    const std::wstring id = widgets_[index].childWidgetIds[member];
    if (pressed)
    {
        luaStackIndicator_.pressedMember = id;
        snowdesktop::private_pointer::SetPointerCapture(floatingPopupHwnd_);
    }
    else
    {
        const bool activate = luaStackIndicator_.pressedMember == id;
        luaStackIndicator_.pressedMember.clear();
        snowdesktop::private_pointer::ReleasePointerCapture();
        luaStackIndicator_.wheelRemainder = 0;
        if (activate) (void)SelectLuaStackMember(luaStackIndicator_.stackId, id);
        UpdateLuaStackIndicator(point);
    }
    return true;
}

void DesktopApp::DrawLuaStackIndicator(ID2D1DeviceContext* context)
{
    if (!context || !IsLuaStackIndicatorVisible()) return;
    const size_t index = FindWidgetIndexById(luaStackIndicator_.stackId);
    const auto& stack = widgets_[index];
    const float scale = GetWidgetCellScale(stack);
    auto appearance = CurrentPersonalization();
    appearance.widgetEdgeHighlightEnabled = false;
    HIGHCONTRASTW contrast{sizeof(contrast)};
    const bool highContrast = SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(contrast), &contrast, 0) &&
        (contrast.dwFlags & HCF_HIGHCONTRASTON);
    const auto systemColor = [](int index) {
        const COLORREF color = GetSysColor(index);
        return D2D1::ColorF(GetRValue(color) / 255.0f, GetGValue(color) / 255.0f, GetBValue(color) / 255.0f);
    };
    const float corner = static_cast<float>(ScaleWidgetCu(12.0f, scale));
    if (highContrast)
        DrawD2DRoundedRectangle(context, luaStackIndicator_.bounds, corner,
            systemColor(COLOR_WINDOW), systemColor(COLOR_WINDOWTEXT), std::max(1.0f, scale));
    else
        DrawWidgetPanelBackground(context, luaStackIndicator_.bounds, corner,
            D2D1::ColorF(appearance.widgetBgR, appearance.widgetBgG, appearance.widgetBgB, appearance.widgetAlpha),
            D2D1::ColorF(appearance.widgetBorderR, appearance.widgetBorderG, appearance.widgetBorderB, appearance.widgetBorderAlpha),
            false, scale, &appearance, false, 0, scale);
    ComPtr<ID2D1SolidColorBrush> brush;
    const bool light = IsLightContentTheme();
    const D2D1_COLOR_F foreground = highContrast ? systemColor(COLOR_WINDOWTEXT) :
        (light ? D2D1::ColorF(0, 0, 0, 0.8f) : D2D1::ColorF(1, 1, 1, 0.9f));
    if (FAILED(context->CreateSolidColorBrush(foreground, &brush))) return;
    const float radius = static_cast<float>(ScaleWidgetCu(3.0f, scale));
    for (size_t row = 0; row < luaStackIndicator_.visibleCount; ++row)
    {
        const auto center = D2D1::Point2F((luaStackIndicator_.bounds.left + luaStackIndicator_.bounds.right) * 0.5f,
            static_cast<float>(luaStackIndicator_.bounds.top + luaStackIndicator_.padding) +
                (static_cast<float>(row) + 0.5f) * luaStackIndicator_.pitch);
        const auto dot = D2D1::Ellipse(center, radius, radius);
        if (stack.childWidgetIds[luaStackIndicator_.first + row] == stack.activeCategoryId)
        { brush->SetOpacity(1.0f); context->FillEllipse(dot, brush.Get()); }
        else
        { brush->SetOpacity(highContrast ? 1.0f : 0.55f); context->DrawEllipse(dot, brush.Get(), std::max(1.0f, scale)); }
    }
}
