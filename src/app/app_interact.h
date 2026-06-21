/**
 * @file app_interact.h
 * @brief DesktopApp 交互与拖放操作的内联实现
 *
 * 本文件包含 DesktopApp 类的所有交互处理内联方法，包括鼠标事件、键盘事件、
 * 拖放操作、重命名、窗口小部件、集合弹窗、快捷导航面板以及系统托盘等功能。
 * 该文件在 app.h 中类定义之后通过 #include 包含。
 */

#pragma once
// Inline implementations for DesktopApp — Interaction & Tray.
// This file is included by app.h after the class definition.

#include "drop_model.h"
#include <dwmapi.h>
#include <wincodec.h>
#include <urlmon.h>

// ── Interaction ─────────────────────────────────────────────

/**
 * @brief 命中测试：根据点坐标查找桌面项索引（向后兼容包装）
 * @param pt 客户端坐标点
 * @return 项在 items_ 数组中的索引，未找到返回 -1
 */
inline int DesktopApp::HitTestItem(POINT pt) const
{
    // Backward-compat wrapper: returns items_ index for Shell/COM code
    DesktopIcon* icon = HitTestIcon(pt);
    if (!icon) return -1;
    DesktopItem* di = icon->GetDesktopItem();
    for (size_t j = 0; j < items_.size(); ++j)
        if (&items_[j] == di) return static_cast<int>(j);
    return -1;
}

/**
 * @brief 命中测试：根据点坐标查找桌面图标对象
 * @param pt 客户端坐标点
 * @return 指向 DesktopIcon 的指针，未找到返回 nullptr
 */
inline DesktopIcon* DesktopApp::HitTestIcon(POINT pt) const
{
    for (int i = static_cast<int>(items_oo_.size()) - 1; i >= 0; --i)
    {
        auto* icon = dynamic_cast<DesktopIcon*>(items_oo_[i].get());
        if (!icon) continue;
        DesktopItem* di = icon->GetDesktopItem();
        if (!di || IsRectEmptyRect(di->bounds)) continue;
        if (!di->layoutKey.empty() && collectedKeysCache_.count(ToUpperInvariant(di->layoutKey))) continue;
        RECT selRect = GetItemSelectionRect(di->bounds, di->selected);
        if (PtInRect(&selRect, pt)) return icon;
    }
    return nullptr;
}

/**
 * @brief 判断指定桌面项是否位于任意窗口小部件内
 * @param item 要检查的桌面项
 * @return 若在任意小部件内返回 true
 */
inline bool DesktopApp::IsItemInAnyWidget(const DesktopItem& item) const
{
    std::wstring key = ToUpperInvariant(item.layoutKey);
    if (key.empty()) return false;
    return collectedKeysCache_.contains(key);
}

/**
 * @brief 获取独立窗口小部件的框架矩形（考虑网格间距）
 * @param widget 桌面小部件引用
 * @return 框架矩形
 */
inline RECT DesktopApp::GetStandaloneWidgetFrameRect(const DesktopWidget& widget) const
{
    RECT rect = widget.bounds;
    for (const auto& page : gridPages_)
    {
        if (page.id != widget.gridCell.pageId) continue;
        int halfGapX = std::max(2, page.gapX / 2);
        int halfGapY = std::max(2, page.gapY / 2);
        rect.left   -= halfGapX;
        rect.top    -= halfGapY;
        rect.right  += halfGapX;
        rect.bottom += halfGapY;
        break;
    }
    if (rect.right - rect.left > 16 && rect.bottom - rect.top > 16)
        InflateRect(&rect, -4, -4);
    return rect;
}

/**
 * @brief 获取独立窗口小部件的移动手柄矩形
 * @param widget 桌面小部件引用
 * @return 移动手柄矩形
 */
inline RECT DesktopApp::GetStandaloneWidgetMoveHandleRect(const DesktopWidget& widget) const
{
    RECT frame = GetStandaloneWidgetFrameRect(widget);
    constexpr int handleHeight = 24;
    return {
        frame.left + 4,
        std::max<LONG>(frame.top, frame.bottom - handleHeight - 2),
        frame.right - 4,
        frame.bottom - 2
    };
}

/**
 * @brief 获取独立窗口小部件的调整大小手柄矩形
 * @param widget 桌面小部件引用
 * @return 调整大小手柄矩形
 */
inline RECT DesktopApp::GetStandaloneWidgetResizeHandleRect(const DesktopWidget& widget) const
{
    RECT handle = GetStandaloneWidgetMoveHandleRect(widget);
    constexpr int handleWidth = 24;
    return {
        std::max<LONG>(handle.left, handle.right - handleWidth),
        handle.top,
        handle.right,
        handle.bottom
    };
}

/**
 * @brief 对独立窗口小部件进行命中测试
 * @param widgetIndex 小部件索引
 * @param pt 客户端坐标点
 * @return 命中类型（无/移动手柄/调整大小手柄/内容区域）
 */
inline WidgetHit DesktopApp::HitTestStandaloneWidget(size_t widgetIndex, POINT pt) const
{
    if (widgetIndex >= widgets_.size()) return WidgetHit::None;
    const DesktopWidget& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::LuaScript) return WidgetHit::None;

    RECT frame = GetStandaloneWidgetFrameRect(widget);
    if (!PtInRect(&frame, pt)) return WidgetHit::None;
    RECT resize = GetStandaloneWidgetResizeHandleRect(widget);
    if (PtInRect(&resize, pt)) return WidgetHit::ResizeHandle;
    RECT move = GetStandaloneWidgetMoveHandleRect(widget);
    if (PtInRect(&move, pt)) return WidgetHit::MoveHandle;
    return WidgetHit::Content;
}

/**
 * @brief 命中测试：查找鼠标点所在的独立小部件索引
 * @param pt 客户端坐标点
 * @return 小部件索引，未找到返回 (size_t)-1
 */
inline size_t DesktopApp::HitTestStandaloneWidgetIndex(POINT pt) const
{
    for (size_t n = widgets_.size(); n > 0; --n)
    {
        size_t i = n - 1;
        if (HitTestStandaloneWidget(i, pt) != WidgetHit::None)
            return i;
    }
    return static_cast<size_t>(-1);
}

/**
 * @brief 将宽字符串转换为 UTF-8 编码（用于 Lua 交互）
 * @param value 输入的宽字符串
 * @return UTF-8 编码的字符串
 */
inline std::string LuaWidgetWideToUtf8(const std::wstring& value)
{
    if (value.empty()) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(len), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        result.data(), len, nullptr, nullptr);
    return result;
}

/**
 * @brief 构建 Lua 桌面快照：收集所有桌面项和文件夹条目的信息
 * @param selectedOnly 是否仅包含选中的项
 * @return LuaDesktopItemInfo 向量，供 Lua 脚本使用
 */
inline std::vector<LuaDesktopItemInfo> DesktopApp::BuildLuaDesktopSnapshot(bool selectedOnly) const
{
    std::vector<LuaDesktopItemInfo> result;
    auto appendDesktopItem = [&](const DesktopItem& item, const std::wstring& source) {
        if (selectedOnly && !item.selected) return;
        LuaDesktopItemInfo info;
        info.id = LuaWidgetWideToUtf8(item.layoutKey.empty() ? item.parsingName : item.layoutKey);
        info.title = LuaWidgetWideToUtf8(item.name);
        info.path = LuaWidgetWideToUtf8(item.parsingName);
        info.source = LuaWidgetWideToUtf8(source);
        info.type = LuaWidgetWideToUtf8(item.typeName.empty() ? L"desktopItem" : item.typeName);
        info.selected = item.selected;
        result.push_back(std::move(info));
    };

    for (const auto& item : items_)
    {
        if (!IsItemInAnyWidget(item))
            appendDesktopItem(item, L"desktop");
    }

    for (const auto& widget : widgets_)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            for (const auto& entry : widget.folderEntries)
            {
                if (selectedOnly && !entry.selected) continue;
                LuaDesktopItemInfo info;
                info.id = LuaWidgetWideToUtf8(entry.fullPath);
                info.title = LuaWidgetWideToUtf8(entry.name);
                info.path = LuaWidgetWideToUtf8(entry.fullPath);
                info.source = LuaWidgetWideToUtf8(widget.title.empty() ? L"folderMapping" : widget.title);
                info.type = entry.isDirectory ? "folder" : "file";
                info.selected = entry.selected;
                result.push_back(std::move(info));
            }
            continue;
        }

        for (const auto& key : widget.itemKeys)
        {
            size_t idx = FindItemIndexByKey(key);
            if (idx != static_cast<size_t>(-1))
                appendDesktopItem(items_[idx], widget.title.empty() ? L"widget" : widget.title);
        }
    }
    return result;
}

/**
 * @brief Lua 调用：通过 ShellExecute 打开指定路径
 * @param path 要打开的文件或文件夹路径
 * @return 是否成功打开
 */
inline bool DesktopApp::LuaOpenPath(const std::wstring& path)
{
    if (path.empty()) return false;
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

/**
 * @brief Lua 调用：在资源管理器中选中并显示指定路径
 * @param path 要揭示的文件或文件夹路径
 * @return 是否成功执行
 */
inline bool DesktopApp::LuaRevealPath(const std::wstring& path)
{
    if (path.empty()) return false;
    std::wstring params = L"/select,\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(hwnd_, L"open", L"explorer.exe", params.c_str(), nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR>(result) > 32;
}

/**
 * @brief Lua 调用：设置指定小部件的标题
 * @param widgetId 小部件 ID
 * @param title 新标题
 */
inline void DesktopApp::LuaSetWidgetTitle(const std::wstring& widgetId, const std::wstring& title)
{
    if (title.empty()) return;
    for (auto& widget : widgets_)
    {
        if (widget.id != widgetId) continue;
        if (widget.title == title) return;
        widget.title = title;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

/**
 * @brief 开始 Lua 内联文本编辑（创建弹出式编辑框）
 * @param request 编辑请求参数（位置、文本、多行模式等）
 */
inline void DesktopApp::BeginLuaInlineTextEdit(const LuaInlineTextEditRequest& request)
{
    if (renameEdit_ != nullptr || request.widgetId.empty() || request.storageKey.empty())
        return;
    if (luaInlineEdit_ != nullptr)
        CommitLuaInlineTextEdit(false);

    size_t widgetIndex = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].id == request.widgetId && widgets_[i].type == DesktopWidgetType::LuaScript)
        {
            widgetIndex = i;
            break;
        }
    }
    if (widgetIndex == static_cast<size_t>(-1))
        return;

    RECT frame = GetStandaloneWidgetFrameRect(widgets_[widgetIndex]);
    RECT rect = {
        frame.left + request.localRect.left,
        frame.top + request.localRect.top,
        frame.left + request.localRect.right,
        frame.top + request.localRect.bottom
    };
    rect.left = std::max<LONG>(frame.left + 2, std::min<LONG>(rect.left, frame.right - 4));
    rect.top = std::max<LONG>(frame.top + 2, std::min<LONG>(rect.top, frame.bottom - 4));
    rect.right = std::min<LONG>(std::max<LONG>(rect.right, rect.left + 24), frame.right - 2);
    rect.bottom = std::min<LONG>(std::max<LONG>(rect.bottom, rect.top + 22), frame.bottom - 2);
    if (IsRectEmptyRect(rect))
        return;

    RECT screenRect = rect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    std::wstring initial = Utf8ToWide(request.text);
    DWORD style = WS_POPUP | WS_VISIBLE | ES_LEFT | ES_NOHIDESEL | ES_AUTOVSCROLL;
    if (request.multiline)
        style |= ES_MULTILINE | ES_WANTRETURN | WS_VSCROLL;
    else
        style |= ES_AUTOHSCROLL;

    luaInlineEdit_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", initial.c_str(), style,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!luaInlineEdit_)
        return;

    luaInlineEditWidgetId_ = request.widgetId;
    luaInlineEditStorageKey_ = request.storageKey;
    luaInlineEditMultiline_ = request.multiline;
    luaInlineEditTextColor_ = RGB((request.textColor >> 16) & 0xFF,
        (request.textColor >> 8) & 0xFF, request.textColor & 0xFF);

    if (luaInlineEditFont_) DeleteObject(luaInlineEditFont_);
    luaInlineEditFont_ = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(luaInlineEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(luaInlineEditFont_ ? luaInlineEditFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(luaInlineEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELPARAM(8, 8));
    SetWindowSubclass(luaInlineEdit_, &DesktopApp::LuaInlineEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(luaInlineEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    if (request.selectAll)
        SendMessageW(luaInlineEdit_, EM_SETSEL, 0, -1);
    else
        SendMessageW(luaInlineEdit_, EM_SETSEL,
            static_cast<WPARAM>(-1), static_cast<LPARAM>(-1));
    SetFocus(luaInlineEdit_);
}

/**
 * @brief 判断鼠标点是否位于任意小部件的装饰区域（含独立小部件）
 * @param pt 客户端坐标点
 * @return 若在小部件装饰区上返回 true
 */
inline bool DesktopApp::IsPointOverWidgetChrome(POINT pt) const
{
    for (auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc) continue;
        RECT frame = wc->GetFrameRect();
        if (!IsRectEmptyRect(frame) && PtInRect(&frame, pt))
            return true;
    }
    return HitTestStandaloneWidgetIndex(pt) != static_cast<size_t>(-1);
}

/**
 * @brief 使拖拽静态场景失效（更新拖拽渲染缓存）
 */
inline void DesktopApp::InvalidateDragStaticScene()
{
    if (!dragSession_.IsActive()) return;
    dragSession_.InvalidateStaticScene();
    dragRenderCache_.Reset();
}

/**
 * @brief 结束当前拖拽会话，重置拖拽渲染缓存
 */
inline void DesktopApp::EndDragSession()
{
    dragSession_.End();
    dragRenderCache_.Reset();
}

/**
 * @brief 显示设置窗口
 */
inline void DesktopApp::ShowSettingsWindow()
{
    if (settingsWindow_)
        settingsWindow_->Show();
    else
        MessageBeep(MB_ICONWARNING);
}

/**
 * @brief 加载导航设置并应用热键注册
 */
inline void DesktopApp::LoadNavigationSettingsAndApply()
{
    NavigationSettings settings;
    LoadNavigationSettings(GetNavigationSettingsPath().c_str(), settings);
    navigationSettings_ = settings;
    ApplyNavigationHotkey();
}

/**
 * @brief 注销快捷导航热键
 */
inline void DesktopApp::UnregisterNavigationHotkey()
{
    if (navigationHotkeyRegistered_ && hwnd_)
    {
        UnregisterHotKey(hwnd_, kQuickNavigationHotkeyId);
        navigationHotkeyRegistered_ = false;
    }
}

/**
 * @brief 创建快捷导航窗口（若已存在则直接返回）
 * @return 窗口创建是否成功
 */
inline bool DesktopApp::CreateQuickNavigationWindow()
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        return true;

    quickNavigationHwnd_ = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kQuickNavigationWindowClassName,
L"SnowDesktop 快捷导航",
        WS_POPUP | WS_CLIPCHILDREN,
        0, 0, 1, 1,
        nullptr, nullptr, instance_, this);
    if (!quickNavigationHwnd_)
        return false;

    return true;
}

/**
 * @brief 销毁快捷导航窗口及其子控件
 */
inline void DesktopApp::DestroyQuickNavigationWindow()
{
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        RemoveWindowSubclass(quickNavigationSearchEdit_, &DesktopApp::QuickNavigationSearchSubclassProc, 1);
        DestroyWindow(quickNavigationSearchEdit_);
    }
    quickNavigationSearchEdit_ = nullptr;
    if (quickNavigationSearchFont_)
    {
        DeleteObject(quickNavigationSearchFont_);
        quickNavigationSearchFont_ = nullptr;
    }
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        DestroyWindow(quickNavigationHwnd_);
    quickNavigationHwnd_ = nullptr;
}

/**
 * @brief 确保快捷导航的搜索编辑框已创建
 */
