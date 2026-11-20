#include "ui/input/private_pointer.h"
#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "widget/runtime/widget_engine.h"
#include "desktop/desktop_source.h"
#include "system/panel/system_panel.h"
#include "layout/page_navigation_rules.h"
#include "desktop/desktop_keyboard_rules.h"
#include "app/popup/categorized_popup_scope.h"
#include "widgets/single_cell_widget_rules.h"

// Top-level keyboard command dispatch.

bool DesktopApp::TryHandlePageNavigationKey(
    WPARAM key, bool repeated)
{
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT)
        return false;

    bool textInputActive = renameController_.IsActive() ||
        (widgetEngine_ && widgetEngine_->HasFocusedHostInput());
    if (auto* view = GetCategorizedPopupView(); view && view->IsSearchFocused()) textInputActive = true;
    for (const auto& container : containers_)
    {
        const auto* searchable =
            dynamic_cast<const ScrollingItemWidget*>(container.get());
        if (searchable && searchable->IsSearchFocused())
            textInputActive = true;
    }
    const bool movingWidget = widgetAction_ == WidgetAction::Move &&
        mouseDownWidgetIndex_ < widgets_.size();
    using snowdesktop::page_navigation_rules::KeyboardNavigationAction;
    const auto navigationAction =
        snowdesktop::page_navigation_rules::ResolveKeyboardNavigationAction(
            generalSettings_.pageNavigationKeyboardEnabled, textInputActive,
            quickNavigation_->open || IsCollectionPopupKeyboardTarget() ||
                !luaWidgetPanelRequest_.widgetId.empty() ||
                (systemPanel_ && systemPanel_->IsOpen()) ||
                keyboardNavInsideWidget_ ||
                (widgetEngine_ && widgetEngine_->HasHostViewKeyboardFocus()),
            dragSession_.IsActive() || movingWidget);
    if (navigationAction == KeyboardNavigationAction::Ignore)
        return false;

    UINT pressedModifiers = 0;
    if ((snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) != 0)
        pressedModifiers |= MOD_CONTROL;
    if ((snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) != 0)
        pressedModifiers |= MOD_ALT;
    if ((snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000) != 0)
        pressedModifiers |= MOD_SHIFT;
    if ((snowdesktop::private_pointer::AsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (snowdesktop::private_pointer::AsyncKeyState(VK_RWIN) & 0x8000) != 0)
        pressedModifiers |= MOD_WIN;

    using snowdesktop::page_navigation_rules::ShortcutMatches;
    const UINT virtualKey = static_cast<UINT>(key);
    const bool matchesPrevious = ShortcutMatches(
        generalSettings_.pageNavigationPreviousModifiers,
        generalSettings_.pageNavigationPreviousVirtualKey,
        pressedModifiers, virtualKey);
    const bool matchesNext = ShortcutMatches(
        generalSettings_.pageNavigationNextModifiers,
        generalSettings_.pageNavigationNextVirtualKey,
        pressedModifiers, virtualKey);
    if (!matchesPrevious && !matchesNext)
        return false;

    const auto conflictsWith = [&](UINT modifiers,
        UINT configuredVirtualKey) {
        return ShortcutMatches(
            modifiers, configuredVirtualKey,
            pressedModifiers, virtualKey);
    };
    const bool configuredConflict =
        (matchesPrevious && matchesNext) ||
        (matchesPrevious &&
            snowdesktop::page_navigation_rules::
                IsReservedDesktopSingleKey(
                    generalSettings_.pageNavigationPreviousModifiers,
                    generalSettings_.pageNavigationPreviousVirtualKey)) ||
        (matchesNext &&
            snowdesktop::page_navigation_rules::
                IsReservedDesktopSingleKey(
                    generalSettings_.pageNavigationNextModifiers,
                    generalSettings_.pageNavigationNextVirtualKey)) ||
        (navigationSettings_.enabled && conflictsWith(
            navigationSettings_.modifiers,
            navigationSettings_.virtualKey)) ||
        (generalSettings_.forceRestartHotkeyEnabled &&
            conflictsWith(generalSettings_.forceRestartHotkeyModifiers,
                generalSettings_.forceRestartHotkeyVirtualKey)) ||
        (generalSettings_.softwareDesktopHotkeyEnabled &&
            conflictsWith(generalSettings_.softwareDesktopHotkeyModifiers,
                generalSettings_.softwareDesktopHotkeyVirtualKey)) ||
        (generalSettings_.desktopPassthroughHotkeyEnabled &&
            conflictsWith(
                generalSettings_.desktopPassthroughHotkeyModifiers,
                generalSettings_.desktopPassthroughHotkeyVirtualKey)) ||
        (generalSettings_.dockEnabled &&
            dockSettings_.floatingShortcutMode &&
            conflictsWith(
                dockSettings_.floatingHotkeyModifiers,
                dockSettings_.floatingHotkeyVirtualKey));
    if (configuredConflict)
        return false;

    // A configured key remains consumed at page boundaries and on repeats,
    // but only a fresh physical press may initiate one page transition.
    if (!repeated)
    {
        const int oldOffset = pageOffset_;
        NavigatePageOffset(matchesPrevious ? -1 : 1);
        if (pageOffset_ != oldOffset &&
            navigationAction == KeyboardNavigationAction::NavigateDuringDrag)
        {
            // Paging is a preview: retain the source cells until release.
            // Rebuild the target now, including when the next event is button-up.
            navAutoFlipDir_ = 0;
            navAutoFlipTick_ = 0;
            cachedDropPreview_ = {};
            cachedDropPreviewPoint_ = { -1, -1 };
            cachedDropPreviewTarget_ = nullptr;
            cachedDropPreviewSlot_ = nullptr;
            InvalidateDragStaticScene();
            if (dragSession_.IsActive())
            {
                int mods = 0;
                if (pressedModifiers & MOD_CONTROL) mods |= MK_CONTROL;
                if (pressedModifiers & MOD_SHIFT) mods |= MK_SHIFT;
                if (pressedModifiers & MOD_ALT) mods |= MK_ALT;
                RefreshDragTargetAt(dragSession_.CurrentPoint(), mods);
            }
            else if (movingWidget && mouseDownWidgetIndex_ < widgets_.size())
            {
                OnMouseMoveAt(0, lastMousePoint_);
            }
            PresentPointerInteractionFrame();
        }
    }
    return true;
}

