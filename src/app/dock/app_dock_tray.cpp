#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "dock/dock_tray_targets.h"
#include "app/navigation/quick_navigation_theme.h"

namespace
{
HBITMAP TrayBitmap(const snowdesktop::tray::Icon& icon)
{
    if (!icon.width || !icon.height || icon.width > 64 || icon.height > 64 ||
        icon.pixels.size() != static_cast<size_t>(icon.width) * icon.height) return nullptr;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(icon.width);
    info.bmiHeader.biHeight = -static_cast<LONG>(icon.height);
    info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    const auto bitmap = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (!bitmap || !pixels) { if (bitmap) DeleteObject(bitmap); return nullptr; }
    std::copy(icon.pixels.begin(), icon.pixels.end(), static_cast<std::uint32_t*>(pixels));
    return bitmap;
}
}

std::shared_ptr<snowdesktop::tray::Service> DesktopApp::EnsureDockTrayService()
{
    if (!dockTrayService_ && statusBar_) dockTrayService_ = statusBar_->Tray();
    if (!dockTrayService_) dockTrayService_ = std::make_shared<snowdesktop::tray::Service>();
    if (statusBar_) statusBar_->SetSharedTray(dockTrayService_);
    return dockTrayService_;
}

size_t DesktopApp::DockTrayTargetCount(size_t itemIndex)
{
    if (!dockTrayService_ || itemIndex >= items_.size() ||
        GetDockWindowVisualState(itemIndex) != DockWindowVisualState::Closed) return 0;
    const auto identity = ResolveDockAppIdentity(itemIndex);
    return snowdesktop::dock_tray::CollectTargets(dockTrayService_->Current(),
        dockSettings_.trayApplications, identity.executablePath, identity.validatedPayloadPaths).size();
}