inline void DesktopApp::EnsureQuickNavigationSearchEdit()
{
    if (!quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return;
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
        return;

    quickNavigationSearchEdit_ = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 1, 1, quickNavigationHwnd_, reinterpret_cast<HMENU>(1002),
        instance_, nullptr);
    if (!quickNavigationSearchEdit_)
        return;

    quickNavigationSearchFont_ = CreateFontW(-QuickNavScale(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    SendMessageW(quickNavigationSearchEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(quickNavigationSearchFont_ ? quickNavigationSearchFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    SendMessageW(quickNavigationSearchEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(QuickNavScale(10), QuickNavScale(10)));
    SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"搜索应用、桌面文件、映射文件夹..."));
    SetWindowSubclass(quickNavigationSearchEdit_, &DesktopApp::QuickNavigationSearchSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
}

/**
 * @brief 更新快捷导航搜索编辑框的位置和大小
 */
inline void DesktopApp::UpdateQuickNavigationSearchEditRect()
{
    if (!quickNavigationSearchEdit_ || !IsWindow(quickNavigationSearchEdit_))
        return;
    RECT search = GetQuickNavigationSearchRect(quickNavigationRect_);
    OffsetRect(&search, -quickNavigationRect_.left, -quickNavigationRect_.top);
    SetWindowPos(quickNavigationSearchEdit_, HWND_TOP,
        search.left + QuickNavScale(4), search.top + QuickNavScale(6),
        std::max<LONG>(1, search.right - search.left - QuickNavScale(8)),
        std::max<LONG>(1, search.bottom - search.top - QuickNavScale(10)),
        SWP_SHOWWINDOW);
}

/**
 * @brief 刷新快捷导航搜索文本内容（从编辑框读取）
 */
inline void DesktopApp::RefreshQuickNavigationSearchText()
{
    quickNavigationSearchText_.clear();
    if (!quickNavigationSearchEdit_ || !IsWindow(quickNavigationSearchEdit_))
        return;
    int len = GetWindowTextLengthW(quickNavigationSearchEdit_);
    if (len <= 0) return;
    std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
    GetWindowTextW(quickNavigationSearchEdit_, buffer.data(), len + 1);
    buffer.resize(static_cast<size_t>(len));
    quickNavigationSearchText_ = std::move(buffer);
}

/**
 * @brief 定位并显示快捷导航窗口（含圆角区域设置）
 */
inline void DesktopApp::PositionQuickNavigationWindow()
{
    if (!quickNavigationHwnd_ || !IsWindow(quickNavigationHwnd_))
        return;

    quickNavigationRect_ = GetQuickNavigationRect();
    const int width = std::max<LONG>(1, quickNavigationRect_.right - quickNavigationRect_.left);
    const int height = std::max<LONG>(1, quickNavigationRect_.bottom - quickNavigationRect_.top);

    // Prefer compositor-provided corners. Unlike a window region, DWM corners
    // retain partial-alpha edge pixels and therefore do not look stair-stepped.
    SetWindowRgn(quickNavigationHwnd_, nullptr, FALSE);
    const DWM_WINDOW_CORNER_PREFERENCE cornerPreference = DWMWCP_ROUND;
    const HRESULT cornerResult = DwmSetWindowAttribute(quickNavigationHwnd_,
        DWMWA_WINDOW_CORNER_PREFERENCE, &cornerPreference, sizeof(cornerPreference));
    if (FAILED(cornerResult))
    {
        // Older Windows versions do not expose DWM corner preferences.
        HRGN region = CreateRoundRectRgn(0, 0, width + 1, height + 1,
            QuickNavScale(16), QuickNavScale(16));
        if (region)
            SetWindowRgn(quickNavigationHwnd_, region, TRUE);
    }

    SetWindowPos(quickNavigationHwnd_, HWND_TOPMOST,
        quickNavigationRect_.left + virtualLeft_,
        quickNavigationRect_.top + virtualTop_,
        width, height,
        SWP_SHOWWINDOW);
    EnsureQuickNavigationSearchEdit();
    UpdateQuickNavigationSearchEditRect();
}

/**
 * @brief 使快捷导航窗口失效并触发重绘
 */
inline void DesktopApp::InvalidateQuickNavigationWindow()
{
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        InvalidateRect(quickNavigationHwnd_, nullptr, FALSE);
}

/**
 * @brief 应用并注册导航热键
 */
inline void DesktopApp::ApplyNavigationHotkey()
{
    UnregisterNavigationHotkey();
    if (!hwnd_ || !navigationSettings_.enabled || navigationSettings_.virtualKey == 0)
        return;

    UINT modifiers = navigationSettings_.modifiers | MOD_NOREPEAT;
    navigationHotkeyRegistered_ =
        RegisterHotKey(hwnd_, kQuickNavigationHotkeyId, modifiers, navigationSettings_.virtualKey) != FALSE;
}

/**
 * @brief 打开快捷导航面板
 */
inline void DesktopApp::OpenQuickNavigation()
{
    if (dragSession_.IsActive() || externalDragActive_)
        return;
    CloseCollectionPopup();
    POINT cursor{};
    if (GetCursorPos(&cursor))
    {
        quickNavigationOpenPoint_ = { cursor.x - virtualLeft_, cursor.y - virtualTop_ };
        lastMousePoint_ = quickNavigationOpenPoint_;
        HMONITOR monitor = MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
        UINT dpiX = 96, dpiY = 96;
        if (monitor)
            GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
        quickNavDpiScale_ = static_cast<float>(dpiX) / 96.0f;
    }
    else
    {
        quickNavigationOpenPoint_ = lastMousePoint_;
    }
    quickNavigationOpen_ = true;
    EnsureNavTabOrder();
    if (quickNavigationActiveWidgetIndex_ >= widgets_.size() ||
        widgets_[quickNavigationActiveWidgetIndex_].type != DesktopWidgetType::Collection)
    {
        quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
    }
    quickNavigationScrollOffset_ = 0;
    quickNavigationTabScrollOffset_ = 0;
    quickNavigationSearchText_.clear();
    if (!CreateQuickNavigationWindow())
    {
        quickNavigationOpen_ = false;
        MessageBeep(MB_ICONWARNING);
        return;
    }
    PositionQuickNavigationWindow();
    if (quickNavigationSearchEdit_)
        SetWindowTextW(quickNavigationSearchEdit_, L"");
    ShowWindow(quickNavigationHwnd_, SW_SHOWNA);
    AnimateWindow(quickNavigationHwnd_, 160, AW_BLEND);
    SetForegroundWindow(quickNavigationHwnd_);
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        SetFocus(quickNavigationSearchEdit_);
        SendMessageW(quickNavigationSearchEdit_, EM_SETSEL, 0, -1);
    }
    else
    {
        SetFocus(quickNavigationHwnd_);
    }
    InvalidateQuickNavigationWindow();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 关闭快捷导航面板
 */
inline void DesktopApp::CloseQuickNavigation()
{
    if (!quickNavigationOpen_) return;
    quickNavigationOpen_ = false;
    quickNavigationScrollOffset_ = 0;
    quickNavigationTabScrollOffset_ = 0;
    quickNavigationSearchText_.clear();
    quickNavigationRect_ = {};
    quickNavTabDragIndex_ = static_cast<size_t>(-1);
    quickNavTabDragDeltaX_ = 0;
    quickNavTabDragging_ = false;
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        AnimateWindow(quickNavigationHwnd_, 120, AW_BLEND | AW_HIDE);
    DestroyQuickNavigationWindow();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 切换快捷导航面板的打开/关闭状态
 */
inline void DesktopApp::ToggleQuickNavigation()
{
    if (quickNavigationOpen_)
        CloseQuickNavigation();
    else
        OpenQuickNavigation();
}

/**
 * @brief 处理快捷导航面板内的点击事件
 * @param point 点击坐标（客户端坐标）
 * @return 是否已处理
 */
inline bool DesktopApp::HandleQuickNavigationClick(POINT point)
{
    if (!quickNavigationOpen_)
        return false;

    RECT overlay = GetQuickNavigationRect();
    if (!PtInRect(&overlay, point))
    {
        CloseQuickNavigation();
        return true;
    }

    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    if (quickNavigationSearchText_.empty())
    {
        RECT tab0Rect = GetQuickNavigationTabRect(overlay, 0);
        if (PtInRect(&tab0Rect, point))
        {
            quickNavigationActiveWidgetIndex_ = static_cast<size_t>(-1);
            quickNavigationScrollOffset_ = 0;
            InvalidateDragStaticScene();
            InvalidateQuickNavigationWindow();
            return true;
        }
    }

    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return true;

    std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
    for (size_t i = 0; i < entries.size(); ++i)
    {
        RECT itemRect = GetQuickNavigationItemRect(overlay, i);
        RECT clipped = itemRect;
        clipped.top = std::max(clipped.top, content.top);
        clipped.bottom = std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point)) continue;

        const QuickNavigationEntry entry = std::move(entries[i]);
        CloseQuickNavigation();
        if (entry.kind == QuickNavigationEntry::Kind::DesktopItem &&
            entry.itemIndex != static_cast<size_t>(-1) && entry.itemIndex < items_.size())
        {
            ShellExecuteW(nullptr, L"open", items_[entry.itemIndex].parsingName.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        }
        else if (entry.kind == QuickNavigationEntry::Kind::FolderEntry && !entry.path.empty())
        {
            ShellExecuteW(nullptr, L"open", entry.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        return true;
    }

    return true;
}

/**
 * @brief 绘制快捷导航窗口（含搜索栏、标签页、列表、滚动条）
 * @param hwnd 窗口句柄
 */
inline void DesktopApp::PaintQuickNavigationWindow(HWND hwnd)
{
    PAINTSTRUCT ps{};
    HDC hdc = BeginPaint(hwnd, &ps);
    if (!hdc)
        return;

    RECT client{};
    GetClientRect(hwnd, &client);
    HDC memoryDc = CreateCompatibleDC(hdc);
    HBITMAP memoryBitmap = CreateCompatibleBitmap(hdc,
        std::max<LONG>(1, client.right - client.left),
        std::max<LONG>(1, client.bottom - client.top));
    HBITMAP oldBitmap = static_cast<HBITMAP>(SelectObject(memoryDc, memoryBitmap));
    SetBkMode(memoryDc, TRANSPARENT);
    SetStretchBltMode(memoryDc, HALFTONE);

    ComPtr<ID2D1DCRenderTarget> dcRenderTarget;
    if (d2dFactory_)
    {
        const D2D1_RENDER_TARGET_PROPERTIES properties = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE),
            0.0f, 0.0f,
            D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE);
        if (SUCCEEDED(d2dFactory_->CreateDCRenderTarget(&properties, &dcRenderTarget)))
        {
            if (FAILED(dcRenderTarget->BindDC(memoryDc, &client)))
                dcRenderTarget.Reset();
        }
    }

    auto offsetRect = [&](RECT rect) {
        OffsetRect(&rect, -quickNavigationRect_.left, -quickNavigationRect_.top);
        return rect;
    };
    auto fillRound = [&](RECT rect, COLORREF fill, COLORREF stroke, int radius) {
        if (dcRenderTarget)
        {
            ComPtr<ID2D1SolidColorBrush> fillBrush;
            ComPtr<ID2D1SolidColorBrush> strokeBrush;
            const D2D1_COLOR_F fillColor = D2D1::ColorF(
                GetRValue(fill) / 255.0f, GetGValue(fill) / 255.0f, GetBValue(fill) / 255.0f);
            const D2D1_COLOR_F strokeColor = D2D1::ColorF(
                GetRValue(stroke) / 255.0f, GetGValue(stroke) / 255.0f, GetBValue(stroke) / 255.0f);
            if (SUCCEEDED(dcRenderTarget->CreateSolidColorBrush(fillColor, &fillBrush)) &&
                SUCCEEDED(dcRenderTarget->CreateSolidColorBrush(strokeColor, &strokeBrush)))
            {
                // GDI RoundRect receives the corner ellipse diameter; Direct2D
                // receives the radius, hence the 0.5 conversion.
                const float cornerRadius = std::max(0.5f, radius * 0.5f);
                const D2D1_ROUNDED_RECT rounded = D2D1::RoundedRect(
                    D2D1::RectF(
                        static_cast<float>(rect.left) + 0.5f,
                        static_cast<float>(rect.top) + 0.5f,
                        static_cast<float>(rect.right) - 0.5f,
                        static_cast<float>(rect.bottom) - 0.5f),
                    cornerRadius, cornerRadius);
                dcRenderTarget->BeginDraw();
                dcRenderTarget->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
                dcRenderTarget->FillRoundedRectangle(rounded, fillBrush.Get());
                dcRenderTarget->DrawRoundedRectangle(rounded, strokeBrush.Get(), 1.0f);
                if (SUCCEEDED(dcRenderTarget->EndDraw()))
                    return;
                dcRenderTarget.Reset();
            }
        }

        HBRUSH brush = CreateSolidBrush(fill);
        HPEN pen = CreatePen(PS_SOLID, 1, stroke);
        HGDIOBJ oldBrush = SelectObject(memoryDc, brush);
        HGDIOBJ oldPen = SelectObject(memoryDc, pen);
        RoundRect(memoryDc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
        SelectObject(memoryDc, oldPen);
        SelectObject(memoryDc, oldBrush);
        DeleteObject(pen);
        DeleteObject(brush);
    };
    auto drawText = [&](const std::wstring& text, RECT rect, HFONT font, COLORREF color, UINT flags) {
        HGDIOBJ oldFont = SelectObject(memoryDc, font);
        SetTextColor(memoryDc, color);
        DrawTextW(memoryDc, text.c_str(), static_cast<int>(text.size()), &rect, flags);
        SelectObject(memoryDc, oldFont);
    };
    auto drawBitmap = [&](HBITMAP bitmap, RECT dst) {
        if (!bitmap) return;
        BITMAP bm{};
        if (GetObjectW(bitmap, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight <= 0)
            return;

        const int boundsWidth = std::max<LONG>(1, dst.right - dst.left);
        const int boundsHeight = std::max<LONG>(1, dst.bottom - dst.top);
        const double scale = std::min(
            static_cast<double>(boundsWidth) / bm.bmWidth,
            static_cast<double>(boundsHeight) / bm.bmHeight);
        const int drawWidth = std::clamp(
            static_cast<int>(std::round(bm.bmWidth * scale)), 1, boundsWidth);
        const int drawHeight = std::clamp(
            static_cast<int>(std::round(bm.bmHeight * scale)), 1, boundsHeight);
        const int drawX = dst.left + (boundsWidth - drawWidth) / 2;
        const int drawY = dst.top + (boundsHeight - drawHeight) / 2;

        HDC srcDc = CreateCompatibleDC(memoryDc);
        HBITMAP oldSrc = static_cast<HBITMAP>(SelectObject(srcDc, bitmap));
        const int oldStretchMode = SetStretchBltMode(memoryDc, COLORONCOLOR);
        BLENDFUNCTION blend{ AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
        AlphaBlend(memoryDc, drawX, drawY, drawWidth, drawHeight,
            srcDc, 0, 0, bm.bmWidth, bm.bmHeight, blend);
        SetStretchBltMode(memoryDc, oldStretchMode);
        SelectObject(srcDc, oldSrc);
        DeleteDC(srcDc);
    };

    HBRUSH background = CreateSolidBrush(RGB(18, 22, 30));
    FillRect(memoryDc, &client, background);
    DeleteObject(background);
    fillRound(MakeRect(client.left, client.top, client.right - 1, client.bottom - 1),
        RGB(18, 22, 30), RGB(120, 130, 150), QuickNavScale(16));

    HFONT titleFont = CreateFontW(-QuickNavScale(18), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT tabFont = CreateFontW(-QuickNavScale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");
    HFONT itemFont = CreateFontW(-QuickNavScale(13), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_SWISS, L"Segoe UI");

    std::vector<size_t> collectionIndices = GetQuickNavigationCollectionIndices();
    std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
    quickNavigationTabScrollOffset_ = std::clamp(quickNavigationTabScrollOffset_, 0,
        GetQuickNavigationMaxTabScrollOffset(quickNavigationRect_));
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(quickNavigationRect_));

    RECT titleRect = offsetRect(MakeRect(quickNavigationRect_.left + QuickNavScale(24), quickNavigationRect_.top + QuickNavScale(18),
        quickNavigationRect_.right - QuickNavScale(24), quickNavigationRect_.top + QuickNavScale(46)));
    std::wstring title = L"快捷导航";
    if (!entries.empty())
        title += L"  " + std::to_wstring(entries.size()) + L" 项";
    drawText(title, titleRect, titleFont, RGB(245, 248, 252),
        DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

    RECT searchRect = offsetRect(GetQuickNavigationSearchRect(quickNavigationRect_));
    fillRound(searchRect, RGB(255, 255, 255), RGB(92, 105, 128), QuickNavScale(12));

    if (quickNavigationSearchText_.empty())
    {
        RECT tabs = offsetRect(GetQuickNavigationTabsRect(quickNavigationRect_));
        SaveDC(memoryDc);
        IntersectClipRect(memoryDc, tabs.left, tabs.top, tabs.right, tabs.bottom);

        const size_t tabCount = collectionIndices.size() + 1;
        const int tabWidth = GetQuickNavigationTabWidth();
        const int gap = QuickNavScale(8);

        auto calcTabPosX = [&](size_t tabIdx) -> int {
            return tabs.left + static_cast<int>(tabIdx) * (tabWidth + gap) - quickNavigationTabScrollOffset_;
        };

        int dragTargetTab = -1;
        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            int unit = tabWidth + gap;
            dragTargetTab = static_cast<int>(quickNavTabDragIndex_) +
                quickNavTabDragDeltaX_ / unit;
            if (dragTargetTab < 1) dragTargetTab = 1;
            if (dragTargetTab > static_cast<int>(collectionIndices.size())) dragTargetTab = static_cast<int>(collectionIndices.size());
        }

        auto drawTab = [&](size_t tab, int offsetX) {
            int posX = calcTabPosX(tab) + offsetX;
            RECT tabRect = MakeRect(posX, tabs.top, posX + tabWidth, tabs.bottom);
            if (tabRect.right <= tabs.left || tabRect.left >= tabs.right) return;
            tabRect.left = std::max(tabRect.left, tabs.left);
            tabRect.right = std::min(tabRect.right, tabs.right);

            const bool active = tab == 0
                ? quickNavigationActiveWidgetIndex_ == static_cast<size_t>(-1)
                : quickNavigationActiveWidgetIndex_ == collectionIndices[tab - 1];
            bool hovered = false;
            RECT tabRectApp = MakeRect(posX, tabs.top, posX + tabWidth, tabs.bottom);
            if (!quickNavTabDragging_)
                hovered = PtInRect(&tabRectApp, lastMousePoint_) != FALSE;

            COLORREF fill, stroke;
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
            {
                fill = RGB(80, 92, 112);
                stroke = RGB(120, 140, 180);
            }
            else
            {
                fill = active ? RGB(48, 112, 215) : hovered ? RGB(66, 72, 84) : RGB(42, 47, 58);
                stroke = active ? RGB(82, 140, 235) : RGB(68, 76, 92);
            }
            fillRound(tabRect, fill, stroke, QuickNavScale(14));

            std::wstring label = L"全部";
            if (tab > 0)
            {
                const DesktopWidget& widget = widgets_[collectionIndices[tab - 1]];
                label = widget.title.empty() ? L"集合" + std::to_wstring(tab) : widget.title;
            }
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            drawText(label, textRect, tabFont, RGB(245, 248, 252),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        };

        for (size_t tab = 0; tab < tabCount; ++tab)
        {
            if (quickNavTabDragging_ && tab == quickNavTabDragIndex_)
                continue;
            int offsetX = 0;
            if (quickNavTabDragging_ && dragTargetTab >= 1)
            {
                size_t src = quickNavTabDragIndex_;
                int dst = dragTargetTab;
                int cur = static_cast<int>(tab);
                if (cur > src && cur <= dst) offsetX = -(tabWidth + gap);
                else if (cur < src && cur >= dst) offsetX = tabWidth + gap;
            }
            drawTab(tab, offsetX);
        }

        if (quickNavTabDragging_ && quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            int posX = calcTabPosX(quickNavTabDragIndex_) + quickNavTabDragDeltaX_;
            RECT tabRect = MakeRect(posX, tabs.top, posX + tabWidth, tabs.bottom);
            tabRect.left = std::max(tabRect.left, tabs.left);
            tabRect.right = std::min(tabRect.right, tabs.right);
            fillRound(tabRect, RGB(60, 80, 110), RGB(100, 130, 200), QuickNavScale(14));

            std::wstring label = L"全部";
            if (quickNavTabDragIndex_ > 0)
            {
                const DesktopWidget& widget = widgets_[collectionIndices[quickNavTabDragIndex_ - 1]];
                label = widget.title.empty() ? L"集合" + std::to_wstring(quickNavTabDragIndex_) : widget.title;
            }
            RECT textRect = tabRect;
            textRect.left += QuickNavScale(8);
            textRect.right -= QuickNavScale(8);
            drawText(label, textRect, tabFont, RGB(245, 248, 252),
                DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (dragTargetTab >= 1 && static_cast<size_t>(dragTargetTab) <= collectionIndices.size())
            {
                int insertX = calcTabPosX(static_cast<size_t>(dragTargetTab));
                HPEN indicatorPen = CreatePen(PS_SOLID, QuickNavScale(2), RGB(82, 140, 235));
                HGDIOBJ savedPen = SelectObject(memoryDc, indicatorPen);
                MoveToEx(memoryDc, insertX - gap / 2, tabs.top + QuickNavScale(4), nullptr);
                LineTo(memoryDc, insertX - gap / 2, tabs.bottom - QuickNavScale(4));
                SelectObject(memoryDc, savedPen);
                DeleteObject(indicatorPen);
            }
        }

        RestoreDC(memoryDc, -1);
    }

    RECT content = offsetRect(GetQuickNavigationContentRect(quickNavigationRect_));
    SaveDC(memoryDc);
    IntersectClipRect(memoryDc, content.left, content.top, content.right, content.bottom);
    if (entries.empty())
    {
        RECT emptyRect = content;
        emptyRect.top += QuickNavScale(28);
        drawText(quickNavigationSearchText_.empty()
                ? (collectionIndices.empty() ? L"暂无集合组件" : L"当前分类暂无项目")
                : L"没有匹配结果",
            emptyRect, itemFont, RGB(160, 168, 182), DT_CENTER | DT_TOP | DT_SINGLELINE);
    }
    else
    {
        for (size_t i = 0; i < entries.size(); ++i)
        {
            RECT itemRectApp = GetQuickNavigationItemRect(quickNavigationRect_, i);
            if (itemRectApp.bottom <= quickNavigationRect_.top ||
                itemRectApp.top >= quickNavigationRect_.bottom)
                continue;

            const QuickNavigationEntry& entry = entries[i];

            const int cellW = itemRectApp.right - itemRectApp.left;
            const int cellH = itemRectApp.bottom - itemRectApp.top;
            const int maxIconW = std::max(QuickNavScale(16), cellW - QuickNavScale(8));
            const int maxIconH = std::max(QuickNavScale(16), cellH - QuickNavScale(kQuickNavigationTextHeight) - QuickNavScale(8));
            const int iconSz = std::min(maxIconW, maxIconH);
            const int iconX = itemRectApp.left + (cellW - iconSz) / 2;
            const int iconY = itemRectApp.top + QuickNavScale(2);
            RECT iconRect = MakeRect(iconX, iconY, iconX + iconSz, iconY + iconSz);
            const int textTop = iconRect.bottom + QuickNavScale(2);
            RECT textRect = MakeRect(itemRectApp.left + QuickNavScale(4), textTop,
                itemRectApp.right - QuickNavScale(4), textTop + QuickNavScale(kQuickNavigationTextHeight));
            RECT selRect = textRect;
            selRect.top = std::max(itemRectApp.top, iconRect.top - QuickNavScale(2));
            selRect.left = itemRectApp.left + QuickNavScale(3);
            selRect.right = itemRectApp.right - QuickNavScale(3);
            selRect.bottom = std::min(itemRectApp.bottom - QuickNavScale(2), textRect.bottom);

            if (PtInRect(&itemRectApp, lastMousePoint_) != FALSE)
                fillRound(offsetRect(selRect), RGB(58, 68, 86), RGB(78, 92, 118), QuickNavScale(12));

            drawBitmap(entry.iconBitmap, offsetRect(iconRect));

            drawText(entry.name, offsetRect(textRect), itemFont, RGB(245, 248, 252),
                DT_CENTER | DT_TOP | DT_WORDBREAK | DT_END_ELLIPSIS);
        }
    }
    RestoreDC(memoryDc, -1);

    const int columns = GetQuickNavigationColumnCount(quickNavigationRect_);
    const int rows = entries.empty() ? 1 : (static_cast<int>(entries.size()) + columns - 1) / columns;
    const int contentHeight = rows * QuickNavScale(kQuickNavigationCellHeight);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    if (contentHeight > visibleHeight)
    {
        const int trackW = QuickNavScale(5);
        RECT track = MakeRect(content.right - trackW - QuickNavScale(2), content.top + QuickNavScale(4),
            content.right - QuickNavScale(2), content.bottom - QuickNavScale(4));
        fillRound(track, RGB(55, 62, 76), RGB(55, 62, 76), trackW);
        const int trackH = std::max<LONG>(1, track.bottom - track.top);
        const int thumbH = std::clamp(visibleHeight * trackH / contentHeight, QuickNavScale(20), trackH);
        const int maxScroll = std::max(1, contentHeight - visibleHeight);
        const int thumbTop = track.top + quickNavigationScrollOffset_ * (trackH - thumbH) / maxScroll;
        RECT thumb = MakeRect(track.left, thumbTop, track.right, thumbTop + thumbH);
        fillRound(thumb, RGB(136, 146, 166), RGB(136, 146, 166), trackW);
    }

    BitBlt(hdc, 0, 0, client.right - client.left, client.bottom - client.top, memoryDc, 0, 0, SRCCOPY);

    DeleteObject(itemFont);
    DeleteObject(tabFont);
    DeleteObject(titleFont);
    SelectObject(memoryDc, oldBitmap);
    DeleteObject(memoryBitmap);
    DeleteDC(memoryDc);
    EndPaint(hwnd, &ps);
}

/**
 * @brief 快捷导航窗口的消息处理函数
 * @param hwnd 窗口句柄
 * @param msg 消息 ID
 * @param wp wParam
 * @param lp lParam
 * @return 消息处理结果
 */
inline LRESULT DesktopApp::HandleQuickNavigationMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg)
    {
    case WM_PAINT:
        PaintQuickNavigationWindow(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDOWN:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{ pt.x + quickNavigationRect_.left, pt.y + quickNavigationRect_.top };

        if (quickNavigationSearchText_.empty())
        {
            RECT overlay = quickNavigationRect_;
            std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
            for (size_t tab = 1; tab <= ci.size(); ++tab)
            {
                RECT tabRect = GetQuickNavigationTabRect(overlay, tab);
                if (PtInRect(&tabRect, appPoint))
                {
                    quickNavTabDragIndex_ = tab;
                    quickNavTabDragStartPoint_ = appPoint;
                    quickNavTabDragDeltaX_ = 0;
                    quickNavTabDragging_ = false;
                    SetCapture(hwnd);
                    return 0;
                }
            }
        }

        HandleQuickNavigationClick(appPoint);
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        POINT appPoint{ pt.x + quickNavigationRect_.left, pt.y + quickNavigationRect_.top };
        lastMousePoint_ = appPoint;

        if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            int dx = appPoint.x - quickNavTabDragStartPoint_.x;
            if (!quickNavTabDragging_ && std::abs(dx) > 4)
                quickNavTabDragging_ = true;
            if (quickNavTabDragging_)
                quickNavTabDragDeltaX_ = dx;
            InvalidateQuickNavigationWindow();
            InvalidateDragStaticScene();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        InvalidateQuickNavigationWindow();
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
        {
            ReleaseCapture();
            size_t dragTab = quickNavTabDragIndex_;

            if (quickNavTabDragging_)
            {
                std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
                size_t tabCount = ci.size();
                int tabWidth = GetQuickNavigationTabWidth();
                int gap = QuickNavScale(8);
                int unit = tabWidth + gap;
                int targetTab = static_cast<int>(dragTab) + quickNavTabDragDeltaX_ / unit;
                targetTab = std::clamp(targetTab, 1, static_cast<int>(tabCount));

                if (targetTab != static_cast<int>(dragTab) && targetTab >= 1 && static_cast<size_t>(targetTab) <= tabCount)
                {
                    size_t srcIdx = dragTab - 1;
                    size_t dstIdx = static_cast<size_t>(targetTab) - 1;
                    EnsureNavTabOrder();

                    if (srcIdx < navTabOrder_.size() && dstIdx < navTabOrder_.size())
                    {
                        std::wstring id = navTabOrder_[srcIdx];
                        navTabOrder_.erase(navTabOrder_.begin() + srcIdx);
                        navTabOrder_.insert(navTabOrder_.begin() + dstIdx, id);
                        quickNavigationActiveWidgetIndex_ = ci[dstIdx];
                        quickNavigationScrollOffset_ = 0;
                        SaveLayoutSlots();
                    }
                }
            }
            else
            {
                std::vector<size_t> ci = GetQuickNavigationCollectionIndices();
                if (dragTab - 1 < ci.size())
                {
                    quickNavigationActiveWidgetIndex_ = ci[dragTab - 1];
                    quickNavigationScrollOffset_ = 0;
                }
            }

            quickNavTabDragIndex_ = static_cast<size_t>(-1);
            quickNavTabDragDeltaX_ = 0;
            quickNavTabDragging_ = false;
            InvalidateQuickNavigationWindow();
            InvalidateDragStaticScene();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return 0;
        }
        break;
    }
    case WM_MOUSEWHEEL:
        OnMouseWheel(wp, lp);
        return 0;
    case WM_COMMAND:
        if (reinterpret_cast<HWND>(lp) == quickNavigationSearchEdit_ && HIWORD(wp) == EN_CHANGE)
        {
            RefreshQuickNavigationSearchText();
            quickNavigationScrollOffset_ = 0;
            InvalidateQuickNavigationWindow();
            return 0;
        }
        break;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE)
        {
            if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
            {
                ReleaseCapture();
                quickNavTabDragIndex_ = static_cast<size_t>(-1);
                quickNavTabDragDeltaX_ = 0;
                quickNavTabDragging_ = false;
                InvalidateQuickNavigationWindow();
                InvalidateDragStaticScene();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return 0;
            }
            CloseQuickNavigation();
            return 0;
        }
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE)
        {
            if (quickNavTabDragIndex_ != static_cast<size_t>(-1))
            {
                ReleaseCapture();
                quickNavTabDragIndex_ = static_cast<size_t>(-1);
                quickNavTabDragDeltaX_ = 0;
                quickNavTabDragging_ = false;
            }
            CloseQuickNavigation();
            return 0;
        }
        break;
    case WM_CLOSE:
        CloseQuickNavigation();
        return 0;
    case WM_DESTROY:
        if (quickNavigationHwnd_ == hwnd)
            quickNavigationHwnd_ = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 快捷导航搜索编辑框的子类化窗口过程
 * @param hwnd 编辑框句柄
 * @param message 消息 ID
 * @param wParam wParam
 * @param lParam lParam
 * @param subclassId 子类化 ID
 * @param refData 引用数据（指向 DesktopApp 实例）
 * @return 消息处理结果
 */
inline LRESULT CALLBACK DesktopApp::QuickNavigationSearchSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    if (message == WM_KEYDOWN && wParam == VK_ESCAPE)
    {
        app->CloseQuickNavigation();
        return 0;
    }
    if (message == WM_MOUSEWHEEL)
    {
        app->OnMouseWheel(wParam, lParam);
        return 0;
    }

    return DefSubclassProc(hwnd, message, wParam, lParam);
}

/**
 * @brief 刷新拖拽目标：根据鼠标位置更新目标容器、槽位和区域
 * @param clientPoint 客户端坐标点
 * @param mods 修饰键状态
 */
inline void DesktopApp::RefreshDragTargetAt(POINT clientPoint, int mods)
{
    if (!dragSession_.IsActive()) return;

    dragSession_.UpdatePoint(clientPoint);

    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    popupDragTargetSlot_.reset();

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, clientPoint))
        {
            WidgetContainer* popupContainer = nullptr;
            for (auto& c : containers_)
            {
                popupContainer = dynamic_cast<WidgetContainer*>(c.get());
                if (popupContainer && popupContainer->GetWidgetData() == &widgets_[popupWidgetIndex_])
                    break;
                popupContainer = nullptr;
            }

            if (popupContainer)
            {
                std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                RECT content = GetCollectionPopupContentRect(popup);
                size_t slotIndex = 0;
                RECT slotBounds = content;
                HitRegion region = HitRegion::Empty;
                Item* handoffItem = nullptr;

                if (!popupKeys.empty())
                {
                    bool foundItem = false;
                    for (size_t i = 0; i < popupKeys.size(); ++i)
                    {
                        RECT itemRect = GetCollectionPopupItemRect(popup, i);
                        RECT clipped = itemRect;
                        clipped.top = std::max(clipped.top, content.top);
                        clipped.bottom = std::min(clipped.bottom, content.bottom);
                        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, clientPoint))
                            continue;

                        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                        if (itemIndex != static_cast<size_t>(-1) && !items_[itemIndex].selected)
                        {
                            RECT iconRect = GetItemIconRect(itemRect);
                            RECT handoffRect = { iconRect.left - 4, iconRect.top - 2,
                                                 iconRect.right + 4, iconRect.bottom + 4 };
                            if (PtInRect(&handoffRect, clientPoint))
                            {
                                region = HitRegion::Handoff;
                                slotBounds = itemRect;
                                handoffItem = popupContainer->GetMemberItem(i);
                            }
                        }

                        if (region != HitRegion::Handoff)
                        {
                            slotIndex = i;
                            slotBounds = itemRect;
                            region = clientPoint.x < itemRect.left + (itemRect.right - itemRect.left) / 2
                                ? HitRegion::SortBefore
                                : HitRegion::SortAfter;
                        }
                        foundItem = true;
                        break;
                    }

                    if (!foundItem)
                    {
                        slotIndex = popupKeys.size() - 1;
                        slotBounds = GetCollectionPopupItemRect(popup, slotIndex);
                        region = HitRegion::SortAfter;
                    }
                }

                popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
                if (handoffItem)
                    popupDragTargetSlot_->SetItem(handoffItem);
                targetContainer = popupContainer;
                targetSlot = popupDragTargetSlot_.get();
                targetRegion = region;
            }
        }
    }

    if (!targetContainer)
    {
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            Slot* slot = nullptr;
            HitRegion region = (*it)->HitTestDrag(clientPoint, slot);
            if (region != HitRegion::None)
            {
                targetContainer = it->get();
                targetSlot = slot;
                targetRegion = region;
                break;
            }
        }
    }

    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion,
            dragSession_.Items(), dragSession_.Source(), mods);
    ShowDragHintWindow(clientPoint, hint);
}