void DesktopApp::DispatchLuaWidgetViewKeyEvent(
    WPARAM key, bool pressed, bool repeated)
{
    if (!widgetEngine_ || quickNavigation_->open ||
        IsCollectionPopupKeyboardTarget())
        return;
    const bool ctrl = (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift = (snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    const bool alt = (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) != 0;
    (void)widgetEngine_->DispatchHostViewKeyEvent(
        key, pressed, repeated, ctrl, shift, alt);
}

bool DesktopApp::OnKeyDown(WPARAM key, bool repeated)
{
    if (key == VK_ESCAPE && nativeMenuSession_)
    {
        DismissActiveContextMenuForPopupTransition();
        return true;
    }
    if (TryHandleFilePreviewKey(key, repeated)) return true;
    // Text arrives as WM_CHAR. Commands, arrows and scope changes end the
    // previous query before subsequent text can reuse it.
    if ((key < VK_SPACE && key != VK_SHIFT && key != VK_CAPITAL) || key > VK_OEM_CLEAR ||
        (key >= VK_PRIOR && key <= VK_HELP) ||
        (key >= VK_F1 && key <= VK_F24) ||
        key == VK_LWIN || key == VK_RWIN ||
        (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
        (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) != 0)
        typeToSelectSession_.Reset();
    if (DockTrayPopupKeyDown(key)) return true;
    CancelRenameClick();
    if (key == VK_ESCAPE && (dragSession_.HasContext() ||
            widgetAction_ != WidgetAction::None || largeIconGesture_))
    {
        // Holding Esc after dismissing a popup must not cancel the same drag.
        if (repeated || TryDismissPopupForEscape()) return true;
    }
    if (largeIconGesture_)
    {
        if (key == VK_ESCAPE) { CancelLargeIconGesture(); return true; }
        if (largeIconGesture_->creating && (key == VK_LEFT || key == VK_RIGHT || key == VK_UP || key == VK_DOWN))
        {
            auto& config = largeIconGesture_->config;
            if (key == VK_LEFT) config.columns = std::max(1, config.columns - 1);
            if (key == VK_RIGHT) config.columns = std::min(1024, config.columns + 1);
            if (key == VK_UP) config.rows = std::max(1, config.rows - 1);
            if (key == VK_DOWN) config.rows = std::min(1024, config.rows + 1);
            HandleLargeIconPointerMove(lastMousePoint_);
            return true;
        }
        if (key == VK_RETURN && largeIconGesture_->creating) { HandleLargeIconPointerDown(lastMousePoint_); return true; }
    }
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT)
    {
        RefreshDragHintFromKeyboard();
        return false;
    }

    if (renameController_.IsActive()) return false;
    const bool popupKeyboardTarget = IsCollectionPopupKeyboardTarget();
    const bool previewKeyboardTarget = folderPreview_ && popupKeyboardTarget;
    const bool folderPopupKeyboardTarget = dockFolderPopupOpen_ && (!folderPreview_ || popupKeyboardTarget);
    if (previewKeyboardTarget && key == VK_ESCAPE && repeated) return true;
    if (previewKeyboardTarget && !dragSession_.HasContext() && !dragDropController_.IsTransportActive())
    {
        if (key == 'F' &&
            (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) &&
            !(snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000) &&
            !(snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000))
        {
            if (!repeated) SetFolderPreviewSearchOpen(true);
            return true;
        }
        if (key == VK_ESCAPE && folderPreview_->searchOpen)
        {
            SetFolderPreviewSearchOpen(false);
            return true;
        }
    }

    std::optional<CategorizedPopupScope> popupLayout;
    const bool popupArrowKey = key == VK_UP || key == VK_DOWN || key == VK_LEFT || key == VK_RIGHT;
    const bool popupNavigationKey = key == VK_RETURN || key == VK_ESCAPE || popupArrowKey ||
        key == VK_HOME || key == VK_END || key == VK_TAB;
    if (popupNavigationKey && !dockFolderPopupOpen_ && IsCollectionPopupInteractive())
        if (const auto* popup = GetOpenPopupWidget())
            if (auto* view = GetCategorizedPopupView())
                popupLayout.emplace(view, GetCategorizedPopupFrame(GetCollectionPopupRect(*popup)));

    // Handle searchable widget keyboard input.
    {
        if (auto* view = GetCategorizedPopupView(); view && view->IsSearchFocused())
        {
            const bool handled = view->HandleSearchKey(key);
            popupScrollOffset_ = 0;
            if (previewKeyboardTarget) RefreshDockFolderPopupGeometry();
            else ResetCollectionPopupAnimationCache();
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (handled || (key != VK_RETURN && key != VK_UP && key != VK_DOWN)) return handled;
        }
        if (!previewKeyboardTarget)
        {
            for (auto& c : containers_)
            {
                auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (!searchable || !searchable->IsSearchFocused()) continue;
                if (searchable->HandleSearchKey(key))
                {
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return true;
                }
                // Unhandled edit shortcuts must not affect desktop files.
                if (key != VK_RETURN && key != VK_UP && key != VK_DOWN)
                    return false;
                break;
            }
        }
    }

    if (quickNavigation_->open)
    {
        if (key == VK_ESCAPE)
        {
            CloseQuickNavigation();
            return true;
        }
    }

    bool ctrl = (snowdesktop::private_pointer::AsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (snowdesktop::private_pointer::AsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    bool alt = (snowdesktop::private_pointer::AsyncKeyState(VK_MENU) & 0x8000) != 0;
    bool restoreFloatingDockLayer = false;
    if (key == VK_RETURN && repeated && popupKeyboardTarget) return true;
    if (key == VK_RETURN && !alt && !repeated && !quickNavigation_->open && luaWidgetPanelRequest_.widgetId.empty() &&
        TryFolderPreviewActivation({}, (ctrl ? MK_CONTROL : 0) | (shift ? MK_SHIFT : 0), {}, true))
        return true;

    // Keyboard selection follows the whole fan's order and scrolls its window
    // as needed; the final action still opens the complete grid.
    if (auto* popup = GetOpenPopupWidget(); !ctrl && !alt &&
        popupKeyboardTarget && popup && UsesCollectionPopupFan(*popup) &&
        GetPopupItemCount(*popup) > 0)
    {
        const int count = static_cast<int>(GetPopupItemCount(*popup));
        const auto& keys = GetPopupItemKeysView(*popup);
        int current = popupFanActionFocused_ ? count : -1;
        for (int i = 0; current < 0 && i < count; ++i)
        {
            if (dockFolderPopupOpen_)
            {
                if (popup->folderEntries[GetPopupFolderEntryIndex(
                        *popup, static_cast<size_t>(i))].selected)
                    current = i;
            }
            else
            {
                const auto item = FindItemIndexByKey(keys[i]);
                if (item < items_.size() && items_[item].selected) current = i;
            }
        }
        if (key == VK_RETURN && current >= 0)
        {
            if (current == count)
                ShowAllCollectionPopupItems();
            else if (dockFolderPopupOpen_)
            {
                const auto path = popup->folderEntries[GetPopupFolderEntryIndex(
                    *popup, static_cast<size_t>(current))].fullPath;
                if (LaunchPathWithShortcutPolicy(hwnd_, path)) CloseCollectionPopup();
            }
            else
            {
                const auto item = FindItemIndexByKey(keys[current]);
                const bool keepPopup = popup->type == DesktopWidgetType::Collection;
                if (item < items_.size() && LaunchDesktopItem(item) && !keepPopup) CloseCollectionPopup();
            }
            return true;
        }
        if (popupArrowKey || key == VK_HOME || key == VK_END || key == VK_TAB ||
            (key == VK_RETURN && current < 0))
        {
            int next = current;
            const bool forward = key == VK_RIGHT || (key == VK_TAB && !shift) ||
                (key == VK_DOWN && CollectionPopupFanRootAbove()) ||
                (key == VK_UP && !CollectionPopupFanRootAbove());
            if (current < 0 || key == VK_HOME) next = 0;
            else if (key == VK_END) next = count;
            else if (key == VK_TAB) next = (current + (forward ? 1 : count)) % (count + 1);
            else next = std::clamp(current + (forward ? 1 : -1), 0, count);
            ClearSelection();
            if (dockFolderPopupOpen_)
                for (auto& entry : popup->folderEntries) entry.selected = false;
            popupFanActionFocused_ = next == count;
            if (next < count)
            {
                EnsureCollectionPopupFanItemVisible(static_cast<size_t>(next));
                if (dockFolderPopupOpen_)
                    popup->folderEntries[GetPopupFolderEntryIndex(
                        *popup, static_cast<size_t>(next))].selected = true;
                else
                {
                    const auto item = FindItemIndexByKey(keys[next]);
                    if (item < items_.size()) items_[item].selected = true;
                }
            }
            if (!dockFolderPopupOpen_ && popupWidgetIndex_ < widgets_.size())
            {
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ = popupWidgetIndex_;
                keyboardNavMemberIndex_ = next < count ? next : -1;
                keyboardNavVisualFocus_ = true;
            }
            InvalidateFloatingPopupWindow(false);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return true;
        }
    }

    if (widgetEngine_ && !quickNavigation_->open &&
        !luaWidgetPanelRequest_.widgetId.empty() &&
        widgetEngine_->HandleHostViewKey(
            luaWidgetPanelRequest_.widgetId,
            key, ctrl, shift, alt,
            luaWidgetPanelRequest_.surface, repeated))
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        RestoreInteractionInputFocus();
        return true;
    }
    if (!luaWidgetPanelRequest_.widgetId.empty() &&
        (luaWidgetPanelRequest_.modal ||
            (luaWidgetPanelAnimation_.IsInteractive() && popupNavigationKey) ||
            (widgetEngine_ && widgetEngine_->HasHostViewKeyboardFocus())) &&
        key != VK_ESCAPE)
        return true;

    if (dockFolderPopupOpen_ && popupKeyboardTarget && !ctrl && !alt &&
        (popupArrowKey || key == VK_HOME || key == VK_END ||
            (key == VK_RETURN && std::none_of(dockFolderPopupWidget_.folderEntries.begin(),
                dockFolderPopupWidget_.folderEntries.end(), [](const auto& entry) { return entry.selected; }))))
    {
        const int count = static_cast<int>(GetPopupItemCount(dockFolderPopupWidget_));
        if (count == 0) return true;
        int current = -1;
        for (int index = 0; index < count; ++index)
            if (dockFolderPopupWidget_.folderEntries[
                    GetPopupFolderEntryIndex(dockFolderPopupWidget_, static_cast<size_t>(index))].selected)
            {
                current = index;
                break;
            }
        const int columns = UsesCollectionPopupList(dockFolderPopupWidget_)
            ? 1 : std::max(1, GetCollectionPopupColumnCount(popupRect_));
        int next = current < 0 ? 0 : current;
        if (key == VK_HOME) next = 0;
        else if (key == VK_END) next = count - 1;
        else if (current >= 0)
            next = std::clamp(current + (key == VK_UP ? -columns : key == VK_DOWN ? columns :
                key == VK_LEFT ? -1 : 1), 0, count - 1);
        if (auto* view = GetCategorizedPopupView()) view->SetSearchFocused(false);
        ClearSelection();
        for (auto& entry : dockFolderPopupWidget_.folderEntries) entry.selected = false;
        dockFolderPopupWidget_.folderEntries[GetPopupFolderEntryIndex(
            dockFolderPopupWidget_, static_cast<size_t>(next))].selected = true;
        keyboardNavWidgetIndex_ = FindWidgetIndexById(dockFolderPopupMappingWidgetId_);
        keyboardNavInsideWidget_ = keyboardNavWidgetIndex_ < widgets_.size();
        keyboardNavMemberIndex_ = next;
        keyboardNavVisualFocus_ = true;
        const RECT content = GetCollectionPopupContentRect(popupRect_);
        const RECT item = GetCollectionPopupItemRect(popupRect_, static_cast<size_t>(next));
        if (item.top < content.top) popupScrollOffset_ += item.top - content.top;
        else if (item.bottom > content.bottom) popupScrollOffset_ += item.bottom - content.bottom;
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
            GetCollectionPopupMaxScrollOffset(dockFolderPopupWidget_, popupRect_));
        ResetCollectionPopupAnimationCache();
        UpdateHostInputImePosition();
        InvalidateFloatingPopupWindow(false);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return true;
    }

    if (!previewKeyboardTarget && ctrl && !alt && (key == 'Z' || key == 'Y') && widgetEngine_ &&
        !widgetEngine_->HasFocusedHostInput() &&
        !(snowdesktop::private_pointer::AsyncKeyState(VK_LWIN) & 0x8000) && !(snowdesktop::private_pointer::AsyncKeyState(VK_RWIN) & 0x8000))
    {
        size_t selectedLua = static_cast<size_t>(-1);
        bool multipleSelected = false;
        for (size_t index = 0; index < widgets_.size(); ++index)
        {
            if (!widgets_[index].selected) continue;
            if (selectedLua != static_cast<size_t>(-1) ||
                !IsLuaDesktopSurface(widgets_[index]))
            {
                multipleSelected = true;
                break;
            }
            selectedLua = index;
        }
        if (!multipleSelected && selectedLua < widgets_.size())
        {
            if (repeated) return true;
            const bool redo = key == 'Y' || shift;
            const auto* content = ResolveLuaWidgetContent(&widgets_[selectedLua]);
            if (!content) return true;
            const auto& widgetId = content->id;
            widgetEngine_->EnsureWidgetLoaded(
                widgetId, content->packageId);
            const bool available = redo
                ? widgetEngine_->RuntimeCanRedoHostLogicalSlot(widgetId)
                : widgetEngine_->RuntimeCanUndoHostLogicalSlot(widgetId);
            if (available)
            {
                snowdesktop::widget_runtime::LogicalSlotChange change;
                std::string error;
                const bool succeeded = redo
                    ? widgetEngine_->RuntimeRedoHostLogicalSlot(
                        widgetId, change, error)
                    : widgetEngine_->RuntimeUndoHostLogicalSlot(
                        widgetId, change, error);
                if (!succeeded)
                {
                    widgetEngine_->RuntimeRecordError(widgetId,
                        "logical slot host history: " + error);
                    MessageBeep(MB_ICONWARNING);
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                RestoreInteractionInputFocus();
                return true;
            }
            // A selected Lua component owns this shortcut even when its
            // logical-slot history is empty; never fall through to file undo.
            return true;
        }
        if (std::any_of(widgets_.begin(), widgets_.end(), [](const auto& widget) {
                return widget.selected && (widget.type == DesktopWidgetType::LuaScript || widget.type == DesktopWidgetType::LuaStack);
            })) return true;
    }

    if (widgetEngine_ && !quickNavigation_->open &&
        luaWidgetPanelRequest_.widgetId.empty() &&
        !popupKeyboardTarget)
    {
        size_t selectedLua = static_cast<size_t>(-1);
        bool multipleSelected = false;
        for (size_t index = 0; index < widgets_.size(); ++index)
        {
            if (!widgets_[index].selected) continue;
            if (selectedLua != static_cast<size_t>(-1) ||
                !IsLuaDesktopSurface(widgets_[index]))
            {
                multipleSelected = true;
                break;
            }
            selectedLua = index;
        }
        if (!multipleSelected && selectedLua < widgets_.size())
        {
            const auto* content = ResolveLuaWidgetContent(&widgets_[selectedLua]);
            if (!content) return true;
            const auto& widgetId = content->id;
            widgetEngine_->EnsureWidgetLoaded(
                widgetId, content->packageId);
            if (widgetEngine_->HandleHostViewKey(
                    widgetId, key, ctrl, shift, alt,
                    "desktop", repeated))
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
                RestoreInteractionInputFocus();
                return true;
            }
        }
    }

    bool handled = false;
    if (widgetEngine_ && widgetEngine_->HasFocusedHostInput()) return false;
    if (dragSession_.IsActive() && key != VK_ESCAPE) return false;
    const bool win = (snowdesktop::private_pointer::AsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
        (snowdesktop::private_pointer::AsyncKeyState(VK_RWIN) & 0x8000) != 0;
    const bool insertPaste = key == VK_INSERT && shift && !ctrl && !alt && !win;
    key = snowdesktop::desktop_keyboard_rules::NormalizeFileCommandKey(
        static_cast<UINT>(key), ctrl, shift, alt, win);
    if (insertPaste) { ctrl = true; shift = false; }
    // Alt/Win combinations belong to the system or focused component.
    if (win || (alt && key != VK_RETURN)) return false;
    if (!ctrl && !alt && (key == VK_RETURN || key == VK_SPACE) &&
        !quickNavigation_->open && !IsCollectionPopupInteractive() &&
        luaWidgetPanelRequest_.widgetId.empty() && (!systemPanel_ || !systemPanel_->IsOpen()) &&
        ActivateDesktopSurfaceKeyboardFocus(repeated))
        return true;
    if (!ctrl && !alt && popupKeyboardTarget && !dockFolderPopupOpen_ &&
        popupWidgetIndex_ < widgets_.size() && (popupArrowKey || key == VK_RETURN) &&
        (!keyboardNavInsideWidget_ || keyboardNavWidgetIndex_ != popupWidgetIndex_ ||
            (keyboardNavMemberIndex_ < 0 && !keyboardNavSearchBox_)))
    {
        popupLayout.reset();
        popupScrollOffset_ = 0;
        EnterWidget();
        InvalidateFloatingPopupWindow(false);
        return true;
    }
    switch (key)
    {
    case 'Z':
    case 'Y':
        if (!ctrl || quickNavigation_->open || !luaWidgetPanelRequest_.widgetId.empty()) break;
        if (!repeated && !QueueFileHistory(key == 'Y' || shift)) MessageBeep(MB_ICONWARNING);
        return true;
    case 'N':
        if (!ctrl || !shift) break;
        handled = true;
        restoreFloatingDockLayer = true;
        if (!repeated)
        {
            std::wstring directory;
            std::optional<NewItemTarget> newItemTarget;
            if (previewKeyboardTarget && !dockFolderPopupAvailable_)
            {
                MessageBeep(MB_ICONWARNING);
                break;
            }
            if (folderPopupKeyboardTarget && dockFolderPopupAvailable_)
            {
                directory = dockFolderPopupWidget_.sourceFolderPath;
                newItemTarget = NewItemTarget{NewItemTarget::Surface::FolderPopup, {}, dockFolderPopupSourceId_};
            }
            else
            {
                const size_t target = FindNewItemShortcutTarget();
                if (target < widgets_.size() &&
                    widgets_[target].type == DesktopWidgetType::FolderMapping)
                {
                    directory = widgets_[target].sourceFolderPath;
                    newItemTarget = NewItemTarget{NewItemTarget::Surface::FolderMapping, {}, widgets_[target].id};
                }
                else
                {
                    newItemTarget = target < widgets_.size()
                        ? NewItemTarget{NewItemTarget::Surface::FileCategories, {}, widgets_[target].id}
                        : NewItemTarget{NewItemTarget::Surface::Desktop, CellFromPoint(lastMousePoint_), {}};
                    wchar_t desktopPath[MAX_PATH]{};
                    if (snowdesktop::desktop_source::CopyDirectory(desktopPath)) directory = desktopPath;
                }
            }
            if (!directory.empty())
            {
                POINT point = lastMousePoint_;
                ClientToScreen(hwnd_, &point);
                ShowNewMenuAndInvoke(point, directory, true, newItemTarget);
                RequestShellRefresh();
            }
        }
        break;
    case VK_F2:
        if (!ctrl && !shift)
        {
            BeginRenameSelected();
            handled = true;
        }
        break;
    case VK_F5:
        handled = true;
        restoreFloatingDockLayer = true;
        ReloadItems();
        break;
    case VK_DELETE:
    {
        if (repeated) return true;
        handled = true;
        restoreFloatingDockLayer = true;
        if (DockContainer* dock = GetDockContainer())
        {
            std::vector<Item*> selectedDockItems = dock->GetSelectedItems();
            if (!selectedDockItems.empty())
            {
                GridCell returnCell;
                if (const GridPage* firstPage = GetFirstPageGridPage())
                    returnCell.pageId = firstPage->id;
                MoveDockItemsToDesktop(selectedDockItems, returnCell);
                SaveLayoutSlots();
                ClearSelection();
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            }
        }

        if (!GetSelectedFolderEntryPaths().empty())
        {
            // Failure to queue a mapped-folder deletion must never fall through
            // and delete a different desktop selection instead.
            DeleteSelectedFolderEntries(shift);
            break;
        }

        const auto selectedStack = std::find_if(widgets_.begin(), widgets_.end(), [](const auto& widget) {
            return widget.selected && widget.type == DesktopWidgetType::LuaStack;
        });
        if (selectedStack != widgets_.end())
        {
            const std::wstring id = selectedStack->id;
            POINT screen = lastMousePoint_; ClientToScreen(hwnd_, &screen);
            (void)RemoveLuaStackMember(id, screen);
            break;
        }
        QueueFileDeletion(GetSelectedDesktopFilePaths(), shift);
        break;
    }
    case 'C':
    {
        if (!ctrl) break;
        handled = true;
        restoreFloatingDockLayer = true;
        if (shift)
        {
            auto paths = GetSelectedFolderEntryPaths();
            if (paths.empty()) paths = GetSelectedDesktopFilePaths();
            CopyPathsToClipboard(paths);
            break;
        }
        if (CopyCutSelectedFolderEntries(false))
            break;
        const bool hasNamespaceSelection = std::any_of(items_.begin(), items_.end(),
            [](const auto& item) {
                return item.selected && item.childPidl.get() && !item.desktopIconClsid.empty();
            });
        if (hasNamespaceSelection)
            InvokeSelectedShellVerb("copy");
        else
            CopyCutSelectedDesktopFiles(false, L"desktop_keyboard");
        break;
    }
    case 'X':
        if (!ctrl) break;
    {
        handled = true;
        restoreFloatingDockLayer = true;
        if (CopyCutSelectedFolderEntries(true))
            break;

        CopyCutSelectedDesktopFiles(true);
    }
    break;
    case 'V':
        if (!ctrl) break;
    {
        handled = true;
        restoreFloatingDockLayer = true;
        // Once a popup/mapping has selected the destination, failure belongs
        // to that target. Never retry the user's paste on the desktop.
        if (folderPopupKeyboardTarget)
        {
            if (!dockFolderPopupAvailable_ || !PasteClipboardToFolderPath(
                    dockFolderPopupWidget_.sourceFolderPath))
                MessageBeep(MB_ICONWARNING);
            break;
        }
        const auto mappingIndex = FindFolderMappingShortcutTarget();
        if (mappingIndex < widgets_.size())
        {
            if (!PasteClipboardToFolderMapping(mappingIndex))
                MessageBeep(MB_ICONWARNING);
            break;
        }
        PasteClipboardToDesktop();
    }
    break;
    case 'A':
        if (!ctrl) break;
        if (popupKeyboardTarget)
            ShowAllCollectionPopupItems();
        handled = true;
        restoreFloatingDockLayer = true;
    {
        if (auto* popup = GetOpenPopupWidget();
            popupKeyboardTarget && popup)
        {
            const size_t owner = dockFolderPopupOpen_
                ? FindWidgetIndexById(dockFolderPopupMappingWidgetId_) : popupWidgetIndex_;
            const auto targets = GetPopupSelectionTargets();
            ClearSelection();
            if (dockFolderPopupOpen_)
                for (auto& entry : popup->folderEntries) entry.selected = false;
            selectionController_.SelectAll(targets);
            SyncKeyboardNavFromSelection();
            if (owner < widgets_.size() && snowdesktop::single_cell_widget::IsSingleCell(widgets_[owner]) &&
                (!keyboardNavInsideWidget_ || dockFolderPopupOpen_ ||
                    widgets_[owner].type == DesktopWidgetType::FileCategories))
            {
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ = owner;
                keyboardNavMemberIndex_ = targets.empty() ? -1 : 0;
            }
            keyboardNavVisualFocus_ = true;
            InvalidateRect(
                hwnd_, nullptr, FALSE);
            break;
        }
        const size_t widgetIndex = GetSelectionWidgetIndex();
        if (widgetIndex < widgets_.size())
        {
            const auto targets = GetWidgetSelectionTargets(widgetIndex);
            ClearSelection();
            selectionController_.SelectAll(targets);
            if (targets.empty())
                widgets_[widgetIndex].selected = true;
            SyncKeyboardNavFromSelection();
            keyboardNavVisualFocus_ = targets.empty();
            InvalidateRect(hwnd_, nullptr, FALSE);
            break;
        }
        const auto targets = GetDesktopSelectionTargets();
        ClearSelection();
        selectionController_.SelectAll(targets);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    break;
    case VK_RETURN:
        handled = true;
        restoreFloatingDockLayer = true;
        if (alt)
        {
            if (ctrl || shift || repeated) break;
            const auto paths = GetSelectedFolderEntryPaths();
            if (paths.size() == 1) ShowPathProperties(paths.front());
            else if (paths.empty()) InvokeSelectedShellVerb("properties");
            break;
        }
        if (dockFolderPopupOpen_ && popupKeyboardTarget)
        {
            if (repeated) break;
            const auto paths = GetSelectedFolderEntryPaths();
            if (paths.size() == 1 &&
                LaunchPathWithShortcutPolicy(hwnd_, paths.front()) && !folderPreview_)
                CloseCollectionPopup();
            break;
        }
        if (keyboardNavInsideWidget_)
        {
            if (keyboardNavSearchBox_ &&
                keyboardNavWidgetIndex_ < widgets_.size())
            {
                for (auto& container : containers_)
                {
                    auto* searchable =
                        dynamic_cast<ScrollingItemWidget*>(
                            container.get());
                    if (!searchable ||
                        searchable->GetWidgetData() !=
                            &widgets_[keyboardNavWidgetIndex_])
                        continue;
                    searchable->SetSearchFocused(true);
                    UpdateHostInputImePosition();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    break;
                }
            }
            else if (keyboardNavWidgetIndex_ < widgets_.size() &&
                ((widgets_[keyboardNavWidgetIndex_].type ==
                      DesktopWidgetType::CollectionGroup &&
                  keyboardNavCollectionGroupTabs_) ||
                 (widgets_[keyboardNavWidgetIndex_].type ==
                      DesktopWidgetType::FileGroup &&
                  (keyboardNavCollectionGroupTabs_ ||
                   keyboardNavFileGroupCategoryTabs_))))
                NavigateWidgetMembers(VK_DOWN);
            else
                OpenWidgetMember(
                    keyboardNavWidgetIndex_,
                    keyboardNavMemberIndex_);
        }
        else if (std::any_of(widgets_.begin(), widgets_.end(),
            [](const DesktopWidget& w) { return w.selected; }))
            EnterWidget();
        else
            OpenSelectedDesktopItem();
        break;
    case VK_ESCAPE:
        handled = true;
        restoreFloatingDockLayer = true;
        // Release projected geometry before a synchronous close can clear it
        // or retire its transient source view.
        popupLayout.reset();
        for (auto& container : containers_)
            if (auto* view = dynamic_cast<ScrollingItemWidget*>(container.get()); view && view->HasCategoryTabPress())
            {
                CancelPointerPressWithoutCaptureRelease();
                snowdesktop::private_pointer::ReleasePointerCapture();
                return true;
            }
        if (auto* view = GetCategorizedPopupView(); view && view->HasCategoryTabPress())
        {
            CancelPointerPressWithoutCaptureRelease();
            snowdesktop::private_pointer::ReleasePointerCapture();
            return true;
        }
        if (widgetAction_ != WidgetAction::None)
        {
            CancelPointerPressWithoutCaptureRelease();
            snowdesktop::private_pointer::ReleasePointerCapture();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else if (dragSession_.IsActive())
        {
            CancelActiveItemDrag();
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else if (!luaWidgetPanelRequest_.widgetId.empty())
        {
            if (luaWidgetPanelRequest_.dismissOnEscape)
                CloseLuaWidgetPanel(
                    luaWidgetPanelRequest_.widgetId,
                    "escape");
        }
        else if (previewKeyboardTarget)
            NavigateFolderPreviewBack();
        else if (popupKeyboardTarget)
        {
            if (keyboardNavInsideWidget_ && keyboardNavWidgetIndex_ < widgets_.size() &&
                (keyboardNavWidgetIndex_ == popupWidgetIndex_ ||
                    (dockFolderPopupOpen_ &&
                        widgets_[keyboardNavWidgetIndex_].id == dockFolderPopupMappingWidgetId_ &&
                        snowdesktop::single_cell_widget::IsSingleCell(widgets_[keyboardNavWidgetIndex_]))))
            {
                ExitWidget();
                if (IsCollectionPopupInteractive()) CloseCollectionPopup(false);
            }
            else CloseCollectionPopup();
        }
        else if (keyboardNavInsideWidget_)
            ExitWidget();
        else
        {
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        break;
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
        handled = true;
        if (keyboardNavInsideWidget_)
        {
            NavigateWidgetMembers(key);
            if (popupKeyboardTarget) InvalidateFloatingPopupWindow(false);
        }
        else
            NavigateDesktopGrid(key);
        break;
    default:
        break;
    }

    if (restoreFloatingDockLayer)
    {
        // Asynchronous Shell operations restore the floating input proxy
        // after the final completion. Synchronous actions can retain it now.
        if (shellFileOperationInFlight_ == 0)
            RestoreInteractionInputFocus();
    }
    return handled;
}

bool DesktopApp::TryDismissPopupForEscape()
{
    using snowdesktop::desktop_keyboard_rules::PopupEscapeAction;
    const auto action = snowdesktop::desktop_keyboard_rules::ResolvePopupEscapeAction(
        quickNavigation_->open, !luaWidgetPanelRequest_.widgetId.empty(),
        luaWidgetPanelRequest_.dismissOnEscape,
        GetOpenPopupWidget() != nullptr && (!folderPreview_ || folderPreview_->keyboardFocused));
    switch (action)
    {
    case PopupEscapeAction::CloseQuickNavigation:
        CloseQuickNavigation();
        return true;
    case PopupEscapeAction::CloseLuaPanel:
        CloseLuaWidgetPanel(luaWidgetPanelRequest_.widgetId, "escape");
        return true;
    case PopupEscapeAction::KeepLuaPanel:
        return true;
    case PopupEscapeAction::CloseCollectionPopup:
        if (folderPreview_)
            return NavigateFolderPreviewBack();
        pendingCollectionPopupOpen_.reset();
        CancelCollectionPopupDwell();
        CancelCollectionGroupTabDwell();
        CloseCollectionPopup(false);
        cachedDropPreview_ = {};
        cachedDropPreviewPoint_ = { -1, -1 };
        cachedDropPreviewTarget_ = nullptr;
        cachedDropPreviewSlot_ = nullptr;
        return true;
    case PopupEscapeAction::None:
    default:
        return false;
    }
}

/**
 * @brief 根据键盘修饰键状态刷新拖拽提示信息
 */