bool DesktopApp::ActivateDockTrayTarget(const snowdesktop::tray::Icon& target,
    const std::wstring& executable, size_t itemIndex, RECT anchor, bool context)
{
    namespace rules = snowdesktop::dock_tray;
    if (exitRequested_ || !generalSettings_.dockEnabled || !dockTrayService_ ||
        !rules::IsCurrentTarget(dockTrayService_->Current(), target)) return false;
    const auto identity = itemIndex < items_.size() ? ResolveDockAppIdentity(itemIndex) : DockAppIdentity{};
    const auto* current = rules::TargetRule(dockSettings_.trayApplications, executable, target.application, identity.validatedPayloadPaths);
    if (!current || !current->enabled) return false;
    const POINT point{(anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2};
    const auto service = dockTrayService_;
    if (context)
    {
        const HWND owner = ShellLaunchOwnerHwnd();
        return service->Activate(target.key, snowdesktop::tray::Activation::RightDown, point, {owner, owner}, anchor, target.identity) &&
            service->Activate(target.key, snowdesktop::tray::Activation::RightUp, point, {owner, owner}, anchor, target.identity);
    }
    if (itemIndex < items_.size()) StartDockLaunchBounce(itemIndex);
    const bool accepted = service->ActivatePrimary(target.key, current->doubleClick, point, {}, anchor, target.identity);
    if (accepted && itemIndex < items_.size()) RecordDockItemUsage(itemIndex);
    return accepted;
}

bool DesktopApp::TryActivateDockTray(const std::wstring& executable, size_t itemIndex,
    std::optional<RECT> capturedAnchor)
{
    namespace rules = snowdesktop::dock_tray;
    const auto* rule = rules::FindRule(dockSettings_.trayApplications, executable);
    if (!snowdesktop::runtime_modules::Allowed(snowdesktop::runtime_modules::Module::Tray) || !generalSettings_.dockEnabled || !rule || !rule->enabled || exitRequested_) return false;
    // Existing task windows retain normal Dock activation/restoration/minimize.
    if (itemIndex < items_.size() && GetDockWindowVisualState(itemIndex) != DockWindowVisualState::Closed)
    { if (dockTrayPopup_) CloseCollectionPopup(false); return false; }
    DockAppIdentity identity = itemIndex < items_.size() ? ResolveDockAppIdentity(itemIndex) : DockAppIdentity{};
    identity.kind = DockAppIdentityKind::Executable; identity.executablePath = executable;
    if (IsDockAppClosePending(identity)) return true;
    const auto service = EnsureDockTrayService();
    const auto targets = rules::CollectTargets(service->Current(), dockSettings_.trayApplications, executable, identity.validatedPayloadPaths);
    if (targets.empty()) return false;
    DismissDockWindowPreviewUntilLeave();
    DockContainer* dock = GetDockContainerAtPoint(lastMousePoint_);
    RECT anchor = capturedAnchor.value_or(RECT{});
    if (IsRectEmpty(&anchor) && dock)
    {
        RECT bounds{};
        if (auto* entry = dock->EntryAtPoint(lastMousePoint_)) bounds = entry->GetBounds();
        else if (auto* frequent = dock->FrequentItemAtPoint(lastMousePoint_)) bounds = frequent->GetBounds();
        else if (auto* running = dock->RunningItemAtPoint(lastMousePoint_)) bounds = running->GetBounds();
        if (!IsRectEmpty(&bounds))
        {
            anchor = dock->GetElementVisualRect(bounds, lastMousePoint_);
            MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&anchor), 2);
        }
    }
    if (IsRectEmpty(&anchor))
    {
        POINT cursor{}; snowdesktop::private_pointer::CursorPosition(&cursor);
        anchor = {cursor.x, cursor.y, cursor.x + 1, cursor.y + 1};
    }
    if (targets.size() == 1)
    { if (dockTrayPopup_) CloseCollectionPopup(false); ActivateDockTrayTarget(targets.front(), executable, itemIndex, anchor); return true; }
    if (dockTrayPopup_ && dockTrayPopup_->executable == executable && IsCollectionPopupInteractive())
    { CloseCollectionPopup(false); return true; }
    CancelPopupHover(true);
    pendingCollectionPopupOpen_.reset();
    DismissActiveContextMenuForPopupTransition();
    PreserveDockFolderPopupDragSourceForTransition();
    FinalizeCloseCollectionPopup();
    AdvanceFloatingPopupContentGeneration();
    auto state = std::make_shared<DockTrayPopupState>();
    ClearSelection();
    state->executable = executable;
    state->itemKey = itemIndex < items_.size() ? DockItemWindowKey(items_[itemIndex]) : L"";
    state->launchTarget = itemIndex < items_.size() ? items_[itemIndex].parsingName : executable;
    state->anchorScreen = anchor;
    state->widget.type = DesktopWidgetType::Collection;
    state->widget.id = L"__dock_tray_popup"; state->widget.listMode = true;
    state->widget.title = itemIndex < items_.size() ? items_[itemIndex].name : rules::ExecutableName(executable);
    dockTrayPopup_ = state;
    popupWidgetIndex_ = static_cast<size_t>(-1); popupScrollOffset_ = 0;
    popupHasAnchor_ = popupAnchoredToDock_ = true;
    popupDockPosition_ = dockSettings_.position;
    collectionPopupDockHost_ = FindPersistentDockHost(dock);
    RECT desktopAnchor = anchor;
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&desktopAnchor), 2);
    switch (popupDockPosition_)
    {
    case DockPosition::Top: popupAnchorPoint_ = {(desktopAnchor.left + desktopAnchor.right) / 2, desktopAnchor.bottom}; break;
    case DockPosition::Left: popupAnchorPoint_ = {desktopAnchor.right, (desktopAnchor.top + desktopAnchor.bottom) / 2}; break;
    case DockPosition::Right: popupAnchorPoint_ = {desktopAnchor.left, (desktopAnchor.top + desktopAnchor.bottom) / 2}; break;
    default: popupAnchorPoint_ = {(desktopAnchor.left + desktopAnchor.right) / 2, desktopAnchor.top}; break;
    }
    popupPageId_.clear(); popupCategoryId_.clear();
    for (const auto& page : gridPages_)
        if (PtInRect(&page.bounds, popupAnchorPoint_)) { popupPageId_ = page.id; break; }
    EnsureFloatingDockVisibleForAssociatedSurface({(anchor.left + anchor.right) / 2, (anchor.top + anchor.bottom) / 2});
    service->SubscribeChanges(hwnd_);
    service->RefreshNativeTooltips();
    RefreshDockTrayPopup();
    StartCollectionPopupAnimation();
    UpdateFloatingPopupWindowBounds(true);
    InvalidateCollectionPopupAnimation(true);
    return true;
}

void DesktopApp::ResetDockTrayPopup()
{
    if (!dockTrayPopup_) return;
    if (dockTrayService_) dockTrayService_->UnsubscribeChanges(hwnd_);
    for (auto bitmap : dockTrayPopup_->images) EraseD2DIconCacheForBitmap(bitmap);
    dockTrayPopup_.reset();
}