/**
 * @brief 在重建容器后重新绑定拖拽源
 * @note 用于在容器重建后恢复拖拽会话的源引用
 */
inline void DesktopApp::RebindDragSourceAfterRebuild()
{
    if (!dragSession_.IsActive() || dragSession_.Items().empty()) return;

    Container* source = nullptr;
    const DragSourceList& oldSourceList = dragSession_.SourceList();
    if (oldSourceList.hasOriginWidget)
    {
        for (auto& c : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(c.get());
            DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
            if (data && data->id == oldSourceList.originWidgetId)
            {
                source = widget;
                break;
            }
        }
    }
    else
    {
        source = GetDesktopGrid();
    }

    if (!source)
    {
        EndDragSession();
        return;
    }

    std::vector<Item*> reboundItems = source->GetSelectedItems();
    if (reboundItems.empty())
    {
        EndDragSession();
        return;
    }
    DragSourceList reboundList = BuildDragSourceList(reboundItems, source);
    dragSession_.RebindSource(source, std::move(reboundItems), std::move(reboundList));
}

/**
 * @brief 清除所有桌面项、小部件和文件夹条目的选中状态
 */
inline void DesktopApp::ClearSelection()
{
    for (auto& item : items_)
        item.selected = false;
    for (auto& widget : widgets_)
    {
        widget.selected = false;
        for (auto& entry : widget.folderEntries)
            entry.selected = false;
    }
}

/**
 * @brief 清除指定小部件之外的所有选中状态
 * @param widgetIndex 保留选中的小部件索引
 */
inline void DesktopApp::ClearSelectionOutsideWidget(size_t widgetIndex)
{
    std::unordered_set<std::wstring> allowedKeys;
    if (widgetIndex < widgets_.size())
    {
        for (const auto& key : widgets_[widgetIndex].itemKeys)
            allowedKeys.insert(ToUpperInvariant(key));
    }

    for (auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!allowedKeys.contains(key))
            item.selected = false;
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        widgets_[wi].selected = false;
        if (wi == widgetIndex && widgets_[wi].type == DesktopWidgetType::FolderMapping)
            continue;
        for (auto& entry : widgets_[wi].folderEntries)
            entry.selected = false;
    }
}

/**
 * @brief 清除桌面区域之外（即小部件内）的所有选中状态
 */
inline void DesktopApp::ClearSelectionOutsideDesktop()
{
    for (auto& item : items_)
    {
        if (IsItemInAnyWidget(item))
            item.selected = false;
    }
    for (auto& widget : widgets_)
    {
        widget.selected = false;
        for (auto& entry : widget.folderEntries)
            entry.selected = false;
    }
}

/**
 * @brief 仅选中指定索引的桌面项（清除其他所有选中状态）
 * @param index 桌面项索引
 */
inline void DesktopApp::SelectOnly(int index)
{
    ClearSelection();
    if (index >= 0 && static_cast<size_t>(index) < items_.size())
    {
        // Find the OO icon for this item
        items_[index].selected = true;
    }
}

/**
 * @brief 切换指定桌面项的选中/未选中状态
 * @param index 桌面项索引
 */
inline void DesktopApp::ToggleSelection(int index)
{
    if (index >= 0 && static_cast<size_t>(index) < items_.size())
        items_[index].selected = !items_[index].selected;
}

inline int DesktopApp::GetMarqueeScrollOffset() const
{
    if (marqueeWidgetIndex_ >= widgets_.size())
        return 0;
    if (popupWidgetIndex_ == marqueeWidgetIndex_)
        return popupScrollOffset_;

    for (const auto& container : containers_)
    {
        auto* widgetContainer = dynamic_cast<WidgetContainer*>(container.get());
        if (widgetContainer &&
            widgetContainer->GetWidgetData() == &widgets_[marqueeWidgetIndex_])
        {
            return widgetContainer->GetScrollOffset();
        }
    }
    return 0;
}

inline RECT DesktopApp::GetMarqueeViewportRect() const
{
    if (marqueeWidgetIndex_ >= widgets_.size())
    {
        RECT client{};
        if (hwnd_)
            GetClientRect(hwnd_, &client);
        return client;
    }
    if (popupWidgetIndex_ == marqueeWidgetIndex_)
    {
        return GetCollectionPopupContentRect(
            GetCollectionPopupRect(widgets_[popupWidgetIndex_]));
    }

    for (const auto& container : containers_)
    {
        auto* widgetContainer = dynamic_cast<WidgetContainer*>(container.get());
        if (widgetContainer &&
            widgetContainer->GetWidgetData() == &widgets_[marqueeWidgetIndex_])
        {
            return widgetContainer->GetContentViewportRect();
        }
    }
    return {};
}

inline void DesktopApp::UpdateMarqueeSelection(POINT current)
{
    if (marqueeWidgetIndex_ < widgets_.size())
    {
        const int currentScroll = GetMarqueeScrollOffset();
        RECT viewport = GetMarqueeViewportRect();
        POINT contentAnchor{
            marqueeAnchorPoint_.x,
            marqueeAnchorPoint_.y + marqueeInitialScrollOffset_
        };
        POINT contentCurrent{
            std::clamp<LONG>(current.x, viewport.left, viewport.right),
            std::clamp<LONG>(current.y, viewport.top, viewport.bottom) + currentScroll
        };
        RECT contentSelectionRect = NormalizeRect(contentAnchor, contentCurrent);

        marqueeRect_ = contentSelectionRect;
        OffsetRect(&marqueeRect_, 0, -currentScroll);

        if (popupWidgetIndex_ == marqueeWidgetIndex_)
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            std::vector<std::wstring> popupKeys =
                GetPopupItemKeys(widgets_[popupWidgetIndex_]);
            for (size_t i = 0; i < popupKeys.size(); ++i)
            {
                RECT itemRect = GetCollectionPopupItemRect(popup, i);
                OffsetRect(&itemRect, 0, currentScroll);
                size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                if (itemIndex == static_cast<size_t>(-1))
                    continue;
                items_[itemIndex].selected =
                    RectsIntersect(itemRect, contentSelectionRect);
            }
        }
        else
        {
            for (auto& container : containers_)
            {
                auto* widgetContainer =
                    dynamic_cast<WidgetContainer*>(container.get());
                if (widgetContainer &&
                    widgetContainer->GetWidgetData() ==
                        &widgets_[marqueeWidgetIndex_])
                {
                    widgetContainer->ApplyMarqueeSelection(
                        contentSelectionRect);
                    break;
                }
            }
        }
    }
    else
    {
        marqueeRect_ = NormalizeRect(marqueeAnchorPoint_, current);
        for (auto& itemObject : items_oo_)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(itemObject.get());
            if (!icon)
                continue;
            DesktopItem* item = icon->GetDesktopItem();
            if (!item || IsItemInAnyWidget(*item) || IsRectEmptyRect(item->bounds))
                continue;
            RECT selectionRect = GetItemSelectionRect(item->bounds, false);
            item->selected = RectsIntersect(selectionRect, marqueeRect_);
        }
    }
}

/**
 * @brief 仅选中指定小部件（清除其他所有选中状态）
 * @param index 小部件索引
 */
inline void DesktopApp::SelectWidgetOnly(size_t index)
{
    ClearSelection();
    for (auto& w : widgets_)
    {
        w.selected = (&w == &widgets_[index]);
        for (auto& e : w.folderEntries) e.selected = false;
    }
}

/**
 * @brief 处理鼠标左键按下事件
 * @param wp WPARAM（含修饰键状态）
 * @param lp LPARAM（含鼠标坐标）
 * @details 处理逻辑：集合弹窗点击 -> 页面导航点击 -> 小部件点击 -> 桌面图标点击
 */
inline void DesktopApp::OnLeftButtonDown(WPARAM wp, LPARAM lp)
{
    if (renameEdit_ != nullptr) return;
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    SetFocus(hwnd_);
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    mouseDown_ = true;
    mouseDownPoint_ = pt;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeAnchorPoint_ = pt;
    marqueeInitialScrollOffset_ = 0;
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    marqueeRect_ = MakeRect(pt.x, pt.y, pt.x, pt.y);

    if (HandleQuickNavigationClick(pt))
    {
        mouseDown_ = false;
        return;
    }

    if (HandlePageNavClick(pt)) return;

    bool ctrl = (wp & MK_CONTROL) != 0;

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (!PtInRect(&popup, pt))
        {
            CloseCollectionPopup();
            mouseDown_ = false;
            return;
        }

        std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
        RECT content = GetCollectionPopupContentRect(popup);
        bool clickedPopupItem = false;
        for (size_t i = 0; i < popupKeys.size(); ++i)
        {
            RECT itemRect = GetCollectionPopupItemRect(popup, i);
            RECT clipped = itemRect;
            clipped.top = std::max(clipped.top, content.top);
            clipped.bottom = std::min(clipped.bottom, content.bottom);
            if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;

            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
            if (itemIndex != static_cast<size_t>(-1))
            {
                if (ctrl)
                {
                    ClearSelectionOutsideWidget(popupWidgetIndex_);
                    ToggleSelection(static_cast<int>(itemIndex));
                }
                else if (!items_[itemIndex].selected)
                {
                    SelectOnly(static_cast<int>(itemIndex));
                }
                else
                {
                    ClearSelectionOutsideWidget(popupWidgetIndex_);
                }
                WidgetContainer* wc = nullptr;
                for (auto& c : containers_)
                {
                    wc = dynamic_cast<WidgetContainer*>(c.get());
                    if (wc && wc->GetWidgetData() == &widgets_[popupWidgetIndex_]) break;
                    wc = nullptr;
                }
                popupMouseDownItem_ = std::make_unique<DesktopIcon>(&items_[itemIndex], wc, this);
                popupMouseDownItem_->SetBounds(itemRect);
                mouseDownHit_ = popupMouseDownItem_.get();
                clickedPopupItem = true;
            }
            break;
        }
        if (!clickedPopupItem && !ctrl)
            ClearSelection();

        if (!clickedPopupItem)
            mouseDownHit_ = nullptr;
        marqueeWidgetIndex_ = popupWidgetIndex_;
        marqueeInitialScrollOffset_ = popupScrollOffset_;
        mouseDownWidgetIndex_ = popupWidgetIndex_;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    // ── Widget hit-test ─────────────────────────────────────
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    widgetAction_ = WidgetAction::None;
    for (size_t n = widgets_.size(); n > 0; --n)
    {
        size_t wi = n - 1;
        WidgetHit wh = HitTestStandaloneWidget(wi, pt);
        if (wh == WidgetHit::None) continue;

        if (wh == WidgetHit::ResizeHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingResize;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        if (wh == WidgetHit::MoveHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingMove;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            RECT bounds = widgets_[wi].bounds;
            dragGroupOriginX_ = bounds.left;
            dragGroupOriginY_ = bounds.top;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }

        SelectWidgetOnly(wi);
        mouseDownWidgetIndex_ = wi;
        mouseDownHit_ = nullptr;
        SetCapture(hwnd_);
        if (widgetEngine_ && widgets_[wi].type == DesktopWidgetType::LuaScript)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[wi]);
            widgetEngine_->EnsureWidgetLoaded(widgets_[wi].id, widgets_[wi].scriptPath);
            int localX = pt.x - frame.left;
            int localY = pt.y - frame.top;
            if (!widgetEngine_->HandleHostUiPointer(widgets_[wi].id, localX, localY, 0, false))
                widgetEngine_->InvokeMouseEvent(widgets_[wi].id, "onMouseDown",
                    localX, localY, 1, 0);
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        WidgetContainer* wc = nullptr;
        for (auto& c : containers_)
        {
            wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[wi]) break;
            wc = nullptr;
        }
        if (!wc) continue;

        WidgetHit wh = wc->HitTestWidget(pt);
        if (wh == WidgetHit::None) continue;

        if (wh == WidgetHit::ResizeHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingResize;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::MoveHandle)
        {
            SelectWidgetOnly(wi);
            widgetAction_ = WidgetAction::PendingMove;
            InvalidateDragStaticScene();
            widgetDragOriginalCell_ = widgets_[wi].gridCell;
            widgetDragOriginalSpan_ = widgets_[wi].gridSpan;
            widgetPreviewCell_ = widgetDragOriginalCell_;
            widgetPreviewSpan_ = widgetDragOriginalSpan_;
            RECT bounds = widgets_[wi].bounds;
            dragGroupOriginX_ = bounds.left;
            dragGroupOriginY_ = bounds.top;
            mouseDownWidgetIndex_ = wi;
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::Content)
        {
            Item* memberItem = nullptr;
            RECT bodyRect = wc->GetBodyRect();
            auto& slots = wc->GetSlots();
            for (auto& slot : slots)
            {
                RECT bounds = slot->GetBounds();
                if (PtInRect(&bounds, pt) && PtInRect(&bodyRect, pt))
                {
                    memberItem = slot->GetItem();
                    break;
                }
            }

            if (memberItem)
            {
                if (ctrl)
                {
                    ClearSelectionOutsideWidget(wi);
                    if (memberItem->IsSelected())
                        pendingCtrlToggleWidgetItem_ = memberItem;
                    else
                        memberItem->SetSelected(true);
                }
                else if (!memberItem->IsSelected())
                {
                    ClearSelection();
                    memberItem->SetSelected(true);
                }
                else
                {
                    ClearSelectionOutsideWidget(wi);
                }
                mouseDownWidgetIndex_ = wi;
                mouseDownHit_ = memberItem;
                SetCapture(hwnd_);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }

            // Empty content selects the widget itself.
            ClearSelection();
            widgets_[wi].selected = true;
            mouseDownWidgetIndex_ = wi;
            marqueeWidgetIndex_ = wi;
            marqueeInitialScrollOffset_ = wc->GetScrollOffset();
            mouseDownHit_ = nullptr;
            SetCapture(hwnd_);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::CollectionOpenBtn)
        {
            SelectWidgetOnly(wi);
            OpenCollectionPopupAt(wi, pt);
            mouseDown_ = false;
            mouseDownWidgetIndex_ = static_cast<size_t>(-1);
            mouseDownHit_ = nullptr;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else if (wh == WidgetHit::ListToggleBtn)
        {
            if (widgets_[wi].type == DesktopWidgetType::FolderMapping ||
                widgets_[wi].type == DesktopWidgetType::FileCategories)
            {
                widgets_[wi].listMode = !widgets_[wi].listMode;
                wc->InvalidateSlots();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::OpenFolderBtn)
        {
            // FolderMapping: open source folder
            if (widgets_[wi].type == DesktopWidgetType::FolderMapping
                && !widgets_[wi].sourceFolderPath.empty())
            {
                ShellExecuteW(hwnd_, L"open", widgets_[wi].sourceFolderPath.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            return;
        }
        else if (wh == WidgetHit::CategoryTab)
        {
            if (widgets_[wi].type == DesktopWidgetType::FileCategories)
            {
                auto* fc = dynamic_cast<FileCategories*>(wc);
                std::wstring id = fc ? fc->CategoryIdAtPoint(pt) : L"";
                if (!id.empty())
                {
                    widgets_[wi].activeCategoryId = id;
                    widgets_[wi].scrollOffset = 0;
                    wc->InvalidateSlots();
                    SaveLayoutSlots();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            return;
        }
    }

    // ── Desktop icon hit-test ───────────────────────────────
    DesktopIcon* hit = HitTestIcon(pt);
    mouseDownHit_ = hit;

    // Clear widget selection when clicking desktop area
    if (!hit && mouseDownWidgetIndex_ == static_cast<size_t>(-1) && !ctrl)
    {
        for (auto& w : widgets_) w.selected = false;
    }

    if (hit)
    {
        DesktopItem* di = hit->GetDesktopItem();
        if (ctrl)
        {
            ClearSelectionOutsideDesktop();
            size_t hitIndex = (di && !di->layoutKey.empty())
                ? FindItemIndexByKey(di->layoutKey)
                : static_cast<size_t>(-1);
            if (hit->IsSelected())
                pendingCtrlToggleDesktopIndex_ = hitIndex;
            else
                hit->SetSelected(true);
        }
        else if (!di->selected)
        {
            ClearSelection();
            hit->SetSelected(true);
        }
        else
        {
            ClearSelectionOutsideDesktop();
        }
    }
    else if (!ctrl)
        ClearSelection();

    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 处理鼠标移动事件
 * @param wp WPARAM
 * @param lp LPARAM（含鼠标坐标）
 * @details 处理拖拽会话、小部件移动/调整大小、框选、导航按钮悬停等
 */
inline void DesktopApp::OnMouseMove(WPARAM wp, LPARAM lp)
{
    (void)wp;
    POINT current{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT oldMouse = lastMousePoint_;
    lastMousePoint_ = current;

    if (!dragSession_.IsActive() && widgetAction_ == WidgetAction::None &&
        mouseDownWidgetIndex_ < widgets_.size() &&
        widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::LuaScript &&
        widgetEngine_)
    {
        WidgetHit hit = HitTestStandaloneWidget(mouseDownWidgetIndex_, current);
        if (hit == WidgetHit::Content)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]);
            widgetEngine_->EnsureWidgetLoaded(widgets_[mouseDownWidgetIndex_].id,
                widgets_[mouseDownWidgetIndex_].scriptPath);
            widgetEngine_->InvokeMouseEvent(widgets_[mouseDownWidgetIndex_].id, "onMouseMove",
                current.x - frame.left, current.y - frame.top,
                (GetAsyncKeyState(VK_LBUTTON) & 0x8000) ? 1 : 0, 0);
        }
    }

    if (mouseDown_ && mouseDownHit_ && mouseDownHit_->IsSelected() && !dragSession_.IsActive())
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            Container* source = mouseDownHit_->GetContainer();
            std::vector<Item*> sourceItems = source ? source->GetSelectedItems() : std::vector<Item*>{};
            DragSourceList sourceList = BuildDragSourceList(sourceItems, source);
            if (sourceItems.empty())
            {
                return;
            }
            dragSession_.Begin(source, std::move(sourceItems), std::move(sourceList),
                mouseDownPoint_, current);
            pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
            pendingCtrlToggleWidgetItem_ = nullptr;
            marqueeActive_ = false;
            marqueeWidgetIndex_ = static_cast<size_t>(-1);
            if (source == GetDesktopGrid())
            {
                UpdateDragGroupOrigin();
            }
            else
            {
                RECT groupBounds{};
                bool hasBounds = false;
                for (auto* item : dragSession_.Items())
                {
                    if (!item) continue;
                    RECT bounds = item->GetBounds();
                    if (IsRectEmptyRect(bounds)) continue;
                    groupBounds = hasBounds ? UnionCopy(groupBounds, bounds) : bounds;
                    hasBounds = true;
                }
                if (hasBounds)
                {
                    dragGroupOriginX_ = groupBounds.left;
                    dragGroupOriginY_ = groupBounds.top;
                }
            }
        }
    }

    UpdateCollectionPopupDwell(current);

    if (mouseDown_ && !dragSession_.IsActive()
        && (widgetAction_ == WidgetAction::PendingMove || widgetAction_ == WidgetAction::PendingResize)
        && mouseDownWidgetIndex_ < widgets_.size()
        && (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3))
    {
        if (widgetAction_ == WidgetAction::PendingMove)
            widgetAction_ = WidgetAction::Move;
        else if (widgetAction_ == WidgetAction::PendingResize)
            widgetAction_ = WidgetAction::Resize;
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    // Widget resize preview
    if (widgetAction_ == WidgetAction::Resize && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        const auto& widget = widgets_[mouseDownWidgetIndex_];
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (page)
        {
            int stepX = std::max(1, page->cellWidth + page->gapX);
            int stepY = std::max(1, page->cellHeight + page->gapY);
            int dCol = static_cast<int>(std::round(static_cast<double>(current.x - mouseDownPoint_.x) / static_cast<double>(stepX)));
            int dRow = static_cast<int>(std::round(static_cast<double>(current.y - mouseDownPoint_.y) / static_cast<double>(stepY)));

            GridCell cell = widgetDragOriginalCell_;
            GridSpan span = widgetDragOriginalSpan_;
            span.columns += dCol;
            span.rows += dRow;
            span = ClampWidgetGridSpan(widget, span,
                page->columns - cell.column, page->rows - cell.row);

            widgetPreviewCell_ = cell;
            widgetPreviewSpan_ = span;
        }
        ShowDragHintWindow(current, L"释放：调整组件大小");
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    // Widget drag preview
    if (widgetAction_ == WidgetAction::Move && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline int SlotFromCell(const std::vector<GridPage>&, const GridCell&);
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);
        POINT adjusted = {
            dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
            dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
        };
        GridCell cell = CellFromPoint(adjusted);
        if (!cell.pageId.empty())
        {
            const GridPage* page = FindGridPage(gridPages_, cell.pageId);
            if (page)
            {
                cell.column = std::clamp(cell.column, 0, page->columns - widgetDragOriginalSpan_.columns);
                cell.row    = std::clamp(cell.row,    0, page->rows    - widgetDragOriginalSpan_.rows);
            }
            widgetPreviewCell_ = cell;
        }
        ShowDragHintWindow(current, L"释放：移动组件");
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    if (dragSession_.IsActive() && !dragSession_.Items().empty())
    {
        dragSession_.UpdatePoint(current);
        int currentMods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) currentMods |= MK_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    currentMods |= MK_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   currentMods |= MK_SHIFT;
        dragSession_.UpdateActionFromMods(currentMods);

        POINT screenPt = current;
        ClientToScreen(hwnd_, &screenPt);
        bool overExternal = IsExternalDropWindowAt(current);

        if (overExternal)
        {
            DropPayload payload = DropPayload::From(dragSession_.Items());
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj)
            {
                auto* sourceWidget = dynamic_cast<WidgetContainer*>(dragSession_.Source());
                DesktopWidget* sourceWidgetData = sourceWidget ? sourceWidget->GetWidgetData() : nullptr;

                HideDragHintWindow();
                ReleaseCapture();
                mouseDown_ = false;
                mouseDownHit_ = nullptr;

                selfDragActive_ = true;
                selfDragReturned_ = false;
                selfDragOutKeys_.clear();
                for (const auto& item : items_)
                    if (item.selected) selfDragOutKeys_.push_back(item.layoutKey);

                InvalidateRect(hwnd_, nullptr, FALSE);
                UpdateWindow(hwnd_);

                DWORD oleEffect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                HRESULT hr = DoDragDrop(dataObj.Get(), static_cast<IDropSource*>(this), oleEffect, &oleEffect);
                selfDragActive_ = false;

                if (hr == DRAGDROP_S_DROP && oleEffect == DROPEFFECT_MOVE
                    && !selfDragReturned_ && payload.hasDesktopIcons)
                {
                    for (auto it = items_.rbegin(); it != items_.rend(); ++it)
                    {
                        if (it->selected && !it->desktopIconClsid.empty()) continue;
                        if (!it->selected) continue;
                        wchar_t path[MAX_PATH]{};
                        if (SHGetPathFromIDList(it->absolutePidl.get(), path))
                        {
                            SHFILEOPSTRUCTW op{};
                            op.wFunc = FO_DELETE;
                            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                            wchar_t from[MAX_PATH + 2]{};
                            wcscpy_s(from, path);
                            from[wcslen(path) + 1] = L'\0';
                            op.pFrom = from;
                            SHFileOperationW(&op);
                        }
                    }
                    SaveLayoutSlots();
                }

                if (!selfDragReturned_ && sourceWidgetData
                    && sourceWidgetData->type == DesktopWidgetType::FolderMapping)
                {
                    for (size_t i = 0; i < widgets_.size(); ++i)
                    {
                        if (&widgets_[i] == sourceWidgetData)
                        {
                            RefreshFolderMappingWidget(i);
                            break;
                        }
                    }
                }

                if (!selfDragReturned_)
                {
                    ClearSelection();
                    EndDragSession();
                    ReloadItems();
                }
                else
                {
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
                selfDragOutKeys_.clear();
                return;
            }
        }

        {
            RECT prevRect, nextRect;
            GetNavButtonRects(prevRect, nextRect);
            int navSide = 0;
            const bool hasPrev = pageOffset_ > 0;
            const bool hasNext = pageOffset_ < MaxPageOffset();
            if (hasPrev && PtInRect(&prevRect, current)) navSide = -1;
            else if (hasNext && PtInRect(&nextRect, current)) navSide = 1;
            navHoverSide_ = navSide;

            if (navSide != 0)
            {
                DWORD now = GetTickCount();
                if (navAutoFlipDir_ != navSide)
                {
                    navAutoFlipDir_ = navSide;
                    navAutoFlipTick_ = now;
                }
                else if (now - navAutoFlipTick_ > 500)
                {
                    int newOffset = NextNonEmptyOffset(pageOffset_, navSide);
                    if (newOffset != pageOffset_)
                    {
                        pageOffset_ = newOffset;
                        ApplyPageMapping();
                        MigrateSelectedItemsToLastMonitorPage();
                        LayoutItems();
                        InvalidateDragStaticScene();
                        UpdateDragGroupOrigin();
                        SaveLayoutSlots();
                        navAutoFlipTick_ = now;
                    }
                }
            }
            else
            {
                navAutoFlipDir_ = 0;
                navAutoFlipTick_ = 0;
            }
        }

        // OO hit testing: iterate all containers in reverse (topmost first)
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        popupDragTargetSlot_.reset();

        if (popupWidgetIndex_ < widgets_.size())
        {
            RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
            if (PtInRect(&popup, current))
            {
                WidgetContainer* popupContainer = nullptr;
                for (auto& c : containers_)
                {
                    popupContainer = dynamic_cast<WidgetContainer*>(c.get());
                    if (popupContainer && popupContainer->GetWidgetData() == &widgets_[popupWidgetIndex_])
                        break;
                    popupContainer = nullptr;
                }

                if (popupContainer)
                {
                    std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
                    RECT content = GetCollectionPopupContentRect(popup);
                    size_t slotIndex = 0;
                    RECT slotBounds = content;
                    HitRegion region = HitRegion::Empty;
                    Item* handoffItem = nullptr;

                    if (!popupKeys.empty())
                    {
                        bool foundItem = false;
                        for (size_t i = 0; i < popupKeys.size(); ++i)
                        {
                            RECT itemRect = GetCollectionPopupItemRect(popup, i);
                            RECT clipped = itemRect;
                            clipped.top = std::max(clipped.top, content.top);
                            clipped.bottom = std::min(clipped.bottom, content.bottom);
                            if (clipped.bottom <= clipped.top || !PtInRect(&clipped, current))
                                continue;

                            // Check Handoff: mouse on icon area of unselected item
                            size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                            if (itemIndex != static_cast<size_t>(-1) && !items_[itemIndex].selected)
                            {
                                RECT iconRect = GetItemIconRect(itemRect);
                                RECT hf = { iconRect.left - 4, iconRect.top - 2,
                                            iconRect.right + 4, iconRect.bottom + 4 };
                                if (PtInRect(&hf, current))
                                {
                                    region = HitRegion::Handoff;
                                    slotBounds = itemRect;
                                    handoffItem = popupContainer->GetMemberItem(i);
                                }
                            }

                            if (region != HitRegion::Handoff)
                            {
                                slotIndex = i;
                                slotBounds = itemRect;
                                region = current.x < itemRect.left + (itemRect.right - itemRect.left) / 2
                                    ? HitRegion::SortBefore
                                    : HitRegion::SortAfter;
                            }
                            foundItem = true;
                            break;
                        }

                        if (!foundItem)
                        {
                            slotIndex = popupKeys.size() - 1;
                            slotBounds = GetCollectionPopupItemRect(popup, slotIndex);
                            region = HitRegion::SortAfter;
                        }
                    }

                    popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
                    if (handoffItem)
                        popupDragTargetSlot_->SetItem(handoffItem);
                    targetContainer = popupContainer;
                    targetSlot = popupDragTargetSlot_.get();
                    targetRegion = region;
                }
            }
        }

        if (!targetContainer)
        {
            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                Slot* slot = nullptr;
                HitRegion region = (*it)->HitTestDrag(current, slot);
                if (region != HitRegion::None)
                {
                    targetContainer = it->get();
                    targetSlot = slot;
                    targetRegion = region;
                    break;
                }
            }
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        std::wstring hint;
        if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), currentMods);

        ShowDragHintWindow(current, hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
        return;
    }

    if (mouseDown_ && !mouseDownHit_)
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            marqueeActive_ = true;
            UpdateMarqueeSelection(current);
        }
    }

    {
        int oldHover = navHoverSide_;
        navHoverSide_ = 0;
        if (MaxPageOffset() > 0 || pageOffset_ > 0)
        {
            RECT prevRect, nextRect;
            GetNavButtonRects(prevRect, nextRect);
            if (pageOffset_ > 0 && PtInRect(&prevRect, current)) navHoverSide_ = -1;
            else if (pageOffset_ < MaxPageOffset() && PtInRect(&nextRect, current)) navHoverSide_ = 1;
        }
        if (navHoverSide_ != oldHover)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

    if (oldMouse.x != current.x || oldMouse.y != current.y)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 处理鼠标左键释放事件
 * @param wp WPARAM
 * @param lp LPARAM（含鼠标坐标）
 * @details 完成拖拽放置、小部件移动/调整大小、Ctrl 点击切换等
 */
inline void DesktopApp::OnLeftButtonUp(WPARAM wp, LPARAM lp)
{
    (void)wp;
    POINT upPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    HideDragHintWindow();

    // ── Widget action completion ────────────────────────────
    if (widgetAction_ != WidgetAction::None && mouseDownWidgetIndex_ < widgets_.size())
    {
        if (widgetAction_ == WidgetAction::Move)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, true);
        else if (widgetAction_ == WidgetAction::Resize)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, false);
        // PendingMove/PendingResize: just cancel without displacement
        widgetAction_ = WidgetAction::None;
        InvalidateDragStaticScene();
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragSession_.IsActive())
    {
        if (mouseDownWidgetIndex_ < widgets_.size() &&
            widgets_[mouseDownWidgetIndex_].type == DesktopWidgetType::LuaScript &&
            HitTestStandaloneWidget(mouseDownWidgetIndex_, upPoint) == WidgetHit::Content &&
            widgetEngine_)
        {
            RECT frame = GetStandaloneWidgetFrameRect(widgets_[mouseDownWidgetIndex_]);
            widgetEngine_->EnsureWidgetLoaded(widgets_[mouseDownWidgetIndex_].id,
                widgets_[mouseDownWidgetIndex_].scriptPath);
            widgetEngine_->InvokeMouseEvent(widgets_[mouseDownWidgetIndex_].id, "onMouseUp",
                upPoint.x - frame.left, upPoint.y - frame.top, 1, 0);
            widgetEngine_->InvokeClick(widgets_[mouseDownWidgetIndex_].id,
                upPoint.x - frame.left, upPoint.y - frame.top);
        }
        if (pendingCtrlToggleDesktopIndex_ < items_.size())
            items_[pendingCtrlToggleDesktopIndex_].selected = false;
        pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
        if (pendingCtrlToggleWidgetItem_)
        {
            pendingCtrlToggleWidgetItem_->SetSelected(!pendingCtrlToggleWidgetItem_->IsSelected());
            pendingCtrlToggleWidgetItem_ = nullptr;
        }
        mouseDown_ = false;
        marqueeActive_ = false;
        marqueeWidgetIndex_ = static_cast<size_t>(-1);
        navHoverSide_ = 0;
        navAutoFlipDir_ = 0;
        mouseDownHit_ = nullptr;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        ReleaseCapture();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (!dragSession_.TargetContainer() || dragSession_.TargetRegion() == HitRegion::None)
    {
        goto cleanup;
    }

    {
        int mods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;

        if (dragSession_.TargetRegion() == HitRegion::Handoff && dragSession_.TargetSlot()
            && dragSession_.TargetSlot()->GetItem())
        {
            Item* targetItem = dragSession_.TargetSlot()->GetItem();
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj)
            {
                ComPtr<IDropTarget> dropTarget;
                if (auto* targetIcon = dynamic_cast<DesktopIcon*>(targetItem))
                {
                    DesktopItem* desktopItem = targetIcon->GetDesktopItem();
                    if (desktopItem && desktopItem->childPidl.get())
                    {
                        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(desktopItem->childPidl.get());
                        desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                            reinterpret_cast<void**>(dropTarget.GetAddressOf()));
                    }
                }
                else if (!targetItem->GetPath().empty())
                {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                        nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                    {
                        shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dropTarget));
                    }
                }

                if (dropTarget)
                {
                    POINT screen = dragSession_.CurrentPoint();
                    ClientToScreen(hwnd_, &screen);
                    POINTL spl{ screen.x, screen.y };
                    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
                    DWORD keyState = MK_LBUTTON;
                    if (mods & MK_CONTROL) keyState |= MK_CONTROL;
                    if (mods & MK_ALT)     keyState |= MK_ALT;
                    if (mods & MK_SHIFT)   keyState |= MK_SHIFT;
                    if (SUCCEEDED(dropTarget->DragEnter(dataObj.Get(), keyState, spl, &effect)))
                    {
                        dropTarget->DragOver(keyState, spl, &effect);
                        dropTarget->Drop(dataObj.Get(), keyState, spl, &effect);
                    }
                }
            }
            SaveLayoutSlots();
            ClearSelection();
            EndDragSession();
            ReloadItems();
            goto cleanup;
        }

        Container* targetContainer = dragSession_.TargetContainer();
        bool needsReload = targetContainer->NeedsShellReloadAfterDrop();
        targetContainer->OnItemsDropped(dragSession_.Items(), dragSession_.Source(),
            dragSession_.TargetSlot(), dragSession_.TargetRegion(), mods);

        SaveLayoutSlots();
        ClearSelection();
        EndDragSession();
        RebuildContainersAndItems();
        if (needsReload)
            ReloadItems();
        else
            InvalidateRect(hwnd_, nullptr, FALSE);
    }

cleanup:
    EndDragSession();
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    popupDwellWidgetIndex_ = static_cast<size_t>(-1);
    KillTimer(hwnd_, kCollectionPopupDwellTimerId);
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    ReleaseCapture();
}

/**
 * @brief 获取所有选中的文件夹条目路径
 * @param firstWidgetIndex [out] 第一个包含选中条目的部件索引
 * @return 选中的文件路径列表
 */
inline std::vector<std::wstring> DesktopApp::GetSelectedFolderEntryPaths(size_t* firstWidgetIndex) const
{
    if (firstWidgetIndex)
        *firstWidgetIndex = static_cast<size_t>(-1);

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;

        std::vector<std::wstring> paths;
        for (const auto& entry : widget.folderEntries)
            if (entry.selected && !entry.fullPath.empty())
                paths.push_back(entry.fullPath);

        if (!paths.empty())
        {
            if (firstWidgetIndex)
                *firstWidgetIndex = i;
            return paths;
        }
    }

    return {};
}

/**
 * @brief 查找文件夹映射的快捷操作目标部件
 * @return 部件索引，未找到返回 (size_t)-1
 */
inline size_t DesktopApp::FindFolderMappingShortcutTarget() const
{
    size_t selectedEntryWidget = static_cast<size_t>(-1);
    (void)GetSelectedFolderEntryPaths(&selectedEntryWidget);
    if (selectedEntryWidget < widgets_.size())
        return selectedEntryWidget;

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type != DesktopWidgetType::FolderMapping || widget.sourceFolderPath.empty())
            continue;
        if (PtInRect(&widget.bounds, lastMousePoint_))
            return i;
    }

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type == DesktopWidgetType::FolderMapping &&
            widget.selected && !widget.sourceFolderPath.empty())
            return i;
    }

    return static_cast<size_t>(-1);
}

/**
 * @brief 复制或剪切选中的文件夹条目到剪贴板
 * @param cut true 为剪切，false 为复制
 * @return 是否成功
 */
inline bool DesktopApp::CopyCutSelectedFolderEntries(bool cut)
{
    std::vector<std::wstring> paths = GetSelectedFolderEntryPaths();
    if (paths.empty()) return false;

    ComPtr<IDataObject> dataObj = CreateFileDropDataObject(paths);
    if (!dataObj) return false;

    cutPaths_.clear();
    if (cut)
    {
        CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        FORMATETC fmt{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM med{};
        med.tymed = TYMED_HGLOBAL;
        med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
        if (med.hGlobal)
        {
            *static_cast<DWORD*>(GlobalLock(med.hGlobal)) = DROPEFFECT_MOVE;
            GlobalUnlock(med.hGlobal);
            dataObj->SetData(&fmt, &med, TRUE);
        }

        for (const auto& path : paths)
            cutPaths_.insert(path);
    }

    OleSetClipboard(dataObj.Get());
    OleFlushClipboard();
    UpdateCutState();
    InvalidateRect(hwnd_, nullptr, FALSE);
    return true;
}

/**
 * @brief 删除选中的文件夹条目（移至回收站）
 * @return 是否执行了删除操作
 */
inline bool DesktopApp::DeleteSelectedFolderEntries()
{
    std::vector<std::wstring> paths = GetSelectedFolderEntryPaths();
    if (paths.empty()) return false;

    std::wstring from;
    for (const auto& path : paths)
    {
        cutPaths_.erase(path);
        from += path;
        from.push_back(L'\0');
    }
    from.push_back(L'\0');

    SHFILEOPSTRUCTW op{};
    op.hwnd = hwnd_;
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI;
    if (SHFileOperationW(&op) != 0 || op.fAnyOperationsAborted)
        return true;

    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].type == DesktopWidgetType::FolderMapping)
            RefreshFolderMappingWidget(i);
    ReloadItems(false);
    return true;
}

/**
 * @brief 将剪贴板内容粘贴到指定文件夹映射部件中
 * @param widgetIndex 目标部件索引
 * @return 是否成功粘贴
 */
inline bool DesktopApp::PasteClipboardToFolderMapping(size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return false;
    DesktopWidget& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::FolderMapping || widget.sourceFolderPath.empty())
        return false;

    ComPtr<IDataObject> clipObj;
    if (FAILED(OleGetClipboard(&clipObj)) || !clipObj)
        return false;

    DropAction action = DropAction::Copy;
    CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
    FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medPref{};
    if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) && medPref.hGlobal)
    {
        DWORD* pEffect = static_cast<DWORD*>(GlobalLock(medPref.hGlobal));
        if (pEffect)
        {
            if (*pEffect & DROPEFFECT_MOVE)
                action = DropAction::Move;
            else if (*pEffect & DROPEFFECT_LINK)
                action = DropAction::Link;
            GlobalUnlock(medPref.hGlobal);
        }
        ReleaseStgMedium(&medPref);
    }

    std::vector<std::wstring> paths;
    FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
    STGMEDIUM medDrop{};
    if (SUCCEEDED(clipObj->GetData(&fmtDrop, &medDrop)) && medDrop.hGlobal)
    {
        HDROP hDrop = static_cast<HDROP>(medDrop.hGlobal);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        paths.reserve(count);
        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                paths.push_back(path);
        }
        ReleaseStgMedium(&medDrop);
    }
    if (paths.empty()) return false;

    DragSourceList sourceList;
    sourceList.hasExternalFiles = true;
    sourceList.entries.reserve(paths.size());
    for (const auto& path : paths)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = sourceList.entries.size();
        entry.filePath = path;
        entry.displayName = FileNameFromPath(path);
        sourceList.entries.push_back(std::move(entry));
    }

    if (!MaterializeFilesToFolder(sourceList, widget.sourceFolderPath, action))
        return true;

    if (action == DropAction::Move)
    {
        cutPaths_.clear();
        if (OpenClipboard(hwnd_))
        {
            EmptyClipboard();
            CloseClipboard();
        }
    }

    for (size_t i = 0; i < widgets_.size(); ++i)
        if (widgets_[i].type == DesktopWidgetType::FolderMapping)
            RefreshFolderMappingWidget(i);
    ReloadItems(false);
    return true;
}

/**
 * @brief 处理键盘按键按下事件
 * @param key 虚拟键码
 */
inline void DesktopApp::OnKeyDown(WPARAM key)
{
    if (key == VK_CONTROL || key == VK_MENU || key == VK_SHIFT)
    {
        RefreshDragHintFromKeyboard();
        return;
    }

    if (renameEdit_ != nullptr) return;

    if (quickNavigationOpen_)
    {
        if (key == VK_ESCAPE)
        {
            CloseQuickNavigation();
            return;
        }
    }

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;

    switch (key)
    {
    case VK_F2:
    case 'R':
        if (key == 'R' && !ctrl) break;
        if (key == VK_F2 || ctrl)
            BeginRenameSelected();
        break;
    case VK_F5:
        ReloadItems();
        break;
    case VK_DELETE:
    {
        if (DeleteSelectedFolderEntries())
            break;

        cutPaths_.clear();
        for (const auto& item : items_)
        {
            if (!item.selected || !item.desktopIconClsid.empty()) continue;
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
            {
                SHFILEOPSTRUCTW op{};
                op.wFunc = FO_DELETE;
                op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION;
                wchar_t from[MAX_PATH + 2]{};
                wcscpy_s(from, path);
                from[wcslen(path) + 1] = L'\0';
                op.pFrom = from;
                SHFileOperationW(&op);
            }
        }
        ReloadItems();
        break;
    }
    case 'C':
        if (!ctrl) break;
        if (CopyCutSelectedFolderEntries(false))
            break;
        InvokeSelectedShellVerb("copy");
        break;
    case 'X':
        if (!ctrl) break;
    {
        if (CopyCutSelectedFolderEntries(true))
            break;

        cutPaths_.clear();

        std::vector<PCUITEMID_CHILD> pidls;
        std::vector<size_t> selectedIndexes;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (!items_[i].selected || !items_[i].desktopIconClsid.empty()) continue;
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(items_[i].childPidl.get()));
            selectedIndexes.push_back(i);
        }

        if (!pidls.empty())
        {
            ComPtr<IDataObject> dataObj;
            if (SUCCEEDED(desktopFolder_->GetUIObjectOf(hwnd_, static_cast<UINT>(pidls.size()),
                pidls.data(), IID_IDataObject, nullptr,
                reinterpret_cast<void**>(dataObj.GetAddressOf()))) && dataObj)
            {
                CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
                FORMATETC fmt{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                STGMEDIUM med{};
                med.tymed = TYMED_HGLOBAL;
                med.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
                if (med.hGlobal)
                {
                    *static_cast<DWORD*>(GlobalLock(med.hGlobal)) = DROPEFFECT_MOVE;
                    GlobalUnlock(med.hGlobal);
                    dataObj->SetData(&fmt, &med, TRUE);
                }

                OleSetClipboard(dataObj.Get());
                OleFlushClipboard();
            }
        }

        for (size_t idx : selectedIndexes)
        {
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(items_[idx].absolutePidl.get(), path))
                cutPaths_.insert(path);
        }

        UpdateCutState();
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    break;
    case 'V':
        if (!ctrl) break;
    {
        if (PasteClipboardToFolderMapping(FindFolderMappingShortcutTarget()))
            break;

        bool fromDesktop = false;
        std::unordered_set<std::wstring> clipPaths;

        ComPtr<IDataObject> clipObj;
        if (SUCCEEDED(OleGetClipboard(&clipObj)) && clipObj)
        {
            CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
            FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
            STGMEDIUM medPref{};
            if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) && medPref.hGlobal)
            {
                DWORD* pEffect = static_cast<DWORD*>(GlobalLock(medPref.hGlobal));
                bool isMove = pEffect && (*pEffect & DROPEFFECT_MOVE);
                if (pEffect) GlobalUnlock(medPref.hGlobal);
                if (isMove)
                {
                    FORMATETC fmtDrop{ CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                    STGMEDIUM medDrop{};
                    if (SUCCEEDED(clipObj->GetData(&fmtDrop, &medDrop)) && medDrop.hGlobal)
                    {
                        HDROP hDrop = static_cast<HDROP>(medDrop.hGlobal);
                        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
                        for (UINT i = 0; i < count; ++i)
                        {
                            wchar_t path[MAX_PATH]{};
                            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                                clipPaths.insert(path);
                        }
                        ReleaseStgMedium(&medDrop);
                    }
                }
                ReleaseStgMedium(&medPref);
            }
        }

        if (!clipPaths.empty())
        {
            for (const auto& item : items_)
            {
                wchar_t path[MAX_PATH]{};
                if (SHGetPathFromIDListW(item.absolutePidl.get(), path) && clipPaths.contains(path))
                {
                    fromDesktop = true;
                    break;
                }
            }
        }

        if (fromDesktop)
        {
            cutPaths_.clear();
            if (OpenClipboard(hwnd_))
            {
                EmptyClipboard();
                CloseClipboard();
            }
            UpdateCutState();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else
        {
            ComPtr<IContextMenu> bgMenu;
            if (SUCCEEDED(desktopFolder_->CreateViewObject(hwnd_, IID_IContextMenu,
                reinterpret_cast<void**>(bgMenu.GetAddressOf()))) && bgMenu)
            {
                CMINVOKECOMMANDINFO info{};
                info.cbSize = sizeof(info);
                info.hwnd = hwnd_;
                info.lpVerb = "paste";
                info.nShow = SW_SHOWNORMAL;
                bgMenu->InvokeCommand(&info);
                cutPaths_.clear();
                ReloadItems();
            }
        }
    }
    break;
    case 'A':
        if (!ctrl) break;
    {
        ClearSelection();
        for (auto& oo : items_oo_)
        {
            auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
            if (!icon) continue;
            DesktopItem* di = icon->GetDesktopItem();
            if (!di || di->name.empty()) continue;
            di->selected = true;
        }
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
    break;
    case VK_UP:
    case VK_DOWN:
    case VK_LEFT:
    case VK_RIGHT:
        MoveKeyboardSelection(key);
        break;
    default:
        break;
    }
}