void DesktopApp::RefreshDockTrayPopup()
{
    if (!dockTrayPopup_ || !dockTrayService_ || popupAnimation_.IsClosing()) return;
    const auto keepAlive = dockTrayPopup_;
    auto& state = *keepAlive;
    const auto snapshot = dockTrayService_->Current();
    const auto focused = state.focused >= 0 && static_cast<size_t>(state.focused) < state.targets.size()
        ? std::optional<snowdesktop::tray::Icon>(state.targets[state.focused]) : std::nullopt;
    const bool launchFocused = state.focused >= 0 && static_cast<size_t>(state.focused) == state.targets.size();
    for (auto bitmap : state.images) { EraseD2DIconCacheForBitmap(bitmap); if (bitmap) DeleteObject(bitmap); }
    state.images.clear(); state.rowHeights.clear();
    const auto identity = ResolveDockAppIdentity(FindItemIndexByKey(state.itemKey));
    state.targets = snowdesktop::dock_tray::CollectTargets(snapshot, dockSettings_.trayApplications, state.executable, identity.validatedPayloadPaths);
    const auto metrics = GetCollectionPopupLayoutMetrics(state.widget);
    const float scale = metrics.scale;
    const RECT geometry = GetCollectionPopupRect(state.widget);
    state.footerHeight = static_cast<int>(std::ceil(52 * scale));
    state.rowGap = static_cast<int>(std::ceil(8 * scale));
    const float textWidth = std::max(1.0f, geometry.right - geometry.left - metrics.paddingX * 2.0f - 74.0f * scale);
    auto* format = GetCollectionPopupTextFormat(scale, false);
    for (const auto& icon : state.targets)
    {
        state.images.push_back(TrayBitmap(icon));
        float textHeight = 0;
        ComPtr<IDWriteTextLayout> layout;
        const auto label = snowdesktop::dock_tray::popup::Label(icon.tip, state.widget.title);
        if (dwriteFactory_ && format && SUCCEEDED(dwriteFactory_->CreateTextLayout(label.data(),
                static_cast<UINT32>(label.size()), format, textWidth, 4096 * scale, &layout)))
        {
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            DWRITE_TEXT_METRICS measured{};
            if (SUCCEEDED(layout->GetMetrics(&measured))) textHeight = measured.height;
        }
        state.rowHeights.push_back(static_cast<int>(std::ceil(std::max(64 * scale, textHeight + 28 * scale))) + state.rowGap);
    }
    state.widget.itemKeys.resize(state.targets.size() + 1);
    state.focused = launchFocused ? static_cast<int>(state.targets.size()) : -1;
    if (focused)
        for (size_t row = 0; row < state.targets.size(); ++row)
            if (state.targets[row].key == focused->key && snowdesktop::tray::SameFocusIdentity(state.targets[row].identity, focused->identity))
                state.focused = static_cast<int>(row);
    const RECT popup = GetCollectionPopupRect(state.widget);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0, GetCollectionPopupMaxScrollOffset(state.widget, popup));
    POINT pointer{};
    if (snowdesktop::private_pointer::CursorPosition(&pointer) && ScreenToClient(hwnd_, &pointer))
        state.hovered = DockTrayPopupHitTest(pointer);
    else state.hovered = -1;
    InvalidateCollectionPopupContent();
    if (popupAnimation_.IsInteractive())
    {
        UpdateFloatingPopupWindowBounds(true);
        InvalidateCollectionPopupAnimation(true);
    }
}

int DesktopApp::DockTrayPopupHitTest(POINT point) const
{
    if (!dockTrayPopup_ || !IsCollectionPopupInteractive()) return -1;
    const RECT popup = GetCollectionPopupRect(dockTrayPopup_->widget);
    const RECT content = GetCollectionPopupContentRect(popup);
    const auto& state = *dockTrayPopup_;
    const RECT launch = GetCollectionPopupItemRect(popup, state.targets.size());
    return snowdesktop::dock_tray::popup::HitTest(point, content, launch,
        state.rowHeights, popupScrollOffset_, state.rowGap);
}

bool DesktopApp::DockTrayPopupPointerDown(POINT point, bool context)
{
    if (!dockTrayPopup_ || !IsPointOccludedByOpenPopup(point)) return false;
    auto& state = *dockTrayPopup_;
    state.keyboardNavigation = false;
    state.pointerDown = true; state.context = context;
    state.pressedTarget.reset(); state.pressedLaunch = false;
    const int row = DockTrayPopupHitTest(point);
    state.hovered = row; state.pointer = point;
    if (row >= 0 && static_cast<size_t>(row) < state.targets.size()) state.pressedTarget = state.targets[row];
    else if (row >= 0 && !context) state.pressedLaunch = true;
    InvalidateCollectionPopupContent();
    InvalidateFloatingPopupWindow(false);
    return true;
}