/**
 * @brief 根据键盘修饰键状态刷新拖拽提示信息
 */
inline void DesktopApp::RefreshDragHintFromKeyboard()
{
    if (!dragSession_.IsActive() && !externalDragActive_ && !selfDragActive_) return;

    int mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(mods, externalDragActive_ ? DropAction::Copy : DropAction::Move);

    std::wstring hint;
    if (dragSession_.TargetContainer() && dragSession_.TargetRegion() != HitRegion::None)
    {
        hint = dragSession_.TargetContainer()->GetDragHint(dragSession_.TargetSlot(),
            dragSession_.TargetRegion(), dragSession_.Items(), dragSession_.Source(), mods);
    }

    if (externalDragActive_ || selfDragActive_)
    {
        POINT screenPoint = dragSession_.CurrentPoint();
        ClientToScreen(hwnd_, &screenPoint);
        ShowDragHintWindowScreen(screenPoint, hint);
        OnPaint();
    }
    else
    {
        ShowDragHintWindow(dragSession_.CurrentPoint(), hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

/**
 * @brief 对选中的桌面项调用 Shell 动词（如 "copy"）
 * @param verb Shell 动词字符串
 */
inline void DesktopApp::InvokeSelectedShellVerb(const char* verb)
{
    std::vector<PCUITEMID_CHILD> pidls;
    for (const auto& item : items_)
    {
        if (item.selected && item.childPidl.value)
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.value));
    }
    if (pidls.empty()) return;

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IContextMenu, nullptr,
        reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    if (FAILED(hr) || !contextMenu) return;

    CMINVOKECOMMANDINFO info{};
    info.cbSize = sizeof(info);
    info.hwnd = hwnd_;
    info.lpVerb = verb;
    info.nShow = SW_SHOWNORMAL;
    if (SUCCEEDED(contextMenu->InvokeCommand(&info)))
        ReloadItems();
}

/**
 * @brief 使用键盘方向键移动选中项
 * @param arrowKey 方向键虚拟键码
 */
inline void DesktopApp::MoveKeyboardSelection(WPARAM arrowKey)
{
    if (items_.empty()) return;

    std::vector<size_t> visible;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        if (items_[i].selected || !IsRectEmptyRect(items_[i].bounds))
            visible.push_back(i);
    }
    if (visible.empty()) return;

    int current = -1;
    for (size_t i = 0; i < visible.size(); ++i)
    {
        if (items_[visible[i]].selected) { current = static_cast<int>(i); break; }
    }
    int delta = 0;
    switch (arrowKey)
    {
    case VK_LEFT:  delta = -1; break;
    case VK_RIGHT: delta =  1; break;
    case VK_UP:    delta = -1; break;
    case VK_DOWN:  delta =  1; break;
    }
    if (delta == 0) return;

    int next = current < 0 ? 0 : current + delta;
    next = std::clamp(next, 0, static_cast<int>(visible.size()) - 1);
    SelectOnly(static_cast<int>(visible[static_cast<size_t>(next)]));
    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 处理页面导航按钮点击事件（上一页/下一页）
 * @param point 点击坐标
 * @return 是否已处理导航
 */
inline bool DesktopApp::HandlePageNavClick(POINT point)
{
    if (gridPages_.empty()) return false;
    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();
    if (!hasPrev && !hasNext) return false;

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    int delta = 0;
    if (hasPrev && PtInRect(&prevRect, point)) delta = -1;
    else if (hasNext && PtInRect(&nextRect, point)) delta = 1;
    if (delta == 0) return false;

    int newOffset = NextNonEmptyOffset(pageOffset_, delta);
    if (newOffset == pageOffset_) return false;

    bool wasDragging = dragSession_.IsActive();
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (wasDragging) MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();
    if (wasDragging) InvalidateDragStaticScene();
    if (wasDragging) UpdateDragGroupOrigin();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

/**
 * @brief 处理鼠标右键释放事件（显示上下文菜单）
 * @param lp LPARAM（含鼠标坐标）
 */
inline void DesktopApp::OnRightButtonUp(LPARAM lp)
{
    if (renameEdit_ != nullptr) return;
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT screenPt = pt;
    ClientToScreen(hwnd_, &screenPt);

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt))
        {
            std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
            RECT content = GetCollectionPopupContentRect(popup);
            for (size_t i = 0; i < popupKeys.size(); ++i)
            {
                RECT itemRect = GetCollectionPopupItemRect(popup, i);
                RECT clipped = itemRect;
                clipped.top = std::max(clipped.top, content.top);
                clipped.bottom = std::min(clipped.bottom, content.bottom);
                if (clipped.bottom <= clipped.top || !PtInRect(&clipped, pt)) continue;

                size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
                if (itemIndex != static_cast<size_t>(-1))
                {
                    if (!items_[itemIndex].selected)
                        SelectOnly(static_cast<int>(itemIndex));
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (IsProtectedDesktopIcon(items_[itemIndex]))
                        ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
                    else
                        ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
                    return;
                }
            }
        }
    }

    // Check widget member items first; otherwise the widget frame menu steals member right-clicks.
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(it->get());
        if (!wc) continue;

        WidgetHit wh = wc->HitTestWidget(pt);
        if (wh == WidgetHit::MoveHandle || wh == WidgetHit::ResizeHandle)
            continue;

        RECT bodyRect = wc->GetBodyRect();

        auto& slots = wc->GetSlots();
        for (auto& slot : slots)
        {
            if (!slot) continue;
            RECT bounds = slot->GetBounds();
            if (!PtInRect(&bounds, pt)) continue;
            if (!PtInRect(&bodyRect, pt)) continue;

            auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem());
            DesktopItem* item = icon ? icon->GetDesktopItem() : nullptr;
            if (!item)
            {
                auto* folderIcon = dynamic_cast<FolderEntryIcon*>(slot->GetItem());
                FolderEntry* entry = folderIcon ? folderIcon->GetFolderEntry() : nullptr;
                if (!entry) break;

                auto* folderWidget = dynamic_cast<WidgetContainer*>(wc);
                DesktopWidget* data = folderWidget ? folderWidget->GetWidgetData() : nullptr;
                size_t widgetIndex = static_cast<size_t>(-1);
                size_t memberIndex = static_cast<size_t>(-1);
                for (size_t wi = 0; wi < widgets_.size(); ++wi)
                {
                    if (&widgets_[wi] != data) continue;
                    widgetIndex = wi;
                    for (size_t mi = 0; mi < widgets_[wi].folderEntries.size(); ++mi)
                    {
                        if (&widgets_[wi].folderEntries[mi] == entry)
                        {
                            memberIndex = mi;
                            break;
                        }
                    }
                    break;
                }
                if (widgetIndex == static_cast<size_t>(-1) ||
                    memberIndex == static_cast<size_t>(-1))
                    break;

                if (!entry->selected)
                {
                    ClearSelection();
                    widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                ShowFolderEntryContextMenu(screenPt, widgetIndex, memberIndex);
                return;
            }

            size_t itemIndex = FindItemIndexByKey(item->layoutKey);
            if (itemIndex == static_cast<size_t>(-1)) break;

            if (!items_[itemIndex].selected)
                SelectOnly(static_cast<int>(itemIndex));
            InvalidateRect(hwnd_, nullptr, FALSE);
            if (IsProtectedDesktopIcon(items_[itemIndex]))
                ShowShellContextMenu(screenPt, static_cast<int>(itemIndex));
            else
                ShowItemContextMenu(screenPt, static_cast<int>(itemIndex));
            return;
        }
    }

    // Check widget hit after member items.
    size_t hitWidget = static_cast<size_t>(-1);
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (!wc || wc->GetWidgetData() != &widgets_[wi]) continue;
            if (wc->HitTestWidget(pt) != WidgetHit::None)
            {
                hitWidget = wi;
                break;
            }
        }
        if (hitWidget != static_cast<size_t>(-1)) break;
    }

    if (hitWidget != static_cast<size_t>(-1))
    {
        // Select the widget and show its context menu
        SelectWidgetOnly(hitWidget);
        InvalidateRect(hwnd_, nullptr, FALSE);

        ShowWidgetContextMenu(screenPt, hitWidget);
        return;
    }

    size_t hitStandaloneWidget = HitTestStandaloneWidgetIndex(pt);
    if (hitStandaloneWidget != static_cast<size_t>(-1))
    {
        SelectWidgetOnly(hitStandaloneWidget);
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowWidgetContextMenu(screenPt, hitStandaloneWidget);
        return;
    }

    if (IsPointOverWidgetChrome(pt))
    {
        ClearSelection();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowBackgroundContextMenu(screenPt);
        return;
    }

    int hit = HitTestItem(pt);
    if (hit >= 0 && !items_[hit].selected)
        SelectOnly(hit);
    else if (hit < 0)
        ClearSelection();
    InvalidateRect(hwnd_, nullptr, FALSE);

    if (hit >= 0)
    {
        if (IsProtectedDesktopIcon(items_[hit]))
            ShowShellContextMenu(screenPt, hit);
        else
            ShowItemContextMenu(screenPt, hit);
    }
    else
        ShowBackgroundContextMenu(screenPt);
}

/**
 * @brief 处理定时器事件
 * @param timerId 定时器 ID
 */
inline void DesktopApp::OnTimer(WPARAM timerId)
{
    if (timerId == kShellChangeTimerId)
    {
        KillTimer(hwnd_, kShellChangeTimerId);
        if (!mouseDown_ && !reloading_)
            ReloadItems();
    }
    else if (timerId == kRecycleBinPollTimerId)
    {
        SHQUERYRBINFO info{};
        info.cbSize = sizeof(info);
        if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &info)))
        {
            if (lastRecycleBinItemCount_ >= 0 && info.i64NumItems != lastRecycleBinItemCount_)
            {
                if (!mouseDown_ && !reloading_)
                    ReloadItems();
            }
            lastRecycleBinItemCount_ = info.i64NumItems;
        }
    }
    else if (timerId == kDesktopHostWatchTimerId)
    {
        WatchDesktopHost();
    }
    else if (timerId == kWidgetRefreshTimerId)
    {
        if (widgetEngine_)
            widgetEngine_->TickRuntime();
        bool hasLuaWidget = false;
        for (const auto& widget : widgets_)
        {
            if (widget.type == DesktopWidgetType::LuaScript)
            {
                hasLuaWidget = true;
                break;
            }
        }
        if (hasLuaWidget)
            InvalidateRect(hwnd_, nullptr, FALSE);
    }
    else if (timerId == kCollectionPopupDwellTimerId)
    {
        if (!dragSession_.IsActive() || popupWidgetIndex_ < widgets_.size() ||
            popupDwellWidgetIndex_ >= widgets_.size())
        {
            popupDwellWidgetIndex_ = static_cast<size_t>(-1);
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            return;
        }

        if (TryOpenDwellCollectionPopup(GetTickCount()))
        {
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            OnMouseMove(0, MAKELPARAM(lastMousePoint_.x, lastMousePoint_.y));
        }
    }
}

/**
 * @brief 更新集合弹窗的停留检测逻辑
 * @param point 当前鼠标位置
 */
inline void DesktopApp::UpdateCollectionPopupDwell(POINT point)
{
    if (!dragSession_.IsActive() || popupWidgetIndex_ < widgets_.size())
    {
        popupDwellWidgetIndex_ = static_cast<size_t>(-1);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        return;
    }

    size_t hoveredCollection = static_cast<size_t>(-1);
    for (auto& c : containers_)
    {
        auto* collection = dynamic_cast<Collection*>(c.get());
        if (!collection) continue;

        RECT buttonRect = collection->GetAllButtonRect();
        if (IsRectEmptyRect(buttonRect) || !PtInRect(&buttonRect, point))
            continue;

        DesktopWidget* data = collection->GetWidgetData();
        for (size_t wi = 0; wi < widgets_.size(); ++wi)
        {
            if (&widgets_[wi] == data)
            {
                hoveredCollection = wi;
                break;
            }
        }
        break;
    }

    if (hoveredCollection == static_cast<size_t>(-1))
    {
        popupDwellWidgetIndex_ = static_cast<size_t>(-1);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        return;
    }

    DWORD now = GetTickCount();
    if (popupDwellWidgetIndex_ != hoveredCollection)
    {
        popupDwellWidgetIndex_ = hoveredCollection;
        popupDwellTick_ = now;
        SetTimer(hwnd_, kCollectionPopupDwellTimerId, kCollectionPopupDwellIntervalMs, nullptr);
        return;
    }

    TryOpenDwellCollectionPopup(now);
}

/**
 * @brief 尝试在停留时间达标后打开集合弹窗
 * @param now 当前时间（毫秒）
 * @return 是否成功打开了弹窗
 */
inline bool DesktopApp::TryOpenDwellCollectionPopup(DWORD now)
{
    if (popupDwellWidgetIndex_ >= widgets_.size())
        return false;
    if (now - popupDwellTick_ < kCollectionPopupDwellDelayMs)
        return false;

    size_t widgetIndex = popupDwellWidgetIndex_;
    OpenCollectionPopupAt(widgetIndex, lastMousePoint_);
    UpdateWindow(hwnd_);
    return true;
}

// ── Collection popup ─────────────────────────────────────────

/**
 * @brief 在指定位置打开集合弹窗
 * @param widgetIndex 集合小部件索引
 * @param anchorPoint 锚点位置
 * @param categoryId 可选的分类 ID
 */
inline void DesktopApp::OpenCollectionPopupAt(size_t widgetIndex, POINT anchorPoint,
    const std::wstring& categoryId)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::Collection)
        return;

    popupWidgetIndex_ = widgetIndex;
    popupScrollOffset_ = 0;
    popupHasAnchor_ = anchorPoint.x != LONG_MIN || anchorPoint.y != LONG_MIN;
    popupAnchorPoint_ = anchorPoint;
    popupCategoryId_ = categoryId;
    popupPageId_ = widgets_[widgetIndex].gridCell.pageId;
    popupRect_ = GetCollectionPopupRect(widgets_[widgetIndex]);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widgets_[widgetIndex], popupRect_));
    popupDwellWidgetIndex_ = static_cast<size_t>(-1);
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 关闭当前打开的集合弹窗
 */
inline void DesktopApp::CloseCollectionPopup()
{
    if (popupWidgetIndex_ == static_cast<size_t>(-1)) return;
    popupWidgetIndex_ = static_cast<size_t>(-1);
    popupScrollOffset_ = 0;
    popupHasAnchor_ = false;
    popupAnchorPoint_ = {};
    popupPageId_.clear();
    popupCategoryId_.clear();
    popupRect_ = {};
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 处理鼠标滚轮事件
 * @param wp WPARAM（含滚轮增量）
 * @param lp LPARAM（含鼠标坐标）
 */
inline void DesktopApp::OnMouseWheel(WPARAM wp, LPARAM lp)
{
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    ScreenToClient(hwnd_, &pt);
    int currentMods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000) currentMods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)    currentMods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   currentMods |= MK_SHIFT;
    if (dragSession_.IsActive())
        dragSession_.UpdateActionFromMods(currentMods, externalDragActive_ ? DropAction::Copy : DropAction::Move);

    if (quickNavigationOpen_)
    {
        RECT overlay = GetQuickNavigationRect();
        if (PtInRect(&overlay, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            RECT tabs = GetQuickNavigationTabsRect(overlay);
            if (quickNavigationSearchText_.empty() && PtInRect(&tabs, pt))
            {
                int maxTabScroll = GetQuickNavigationMaxTabScrollOffset(overlay);
                quickNavigationTabScrollOffset_ =
                    std::clamp(quickNavigationTabScrollOffset_ - delta / 2, 0, maxTabScroll);
            }
            else
            {
                int maxScroll = GetQuickNavigationMaxScrollOffset(overlay);
                quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_ - delta / 2, 0, maxScroll);
            }
            InvalidateDragStaticScene();
            InvalidateQuickNavigationWindow();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    size_t luaWidget = HitTestStandaloneWidgetIndex(pt);
    if (luaWidget != static_cast<size_t>(-1) &&
        widgets_[luaWidget].type == DesktopWidgetType::LuaScript &&
        HitTestStandaloneWidget(luaWidget, pt) == WidgetHit::Content &&
        widgetEngine_)
    {
        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        RECT frame = GetStandaloneWidgetFrameRect(widgets_[luaWidget]);
        widgetEngine_->EnsureWidgetLoaded(widgets_[luaWidget].id, widgets_[luaWidget].scriptPath);
        int localX = pt.x - frame.left;
        int localY = pt.y - frame.top;
        if (!widgetEngine_->HandleHostUiPointer(widgets_[luaWidget].id, localX, localY, delta, true))
            widgetEngine_->InvokeMouseEvent(widgets_[luaWidget].id, "onWheel",
                localX, localY, 0, delta);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    auto refreshDragAfterScroll = [&]()
    {
        if (!dragSession_.IsActive()) return;
        RefreshDragTargetAt(pt, currentMods);
        InvalidateDragStaticScene();
    };

    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            int maxScroll = GetCollectionPopupMaxScrollOffset(widgets_[popupWidgetIndex_], popup);
            popupScrollOffset_ = std::clamp(popupScrollOffset_ - delta / 2, 0, maxScroll);
            if (marqueeActive_ && marqueeWidgetIndex_ == popupWidgetIndex_)
                UpdateMarqueeSelection(pt);
            refreshDragAfterScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // Scroll widgets with overflow content
    for (auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc || !wc->GetWidgetData()) continue;
        RECT frame = wc->GetFrameRect();
        if (!PtInRect(&frame, pt)) continue;

        int delta = GET_WHEEL_DELTA_WPARAM(wp);
        DesktopWidget* data = wc->GetWidgetData();

        // FileCategories: horizontal scroll for tabs area
        if (data->type == DesktopWidgetType::FileCategories)
        {
            auto* fc = dynamic_cast<FileCategories*>(wc);
            if (fc && fc->TryScrollTabs(pt, delta))
            {
                refreshDragAfterScroll();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
        }

        int maxScroll = wc->GetMaxScrollOffset();
        if (maxScroll <= 0) continue;

        data->scrollOffset = std::clamp(data->scrollOffset - delta / 2, 0, maxScroll);
        wc->InvalidateSlots();
        if (marqueeActive_ && marqueeWidgetIndex_ < widgets_.size() &&
            &widgets_[marqueeWidgetIndex_] == data)
        {
            UpdateMarqueeSelection(pt);
        }
        if (mouseDownHit_ && mouseDownHit_->GetContainer() == wc)
            mouseDownHit_ = nullptr;
        refreshDragAfterScroll();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }
}

// ── Rename ──────────────────────────────────────────────────

inline bool DesktopApp::CanRenameWidget(const DesktopWidget& widget) const
{
    return widget.type != DesktopWidgetType::LuaScript || widget.showTitle;
}

/**
 * @brief 获取集合中可见项的边界矩形
 * @param itemIndex 桌面项索引
 * @return 边界矩形（在弹窗或小部件中可见的部分）
 */
inline RECT DesktopApp::GetVisibleCollectionItemBounds(size_t itemIndex) const
{
    if (itemIndex >= items_.size()) return {};
    std::wstring key = ToUpperInvariant(items_[itemIndex].layoutKey);

    if (popupWidgetIndex_ < widgets_.size())
    {
        const DesktopWidget& widget = widgets_[popupWidgetIndex_];
        std::vector<std::wstring> keys = GetPopupItemKeys(widget);
        RECT popup = GetCollectionPopupRect(widget);
        RECT content = GetCollectionPopupContentRect(popup);
        for (size_t i = 0; i < keys.size(); ++i)
        {
            if (ToUpperInvariant(keys[i]) != key) continue;
            RECT rect = GetCollectionPopupItemRect(popup, i);
            if (RectsIntersect(rect, content)) return rect;
        }
    }

    for (const auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc) continue;
        for (const auto& slot : wc->GetSlots())
        {
            auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem());
            if (icon && icon->GetDesktopItem() == &items_[itemIndex])
                return slot->GetBounds();
        }
    }
    return {};
}

/**
 * @brief 查找唯一选中的文件夹条目
 * @param widgetIndex [out] 部件索引
 * @param memberIndex [out] 条目在部件中的索引
 * @return 是否恰好有一个选中条目
 */
inline bool DesktopApp::FindSingleSelectedFolderEntry(size_t& widgetIndex, size_t& memberIndex) const
{
    size_t foundWidget = static_cast<size_t>(-1);
    size_t foundMember = static_cast<size_t>(-1);
    int count = 0;
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const auto& widget = widgets_[wi];
        if (widget.type != DesktopWidgetType::FolderMapping) continue;
        for (size_t mi = 0; mi < widget.folderEntries.size(); ++mi)
        {
            if (!widget.folderEntries[mi].selected) continue;
            foundWidget = wi;
            foundMember = mi;
            ++count;
        }
    }
    if (count != 1) return false;
    widgetIndex = foundWidget;
    memberIndex = foundMember;
    return true;
}

/**
 * @brief 获取文件夹条目重命名编辑框的矩形位置
 * @param widgetIndex 部件索引
 * @param memberIndex 条目索引
 * @return 重命名编辑框的矩形
 */
inline RECT DesktopApp::GetFolderEntryRenameRect(size_t widgetIndex, size_t memberIndex) const
{
    if (widgetIndex >= widgets_.size() ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return {};

    for (const auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (!wc || wc->GetWidgetData() != &widgets_[widgetIndex]) continue;
        const auto& slots = wc->GetSlots();
        if (memberIndex >= slots.size()) break;
        RECT itemRect = slots[memberIndex]->GetBounds();
        if (widgets_[widgetIndex].listMode)
        {
            const int itemH = std::max<int>(1, static_cast<int>(itemRect.bottom - itemRect.top));
            const int iconSz = std::min(32, itemH - 4);
            return MakeRect(itemRect.left + 4 + iconSz + 6, itemRect.top + 5,
                itemRect.right - 6, itemRect.bottom - 5);
        }
        return GetItemTextRect(itemRect, true);
    }
    return {};
}

/**
 * @brief 开始对文件夹条目进行重命名（创建弹出式编辑框）
 * @param widgetIndex 部件索引
 * @param memberIndex 条目索引
 */
inline void DesktopApp::BeginRenameFolderEntry(size_t widgetIndex, size_t memberIndex)
{
    if (renameEdit_ != nullptr ||
        widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return;

    ClearSelection();
    widgets_[widgetIndex].folderEntries[memberIndex].selected = true;
    renamingFolderEntry_ = true;
    renameFolderWidgetIndex_ = widgetIndex;
    renameFolderEntryIndex_ = memberIndex;

    RECT rect = GetFolderEntryRenameRect(widgetIndex, memberIndex);
    if (IsRectEmptyRect(rect))
    {
        renamingFolderEntry_ = false;
        renameFolderWidgetIndex_ = static_cast<size_t>(-1);
        renameFolderEntryIndex_ = static_cast<size_t>(-1);
        return;
    }
    InflateRect(&rect, 2, 2);
    RECT screenRect = rect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    DWORD style = WS_POPUP | WS_VISIBLE | ES_AUTOVSCROLL;
    style |= widgets_[widgetIndex].listMode ? ES_LEFT : (ES_MULTILINE | ES_CENTER | ES_WANTRETURN);
    renameEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", widgets_[widgetIndex].folderEntries[memberIndex].name.c_str(), style,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);

    if (!renameEdit_)
    {
        renamingFolderEntry_ = false;
        renameFolderWidgetIndex_ = static_cast<size_t>(-1);
        renameFolderEntryIndex_ = static_cast<size_t>(-1);
        return;
    }

    if (renameFont_) DeleteObject(renameFont_);
    const float renameScale = GetItemLayoutScale(rect);
    renameFont_ = CreateFontW(-std::max(1, static_cast<int>(std::round(itemFontSize_ * renameScale))),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
    SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(renameMargin, renameMargin));
    SetWindowSubclass(renameEdit_, &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(renameEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
    SetFocus(renameEdit_);
}

/**
 * @brief 判断 Shell 上下文菜单命令是否为重命名命令
 * @param contextMenu Shell 上下文菜单接口
 * @param commandOffset 命令偏移量
 * @return 如果是重命名命令返回 true
 */
inline bool DesktopApp::IsShellRenameCommand(IContextMenu* contextMenu, UINT commandOffset) const
{
    if (!contextMenu) return false;

    wchar_t verbW[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(commandOffset, GCS_VERBW, nullptr,
        reinterpret_cast<LPSTR>(verbW), static_cast<UINT>(_countof(verbW)))) &&
        lstrcmpiW(verbW, L"rename") == 0)
        return true;

    char verbA[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(commandOffset, GCS_VERBA, nullptr,
        verbA, static_cast<UINT>(_countof(verbA)))) &&
        lstrcmpiA(verbA, "rename") == 0)
        return true;

    return false;
}

/**
 * @brief 显示文件夹条目的 Shell 上下文菜单
 * @param screenPoint 屏幕坐标点
 * @param widgetIndex 部件索引
 * @param memberIndex 条目索引
 */
inline void DesktopApp::ShowFolderEntryContextMenu(POINT screenPoint, size_t widgetIndex, size_t memberIndex)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size())
        return;

    const std::wstring fullPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(fullPath.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl)
        return;

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    if (FAILED(SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child)) || !parentFolder)
    {
        ILFree(pidl);
        return;
    }

    ComPtr<IContextMenu> contextMenu;
    HRESULT hr = parentFolder->GetUIObjectOf(hwnd_, 1, &child, IID_IContextMenu,
        nullptr, reinterpret_cast<void**>(contextMenu.GetAddressOf()));
    parentFolder->Release();
    if (FAILED(hr) || !contextMenu)
    {
        ILFree(pidl);
        return;
    }

    HMENU menu = CreatePopupMenu();
    if (!menu)
    {
        ILFree(pidl);
        return;
    }

    constexpr UINT kFirstCmd = 1;
    constexpr UINT kLastCmd = 0x7FFF;
    hr = contextMenu->QueryContextMenu(menu, 0, kFirstCmd, kLastCmd, CMF_NORMAL | CMF_CANRENAME);
    if (FAILED(hr))
    {
        DestroyMenu(menu);
        ILFree(pidl);
        RestoreDesktopWindowLayer();
        return;
    }

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command >= kFirstCmd && command <= kLastCmd)
    {
        UINT commandOffset = command - kFirstCmd;
        wchar_t menuText[128]{};
        bool renameCommand = IsShellRenameCommand(contextMenu.Get(), commandOffset);
        if (!renameCommand &&
            GetMenuStringW(menu, command, menuText, static_cast<int>(_countof(menuText)), MF_BYCOMMAND) > 0)
        {
            renameCommand = StrStrIW(menuText, L"重命名") != nullptr ||
                StrStrIW(menuText, L"Rename") != nullptr;
        }

        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);

        if (renameCommand)
        {
            BeginRenameFolderEntry(widgetIndex, memberIndex);
            return;
        }

        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = hwnd_;
        invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
        invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        contextMenu->InvokeCommand(reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
        RefreshFolderMappingWidget(widgetIndex);
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    DestroyMenu(menu);
    RestoreDesktopWindowLayer();
    ILFree(pidl);
}

/**
 * @brief 提交或取消文件夹条目的重命名
 * @param newName 新名称
 * @param cancel 是否取消重命名
 */
inline void DesktopApp::CommitFolderEntryRename(const std::wstring& newName, bool cancel)
{
    size_t widgetIndex = renameFolderWidgetIndex_;
    size_t memberIndex = renameFolderEntryIndex_;
    renamingFolderEntry_ = false;
    renameFolderWidgetIndex_ = static_cast<size_t>(-1);
    renameFolderEntryIndex_ = static_cast<size_t>(-1);

    if (cancel ||
        widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FolderMapping ||
        memberIndex >= widgets_[widgetIndex].folderEntries.size() ||
        newName.empty() ||
        newName == widgets_[widgetIndex].folderEntries[memberIndex].name)
        return;

    PIDLIST_ABSOLUTE pidl = nullptr;
    const std::wstring oldPath = widgets_[widgetIndex].folderEntries[memberIndex].fullPath;
    if (FAILED(SHParseDisplayName(oldPath.c_str(), nullptr, &pidl, 0, nullptr)))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    IShellFolder* parentFolder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_IShellFolder,
        reinterpret_cast<void**>(&parentFolder), &child);
    if (SUCCEEDED(hr) && parentFolder)
    {
        PITEMID_CHILD newChild = nullptr;
        hr = parentFolder->SetNameOf(hwnd_, child, newName.c_str(), SHGDN_NORMAL, &newChild);
        if (newChild) ILFree(newChild);
        parentFolder->Release();
    }
    ILFree(pidl);

    if (FAILED(hr))
    {
        MessageBeep(MB_ICONWARNING);
        return;
    }

    RefreshFolderMappingWidget(widgetIndex);
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 开始重命名选中的元素（小部件、文件夹条目或桌面项）
 */
inline void DesktopApp::BeginRenameSelected()
{
    if (renameEdit_ != nullptr) return;

    int selectedWidgetCount = 0;
    size_t selectedWidgetIndex = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].selected)
        {
            ++selectedWidgetCount;
            selectedWidgetIndex = i;
        }
    }
    if (selectedWidgetCount == 1 && selectedWidgetIndex < widgets_.size())
    {
        if (!CanRenameWidget(widgets_[selectedWidgetIndex])) return;

        renamingWidget_ = true;
        renameIndex_ = selectedWidgetIndex;

        RECT frame = widgets_[selectedWidgetIndex].bounds;
        RECT handle = frame;
        for (const auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[selectedWidgetIndex])
            {
                frame = wc->GetFrameRect();
                handle = wc->GetMoveHandleRect();
                break;
            }
        }
        const int editHeight = std::max(40, static_cast<int>(handle.bottom - handle.top) * 2);
        RECT rect = MakeRect(frame.left + 4, handle.top, frame.right - 4, handle.top + editHeight);
        InflateRect(&rect, 2, 2);
        RECT screenRect = rect;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

        renameEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"EDIT",
            widgets_[selectedWidgetIndex].title.c_str(),
            WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
            screenRect.left, screenRect.top,
            screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
            hwnd_, nullptr, instance_, nullptr);
        if (!renameEdit_)
        {
            renamingWidget_ = false;
            renameIndex_ = static_cast<size_t>(-1);
            return;
        }

        if (renameFont_) DeleteObject(renameFont_);
        const float renameScale = GetItemLayoutScale(frame);
        renameFont_ = CreateFontW(-std::max(1, static_cast<int>(std::round(itemFontSize_ * renameScale))),
            0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
        SendMessageW(renameEdit_, WM_SETFONT,
            reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
        const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
        SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
            MAKELPARAM(renameMargin, renameMargin));
        SetWindowSubclass(renameEdit_, &DesktopApp::RenameEditSubclassProc, 1,
            reinterpret_cast<DWORD_PTR>(this));
        SetWindowPos(renameEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
            screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
        SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
        SetFocus(renameEdit_);
        return;
    }

    size_t folderWidget = static_cast<size_t>(-1);
    size_t folderMember = static_cast<size_t>(-1);
    if (FindSingleSelectedFolderEntry(folderWidget, folderMember))
    {
        BeginRenameFolderEntry(folderWidget, folderMember);
        return;
    }

    int selectedCount = 0;
    int selectedIndex = -1;
    for (int i = 0; i < static_cast<int>(items_.size()); ++i)
    {
        if (items_[i].selected)
        {
            ++selectedCount;
            selectedIndex = i;
        }
    }
    if (selectedCount != 1 || selectedIndex < 0) return;
    if (!items_[selectedIndex].desktopIconClsid.empty()) return;

    wchar_t path[MAX_PATH]{};
    if (!SHGetPathFromIDListW(items_[selectedIndex].absolutePidl.get(), path)) return;

    renameIndex_ = static_cast<size_t>(selectedIndex);
    RECT itemBounds = GetVisibleCollectionItemBounds(renameIndex_);
    if (IsRectEmptyRect(itemBounds))
        itemBounds = items_[selectedIndex].bounds;
    if (IsRectEmptyRect(itemBounds)) return;
    RECT textRect = GetItemTextRect(itemBounds, true);
    InflateRect(&textRect, 2, 2);
    RECT screenRect = textRect;
    MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT",
        items_[selectedIndex].name.c_str(),
        WS_POPUP | WS_VISIBLE | ES_MULTILINE | ES_CENTER | ES_AUTOVSCROLL | ES_WANTRETURN,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);

    if (!renameEdit_)
    {
        renameIndex_ = static_cast<size_t>(-1);
        return;
    }

    if (renameFont_) DeleteObject(renameFont_);
    const float renameScale = GetItemLayoutScale(itemBounds);
    renameFont_ = CreateFontW(-std::max(1, static_cast<int>(std::round(itemFontSize_ * renameScale))),
        0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(renameFont_ ? renameFont_ : GetStockObject(DEFAULT_GUI_FONT)), TRUE);
    const int renameMargin = std::max(1, static_cast<int>(std::round(6.0f * renameScale)));
    SendMessageW(renameEdit_, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(renameMargin, renameMargin));
    SetWindowSubclass(renameEdit_, &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(renameEdit_, HWND_TOPMOST, screenRect.left, screenRect.top,
        screenRect.right - screenRect.left, screenRect.bottom - screenRect.top, SWP_SHOWWINDOW);
    SendMessageW(renameEdit_, EM_SETSEL, 0, -1);
    SetFocus(renameEdit_);
}

/**
 * @brief 提交或取消重命名编辑
 * @param cancel 是否取消重命名
 */
inline void DesktopApp::CommitRename(bool cancel)
{
    if (renameEdit_ == nullptr) return;

    HWND edit = renameEdit_;
    renameEdit_ = nullptr;
    RemoveWindowSubclass(edit, &DesktopApp::RenameEditSubclassProc, 1);

    std::wstring newName;
    if (!cancel)
    {
        int length = GetWindowTextLengthW(edit);
        if (length > 0)
        {
            std::vector<wchar_t> buffer(static_cast<size_t>(length) + 1);
            GetWindowTextW(edit, buffer.data(), length + 1);
            newName.assign(buffer.data());
        }
    }

    DestroyWindow(edit);
    if (renameFont_) { DeleteObject(renameFont_); renameFont_ = nullptr; }

    if (renamingFolderEntry_)
    {
        CommitFolderEntryRename(newName, cancel);
        return;
    }

    if (renamingWidget_)
    {
        if (!cancel && renameIndex_ < widgets_.size() && !newName.empty())
        {
            widgets_[renameIndex_].title = newName;
            SaveLayoutSlots();
        }
        renamingWidget_ = false;
        renameIndex_ = static_cast<size_t>(-1);
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    bool keepLayoutSlots = false;
    if (!cancel && renameIndex_ < items_.size() && !newName.empty() && newName != items_[renameIndex_].name)
    {
        std::wstring oldLayoutKey = items_[renameIndex_].layoutKey;
        PITEMID_CHILD newChild = nullptr;
        HRESULT hr = desktopFolder_->SetNameOf(hwnd_,
            reinterpret_cast<PCUITEMID_CHILD>(items_[renameIndex_].childPidl.get()),
            newName.c_str(), SHGDN_NORMAL, &newChild);
        if (SUCCEEDED(hr))
        {
            if (newChild)
            {
                PIDLIST_ABSOLUTE newAbsolute = ILCombine(desktopPidl_.get(), newChild);
                std::wstring newParsingName = StrRetToString(desktopFolder_.Get(), newChild, SHGDN_FORPARSING);
                if (newAbsolute)
                {
                    std::wstring newLayoutKey = GetStableLayoutKey(newAbsolute, newParsingName);
                    LayoutRecord record;
                    record.cell = items_[renameIndex_].gridCell;
                    record.span = items_[renameIndex_].gridSpan;
                    record.hasGrid = true;
                    record.legacySlot = items_[renameIndex_].slot;
                    layoutRecords_[newLayoutKey] = record;
                    for (auto& widget : widgets_)
                    {
                        for (auto& key : widget.itemKeys)
                        {
                            if (ToUpperInvariant(key) == ToUpperInvariant(oldLayoutKey))
                                key = newLayoutKey;
                        }
                    }
                    keepLayoutSlots = true;
                    ILFree(newAbsolute);
                }
            }
            ILFree(newChild);
        }
        else
        {
            MessageBeep(MB_ICONWARNING);
        }
    }

    renameIndex_ = static_cast<size_t>(-1);
    ReloadItems(!keepLayoutSlots);
}

/**
 * @brief 重命名编辑框的子类化窗口过程
 */
inline LRESULT CALLBACK DesktopApp::RenameEditSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message)
    {
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) { app->CommitRename(false); return 0; }
        if (wParam == VK_ESCAPE) { app->CommitRename(true); return 0; }
        break;
    case WM_KILLFOCUS:
        app->CommitRename(false);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

/**
 * @brief 提交或取消 Lua 内联文本编辑
 * @param cancel 是否取消编辑
 */
inline void DesktopApp::CommitLuaInlineTextEdit(bool cancel)
{
    if (luaInlineEdit_ == nullptr) return;

    HWND edit = luaInlineEdit_;
    luaInlineEdit_ = nullptr;
    RemoveWindowSubclass(edit, &DesktopApp::LuaInlineEditSubclassProc, 1);

    std::wstring value;
    if (!cancel)
    {
        int length = GetWindowTextLengthW(edit);
        std::vector<wchar_t> buffer(static_cast<size_t>(std::max(0, length)) + 1);
        GetWindowTextW(edit, buffer.data(), length + 1);
        value.assign(buffer.data());
    }

    DestroyWindow(edit);
    if (luaInlineEditFont_) { DeleteObject(luaInlineEditFont_); luaInlineEditFont_ = nullptr; }

    if (!cancel && widgetEngine_ && !luaInlineEditWidgetId_.empty() && !luaInlineEditStorageKey_.empty())
    {
        widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
            LuaWidgetWideToUtf8(value));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    luaInlineEditWidgetId_.clear();
    luaInlineEditStorageKey_.clear();
    luaInlineEditMultiline_ = false;
    luaInlineEditTextColor_ = RGB(0, 0, 0);
}

/**
 * @brief Lua 内联编辑框的子类化窗口过程
 */
inline LRESULT CALLBACK DesktopApp::LuaInlineEditSubclassProc(
    HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam,
    UINT_PTR subclassId, DWORD_PTR refData)
{
    (void)subclassId;
    auto* app = reinterpret_cast<DesktopApp*>(refData);
    if (!app) return DefSubclassProc(hwnd, message, wParam, lParam);

    switch (message)
    {
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) { app->CommitLuaInlineTextEdit(true); return 0; }
        if (wParam == VK_RETURN)
        {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (!app->luaInlineEditMultiline_ || ctrl)
            {
                app->CommitLuaInlineTextEdit(false);
                return 0;
            }
        }
        break;
    case WM_KILLFOCUS:
        app->CommitLuaInlineTextEdit(false);
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

/**
 * @brief 判断两个窗口是否在同一窗口树中
 * @param parent 父窗口
 * @param window 待检查窗口
 * @return 若 window 是 parent 自身或子窗口则返回 true
 */
inline bool DesktopApp::IsSameWindowTree(HWND parent, HWND window)
{
    return parent != nullptr && window != nullptr && (window == parent || IsChild(parent, window));
}

/**
 * @brief 判断是否为已知的桌面表层窗口
 * @param window 待检查窗口句柄
 * @return 若属于桌面表层窗口体系返回 true
 */
inline bool DesktopApp::IsKnownDesktopSurfaceWindow(HWND window) const
{
    if (!window) return false;
    HWND root = GetAncestor(window, GA_ROOT);
    if (!root) root = window;

    if (IsSameWindowTree(hwnd_, window) || window == hwnd_ || root == hwnd_) return true;
    if (luaInlineEdit_ && (IsSameWindowTree(luaInlineEdit_, window) || root == luaInlineEdit_)) return true;
    if (hintHwnd_ && (IsSameWindowTree(hintHwnd_, window) || root == hintHwnd_)) return true;
    if (controlHwnd_ && (IsSameWindowTree(controlHwnd_, window) || root == controlHwnd_)) return true;

    auto isSurface = [&](HWND candidate) {
        return candidate && (window == candidate || root == candidate || IsChild(candidate, window));
    };
    if (isSurface(desktopWindows_.host) || isSurface(desktopWindows_.progman) ||
        isSurface(desktopWindows_.defView) || isSurface(desktopWindows_.listView))
        return true;

    HWND desktop = GetDesktopWindow();
    return window == desktop || root == desktop;
}

/**
 * @brief 判断指定点是否位于外部可放置窗口上
 * @param clientPoint 客户端坐标点
 * @return 如果是外部窗口返回 true
 */
inline bool DesktopApp::IsExternalDropWindowAt(POINT clientPoint) const
{
    POINT screenPoint = clientPoint;
    ClientToScreen(hwnd_, &screenPoint);
    HWND hit = WindowFromPoint(screenPoint);
    if (!hit || IsKnownDesktopSurfaceWindow(hit)) return false;
    HWND root = GetAncestor(hit, GA_ROOT);
    if (!root) root = hit;
    return IsWindowVisible(root) != FALSE;
}

/**
 * @brief 根据修饰键状态和允许的效果选择拖放效果
 * @param keyState 键盘修饰键状态
 * @param allowed 允许的拖放效果标志
 * @return 选择的 DROPEFFECT
 */
inline DWORD DesktopApp::ChooseDropEffect(DWORD keyState, DWORD allowed) const
{
    if ((keyState & MK_ALT)) return DROPEFFECT_LINK;
    if ((keyState & MK_SHIFT)) return DROPEFFECT_MOVE;
    if ((keyState & MK_CONTROL)) return DROPEFFECT_COPY;

    DWORD available = allowed & (DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK);
    if (!available) available = DROPEFFECT_COPY | DROPEFFECT_MOVE;
    if (available & DROPEFFECT_MOVE) return DROPEFFECT_MOVE;
    if (available & DROPEFFECT_COPY) return DROPEFFECT_COPY;
    return DROPEFFECT_LINK;
}

// ── OLE drag-drop ───────────────────────────────────────────

/**
 * @brief 将屏幕坐标转换为客户端坐标
 * @param screen 屏幕坐标点
 * @return 客户端坐标点
 */
inline POINT DesktopApp::ScreenPointToClient(POINTL screen) const
{
    POINT pt{ screen.x, screen.y };
    if (hwnd_ && IsWindow(hwnd_))
        ScreenToClient(hwnd_, &pt);
    return pt;
}

/**
 * @brief COM QueryInterface 实现
 * @param riid 接口 ID
 * @param object [out] 返回的接口指针
 * @return S_OK 或 E_NOINTERFACE
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::QueryInterface(REFIID riid, void** object)
{
    if (!object) return E_POINTER;
    if (riid == IID_IUnknown || riid == IID_IDropTarget)
    {
        *object = static_cast<IDropTarget*>(this);
        AddRef();
        return S_OK;
    }
    if (riid == IID_IDropSource)
    {
        *object = static_cast<IDropSource*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

/**
 * @brief COM AddRef 实现（递增引用计数）
 * @return 新的引用计数值
 */
inline ULONG STDMETHODCALLTYPE DesktopApp::AddRef()
{
    return static_cast<ULONG>(InterlockedIncrement(&refCount_));
}

/**
 * @brief COM Release 实现（递减引用计数）
 * @return 新的引用计数值
 */
inline ULONG STDMETHODCALLTYPE DesktopApp::Release()
{
    return static_cast<ULONG>(InterlockedDecrement(&refCount_));
}

/**
 * @brief COM IDropTarget::DragEnter 实现
 * @param dataObject 拖放数据对象
 * @param keyState 键盘修饰键状态
 * @param point 鼠标屏幕坐标
 * @param effect [in/out] 拖放效果
 * @return S_OK 或错误码
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::DragEnter(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;

    if (selfDragActive_)
    {
        selfDragReturned_ = true;
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
        }

        // OO hit-test
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            Slot* slot = nullptr;
            HitRegion region = (*it)->HitTestDrag(client, slot);
            if (region != HitRegion::None)
            {
                targetContainer = it->get();
                targetSlot = slot;
                targetRegion = region;
                break;
            }
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        OnPaint();
        return S_OK;
    }

    externalDragActive_ = true;
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
        dragSession_.Begin(nullptr, {}, {}, client, client);
    else
        dragSession_.UpdatePoint(client);

    // OO hit-test for external drop
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        Slot* slot = nullptr;
        HitRegion region = (*it)->HitTestDrag(client, slot);
        if (region != HitRegion::None)
        {
            targetContainer = it->get();
            targetSlot = slot;
            targetRegion = region;
            break;
        }
    }
    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    externalDropHasShortcut_ = false;
    if (dataObject)
    {
        std::vector<std::wstring> paths = GetDropPaths(dataObject);
        externalDropFileCount_ = static_cast<int>(paths.size());
        externalDropHasShortcut_ = std::any_of(paths.begin(), paths.end(),
            [](const std::wstring& path) {
                return _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
            });
    }
    else
    {
        externalDropFileCount_ = 1;
    }

    int mods = 0;
    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
    if (keyState & MK_ALT)     mods |= MK_ALT;
    if (keyState & MK_SHIFT)   mods |= MK_SHIFT;
    dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ChooseDropEffect(keyState, *effect);
    OnPaint();
    return S_OK;
}

/**
 * @brief COM IDropTarget::DragOver 实现
 * @param keyState 键盘修饰键状态
 * @param point 鼠标屏幕坐标
 * @param effect [in/out] 拖放效果
 * @return S_OK 或错误码
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::DragOver(
    DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;

    if (selfDragActive_)
    {
        POINT client = ScreenPointToClient(point);
        if (dragSession_.IsActive())
        {
            dragSession_.UpdatePoint(client);
            dragSession_.UpdateActionFromMods(static_cast<int>(keyState & (MK_CONTROL | MK_ALT | MK_SHIFT)));
        }

        // OO hit-test
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            Slot* slot = nullptr;
            HitRegion region = (*it)->HitTestDrag(client, slot);
            if (region != HitRegion::None)
            {
                targetContainer = it->get();
                targetSlot = slot;
                targetRegion = region;
                break;
            }
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = DROPEFFECT_COPY | DROPEFFECT_MOVE;
        OnPaint();
        return S_OK;
    }

    externalDragActive_ = true;
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
        dragSession_.Begin(nullptr, {}, {}, client, client);
    else
        dragSession_.UpdatePoint(client);

    // OO hit-test for external drop
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
    {
        Slot* slot = nullptr;
        HitRegion region = (*it)->HitTestDrag(client, slot);
        if (region != HitRegion::None)
        {
            targetContainer = it->get();
            targetSlot = slot;
            targetRegion = region;
            break;
        }
    }
    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    int mods = 0;
    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
    if (keyState & MK_ALT)     mods |= MK_ALT;
    if (keyState & MK_SHIFT)   mods |= MK_SHIFT;
    dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = ChooseDropEffect(keyState, *effect);
    OnPaint();
    return S_OK;
}

/**
 * @brief COM IDropTarget::DragLeave 实现
 * @return S_OK
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::DragLeave()
{
    if (selfDragActive_)
    {
        dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        HideDragHintWindow();
        OnPaint();
        return S_OK;
    }
    externalDragActive_ = false;
    externalDropFileCount_ = 0;
    externalDropHasShortcut_ = false;
    EndDragSession();
    HideDragHintWindow();
    OnPaint();
    return S_OK;
}

/**
 * @brief COM IDropTarget::Drop 实现 — 处理拖放完成事件
 * @param dataObject 拖放数据对象
 * @param keyState 键盘修饰键状态
 * @param point 鼠标屏幕坐标
 * @param effect [in/out] 拖放效果
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::Drop(
    IDataObject* dataObject, DWORD keyState, POINTL point, DWORD* effect)
{
    if (!effect) return E_POINTER;
    HideDragHintWindow();

    POINT clientPoint = ScreenPointToClient(point);

    if (selfDragActive_)
    {
        selfDragActive_ = false;
        selfDragReturned_ = true;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        ReleaseCapture();

        if (dragSession_.TargetRegion() == HitRegion::Handoff)
        {
            // ── Shell handoff via IShellFolder::IDropTarget ────
            Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
            ComPtr<IDataObject> dataObj = CreateDataObjectForItems(dragSession_.Items());
            if (dataObj && targetItem)
            {
                ComPtr<IDropTarget> dt;
                if (auto* icon = dynamic_cast<DesktopIcon*>(targetItem))
                {
                    DesktopItem* di = icon->GetDesktopItem();
                    if (di && di->childPidl.get())
                    {
                        PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(di->childPidl.get());
                        desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                            reinterpret_cast<void**>(dt.GetAddressOf()));
                    }
                }
                if (!dt && !targetItem->GetPath().empty())
                {
                    ComPtr<IShellItem> shellItem;
                    if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                        nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                    {
                        shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt));
                    }
                }
                if (dt)
                {
                    POINTL spl{ point.x, point.y };
                    DWORD le = DROPEFFECT_COPY | DROPEFFECT_MOVE;
                    dt->DragEnter(dataObj.Get(), keyState, spl, &le);
                    dt->DragOver(keyState, spl, &le);
                    dt->Drop(dataObj.Get(), keyState, spl, &le);
                }
            }
            ClearSelection();
            EndDragSession();
            ReloadItems();
            *effect = DROPEFFECT_MOVE;
            return S_OK;
        }

        // ── OO dispatch ────────────────────────────────────
        if (dragSession_.TargetContainer())
        {
            int mods = 0;
            if (keyState & MK_CONTROL) mods |= MK_CONTROL;
            if (keyState & MK_ALT)     mods |= MK_ALT;
            if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

            Container* targetContainer = dragSession_.TargetContainer();
            bool needsReload = targetContainer->NeedsShellReloadAfterDrop();
            targetContainer->OnItemsDropped(dragSession_.Items(), dragSession_.Source(),
                dragSession_.TargetSlot(), dragSession_.TargetRegion(), mods);

            SaveLayoutSlots();
            ClearSelection();
            EndDragSession();
            RebuildContainersAndItems();
            if (needsReload)
                ReloadItems();
            else
                InvalidateRect(hwnd_, nullptr, FALSE);
        }
        *effect = DROPEFFECT_MOVE;
        return S_OK;
    }

    // ── External drop ──────────────────────────────────────────
    externalDragActive_ = false;
    externalDropFileCount_ = 0;
    externalDropHasShortcut_ = false;

    std::vector<std::wstring> dropPaths = dataObject ? GetDropPaths(dataObject) : std::vector<std::wstring>();
    if (dropPaths.empty() && dataObject)
        dropPaths = TryGetNonFileDropPaths(dataObject);

    if (dragSession_.TargetRegion() == HitRegion::Handoff && dataObject)
    {
        // ── Handoff on item (desktop OR widget member) ──
        Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
        ComPtr<IDropTarget> dt;
        if (targetItem)
        {
            if (auto* icon = dynamic_cast<DesktopIcon*>(targetItem))
            {
                DesktopItem* di = icon->GetDesktopItem();
                if (di && di->childPidl.get())
                {
                    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(di->childPidl.get());
                    desktopFolder_->GetUIObjectOf(hwnd_, 1, &child, IID_IDropTarget, nullptr,
                        reinterpret_cast<void**>(dt.GetAddressOf()));
                }
            }
            if (!dt && !targetItem->GetPath().empty())
            {
                ComPtr<IShellItem> shellItem;
                if (SUCCEEDED(SHCreateItemFromParsingName(targetItem->GetPath().c_str(),
                    nullptr, IID_PPV_ARGS(&shellItem))) && shellItem)
                {
                    shellItem->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&dt));
                }
            }
        }

        if (dt)
        {
            DWORD le = *effect;
            POINTL spl{ point.x, point.y };
            dt->DragEnter(dataObject, keyState, spl, &le);
            dt->DragOver(keyState, spl, &le);
            dt->Drop(dataObject, keyState, spl, &le);
            *effect = le;
            EndDragSession();
            ReloadItems(false);
            return S_OK;
        }
    }

    if (dataObject && !dropPaths.empty())
    {
        std::vector<std::unique_ptr<ExternalFileItem>> externalItems;
        std::vector<Item*> sourceItems;
        for (const auto& path : dropPaths)
        {
            auto item = std::make_unique<ExternalFileItem>(path);
            sourceItems.push_back(item.get());
            externalItems.push_back(std::move(item));
        }

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        DragSourceList sourceList = BuildDragSourceList(sourceItems, nullptr);
        Container* target = dragSession_.TargetContainer() ? dragSession_.TargetContainer() : GetDesktopGrid();
        HitRegion targetRegion = dragSession_.TargetRegion() != HitRegion::None ? dragSession_.TargetRegion() : HitRegion::Empty;
        DropPreviewList preview = BuildDropPreviewList(sourceList, target,
            dragSession_.TargetContainer() ? dragSession_.TargetSlot() : nullptr, targetRegion, mods, clientPoint);
        bool executed = ExecuteDropPipeline(sourceList, preview);
        if (executed)
        {
            SaveLayoutSlots();
            EndDragSession();
            RebuildContainersAndItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
            *effect = ChooseDropEffect(keyState, *effect);
            return S_OK;
        }
        selfDragOutKeys_.clear();
    }

    *effect = DROPEFFECT_NONE;
    EndDragSession();
    return S_OK;
}

/**
 * @brief COM IDropSource::QueryContinueDrag 实现
 * @param escapePressed 是否按下了 Escape
 * @param keyState 键盘修饰键状态
 * @return DRAGDROP_S_CANCEL、DRAGDROP_S_DROP 或 S_OK
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::QueryContinueDrag(BOOL escapePressed, DWORD keyState)
{
    if (escapePressed) return DRAGDROP_S_CANCEL;
    if ((keyState & (MK_LBUTTON | MK_RBUTTON)) == 0) return DRAGDROP_S_DROP;
    return S_OK;
}

/**
 * @brief COM IDropSource::GiveFeedback 实现
 * @return DRAGDROP_S_USEDEFAULTCURSORS（使用默认光标）
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::GiveFeedback(DWORD)
{
    return DRAGDROP_S_USEDEFAULTCURSORS;
}

/**
 * @brief 从数据对象中提取文件路径列表
 * @param dataObject COM 数据对象
 * @return 文件路径列表
 */
inline std::vector<std::wstring> DesktopApp::GetDropPaths(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    FORMATETC fmt{};
    fmt.cfFormat = CF_HDROP;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (SUCCEEDED(dataObject->GetData(&fmt, &med)))
    {
        HDROP hDrop = static_cast<HDROP>(med.hGlobal);
        UINT count = DragQueryFileW(hDrop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < count; ++i)
        {
            wchar_t path[MAX_PATH]{};
            if (DragQueryFileW(hDrop, i, path, MAX_PATH) > 0)
                paths.push_back(path);
        }
        ReleaseStgMedium(&med);
    }
    return paths;
}

/**
 * @brief 判断 URL 是否指向可下载的文件
 * @param url URL 字符串
 * @param fileName [out] 解析出的文件名
 * @return 是可下载文件返回 true
 */
inline bool DesktopApp::IsFileDownloadUrl(const std::wstring& url, std::wstring& fileName)
{
    const wchar_t* afterScheme = wcschr(url.c_str(), L':');
    if (!afterScheme || afterScheme[1] != L'/' || afterScheme[2] != L'/')
        return false;
    const wchar_t* hostStart = afterScheme + 3;
    const wchar_t* pathStart = wcschr(hostStart, L'/');
    if (!pathStart) return false;

    const wchar_t* p = pathStart + 1;
    const wchar_t* lastSlash = pathStart;
    const wchar_t* queryStart = wcschr(p, L'?');
    const wchar_t* fragStart = wcschr(p, L'#');
    const wchar_t* end = p + wcslen(p);
    if (queryStart && queryStart < end) end = queryStart;
    if (fragStart && fragStart < end) end = fragStart;

    for (const wchar_t* s = p; s < end; ++s)
        if (*s == L'/') lastSlash = s;

    if (lastSlash >= end - 1) return false;
    fileName.assign(lastSlash + 1, end - lastSlash - 1);
    if (fileName.empty()) return false;

    size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return false;

    std::wstring ext = fileName.substr(dot);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));

    static const std::vector<std::wstring> webExts = {
        L".html", L".htm", L".php", L".asp", L".aspx", L".jsp", L".cfm", L".shtml", L".xhtml"
    };
    for (const auto& we : webExts)
        if (ext == we) return false;

    return true;
}