bool DesktopApp::DockTrayPopupPointerUp(POINT point)
{
    if (!dockTrayPopup_ || !dockTrayPopup_->pointerDown) return false;
    const auto state = dockTrayPopup_;
    state->pointerDown = false;
    mouseDown_ = false; mouseDownHit_ = nullptr;
    snowdesktop::private_pointer::ReleasePointerCapture();
    const int row = DockTrayPopupHitTest(point);
    if (row >= 0)
    {
        const auto index = static_cast<size_t>(row);
        const bool launch = state->pressedLaunch && index == state->targets.size();
        const bool target = index < state->targets.size() && state->pressedTarget &&
            state->targets[index].key == state->pressedTarget->key &&
            snowdesktop::tray::SameFocusIdentity(state->targets[index].identity, state->pressedTarget->identity);
        if (launch || target) ActivateDockTrayPopupRow(index, state->context);
    }
    state->pressedTarget.reset(); state->pressedLaunch = false;
    if (dockTrayPopup_ == state)
    { InvalidateCollectionPopupContent(); InvalidateFloatingPopupWindow(false); }
    return true;
}

bool DesktopApp::DockTrayPopupPointerMove(POINT point)
{
    if (!dockTrayPopup_) return false;
    const auto state = dockTrayPopup_;
    bool presentationChanged = false;
    if (state->pointer.x != point.x || state->pointer.y != point.y)
    {
        state->pointer = point;
        presentationChanged = state->keyboardNavigation;
        state->keyboardNavigation = false;
    }
    const int row = DockTrayPopupHitTest(point);
    if (state->hovered != row || presentationChanged)
    {
        state->hovered = row;
        InvalidateCollectionPopupContent();
        InvalidateFloatingPopupWindow(false);
    }
    if (!IsPointOccludedByOpenPopup(point) && !state->pointerDown) return false;
    lastMousePoint_ = point;
    return true;
}

void DesktopApp::ActivateDockTrayPopupRow(size_t row, bool context)
{
    const auto state = dockTrayPopup_;
    if (!state || !IsCollectionPopupInteractive() || row > state->targets.size()) return;
    const size_t itemIndex = FindItemIndexByKey(state->itemKey);
    RECT invocation = state->anchorScreen;
    if (context)
    {
        const RECT popup = GetCollectionPopupRect(state->widget);
        const RECT bounds = GetCollectionPopupItemRect(popup, row);
        const RECT viewport = GetCollectionPopupContentRect(popup);
        IntersectRect(&invocation, &bounds, &viewport);
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&invocation), 2);
    }
    CloseCollectionPopup(false);
    if (row == state->targets.size())
    {
        if (context) return;
        if (itemIndex < items_.size() && items_[itemIndex].parsingName == state->launchTarget)
            LaunchDesktopItem(itemIndex, true, false);
        else if (state->itemKey.empty()) LaunchPathWithShortcutPolicy(ShellLaunchOwnerHwnd(), state->launchTarget);
        return;
    }
    if (!context && itemIndex < items_.size() && GetDockWindowVisualState(itemIndex) != DockWindowVisualState::Closed)
    { ActivateOrToggleDockItem(itemIndex); return; }
    ActivateDockTrayTarget(state->targets[row], state->executable, itemIndex, invocation, context);
}

bool DesktopApp::DockTrayPopupKeyDown(WPARAM key)
{
    if (!dockTrayPopup_ || !IsCollectionPopupInteractive()) return false;
    auto& state = *dockTrayPopup_;
    const int count = static_cast<int>(state.targets.size()) + 1;
    if (key == VK_ESCAPE) { CloseCollectionPopup(false); return true; }
    if (key == VK_RETURN) { if (state.focused >= 0) ActivateDockTrayPopupRow(state.focused); return true; }
    if ((key == VK_APPS || (key == VK_F10 && (snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000))) &&
        state.focused >= 0) { ActivateDockTrayPopupRow(state.focused, true); return true; }
    if (key != VK_UP && key != VK_DOWN && key != VK_HOME && key != VK_END && key != VK_TAB)
        return key == VK_DELETE || key == VK_F2 || key == VK_BACK;
    state.keyboardNavigation = true;
    if (key == VK_HOME) state.focused = 0;
    else if (key == VK_END) state.focused = count - 1;
    else
    {
        const int direction = key == VK_UP || (key == VK_TAB && (snowdesktop::private_pointer::KeyState(VK_SHIFT) & 0x8000)) ? -1 : 1;
        state.focused = state.focused < 0 ? 0 : (state.focused + direction + count) % count;
    }
    const RECT popup = GetCollectionPopupRect(state.widget);
    const RECT content = GetCollectionPopupContentRect(popup);
    const RECT row = GetCollectionPopupItemRect(popup, state.focused);
    if (state.focused < static_cast<int>(state.targets.size()))
    {
        if (row.top < content.top || row.bottom - row.top > content.bottom - content.top)
            popupScrollOffset_ = std::max(0, popupScrollOffset_ + static_cast<int>(row.top - content.top));
        else if (row.bottom > content.bottom) popupScrollOffset_ += static_cast<int>(row.bottom - content.bottom);
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0, GetCollectionPopupMaxScrollOffset(state.widget, popup));
    }
    InvalidateCollectionPopupContent();
    InvalidateFloatingPopupWindow(false);
    return true;
}