/**
 * @brief 处理 URL 内容：文件链接则下载，否则创建 .lnk
 * @param url URL 字符串
 * @return 临时文件路径
 */
inline std::wstring DesktopApp::HandleUrlContent(const std::wstring& url)
{
    std::wstring result;

    std::wstring fileName;
    if (IsFileDownloadUrl(url, fileName))
    {
        wchar_t tempPath[MAX_PATH]{};
        GetTempPathW(MAX_PATH, tempPath);
        wchar_t destPath[MAX_PATH]{};
        PathCombineW(destPath, tempPath, fileName.c_str());

        for (int i = 0; i < 100; ++i)
        {
            if (i > 0)
            {
                size_t dot = fileName.find_last_of(L'.');
                std::wstring name = dot == std::wstring::npos
                    ? fileName + L" (" + std::to_wstring(i) + L")"
                    : fileName.substr(0, dot) + L" (" + std::to_wstring(i) + L")" + fileName.substr(dot);
                PathCombineW(destPath, tempPath, name.c_str());
            }
            if (GetFileAttributesW(destPath) == INVALID_FILE_ATTRIBUTES)
                break;
        }

        if (SUCCEEDED(URLDownloadToFileW(nullptr, url.c_str(), destPath, 0, nullptr)))
            result = destPath;
    }

    if (!result.empty()) return result;

    std::wstring hostName;
    const wchar_t* afterScheme = wcschr(url.c_str(), L':');
    if (afterScheme && afterScheme[1] == L'/' && afterScheme[2] == L'/')
    {
        const wchar_t* hostStart = afterScheme + 3;
        const wchar_t* hostEnd = wcschr(hostStart, L'/');
        if (!hostEnd) hostEnd = wcschr(hostStart, L'?');
        if (!hostEnd) hostEnd = wcschr(hostStart, L'#');
        if (!hostEnd) hostEnd = hostStart + wcslen(hostStart);
        hostName.assign(hostStart, hostEnd - hostStart);
    }
    if (hostName.size() > 4 && _wcsnicmp(hostName.c_str(), L"www.", 4) == 0)
        hostName = hostName.substr(4);
    if (hostName.empty()) hostName = L"链接";

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    wchar_t lnkPath[MAX_PATH]{};
    for (int i = 0; i < 100; ++i)
    {
        std::wstring name = i == 0
            ? hostName + L".lnk"
            : hostName + L" (" + std::to_wstring(i) + L").lnk";
        PathCombineW(lnkPath, tempPath, name.c_str());
        if (GetFileAttributesW(lnkPath) == INVALID_FILE_ATTRIBUTES)
            break;
    }

    ComPtr<IShellLinkW> shellLink;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&shellLink))))
    {
        shellLink->SetPath(url.c_str());
        shellLink->SetDescription(url.c_str());
        ComPtr<IPersistFile> persistFile;
        if (SUCCEEDED(shellLink.As(&persistFile)))
        {
            if (SUCCEEDED(persistFile->Save(lnkPath, TRUE)))
                result = lnkPath;
        }
    }
    return result;
}

/**
 * @brief 从数据对象中提取 URL 并处理（下载文件或创建 .lnk）
 * @param dataObject COM 数据对象
 * @return 临时文件路径列表
 */
inline std::vector<std::wstring> DesktopApp::TryExtractUrlFromDataObject(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    if (!dataObject) return paths;

    std::wstring url;

    CLIPFORMAT cfUrl = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(L"UniformResourceLocator"));
    FORMATETC fmt{};
    fmt.cfFormat = cfUrl;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (SUCCEEDED(dataObject->GetData(&fmt, &med)) && med.hGlobal)
    {
        const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(med.hGlobal));
        if (data) url = data;
        GlobalUnlock(med.hGlobal);
        ReleaseStgMedium(&med);
    }

    if (url.empty())
    {
        FORMATETC fmtText{};
        fmtText.cfFormat = CF_UNICODETEXT;
        fmtText.dwAspect = DVASPECT_CONTENT;
        fmtText.lindex = -1;
        fmtText.tymed = TYMED_HGLOBAL;
        STGMEDIUM medText{};
        if (SUCCEEDED(dataObject->GetData(&fmtText, &medText)) && medText.hGlobal)
        {
            const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(medText.hGlobal));
            if (data) url = data;
            GlobalUnlock(medText.hGlobal);
            ReleaseStgMedium(&medText);
        }
    }

    if (url.empty()) return paths;

    bool isUrl = (_wcsnicmp(url.c_str(), L"http://", 7) == 0 ||
                  _wcsnicmp(url.c_str(), L"https://", 8) == 0 ||
                  _wcsnicmp(url.c_str(), L"ftp://", 6) == 0);
    if (!isUrl) return paths;

    std::wstring resultPath = HandleUrlContent(url);
    if (!resultPath.empty())
        paths.push_back(resultPath);
    return paths;
}

/**
 * @brief 从数据对象中提取位图图像并保存为 PNG 文件
 * @param dataObject COM 数据对象
 * @return 临时 PNG 文件路径列表
 */
inline std::vector<std::wstring> DesktopApp::TryExtractImageFromDataObject(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    if (!dataObject) return paths;

    FORMATETC fmt{};
    fmt.cfFormat = CF_DIB;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (FAILED(dataObject->GetData(&fmt, &med)) || !med.hGlobal)
        return paths;

    BITMAPINFOHEADER* bmih = static_cast<BITMAPINFOHEADER*>(GlobalLock(med.hGlobal));
    if (!bmih)
    {
        ReleaseStgMedium(&med);
        return paths;
    }

    int colorsUsed = bmih->biClrUsed;
    if (colorsUsed == 0 && bmih->biBitCount <= 8)
        colorsUsed = 1 << bmih->biBitCount;
    int colorTableSize = colorsUsed * sizeof(RGBQUAD);
    if (bmih->biCompression == BI_BITFIELDS)
        colorTableSize = 3 * sizeof(DWORD);

    BYTE* pixelData = reinterpret_cast<BYTE*>(bmih) + bmih->biSize + colorTableSize;

    HDC screenDc = GetDC(nullptr);
    HBITMAP hBitmap = nullptr;

    if (bmih->biCompression == BI_RGB || bmih->biCompression == BI_BITFIELDS)
    {
        hBitmap = CreateDIBitmap(screenDc, bmih, CBM_INIT, pixelData,
            reinterpret_cast<const BITMAPINFO*>(bmih), DIB_RGB_COLORS);
    }

    if (!hBitmap)
    {
        GlobalUnlock(med.hGlobal);
        ReleaseStgMedium(&med);
        ReleaseDC(nullptr, screenDc);
        return paths;
    }

    ComPtr<IWICImagingFactory> wicFactory;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(&wicFactory));

    if (SUCCEEDED(hr))
    {
        ComPtr<IWICBitmap> wicBitmap;
        hr = wicFactory->CreateBitmapFromHBITMAP(hBitmap, nullptr, WICBitmapUseAlpha, &wicBitmap);

        if (SUCCEEDED(hr))
        {
            wchar_t tempPath[MAX_PATH]{};
            GetTempPathW(MAX_PATH, tempPath);
            wchar_t pngPath[MAX_PATH]{};
            SYSTEMTIME st;
            GetLocalTime(&st);
            wchar_t nameBuf[64]{};
            swprintf_s(nameBuf, L"snow_image_%04d%02d%02d_%02d%02d%02d.png",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            PathCombineW(pngPath, tempPath, nameBuf);

            ComPtr<IWICStream> stream;
            hr = wicFactory->CreateStream(&stream);
            if (SUCCEEDED(hr))
            {
                hr = stream->InitializeFromFilename(pngPath, GENERIC_WRITE);
                if (SUCCEEDED(hr))
                {
                    ComPtr<IWICBitmapEncoder> encoder;
                    hr = wicFactory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
                    if (SUCCEEDED(hr))
                    {
                        hr = encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
                        if (SUCCEEDED(hr))
                        {
                            ComPtr<IWICBitmapFrameEncode> frame;
                            hr = encoder->CreateNewFrame(&frame, nullptr);
                            if (SUCCEEDED(hr))
                            {
                                hr = frame->Initialize(nullptr);
                                if (SUCCEEDED(hr))
                                {
                                    hr = frame->WriteSource(wicBitmap.Get(), nullptr);
                                    if (SUCCEEDED(hr))
                                    {
                                        hr = frame->Commit();
                                        if (SUCCEEDED(hr))
                                        {
                                            hr = encoder->Commit();
                                            if (SUCCEEDED(hr))
                                                paths.push_back(pngPath);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    DeleteObject(hBitmap);
    GlobalUnlock(med.hGlobal);
    ReleaseStgMedium(&med);
    ReleaseDC(nullptr, screenDc);
    return paths;
}

/**
 * @brief 从数据对象中提取文本并保存为 UTF-8 .txt 文件
 * @param dataObject COM 数据对象
 * @return 临时 .txt 文件路径列表
 */
inline std::vector<std::wstring> DesktopApp::TryExtractTextFromDataObject(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;
    if (!dataObject) return paths;

    FORMATETC fmt{};
    fmt.cfFormat = CF_UNICODETEXT;
    fmt.dwAspect = DVASPECT_CONTENT;
    fmt.lindex = -1;
    fmt.tymed = TYMED_HGLOBAL;
    STGMEDIUM med{};
    if (FAILED(dataObject->GetData(&fmt, &med)) || !med.hGlobal)
        return paths;

    const wchar_t* data = static_cast<const wchar_t*>(GlobalLock(med.hGlobal));
    if (!data)
    {
        ReleaseStgMedium(&med);
        return paths;
    }

    std::wstring text(data);
    GlobalUnlock(med.hGlobal);
    ReleaseStgMedium(&med);

    size_t start = 0;
    while (start < text.size() && (text[start] == L' ' || text[start] == L'\t' || text[start] == L'\r' || text[start] == L'\n'))
        ++start;
    size_t end = text.size();
    while (end > start && (text[end - 1] == L' ' || text[end - 1] == L'\t' || text[end - 1] == L'\r' || text[end - 1] == L'\n'))
        --end;
    text = text.substr(start, end - start);
    if (text.empty()) return paths;

    bool textIsUrl = (_wcsnicmp(text.c_str(), L"http://", 7) == 0 ||
                     _wcsnicmp(text.c_str(), L"https://", 8) == 0 ||
                     _wcsnicmp(text.c_str(), L"ftp://", 6) == 0);
    if (textIsUrl)
    {
        std::wstring resultPath = HandleUrlContent(text);
        if (!resultPath.empty())
            paths.push_back(resultPath);
        return paths;
    }

    std::wstring firstLine = text;
    size_t nl = firstLine.find_first_of(L"\r\n");
    if (nl != std::wstring::npos) firstLine = firstLine.substr(0, nl);
    if (firstLine.size() > 30) firstLine = firstLine.substr(0, 30);

    for (auto& ch : firstLine)
        if (ch == L'\\' || ch == L'/' || ch == L':' || ch == L'*' || ch == L'?' || ch == L'"' || ch == L'<' || ch == L'>' || ch == L'|')
            ch = L'_';

    std::wstring baseName = firstLine.empty() ? L"snow_text" : firstLine;

    wchar_t tempPath[MAX_PATH]{};
    GetTempPathW(MAX_PATH, tempPath);
    wchar_t txtPath[MAX_PATH]{};

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t timePart[32]{};
    swprintf_s(timePart, L"_%04d%02d%02d_%02d%02d%02d",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);

    std::wstring name = baseName + timePart + L".txt";
    PathCombineW(txtPath, tempPath, name.c_str());

    FILE* f = nullptr;
    if (_wfopen_s(&f, txtPath, L"w,ccs=UTF-8") == 0 && f)
    {
        fputws(text.c_str(), f);
        fclose(f);
        paths.push_back(txtPath);
    }

    return paths;
}

/**
 * @brief 尝试从非文件拖放格式中提取内容，优先：图像 > URL > 文本
 * @param dataObject COM 数据对象
 * @return 临时文件路径列表
 */
inline std::vector<std::wstring> DesktopApp::TryGetNonFileDropPaths(IDataObject* dataObject)
{
    std::vector<std::wstring> paths;

    paths = TryExtractImageFromDataObject(dataObject);
    if (!paths.empty()) return paths;

    paths = TryExtractUrlFromDataObject(dataObject);
    if (!paths.empty()) return paths;

    paths = TryExtractTextFromDataObject(dataObject);
    return paths;
}

/**
 * @brief 从完整路径中提取文件名部分
 * @param path 完整路径
 * @return 文件名
 */
inline std::wstring DesktopApp::FileNameFromPath(const std::wstring& path)
{
    size_t pos = path.find_last_of(L"\\/");
    if (pos == std::wstring::npos) return path;
    return path.substr(pos + 1);
}

/**
 * @brief 匹配待处理文件名（支持快捷方式和副本后缀的模糊匹配）
 * @param itemName 现有项名称
 * @param srcFileName 源文件名
 * @return 是否匹配成功
 */
inline bool DesktopApp::MatchPendingName(const std::wstring& itemName, const std::wstring& srcFileName)
{
    static const std::wstring kShortcutSuffix = L" - 快捷方式";
    static const std::wstring kCopySuffix = L" - 副本";

    auto stripLnk = [](const std::wstring& s) -> std::wstring {
        if (s.size() > 4 && _wcsicmp(s.c_str() + s.size() - 4, L".lnk") == 0)
            return s.substr(0, s.size() - 4);
        return s;
    };
    auto stripExt = [](const std::wstring& s) -> std::wstring {
        size_t dot = s.find_last_of(L'.');
        if (dot == std::wstring::npos || dot == 0) return s;
        return s.substr(0, dot);
    };
    auto stripShortcut = [&](const std::wstring& s) -> std::wstring {
        if (s.size() > kShortcutSuffix.size() &&
            s.compare(s.size() - kShortcutSuffix.size(), kShortcutSuffix.size(), kShortcutSuffix) == 0)
            return s.substr(0, s.size() - kShortcutSuffix.size());
        return s;
    };
    auto stripCopy = [&](const std::wstring& s) -> std::wstring {
        std::wstring value = s;
        size_t paren = value.rfind(L" (");
        if (paren != std::wstring::npos && value.ends_with(L")"))
            value = value.substr(0, paren);
        if (value.size() > kCopySuffix.size() &&
            value.compare(value.size() - kCopySuffix.size(), kCopySuffix.size(), kCopySuffix) == 0)
            return value.substr(0, value.size() - kCopySuffix.size());
        return s;
    };
    auto eqi = [](const std::wstring& a, const std::wstring& b) -> bool {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (towlower(a[i]) != towlower(b[i])) return false;
        return true;
    };

    if (eqi(itemName, srcFileName)) return true;

    std::wstring nameNoLnk = stripLnk(itemName);
    std::wstring srcNoExt = stripExt(srcFileName);

    if (eqi(nameNoLnk, srcFileName)) return true;
    if (eqi(itemName, srcNoExt)) return true;
    if (eqi(nameNoLnk, srcNoExt)) return true;

    std::wstring nameNoShortcut = stripShortcut(itemName);
    std::wstring nameNoLnkNoShortcut = stripShortcut(nameNoLnk);

    if (eqi(nameNoShortcut, srcFileName)) return true;
    if (eqi(nameNoShortcut, srcNoExt)) return true;
    if (eqi(nameNoLnkNoShortcut, srcFileName)) return true;
    if (eqi(nameNoLnkNoShortcut, srcNoExt)) return true;

    std::wstring nameNoCopy = stripCopy(itemName);
    std::wstring nameNoLnkNoCopy = stripCopy(nameNoLnk);
    std::wstring nameNoExtNoCopy = stripCopy(stripExt(itemName));
    if (eqi(nameNoCopy, srcFileName)) return true;
    if (eqi(nameNoCopy, srcNoExt)) return true;
    if (eqi(nameNoLnkNoCopy, srcFileName)) return true;
    if (eqi(nameNoLnkNoCopy, srcNoExt)) return true;
    if (eqi(nameNoExtNoCopy, srcNoExt)) return true;

    return false;
}

// ── Drag hint ────────────────────────────────────────────────

/**
 * @brief 确保拖拽提示窗口已创建
 * @return 窗口是否可用
 */
inline bool DesktopApp::EnsureDragHintWindow()
{
    if (hintHwnd_) return true;
    hintHwnd_ = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kHintWindowClassName, L"", WS_POPUP,
        0, 0, 1, 1, nullptr, nullptr, instance_, nullptr);
    return hintHwnd_ != nullptr;
}

/**
 * @brief 隐藏拖拽提示窗口
 */
inline void DesktopApp::HideDragHintWindow()
{
    if (hintHwnd_) { ShowWindow(hintHwnd_, SW_HIDE); hintTextCache_.clear(); }
}

/**
 * @brief 销毁拖拽提示窗口
 */
inline void DesktopApp::DestroyDragHintWindow()
{
    if (hintHwnd_) { DestroyWindow(hintHwnd_); hintHwnd_ = nullptr; }
}

/**
 * @brief 显示拖拽提示窗口（客户端坐标版本）
 * @param clientPoint 客户端坐标点
 * @param text 提示文本内容
 */
inline void DesktopApp::ShowDragHintWindow(POINT clientPoint, const std::wstring& text)
{
    if (text.empty())
    {
        HideDragHintWindow();
        return;
    }

    // Skip expensive GDI rebuild if text hasn't changed — just move the window
    if (text == hintTextCache_)
    {
        if (hintHwnd_ && IsWindowVisible(hintHwnd_))
        {
            POINT screenPoint = clientPoint;
            ClientToScreen(hwnd_, &screenPoint);
            SetWindowPos(hintHwnd_, HWND_TOPMOST,
                screenPoint.x + 48, screenPoint.y + 22,
                0, 0, SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        }
        return;
    }
    hintTextCache_ = text;

    if (!EnsureDragHintWindow())
    {
        HideDragHintWindow();
        return;
    }

    POINT screenPoint = clientPoint;
    ClientToScreen(hwnd_, &screenPoint);

    HDC screenDc = GetDC(nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(screenDc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
    SelectObject(screenDc, oldFont);

    int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
    int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
    POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8,
            monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
        windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8,
            monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        HideDragHintWindow();
        return;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
        return (static_cast<std::uint32_t>(a) << 24) |
            (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
    };

    const std::uint32_t bg = argb(255, 255, 255, 255);
    const std::uint32_t bd = argb(255, 205, 211, 220);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(y * width) + x] = (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? bd : bg;

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memoryDc, bitmap);
    HGDIOBJ oldMFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(25, 32, 42));
    RECT textRect{ 10, 0, width - 10, height };
    DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (int i = 0; i < width * height; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        pixels[i] = argb(255, r, g, b);
    }

    POINT sourcePoint{ 0, 0 };
    SIZE windowSize{ width, height };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
    ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

    SelectObject(memoryDc, oldMFont);
    SelectObject(memoryDc, oldBmp);
    DeleteDC(memoryDc);
    DeleteObject(bitmap);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDc);
}

/**
 * @brief 显示拖拽提示窗口（屏幕坐标版本）
 * @param screenPoint 屏幕坐标点
 * @param text 提示文本内容
 */
inline void DesktopApp::ShowDragHintWindowScreen(POINT screenPoint, const std::wstring& text)
{
    if (text.empty() || !EnsureDragHintWindow())
    {
        HideDragHintWindow();
        return;
    }

    HDC screenDc = GetDC(nullptr);
    HFONT font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(screenDc, font);
    SIZE textSize{};
    GetTextExtentPoint32W(screenDc, text.c_str(), static_cast<int>(text.size()), &textSize);
    SelectObject(screenDc, oldFont);

    int width = std::clamp(static_cast<int>(textSize.cx + 24), 130, 520);
    int height = std::clamp(static_cast<int>(textSize.cy + 14), 32, 46);
    POINT windowPos{ screenPoint.x + 48, screenPoint.y + 22 };

    HMONITOR monitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
    {
        windowPos.x = std::clamp<LONG>(windowPos.x, monitorInfo.rcWork.left + 8,
            monitorInfo.rcWork.right - static_cast<LONG>(width) - 8);
        windowPos.y = std::clamp<LONG>(windowPos.y, monitorInfo.rcWork.top + 8,
            monitorInfo.rcWork.bottom - static_cast<LONG>(height) - 8);
    }

    BITMAPINFO bitmapInfo{};
    bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
    bitmapInfo.bmiHeader.biWidth = width;
    bitmapInfo.bmiHeader.biHeight = -height;
    bitmapInfo.bmiHeader.biPlanes = 1;
    bitmapInfo.bmiHeader.biBitCount = 32;
    bitmapInfo.bmiHeader.biCompression = BI_RGB;

    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(screenDc, &bitmapInfo, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap || !bits)
    {
        DeleteObject(font);
        ReleaseDC(nullptr, screenDc);
        HideDragHintWindow();
        return;
    }

    auto* pixels = static_cast<std::uint32_t*>(bits);
    auto argb = [](std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) -> std::uint32_t {
        return (static_cast<std::uint32_t>(a) << 24) |
            (static_cast<std::uint32_t>(r) << 16) |
            (static_cast<std::uint32_t>(g) << 8) |
            static_cast<std::uint32_t>(b);
    };

    const std::uint32_t bg = argb(255, 255, 255, 255);
    const std::uint32_t bd = argb(255, 205, 211, 220);
    for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            pixels[(y * width) + x] = (x == 0 || y == 0 || x == width - 1 || y == height - 1) ? bd : bg;

    HDC memoryDc = CreateCompatibleDC(screenDc);
    HGDIOBJ oldBmp = SelectObject(memoryDc, bitmap);
    HGDIOBJ oldMFont = SelectObject(memoryDc, font);
    SetBkMode(memoryDc, TRANSPARENT);
    SetTextColor(memoryDc, RGB(25, 32, 42));
    RECT textRect{ 10, 0, width - 10, height };
    DrawTextW(memoryDc, text.c_str(), -1, &textRect, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS);

    for (int i = 0; i < width * height; ++i)
    {
        std::uint32_t pixel = pixels[i];
        std::uint8_t r = static_cast<std::uint8_t>((pixel >> 16) & 0xff);
        std::uint8_t g = static_cast<std::uint8_t>((pixel >> 8) & 0xff);
        std::uint8_t b = static_cast<std::uint8_t>(pixel & 0xff);
        pixels[i] = argb(255, r, g, b);
    }

    POINT sourcePoint{ 0, 0 };
    SIZE windowSize{ width, height };
    BLENDFUNCTION blend{};
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;

    UpdateLayeredWindow(hintHwnd_, screenDc, &windowPos, &windowSize, memoryDc, &sourcePoint, 0, &blend, ULW_ALPHA);
    ShowWindow(hintHwnd_, SW_SHOWNOACTIVATE);

    SelectObject(memoryDc, oldMFont);
    SelectObject(memoryDc, oldBmp);
    DeleteDC(memoryDc);
    DeleteObject(bitmap);
    DeleteObject(font);
    ReleaseDC(nullptr, screenDc);
}

// ── Widget context menu ────────────────────────────────────

/**
 * @brief 显示 Lua 小部件的编辑器宿主页
 * @param widgetIndex 小部件索引
 */
inline void DesktopApp::ShowWidgetEditorHost(size_t widgetIndex)
{
    if (!settingsWindow_ || widgetIndex >= widgets_.size()) return;
    const auto& widget = widgets_[widgetIndex];
    if (widget.type != DesktopWidgetType::LuaScript) return;
    settingsWindow_->ShowWidgetEditor(widgetIndex, widget.id.c_str(),
        widget.title.c_str(), widget.scriptPath.c_str());
}

/**
 * @brief 显示窗口小部件的右键上下文菜单
 * @param screenPoint 屏幕坐标点
 * @param widgetIndex 小部件索引
 */
inline void DesktopApp::ShowWidgetContextMenu(POINT screenPoint, size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return;
    ClearMenuIcons();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const auto& widget = widgets_[widgetIndex];
    std::vector<LuaWidgetMenuItem> luaMenuItems;

    if (widget.type == DesktopWidgetType::Collection)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpen, L"打开全部");
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetManualCollect, L"立即收集");
        AppendMenuW(menu, MF_STRING | (widget.autoCollect ? MF_CHECKED : 0), kContextWidgetToggleAutoCollect, L"自动收集");
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode, widget.listMode ? L"图标显示" : L"列表显示");
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpenFolder, L"打开文件夹");
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode, widget.listMode ? L"图标显示" : L"列表显示");
        AppendMenuW(menu, MF_STRING, kContextNewMenu, L"新建");
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, L"展开更多选项");
    }
    else if (widget.type == DesktopWidgetType::LuaScript)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetEdit, L"详细设置");
        if (widgetEngine_)
        {
            widgetEngine_->EnsureWidgetLoaded(widget.id, widget.scriptPath);
            luaMenuItems = widgetEngine_->GetContextMenu(widget.id);
            for (size_t i = 0; i < luaMenuItems.size() &&
                kContextLuaWidgetMenuFirst + static_cast<UINT>(i) <= kContextLuaWidgetMenuLast; ++i)
            {
                const auto& item = luaMenuItems[i];
                if (item.separator)
                {
                    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
                    continue;
                }
                UINT flags = MF_STRING | (item.enabled ? 0 : MF_GRAYED);
                AppendMenuW(menu, flags,
                    kContextLuaWidgetMenuFirst + static_cast<UINT>(i),
                    Utf8ToWide(item.label).c_str());
                if (!item.icon.empty())
                {
                    std::wstring icon = Utf8ToWide(item.icon);
                    SetMenuItemIcon(menu,
                        kContextLuaWidgetMenuFirst + static_cast<UINT>(i),
                        icon.c_str());
                }
            }
        }
    }

    HMENU sortMenu = nullptr, wNameMenu = nullptr, wTypeMenu = nullptr, wDateMenu = nullptr;
    if (widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::Collection)
    {
        sortMenu = CreatePopupMenu();
        if (sortMenu)
        {
            wNameMenu = CreatePopupMenu();
            if (wNameMenu)
            {
                AppendMenuW(wNameMenu, MF_STRING, kContextWidgetSortByName, L"正序");
                AppendMenuW(wNameMenu, MF_STRING, kContextWidgetSortByNameDesc, L"反序");
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wNameMenu), L"名称");
            }
            wTypeMenu = CreatePopupMenu();
            if (wTypeMenu)
            {
                AppendMenuW(wTypeMenu, MF_STRING, kContextWidgetSortByType, L"正序");
                AppendMenuW(wTypeMenu, MF_STRING, kContextWidgetSortByTypeDesc, L"反序");
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wTypeMenu), L"类型");
            }
            wDateMenu = CreatePopupMenu();
            if (wDateMenu)
            {
                AppendMenuW(wDateMenu, MF_STRING, kContextWidgetSortByDate, L"正序");
                AppendMenuW(wDateMenu, MF_STRING, kContextWidgetSortByDateDesc, L"反序");
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wDateMenu), L"修改日期");
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), L"排序方式");
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (CanRenameWidget(widget))
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetRename, L"重命名");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, kContextWidgetDelete, L"删除组件");

    SetMenuItemIcon(menu, kContextWidgetOpen, L"");
    SetMenuItemIcon(menu, kContextWidgetManualCollect, L"");
    SetMenuItemIcon(menu, kContextWidgetToggleListMode, widget.listMode ? L"" : L"");
    SetMenuItemIcon(menu, kContextWidgetOpenFolder, L"");
    SetMenuItemIcon(menu, kContextNewMenu, L"");
    SetMenuItemIcon(menu, kContextMoreCommand, L"");
    SetMenuItemIcon(menu, kContextWidgetEdit, L"");
    SetMenuItemIcon(menu, kContextWidgetRename, L"");
    SetMenuItemIcon(menu, kContextWidgetDelete, L"");
    if (sortMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(sortMenu), L"");
        if (wNameMenu)
        {
            SetMenuItemIcon(sortMenu, reinterpret_cast<UINT_PTR>(wNameMenu), L"");
            SetMenuItemIcon(wNameMenu, kContextWidgetSortByName, L"");
            SetMenuItemIcon(wNameMenu, kContextWidgetSortByNameDesc, L"");
        }
        if (wTypeMenu)
        {
            SetMenuItemIcon(sortMenu, reinterpret_cast<UINT_PTR>(wTypeMenu), L"");
            SetMenuItemIcon(wTypeMenu, kContextWidgetSortByType, L"");
            SetMenuItemIcon(wTypeMenu, kContextWidgetSortByTypeDesc, L"");
        }
        if (wDateMenu)
        {
            SetMenuItemIcon(sortMenu, reinterpret_cast<UINT_PTR>(wDateMenu), L"");
            SetMenuItemIcon(wDateMenu, kContextWidgetSortByDate, L"");
            SetMenuItemIcon(wDateMenu, kContextWidgetSortByDateDesc, L"");
        }
    }

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    DestroyMenu(menu);
    ClearMenuIcons();

    if (command >= kContextLuaWidgetMenuFirst && command <= kContextLuaWidgetMenuLast)
    {
        size_t itemIndex = static_cast<size_t>(command - kContextLuaWidgetMenuFirst);
        if (itemIndex < luaMenuItems.size() && widgetEngine_)
        {
            widgetEngine_->InvokeMenu(widgets_[widgetIndex].id, luaMenuItems[itemIndex].id);
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        return;
    }

    switch (command)
    {
    case kContextWidgetOpen:
    {
        POINT clientPoint = screenPoint;
        ScreenToClient(hwnd_, &clientPoint);
        OpenCollectionPopupAt(widgetIndex, clientPoint, L"");
        break;
    }
    case kContextWidgetOpenFolder:
        if (!widget.sourceFolderPath.empty())
            ShellExecuteW(hwnd_, L"open", widget.sourceFolderPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kContextWidgetToggleListMode:
        widgets_[widgetIndex].listMode = !widgets_[widgetIndex].listMode;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[widgetIndex])
            { wc->InvalidateSlots(); break; }
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleAutoCollect:
        widgets_[widgetIndex].autoCollect = !widgets_[widgetIndex].autoCollect;
        if (widgets_[widgetIndex].autoCollect)
        {
            EnforceSingleAutoCollectFileCategory(widgetIndex);
            CollectFileCategoryWidget(widgetIndex, false);
        }
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetManualCollect:
        if (!CollectFileCategoryWidget(widgetIndex, true))
            MessageBeep(MB_ICONINFORMATION);
        break;
    case kContextWidgetRename:
        if (CanRenameWidget(widgets_[widgetIndex]))
        {
            SelectWidgetOnly(widgetIndex);
            BeginRenameSelected();
        }
        break;
    case kContextWidgetEdit:
        ShowWidgetEditorHost(widgetIndex);
        break;
    case kContextWidgetDelete:
    {
        if (widgets_[widgetIndex].type == DesktopWidgetType::LuaScript && widgetEngine_)
            widgetEngine_->UnloadWidget(widgets_[widgetIndex].id);
        // Move widget's itemKeys back to desktop by just removing the widget
        widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(widgetIndex));
        EnsureNavTabOrder();
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    }
    case kContextNewMenu:
        if (widgetIndex < widgets_.size() && widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping &&
            !widgets_[widgetIndex].sourceFolderPath.empty())
        {
            ShowNewMenuAndInvoke(screenPoint, widgets_[widgetIndex].sourceFolderPath);
            RefreshFolderMappingWidget(widgetIndex);
            RebuildContainersAndItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextMoreCommand:
        if (widgetIndex < widgets_.size() && widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping &&
            !widgets_[widgetIndex].sourceFolderPath.empty())
        {
            ShowShellContextMenuForPath(widgets_[widgetIndex].sourceFolderPath, screenPoint);
        }
        break;
    case kContextWidgetSortByName:
        SortWidgetContents(widgetIndex, 0, true);
        break;
    case kContextWidgetSortByNameDesc:
        SortWidgetContents(widgetIndex, 0, false);
        break;
    case kContextWidgetSortByType:
        SortWidgetContents(widgetIndex, 1, true);
        break;
    case kContextWidgetSortByTypeDesc:
        SortWidgetContents(widgetIndex, 1, false);
        break;
    case kContextWidgetSortByDate:
        SortWidgetContents(widgetIndex, 2, true);
        break;
    case kContextWidgetSortByDateDesc:
        SortWidgetContents(widgetIndex, 2, false);
        break;
    default:
        break;
    }
}

// ── Tray ────────────────────────────────────────────────────

/**
 * @brief 添加系统托盘图标
 * @param force 是否强制重新添加
 */
inline void DesktopApp::AddTrayIcon(bool force)
{
    HWND owner = controlHwnd_ ? controlHwnd_ : hwnd_;
    if (!owner || !IsWindow(owner)) return;
    if (trayIconAdded_ && !force) return;

    if (force && trayIconAdded_)
    {
        NOTIFYICONDATAW del{};
        del.cbSize = sizeof(del);
        del.hWnd = trayIconOwnerHwnd_ ? trayIconOwnerHwnd_ : owner;
        del.uID = kTrayIconId;
        Shell_NotifyIconW(NIM_DELETE, &del);
        trayIconAdded_ = false;
    }

    if (!trayIcon_)
    {
        trayIcon_ = static_cast<HICON>(LoadImageW(
            GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON_SMALL),
            IMAGE_ICON, GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    }

    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = owner;
    data.uID = kTrayIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    data.uCallbackMessage = kTrayCallbackMessage;
    data.hIcon = trayIcon_;
    wcscpy_s(data.szTip, L"SnowDesktop");
    if (Shell_NotifyIconW(NIM_ADD, &data))
    {
        trayIconAdded_ = true;
        trayIconOwnerHwnd_ = owner;
        data.uVersion = NOTIFYICON_VERSION_4;
        Shell_NotifyIconW(NIM_SETVERSION, &data);
    }
}

/**
 * @brief 移除系统托盘图标
 */
inline void DesktopApp::RemoveTrayIcon()
{
    if (!trayIconAdded_) return;
    NOTIFYICONDATAW data{};
    data.cbSize = sizeof(data);
    data.hWnd = trayIconOwnerHwnd_ ? trayIconOwnerHwnd_ : hwnd_;
    data.uID = kTrayIconId;
    Shell_NotifyIconW(NIM_DELETE, &data);
    trayIconAdded_ = false;
}

/**
 * @brief 显示系统托盘气泡通知
 * @param title 通知标题
 * @param message 通知内容
 */
inline void DesktopApp::ShowBalloonNotification(const std::wstring& title, const std::wstring& message)
{
    HWND owner = controlHwnd_ ? controlHwnd_ : hwnd_;
    if (!owner || !IsWindow(owner)) return;

    AddTrayIcon();

    NOTIFYICONDATAW nid{};
    nid.cbSize = sizeof(nid);
    nid.hWnd = owner;
    nid.uID = kTrayIconId;
    nid.uFlags = NIF_INFO;
    nid.dwInfoFlags = NIIF_INFO;
    wcsncpy_s(nid.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(nid.szInfo, message.c_str(), _TRUNCATE);
    nid.uTimeout = 10000;
    Shell_NotifyIconW(NIM_MODIFY, &nid);
}

/**
 * @brief 处理系统托盘回调消息
 * @param lParam 消息参数（含右键点击、双击等事件）
 */
inline void DesktopApp::OnTrayCallback(LPARAM lParam)
{
    UINT message = LOWORD(lParam);
    if (message == WM_CONTEXTMENU || message == WM_RBUTTONUP)
    {
        POINT pt{};
        GetCursorPos(&pt);
        ShowTrayMenu(pt);
    }
    else if (message == WM_LBUTTONDBLCLK)
    {
        ReloadItems();
    }
}

/**
 * @brief 显示系统托盘右键菜单
 * @param screenPoint 屏幕坐标点
 */
inline void DesktopApp::ShowTrayMenu(POINT screenPoint)
{
    ClearMenuIcons();
    HMENU menu = CreatePopupMenu();

    HMENU iconMenu = CreatePopupMenu();
    if (iconMenu)
    {
        struct IS { UINT cmd; const wchar_t* clsid; const wchar_t* label; };
        const IS items[] = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC, L"计算机" },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles, L"用户的文件" },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork, L"网络" },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel, L"控制面板" },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin, L"回收站" },
        };
        for (const auto& s : items)
        {
            UINT flags = MF_STRING;
            DWORD val = 0;
            if (TryReadDesktopIconRegistryValueAnyRoot(s.clsid, val))
            { if (val == 0) flags |= MF_CHECKED; }
            else
            {
                static const std::unordered_map<std::wstring, bool> defVis = {
                    { kDesktopIconClsidThisPC, false }, { kDesktopIconClsidUserFiles, false },
                    { kDesktopIconClsidNetwork, false }, { kDesktopIconClsidControlPanel, false },
                    { kDesktopIconClsidRecycleBin, true },
                };
                auto it = defVis.find(s.clsid);
                if (it != defVis.end() && it->second) flags |= MF_CHECKED;
            }
            AppendMenuW(iconMenu, flags, s.cmd, s.label);
        }
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), L"桌面图标设置");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        bool nativeActive = !customDesktopVisible_;
        AppendMenuW(menu, MF_STRING | (nativeActive ? MF_CHECKED : 0),
            kTraySwitchNativeCommand, L"切换原生桌面");
        AppendMenuW(menu, MF_STRING | (nativeActive ? 0 : MF_CHECKED),
            kTraySwitchCustomCommand, L"切换软件桌面");
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTraySettingsCommand, L"设置");
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, L"退出软件");

    SetForegroundWindow(controlHwnd_ ? controlHwnd_ : hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, controlHwnd_ ? controlHwnd_ : hwnd_, nullptr);

    if (iconMenu) DestroyMenu(iconMenu);
    DestroyMenu(menu);
    ClearMenuIcons();
    RestoreDesktopWindowLayer();

    switch (command)
    {
    case kTraySwitchNativeCommand:
        customDesktopVisible_ = false;
        KillTimer(controlHwnd_, kDesktopHostWatchTimerId);
        SaveLayoutSlots();
        HideDragHintWindow();
        RestoreExplorerIcons();
        ShowWindow(hwnd_, SW_HIDE);
        break;
    case kTraySwitchCustomCommand:
        customDesktopVisible_ = true;
        HideExplorerIcons();
        ShowWindow(hwnd_, SW_SHOW);
        SetTimer(controlHwnd_, kDesktopHostWatchTimerId, kDesktopHostWatchIntervalMs, nullptr);
        InvalidateRect(hwnd_, nullptr, TRUE);
        ReloadItems();
        break;
    case kTraySettingsCommand:
        ShowSettingsWindow();
        break;
    case kTrayExitCommand:
        if (settingsWindow_)
            settingsWindow_->ShowExitConfirm();
        else
            RequestExit();
        break;
    case kTrayDesktopIconThisPC:
    case kTrayDesktopIconUserFiles:
    case kTrayDesktopIconNetwork:
    case kTrayDesktopIconControlPanel:
    case kTrayDesktopIconRecycleBin:
    {
        struct TV { UINT cmd; const wchar_t* clsid; };
        static const TV tv[] = {
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin },
        };
        for (const auto& t : tv)
        {
            if (t.cmd == command)
            {
                DWORD val = 0;
                bool visible = true;
                if (TryReadDesktopIconRegistryValueAnyRoot(t.clsid, val))
                    visible = (val == 0);
                WriteDesktopIconRegistryValue(t.clsid, !visible);
                ReloadItems();
                break;
            }
        }
        break;
    }
    default:
        break;
    }
}