void DesktopApp::DrawDockTrayPopup(ID2D1DeviceContext* context, const RECT& popup)
{
    if (!dockTrayPopup_) return;
    const auto& state = *dockTrayPopup_;
    const float scale = GetOpenCollectionPopupLayoutMetrics().scale;
    const auto color = (collectionPopupLightTheme_ ? kQuickNavLight : kQuickNavDark).popupTitle;
    const RECT content = GetCollectionPopupContentRect(popup);
    context->PushAxisAlignedClip(D2D1::RectF(static_cast<float>(content.left), static_cast<float>(content.top),
        static_cast<float>(content.right), static_cast<float>(content.bottom)), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t index = 0; index < state.targets.size(); ++index)
    {
        const RECT row = GetCollectionPopupItemRect(popup, index);
        if (row.bottom <= content.top || row.top >= content.bottom) continue;
        const bool hovered = !state.keyboardNavigation && state.hovered == static_cast<int>(index);
        const bool focused = state.keyboardNavigation && state.focused == static_cast<int>(index);
        const bool pressed = state.pointerDown && hovered && state.pressedTarget &&
            state.targets[index].key == state.pressedTarget->key &&
            snowdesktop::tray::SameFocusIdentity(state.targets[index].identity, state.pressedTarget->identity);
        const float shade = pressed ? 0.16f : hovered || focused ? 0.10f : 0.035f;
        const auto fill = collectionPopupLightTheme_ ? D2D1::ColorF(0, 0, 0, shade) : D2D1::ColorF(1, 1, 1, shade);
        auto border = color; border.a = focused ? 0.4f : 0.08f;
        DrawD2DRoundedRectangle(context, row, 9 * scale, fill, border, scale);
        RECT text = row;
        const int padding = static_cast<int>(14 * scale);
        text.left += padding; text.right -= padding;
        text.top += padding; text.bottom -= padding;
        const int iconSize = static_cast<int>(32 * scale);
        const RECT icon{text.left, row.top + padding, text.left + iconSize, row.top + padding + iconSize};
        if (auto* bitmap = GetOrCreateD2DBitmap(state.images[index], false)) DrawIconBitmap(context, bitmap, icon);
        text.left = icon.right + padding;
        DrawD2DText(context, std::wstring(snowdesktop::dock_tray::popup::Label(state.targets[index].tip, state.widget.title)),
            text, GetCollectionPopupTextFormat(scale, false), color, DWRITE_WORD_WRAPPING_WRAP);
    }
    context->PopAxisAlignedClip();
    const auto launchIndex = static_cast<int>(state.targets.size());
    const RECT launch = GetCollectionPopupItemRect(popup, state.targets.size());
    const bool launchFocused = state.keyboardNavigation && state.focused == launchIndex;
    const bool launchHovered = !state.keyboardNavigation && state.hovered == launchIndex;
    const float shade = state.pointerDown && state.pressedLaunch && launchHovered ? 0.18f :
        launchHovered || launchFocused ? 0.13f : 0.065f;
    auto border = color; border.a = launchFocused ? 0.4f : 0.16f;
    DrawD2DRoundedRectangle(context, launch, 8 * scale,
        collectionPopupLightTheme_ ? D2D1::ColorF(0, 0, 0, shade) : D2D1::ColorF(1, 1, 1, shade), border, scale);
    if (auto* format = GetCollectionPopupTextFormat(scale, false))
    {
        const auto alignment = format->GetTextAlignment();
        format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        DrawD2DText(context, _LW("settings.dock.trayLaunch"), launch, format, color);
        format->SetTextAlignment(alignment);
    }
}
