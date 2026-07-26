/**
 * @file app_interact.h
 * @brief DesktopApp 交互与拖放操作的内联实现
 *
 * 本文件包含 DesktopApp 类的所有交互处理内联方法，包括鼠标事件、键盘事件、
 * 拖放操作、重命名、窗口小部件、集合弹窗以及系统托盘等功能。
 * 该文件在 app.h 中类定义之后通过 #include 包含。
 */

#pragma once
#include "../crashlog.h"
// Inline implementations for DesktopApp — Interaction & Tray.
// This file is included by app.h after the class definition.

#include "drop_model.h"
#include "../widgets/collection_group_rules.h"
#include "../core/slot_contract.h"
#include <imm.h>
#include <wincodec.h>
#include <urlmon.h>

// ── Interaction ─────────────────────────────────────────────

inline snowdesktop::slot_contract::DragPayloadKind
ClassifySlotDragPayload(const DragSourceList& sourceList)
{
    bool collectionWidgetsOnly = false;
    if (sourceList.hasWidgets)
    {
        collectionWidgetsOnly = true;
        bool sawWidget = false;
        for (const auto& entry : sourceList.entries)
        {
            if (entry.kind != DropSourceKind::Widget)
                continue;
            sawWidget = true;
            if (entry.fromDock)
            {
                collectionWidgetsOnly =
                    collectionWidgetsOnly &&
                    entry.dockEntryType ==
                        DockEntryType::Collection;
                continue;
            }
            auto* widget =
                dynamic_cast<Widget*>(entry.item);
            DesktopWidget* data =
                widget ? widget->GetWidgetData() : nullptr;
            collectionWidgetsOnly =
                collectionWidgetsOnly && data &&
                data->type ==
                    DesktopWidgetType::Collection;
        }
        collectionWidgetsOnly =
            sawWidget && collectionWidgetsOnly;
    }
    return snowdesktop::slot_contract::
        ClassifyPayload({
            sourceList.hasDesktopIcons,
            sourceList.hasFolderEntries,
            sourceList.hasExternalFiles,
            sourceList.hasWidgets,
            collectionWidgetsOnly,
            sourceList.hasCollectionGroupEntries,
            sourceList.hasFileGroupEntries,
        });
}

inline snowdesktop::slot_contract::DragPayloadKind
SlotPayloadForWidgetType(DesktopWidgetType type)
{
    return snowdesktop::slot_contract::
        PayloadForWidgetType(type);
}

inline bool AcceptsSlotSurfaceDrop(
    Container* container,
    const DragSourceList& sourceList)
{
    namespace contract =
        snowdesktop::slot_contract;
    if (!container)
        return false;
    const auto payload =
        ClassifySlotDragPayload(sourceList);
    if (payload ==
        contract::DragPayloadKind::Count)
        return false;
    const auto sourceSurface =
        sourceList.origin
            ? sourceList.origin->
                GetSlotSurfaceKind()
            : (sourceList.hasExternalFiles
                ? contract::SlotSurfaceKind::External
                : contract::SlotSurfaceKind::Desktop);
    const auto targetSurface =
        container->GetSlotSurfaceKind();
    const auto relation =
        contract::ClassifyRelation(
            sourceSurface, targetSurface,
            sourceList.origin == container);
    return contract::AcceptsSlotDrop(
        sourceSurface, payload,
        targetSurface, relation);
}

inline bool AcceptsExternalSlotSurfaceDrop(
    Container* container)
{
    namespace contract =
        snowdesktop::slot_contract;
    if (!container)
        return false;
    return contract::AcceptsSlotDrop(
        contract::SlotSurfaceKind::External,
        contract::DragPayloadKind::ExternalFile,
        container->GetSlotSurfaceKind(),
        contract::DragRelation::ExternalIngress);
}

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
inline float DesktopApp::GetWidgetCellScale(const DesktopWidget& widget) const
{
    return widget.cellScale;
}

inline RECT DesktopApp::GetStandaloneWidgetFrameRect(const DesktopWidget& widget) const
{
    RECT rect = widget.bounds;
    const float cellScale = GetWidgetCellScale(widget);
    for (const auto& page : gridPages_)
    {
        if (page.id != widget.gridCell.pageId) continue;
        int halfGapX = std::max(ScaleWidgetCu(2.0f, cellScale), page.gapX / 2);
        int halfGapY = std::max(ScaleWidgetCu(2.0f, cellScale), page.gapY / 2);
        rect.left   -= halfGapX;
        rect.top    -= halfGapY;
        rect.right  += halfGapX;
        rect.bottom += halfGapY;
        break;
    }
    const int inset = ScaleWidgetCu(4.0f, cellScale);
    if (rect.right - rect.left > inset * 4 && rect.bottom - rect.top > inset * 4)
        InflateRect(&rect, -inset, -inset);
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
    const float cellScale = GetWidgetCellScale(widget);
    const float barHeight = settingsWindow_ ? settingsWindow_->GetPersonalization().barHeight : 24.0f;
    const int handleHeight = ScaleWidgetCu(barHeight, cellScale);
    return {
        frame.left + ScaleWidgetCu(4.0f, cellScale),
        std::max<LONG>(frame.top,
            frame.bottom - handleHeight - ScaleWidgetCu(2.0f, cellScale)),
        frame.right - ScaleWidgetCu(4.0f, cellScale),
        frame.bottom - ScaleWidgetCu(2.0f, cellScale)
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
    const float barHeight = settingsWindow_ ? settingsWindow_->GetPersonalization().barHeight : 24.0f;
    const int handleWidth = ScaleWidgetCu(barHeight, GetWidgetCellScale(widget));
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

inline std::wstring LuaWidgetUtf8ToWide(const std::string& value)
{
    if (value.empty()) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()),
        result.data(), len);
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

inline std::vector<LuaDesktopItemInfo> DesktopApp::BuildLuaEverythingSearch(const std::string& query, int maxResults) const
{
    std::vector<LuaDesktopItemInfo> result;
    std::wstring queryWide = LuaWidgetUtf8ToWide(query);
    if (queryWide.empty() || maxResults <= 0)
        return result;
    std::unordered_set<std::wstring> seenPaths;
    DWORD limit = static_cast<DWORD>(std::clamp(maxResults, 1, 200));
    for (const auto& entry : SearchEverythingCached(queryWide, limit))
    {
        std::wstring normalizedPath = ToUpperInvariant(entry.path);
        if (normalizedPath.empty() || seenPaths.contains(normalizedPath))
            continue;
        seenPaths.insert(std::move(normalizedPath));

        LuaDesktopItemInfo info;
        info.id = LuaWidgetWideToUtf8(entry.path);
        info.title = LuaWidgetWideToUtf8(entry.name);
        info.path = LuaWidgetWideToUtf8(entry.path);
        info.source = "Everything";
        info.type = entry.isDirectory ? "folder" : "file";
        info.selected = false;
        result.push_back(std::move(info));
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
    return snowdesktop::item_location::Reveal(hwnd_, path);
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
        widget.scriptTitle = title;
        if (!widget.customTitle.empty()) return;
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
    luaInlineEditOriginalText_ = initial;
    luaInlineEditMultiline_ = request.multiline;
    luaInlineEditLiveUpdate_ = request.liveUpdate;
    luaInlineEditTextColor_ = RGB((request.textColor >> 16) & 0xFF,
        (request.textColor >> 8) & 0xFF, request.textColor & 0xFF);
    luaInlineEditBackgroundColor_ = RGB((request.backgroundColor >> 16) & 0xFF,
        (request.backgroundColor >> 8) & 0xFF, request.backgroundColor & 0xFF);
    if (luaInlineEditBackgroundBrush_) DeleteObject(luaInlineEditBackgroundBrush_);
    luaInlineEditBackgroundBrush_ = CreateSolidBrush(luaInlineEditBackgroundColor_);

    if (luaInlineEditFont_) DeleteObject(luaInlineEditFont_);
    const int editFontSize = std::clamp(
        static_cast<int>(std::round(request.fontSize)), 9, 96);
    luaInlineEditFont_ = CreateFontW(-editFontSize, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
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
    dragSession_.InvalidateStaticScene();
    dragRenderCache_.Reset();
}

/**
 * @brief 结束当前拖拽会话，重置拖拽渲染缓存
 */
inline void DesktopApp::EndDragSession()
{
    if (hwnd_)
    {
        KillTimer(hwnd_, kDockHandoffDwellTimerId);
        KillTimer(
            hwnd_, kCollectionGroupTabDwellTimerId);
    }
    dockHandoffDwellIndex_ = static_cast<size_t>(-1);
    dockHandoffDwellStartTick_ = 0;
    dockHandoffDwellReady_ = false;
    collectionGroupTabDwellWidgetIndex_ =
        static_cast<size_t>(-1);
    collectionGroupTabDwellId_.clear();
    collectionGroupTabDwellTick_ = 0;
    dragSession_.End();
    dragRenderCache_.Reset();
    // 清除拖放预览缓存
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
    cachedDropPreviewTarget_ = nullptr;
    cachedDropPreviewSlot_ = nullptr;
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 显示设置窗口
 */
inline void DesktopApp::ShowSettingsWindow()
{
    if (settingsWindow_)
    {
        showSettingsPending_ = false;
        settingsWindow_->Show();
    }
    else
    {
        showSettingsPending_ = true;
    }
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

inline void DesktopApp::LoadGeneralSettingsAndApply()
{
    const bool dockEnabled = generalSettings_.dockEnabled;
    GeneralSettings settings;
    LoadGeneralSettings(GetGeneralSettingsPath().c_str(), settings);
    generalSettings_ = settings;
    if (std::strcmp(generalSettings_.language, "system") != 0 &&
        !Locale::Instance().HasLanguage(generalSettings_.language))
    {
        std::strncpy(generalSettings_.language, "system",
            sizeof(generalSettings_.language) - 1);
        generalSettings_.language[sizeof(generalSettings_.language) - 1] = '\0';
    }
    Locale::Instance().SetLanguage(generalSettings_.language);
    generalSettings_.dockEnabled = dockEnabled;
    generalSettings_.quickNavTheme = std::clamp(generalSettings_.quickNavTheme, 0, 3);
    SetSoftwareDesktopEnabled(generalSettings_.softwareDesktopEnabled, false);
    ApplyQuickNavigationAppearance();
}

inline void DesktopApp::ApplyQuickNavigationAppearance()
{
    PersonalizationSettings globalAppearance;
    if (settingsWindow_)
    {
        globalAppearance = settingsWindow_->GetPersonalization();
    }
    else
    {
        globalAppearance = PersonalizationSettings::DarkPreset();
        LoadPersonalization(GetPersonalizationPath().c_str(), globalAppearance);
    }
    constexpr int quickNavPresetIds[] = {
        kAppearancePresetDark, kAppearancePresetLight,
        kAppearancePresetAcrylicDark, kAppearancePresetAcrylicLight
    };
    const int presetId = globalAppearance.backgroundPreset == kAppearancePresetCustom
        ? quickNavPresetIds[std::clamp(generalSettings_.quickNavTheme, 0, 3)]
        : globalAppearance.backgroundPreset;
    const PersonalizationSettings appearance =
        MakeQuickNavigationAppearancePreset(presetId);

    const float luminance = appearance.widgetBgR * 0.2126f +
        appearance.widgetBgG * 0.7152f + appearance.widgetBgB * 0.0722f;
    quickNavLightTheme_ = (presetId == kAppearancePresetLight ||
        presetId == kAppearancePresetAcrylicLight) ||
        luminance >= 0.55f;
    quickNavGlassTheme_ = appearance.glassEnabled;
    quickNavBlurRadius_ = std::clamp(appearance.glassBlurRadius, 4.0f, 48.0f);
    quickNavAppearance_ = appearance;
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        UpdateQuickNavigationBackdrop();
}

inline void DesktopApp::LoadDockSettingsAndApply()
{
    DockSettings settings;
    LoadDockSettings(GetDockSettingsPath().c_str(), settings);
    SetSystemTaskbarAutoHideEnabled(settings.systemTaskbarAutoHide);
    settings.systemTaskbarAutoHide = IsSystemTaskbarAutoHideEnabled();
    SetSystemTaskbarAlignmentCentered(settings.systemTaskbarAlignment == 1);
    settings.systemTaskbarAlignment = IsSystemTaskbarAlignmentCentered() ? 1 : 0;
    dockSettings_ = settings;
    ApplyFloatingDockHotkey();
    systemTaskbarWindowStateChangedTick_.fetch_add(1,
        std::memory_order_relaxed);
    RefreshSystemTaskbarAppearance(true);
    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, FALSE);
}

inline void DesktopApp::LoadCategorySettingsAndApply()
{
    CategorySettings settings = CategorySettings::Defaults();
    LoadCategorySettings(GetCategorySettingsPath().c_str(), settings);
    categorySettings_ = settings;

    for (auto& c : containers_)
    {
        if (auto* fc = dynamic_cast<FileCategories*>(c.get()))
            fc->InvalidateCategoryCache();
        else if (auto* mapping =
                     dynamic_cast<FolderMapping*>(c.get()))
            mapping->InvalidateFilterCache();
        else if (auto* group =
                     dynamic_cast<FileGroup*>(c.get()))
            group->InvalidateHostedView();
    }
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

inline void DesktopApp::ApplyLanguageChange()
{
    LoadCategorySettingsAndApply();
    if (settingsWindow_)
        settingsWindow_->ApplyLanguageChange();
    if (quickNavigationHwnd_ && IsWindow(quickNavigationHwnd_))
        SetWindowTextW(quickNavigationHwnd_, _LW("app.interact.snow_nav_title"));
    if (quickNavigationSearchEdit_ && IsWindow(quickNavigationSearchEdit_))
    {
        SendMessageW(quickNavigationSearchEdit_, EM_SETCUEBANNER, TRUE,
            reinterpret_cast<LPARAM>(_LW("app.nav.search_hint")));
    }

    bool titleChanged = false;
    for (auto& widget : widgets_)
    {
        std::wstring defaultTitle;
        switch (widget.type)
        {
        case DesktopWidgetType::Collection:
            defaultTitle = _LW("widget.collection");
            break;
        case DesktopWidgetType::CollectionGroup:
            defaultTitle = _LW("widget.collection_group");
            break;
        case DesktopWidgetType::FileGroup:
            defaultTitle = _LW("widget.file_group");
            break;
        case DesktopWidgetType::FileCategories:
            defaultTitle = _LW("widget.desktop_files");
            break;
        case DesktopWidgetType::Guide:
            defaultTitle = _LW("app.guide.title");
            break;
        case DesktopWidgetType::LuaScript:
            if (widgetEngine_ && !widget.scriptPath.empty())
            {
                if (!widgetEngine_->ReloadWidget(widget.id))
                    widgetEngine_->EnsureWidgetLoaded(widget.id, widget.scriptPath);
                widgetEngine_->NotifyLanguageChanged(widget.id);
                const auto& runtimeWidgets = widgetEngine_->GetWidgets();
                auto runtime = std::find_if(runtimeWidgets.begin(), runtimeWidgets.end(),
                    [&](const LuaWidget& loaded) {
                        return loaded.widgetId == widget.id;
                    });
                if (runtime != runtimeWidgets.end())
                    defaultTitle = Utf8ToWide(runtime->name);
            }
            break;
        case DesktopWidgetType::FolderMapping:
        default:
            break;
        }

        if (widget.customTitle.empty() &&
            !defaultTitle.empty() &&
            (widget.type != DesktopWidgetType::LuaScript ||
                widget.scriptTitle.empty()) &&
            widget.title != defaultTitle)
        {
            widget.title = std::move(defaultTitle);
            titleChanged = true;
        }
    }

    if (titleChanged)
        SaveLayoutSlots();
    if (quickNavigationOpen_)
        InvalidateQuickNavigationWindow();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::ToggleDesktopIconsVisibility()
{
    desktopIconsHidden_ = !desktopIconsHidden_;
    // The control-window timer also maintains the Explorer taskbar hook and
    // the blurred desktop background. Keep it alive while icons are hidden.
    ClearHiddenHint();

    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::ShowHiddenHint()
{
    if (!generalSettings_.doubleClickHideDesktop) return;
    showHiddenHint_ = true;
    hiddenHintStartTick_ = GetTickCount();
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kHiddenHintTimerId, 100, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

inline void DesktopApp::ClearHiddenHint()
{
    showHiddenHint_ = false;
    hiddenHintStartTick_ = 0;
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kHiddenHintTimerId);
}

inline void DesktopApp::ShowWidgetAddedHint()
{
    showWidgetAddedHint_ = true;
    widgetAddedHintStartTick_ = GetTickCount();
    if (hwnd_ && IsWindow(hwnd_))
    {
        SetTimer(hwnd_, kWidgetAddedHintTimerId, 100, nullptr);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }
}

inline void DesktopApp::ClearWidgetAddedHint()
{
    showWidgetAddedHint_ = false;
    widgetAddedHintStartTick_ = 0;
    if (hwnd_ && IsWindow(hwnd_))
        KillTimer(hwnd_, kWidgetAddedHintTimerId);
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

    const bool suppressDesktopWidgetTargets =
        SuppressDesktopWidgetDragTargets();
    const bool groupedEntryDrag =
        dragSession_.SourceList().
            hasCollectionGroupEntries ||
        dragSession_.SourceList().
            hasFileGroupEntries;
    const bool popupHit =
        !suppressDesktopWidgetTargets &&
        !groupedEntryDrag &&
        HitTestPopupForDrag(clientPoint, targetContainer, targetSlot, targetRegion);

    if (!popupHit && !targetContainer)
    {
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            if (!AcceptsSlotSurfaceDrop(
                    it->get(),
                    dragSession_.SourceList()))
                continue;
            if (suppressDesktopWidgetTargets &&
                (dynamic_cast<DesktopGrid*>(it->get()) ||
                 dynamic_cast<WidgetContainer*>(it->get())))
                continue;
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
    InvalidateFloatingDockWindow(true);
}

/**
 * @brief 在重建容器后重新绑定拖拽源
 * @note 用于在容器重建后恢复拖拽会话的源引用
 */
inline void DesktopApp::RebindDragSourceAfterRebuild()
{
    if (!dragSession_.IsActive()) return;

    Container* source = nullptr;
    FileGroup* sourceFileGroup = nullptr;
    const DragSourceList& oldSourceList = dragSession_.SourceList();
    // External OLE drags do not have an internal source. Keep the session active
    // with empty source bindings; the next DragOver will rebuild its target.
    if (oldSourceList.Empty()) return;

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
        const size_t fileGroupIndex =
            FindFileGroupIndexForChild(
                oldSourceList.originWidgetId);
        if (fileGroupIndex < widgets_.size())
        {
            for (auto& c : containers_)
            {
                auto* group =
                    dynamic_cast<FileGroup*>(c.get());
                if (group &&
                    group->GetWidgetData() ==
                        &widgets_[fileGroupIndex])
                {
                    source = group->
                        GetSourceContainerById(
                            oldSourceList.originWidgetId);
                    sourceFileGroup = group;
                    break;
                }
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

    std::vector<Item*> reboundItems =
        sourceFileGroup
            ? sourceFileGroup->
                GetHostedSelectedItemsForSource(
                    oldSourceList.originWidgetId)
            : source->GetSelectedItems();
    if (reboundItems.empty())
    {
        EndDragSession();
        return;
    }
    DragSourceList reboundList = BuildDragSourceList(reboundItems, source);
    dragSession_.RebindSource(source, std::move(reboundItems), std::move(reboundList));
}

/**
 * @brief 拖拽时优先检查集合弹窗命中（弹窗遮挡的容器不应被穿透命中）。
 * @param client 客户端坐标
 * @param[out] targetContainer 命中的容器
 * @param[out] targetSlot 命中的槽位
 * @param[out] targetRegion 命中的区域
 * @return 命中弹窗返回 true，否则 false
 */
inline bool DesktopApp::HitTestPopupForDrag(POINT client,
    Container*& targetContainer, Slot*& targetSlot, HitRegion& targetRegion)
{
    if (popupWidgetIndex_ >= widgets_.size()) return false;

    RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
    if (!PtInRect(&popup, client)) return false;

    WidgetContainer* popupContainer = nullptr;
    for (auto& c : containers_)
    {
        popupContainer = dynamic_cast<WidgetContainer*>(c.get());
        if (popupContainer && popupContainer->GetWidgetData() == &widgets_[popupWidgetIndex_])
            break;
        popupContainer = nullptr;
    }
    if (!popupContainer) return false;

    std::vector<std::wstring> popupKeys = GetPopupItemKeys(widgets_[popupWidgetIndex_]);
    RECT content = GetCollectionPopupContentRect(popup);
    targetContainer = popupContainer;
    targetSlot = nullptr;
    targetRegion = HitRegion::None;
    if (!PtInRect(&content, client))
        return true;

    size_t slotIndex = 0;
    RECT slotBounds = content;
    HitRegion region = HitRegion::Empty;
    Item* handoffItem = nullptr;

    if (popupKeys.empty())
    {
        popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, content, 0);
        targetSlot = popupDragTargetSlot_.get();
        targetRegion = HitRegion::Empty;
        return true;
    }

    for (size_t i = 0; i < popupKeys.size(); ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popup, i);
        RECT clipped{};
        if (!IntersectRect(&clipped, &itemRect, &content) ||
            !PtInRect(&clipped, client))
            continue;

        size_t itemIndex = FindItemIndexByKey(popupKeys[i]);
        if (itemIndex != static_cast<size_t>(-1) && !items_[itemIndex].selected)
        {
            RECT iconRect = GetItemIconRect(itemRect);
            RECT handoffRect = { iconRect.left - 4, iconRect.top - 2,
                                 iconRect.right + 4, iconRect.bottom + 4 };
            if (PtInRect(&handoffRect, client))
            {
                region = HitRegion::Handoff;
                handoffItem = popupContainer->GetMemberItem(i);
            }
        }

        slotIndex = i;
        slotBounds = itemRect;
        if (region != HitRegion::Handoff)
        {
            region = client.x < itemRect.left + (itemRect.right - itemRect.left) / 2
                ? HitRegion::SortBefore : HitRegion::SortAfter;
        }
        popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
        if (handoffItem)
            popupDragTargetSlot_->SetItem(handoffItem);
        targetSlot = popupDragTargetSlot_.get();
        targetRegion = region;
        return true;
    }

    long long bestDistanceSquared = std::numeric_limits<long long>::max();
    for (size_t i = 0; i < popupKeys.size(); ++i)
    {
        RECT itemRect = GetCollectionPopupItemRect(popup, i);
        RECT clipped{};
        if (!IntersectRect(&clipped, &itemRect, &content))
            continue;

        const LONG edgeXs[] = {
            itemRect.left - kCollectionPopupGapX / 2,
            itemRect.right + kCollectionPopupGapX / 2,
        };
        const HitRegion edgeRegions[] = {
            HitRegion::SortBefore,
            HitRegion::SortAfter,
        };
        for (size_t edge = 0; edge < 2; ++edge)
        {
            const long long dx = static_cast<long long>(client.x) - edgeXs[edge];
            long long dy = 0;
            if (client.y < clipped.top)
                dy = static_cast<long long>(clipped.top) - client.y;
            else if (client.y >= clipped.bottom)
                dy = static_cast<long long>(client.y) - clipped.bottom + 1;
            const long long distanceSquared = dx * dx + dy * dy;
            if (distanceSquared >= bestDistanceSquared) continue;
            bestDistanceSquared = distanceSquared;
            slotIndex = i;
            slotBounds = itemRect;
            region = edgeRegions[edge];
        }
    }

    if (bestDistanceSquared == std::numeric_limits<long long>::max())
        return true;

    popupDragTargetSlot_ = std::make_unique<Slot>(popupContainer, slotBounds, slotIndex);
    targetSlot = popupDragTargetSlot_.get();
    targetRegion = region;
    return true;
}

/**
 * @brief 更新拖拽翻页按钮的悬停和自动翻页状态
 * @param clientPoint 当前鼠标客户端坐标
 * @return 拖拽会话仍可继续时返回 true
 */
inline bool DesktopApp::UpdateDragPageNavigation(POINT clientPoint)
{
    lastMousePoint_ = clientPoint;
    if (!dragSession_.IsActive())
    {
        navHoverSide_ = 0;
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return false;
    }

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    int navSide = 0;
    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();
    // 悬停检测不限制 hasPrev/hasNext，让置灰按钮也有 hover 视觉反馈
    if (PtInRect(&prevRect, clientPoint)) navSide = -1;
    else if (PtInRect(&nextRect, clientPoint)) navSide = 1;
    navHoverSide_ = navSide;

    // 自动翻页仅在可操作方向触发
    const bool navEnabled = (navSide == -1 && hasPrev) || (navSide == 1 && hasNext);
    if (navSide == 0 || !navEnabled)
    {
        navAutoFlipDir_ = 0;
        navAutoFlipTick_ = 0;
        return true;
    }

    const DWORD now = GetTickCount();
    if (navAutoFlipDir_ != navSide)
    {
        navAutoFlipDir_ = navSide;
        navAutoFlipTick_ = now;
        return true;
    }
    if (now - navAutoFlipTick_ <= 500)
        return true;

    const int newOffset = NextNonEmptyOffset(pageOffset_, navSide);
    if (newOffset == pageOffset_)
        return true;

    const bool hasInternalItems = !dragSession_.Items().empty();
    const bool groupedEntryDrag =
        dragSession_.SourceList().
            hasCollectionGroupEntries ||
        dragSession_.SourceList().
            hasFileGroupEntries;
    // 保存迁移前第一个选中项的实际 bounds（含页面渲染尺寸差异）
    RECT oldFirstBounds{};
    bool hasOldBounds = false;
    if (hasInternalItems && !dragSession_.Items().empty())
    {
        for (const auto& item : items_)
        {
            if (item.selected && !item.name.empty())
            {
                oldFirstBounds = item.bounds;
                hasOldBounds = !IsRectEmptyRect(oldFirstBounds);
                break;
            }
        }
    }
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (hasInternalItems && !groupedEntryDrag)
        MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();

    navAutoFlipTick_ = now;
    if (!dragSession_.IsActive() || (hasInternalItems && dragSession_.Items().empty()))
    {
        mouseDownHit_ = nullptr;
        mouseDown_ = false;
        return false;
    }

    InvalidateDragStaticScene();
    if (hasInternalItems && !groupedEntryDrag)
    {
        UpdateDragGroupOrigin();
        // 用实际 bounds 差值补偿 mouseDown，消除跨页渲染尺寸差异导致的视觉跳动
        if (hasOldBounds)
        {
            for (const auto& item : items_)
            {
                if (item.selected && !item.name.empty())
                {
                    dragSession_.AdjustMouseDownPoint({
                        item.bounds.left - oldFirstBounds.left,
                        item.bounds.top  - oldFirstBounds.top
                    });
                    break;
                }
            }
        }
        else
        {
            UpdateDragGroupOrigin();
        }
    }
    // 页面迁移后 usedSlots 变化，预览缓存失效
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
    SaveLayoutSlots();
    return true;
}

/**
 * @brief 清除所有桌面项、小部件和文件夹条目的选中状态
 */
inline void DesktopApp::ClearSelection()
{
    for (auto& item : items_)
        item.selected = false;
    for (auto& entry : dockEntries_)
        entry.selected = false;
    for (auto& app : dockUnpinnedRunningApps_)
        app.selected = false;
    for (auto& widget : widgets_)
    {
        widget.selected = false;
        for (auto& entry : widget.folderEntries)
            entry.selected = false;
    }
    keyboardNavInsideWidget_ = false;
    keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    keyboardNavMemberIndex_ = -1;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;
}

/**
 * @brief 根据当前选中状态同步键盘导航上下文
 *
 * 扫描所有组件成员项的选中状态，
 * 若有成员项被选中则将导航上下文切换到该组件内部，
 * 否则重置为桌面网格导航模式。
 */
inline void DesktopApp::SyncKeyboardNavFromSelection()
{
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;
    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        const auto& w = widgets_[wi];
        if (IsGroupedWidget(w))
            continue;
        if (w.type == DesktopWidgetType::FileGroup)
        {
            for (auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<FileGroup*>(
                        container.get());
                if (!group ||
                    group->GetWidgetData() != &w)
                    continue;
                const auto& sourceIds =
                    group->GetVisibleSourceIds();
                for (size_t k = 0;
                    k < sourceIds.size(); ++k)
                {
                    const size_t childIndex =
                        FindWidgetIndexById(
                            sourceIds[k]);
                    if (childIndex < widgets_.size() &&
                        widgets_[childIndex].selected)
                    {
                        keyboardNavInsideWidget_ = true;
                        keyboardNavWidgetIndex_ = wi;
                        keyboardNavMemberIndex_ =
                            static_cast<int>(k);
                        keyboardNavCollectionGroupTabs_ =
                            true;
                        return;
                    }
                }
                const auto keys =
                    group->GetHostedVisibleItemKeys();
                for (size_t k = 0; k < keys.size(); ++k)
                {
                    const size_t itemIndex =
                        FindItemIndexByKey(keys[k]);
                    if (itemIndex < items_.size() &&
                        items_[itemIndex].selected)
                    {
                        keyboardNavInsideWidget_ = true;
                        keyboardNavWidgetIndex_ = wi;
                        keyboardNavMemberIndex_ =
                            static_cast<int>(k);
                        return;
                    }
                }
                auto* active =
                    group->GetActiveSourceContainer();
                DesktopWidget* activeData = active
                    ? active->GetWidgetData() : nullptr;
                const auto indices =
                    group->
                        GetHostedVisibleFolderIndices();
                if (activeData)
                    for (size_t k = 0;
                        k < indices.size(); ++k)
                    {
                        const size_t entryIndex =
                            indices[k];
                        if (entryIndex <
                                activeData->
                                    folderEntries.size() &&
                            activeData->
                                folderEntries[entryIndex].
                                    selected)
                        {
                            keyboardNavInsideWidget_ = true;
                            keyboardNavWidgetIndex_ = wi;
                            keyboardNavMemberIndex_ =
                                static_cast<int>(k);
                            return;
                        }
                    }
                break;
            }
        }
        if (w.type == DesktopWidgetType::CollectionGroup)
        {
            for (auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<CollectionGroup*>(container.get());
                if (!group || group->GetWidgetData() != &w)
                    continue;
                const auto& keys = group->GetVisibleItemKeys();
                for (size_t k = 0; k < keys.size(); ++k)
                {
                    const size_t itemIndex =
                        FindItemIndexByKey(keys[k]);
                    if (itemIndex < items_.size() &&
                        items_[itemIndex].selected)
                    {
                        keyboardNavInsideWidget_ = true;
                        keyboardNavWidgetIndex_ = wi;
                        keyboardNavMemberIndex_ =
                            static_cast<int>(k);
                        return;
                    }
                }
                break;
            }
        }
        for (size_t k = 0; k < w.itemKeys.size(); ++k)
        {
            size_t idx = FindItemIndexByKey(w.itemKeys[k]);
            if (idx != static_cast<size_t>(-1) && items_[idx].selected)
            {
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ = wi;
                keyboardNavMemberIndex_ = static_cast<int>(k);
                return;
            }
        }
        for (size_t k = 0; k < w.folderEntries.size(); ++k)
        {
            if (w.folderEntries[k].selected)
            {
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ = wi;
                keyboardNavMemberIndex_ = static_cast<int>(k);
                return;
            }
        }
        if (w.type == DesktopWidgetType::CollectionGroup)
        {
            for (size_t k = 0; k < w.childWidgetIds.size(); ++k)
            {
                const size_t childIndex =
                    FindWidgetIndexById(w.childWidgetIds[k]);
                if (childIndex < widgets_.size() &&
                    widgets_[childIndex].selected)
                {
                    keyboardNavInsideWidget_ = true;
                    keyboardNavWidgetIndex_ = wi;
                    keyboardNavMemberIndex_ =
                        static_cast<int>(k);
                    keyboardNavCollectionGroupTabs_ = true;
                    return;
                }
            }
        }
    }
    keyboardNavInsideWidget_ = false;
    keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    keyboardNavMemberIndex_ = -1;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;
}

/**
 * @brief 清除指定小部件之外的所有选中状态
 * @param widgetIndex 保留选中的小部件索引
 */
inline void DesktopApp::ClearSelectionOutsideWidget(size_t widgetIndex)
{
    for (auto& entry : dockEntries_)
        entry.selected = false;
    std::unordered_set<std::wstring> allowedKeys;
    std::unordered_set<std::wstring> allowedWidgetIds;
    std::unordered_set<std::wstring>
        allowedFolderWidgetIds;
    if (widgetIndex < widgets_.size())
    {
        for (const auto& key : widgets_[widgetIndex].itemKeys)
            allowedKeys.insert(ToUpperInvariant(key));
        if (widgets_[widgetIndex].type ==
                DesktopWidgetType::CollectionGroup ||
            widgets_[widgetIndex].type ==
                DesktopWidgetType::FileGroup)
        {
            allowedWidgetIds.insert(
                widgets_[widgetIndex].childWidgetIds.begin(),
                widgets_[widgetIndex].childWidgetIds.end());
            for (const auto& childId :
                widgets_[widgetIndex].childWidgetIds)
            {
                const size_t childIndex =
                    FindWidgetIndexById(childId);
                if (childIndex >= widgets_.size()) continue;
                for (const auto& key :
                    widgets_[childIndex].itemKeys)
                    allowedKeys.insert(ToUpperInvariant(key));
                if (widgets_[childIndex].type ==
                    DesktopWidgetType::FolderMapping)
                    allowedFolderWidgetIds.insert(
                        widgets_[childIndex].id);
            }
        }
    }

    for (auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!allowedKeys.contains(key))
            item.selected = false;
    }

    for (size_t wi = 0; wi < widgets_.size(); ++wi)
    {
        if (!allowedWidgetIds.contains(widgets_[wi].id))
            widgets_[wi].selected = false;
        if ((wi == widgetIndex &&
             widgets_[wi].type ==
                DesktopWidgetType::FolderMapping) ||
            allowedFolderWidgetIds.contains(
                widgets_[wi].id))
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
    for (auto& entry : dockEntries_)
        entry.selected = false;
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
    if (index >= widgets_.size()) return;
    ClearSelection();
    for (auto& w : widgets_)
    {
        w.selected = (&w == &widgets_[index]);
        for (auto& e : w.folderEntries) e.selected = false;
    }
    if (widgetEngine_ && widgets_[index].type == DesktopWidgetType::LuaScript &&
        widgetEngine_->EnsureWidgetLoaded(widgets_[index].id, widgets_[index].scriptPath))
        widgetEngine_->InvokeSelected(widgets_[index].id);
}

/**
 * @brief 处理鼠标左键按下事件
 * @param wp WPARAM（含修饰键状态）
 * @param lp LPARAM（含鼠标坐标）
 * @details 处理逻辑：集合弹窗点击 -> 页面导航点击 -> 小部件点击 -> 桌面图标点击
 */
inline void DesktopApp::OnLeftButtonDown(WPARAM wp, LPARAM lp)
{
    if (middleButtonWidgetMove_) return;
    if (renameEdit_ != nullptr) return;
    popupMouseDownItem_.reset();
    popupDragTargetSlot_.reset();
    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    DockContainer* pointDock = GetDockContainerAtPoint(pt);
    const bool pointInDock = pointDock != nullptr;
    size_t pressedDockCollectionWidgetIndex =
        static_cast<size_t>(-1);
    if (pointDock)
    {
        if (DockEntryItem* pressedDockItem =
                pointDock->EntryAtPoint(pt);
            pressedDockItem &&
            pressedDockItem->GetEntryType() ==
                DockEntryType::Collection)
        {
            pressedDockCollectionWidgetIndex =
                FindWidgetIndexById(
                    pressedDockItem->GetReference());
        }
    }
    const bool pressedOpenPopupDockToggle =
        snowdesktop::floating_dock_rules::
            ShouldCloseCollectionPopup(
                popupWidgetIndex_,
                pressedDockCollectionWidgetIndex);
    HWND interactionCaptureHwnd = hwnd_;
    if (handlingFloatingDockInput_ &&
        floatingDockHwnd_ &&
        IsWindow(floatingDockHwnd_))
        interactionCaptureHwnd =
            floatingDockHwnd_;
    dockPressedContainer_ = pointDock;
    // Dock is an app switcher: do not move focus away from the current app before
    // deciding whether this click should minimize or restore it. The decision
    // itself is captured from the indicator state during hit testing below.
    if (!pointInDock)
        FocusDesktopInputWindow();
    if (widgetEngine_ && widgetEngine_->HasFocusedHostInput())
        widgetEngine_->BlurHostInput(false);
    mouseDown_ = true;
    mouseDownPoint_ = pt;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    marqueeAnchorPoint_ = pt;
    marqueeInitialScrollOffset_ = 0;
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    marqueeRect_ = MakeRect(pt.x, pt.y, pt.x, pt.y);

    // 外部点击先关闭集合弹窗，但保留本次按下事件，继续命中弹窗下方的真实目标。
    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (snowdesktop::floating_dock_rules::
                ShouldCloseCollectionPopupOnPointerDown(
                    popupWidgetIndex_,
                    pressedDockCollectionWidgetIndex,
                    PtInRect(&popup, pt) != FALSE))
            CloseCollectionPopup();
    }

    if (HandleQuickNavigationClick(pt))
    {
        mouseDown_ = false;
        return;
    }

    if (HandlePageNavClick(pt)) return;

    bool ctrl = (wp & MK_CONTROL) != 0;

    if (popupWidgetIndex_ < widgets_.size() &&
        !pressedOpenPopupDockToggle)
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);

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
        SetCapture(interactionCaptureHwnd);
        InvalidateRect(hwnd_, nullptr, FALSE);
        SyncKeyboardNavFromSelection();
        return;
    }

    // ── Dock hit-test（位于普通组件和桌面网格之上）──────────
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    if (DockContainer* dock = pointDock)
    {
        if (dock->ContainsInteractivePoint(pt))
        {
            if (dock->IsWindowsButtonPoint(pt))
            {
                mouseDown_ = false;
                ToggleWindowsStartMenu();
                if (floatingDockVisible_)
                    CloseFloatingDock();
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (dock->IsSearchPoint(pt))
            {
                mouseDown_ = false;
                OpenQuickNavigation();
                if (floatingDockVisible_)
                    CloseFloatingDock();
                return;
            }
            if (DockEntryItem* dockItem = dock->EntryAtPoint(pt))
            {
                if (!ctrl) ClearSelection();
                dockItem->SetSelected(true);
                dockPressedEntry_ = dockItem->GetEntryIndex();
                if (dockPressedEntry_ < dockEntries_.size() &&
                    dockEntries_[dockPressedEntry_].type ==
                        DockEntryType::DesktopItem)
                {
                    const size_t itemIndex = FindItemIndexByKey(
                        dockEntries_[dockPressedEntry_].reference);
                    if (itemIndex < items_.size())
                    {
                        const DockWindowVisualState state =
                            GetDockWindowVisualState(itemIndex);
                        dockPressedWindowAction_ =
                            snowdesktop::dock_window_rules::
                                ResolveDockClickAction(
                                    state != DockWindowVisualState::Closed,
                                    state == DockWindowVisualState::Minimized,
                                    state == DockWindowVisualState::Foreground);
                        const auto running = dockRunningWindows_.find(
                            DockItemWindowKey(items_[itemIndex]));
                        if (running != dockRunningWindows_.end() &&
                            IsWindow(running->second.window))
                            dockPressedTargetWindow_ =
                                running->second.window;
                    }
                }
                mouseDownHit_ = dockItem;
                SetCapture(interactionCaptureHwnd);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (DockRunningItem* runningItem = dock->RunningItemAtPoint(pt))
            {
                if (!ctrl) ClearSelection();
                runningItem->SetSelected(true);
                dockPressedRunningAppKey_ = runningItem->GetIdentityKey();
                const auto running = std::find_if(
                    dockUnpinnedRunningApps_.begin(),
                    dockUnpinnedRunningApps_.end(),
                    [&](const DockRunningAppInfo& app) {
                        return app.identityKey ==
                            dockPressedRunningAppKey_;
                    });
                if (running != dockUnpinnedRunningApps_.end())
                {
                    dockPressedWindowAction_ =
                        snowdesktop::dock_window_rules::
                            ResolveDockClickAction(
                                true, running->minimized,
                                running->foreground);
                    if (IsWindow(running->window))
                        dockPressedTargetWindow_ =
                            running->window;
                }
                mouseDownHit_ = runningItem;
                SetCapture(interactionCaptureHwnd);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (DockFrequentItem* frequentItem = dock->FrequentItemAtPoint(pt))
            {
                if (!ctrl) ClearSelection();
                frequentItem->SetSelected(true);
                dockPressedFrequentItem_ = frequentItem->GetItemIndex();
                if (dockPressedFrequentItem_ < items_.size())
                {
                    const DockWindowVisualState state =
                        GetDockWindowVisualState(
                            dockPressedFrequentItem_);
                    dockPressedWindowAction_ =
                        snowdesktop::dock_window_rules::
                            ResolveDockClickAction(
                                state != DockWindowVisualState::Closed,
                                state == DockWindowVisualState::Minimized,
                                state == DockWindowVisualState::Foreground);
                    const auto running = dockRunningWindows_.find(
                        DockItemWindowKey(
                            items_[dockPressedFrequentItem_]));
                    if (running != dockRunningWindows_.end() &&
                        IsWindow(running->second.window))
                        dockPressedTargetWindow_ =
                            running->second.window;
                }
                mouseDownHit_ = frequentItem;
                SetCapture(interactionCaptureHwnd);
                InvalidateRect(hwnd_, nullptr, FALSE);
                return;
            }
            if (!ctrl) ClearSelection();
            mouseDown_ = false;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // ── Widget hit-test ─────────────────────────────────────
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    widgetAction_ = WidgetAction::None;
    widgetCollectionGroupTargetIndex_ = static_cast<size_t>(-1);
    widgetCollectionGroupInsertIndex_ = static_cast<size_t>(-1);

    // Defocus search box when clicking outside all search boxes
    {
        bool clickedSearchBox = false;
        for (auto& c : containers_)
        {
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
            if (!searchable) continue;
            RECT sr = searchable->GetSearchBoxRect();
            if (!IsRectEmptyRect(sr) && PtInRect(&sr, pt))
            {
                clickedSearchBox = true;
                break;
            }
        }
        if (!clickedSearchBox)
        {
            for (auto& c : containers_)
            {
                auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (searchable && searchable->IsSearchFocused())
                {
                    searchable->SetSearchFocused(false);
                    InvalidateRect(hwnd_, nullptr, FALSE);
                }
            }
        }
    }

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
                SyncKeyboardNavFromSelection();
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
                widgets_[wi].type == DesktopWidgetType::FileCategories ||
                widgets_[wi].type == DesktopWidgetType::Collection ||
                widgets_[wi].type == DesktopWidgetType::CollectionGroup ||
                widgets_[wi].type == DesktopWidgetType::FileGroup)
            {
                widgets_[wi].listMode = !widgets_[wi].listMode;
                if (auto* group =
                        dynamic_cast<FileGroup*>(wc))
                    group->InvalidateHostedView();
                else
                    wc->InvalidateSlots();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::DateHeaderToggleBtn)
        {
            if (widgets_[wi].type == DesktopWidgetType::FileCategories ||
                widgets_[wi].type == DesktopWidgetType::FolderMapping ||
                widgets_[wi].type == DesktopWidgetType::FileGroup)
            {
                widgets_[wi].dateHeaders = !widgets_[wi].dateHeaders;
                widgets_[wi].scrollOffset = 0;
                if (auto* mapping = dynamic_cast<FolderMapping*>(wc))
                    mapping->InvalidateFilterCache();
                else if (auto* group =
                             dynamic_cast<FileGroup*>(wc))
                    group->InvalidateHostedView();
                else
                    RebuildContainersAndItems();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::OpenFolderBtn)
        {
            DesktopWidget* folder = &widgets_[wi];
            if (auto* group = dynamic_cast<FileGroup*>(wc))
            {
                const size_t activeIndex =
                    FindWidgetIndexById(
                        group->GetActiveSourceId());
                folder = activeIndex < widgets_.size()
                    ? &widgets_[activeIndex] : nullptr;
            }
            if (folder &&
                folder->type ==
                    DesktopWidgetType::FolderMapping &&
                !folder->sourceFolderPath.empty())
            {
                ShellExecuteW(hwnd_, L"open",
                    folder->sourceFolderPath.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
            }
            return;
        }
        else if (wh == WidgetHit::SourceTab)
        {
            auto* group = dynamic_cast<FileGroup*>(wc);
            const std::wstring id = group
                ? group->SourceIdAtPoint(pt) : L"";
            if (!id.empty())
            {
                widgets_[wi].activeCategoryId = id;
                widgets_[wi].scrollOffset = 0;
                group->InvalidateHostedView();
                ClearSelection();
                const size_t childIndex =
                    FindWidgetIndexById(id);
                if (childIndex < widgets_.size())
                    widgets_[childIndex].selected = true;
                mouseDownWidgetIndex_ = wi;
                mouseDownHit_ =
                    group->GetSourceTabItemAtPoint(pt);
                if (mouseDownHit_)
                {
                    SetCapture(hwnd_);
                    SyncKeyboardNavFromSelection();
                }
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            return;
        }
        else if (wh == WidgetHit::CategoryTab)
        {
            if (widgets_[wi].type == DesktopWidgetType::FileCategories ||
                widgets_[wi].type == DesktopWidgetType::FolderMapping ||
                widgets_[wi].type == DesktopWidgetType::CollectionGroup ||
                widgets_[wi].type == DesktopWidgetType::FileGroup)
            {
                auto* categorized = dynamic_cast<ScrollingItemWidget*>(wc);
                std::wstring id = categorized
                    ? categorized->CategoryIdAtPoint(pt)
                    : L"";
                if (!id.empty())
                {
                    DesktopWidget* categorizedData =
                        &widgets_[wi];
                    if (auto* fileGroup =
                            dynamic_cast<FileGroup*>(wc))
                    {
                        if (auto* active =
                                fileGroup->
                                    GetActiveSourceContainer())
                            categorizedData =
                                active->GetWidgetData();
                    }
                    if (!categorizedData) return;
                    categorizedData->activeCategoryId = id;
                    widgets_[wi].scrollOffset = 0;
                    if (auto* group =
                        dynamic_cast<CollectionGroup*>(wc))
                    {
                        group->InvalidateFilterCache();
                        ClearSelection();
                        const size_t childIndex =
                            FindWidgetIndexById(id);
                        if (childIndex < widgets_.size())
                            widgets_[childIndex].selected = true;
                        mouseDownWidgetIndex_ = wi;
                        mouseDownHit_ =
                            group->GetTabItemAtPoint(pt);
                        if (mouseDownHit_)
                        {
                            SetCapture(hwnd_);
                            SyncKeyboardNavFromSelection();
                        }
                    }
                    else
                    {
                        if (auto* fileGroup =
                                dynamic_cast<FileGroup*>(wc))
                            fileGroup->
                                InvalidateHostedView();
                        else
                            wc->InvalidateSlots();
                    }
                    SaveLayoutSlots();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
            }
            return;
        }
        else if (wh == WidgetHit::SearchBox)
        {
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(wc);
            for (auto& c : containers_)
            {
                auto* other = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (other) other->SetSearchFocused(false);
            }
            if (searchable) searchable->SetSearchFocused(true);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

    // ── Desktop icon hit-test ───────────────────────────────
    DesktopIcon* hit = HitTestIcon(pt);
    mouseDownHit_ = hit;

    // Defocus search when clicking on desktop area
    if (hit || mouseDownWidgetIndex_ == static_cast<size_t>(-1))
    {
        for (auto& c : containers_)
        {
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
            if (searchable && searchable->IsSearchFocused())
            {
                searchable->SetSearchFocused(false);
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
    }

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
    SyncKeyboardNavFromSelection();
}

inline void DesktopApp::OnMiddleButtonDown(WPARAM wp, LPARAM lp)
{
    (void)wp;
    if (renameEdit_ != nullptr || mouseDown_ || dragSession_.IsActive() ||
        widgetAction_ != WidgetAction::None)
        return;

    POINT pt{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    if (quickNavigationOpen_) return;
    if (GetDockContainerAtPoint(pt)) return;
    if (popupWidgetIndex_ < widgets_.size())
    {
        RECT popup = GetCollectionPopupRect(widgets_[popupWidgetIndex_]);
        if (PtInRect(&popup, pt)) return;
        CloseCollectionPopup();
    }

    size_t widgetIndex = static_cast<size_t>(-1);
    for (size_t n = widgets_.size(); n > 0; --n)
    {
        const size_t candidate = n - 1;
        bool hit = HitTestStandaloneWidget(candidate, pt) != WidgetHit::None;
        if (!hit && widgets_[candidate].type != DesktopWidgetType::LuaScript)
        {
            for (auto& container : containers_)
            {
                auto* widgetContainer = dynamic_cast<WidgetContainer*>(container.get());
                if (!widgetContainer ||
                    widgetContainer->GetWidgetData() != &widgets_[candidate])
                    continue;
                hit = widgetContainer->HitTestWidget(pt) != WidgetHit::None;
                break;
            }
        }
        if (hit)
        {
            widgetIndex = candidate;
            break;
        }
    }
    if (widgetIndex >= widgets_.size()) return;

    FocusDesktopInputWindow();
    SelectWidgetOnly(widgetIndex);
    mouseDown_ = true;
    mouseDownPoint_ = pt;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = widgetIndex;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    widgetAction_ = WidgetAction::PendingMove;
    middleButtonWidgetMove_ = true;
    InvalidateDragStaticScene();
    widgetDragOriginalCell_ = widgets_[widgetIndex].gridCell;
    widgetDragOriginalSpan_ = widgets_[widgetIndex].gridSpan;
    widgetPreviewCell_ = widgetDragOriginalCell_;
    widgetPreviewSpan_ = widgetDragOriginalSpan_;
    dragGroupOriginX_ = widgets_[widgetIndex].bounds.left;
    dragGroupOriginY_ = widgets_[widgetIndex].bounds.top;
    SetCapture(hwnd_);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

inline void DesktopApp::OnMiddleButtonUp(WPARAM wp, LPARAM lp)
{
    if (!middleButtonWidgetMove_) return;
    middleButtonWidgetMove_ = false;
    OnLeftButtonUp(wp, lp);
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
    if (!handlingFloatingDockInput_)
    {
        TRACKMOUSEEVENT mouseTrack{};
        mouseTrack.cbSize = sizeof(mouseTrack);
        mouseTrack.dwFlags = TME_LEAVE;
        mouseTrack.hwndTrack = hwnd_;
        TrackMouseEvent(&mouseTrack);
    }

    POINT current{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    POINT oldMouse = lastMousePoint_;
    lastMousePoint_ = current;
    UpdateSystemTaskbarRevealGuard();
    UpdateDockWindowPreview(current);

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

    const bool pressedDockEntryWithoutSelection =
        mouseDownHit_ &&
        dynamic_cast<DockEntryItem*>(mouseDownHit_) &&
        dockPressedEntry_ < dockEntries_.size();
    if (!dragSession_.IsActive() && mouseDown_ && mouseDownHit_ &&
        (mouseDownHit_->IsSelected() ||
            pressedDockEntryWithoutSelection))
    {
        if (dynamic_cast<DockFrequentItem*>(mouseDownHit_) ||
            dynamic_cast<DockRunningItem*>(mouseDownHit_))
            return;
        const bool dockItem = dynamic_cast<DockContainer*>(
            mouseDownHit_->GetContainer()) != nullptr;
        const int thresholdX = dockItem
            ? std::max(8, GetSystemMetrics(SM_CXDRAG)) : 3;
        const int thresholdY = dockItem
            ? std::max(8, GetSystemMetrics(SM_CYDRAG)) : 3;
        if (std::abs(current.x - mouseDownPoint_.x) > thresholdX ||
            std::abs(current.y - mouseDownPoint_.y) > thresholdY)
        {
            Container* source = mouseDownHit_->GetContainer();
            std::vector<Item*> sourceItems = source ? source->GetSelectedItems() : std::vector<Item*>{};
            if (sourceItems.empty() &&
                pressedDockEntryWithoutSelection)
            {
                sourceItems.push_back(mouseDownHit_);
            }
            DragSourceList sourceList = BuildDragSourceList(sourceItems, source);
            if (sourceItems.empty())
            {
                return;
            }
            ClearDockBackdropForDragTransition(oldMouse, current);
            dragSession_.Begin(source, std::move(sourceItems), std::move(sourceList),
                mouseDownPoint_, current);
            // From this point the drag session owns the logical interaction.
            // Do not retain the original wrapper pointer across object rebuilds.
            mouseDownHit_ = nullptr;
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
    UpdateCollectionGroupTabDwell(current);

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
        ShowDragHintWindow(current, _LW("core.drag.resize_widget"));
        InvalidateRect(hwnd_, nullptr, TRUE);
        return;
    }

    // Widget drag preview
    if (widgetAction_ == WidgetAction::Move && mouseDownWidgetIndex_ < widgets_.size())
    {
        extern inline int SlotFromCell(const std::vector<GridPage>&, const GridCell&);
        extern inline const GridPage* FindGridPage(const std::vector<GridPage>&, const std::wstring&);

        const DesktopWidgetType movingType =
            widgets_[mouseDownWidgetIndex_].type;
        const auto movingPayload =
            SlotPayloadForWidgetType(movingType);
        const bool movingCollection =
            snowdesktop::slot_contract::AcceptsSlotDrop(
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Desktop,
                movingPayload,
                snowdesktop::slot_contract::
                    SlotSurfaceKind::CollectionGroup,
                snowdesktop::slot_contract::
                    DragRelation::CrossSurface);
        const bool movingFileSource =
            snowdesktop::slot_contract::AcceptsSlotDrop(
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Desktop,
                movingPayload,
                snowdesktop::slot_contract::
                    SlotSurfaceKind::FileGroup,
                snowdesktop::slot_contract::
                    DragRelation::CrossSurface);
        const size_t groupTarget = movingCollection
            ? HitTestCollectionGroupIndex(
                current, mouseDownWidgetIndex_)
            : (movingFileSource
                ? HitTestFileGroupIndex(
                    current, mouseDownWidgetIndex_)
                : static_cast<size_t>(-1));
        if (groupTarget < widgets_.size())
        {
            widgetCollectionGroupTargetIndex_ = groupTarget;
            widgetCollectionGroupInsertIndex_ =
                widgets_[groupTarget].childWidgetIds.size();
            for (auto& container : containers_)
            {
                auto* group =
                    dynamic_cast<WidgetContainer*>(
                        container.get());
                if (!group ||
                    group->GetWidgetData() != &widgets_[groupTarget])
                    continue;
                Slot* slot = nullptr;
                HitRegion region = group->HitTestDrag(current, slot);
                const bool overTab =
                    widgets_[groupTarget].type ==
                        DesktopWidgetType::CollectionGroup
                        ? !dynamic_cast<CollectionGroup*>(group)->
                            CategoryIdAtPoint(current).empty()
                        : !dynamic_cast<FileGroup*>(group)->
                            SourceIdAtPoint(current).empty();
                if (overTab)
                    widgetCollectionGroupInsertIndex_ =
                        group->GetDropInsertIndex(slot, region);
                break;
            }
            widgetDockTarget_ = false;
            widgetDockTargetContainer_ = nullptr;
            ShowDragHintWindow(current,
                _LW(movingCollection
                    ? "core.drag.move_collection_group"
                    : "core.drag.move_file_group"));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        widgetCollectionGroupTargetIndex_ =
            static_cast<size_t>(-1);
        widgetCollectionGroupInsertIndex_ =
            static_cast<size_t>(-1);

        DockContainer* dock = GetDockContainerAtPoint(current);
        const bool canDock = dock &&
            snowdesktop::slot_contract::AcceptsSlotDrop(
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Desktop,
                movingPayload,
                snowdesktop::slot_contract::
                    SlotSurfaceKind::Dock,
                snowdesktop::slot_contract::
                    DragRelation::CrossSurface);
        if (canDock)
        {
            widgetDockTarget_ = true;
            widgetDockTargetContainer_ = dock;
            widgetDockInsertIndex_ = dock->GetInsertIndexAtPoint(current);
            ShowDragHintWindow(current, _LW("core.drag.move_collection_dock"));
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        widgetDockTarget_ = false;
        widgetDockTargetContainer_ = nullptr;
        widgetDockInsertIndex_ = 0;

        // ── 跨页翻页：检测导航按钮悬停 + 自动翻页 ──
        if (MaxPageOffset() > 0)
        {
            RECT prevRect, nextRect;
            GetNavButtonRects(prevRect, nextRect);

            int navSide = 0;
            if (PtInRect(&prevRect, current)) navSide = -1;
            else if (PtInRect(&nextRect, current)) navSide = 1;
            navHoverSide_ = navSide;

            const bool hasPrev = pageOffset_ > 0;
            const bool hasNext = pageOffset_ < MaxPageOffset();
            const bool navEnabled = (navSide == -1 && hasPrev) || (navSide == 1 && hasNext);

            if (navSide != 0 && navEnabled)
            {
                const DWORD now = GetTickCount();
                if (navAutoFlipDir_ != navSide)
                {
                    navAutoFlipDir_ = navSide;
                    navAutoFlipTick_ = now;
                }
                else if (now - navAutoFlipTick_ > 500)
                {
                    // 触发翻页
                    int newOffset = NextNonEmptyOffset(pageOffset_, navSide);
                    if (newOffset != pageOffset_)
                    {
                        // 保存迁移前组件实际 bounds（含页面渲染尺寸差异）
                        RECT oldWidgetBounds = widgets_[mouseDownWidgetIndex_].bounds;
                        pageOffset_ = newOffset;
                        ApplyPageMapping();
                        LayoutItems();
                        // 用实际 bounds 差值补偿 group origin + mouseDown，保持视觉连续性
                        RECT newWidgetBounds = widgets_[mouseDownWidgetIndex_].bounds;
                        const int dx = newWidgetBounds.left - oldWidgetBounds.left;
                        const int dy = newWidgetBounds.top  - oldWidgetBounds.top;
                        dragGroupOriginX_ += dx;
                        dragGroupOriginY_ += dy;
                        mouseDownPoint_.x += dx;
                        mouseDownPoint_.y += dy;
                        InvalidateRect(hwnd_, nullptr, TRUE);
                    }
                    navAutoFlipTick_ = now;
                }
            }
            else
            {
                navAutoFlipDir_ = 0;
                navAutoFlipTick_ = 0;
            }
        }
        else
        {
            navHoverSide_ = 0;
            navAutoFlipDir_ = 0;
            navAutoFlipTick_ = 0;
        }

        POINT adjusted = {
            dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
            dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
        };
        GridCell cell = CellFromPointForDrag(adjusted);
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
        ShowDragHintWindow(current, _LW("core.drag.move_widget"));
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
                navHoverSide_ = 0;
                navAutoFlipDir_ = 0;
                navAutoFlipTick_ = 0;

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

        const bool suppressDesktopWidgetTargets =
            SuppressDesktopWidgetDragTargets();
        const bool groupedEntryDrag =
            dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries;
        if (suppressDesktopWidgetTargets)
        {
            navHoverSide_ = 0;
            navAutoFlipDir_ = 0;
            navAutoFlipTick_ = 0;
        }
        else if (!UpdateDragPageNavigation(current))
            return;

        // OO hit testing: iterate all containers in reverse (topmost first)
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        popupDragTargetSlot_.reset();

        const bool popupHit =
            !suppressDesktopWidgetTargets &&
            !groupedEntryDrag &&
            HitTestPopupForDrag(current, targetContainer, targetSlot, targetRegion);
        if (!popupHit)
        {
            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                if (!AcceptsSlotSurfaceDrop(
                        it->get(),
                        dragSession_.SourceList()))
                    continue;
                if (suppressDesktopWidgetTargets &&
                    (dynamic_cast<DesktopGrid*>(it->get()) ||
                     dynamic_cast<WidgetContainer*>(it->get())))
                    continue;
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
        if (const std::wstring removalHint = GetDockDragOutRemovalHint(current);
            !removalHint.empty())
            hint = removalHint;
        else if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), currentMods);

        ShowDragHintWindow(current, hint);
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    if (mouseDown_ && !mouseDownHit_)
    {
        if (std::abs(current.x - mouseDownPoint_.x) > 3 ||
            std::abs(current.y - mouseDownPoint_.y) > 3)
        {
            if (!marqueeActive_)
                dragRenderCache_.Reset();
            marqueeActive_ = true;
            UpdateMarqueeSelection(current);
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
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
    {
        struct MouseHoverVisual
        {
            const void* owner = nullptr;
            const void* target = nullptr;
            int kind = 0;
            size_t index = 0;
            bool continuous = false;
        };
        auto sameHoverVisual = [](const MouseHoverVisual& a,
            const MouseHoverVisual& b) {
            return a.owner == b.owner &&
                a.target == b.target &&
                a.kind == b.kind &&
                a.index == b.index;
        };
        auto findHoverVisual = [&](POINT point) -> MouseHoverVisual {
            if (popupWidgetIndex_ < widgets_.size() &&
                !IsRectEmptyRect(popupRect_) && PtInRect(&popupRect_, point))
            {
                const DesktopWidget* popupWidget = &widgets_[popupWidgetIndex_];
                RECT content = GetCollectionPopupContentRect(popupRect_);
                if (PtInRect(&content, point))
                {
                    const std::vector<std::wstring> popupKeys = GetPopupItemKeys(*popupWidget);
                    for (size_t i = 0; i < popupKeys.size(); ++i)
                    {
                        RECT itemRect = GetCollectionPopupItemRect(popupRect_, i);
                        if (itemRect.bottom <= content.top || itemRect.top >= content.bottom)
                            continue;
                        if (PtInRect(&itemRect, point))
                            return { popupWidget, popupWidget, 1, i, false };
                    }
                }
                return { popupWidget, popupWidget, 2, 0, true };
            }

            if (DockContainer* dock = GetDockContainerAtPoint(point))
            {
                if (dock->ContainsInteractivePoint(point))
                {
                    if (DockEntryItem* entry = dock->EntryAtPoint(point))
                        return { dock, entry, 8, entry->GetEntryIndex(), true };
                    if (DockRunningItem* item = dock->RunningItemAtPoint(point))
                        return { dock, item, 12, item->GetRunningIndex(), true };
                    if (DockFrequentItem* item = dock->FrequentItemAtPoint(point))
                        return { dock, item, 11, item->GetItemIndex(), true };
                    if (dock->IsWindowsButtonPoint(point))
                        return { dock, dock, 13, 0, true };
                    if (dock->IsSearchPoint(point))
                        return { dock, dock, 9, 0, true };
                    return { dock, dock, 10, 0, true };
                }
            }

            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                auto* widget = dynamic_cast<WidgetContainer*>(it->get());
                if (!widget)
                    continue;
                RECT frame = widget->GetFrameRect();
                if (IsRectEmptyRect(frame) || !PtInRect(&frame, point))
                    continue;

                DesktopWidget* widgetData = widget->GetWidgetData();
                const WidgetHit hit = widget->HitTestWidget(point);
                if (hit == WidgetHit::Content)
                {
                    RECT content = widget->GetContentViewportRect();
                    if (!IsRectEmptyRect(content) && PtInRect(&content, point))
                    {
                        const auto& slots = widget->GetSlots();
                        for (const auto& slot : slots)
                        {
                            if (!slot)
                                continue;
                            RECT slotRect = slot->GetBounds();
                            if (IsRectEmptyRect(slotRect) || !PtInRect(&slotRect, point))
                                continue;
                            RECT visible{};
                            if (!IntersectRect(&visible, &slotRect, &content))
                                continue;
                            Item* item = slot->GetItem();
                            if (item && !item->IsSelected())
                                return { widgetData, slot.get(), 3, slot->GetIndex(), false };
                            break;
                        }
                    }
                    return { widgetData, widgetData, 4, 0, false };
                }

                return {
                    widgetData,
                    widgetData,
                    5,
                    static_cast<size_t>(hit),
                    hit != WidgetHit::None
                };
            }

            const size_t standalone = HitTestStandaloneWidgetIndex(point);
            if (standalone < widgets_.size())
            {
                const WidgetHit hit = HitTestStandaloneWidget(standalone, point);
                return {
                    &widgets_[standalone],
                    &widgets_[standalone],
                    6,
                    static_cast<size_t>(hit),
                    hit != WidgetHit::Content && hit != WidgetHit::None
                };
            }

            for (int i = static_cast<int>(items_oo_.size()) - 1; i >= 0; --i)
            {
                auto* icon = dynamic_cast<DesktopIcon*>(items_oo_[i].get());
                if (!icon)
                    continue;
                DesktopItem* item = icon->GetDesktopItem();
                if (!item || item->selected || IsRectEmptyRect(item->bounds))
                    continue;
                if (!item->layoutKey.empty() &&
                    collectedKeysCache_.count(ToUpperInvariant(item->layoutKey)))
                    continue;
                if (PtInRect(&item->bounds, point))
                    return { item, item, 7, 0, false };
            }

            return {};
        };

        const MouseHoverVisual oldVisual = findHoverVisual(oldMouse);
        const MouseHoverVisual newVisual = findHoverVisual(current);
        const bool hoverChanged = !sameHoverVisual(oldVisual, newVisual);
        const bool dockHoverChanged = hoverChanged &&
            ((oldVisual.kind >= 8 && oldVisual.kind <= 13) ||
             (newVisual.kind >= 8 && newVisual.kind <= 13));
        const bool needsContinuousHoverPaint =
            (oldVisual.owner && oldVisual.continuous) ||
            (newVisual.owner && newVisual.continuous);
        if (marqueeActive_ || hoverChanged || needsContinuousHoverPaint)
            InvalidateRect(hwnd_, nullptr, FALSE);
        if (dockHoverChanged && hwnd_ && IsWindow(hwnd_))
            UpdateWindow(hwnd_);

        for (auto& w : widgets_)
        {
            if (!w.showOnHoverOnly) continue;
            if (PtInRect(&w.bounds, oldMouse) != PtInRect(&w.bounds, current))
            {
                InvalidateRect(hwnd_, nullptr, FALSE);
                break;
            }
        }
    }
}

inline void DesktopApp::OnMouseLeave()
{
    POINT cursorScreen{};
    GetCursorPos(&cursorScreen);
    const bool pointerStillInteractsWithDockPreview =
        dockWindowPreview_ &&
        dockWindowPreview_->ContainsInteractionPoint(cursorScreen);
    if (pointerStillInteractsWithDockPreview)
        dockWindowPreview_->ScheduleHide();
    else
        HideDockWindowPreview();
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;

    popupDwellWidgetIndex_ = static_cast<size_t>(-1);
    popupDwellTick_ = 0;
    collectionGroupTabDwellWidgetIndex_ =
        static_cast<size_t>(-1);
    collectionGroupTabDwellId_.clear();
    collectionGroupTabDwellTick_ = 0;
    dockHandoffDwellIndex_ = static_cast<size_t>(-1);
    dockHandoffDwellStartTick_ = 0;
    dockHandoffDwellReady_ = false;
    if (hwnd_)
    {
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        KillTimer(hwnd_, kCollectionGroupTabDwellTimerId);
        KillTimer(hwnd_, kDockHandoffDwellTimerId);
    }

    // Capture-based dragging continues to receive coordinates outside the
    // window. Preserve that pointer state, but clear passive hover immediately.
    // The preview owns its independent screen-space transition triangle, so
    // Dock magnification can still be reset while the preview stays reachable.
    const HWND captureWindow = GetCapture();
    const bool ownsInteractionCapture =
        captureWindow == hwnd_ ||
        captureWindow == floatingDockHwnd_;
    if (!ownsInteractionCapture && !mouseDown_ &&
        !dragSession_.IsActive() &&
        widgetAction_ == WidgetAction::None)
    {
        lastMousePoint_ = { LONG_MIN, LONG_MIN };
    }
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, FALSE);
}

inline bool DesktopApp::HandleDockClickRelease(POINT point)
{
    DockContainer* dock = dockPressedContainer_;
    if (!dock) dock = GetDockContainerAtPoint(point);
    if (!dock) return false;

    DockEntryType entryType = DockEntryType::DesktopItem;
    std::wstring reference;
    size_t frequentItemIndex = static_cast<size_t>(-1);
    std::wstring runningAppKey;
    const size_t pressedEntryIndex = dockPressedEntry_;
    const size_t pressedFrequentItemIndex = dockPressedFrequentItem_;
    const auto pressedWindowAction = dockPressedWindowAction_;
    const HWND pressedTargetWindow = dockPressedTargetWindow_;
    std::optional<RECT> pressedAnchorScreen;
    if (mouseDownHit_ && hwnd_)
    {
        RECT anchor = dock->GetElementVisualRect(
            mouseDownHit_->GetBounds(), mouseDownPoint_);
        if (!IsRectEmpty(&anchor))
        {
            MapWindowPoints(
                hwnd_, nullptr,
                reinterpret_cast<POINT*>(&anchor), 2);
            pressedAnchorScreen = anchor;
        }
    }
    if (dockPressedEntry_ < dockEntries_.size())
    {
        DockEntryItem* hit = dock->EntryAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetEntryIndex() != dockPressedEntry_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        entryType = dockEntries_[dockPressedEntry_].type;
        reference = dockEntries_[dockPressedEntry_].reference;
    }
    else if (!dockPressedRunningAppKey_.empty())
    {
        DockRunningItem* hit = dock->RunningItemAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetIdentityKey() != dockPressedRunningAppKey_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        runningAppKey = dockPressedRunningAppKey_;
    }
    else if (dockPressedFrequentItem_ < items_.size())
    {
        DockFrequentItem* hit = dock->FrequentItemAtPoint(point);
        const int clickSlopX = std::max(8, GetSystemMetrics(SM_CXDRAG));
        const int clickSlopY = std::max(8, GetSystemMetrics(SM_CYDRAG));
        const bool withinClickSlop =
            std::abs(point.x - mouseDownPoint_.x) <= clickSlopX &&
            std::abs(point.y - mouseDownPoint_.y) <= clickSlopY;
        if ((!hit || hit->GetItemIndex() != dockPressedFrequentItem_) &&
            (dragSession_.IsActive() || !withinClickSlop))
            return false;
        frequentItemIndex = dockPressedFrequentItem_;
    }
    else
    {
        return false;
    }

    size_t appItemIndex = frequentItemIndex;
    if (appItemIndex >= items_.size() && entryType == DockEntryType::DesktopItem)
        appItemIndex = FindItemIndexByKey(reference);
    bool waitForDoubleClick = false;
    if (appItemIndex < items_.size())
        waitForDoubleClick =
            pressedWindowAction ==
                snowdesktop::dock_window_rules::DockClickAction::Launch;

    // 在激活外部应用前完整结束本次桌面交互，避免鼠标捕获、选择高亮或
    // 拖拽预览跨到新前台窗口后仍残留。
    EndDragSession();
    HideDragHintWindow();
    if (!waitForDoubleClick)
        ClearSelection();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    mouseDownWidgetIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleDesktopIndex_ = static_cast<size_t>(-1);
    pendingCtrlToggleWidgetItem_ = nullptr;
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    dockPressedContainer_ = nullptr;
    marqueeActive_ = false;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    ReleaseCapture();
    InvalidateDragStaticScene();
    const bool collectionOnFloatingDock =
        entryType == DockEntryType::Collection &&
        floatingDockVisible_ &&
        dock == floatingDockContainer_;
    if (hwnd_ && !collectionOnFloatingDock)
    {
        if (floatingDockVisible_ &&
            dock == floatingDockContainer_)
            InvalidateFloatingDockWindow(true);
        else
        {
            InvalidateRect(hwnd_, nullptr, FALSE);
            UpdateWindow(hwnd_);
        }
    }

    if (waitForDoubleClick)
    {
        dockPendingDoubleClickEntry_ = pressedEntryIndex;
        dockPendingDoubleClickFrequentItem_ = pressedFrequentItemIndex;
        dockPendingDoubleClickTick_ = GetTickCount();
        return true;
    }

    dockPendingDoubleClickEntry_ = static_cast<size_t>(-1);
    dockPendingDoubleClickFrequentItem_ = static_cast<size_t>(-1);
    dockPendingDoubleClickTick_ = 0;

    if (!runningAppKey.empty())
    {
        const auto running = std::find_if(dockUnpinnedRunningApps_.begin(),
            dockUnpinnedRunningApps_.end(), [&](const DockRunningAppInfo& app) {
                return app.identityKey == runningAppKey;
            });
        if (running != dockUnpinnedRunningApps_.end())
            ActivateOrToggleDockWindow(
                running->window, pressedWindowAction,
                pressedTargetWindow,
                pressedAnchorScreen);
        if (floatingDockVisible_)
            CloseFloatingDock();
    }
    else if (frequentItemIndex < items_.size())
    {
        ActivateOrToggleDockItem(
            frequentItemIndex, pressedWindowAction,
            pressedTargetWindow,
            pressedAnchorScreen);
        if (floatingDockVisible_)
            CloseFloatingDock();
    }
    else if (entryType == DockEntryType::Collection)
    {
        const size_t widgetIndex = FindWidgetIndexById(reference);
        if (widgetIndex < widgets_.size())
        {
            if (snowdesktop::floating_dock_rules::
                    ShouldCloseCollectionPopup(
                        popupWidgetIndex_,
                        widgetIndex))
                CloseCollectionPopup();
            else
                OpenCollectionPopupAt(
                    widgetIndex, point);
        }
    }
    else
    {
        const size_t itemIndex = FindItemIndexByKey(reference);
        if (itemIndex < items_.size())
            ActivateOrToggleDockItem(
                itemIndex, pressedWindowAction,
                pressedTargetWindow,
                pressedAnchorScreen);
        if (floatingDockVisible_)
            CloseFloatingDock();
    }
    return true;
}

/**
 * @brief 处理鼠标左键释放事件
 * @param wp WPARAM
 * @param lp LPARAM（含鼠标坐标）
 * @details 完成拖拽放置、小部件移动/调整大小、Ctrl 点击切换等
 */
inline void DesktopApp::OnLeftButtonUp(WPARAM wp, LPARAM lp)
{
    if (middleButtonWidgetMove_) return;
    (void)wp;
    POINT upPoint{ GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
    HideDragHintWindow();

    // ── Widget action completion ────────────────────────────
    if (widgetAction_ != WidgetAction::None && mouseDownWidgetIndex_ < widgets_.size())
    {
        if (widgetAction_ == WidgetAction::Move)
        {
            const auto movingPayload =
                SlotPayloadForWidgetType(
                    widgets_[mouseDownWidgetIndex_].type);
            const bool canCollectionGroup =
                widgetCollectionGroupTargetIndex_ < widgets_.size() &&
                widgets_[widgetCollectionGroupTargetIndex_].type ==
                    DesktopWidgetType::CollectionGroup &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::CollectionGroup,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            const bool canFileGroup =
                widgetCollectionGroupTargetIndex_ < widgets_.size() &&
                widgets_[widgetCollectionGroupTargetIndex_].type ==
                    DesktopWidgetType::FileGroup &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::FileGroup,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            const bool canGroup =
                canCollectionGroup || canFileGroup;
            DockContainer* dock = canGroup
                ? nullptr : GetDockContainerAtPoint(upPoint);
            const bool canDock = !canGroup && dock &&
                snowdesktop::slot_contract::AcceptsSlotDrop(
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Desktop,
                    movingPayload,
                    snowdesktop::slot_contract::
                        SlotSurfaceKind::Dock,
                    snowdesktop::slot_contract::
                        DragRelation::CrossSurface);
            if (canGroup)
            {
                if (canCollectionGroup)
                    AddCollectionToGroup(
                        mouseDownWidgetIndex_,
                        widgetCollectionGroupTargetIndex_,
                        widgetCollectionGroupInsertIndex_);
                else
                    AddWidgetToFileGroup(
                        mouseDownWidgetIndex_,
                        widgetCollectionGroupTargetIndex_,
                        widgetCollectionGroupInsertIndex_);
            }
            else if (canDock)
            {
                Widget dockSource(&widgets_[mouseDownWidgetIndex_], this);
                int mods = (GetAsyncKeyState(VK_CONTROL) & 0x8000) ? MK_CONTROL : 0;
                CommitDockDrop({ &dockSource }, nullptr, dock,
                    widgetDockInsertIndex_, mods);
                SaveLayoutSlots();
                RebuildContainersAndItems();
                LayoutItems();
            }
            else
                PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, true);
        }
        else if (widgetAction_ == WidgetAction::Resize)
            PlaceWidgetWithDisplacement(mouseDownWidgetIndex_, widgetPreviewCell_, widgetPreviewSpan_, false);
        // PendingMove/PendingResize: just cancel without displacement
        widgetAction_ = WidgetAction::None;
        widgetDockTarget_ = false;
        widgetDockTargetContainer_ = nullptr;
        widgetDockInsertIndex_ = 0;
        widgetCollectionGroupTargetIndex_ =
            static_cast<size_t>(-1);
        widgetCollectionGroupInsertIndex_ =
            static_cast<size_t>(-1);
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
        if (HandleDockClickRelease(upPoint))
            return;
        dockPressedEntry_ = static_cast<size_t>(-1);
        dockPressedFrequentItem_ = static_cast<size_t>(-1);
        dockPressedRunningAppKey_.clear();
        dockPressedWindowAction_ =
            snowdesktop::dock_window_rules::DockClickAction::None;
        dockPressedTargetWindow_ = nullptr;
        dockPressedContainer_ = nullptr;
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

    // Dock 项轻微拖动后仍落回原项时，按单击处理而不是吞掉本次操作。
    if (HandleDockClickRelease(upPoint))
        goto cleanup;

    if (!GetDockDragOutRemovalHint(upPoint).empty())
    {
        const bool removed = RemoveDockDragOutItems(dragSession_.Items());
        ClearSelection();
        EndDragSession();
        if (removed)
        {
            SaveLayoutSlots();
            RebuildContainersAndItems();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        goto cleanup;
    }

    if (!dragSession_.TargetContainer() ||
        dragSession_.TargetRegion() == HitRegion::None ||
        dragSession_.TargetRegion() == HitRegion::Blocked)
    {
        goto cleanup;
    }

    // Shell drop handlers may synchronously show a progress window. End the
    // interactive/visual phase before entering them, while retaining the
    // source and target context until EndDragSession() performs final cleanup.
    dragSession_.DeactivateForDrop();
    dragRenderCache_.Reset();
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    ReleaseCapture();
    InvalidateRect(hwnd_, nullptr, FALSE);

    {
        int mods = 0;
        if (GetAsyncKeyState(VK_CONTROL) & 0x8000) mods |= MK_CONTROL;
        if (GetAsyncKeyState(VK_MENU) & 0x8000)    mods |= MK_ALT;
        if (GetAsyncKeyState(VK_SHIFT) & 0x8000)   mods |= MK_SHIFT;

        if (dragSession_.TargetRegion() == HitRegion::Handoff && dragSession_.TargetSlot()
            && dragSession_.TargetSlot()->GetItem())
        {
            Item* targetItem = dragSession_.TargetSlot()->GetItem();
            if (auto* dockTarget = dynamic_cast<DockEntryItem*>(targetItem))
            {
                const size_t targetEntryIndex =
                    dockTarget->GetEntryIndex();
                if (dynamic_cast<DockContainer*>(
                        dragSession_.Source()) &&
                    targetEntryIndex <
                        dockEntries_.size() &&
                    IsRecycleBinDockEntry(
                        dockEntries_[targetEntryIndex]) &&
                    RemoveDockDragOutItems(
                        dragSession_.Items()))
                {
                    ClearSelection();
                    EndDragSession();
                    SaveLayoutSlots();
                    RebuildContainersAndItems();
                    LayoutItems();
                    InvalidateRect(
                        hwnd_, nullptr, FALSE);
                    goto cleanup;
                }
                if (dockTarget->GetEntryType() == DockEntryType::Collection)
                {
                    const bool executed = DropItemsIntoDockCollection(
                        dragSession_.Items(), dragSession_.Source(), dockTarget, mods);
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    if (executed)
                    {
                        RebuildContainersAndItems();
                        LayoutItems();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    goto cleanup;
                }
            }
            auto* targetDesktopIcon = dynamic_cast<DesktopIcon*>(targetItem);
            DesktopItem* targetDesktopItem = targetDesktopIcon
                ? targetDesktopIcon->GetDesktopItem() : nullptr;
            if (dynamic_cast<DockContainer*>(dragSession_.Source()) && targetDesktopItem &&
                _wcsicmp(targetDesktopItem->desktopIconClsid.c_str(),
                    kDesktopIconClsidRecycleBin) == 0)
            {
                MoveDockItemsToDesktop(dragSession_.Items(),
                    CellFromPointForDrag(dragSession_.CurrentPoint()));
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                goto cleanup;
            }
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
        if (needsReload)
        {
            RebuildContainersAndItems();
            ReloadItems();
        }
        else
        {
            // 内容变更可能使某些溢出页变空（后面有非空页时应立即清理顺延）
            // 先 ApplyPageMapping（可能重排 pageId），再 RebuildContainersAndItems + LayoutItems
            ApplyPageMapping();
            RebuildContainersAndItems();
            LayoutItems();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
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
    navAutoFlipTick_ = 0;
    mouseDown_ = false;
    mouseDownHit_ = nullptr;
    dockPressedEntry_ = static_cast<size_t>(-1);
    dockPressedFrequentItem_ = static_cast<size_t>(-1);
    dockPressedRunningAppKey_.clear();
    dockPressedWindowAction_ =
        snowdesktop::dock_window_rules::DockClickAction::None;
    dockPressedTargetWindow_ = nullptr;
    dockPressedContainer_ = nullptr;
    marqueeWidgetIndex_ = static_cast<size_t>(-1);
    ReleaseCapture();
}

inline bool DesktopApp::SuppressDesktopWidgetDragTargets() const
{
    if (!dragSession_.IsActive()) return false;
    return std::any_of(dragSession_.Items().begin(), dragSession_.Items().end(),
        [this](Item* item) {
            if (dynamic_cast<DockFrequentItem*>(item)) return true;
            const auto* dockItem = dynamic_cast<DockEntryItem*>(item);
            const size_t index = dockItem
                ? dockItem->GetEntryIndex() : static_cast<size_t>(-1);
            return index < dockEntries_.size() && dockEntries_[index].keepOnDesktop;
        });
}

inline std::wstring DesktopApp::GetDockDragOutRemovalHint(POINT point) const
{
    const auto* sourceDock = dynamic_cast<DockContainer*>(dragSession_.Source());
    if (!sourceDock) return L"";
    // A replicated Dock on another monitor is still a valid Dock target, not
    // a drag-out removal area.
    if (GetDockContainerAtPoint(point)) return L"";
    RECT sourceBounds = sourceDock->GetBounds();
    if (PtInRect(&sourceBounds, point)) return L"";

    for (Item* item : dragSession_.Items())
    {
        if (dynamic_cast<DockFrequentItem*>(item))
            return _LW("core.drag.remove_frequent");
        const auto* dockItem = dynamic_cast<DockEntryItem*>(item);
        const size_t index = dockItem
            ? dockItem->GetEntryIndex() : static_cast<size_t>(-1);
        if (index < dockEntries_.size() && dockEntries_[index].keepOnDesktop)
            return _LW("core.drag.remove_dock_map");
    }
    return L"";
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

    auto activeFileGroupMapping =
        [&](const DesktopWidget& group)
            -> size_t {
        if (group.type !=
            DesktopWidgetType::FileGroup)
            return static_cast<size_t>(-1);
        const size_t childIndex =
            FindWidgetIndexById(
                group.activeCategoryId);
        return childIndex < widgets_.size() &&
            widgets_[childIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[childIndex].
                sourceFolderPath.empty()
            ? childIndex
            : static_cast<size_t>(-1);
    };
    if (keyboardNavInsideWidget_ &&
        keyboardNavWidgetIndex_ < widgets_.size())
    {
        const size_t childIndex =
            activeFileGroupMapping(
                widgets_[keyboardNavWidgetIndex_]);
        if (childIndex < widgets_.size())
            return childIndex;
    }
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type ==
                DesktopWidgetType::FileGroup &&
            PtInRect(&widget.bounds,
                lastMousePoint_))
        {
            const size_t childIndex =
                activeFileGroupMapping(widget);
            if (childIndex < widgets_.size())
                return childIndex;
        }
        if (widget.type != DesktopWidgetType::FolderMapping || widget.sourceFolderPath.empty())
            continue;
        if (PtInRect(&widget.bounds, lastMousePoint_))
            return i;
    }

    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& widget = widgets_[i];
        if (widget.type ==
                DesktopWidgetType::FileGroup &&
            widget.selected)
        {
            const size_t childIndex =
                activeFileGroupMapping(widget);
            if (childIndex < widgets_.size())
                return childIndex;
        }
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
 * @brief 删除选中的文件夹条目
 * @param permanentDelete true 时永久删除并显示 Shell 确认对话框
 * @return 是否执行了删除操作
 */
inline bool DesktopApp::DeleteSelectedFolderEntries(bool permanentDelete)
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
    op.hwnd = ShellDialogOwnerHwnd();
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = static_cast<FILEOP_FLAGS>(permanentDelete
        ? FOF_WANTNUKEWARNING
        : (FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI));
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

    // Handle searchable widget keyboard input.
    {
        for (auto& c : containers_)
        {
            auto* searchable = dynamic_cast<ScrollingItemWidget*>(c.get());
            if (searchable && searchable->IsSearchFocused())
            {
                if (key == VK_ESCAPE)
                {
                    searchable->ClearSearchText();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (key == VK_BACK)
                {
                    searchable->BackspaceSearchText();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (key == VK_DELETE)
                {
                    searchable->DeleteSearchText();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (key == VK_LEFT)
                {
                    searchable->MoveCursorLeft();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (key == VK_RIGHT)
                {
                    searchable->MoveCursorRight();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (key == VK_HOME)
                {
                    searchable->MoveCursorHome();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                if (key == VK_END)
                {
                    searchable->MoveCursorEnd();
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    return;
                }
                break;
            }
        }
    }

    if (quickNavigationOpen_)
    {
        if (key == VK_ESCAPE)
        {
            CloseQuickNavigation();
            return;
        }
    }

    bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
    bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;

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

        if (DeleteSelectedFolderEntries(shift))
            break;

        cutPaths_.clear();
        std::vector<std::wstring> paths;
        for (const auto& item : items_)
        {
            if (!item.selected || !item.desktopIconClsid.empty()) continue;
            wchar_t path[MAX_PATH]{};
            if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
            {
                cutPaths_.erase(path);
                paths.push_back(path);
            }
        }

        if (!paths.empty())
        {
            std::wstring from;
            for (const auto& path : paths)
            {
                from += path;
                from.push_back(L'\0');
            }
            from.push_back(L'\0');

            SHFILEOPSTRUCTW op{};
            op.hwnd = ShellDialogOwnerHwnd();
            op.wFunc = FO_DELETE;
            op.pFrom = from.c_str();
            op.fFlags = static_cast<FILEOP_FLAGS>(shift
                ? FOF_WANTNUKEWARNING
                : (FOF_ALLOWUNDO | FOF_NOCONFIRMATION));
            if (SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted)
                ReloadItems();
        }
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
                info.hwnd = ShellDialogOwnerHwnd();
                info.lpVerb = "paste";
                info.nShow = SW_SHOWNORMAL;
                SafeInvokeCommand(bgMenu.Get(), &info);
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
    case VK_RETURN:
        if (keyboardNavInsideWidget_)
        {
            if (keyboardNavWidgetIndex_ < widgets_.size() &&
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
        if (keyboardNavInsideWidget_)
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
        if (keyboardNavInsideWidget_)
            NavigateWidgetMembers(key);
        else
            NavigateDesktopGrid(key);
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

    std::wstring hint = GetDockDragOutRemovalHint(dragSession_.CurrentPoint());
    if (hint.empty() && dragSession_.TargetContainer() &&
        dragSession_.TargetRegion() != HitRegion::None)
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
        InvalidateFloatingDockWindow(true);
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
    info.hwnd = ShellDialogOwnerHwnd();
    info.lpVerb = verb;
    info.nShow = SW_SHOWNORMAL;
    if (SafeInvokeCommand(contextMenu.Get(), &info))
        ReloadItems();
}

/**
 * @brief 在桌面网格上按 2D 空间导航选中项
 * @param arrowKey 方向键虚拟键码
 *
 * 收集当前可见页面上所有可导航目标（桌面图标 + 组件），
 * 按 gridCell 的 column/row 进行上下左右空间移动。
 * 没有任何选中时，从首列首行开始纵向搜索第一个目标。
 */
inline void DesktopApp::NavigateDesktopGrid(WPARAM arrowKey)
{
    if (items_.empty() && widgets_.empty()) return;

    // 构建当前可见页面 ID 集合
    std::unordered_set<std::wstring> visiblePageIds;
    for (const auto& gp : gridPages_)
        if (!gp.id.empty())
            visiblePageIds.insert(gp.id);
    const bool hasVisiblePages = !visiblePageIds.empty();

    struct Target { bool isWidget; size_t index; int column; int row; int colSpan; int rowSpan; std::wstring pageId; };
    std::vector<Target> targets;

    // 收集未收纳的桌面项（有名称、有边界、在可见页面上）
    for (size_t i = 0; i < items_.size(); ++i)
    {
        const auto& item = items_[i];
        if (item.name.empty()) continue;
        if (IsRectEmptyRect(item.bounds)) continue;
        if (collectedKeysCache_.contains(ToUpperInvariant(item.layoutKey))) continue;
        if (hasVisiblePages && !visiblePageIds.contains(item.gridCell.pageId)) continue;
        targets.push_back({ false, i,
            item.gridCell.column, item.gridCell.row,
            item.gridSpan.columns, item.gridSpan.rows,
            item.gridCell.pageId });
    }

    // 收集当前可见页面上的组件
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const auto& w = widgets_[i];
        if (IsGroupedWidget(w)) continue;
        if (hasVisiblePages && !visiblePageIds.contains(w.gridCell.pageId)) continue;
        targets.push_back({ true, i,
            w.gridCell.column, w.gridCell.row,
            w.gridSpan.columns, w.gridSpan.rows,
            w.gridCell.pageId });
    }

    if (targets.empty()) return;

    // 按列、行排序（纵向搜索：逐列从上到下）
    std::sort(targets.begin(), targets.end(), [](const Target& a, const Target& b) {
        if (a.column != b.column) return a.column < b.column;
        return a.row < b.row;
    });

    // 查找当前选中目标
    int currentIndex = -1;
    for (size_t i = 0; i < targets.size(); ++i)
    {
        const auto& t = targets[i];
        if (t.isWidget ? widgets_[t.index].selected : items_[t.index].selected)
        {
            currentIndex = static_cast<int>(i);
            break;
        }
    }

    if (currentIndex < 0)
    {
        // 无选中：选中鼠标所在页的第一个目标，不执行方向移动
        ClearSelection();
        // 确定鼠标所在的可见页面
        std::wstring mousePageId;
        {
            POINT screenPt{};
            GetCursorPos(&screenPt);
            for (const auto& gp : gridPages_)
            {
                if (!gp.id.empty() && PtInRect(&gp.workArea, screenPt))
                {
                    mousePageId = gp.id;
                    break;
                }
            }
        }
        // 在目标列表中找该页的第一个
        size_t firstIdx = 0;
        if (!mousePageId.empty())
        {
            bool foundOnPage = false;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                if (targets[i].pageId == mousePageId)
                {
                    firstIdx = i;
                    foundOnPage = true;
                    break;
                }
            }
            if (!foundOnPage)
                firstIdx = 0;   // 该页无目标，回退到首个
        }
        const auto& first = targets[firstIdx];
        if (first.isWidget)
            widgets_[first.index].selected = true;
        else
            items_[first.index].selected = true;
        InvalidateRect(hwnd_, nullptr, FALSE);
        return;
    }

    const Target& current = targets[static_cast<size_t>(currentIndex)];
    int nextIndex = -1;

    // 辅助：两个区间 [a, a+lenA) 与 [b, b+lenB) 是否相交
    auto spansOverlap = [](int a, int lenA, int b, int lenB) {
        return a < b + lenB && b < a + lenA;
    };

    switch (arrowKey)
    {
    case VK_UP:
    {
        int bestRow = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.column, current.colSpan, t.column, t.colSpan) &&
                t.row < current.row && t.row > bestRow)
            {
                bestRow = t.row;
                bestIdx = static_cast<int>(i);
            }
        }
        nextIndex = bestIdx;
        break;
    }
    case VK_DOWN:
    {
        int bestRow = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.column, current.colSpan, t.column, t.colSpan) &&
                t.row > current.row)
            {
                if (bestRow < 0 || t.row < bestRow)
                {
                    bestRow = t.row;
                    bestIdx = static_cast<int>(i);
                }
            }
        }
        nextIndex = bestIdx;
        break;
    }
    case VK_LEFT:
    {
        int bestCol = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.row, current.rowSpan, t.row, t.rowSpan) &&
                t.column < current.column && t.column > bestCol)
            {
                bestCol = t.column;
                bestIdx = static_cast<int>(i);
            }
        }
        if (bestIdx < 0)
        {
            int bestRow = -1;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                const auto& t = targets[i];
                if (t.pageId == current.pageId && t.row < current.row && t.row > bestRow)
                    bestRow = t.row;
            }
            if (bestRow >= 0)
            {
                bestCol = -1;
                for (size_t i = 0; i < targets.size(); ++i)
                {
                    const auto& t = targets[i];
                    if (t.pageId == current.pageId && t.row == bestRow &&
                        (bestCol < 0 || t.column > bestCol))
                    {
                        bestCol = t.column;
                        bestIdx = static_cast<int>(i);
                    }
                }
            }
        }
        nextIndex = bestIdx;
        break;
    }
    case VK_RIGHT:
    {
        int bestCol = -1;
        int bestIdx = -1;
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (static_cast<int>(i) == currentIndex) continue;
            const auto& t = targets[i];
            if (t.pageId == current.pageId &&
                spansOverlap(current.row, current.rowSpan, t.row, t.rowSpan) &&
                t.column > current.column)
            {
                if (bestCol < 0 || t.column < bestCol)
                {
                    bestCol = t.column;
                    bestIdx = static_cast<int>(i);
                }
            }
        }
        if (bestIdx < 0)
        {
            int bestRow = -1;
            for (size_t i = 0; i < targets.size(); ++i)
            {
                const auto& t = targets[i];
                if (t.pageId == current.pageId &&
                    t.row > current.row && (bestRow < 0 || t.row < bestRow))
                    bestRow = t.row;
            }
            if (bestRow >= 0)
            {
                bestCol = -1;
                for (size_t i = 0; i < targets.size(); ++i)
                {
                    const auto& t = targets[i];
                    if (t.pageId == current.pageId && t.row == bestRow &&
                        (bestCol < 0 || t.column < bestCol))
                    {
                        bestCol = t.column;
                        bestIdx = static_cast<int>(i);
                    }
                }
            }
        }
        nextIndex = bestIdx;
        break;
    }
    default:
        return;
    }

    if (nextIndex < 0) return;   // 该方向无有效目标

    ClearSelection();
    const auto& next = targets[static_cast<size_t>(nextIndex)];
    if (next.isWidget)
        widgets_[next.index].selected = true;
    else
        items_[next.index].selected = true;

    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 滚动组件以确保指定索引的成员项可见
 * @param widgetIndex 组件索引
 * @param memberIndex 成员索引
 *
 * 计算目标成员在组件内容区域中的近似纵向位置，
 * 如果超出当前滚动视口则调整 scrollOffset。
 */
inline void DesktopApp::ScrollWidgetToMember(size_t widgetIndex, int memberIndex)
{
    if (widgetIndex >= widgets_.size() || memberIndex < 0) return;
    auto& widget = widgets_[widgetIndex];

    WidgetContainer* wc = nullptr;
    for (auto& c : containers_)
    {
        auto* w = dynamic_cast<WidgetContainer*>(c.get());
        if (w && w->GetWidgetData() == &widget) { wc = w; break; }
    }
    if (!wc) return;

    int maxScroll = wc->GetMaxScrollOffset();
    if (maxScroll <= 0) return;

    // 按行比例计算目标滚动位置（适配含 gapY 的 FolderMapping/Collection）
    int columns = std::max(1, widget.gridSpan.columns);
    bool listMode = ((widget.type == DesktopWidgetType::CollectionGroup ||
                      widget.type == DesktopWidgetType::FileGroup) &&
                     (widget.listMode || widget.dateHeaders)) ||
                    (widget.type == DesktopWidgetType::FileCategories && (widget.listMode || widget.dateHeaders)) ||
                    (widget.type == DesktopWidgetType::FolderMapping && (widget.listMode || widget.dateHeaders)) ||
                    (widget.type == DesktopWidgetType::Collection && widget.listMode);

    size_t memberCount = (widget.type == DesktopWidgetType::FolderMapping)
        ? widget.folderEntries.size()
        : widget.itemKeys.size();
    if (widget.type == DesktopWidgetType::FileCategories)
    {
        auto* fc = dynamic_cast<FileCategories*>(wc);
        if (fc) memberCount = fc->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        auto* mapping = dynamic_cast<FolderMapping*>(wc);
        if (mapping) memberCount = mapping->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        auto* group = dynamic_cast<FileGroup*>(wc);
        if (group) memberCount = group->GetSlotCount();
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        auto* group = dynamic_cast<CollectionGroup*>(wc);
        if (group) memberCount = group->GetSlotCount();
    }
    if (memberCount <= 1) return;

    int totalRows = listMode
        ? static_cast<int>(memberCount)
        : (static_cast<int>(memberCount) + columns - 1) / columns;
    int row = listMode ? memberIndex : memberIndex / columns;

    int scroll = (totalRows <= 1) ? 0
        : static_cast<int>((static_cast<int64_t>(row) * maxScroll) / (totalRows - 1));

    scroll = std::clamp(scroll, 0, maxScroll);
    if (scroll != widget.scrollOffset)
    {
        widget.scrollOffset = scroll;
        if (auto* group =
                dynamic_cast<FileGroup*>(wc))
            group->InvalidateHostedView();
        else
            wc->InvalidateSlots();
    }
}

/**
 * @brief 在组件内部导航成员项
 * @param arrowKey 方向键虚拟键码
 *
 * 根据组件类型（Collection、FileCategories、FolderMapping）的列数布局，
 * 在组件成员项之间进行上下左右 2D 导航。list 模式的 FileCategories 使用线性上下移动。
 */
inline void DesktopApp::NavigateWidgetMembers(WPARAM arrowKey)
{
    if (keyboardNavWidgetIndex_ >= widgets_.size()) return;
    auto& widget = widgets_[keyboardNavWidgetIndex_];

    if (widget.type == DesktopWidgetType::FileGroup)
    {
        FileGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<FileGroup*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                group = candidate;
                break;
            }
        }
        if (!group) return;
        const auto& visibleSourceIds =
            group->GetVisibleSourceIds();
        const std::vector<std::wstring> sourceIds(
            visibleSourceIds.begin(),
            visibleSourceIds.end());
        if (sourceIds.empty()) return;
        const size_t groupIndex =
            keyboardNavWidgetIndex_;
        auto activeSourceIt = std::find(
            sourceIds.begin(), sourceIds.end(),
            group->GetActiveSourceId());
        const size_t activeSource =
            activeSourceIt == sourceIds.end()
                ? 0
                : static_cast<size_t>(std::distance(
                    sourceIds.begin(), activeSourceIt));

        auto focusSource = [&](size_t index,
            bool activate) {
            if (index >= sourceIds.size()) return;
            ClearSelection();
            if (activate)
            {
                widget.activeCategoryId =
                    sourceIds[index];
                widget.scrollOffset = 0;
                group->InvalidateHostedView();
            }
            const size_t childIndex =
                FindWidgetIndexById(sourceIds[index]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = true;
            keyboardNavFileGroupCategoryTabs_ = false;
            group->EnsureSourceTabVisible(index);
            if (activate) SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        const bool showCategoryRow =
            widget.showFileCategories &&
            !(widget.showSearchBox &&
              !group->GetSearchText().empty());
        const std::vector<std::wstring> categoryIds =
            showCategoryRow
                ? group->GetHostedVisibleCategoryIds()
                : std::vector<std::wstring>{};
        auto* activeContainer =
            group->GetActiveSourceContainer();
        DesktopWidget* activeData = activeContainer
            ? activeContainer->GetWidgetData() : nullptr;
        auto activeCategoryIt = activeData
            ? std::find(categoryIds.begin(),
                categoryIds.end(),
                activeData->activeCategoryId)
            : categoryIds.end();
        const size_t activeCategory =
            activeCategoryIt == categoryIds.end()
                ? 0
                : static_cast<size_t>(std::distance(
                    categoryIds.begin(), activeCategoryIt));

        auto focusCategory = [&](size_t index,
            bool activate) {
            if (!activeData ||
                index >= categoryIds.size())
                return;
            ClearSelection();
            if (activate)
            {
                activeData->activeCategoryId =
                    categoryIds[index];
                widget.scrollOffset = 0;
                group->InvalidateHostedView();
            }
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = true;
            if (activate) SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        const bool groupSearching =
            group->IsGroupSearchActive();
        const auto itemKeys = groupSearching
            ? std::vector<std::wstring>{}
            : group->GetHostedVisibleItemKeys();
        const auto folderIndices = groupSearching
            ? std::vector<size_t>{}
            : group->GetHostedVisibleFolderIndices();
        const size_t itemCount = groupSearching
            ? group->GetSlotCount()
            : (!itemKeys.empty()
                ? itemKeys.size()
                : folderIndices.size());
        auto selectItem = [&](size_t index) {
            if (index >= itemCount) return;
            ClearSelection();
            if (groupSearching)
            {
                Item* item =
                    group->GetMemberItem(index);
                if (!item) return;
                item->SetSelected(true);
            }
            else if (!itemKeys.empty())
            {
                const size_t itemIndex =
                    FindItemIndexByKey(itemKeys[index]);
                if (itemIndex >= items_.size()) return;
                items_[itemIndex].selected = true;
            }
            else if (activeData)
            {
                const size_t entryIndex =
                    folderIndices[index];
                if (entryIndex >=
                    activeData->folderEntries.size())
                    return;
                activeData->folderEntries[
                    entryIndex].selected = true;
            }
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ = groupIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(index);
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            ScrollWidgetToMember(groupIndex,
                static_cast<int>(index));
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        if (keyboardNavCollectionGroupTabs_)
        {
            size_t current =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        sourceIds.size() - 1)
                    : activeSource;
            if (arrowKey == VK_LEFT && current > 0)
                focusSource(current - 1, true);
            else if (arrowKey == VK_RIGHT &&
                     current + 1 < sourceIds.size())
                focusSource(current + 1, true);
            else if (arrowKey == VK_DOWN)
            {
                if (!categoryIds.empty())
                    focusCategory(activeCategory, false);
                else if (itemCount > 0)
                    selectItem(0);
            }
            return;
        }
        if (keyboardNavFileGroupCategoryTabs_)
        {
            if (categoryIds.empty())
            {
                focusSource(activeSource, false);
                return;
            }
            size_t current =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        categoryIds.size() - 1)
                    : activeCategory;
            if (arrowKey == VK_LEFT && current > 0)
                focusCategory(current - 1, true);
            else if (arrowKey == VK_RIGHT &&
                     current + 1 < categoryIds.size())
                focusCategory(current + 1, true);
            else if (arrowKey == VK_UP)
                focusSource(activeSource, false);
            else if (arrowKey == VK_DOWN &&
                     itemCount > 0)
                selectItem(0);
            return;
        }

        if (itemCount == 0)
        {
            if (groupSearching)
                return;
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else
                focusSource(activeSource, false);
            return;
        }
        size_t current =
            keyboardNavMemberIndex_ >= 0
                ? std::min(
                    static_cast<size_t>(
                        keyboardNavMemberIndex_),
                    itemCount - 1)
                : 0;
        const int columns =
            (widget.listMode || widget.dateHeaders)
                ? 1
                : std::max(1, widget.gridSpan.columns);
        if (arrowKey == VK_UP &&
            current < static_cast<size_t>(columns))
        {
            if (groupSearching)
                return;
            if (!categoryIds.empty())
                focusCategory(activeCategory, false);
            else
                focusSource(activeSource, false);
            return;
        }
        int next = static_cast<int>(current);
        if (arrowKey == VK_UP) next -= columns;
        else if (arrowKey == VK_DOWN) next += columns;
        else if (arrowKey == VK_LEFT) --next;
        else if (arrowKey == VK_RIGHT) ++next;
        else return;
        if (next >= 0 &&
            static_cast<size_t>(next) < itemCount)
            selectItem(static_cast<size_t>(next));
        return;
    }

    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        CollectionGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<CollectionGroup*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                group = candidate;
                break;
            }
        }
        if (!group) return;

        const auto& visibleChildIds =
            group->GetVisibleCollectionIds();
        const std::vector<std::wstring> childIds(
            visibleChildIds.begin(),
            visibleChildIds.end());
        const size_t groupWidgetIndex =
            keyboardNavWidgetIndex_;
        auto activeIt = std::find(
            childIds.begin(), childIds.end(),
            group->GetActiveCollectionId());
        size_t activeTab = activeIt == childIds.end()
            ? 0
            : static_cast<size_t>(
                std::distance(childIds.begin(), activeIt));

        auto focusTab = [&](size_t tabIndex,
            bool activate) {
            if (tabIndex >= childIds.size()) return;
            ClearSelection();
            if (activate)
            {
                widget.activeCategoryId =
                    childIds[tabIndex];
                widget.scrollOffset = 0;
                group->InvalidateFilterCache();
            }
            const size_t childIndex =
                FindWidgetIndexById(childIds[tabIndex]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavInsideWidget_ = true;
            keyboardNavWidgetIndex_ =
                groupWidgetIndex;
            keyboardNavMemberIndex_ =
                static_cast<int>(tabIndex);
            keyboardNavCollectionGroupTabs_ = true;
            group->EnsureTabVisible(tabIndex);
            if (activate)
                SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, FALSE);
        };

        if (keyboardNavCollectionGroupTabs_)
        {
            if (childIds.empty()) return;
            size_t currentTab =
                keyboardNavMemberIndex_ >= 0
                    ? std::min(
                        static_cast<size_t>(
                            keyboardNavMemberIndex_),
                        childIds.size() - 1)
                    : activeTab;
            if (arrowKey == VK_LEFT)
            {
                if (currentTab == 0) return;
                focusTab(currentTab - 1, true);
            }
            else if (arrowKey == VK_RIGHT)
            {
                if (currentTab + 1 >=
                    childIds.size())
                    return;
                focusTab(currentTab + 1, true);
            }
            else if (arrowKey == VK_DOWN)
            {
                const auto& keys =
                    group->GetVisibleItemKeys();
                if (keys.empty()) return;
                const size_t itemIndex =
                    FindItemIndexByKey(keys.front());
                if (itemIndex >= items_.size()) return;
                ClearSelection();
                items_[itemIndex].selected = true;
                keyboardNavInsideWidget_ = true;
                keyboardNavWidgetIndex_ =
                    groupWidgetIndex;
                keyboardNavMemberIndex_ = 0;
                keyboardNavCollectionGroupTabs_ =
                    false;
                ScrollWidgetToMember(
                    keyboardNavWidgetIndex_, 0);
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
            }
            return;
        }

        if (arrowKey == VK_UP && !childIds.empty())
        {
            const auto& keys =
                group->GetVisibleItemKeys();
            int current = keyboardNavMemberIndex_;
            for (size_t i = 0; i < keys.size(); ++i)
            {
                const size_t itemIndex =
                    FindItemIndexByKey(keys[i]);
                if (itemIndex < items_.size() &&
                    items_[itemIndex].selected)
                {
                    current = static_cast<int>(i);
                    break;
                }
            }
            const int columns = widget.listMode
                ? 1
                : std::max(
                    1, widget.gridSpan.columns);
            if (current >= 0 && current < columns)
            {
                focusTab(activeTab, false);
                return;
            }
        }
    }

    size_t memberCount = 0;
    int columns = 1;
    bool isListMode = false;

    switch (widget.type)
    {
    case DesktopWidgetType::CollectionGroup:
        memberCount = 0;
        columns = std::max(1, widget.gridSpan.columns);
        if (widget.listMode)
        {
            columns = 1;
            isListMode = true;
        }
        break;
    case DesktopWidgetType::Collection:
    case DesktopWidgetType::FileCategories:
        memberCount = widget.itemKeys.size();
        columns = std::max(1, widget.gridSpan.columns);
        if (widget.listMode || widget.dateHeaders)
        {
            columns = 1;
            isListMode = true;
        }
        break;
    case DesktopWidgetType::FolderMapping:
        memberCount = widget.folderEntries.size();
        columns = std::max(1, widget.gridSpan.columns);
        if (widget.listMode || widget.dateHeaders)
        {
            columns = 1;
            isListMode = true;
        }
        break;
    default:
        return;   // LuaScript、Guide 无内部导航
    }

    // Collection 弹窗打开时，按弹窗实际列数进行 2D 导航
    if (widget.type == DesktopWidgetType::Collection &&
        popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
        popupWidgetIndex_ < widgets_.size())
    {
        int popupCols = GetCollectionPopupColumnCount(popupRect_);
        if (popupCols > 0 && !isListMode)
            columns = popupCols;
    }

    // FileCategories：获取当前可见项目键列表（受搜索/分类标签页过滤）
    std::vector<std::wstring> visibleKeys;
    std::vector<size_t> visibleFolderIndices;
    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        for (auto& c : containers_)
        {
            auto* group =
                dynamic_cast<CollectionGroup*>(c.get());
            if (group && group->GetWidgetData() == &widget)
            {
                const auto& keys =
                    group->GetVisibleItemKeys();
                visibleKeys.assign(keys.begin(), keys.end());
                break;
            }
        }
        memberCount = visibleKeys.size();
        if (memberCount == 0) return;
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widget)
            {
                auto* fc = dynamic_cast<FileCategories*>(wc);
                if (fc)
                {
                    const auto& rk = fc->GetSearchResultKeys();
                    visibleKeys.assign(rk.begin(), rk.end());
                }
                break;
            }
        }
        memberCount = visibleKeys.size();
        if (memberCount == 0) return;
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        for (auto& c : containers_)
        {
            auto* mapping = dynamic_cast<FolderMapping*>(c.get());
            if (mapping && mapping->GetWidgetData() == &widget)
            {
                const auto& indices = mapping->GetVisibleEntryIndices();
                visibleFolderIndices.assign(indices.begin(), indices.end());
                break;
            }
        }
        memberCount = visibleFolderIndices.size();
        if (memberCount == 0) return;
    }
    else if (memberCount == 0)
    {
        return;
    }

    int currentIdx = keyboardNavMemberIndex_;
    if (currentIdx < 0) currentIdx = 0;

    // FileCategories：基于当前可见选中项确定导航起点
    if (!visibleKeys.empty())
    {
        int foundIdx = -1;
        for (size_t i = 0; i < visibleKeys.size(); ++i)
        {
            size_t itemIdx = FindItemIndexByKey(visibleKeys[i]);
            if (itemIdx != static_cast<size_t>(-1) && items_[itemIdx].selected)
            {
                foundIdx = static_cast<int>(i);
                break;
            }
        }
        if (foundIdx >= 0)
            currentIdx = foundIdx;
        else
            currentIdx = 0;
    }
    else if (!visibleFolderIndices.empty())
    {
        int foundIdx = -1;
        for (size_t i = 0; i < visibleFolderIndices.size(); ++i)
        {
            size_t entryIndex = visibleFolderIndices[i];
            if (entryIndex < widget.folderEntries.size() &&
                widget.folderEntries[entryIndex].selected)
            {
                foundIdx = static_cast<int>(i);
                break;
            }
        }
        currentIdx = foundIdx >= 0 ? foundIdx : 0;
    }

    int currentCol = currentIdx % columns;
    int currentRow = currentIdx / columns;
    int totalRows = static_cast<int>((memberCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));

    int nextRow = currentRow;
    int nextCol = currentCol;

    switch (arrowKey)
    {
    case VK_UP:
        nextRow = currentRow - 1;
        if (nextRow < 0) return;
        break;
    case VK_DOWN:
        nextRow = currentRow + 1;
        if (nextRow >= totalRows) return;
        break;
    case VK_LEFT:
        if (isListMode)
        {
            if (currentIdx <= 0) return;
        }
        else
        {
            nextCol = currentCol - 1;
            if (nextCol < 0)
            {
                // 换行：跳到上一行最后一列
                nextRow = currentRow - 1;
                if (nextRow < 0) return;
                nextCol = columns - 1;
            }
        }
        break;
    case VK_RIGHT:
        if (isListMode)
        {
            if (static_cast<size_t>(currentIdx) + 1 >= memberCount) return;
        }
        else
        {
            nextCol = currentCol + 1;
            if (nextCol >= columns)
            {
                // 换行：跳到下一行第一列
                nextRow = currentRow + 1;
                if (nextRow >= totalRows) return;
                nextCol = 0;
            }
        }
        break;
    default:
        return;
    }

    int nextIdx;
    if (isListMode)
    {
        if (arrowKey == VK_UP || arrowKey == VK_LEFT)
            nextIdx = currentIdx - 1;
        else
            nextIdx = currentIdx + 1;
        if (nextIdx < 0 || static_cast<size_t>(nextIdx) >= memberCount) return;
    }
    else
    {
        nextIdx = nextRow * columns + nextCol;
        if (nextIdx < 0 || static_cast<size_t>(nextIdx) >= static_cast<int>(memberCount)) return;
    }

    if (nextIdx == currentIdx) return;

    // 取消旧成员选中
    if (currentIdx >= 0)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            if (static_cast<size_t>(currentIdx) <
                visibleFolderIndices.size())
            {
                size_t entryIndex =
                    visibleFolderIndices[static_cast<size_t>(currentIdx)];
                if (entryIndex < widget.folderEntries.size())
                    widget.folderEntries[entryIndex].selected = false;
            }
        }
        else if (!visibleKeys.empty())
        {
            // 清除所有可见项选中状态，防止多选残留
            for (const auto& key : visibleKeys)
            {
                size_t itemIdx = FindItemIndexByKey(key);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
        else
        {
            const auto& keys = widget.itemKeys;
            if (static_cast<size_t>(currentIdx) < keys.size())
            {
                size_t itemIdx = FindItemIndexByKey(keys[static_cast<size_t>(currentIdx)]);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
    }

    // 选中新成员
    if (widget.type == DesktopWidgetType::FolderMapping)
    {
        size_t entryIndex =
            visibleFolderIndices[static_cast<size_t>(nextIdx)];
        if (entryIndex >= widget.folderEntries.size()) return;
        widget.folderEntries[entryIndex].selected = true;
    }
    else if (!visibleKeys.empty())
    {
        size_t itemIdx = FindItemIndexByKey(visibleKeys[static_cast<size_t>(nextIdx)]);
        if (itemIdx != static_cast<size_t>(-1))
            items_[itemIdx].selected = true;
    }
    else
    {
        const auto& keys = widget.itemKeys;
        size_t itemIdx = FindItemIndexByKey(keys[static_cast<size_t>(nextIdx)]);
        if (itemIdx != static_cast<size_t>(-1))
            items_[itemIdx].selected = true;
    }

    keyboardNavMemberIndex_ =
        widget.type == DesktopWidgetType::FolderMapping
            ? static_cast<int>(
                visibleFolderIndices[static_cast<size_t>(nextIdx)])
            : nextIdx;

    // 确保选中的成员项可见
    ScrollWidgetToMember(keyboardNavWidgetIndex_, nextIdx);

    // Collection 大文件夹模式：超界自动弹窗 / 退回内联区域自动关闭
    if (widget.type == DesktopWidgetType::Collection && !widget.scrollContainerMode)
    {
        int cols = std::max(1, widget.gridSpan.columns);
        int rows = std::max(1, widget.gridSpan.rows);
        size_t inlineCap = (cols <= 1 && rows <= 1) ? 4
                           : static_cast<size_t>(cols * rows - 1);
        if (static_cast<size_t>(nextIdx) >= inlineCap)
        {
            if (popupWidgetIndex_ != keyboardNavWidgetIndex_)
                OpenCollectionPopupAt(keyboardNavWidgetIndex_,
                    POINT{ widget.bounds.left, widget.bounds.top });
        }
        else if (popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
            (cols > 1 || rows > 1))   // 紧凑模式保持弹窗常开
        {
            // 退回内联区域：关闭弹窗（不调用 CloseCollectionPopup，
            // 因为它会 ClearSelection 清除刚导航选中的成员项）
            popupWidgetIndex_ = static_cast<size_t>(-1);
            popupScrollOffset_ = 0;
            popupHasAnchor_ = false;
            popupAnchoredToDock_ = false;
            popupAnchorPoint_ = {};
            popupPageId_.clear();
            popupCategoryId_.clear();
            popupRect_ = {};
        }
    }

    // 弹窗滚动跟随（若弹窗仍打开）
    if (popupWidgetIndex_ == keyboardNavWidgetIndex_ &&
        popupWidgetIndex_ < widgets_.size())
    {
        RECT rPopup = popupRect_;
        int popupCols = GetCollectionPopupColumnCount(rPopup);
        int popupRow = nextIdx / std::max(1, popupCols);
        int cellH = kMinCellHeight;
        for (const auto& page : gridPages_)
            if (page.id == popupPageId_) { cellH = page.cellHeight; break; }
        RECT content = GetCollectionPopupContentRect(rPopup);
        int viewH = std::max(1, static_cast<int>(content.bottom - content.top));
        int targetY = popupRow * cellH;
        int maxPopupScroll = GetCollectionPopupMaxScrollOffset(
            widgets_[keyboardNavWidgetIndex_], rPopup);
        if (targetY < popupScrollOffset_)
            popupScrollOffset_ = targetY;
        else if (targetY + cellH > popupScrollOffset_ + viewH)
            popupScrollOffset_ = targetY + cellH - viewH;
        popupScrollOffset_ = std::clamp(popupScrollOffset_, 0, maxPopupScroll);
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 进入当前选中的组件内部进行导航
 *
 * 清除桌面层面选中，将导航上下文切换到组件内部，
 * 并选中组件的第一个成员项。
 */
inline void DesktopApp::EnterWidget()
{
    int foundIdx = -1;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].selected) { foundIdx = static_cast<int>(i); break; }
    }
    if (foundIdx < 0) return;

    const auto& widget = widgets_[static_cast<size_t>(foundIdx)];
    if (widget.type == DesktopWidgetType::LuaScript ||
        widget.type == DesktopWidgetType::Guide)
        return;   // 此类组件无内部成员导航

    ClearSelection();

    keyboardNavInsideWidget_ = true;
    keyboardNavWidgetIndex_ = static_cast<size_t>(foundIdx);
    keyboardNavMemberIndex_ = 0;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;

    if (widget.type == DesktopWidgetType::FolderMapping)
    {
        size_t entryIndex = static_cast<size_t>(-1);
        for (auto& c : containers_)
        {
            auto* mapping = dynamic_cast<FolderMapping*>(c.get());
            if (mapping && mapping->GetWidgetData() == &widget)
            {
                const auto& visibleEntries =
                    mapping->GetVisibleEntryIndices();
                if (!visibleEntries.empty())
                    entryIndex = visibleEntries.front();
                break;
            }
        }
        if (entryIndex < widget.folderEntries.size())
        {
            widgets_[static_cast<size_t>(foundIdx)]
                .folderEntries[entryIndex].selected = true;
            keyboardNavMemberIndex_ = static_cast<int>(entryIndex);
        }
        else
        {
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        FileGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<FileGroup*>(c.get());
            if (candidate &&
                candidate->GetWidgetData() == &widget)
            {
                group = candidate;
                break;
            }
        }
        std::vector<std::wstring> sourceIds;
        if (group)
        {
            const auto& visible =
                group->GetVisibleSourceIds();
            sourceIds.assign(
                visible.begin(), visible.end());
        }
        if (group &&
            group->IsGroupSearchActive())
        {
            if (group->GetSlotCount() == 0)
            {
                keyboardNavInsideWidget_ = false;
                keyboardNavWidgetIndex_ =
                    static_cast<size_t>(-1);
                keyboardNavMemberIndex_ = -1;
                widgets_[static_cast<size_t>(
                    foundIdx)].selected = true;
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                return;
            }
            Item* item = group->GetMemberItem(0);
            if (!item)
            {
                keyboardNavInsideWidget_ = false;
                keyboardNavWidgetIndex_ =
                    static_cast<size_t>(-1);
                keyboardNavMemberIndex_ = -1;
                widgets_[static_cast<size_t>(
                    foundIdx)].selected = true;
                InvalidateRect(
                    hwnd_, nullptr, FALSE);
                return;
            }
            item->SetSelected(true);
            keyboardNavMemberIndex_ = 0;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
        }
        else if (!group || sourceIds.empty())
        {
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ =
                static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(
                foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        else
        {
            auto active = std::find(
                sourceIds.begin(), sourceIds.end(),
                group->GetActiveSourceId());
            const size_t tabIndex =
                active == sourceIds.end()
                    ? 0
                    : static_cast<size_t>(
                        std::distance(
                            sourceIds.begin(), active));
            widgets_[static_cast<size_t>(foundIdx)]
                .activeCategoryId = sourceIds[tabIndex];
            const size_t childIndex =
                FindWidgetIndexById(
                    sourceIds[tabIndex]);
            if (childIndex < widgets_.size())
                widgets_[childIndex].selected = true;
            keyboardNavMemberIndex_ =
                static_cast<int>(tabIndex);
            keyboardNavCollectionGroupTabs_ = true;
            keyboardNavFileGroupCategoryTabs_ = false;
            group->EnsureSourceTabVisible(tabIndex);
        }
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        CollectionGroup* group = nullptr;
        for (auto& c : containers_)
        {
            auto* candidate =
                dynamic_cast<CollectionGroup*>(c.get());
            if (!candidate ||
                candidate->GetWidgetData() != &widget)
                continue;
            group = candidate;
            break;
        }
        if (!group)
        {
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ =
                static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        const auto& childIds =
            group->GetVisibleCollectionIds();
        if (childIds.empty())
        {
            keyboardNavInsideWidget_ = false;
            keyboardNavWidgetIndex_ =
                static_cast<size_t>(-1);
            keyboardNavMemberIndex_ = -1;
            keyboardNavCollectionGroupTabs_ = false;
            keyboardNavFileGroupCategoryTabs_ = false;
            widgets_[static_cast<size_t>(foundIdx)].selected = true;
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
        auto active = std::find(
            childIds.begin(), childIds.end(),
            group->GetActiveCollectionId());
        const size_t tabIndex =
            active == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), active));
        widgets_[static_cast<size_t>(foundIdx)]
            .activeCategoryId = childIds[tabIndex];
        const size_t childIndex =
            FindWidgetIndexById(childIds[tabIndex]);
        if (childIndex < widgets_.size())
            widgets_[childIndex].selected = true;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        group->EnsureTabVisible(tabIndex);
    }
    else
    {
        if (!widget.itemKeys.empty())
        {
            size_t itemIdx = FindItemIndexByKey(widget.itemKeys[0]);
            if (itemIdx != static_cast<size_t>(-1))
                items_[itemIdx].selected = true;
        }
    }

    // 1 格集合（紧凑模式）：进入时直接打开弹窗
    if (widget.type == DesktopWidgetType::Collection && !widget.scrollContainerMode)
    {
        int cols = std::max(1, widget.gridSpan.columns);
        int rows = std::max(1, widget.gridSpan.rows);
        if (cols <= 1 && rows <= 1)
            OpenCollectionPopupAt(static_cast<size_t>(foundIdx),
                POINT{ widget.bounds.left, widget.bounds.top });
    }

    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 退出组件内部导航，返回桌面网格
 *
 * 清除组件内成员选中，恢复父组件的选中状态，
 * 并将导航上下文切换回桌面网格。
 */
inline void DesktopApp::ExitWidget()
{
    if (!keyboardNavInsideWidget_) return;

    size_t wi = keyboardNavWidgetIndex_;

    if (wi < widgets_.size())
    {
        auto& widget = widgets_[wi];
        if (widget.type == DesktopWidgetType::FolderMapping)
        {
            for (auto& e : widget.folderEntries)
                e.selected = false;
        }
        else if (widget.type == DesktopWidgetType::CollectionGroup)
        {
            for (const auto& childId : widget.childWidgetIds)
            {
                const size_t childIndex =
                    FindWidgetIndexById(childId);
                if (childIndex < widgets_.size())
                {
                    widgets_[childIndex].selected = false;
                    for (const auto& key :
                        widgets_[childIndex].itemKeys)
                    {
                        const size_t itemIndex =
                            FindItemIndexByKey(key);
                        if (itemIndex < items_.size())
                            items_[itemIndex].selected = false;
                    }
                }
            }
        }
        else if (widget.type == DesktopWidgetType::FileGroup)
        {
            for (const auto& childId :
                widget.childWidgetIds)
            {
                const size_t childIndex =
                    FindWidgetIndexById(childId);
                if (childIndex >= widgets_.size())
                    continue;
                DesktopWidget& child =
                    widgets_[childIndex];
                child.selected = false;
                for (auto& entry : child.folderEntries)
                    entry.selected = false;
                for (const auto& key : child.itemKeys)
                {
                    const size_t itemIndex =
                        FindItemIndexByKey(key);
                    if (itemIndex < items_.size())
                        items_[itemIndex].selected = false;
                }
            }
        }
        else
        {
            for (const auto& key : widget.itemKeys)
            {
                size_t itemIdx = FindItemIndexByKey(key);
                if (itemIdx != static_cast<size_t>(-1))
                    items_[itemIdx].selected = false;
            }
        }
    }

    keyboardNavInsideWidget_ = false;
    keyboardNavWidgetIndex_ = static_cast<size_t>(-1);
    keyboardNavMemberIndex_ = -1;
    keyboardNavCollectionGroupTabs_ = false;
    keyboardNavFileGroupCategoryTabs_ = false;

    // 退出组件时关闭其弹窗
    if (popupWidgetIndex_ == wi)
        CloseCollectionPopup();

    if (wi < widgets_.size())
        widgets_[wi].selected = true;

    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 打开当前选中的桌面项
 *
 * 遍历 items_ 查找选中的项，通过 ShellExecuteW 以 "open" 动词启动。
 */
inline void DesktopApp::OpenSelectedDesktopItem()
{
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].selected && !items_[i].name.empty() &&
            !items_[i].parsingName.empty())
        {
            LaunchDesktopItem(i);
            break;
        }
    }
}

/**
 * @brief 打开组件内指定索引的成员项
 * @param widgetIndex 组件索引
 * @param memberIndex 成员索引（-1 表示无成员选中）
 *
 * 根据组件类型，通过 ShellExecuteW 打开对应的文件或桌面项。
 */
inline void DesktopApp::OpenWidgetMember(size_t widgetIndex, int memberIndex)
{
    if (widgetIndex >= widgets_.size() || memberIndex < 0) return;
    const auto& widget = widgets_[widgetIndex];
    if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        for (auto& c : containers_)
        {
            auto* group =
                dynamic_cast<CollectionGroup*>(c.get());
            if (!group || group->GetWidgetData() != &widget)
                continue;
            const auto& keys = group->GetVisibleItemKeys();
            if (static_cast<size_t>(memberIndex) < keys.size())
            {
                const size_t itemIndex =
                    FindItemIndexByKey(
                        keys[static_cast<size_t>(memberIndex)]);
                if (itemIndex < items_.size())
                    LaunchDesktopItem(itemIndex);
            }
            break;
        }
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        for (auto& c : containers_)
        {
            auto* group =
                dynamic_cast<FileGroup*>(c.get());
            if (!group ||
                group->GetWidgetData() != &widget)
                continue;
            if (group->IsGroupSearchActive())
            {
                Item* item = group->GetMemberItem(
                    static_cast<size_t>(memberIndex));
                if (auto* desktop =
                        dynamic_cast<DesktopIcon*>(item))
                {
                    DesktopItem* source =
                        desktop->GetDesktopItem();
                    if (source)
                    {
                        const size_t itemIndex =
                            FindItemIndexByKey(
                                source->layoutKey);
                        if (itemIndex < items_.size())
                            LaunchDesktopItem(itemIndex);
                    }
                }
                else if (item &&
                         !item->GetPath().empty())
                    ShellExecuteW(
                        nullptr, L"open",
                        item->GetPath().c_str(),
                        nullptr, nullptr,
                        SW_SHOWNORMAL);
                break;
            }
            const auto keys =
                group->GetHostedVisibleItemKeys();
            if (static_cast<size_t>(memberIndex) <
                keys.size())
            {
                const size_t itemIndex =
                    FindItemIndexByKey(
                        keys[static_cast<size_t>(
                            memberIndex)]);
                if (itemIndex < items_.size())
                    LaunchDesktopItem(itemIndex);
                break;
            }
            const auto entries =
                group->
                    GetHostedVisibleFolderIndices();
            auto* active =
                group->GetActiveSourceContainer();
            DesktopWidget* activeData = active
                ? active->GetWidgetData() : nullptr;
            if (activeData &&
                static_cast<size_t>(memberIndex) <
                    entries.size())
            {
                const size_t entryIndex =
                    entries[static_cast<size_t>(
                        memberIndex)];
                if (entryIndex <
                    activeData->folderEntries.size())
                {
                    const auto& entry =
                        activeData->
                            folderEntries[entryIndex];
                    if (!entry.fullPath.empty())
                        ShellExecuteW(nullptr, L"open",
                            entry.fullPath.c_str(),
                            nullptr, nullptr,
                            SW_SHOWNORMAL);
                }
            }
            break;
        }
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        if (static_cast<size_t>(memberIndex) < widget.folderEntries.size())
        {
            const auto& entry = widget.folderEntries[static_cast<size_t>(memberIndex)];
            if (!entry.fullPath.empty())
                ShellExecuteW(nullptr, L"open", entry.fullPath.c_str(),
                    nullptr, nullptr, SW_SHOWNORMAL);
        }
    }
    else if (!widget.itemKeys.empty() &&
        static_cast<size_t>(memberIndex) < widget.itemKeys.size())
    {
        size_t itemIdx = FindItemIndexByKey(
            widget.itemKeys[static_cast<size_t>(memberIndex)]);
        if (itemIdx != static_cast<size_t>(-1) &&
            !items_[itemIdx].parsingName.empty())
        {
            LaunchDesktopItem(itemIdx);
        }
    }
}

/**
 * @brief 处理页面导航按钮点击事件（上一页/下一页）
 * @param point 点击坐标
 * @return 是否已处理导航
 */
inline bool DesktopApp::HandlePageNavClick(POINT point)
{
    if (gridPages_.empty()) return false;
    if (MaxPageOffset() <= 0) return false;   // 无溢出页时不处理

    const bool hasPrev = pageOffset_ > 0;
    const bool hasNext = pageOffset_ < MaxPageOffset();

    RECT prevRect, nextRect;
    GetNavButtonRects(prevRect, nextRect);

    int delta = 0;
    if (PtInRect(&prevRect, point)) delta = -1;
    else if (PtInRect(&nextRect, point)) delta = 1;

    // 点击落在导航按钮区域内但方向不可用 → 拦截点击（不穿透到下方图标）
    if (delta != 0 && !((delta == -1 && hasPrev) || (delta == 1 && hasNext)))
        return true;
    if (delta == 0) return false;

    int newOffset = NextNonEmptyOffset(pageOffset_, delta);
    if (newOffset == pageOffset_) return false;

    bool wasDragging = dragSession_.IsActive();
    // 保存迁移前第一个选中项的实际 bounds（含页面渲染尺寸差异）
    RECT oldFirstBounds{};
    bool hasOldBounds = false;
    if (wasDragging)
    {
        for (const auto& item : items_)
        {
            if (item.selected && !item.name.empty())
            {
                oldFirstBounds = item.bounds;
                hasOldBounds = !IsRectEmptyRect(oldFirstBounds);
                break;
            }
        }
    }
    pageOffset_ = newOffset;
    ApplyPageMapping();
    if (wasDragging) MigrateSelectedItemsToLastMonitorPage();
    LayoutItems();
    if (wasDragging && !dragSession_.IsActive())
    {
        mouseDownHit_ = nullptr;
        mouseDown_ = false;
    }
    wasDragging = wasDragging && dragSession_.IsActive();
    if (wasDragging) InvalidateDragStaticScene();
    if (wasDragging)
    {
        UpdateDragGroupOrigin();
        // 用实际 bounds 差值补偿 mouseDown，消除跨页渲染尺寸差异导致的视觉跳动
        if (hasOldBounds)
        {
            for (const auto& item : items_)
            {
                if (item.selected && !item.name.empty())
                {
                    dragSession_.AdjustMouseDownPoint({
                        item.bounds.left - oldFirstBounds.left,
                        item.bounds.top  - oldFirstBounds.top
                    });
                    break;
                }
            }
        }
    }
    // 页面迁移后预览缓存失效
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = { -1, -1 };
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

    if (DockContainer* dock = GetDockContainerAtPoint(pt))
    {
        if (DockEntryItem* dockItem = dock->EntryAtPoint(pt))
        {
            const size_t entryIndex = dockItem->GetEntryIndex();
            if (entryIndex < dockEntries_.size())
            {
                ClearSelection();
                dockItem->SetSelected(true);
                const RECT dockItemBounds = dock->GetElementVisualRect(
                    dockItem->GetBounds(), pt);
                if (dockItem->GetEntryType() == DockEntryType::DesktopItem)
                {
                    size_t itemIndex = FindItemIndexByKey(dockItem->GetReference());
                    if (itemIndex < items_.size())
                    {
                        items_[itemIndex].selected =
                            dockItem->IsSelected();
                        items_[itemIndex].bounds = dockItemBounds;
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        if (IsProtectedDesktopIcon(items_[itemIndex]))
                            ShowShellContextMenu(
                                screenPt,
                                static_cast<int>(itemIndex),
                                false, dockItemBounds);
                        else
                            ShowItemContextMenu(
                                screenPt,
                                static_cast<int>(itemIndex),
                                false, false, dockItemBounds,
                                dockEntries_[entryIndex].
                                        keepOnDesktop
                                    ? std::optional<size_t>(
                                          entryIndex)
                                    : std::nullopt);
                    }
                }
                else
                {
                    size_t widgetIndex = FindWidgetIndexById(dockItem->GetReference());
                    if (widgetIndex < widgets_.size())
                    {
                        widgets_[widgetIndex].selected =
                            dockItem->IsSelected();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                        ShowWidgetContextMenu(
                            screenPt, widgetIndex,
                            dockItemBounds);
                    }
                }
                return;
            }
        }

        if (DockFrequentItem* frequentItem = dock->FrequentItemAtPoint(pt))
        {
            const size_t itemIndex = frequentItem->GetItemIndex();
            if (itemIndex < items_.size())
            {
                ClearSelection();
                frequentItem->SetSelected(true);
                items_[itemIndex].bounds = dock->GetElementVisualRect(
                    frequentItem->GetBounds(), pt);
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (IsProtectedDesktopIcon(items_[itemIndex]))
                    ShowShellContextMenu(
                        screenPt,
                        static_cast<int>(itemIndex),
                        false, dock->GetElementVisualRect(
                            frequentItem->GetBounds(), pt));
                else
                    ShowItemContextMenu(
                        screenPt,
                        static_cast<int>(itemIndex),
                        true, false,
                        dock->GetElementVisualRect(
                            frequentItem->GetBounds(), pt));
                return;
            }
        }

        if (dock->ContainsInteractivePoint(pt))
        {
            ClearSelection();
            InvalidateRect(hwnd_, nullptr, FALSE);
            ShowDockContextMenu(screenPt);
            return;
        }
    }

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

    // File-group source tabs own their context menu; do not let the
    // surrounding widget frame consume a tab right-click.
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        auto* group =
            dynamic_cast<FileGroup*>(it->get());
        if (!group) continue;
        const std::wstring childId =
            group->SourceIdAtPoint(pt);
        if (childId.empty()) continue;
        DesktopWidget* groupData = group->GetWidgetData();
        const size_t groupIndex = groupData
            ? FindWidgetIndexById(groupData->id)
            : static_cast<size_t>(-1);
        const size_t childIndex =
            FindWidgetIndexById(childId);
        if (groupIndex >= widgets_.size() ||
            childIndex >= widgets_.size())
            break;

        const auto childIds =
            group->GetVisibleSourceIds();
        const auto childIt = std::find(
            childIds.begin(), childIds.end(), childId);
        const size_t tabIndex =
            childIt == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), childIt));
        ClearSelection();
        widgets_[groupIndex].activeCategoryId = childId;
        widgets_[groupIndex].scrollOffset = 0;
        widgets_[childIndex].selected = true;
        group->InvalidateHostedView();
        group->EnsureSourceTabVisible(tabIndex);
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowFileGroupSourceTabContextMenu(
            screenPt, groupIndex, childId);
        return;
    }

    // Collection-group tabs own their context menu.
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        auto* group =
            dynamic_cast<CollectionGroup*>(it->get());
        if (!group) continue;
        const std::wstring collectionId =
            group->CategoryIdAtPoint(pt);
        if (collectionId.empty()) continue;

        DesktopWidget* groupData =
            group->GetWidgetData();
        size_t groupIndex =
            static_cast<size_t>(-1);
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (&widgets_[i] == groupData)
            {
                groupIndex = i;
                break;
            }
        }
        const size_t childIndex =
            FindWidgetIndexById(collectionId);
        if (groupIndex >= widgets_.size() ||
            childIndex >= widgets_.size())
            break;

        const auto& childIds =
            group->GetVisibleCollectionIds();
        auto childIt = std::find(
            childIds.begin(), childIds.end(),
            collectionId);
        const size_t tabIndex =
            childIt == childIds.end()
                ? 0
                : static_cast<size_t>(
                    std::distance(
                        childIds.begin(), childIt));
        ClearSelection();
        widgets_[groupIndex].activeCategoryId =
            collectionId;
        widgets_[groupIndex].scrollOffset = 0;
        widgets_[childIndex].selected = true;
        group->InvalidateFilterCache();
        group->EnsureTabVisible(tabIndex);
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        keyboardNavMemberIndex_ =
            static_cast<int>(tabIndex);
        keyboardNavCollectionGroupTabs_ = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, FALSE);
        ShowCollectionGroupTabContextMenu(
            screenPt, groupIndex, collectionId);
        return;
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
    if (timerId >= kWidgetTimerIdBase)
    {
        auto it = widgetTimerIds_.find(static_cast<UINT_PTR>(timerId));
        if (it != widgetTimerIds_.end())
        {
            if (widgetEngine_)
                widgetEngine_->OnWidgetTimer(it->second);
            return;
        }
    }

    if (timerId == kDisplayTopologyRefreshTimerId)
    {
        if (controlHwnd_ && IsWindow(controlHwnd_))
            KillTimer(controlHwnd_, kDisplayTopologyRefreshTimerId);
        if (hwnd_ && IsWindow(hwnd_))
            KillTimer(hwnd_, kDisplayTopologyRefreshTimerId);
        RefreshDisplayTopologyIfChanged();
    }
    else if (timerId == kShellChangeTimerId)
    {
        KillTimer(hwnd_, kShellChangeTimerId);
        if (!mouseDown_ && !reloading_)
            ReloadItems();
    }
    else if (timerId == kRecycleBinPollTimerId)
    {
        const auto pollState = recycleBinPollState_;
        if (pollState->queryInFlight.exchange(true))
            return;
        const HWND target = hwnd_;
        pollState->targetWindow = target;
        std::thread([target, pollState] {
            SHQUERYRBINFO info{};
            info.cbSize = sizeof(info);
            const HRESULT result = SHQueryRecycleBinW(nullptr, &info);
            if (SUCCEEDED(result))
            {
                const int64_t previousCount = pollState->itemCount.exchange(info.i64NumItems);
                if (previousCount >= 0 && previousCount != info.i64NumItems &&
                    pollState->targetWindow.load() == target)
                    PostMessageW(target, kShellChangeMessage, 0, 0);
            }
            pollState->queryInFlight = false;
        }).detach();
    }
    else if (timerId == kDesktopHostWatchTimerId)
    {
        // Restore the Explorer-owned desktop host first. Hook injection can
        // take time while the new taskbar XAML tree is still starting up.
        WatchDesktopHost();
        const DWORD now = GetTickCount();
        if (IsSystemTaskbarHookRequired(dockSettings_) &&
            (systemTaskbarBackdropRefreshTick_ == 0 ||
                now - systemTaskbarBackdropRefreshTick_ >= 1500))
        {
            RefreshSystemTaskbarAppearance(true);
        }
    }
    else if (timerId == kWidgetRefreshTimerId)
    {
        if (widgetEngine_)
            widgetEngine_->TickRuntime();
        RefreshDockRunningWindows();
    }
    else if (timerId == kDockWindowPreviewHoverTimerId)
    {
        OnDockWindowPreviewHoverTimer();
    }
    else if (timerId == kDockLaunchBounceTimerId)
    {
        OnDockLaunchBounceTimer();
    }
    else if (timerId == kFloatingDockEdgeSwipeTimerId)
    {
        UpdateFloatingDockEdgeSwipe();
    }
    else if (timerId == kTaskbarRevealGuardTimerId)
    {
        UpdateSystemTaskbarRevealGuard();
        const DWORD now = GetTickCount();
        const DWORD foregroundTick = dockForegroundChangedTick_.load();
        const DWORD windowStateTick =
            systemTaskbarWindowStateChangedTick_.load();
        const bool foregroundChanged = foregroundTick != 0 &&
            foregroundTick != systemTaskbarBackdropForegroundTick_;
        const bool windowStateChanged = windowStateTick !=
            systemTaskbarWindowStateObservedTick_;
        const bool foregroundSettling = foregroundTick != 0 &&
            now - foregroundTick <= 400 &&
            now - systemTaskbarBackdropRefreshTick_ >= 100;
        if (IsSystemTaskbarHookRequired(dockSettings_) &&
            (foregroundChanged || windowStateChanged || foregroundSettling))
        {
            RefreshSystemTaskbarAppearance(true);
            systemTaskbarBackdropForegroundTick_ = foregroundTick;
            if (hwnd_ && IsWindow(hwnd_))
                InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    else if (timerId == kCollectionPopupDwellTimerId)
    {
        if (!dragSession_.IsActive() ||
            popupDwellWidgetIndex_ >= widgets_.size() ||
            popupDwellWidgetIndex_ == popupWidgetIndex_)
        {
            popupDwellWidgetIndex_ = static_cast<size_t>(-1);
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            return;
        }

        if (TryOpenDwellCollectionPopup(GetTickCount()))
        {
            KillTimer(hwnd_, kCollectionPopupDwellTimerId);
            OnMouseMove(0, MAKELPARAM(lastMousePoint_.x, lastMousePoint_.y));
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kCollectionGroupTabDwellTimerId)
    {
        if (!dragSession_.IsActive() ||
            collectionGroupTabDwellWidgetIndex_ >=
                widgets_.size() ||
            collectionGroupTabDwellId_.empty())
        {
            collectionGroupTabDwellWidgetIndex_ =
                static_cast<size_t>(-1);
            collectionGroupTabDwellId_.clear();
            KillTimer(
                hwnd_, kCollectionGroupTabDwellTimerId);
            return;
        }

        if (TryActivateCollectionGroupTab(GetTickCount()))
        {
            KillTimer(
                hwnd_, kCollectionGroupTabDwellTimerId);
            OnMouseMove(
                0, MAKELPARAM(
                    lastMousePoint_.x,
                    lastMousePoint_.y));
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kDockHandoffDwellTimerId)
    {
        if (!dragSession_.IsActive() || dockHandoffDwellIndex_ == static_cast<size_t>(-1))
        {
            KillTimer(hwnd_, kDockHandoffDwellTimerId);
            dockHandoffDwellReady_ = false;
            return;
        }
        if (GetTickCount() - dockHandoffDwellStartTick_ >= kDockHandoffDwellDelayMs)
        {
            dockHandoffDwellReady_ = true;
            KillTimer(hwnd_, kDockHandoffDwellTimerId);
            if (!externalDragActive_)
                OnMouseMove(0, MAKELPARAM(
                    dragSession_.CurrentPoint().x, dragSession_.CurrentPoint().y));
            InvalidateRect(hwnd_, nullptr, FALSE);
            InvalidateFloatingDockWindow(true);
        }
    }
    else if (timerId == kPageNotifyTimerId)
    {
        // 换页通知覆盖层：定期触发重绘以驱动淡入淡出动画
        if (pageNotifyActive_)
        {
            const DWORD elapsed = GetTickCount() - pageNotifyStartTick_;
            if (elapsed >= kPageNotifyVisibleMs)
            {
                pageNotifyActive_ = false;
                pageNotifyText_.clear();
                KillTimer(hwnd_, kPageNotifyTimerId);
            }
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
        else
        {
            KillTimer(hwnd_, kPageNotifyTimerId);
        }
    }
    else if (timerId == kHiddenHintTimerId)
    {
        const DWORD elapsed = GetTickCount() - hiddenHintStartTick_;
        if (elapsed >= kHiddenHintVisibleMs)
        {
            ClearHiddenHint();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
    else if (timerId == kWidgetAddedHintTimerId)
    {
        const DWORD elapsed = GetTickCount() - widgetAddedHintStartTick_;
        if (elapsed >= kWidgetAddedHintVisibleMs)
        {
            ClearWidgetAddedHint();
            InvalidateRect(hwnd_, nullptr, FALSE);
        }
    }
}

/**
 * @brief 更新集合弹窗的停留检测逻辑
 * @param point 当前鼠标位置
 */
inline void DesktopApp::UpdateCollectionPopupDwell(POINT point)
{
    lastMousePoint_ = point;
    if (!dragSession_.IsActive() ||
        SuppressDesktopWidgetDragTargets() ||
        (dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries))
    {
        popupDwellWidgetIndex_ = static_cast<size_t>(-1);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        return;
    }

    size_t hoveredCollection = static_cast<size_t>(-1);
    if (DockContainer* dock = GetDockContainerAtPoint(point))
    {
        if (DockEntryItem* entry = dock->EntryAtPoint(point);
            entry && entry->GetEntryType() == DockEntryType::Collection)
        {
            hoveredCollection = FindWidgetIndexById(entry->GetReference());
        }
    }

    for (auto& c : containers_)
    {
        if (hoveredCollection < widgets_.size()) break;
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

    if (hoveredCollection == static_cast<size_t>(-1) ||
        hoveredCollection == popupWidgetIndex_)
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
    if (SuppressDesktopWidgetDragTargets())
        return false;
    if (popupDwellWidgetIndex_ >= widgets_.size())
        return false;
    if (popupDwellWidgetIndex_ == popupWidgetIndex_)
        return false;
    if (now - popupDwellTick_ < kCollectionPopupDwellDelayMs)
        return false;

    size_t widgetIndex = popupDwellWidgetIndex_;
    OpenCollectionPopupAt(widgetIndex, lastMousePoint_);
    UpdateWindow(hwnd_);
    return true;
}

inline void DesktopApp::UpdateCollectionGroupTabDwell(
    POINT point)
{
    auto clearDwell = [&]() {
        collectionGroupTabDwellWidgetIndex_ =
            static_cast<size_t>(-1);
        collectionGroupTabDwellId_.clear();
        collectionGroupTabDwellTick_ = 0;
        KillTimer(
            hwnd_, kCollectionGroupTabDwellTimerId);
    };

    const DragSourceList& sourceList =
        dragSession_.SourceList();
    if (!dragSession_.IsActive() ||
        !(sourceList.hasDesktopIcons ||
          sourceList.hasFolderEntries ||
          sourceList.hasExternalFiles) ||
        sourceList.hasCollectionGroupEntries ||
        sourceList.hasFileGroupEntries)
    {
        clearDwell();
        return;
    }

    size_t hoveredGroup = static_cast<size_t>(-1);
    std::wstring hoveredId;
    for (auto it = containers_.rbegin();
        it != containers_.rend(); ++it)
    {
        DesktopWidget* data = nullptr;
        std::wstring id;
        if (auto* group =
                dynamic_cast<CollectionGroup*>(it->get());
            group && sourceList.hasDesktopIcons)
        {
            id = group->CategoryIdAtPoint(point);
            if (id.empty() ||
                id == group->GetActiveCollectionId())
                continue;
            data = group->GetWidgetData();
        }
        else if (auto* fileGroup =
                     dynamic_cast<FileGroup*>(it->get()))
        {
            const std::wstring sourceId =
                fileGroup->SourceIdAtPoint(point);
            if (!sourceId.empty() &&
                sourceId != fileGroup->GetActiveSourceId())
            {
                id = L"source:" + sourceId;
            }
            else
            {
                const std::wstring categoryId =
                    fileGroup->CategoryIdAtPoint(point);
                ScrollingItemWidget* active =
                    fileGroup->GetActiveSourceContainer();
                DesktopWidget* activeData = active
                    ? active->GetWidgetData() : nullptr;
                if (categoryId.empty() ||
                    (activeData &&
                     activeData->activeCategoryId ==
                        categoryId))
                    continue;
                id = L"category:" + categoryId;
            }
            data = fileGroup->GetWidgetData();
        }
        else
        {
            continue;
        }
        if (!data) continue;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (&widgets_[i] != data) continue;
            hoveredGroup = i;
            hoveredId = std::move(id);
            break;
        }
        if (hoveredGroup < widgets_.size())
            break;
    }

    if (hoveredGroup >= widgets_.size() ||
        hoveredId.empty())
    {
        clearDwell();
        return;
    }

    if (collectionGroupTabDwellWidgetIndex_ !=
            hoveredGroup ||
        collectionGroupTabDwellId_ != hoveredId)
    {
        collectionGroupTabDwellWidgetIndex_ =
            hoveredGroup;
        collectionGroupTabDwellId_ =
            std::move(hoveredId);
        collectionGroupTabDwellTick_ =
            GetTickCount();
        SetTimer(
            hwnd_,
            kCollectionGroupTabDwellTimerId,
            kCollectionGroupTabDwellIntervalMs,
            nullptr);
    }
}

inline bool DesktopApp::TryActivateCollectionGroupTab(
    DWORD now)
{
    if (!dragSession_.IsActive() ||
        collectionGroupTabDwellWidgetIndex_ >=
            widgets_.size() ||
        collectionGroupTabDwellId_.empty() ||
        now - collectionGroupTabDwellTick_ <
            kCollectionGroupTabDwellDelayMs)
        return false;

    DesktopWidget& data =
        widgets_[collectionGroupTabDwellWidgetIndex_];
    const bool collectionGroup =
        data.type == DesktopWidgetType::CollectionGroup;
    const bool fileGroup =
        data.type == DesktopWidgetType::FileGroup;
    if (!collectionGroup && !fileGroup)
    {
        collectionGroupTabDwellWidgetIndex_ =
            static_cast<size_t>(-1);
        collectionGroupTabDwellId_.clear();
        return false;
    }

    WidgetContainer* groupedContainer = nullptr;
    for (auto& container : containers_)
    {
        auto* candidate =
            dynamic_cast<WidgetContainer*>(
                container.get());
        if (candidate &&
            candidate->GetWidgetData() == &data)
        {
            groupedContainer = candidate;
            break;
        }
    }
    if (!groupedContainer)
    {
        collectionGroupTabDwellWidgetIndex_ =
            static_cast<size_t>(-1);
        collectionGroupTabDwellId_.clear();
        return false;
    }

    bool activated = false;
    if (collectionGroup)
    {
        auto* group =
            dynamic_cast<CollectionGroup*>(
                groupedContainer);
        const std::wstring id =
            collectionGroupTabDwellId_;
        if (group &&
            group->CategoryIdAtPoint(lastMousePoint_) == id &&
            std::find(data.childWidgetIds.begin(),
                data.childWidgetIds.end(), id) !=
                data.childWidgetIds.end())
        {
            data.activeCategoryId = id;
            data.scrollOffset = 0;
            group->InvalidateFilterCache();
            activated = true;
        }
    }
    else
    {
        auto* group =
            dynamic_cast<FileGroup*>(groupedContainer);
        constexpr std::wstring_view sourcePrefix =
            L"source:";
        constexpr std::wstring_view categoryPrefix =
            L"category:";
        if (group &&
            collectionGroupTabDwellId_.starts_with(
                sourcePrefix))
        {
            const std::wstring id =
                collectionGroupTabDwellId_.substr(
                    sourcePrefix.size());
            if (group->SourceIdAtPoint(lastMousePoint_) == id &&
                std::find(data.childWidgetIds.begin(),
                    data.childWidgetIds.end(), id) !=
                    data.childWidgetIds.end())
            {
                data.activeCategoryId = id;
                data.scrollOffset = 0;
                group->InvalidateHostedView();
                activated = true;
            }
        }
        else if (group &&
                 collectionGroupTabDwellId_.starts_with(
                    categoryPrefix))
        {
            const std::wstring id =
                collectionGroupTabDwellId_.substr(
                    categoryPrefix.size());
            ScrollingItemWidget* active =
                group->GetActiveSourceContainer();
            DesktopWidget* activeData = active
                ? active->GetWidgetData() : nullptr;
            if (activeData &&
                group->CategoryIdAtPoint(lastMousePoint_) == id)
            {
                activeData->activeCategoryId = id;
                data.scrollOffset = 0;
                group->InvalidateHostedView();
                activated = true;
            }
        }
    }
    if (!activated)
    {
        collectionGroupTabDwellWidgetIndex_ =
            static_cast<size_t>(-1);
        collectionGroupTabDwellId_.clear();
        return false;
    }
    cachedDropPreview_ = {};
    cachedDropPreviewPoint_ = {-1, -1};
    dragSession_.InvalidateStaticScene();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, FALSE);

    collectionGroupTabDwellWidgetIndex_ =
        static_cast<size_t>(-1);
    collectionGroupTabDwellId_.clear();
    collectionGroupTabDwellTick_ = 0;
    int mods = 0;
    if (GetAsyncKeyState(VK_CONTROL) & 0x8000)
        mods |= MK_CONTROL;
    if (GetAsyncKeyState(VK_MENU) & 0x8000)
        mods |= MK_ALT;
    if (GetAsyncKeyState(VK_SHIFT) & 0x8000)
        mods |= MK_SHIFT;
    RefreshDragTargetAt(lastMousePoint_, mods);
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
    popupAnchoredToDock_ = false;
    popupAnchorPoint_ = anchorPoint;
    popupCategoryId_ = categoryId;
    popupPageId_ = widgets_[widgetIndex].gridCell.pageId;
    const size_t groupIndex =
            FindCollectionGroupIndexForChild(
                widgets_[widgetIndex].id);
    if (groupIndex < widgets_.size())
    {
        popupPageId_ = widgets_[groupIndex].gridCell.pageId;
    }
    if (DockContainer* dock = GetDockContainerAtPoint(anchorPoint))
    {
        RECT dockBounds = dock->GetInteractiveBounds();
        if (dock->ContainsInteractivePoint(anchorPoint))
        {
            const POINT dockCenter{
                (dockBounds.left + dockBounds.right) / 2,
                (dockBounds.top + dockBounds.bottom) / 2
            };
            const GridPage* dockPage = nullptr;
            for (const auto& page : gridPages_)
            {
                if (PtInRect(&page.bounds, dockCenter))
                {
                    dockPage = &page;
                    break;
                }
            }
            if (!dockPage) dockPage = GetFirstPageGridPage();
            if (dockPage) popupPageId_ = dockPage->id;
            if (DockEntryItem* dockItem = dock->EntryAtPoint(anchorPoint);
                dockItem && dockItem->GetEntryType() == DockEntryType::Collection &&
                dockItem->GetReference() == widgets_[widgetIndex].id)
            {
                RECT itemBounds = dock->GetElementVisualRect(
                    dockItem->GetBounds(), anchorPoint);
                popupDockPosition_ = dockSettings_.position;
                popupAnchoredToDock_ = true;
                switch (popupDockPosition_)
                {
                case DockPosition::Top:
                    popupAnchorPoint_ = {
                        (itemBounds.left + itemBounds.right) / 2, itemBounds.bottom };
                    break;
                case DockPosition::Left:
                    popupAnchorPoint_ = {
                        itemBounds.right, (itemBounds.top + itemBounds.bottom) / 2 };
                    break;
                case DockPosition::Right:
                    popupAnchorPoint_ = {
                        itemBounds.left, (itemBounds.top + itemBounds.bottom) / 2 };
                    break;
                case DockPosition::Bottom:
                default:
                    popupAnchorPoint_ = {
                        (itemBounds.left + itemBounds.right) / 2, itemBounds.top };
                    break;
                }
            }
        }
    }
    popupRect_ = GetCollectionPopupRect(widgets_[widgetIndex]);
    popupScrollOffset_ = std::clamp(popupScrollOffset_, 0,
        GetCollectionPopupMaxScrollOffset(widgets_[widgetIndex], popupRect_));
    popupDwellWidgetIndex_ = static_cast<size_t>(-1);
    if (floatingDockVisible_)
    {
        floatingDockContainer_ =
            GetDockContainerAtPoint(anchorPoint);
        if (!floatingDockContainer_)
            floatingDockContainer_ =
                SelectFloatingDockContainerForMonitor(
                    floatingDockMonitor_);
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
    }
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 关闭当前打开的集合弹窗
 */
inline void DesktopApp::CloseCollectionPopup()
{
    if (popupWidgetIndex_ == static_cast<size_t>(-1)) return;
    ClearSelection();
    popupWidgetIndex_ = static_cast<size_t>(-1);
    popupScrollOffset_ = 0;
    popupHasAnchor_ = false;
    popupAnchoredToDock_ = false;
    popupAnchorPoint_ = {};
    popupPageId_.clear();
    popupCategoryId_.clear();
    popupRect_ = {};
    if (floatingDockVisible_)
    {
        floatingDockContainer_ =
            SelectFloatingDockContainerForMonitor(
                floatingDockMonitor_);
        UpdateFloatingDockWindowBounds();
        InvalidateFloatingDockWindow(true);
    }
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
        RECT overlay = quickNavigationRect_;
        if (PtInRect(&overlay, pt))
        {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            if (quickNavigationInitialJumpOpen_)
            {
                InvalidateQuickNavigationWindow();
                return;
            }
            RECT tabs = GetQuickNavigationTabsRect(overlay);
            if (GetQuickNavigationEffectiveSearchText().empty() &&
                PtInRect(&tabs, pt))
            {
                if (pt.x >=
                    GetQuickNavigationTabsStart(
                        overlay))
                {
                    int maxTabScroll =
                        GetQuickNavigationMaxTabScrollOffset(
                            overlay);
                    quickNavigationTabScrollOffset_ =
                        std::clamp(
                            quickNavigationTabScrollOffset_ -
                                delta / 2,
                            0, maxTabScroll);
                }
            }
            else
            {
                int maxScroll = GetQuickNavigationMaxScrollOffset(overlay);
                quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_ - delta / 2, 0, maxScroll);
            }
            InvalidateQuickNavigationWindow();
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

    if (DockContainer* dock = GetDockContainerAtPoint(pt))
    {
        const int delta = GET_WHEEL_DELTA_WPARAM(wp);
        if (dock->ScrollByWheelDelta(delta))
        {
            refreshDragAfterScroll();
            InvalidateRect(hwnd_, nullptr, FALSE);
            return;
        }
    }

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

        // File category tabs use horizontal wheel scrolling.
        if (data->type == DesktopWidgetType::FileCategories ||
            data->type == DesktopWidgetType::FolderMapping ||
            data->type == DesktopWidgetType::CollectionGroup ||
            data->type == DesktopWidgetType::FileGroup)
        {
            auto* categorized = dynamic_cast<ScrollingItemWidget*>(wc);
            if (categorized && categorized->TryScrollTabs(pt, delta))
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
        if (auto* group =
                dynamic_cast<FileGroup*>(wc))
            group->InvalidateHostedView();
        else
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

    const size_t fileGroupIndex =
        FindFileGroupIndexForChild(
            widgets_[widgetIndex].id);
    if (fileGroupIndex < widgets_.size())
    {
        for (const auto& c : containers_)
        {
            auto* group =
                dynamic_cast<FileGroup*>(c.get());
            if (!group ||
                group->GetWidgetData() !=
                    &widgets_[fileGroupIndex] ||
                group->GetActiveSourceId() !=
                    widgets_[widgetIndex].id)
                continue;
            for (const auto& slot : group->GetSlots())
            {
                auto* icon = slot
                    ? dynamic_cast<FolderEntryIcon*>(
                        slot->GetItem())
                    : nullptr;
                if (!icon ||
                    icon->GetFolderEntry() !=
                        &widgets_[widgetIndex].
                            folderEntries[memberIndex])
                    continue;
                const RECT itemRect = slot->GetBounds();
                if (widgets_[fileGroupIndex].listMode)
                {
                    const int itemH = std::max<int>(
                        1, itemRect.bottom -
                            itemRect.top);
                    const int iconSize =
                        std::min(32, itemH - 4);
                    return MakeRect(
                        itemRect.left + 4 +
                            iconSize + 6,
                        itemRect.top + 5,
                        itemRect.right - 6,
                        itemRect.bottom - 5);
                }
                return GetItemTextRect(
                    itemRect, true);
            }
        }
    }

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

static int GetRenameInitialSelectionEnd(const std::wstring& name, bool isDirectory)
{
    if (isDirectory || name.empty())
        return -1;

    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size())
        return -1;

    return static_cast<int>(dot);
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
    renameCommitPending_ = false;
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
    SendMessageW(renameEdit_, EM_SETSEL, 0,
        GetRenameInitialSelectionEnd(
            widgets_[widgetIndex].folderEntries[memberIndex].name,
            widgets_[widgetIndex].folderEntries[memberIndex].isDirectory));
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

inline bool DesktopApp::IsShellDeleteCommand(
    IContextMenu* contextMenu,
    UINT commandOffset) const
{
    if (!contextMenu) return false;

    wchar_t verbW[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(
            commandOffset, GCS_VERBW, nullptr,
            reinterpret_cast<LPSTR>(verbW),
            static_cast<UINT>(_countof(verbW)))) &&
        lstrcmpiW(verbW, L"delete") == 0)
    {
        return true;
    }

    char verbA[128]{};
    if (SUCCEEDED(contextMenu->GetCommandString(
            commandOffset, GCS_VERBA, nullptr,
            verbA,
            static_cast<UINT>(_countof(verbA)))) &&
        lstrcmpiA(verbA, "delete") == 0)
    {
        return true;
    }
    return false;
}

/**
 * @brief 显示文件夹条目的 Shell 上下文菜单
 * @param screenPoint 屏幕坐标点
 * @param widgetIndex 部件索引
 * @param memberIndex 条目索引
 */
inline void DesktopApp::ShowFolderEntryContextMenu(
    POINT screenPoint, size_t widgetIndex,
    size_t memberIndex,
    bool keepQuickNavigationOpen)
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

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu,
        snowdesktop::item_location::CanReveal(fullPath)
            ? MF_STRING
            : MF_STRING | MF_GRAYED,
        kContextRevealLocationCommand,
        _LW("app.menu.open_file_location"));

    contextMenu.As(&activeContextMenu2_);
    contextMenu.As(&activeContextMenu3_);

    HWND menuOwner = keepQuickNavigationOpen &&
        quickNavigationHwnd_ &&
        IsWindow(quickNavigationHwnd_)
        ? quickNavigationHwnd_
        : hwnd_;
    SetForegroundWindow(menuOwner);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, menuOwner, nullptr);
    if (!keepQuickNavigationOpen)
        FocusDesktopInputWindow();

    activeContextMenu2_.Reset();
    activeContextMenu3_.Reset();

    if (command == kContextRevealLocationCommand)
    {
        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);
        snowdesktop::item_location::Reveal(hwnd_, fullPath);
        return;
    }

    if (command >= kFirstCmd && command <= kLastCmd)
    {
        UINT commandOffset = command - kFirstCmd;
        wchar_t menuText[128]{};
        bool renameCommand = IsShellRenameCommand(contextMenu.Get(), commandOffset);
        if (!renameCommand &&
            GetMenuStringW(menu, command, menuText, static_cast<int>(_countof(menuText)), MF_BYCOMMAND) > 0)
        {
            renameCommand = StrStrIW(menuText, L"重命名") != nullptr || // l10n-allow: match Chinese Windows shell verb
                StrStrIW(menuText, L"Rename") != nullptr;
        }

        DestroyMenu(menu);
        RestoreDesktopWindowLayer();
        ILFree(pidl);

        if (renameCommand)
        {
            if (keepQuickNavigationOpen)
                BeginQuickNavigationFolderEntryRename(
                    widgetIndex, memberIndex);
            else
                BeginRenameFolderEntry(
                    widgetIndex, memberIndex);
            return;
        }

        CMINVOKECOMMANDINFOEX invoke{};
        invoke.cbSize = sizeof(invoke);
        invoke.fMask = CMIC_MASK_UNICODE | CMIC_MASK_PTINVOKE;
        invoke.hwnd = ShellDialogOwnerHwnd();
        invoke.lpVerb = MAKEINTRESOURCEA(commandOffset);
        invoke.lpVerbW = MAKEINTRESOURCEW(commandOffset);
        invoke.nShow = SW_SHOWNORMAL;
        invoke.ptInvoke = screenPoint;
        SafeInvokeCommand(contextMenu.Get(), reinterpret_cast<LPCMINVOKECOMMANDINFO>(&invoke));
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
 * @brief 如果同名文件/文件夹已存在，自动添加递增序号生成唯一名称
 *
 * 例如 "test.txt" 已存在时返回 "test (2).txt"，依此类推。
 * @param folderPath  父文件夹路径
 * @param desiredName  期望的文件/文件夹名
 * @return 在 folderPath 中不存在的唯一名称
 */
static std::wstring MakeUniqueFileName(const std::wstring& folderPath, const std::wstring& desiredName)
{
    wchar_t fullPath[MAX_PATH]{};
    PathCombineW(fullPath, folderPath.c_str(), desiredName.c_str());
    if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
        return desiredName;

    DWORD attrs = GetFileAttributesW(fullPath);
    bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);

    std::wstring stem = desiredName;
    std::wstring ext;
    if (!isDir)
    {
        wchar_t stemBuf[MAX_PATH]{};
        wcscpy_s(stemBuf, stem.c_str());
        PathRemoveExtensionW(stemBuf);
        stem = stemBuf;
        const wchar_t* extPtr = PathFindExtensionW(desiredName.c_str());
        ext = extPtr ? extPtr : L"";
    }

    for (int i = 2; i < 1000; ++i)
    {
        std::wstring candidate = stem + L" (" + std::to_wstring(i) + L")" + ext;
        PathCombineW(fullPath, folderPath.c_str(), candidate.c_str());
        if (GetFileAttributesW(fullPath) == INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    return stem + L" (1000)" + ext;
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
        wchar_t dirBuf[MAX_PATH]{};
        wcscpy_s(dirBuf, oldPath.c_str());
        PathRemoveFileSpecW(dirBuf);
        std::wstring uniqueName = MakeUniqueFileName(dirBuf, newName);
        PITEMID_CHILD newChild = nullptr;
        hr = parentFolder->SetNameOf(ShellDialogOwnerHwnd(), child, uniqueName.c_str(), SHGDN_NORMAL, &newChild);
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
inline bool DesktopApp::BeginDockAnchoredRename(
    const std::wstring& text, RECT anchorClient,
    int selectionEnd)
{
    RECT anchorScreen = anchorClient;
    MapWindowPoints(hwnd_, nullptr,
        reinterpret_cast<POINT*>(&anchorScreen), 2);
    const POINT anchorCenter{
        (anchorScreen.left + anchorScreen.right) / 2,
        (anchorScreen.top + anchorScreen.bottom) / 2
    };
    const HMONITOR monitor = MonitorFromPoint(
        anchorCenter, MONITOR_DEFAULTTONEAREST);
    MONITORINFO monitorInfo{ sizeof(monitorInfo) };
    if (!GetMonitorInfoW(monitor, &monitorInfo))
    {
        monitorInfo.rcWork = {
            virtualLeft_, virtualTop_,
            virtualLeft_ + virtualWidth_,
            virtualTop_ + virtualHeight_
        };
    }

    UINT dpiX = 96;
    UINT dpiY = 96;
    if (FAILED(GetDpiForMonitor(
            monitor, MDT_EFFECTIVE_DPI,
            &dpiX, &dpiY)))
        dpiX = 96;
    const int desiredWidth =
        std::max(150, MulDiv(180, static_cast<int>(dpiX), 96));
    const int desiredHeight =
        std::max(26, MulDiv(30, static_cast<int>(dpiX), 96));
    const int gap =
        std::max(3, MulDiv(6, static_cast<int>(dpiX), 96));
    const int monitorMargin =
        std::max(3, MulDiv(5, static_cast<int>(dpiX), 96));
    const RECT screenRect =
        snowdesktop::dock_rename_layout::
            CalculateAdjacentEditRect(
                anchorScreen, monitorInfo.rcWork,
                dockSettings_.position,
                desiredWidth, desiredHeight,
                gap, monitorMargin);

    renameEdit_ = CreateWindowExW(
        WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        L"EDIT", text.c_str(),
        WS_POPUP | WS_VISIBLE |
            ES_CENTER | ES_AUTOHSCROLL,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left,
        screenRect.bottom - screenRect.top,
        hwnd_, nullptr, instance_, nullptr);
    if (!renameEdit_)
        return false;

    if (renameFont_)
        DeleteObject(renameFont_);
    const int fontHeight = std::max(
        12, MulDiv(
            static_cast<int>(std::round(itemFontSize_)),
            static_cast<int>(dpiX), 96));
    renameFont_ = CreateFontW(
        -fontHeight, 0, 0, 0, FW_NORMAL,
        FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    SendMessageW(renameEdit_, WM_SETFONT,
        reinterpret_cast<WPARAM>(
            renameFont_ ? renameFont_
                : GetStockObject(DEFAULT_GUI_FONT)),
        TRUE);
    const int editMargin = std::max(
        3, MulDiv(5, static_cast<int>(dpiX), 96));
    SendMessageW(renameEdit_, EM_SETMARGINS,
        EC_LEFTMARGIN | EC_RIGHTMARGIN,
        MAKELPARAM(editMargin, editMargin));
    SetWindowSubclass(renameEdit_,
        &DesktopApp::RenameEditSubclassProc, 1,
        reinterpret_cast<DWORD_PTR>(this));
    SetWindowPos(renameEdit_, HWND_TOPMOST,
        screenRect.left, screenRect.top,
        screenRect.right - screenRect.left,
        screenRect.bottom - screenRect.top,
        SWP_SHOWWINDOW);
    SendMessageW(renameEdit_, EM_SETSEL,
        0, selectionEnd);
    SetFocus(renameEdit_);
    return true;
}

inline void DesktopApp::BeginRenameSelected(
    std::optional<RECT> dockRenameAnchor)
{
    if (renameEdit_ != nullptr) return;
    renameCommitPending_ = false;

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
        if (dockRenameAnchor)
        {
            if (!BeginDockAnchoredRename(
                    widgets_[selectedWidgetIndex].title,
                    *dockRenameAnchor, -1))
            {
                renamingWidget_ = false;
                renameIndex_ = static_cast<size_t>(-1);
            }
            return;
        }

        RECT frame = widgets_[selectedWidgetIndex].bounds;
        RECT handle = frame;
        bool foundContainer = false;
        bool groupedTabRename = false;
        if (IsGroupedCollection(
                widgets_[selectedWidgetIndex]))
        {
            const size_t groupIndex =
                FindCollectionGroupIndexForChild(
                    widgets_[selectedWidgetIndex].id);
            if (groupIndex < widgets_.size())
            {
                for (const auto& c : containers_)
                {
                    auto* group =
                        dynamic_cast<CollectionGroup*>(
                            c.get());
                    if (!group ||
                        group->GetWidgetData() !=
                            &widgets_[groupIndex])
                        continue;
                    frame = group->GetTabRectById(
                        widgets_[selectedWidgetIndex].id);
                    if (!IsRectEmptyRect(frame))
                    {
                        handle = frame;
                        foundContainer = true;
                        groupedTabRename = true;
                    }
                    break;
                }
            }
        }
        else
        {
            const size_t groupIndex =
                FindFileGroupIndexForChild(
                    widgets_[selectedWidgetIndex].id);
            if (groupIndex < widgets_.size())
            {
                for (const auto& c : containers_)
                {
                    auto* group =
                        dynamic_cast<FileGroup*>(
                            c.get());
                    if (!group ||
                        group->GetWidgetData() !=
                            &widgets_[groupIndex])
                        continue;
                    frame = group->GetSourceTabRectById(
                        widgets_[selectedWidgetIndex].id);
                    if (!IsRectEmptyRect(frame))
                    {
                        handle = frame;
                        foundContainer = true;
                        groupedTabRename = true;
                    }
                    break;
                }
            }
        }
        if (!foundContainer)
        {
            for (const auto& c : containers_)
            {
                auto* wc =
                    dynamic_cast<WidgetContainer*>(c.get());
                if (wc && wc->GetWidgetData() ==
                        &widgets_[selectedWidgetIndex])
                {
                    frame = wc->GetFrameRect();
                    handle = wc->GetMoveHandleRect();
                    foundContainer = true;
                    break;
                }
            }
        }
        if (!foundContainer && widgets_[selectedWidgetIndex].type == DesktopWidgetType::LuaScript)
        {
            frame = GetStandaloneWidgetFrameRect(widgets_[selectedWidgetIndex]);
            handle = GetStandaloneWidgetMoveHandleRect(widgets_[selectedWidgetIndex]);
        }
        const int editHeight = groupedTabRename
            ? std::max(
                24, static_cast<int>(
                    handle.bottom - handle.top))
            : std::max(
                40, static_cast<int>(
                    handle.bottom - handle.top) * 2);
        RECT rect = groupedTabRename
            ? MakeRect(
                frame.left + 2, frame.top,
                frame.right - 2, frame.top + editHeight)
            : MakeRect(
                frame.left + 4, handle.top,
                frame.right - 4,
                handle.top + editHeight);
        InflateRect(&rect, 2, 2);
        RECT screenRect = rect;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(&screenRect), 2);

        const DWORD editStyle = groupedTabRename
            ? (WS_POPUP | WS_VISIBLE |
                ES_CENTER | ES_AUTOHSCROLL)
            : (WS_POPUP | WS_VISIBLE |
                ES_MULTILINE | ES_CENTER |
                ES_AUTOVSCROLL | ES_WANTRETURN);
        renameEdit_ = CreateWindowExW(
            WS_EX_CLIENTEDGE | WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
            L"EDIT",
            widgets_[selectedWidgetIndex].title.c_str(),
            editStyle,
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
    DWORD fileAttributes = GetFileAttributesW(path);
    bool isDirectory = fileAttributes != INVALID_FILE_ATTRIBUTES &&
        (fileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;

    renameIndex_ = static_cast<size_t>(selectedIndex);
    if (dockRenameAnchor)
    {
        if (!BeginDockAnchoredRename(
                items_[selectedIndex].name,
                *dockRenameAnchor,
                GetRenameInitialSelectionEnd(
                    items_[selectedIndex].name,
                    isDirectory)))
        {
            renameIndex_ = static_cast<size_t>(-1);
        }
        return;
    }
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
    SendMessageW(renameEdit_, EM_SETSEL, 0,
        GetRenameInitialSelectionEnd(items_[selectedIndex].name, isDirectory));
    SetFocus(renameEdit_);
}

/**
 * @brief 提交或取消重命名编辑
 * @param cancel 是否取消重命名
 */
inline void DesktopApp::CommitRename(bool cancel)
{
    renameCommitPending_ = false;
    if (renameEdit_ == nullptr)
    {
        renamingQuickNavigationItem_ = false;
        return;
    }

    const bool quickNavigationRename =
        renamingQuickNavigationItem_;
    renamingQuickNavigationItem_ = false;

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
        if (quickNavigationRename)
            InvalidateQuickNavigationWindow();
        return;
    }

    if (renamingWidget_)
    {
        if (!cancel && renameIndex_ < widgets_.size())
        {
            if (!newName.empty())
            {
                widgets_[renameIndex_].title = newName;
                widgets_[renameIndex_].customTitle = newName;
                widgets_[renameIndex_].userRenamed = true;
                SaveLayoutSlots();
            }
            else if (!widgets_[renameIndex_].customTitle.empty())
            {
                DesktopWidget& widget = widgets_[renameIndex_];
                widget.customTitle.clear();
                widgets_[renameIndex_].userRenamed = false;
                if (!widget.scriptTitle.empty())
                    widget.title = widget.scriptTitle;
                else if (widget.type == DesktopWidgetType::LuaScript)
                    widget.title = WidgetEngine::GetWidgetDisplayName(widget.scriptPath);
                else if (widget.type == DesktopWidgetType::FileCategories)
                    widget.title = _LW("widget.desktop_files");
                else if (widget.type == DesktopWidgetType::Guide)
                    widget.title = _LW("app.guide.title");
                else if (widget.type == DesktopWidgetType::Collection)
                    widget.title = _LW("widget.collection");
                else if (widget.type ==
                    DesktopWidgetType::CollectionGroup)
                    widget.title =
                        _LW("widget.collection_group");
                SaveLayoutSlots();
            }
        }
        renamingWidget_ = false;
        renameIndex_ = static_cast<size_t>(-1);
        InvalidateRect(hwnd_, nullptr, TRUE);
        if (quickNavigationRename)
            InvalidateQuickNavigationWindow();
        return;
    }

    bool keepLayoutSlots = false;
    bool dockUsageKeyMigrated = false;
    if (!cancel && renameIndex_ < items_.size() && !newName.empty() && newName != items_[renameIndex_].name)
    {
        std::wstring oldLayoutKey = items_[renameIndex_].layoutKey;
        wchar_t desktopPath[MAX_PATH]{};
        SHGetSpecialFolderPathW(nullptr, desktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
        std::wstring uniqueName = MakeUniqueFileName(desktopPath, newName);
        PITEMID_CHILD newChild = nullptr;
        HRESULT hr = desktopFolder_->SetNameOf(ShellDialogOwnerHwnd(),
            reinterpret_cast<PCUITEMID_CHILD>(items_[renameIndex_].childPidl.get()),
            uniqueName.c_str(), SHGDN_NORMAL, &newChild);
        if (SUCCEEDED(hr))
        {
            if (newChild)
            {
                PIDLIST_ABSOLUTE newAbsolute = ILCombine(desktopPidl_.get(), newChild);
                std::wstring newParsingName = StrRetToString(desktopFolder_.Get(), newChild, SHGDN_FORPARSING);
                if (newAbsolute)
                {
                    const std::wstring oldNormalizedKey =
                        ToUpperInvariant(oldLayoutKey);
                    const std::wstring newLayoutKey =
                        ToUpperInvariant(GetStableLayoutKey(
                            newAbsolute, newParsingName));
                    LayoutRecord record;
                    record.cell = items_[renameIndex_].gridCell;
                    record.span = items_[renameIndex_].gridSpan;
                    record.hasGrid = true;
                    record.legacySlot = items_[renameIndex_].slot;
                    if (oldNormalizedKey != newLayoutKey)
                        layoutRecords_.erase(oldNormalizedKey);
                    layoutRecords_[newLayoutKey] = record;

                    snowdesktop::
                        desktop_item_reference_migration::
                            MigrateReferences(
                                widgets_, dockEntries_,
                                oldLayoutKey, newLayoutKey);

                    if (oldNormalizedKey != newLayoutKey)
                    {
                        auto oldUsage =
                            dockUsageStats_.find(
                                oldNormalizedKey);
                        if (oldUsage !=
                            dockUsageStats_.end())
                        {
                            const DockUsageRecord migrated =
                                oldUsage->second;
                            dockUsageStats_.erase(oldUsage);
                            DockUsageRecord& destination =
                                dockUsageStats_[newLayoutKey];
                            destination.launchCount =
                                std::max(
                                    destination.launchCount,
                                    migrated.launchCount);
                            destination.lastUsed =
                                std::max(
                                    destination.lastUsed,
                                    migrated.lastUsed);
                            dockUsageKeyMigrated = true;
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
    if (dockUsageKeyMigrated)
        SaveDockUsageStats();
    if (quickNavigationRename)
        InvalidateQuickNavigationWindow();
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
    case WM_ACTIVATE:
        if (app->renamingQuickNavigationItem_ &&
            LOWORD(wParam) == WA_INACTIVE)
        {
            const HWND activatedWindow =
                reinterpret_cast<HWND>(lParam);
            const bool remainsInQuickNavigation =
                activatedWindow ==
                    app->quickNavigationHwnd_ ||
                activatedWindow ==
                    app->quickNavigationSearchEdit_ ||
                app->quickNavBackdropCompositor_.
                    IsBackdropWindow(
                        activatedWindow);
            if (!remainsInQuickNavigation)
            {
                app->CommitRename(false);
                app->CloseQuickNavigation();
                return 0;
            }
        }
        break;
    case WM_KEYDOWN:
        if (wParam == VK_RETURN) { app->CommitRename(false); return 0; }
        if (wParam == VK_ESCAPE) { app->CommitRename(true); return 0; }
        break;
    case WM_KILLFOCUS:
        if (!app->renameCommitPending_)
        {
            app->renameCommitPending_ = true;
            if (!PostMessageW(app->hwnd_, kCommitRenameMessage, FALSE, 0))
            {
                app->renameCommitPending_ = false;
                app->CommitRename(false);
            }
        }
        return 0;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
}

/** @brief 将 Lua 内联编辑框当前内容实时写回小部件存储。 */
inline void DesktopApp::PreviewLuaInlineTextEdit()
{
    if (!luaInlineEditLiveUpdate_ || !luaInlineEdit_ || !widgetEngine_ ||
        luaInlineEditWidgetId_.empty() || luaInlineEditStorageKey_.empty())
        return;

    int length = GetWindowTextLengthW(luaInlineEdit_);
    std::vector<wchar_t> buffer(static_cast<size_t>(std::max(0, length)) + 1);
    GetWindowTextW(luaInlineEdit_, buffer.data(), length + 1);
    widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
        LuaWidgetWideToUtf8(std::wstring(buffer.data())));
    InvalidateRect(hwnd_, nullptr, FALSE);
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
    if (luaInlineEditBackgroundBrush_)
    {
        DeleteObject(luaInlineEditBackgroundBrush_);
        luaInlineEditBackgroundBrush_ = nullptr;
    }

    if (widgetEngine_ && !luaInlineEditWidgetId_.empty() && !luaInlineEditStorageKey_.empty())
    {
        if (cancel && luaInlineEditLiveUpdate_)
            widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
                LuaWidgetWideToUtf8(luaInlineEditOriginalText_));
        else if (!cancel)
            widgetEngine_->RuntimeSetStorageValue(luaInlineEditWidgetId_, luaInlineEditStorageKey_,
                LuaWidgetWideToUtf8(value));
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    luaInlineEditWidgetId_.clear();
    luaInlineEditStorageKey_.clear();
    luaInlineEditOriginalText_.clear();
    luaInlineEditMultiline_ = false;
    luaInlineEditLiveUpdate_ = false;
    luaInlineEditTextColor_ = RGB(0, 0, 0);
    luaInlineEditBackgroundColor_ = RGB(255, 255, 255);
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
        if (wParam == VK_DELETE && app->luaInlineEditLiveUpdate_)
        {
            LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
            app->PreviewLuaInlineTextEdit();
            return result;
        }
        break;
    case WM_CHAR:
    case WM_PASTE:
    case WM_CUT:
    case WM_CLEAR:
    case WM_UNDO:
    {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        app->PreviewLuaInlineTextEdit();
        return result;
    }
    case WM_IME_COMPOSITION:
    {
        LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
        if ((lParam & GCS_RESULTSTR) != 0)
            app->PreviewLuaInlineTextEdit();
        return result;
    }
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
    if (inputHwnd_ && (IsSameWindowTree(inputHwnd_, window) || root == inputHwnd_)) return true;

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
        UpdateCollectionPopupDwell(client);
        UpdateCollectionGroupTabDwell(client);
        const bool suppressDesktopWidgetTargets = SuppressDesktopWidgetDragTargets();
        const bool groupedEntryDrag =
            dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries;
        if (!suppressDesktopWidgetTargets && !UpdateDragPageNavigation(client))
        {
            *effect = DROPEFFECT_NONE;
            OnPaint();
            InvalidateFloatingDockWindow(true);
            return S_OK;
        }

        // OO hit-test：优先检查集合弹窗（弹窗遮挡的容器不应被穿透命中）
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        const bool popupHit =
            !suppressDesktopWidgetTargets &&
            !groupedEntryDrag &&
            HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion);
        if (!popupHit)
        {
            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                if (!AcceptsSlotSurfaceDrop(
                        it->get(),
                        dragSession_.SourceList()))
                    continue;
                if (suppressDesktopWidgetTargets &&
                    (dynamic_cast<DesktopGrid*>(it->get()) ||
                     dynamic_cast<WidgetContainer*>(it->get())))
                    continue;
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
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (const std::wstring removalHint = GetDockDragOutRemovalHint(client);
            !removalHint.empty())
            hint = removalHint;
        else if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = targetRegion == HitRegion::Blocked
            ? DROPEFFECT_NONE : DROPEFFECT_COPY | DROPEFFECT_MOVE;
        OnPaint();
        InvalidateFloatingDockWindow(true);
        return S_OK;
    }

    externalDragActive_ = true;
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
    {
        ClearDockBackdropForDragTransition(
            lastMousePoint_, client);
        dragSession_.Begin(nullptr, {}, {}, client, client);
    }
    else
        dragSession_.UpdatePoint(client);
    if (!UpdateDragPageNavigation(client))
    {
        *effect = DROPEFFECT_NONE;
        OnPaint();
        InvalidateFloatingDockWindow(true);
        return S_OK;
    }

    // OO hit-test for external drop：优先检查集合弹窗
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    if (!HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion))
    {
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            if (!AcceptsExternalSlotSurfaceDrop(
                    it->get()))
                continue;
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
    const bool externalDockMapping =
        dynamic_cast<DockContainer*>(targetContainer) &&
        targetRegion != HitRegion::Handoff &&
        targetRegion != HitRegion::Blocked;
    if (externalDockMapping)
        dragSession_.UpdateActionFromMods(
            DropActionToMods(
                snowdesktop::dock_drop_rules::
                    ExternalMappingAction()),
            snowdesktop::dock_drop_rules::
                ExternalMappingAction());
    else
        dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = targetRegion == HitRegion::Blocked
        ? DROPEFFECT_NONE
        : (externalDockMapping
            ? snowdesktop::dock_drop_rules::
                ChooseExternalMappingEffect(*effect)
            : ChooseDropEffect(keyState, *effect));
    OnPaint();
    InvalidateFloatingDockWindow(true);
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
        UpdateCollectionPopupDwell(client);
        UpdateCollectionGroupTabDwell(client);
        const bool suppressDesktopWidgetTargets = SuppressDesktopWidgetDragTargets();
        const bool groupedEntryDrag =
            dragSession_.SourceList().
                hasCollectionGroupEntries ||
            dragSession_.SourceList().
                hasFileGroupEntries;
        if (!suppressDesktopWidgetTargets && !UpdateDragPageNavigation(client))
        {
            *effect = DROPEFFECT_NONE;
            OnPaint();
            InvalidateFloatingDockWindow(true);
            return S_OK;
        }

        // OO hit-test：优先检查集合弹窗（弹窗遮挡的容器不应被穿透命中）
        Container* targetContainer = nullptr;
        Slot* targetSlot = nullptr;
        HitRegion targetRegion = HitRegion::None;
        const bool popupHit =
            !suppressDesktopWidgetTargets &&
            !groupedEntryDrag &&
            HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion);
        if (!popupHit)
        {
            for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
            {
                if (!AcceptsSlotSurfaceDrop(
                        it->get(),
                        dragSession_.SourceList()))
                    continue;
                if (suppressDesktopWidgetTargets &&
                    (dynamic_cast<DesktopGrid*>(it->get()) ||
                     dynamic_cast<WidgetContainer*>(it->get())))
                    continue;
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
        }
        dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

        int mods = 0;
        if (keyState & MK_CONTROL) mods |= MK_CONTROL;
        if (keyState & MK_ALT)     mods |= MK_ALT;
        if (keyState & MK_SHIFT)   mods |= MK_SHIFT;

        std::wstring hint;
        if (const std::wstring removalHint = GetDockDragOutRemovalHint(client);
            !removalHint.empty())
            hint = removalHint;
        else if (targetContainer && targetRegion != HitRegion::None)
            hint = targetContainer->GetDragHint(targetSlot, targetRegion,
                dragSession_.Items(), dragSession_.Source(), mods);
        ShowDragHintWindowScreen({ point.x, point.y }, hint);
        *effect = targetRegion == HitRegion::Blocked
            ? DROPEFFECT_NONE : DROPEFFECT_COPY | DROPEFFECT_MOVE;
        OnPaint();
        InvalidateFloatingDockWindow(true);
        return S_OK;
    }

    externalDragActive_ = true;
    POINT client = ScreenPointToClient(point);
    if (!dragSession_.IsActive() || !dragSession_.Items().empty())
    {
        ClearDockBackdropForDragTransition(
            lastMousePoint_, client);
        dragSession_.Begin(nullptr, {}, {}, client, client);
    }
    else
        dragSession_.UpdatePoint(client);
    if (!UpdateDragPageNavigation(client))
    {
        *effect = DROPEFFECT_NONE;
        OnPaint();
        InvalidateFloatingDockWindow(true);
        return S_OK;
    }

    // OO hit-test for external drop：优先检查集合弹窗
    Container* targetContainer = nullptr;
    Slot* targetSlot = nullptr;
    HitRegion targetRegion = HitRegion::None;
    if (!HitTestPopupForDrag(client, targetContainer, targetSlot, targetRegion))
    {
        for (auto it = containers_.rbegin(); it != containers_.rend(); ++it)
        {
            if (!AcceptsExternalSlotSurfaceDrop(
                    it->get()))
                continue;
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
    }
    dragSession_.UpdateTarget(targetContainer, targetSlot, targetRegion);

    int mods = 0;
    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
    if (keyState & MK_ALT)     mods |= MK_ALT;
    if (keyState & MK_SHIFT)   mods |= MK_SHIFT;
    const bool externalDockMapping =
        dynamic_cast<DockContainer*>(targetContainer) &&
        targetRegion != HitRegion::Handoff &&
        targetRegion != HitRegion::Blocked;
    if (externalDockMapping)
        dragSession_.UpdateActionFromMods(
            DropActionToMods(
                snowdesktop::dock_drop_rules::
                    ExternalMappingAction()),
            snowdesktop::dock_drop_rules::
                ExternalMappingAction());
    else
        dragSession_.UpdateActionFromMods(mods, DropAction::Copy);

    std::wstring hint;
    if (targetContainer && targetRegion != HitRegion::None)
        hint = targetContainer->GetDragHint(targetSlot, targetRegion, {}, nullptr, mods);
    ShowDragHintWindowScreen({ point.x, point.y }, hint);
    *effect = targetRegion == HitRegion::Blocked
        ? DROPEFFECT_NONE
        : (externalDockMapping
            ? snowdesktop::dock_drop_rules::
                ChooseExternalMappingEffect(*effect)
            : ChooseDropEffect(keyState, *effect));
    OnPaint();
    InvalidateFloatingDockWindow(true);
    return S_OK;
}

/**
 * @brief COM IDropTarget::DragLeave 实现
 * @return S_OK
 */
inline HRESULT STDMETHODCALLTYPE DesktopApp::DragLeave()
{
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;
    if (selfDragActive_)
    {
        popupDwellWidgetIndex_ = static_cast<size_t>(-1);
        KillTimer(hwnd_, kCollectionPopupDwellTimerId);
        collectionGroupTabDwellWidgetIndex_ =
            static_cast<size_t>(-1);
        collectionGroupTabDwellId_.clear();
        collectionGroupTabDwellTick_ = 0;
        KillTimer(
            hwnd_, kCollectionGroupTabDwellTimerId);
        dragSession_.UpdateTarget(nullptr, nullptr, HitRegion::None);
        HideDragHintWindow();
        OnPaint();
        InvalidateFloatingDockWindow(true);
        return S_OK;
    }
    externalDragActive_ = false;
    externalDropFileCount_ = 0;
    externalDropHasShortcut_ = false;
    EndDragSession();
    HideDragHintWindow();
    OnPaint();
    InvalidateFloatingDockWindow(true);
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
    navHoverSide_ = 0;
    navAutoFlipDir_ = 0;
    navAutoFlipTick_ = 0;

    if (dragSession_.TargetRegion() == HitRegion::Blocked)
    {
        externalDragActive_ = false;
        externalDropFileCount_ = 0;
        externalDropHasShortcut_ = false;
        *effect = DROPEFFECT_NONE;
        EndDragSession();
        return S_OK;
    }

    POINT clientPoint = ScreenPointToClient(point);

    if (selfDragActive_)
    {
        selfDragActive_ = false;
        selfDragReturned_ = true;
        mouseDown_ = false;
        mouseDownHit_ = nullptr;
        ReleaseCapture();
        dragSession_.DeactivateForDrop();
        dragRenderCache_.Reset();
        InvalidateRect(hwnd_, nullptr, FALSE);

        if (!GetDockDragOutRemovalHint(clientPoint).empty())
        {
            const bool removed = RemoveDockDragOutItems(dragSession_.Items());
            ClearSelection();
            EndDragSession();
            if (removed)
            {
                SaveLayoutSlots();
                RebuildContainersAndItems();
                LayoutItems();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
            *effect = DROPEFFECT_MOVE;
            return S_OK;
        }

        if (dragSession_.TargetRegion() == HitRegion::Handoff)
        {
            // ── Shell handoff via IShellFolder::IDropTarget ────
            Item* targetItem = dragSession_.TargetSlot() ? dragSession_.TargetSlot()->GetItem() : nullptr;
            if (auto* dockTarget = dynamic_cast<DockEntryItem*>(targetItem))
            {
                if (dockTarget->GetEntryType() == DockEntryType::Collection)
                {
                    int mods = 0;
                    if (keyState & MK_CONTROL) mods |= MK_CONTROL;
                    if (keyState & MK_ALT) mods |= MK_ALT;
                    if (keyState & MK_SHIFT) mods |= MK_SHIFT;
                    const bool executed = DropItemsIntoDockCollection(
                        dragSession_.Items(), dragSession_.Source(), dockTarget, mods);
                    SaveLayoutSlots();
                    ClearSelection();
                    EndDragSession();
                    if (executed)
                    {
                        RebuildContainersAndItems();
                        LayoutItems();
                        InvalidateRect(hwnd_, nullptr, FALSE);
                    }
                    *effect = executed ? DROPEFFECT_MOVE : DROPEFFECT_NONE;
                    return S_OK;
                }
            }
            auto* targetDesktopIcon = dynamic_cast<DesktopIcon*>(targetItem);
            DesktopItem* targetDesktopItem = targetDesktopIcon
                ? targetDesktopIcon->GetDesktopItem() : nullptr;
            if (dynamic_cast<DockContainer*>(dragSession_.Source()) && targetDesktopItem &&
                _wcsicmp(targetDesktopItem->desktopIconClsid.c_str(),
                    kDesktopIconClsidRecycleBin) == 0)
            {
                MoveDockItemsToDesktop(dragSession_.Items(), CellFromPointForDrag(clientPoint));
                SaveLayoutSlots();
                ClearSelection();
                EndDragSession();
                *effect = DROPEFFECT_MOVE;
                return S_OK;
            }
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
            if (needsReload)
            {
                RebuildContainersAndItems();
                ReloadItems();
            }
            else
            {
                ApplyPageMapping();
                RebuildContainersAndItems();
                LayoutItems();
                InvalidateRect(hwnd_, nullptr, FALSE);
            }
        }
        *effect = DROPEFFECT_MOVE;
        EndDragSession();
        return S_OK;
    }

    // ── External drop ──────────────────────────────────────────
    externalDragActive_ = false;
    externalDropFileCount_ = 0;
    externalDropHasShortcut_ = false;
    dragSession_.DeactivateForDrop();
    dragRenderCache_.Reset();
    InvalidateRect(hwnd_, nullptr, FALSE);

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

        if (auto* dock = dynamic_cast<DockContainer*>(target);
            dock && targetRegion != HitRegion::Handoff)
        {
            if (!dock->HasCapacity(sourceItems.size()))
            {
                MessageBeep(MB_ICONWARNING);
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }

            const DWORD mappingEffect =
                snowdesktop::dock_drop_rules::
                    ChooseExternalMappingEffect(*effect);
            if (mappingEffect == DROPEFFECT_NONE)
            {
                *effect = DROPEFFECT_NONE;
                EndDragSession();
                return S_OK;
            }

            const size_t insertIndex = dock->GetDropInsertIndex(
                dragSession_.TargetSlot(), targetRegion);
            const auto existingKeys = SnapshotDesktopKeys();
            DropPreviewList desktopPreview = BuildDropPreviewList(sourceList, GetDesktopGrid(),
                nullptr, HitRegion::Empty, mods, clientPoint);
            desktopPreview.action =
                snowdesktop::dock_drop_rules::
                    ExternalMappingAction();
            bool executed = ExecuteDropPipeline(sourceList, desktopPreview);
            if (executed)
            {
                AddExternalItemsToDock(NewDesktopKeysSince(existingKeys), insertIndex);
                SaveLayoutSlots();
                EndDragSession();
                InvalidateRect(hwnd_, nullptr, FALSE);
                *effect = mappingEffect;
                return S_OK;
            }
        }

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
    if (hostName.empty()) hostName = _LW("app.interact.link");

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
    const std::vector<std::wstring> shortcutSuffixes =
        Locale::Instance().TranslationValues(
            L10N_KEY("app.interact.shortcut_suffix"));
    const std::vector<std::wstring> copySuffixes =
        Locale::Instance().TranslationValues(
            L10N_KEY("app.interact.copy_suffix"));

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
    auto stripLocalizedSuffix = [](const std::wstring& text,
        const std::vector<std::wstring>& suffixes) -> std::wstring {
        for (const std::wstring& suffix : suffixes)
        {
            if (!suffix.empty() && text.size() > suffix.size() &&
                _wcsicmp(text.c_str() + text.size() - suffix.size(),
                    suffix.c_str()) == 0)
            {
                return text.substr(0, text.size() - suffix.size());
            }
        }
        return text;
    };
    auto stripShortcut = [&](const std::wstring& s) -> std::wstring {
        std::wstring stripped = stripLocalizedSuffix(s, shortcutSuffixes);
        if (stripped != s)
            return stripped;
        return s;
    };
    auto stripCopy = [&](const std::wstring& s) -> std::wstring {
        std::wstring value = s;
        size_t paren = value.rfind(L" (");
        if (paren != std::wstring::npos && value.ends_with(L")"))
            value = value.substr(0, paren);
        std::wstring stripped = stripLocalizedSuffix(value, copySuffixes);
        return stripped != value ? stripped : s;
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
inline void DesktopApp::ShowCollectionGroupTabContextMenu(
    POINT screenPoint, size_t groupIndex,
    const std::wstring& collectionId)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::CollectionGroup)
        return;
    const size_t childIndex =
        FindWidgetIndexById(collectionId);
    if (childIndex >= widgets_.size() ||
        widgets_[childIndex].type !=
            DesktopWidgetType::Collection ||
        std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            collectionId) ==
            widgets_[groupIndex].childWidgetIds.end())
        return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(
        menu, MF_STRING,
        kContextWidgetRename,
        _LW("app.menu.rename"));
    SetMenuItemIcon(
        menu, kContextWidgetRename, L"");
    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y,
        hwnd_, nullptr);
    FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();

    if (command == kContextWidgetRename)
    {
        ClearSelection();
        widgets_[childIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        auto it = std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            collectionId);
        keyboardNavMemberIndex_ =
            it == widgets_[groupIndex].childWidgetIds.end()
                ? 0
                : static_cast<int>(
                    std::distance(
                        widgets_[groupIndex].childWidgetIds.begin(),
                        it));
        keyboardNavCollectionGroupTabs_ = true;
        BeginRenameSelected();
    }
}

inline void DesktopApp::ShowFileGroupSourceTabContextMenu(
    POINT screenPoint, size_t groupIndex,
    const std::wstring& childId)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::FileGroup)
        return;
    const size_t childIndex =
        FindWidgetIndexById(childId);
    if (childIndex >= widgets_.size() ||
        (widgets_[childIndex].type !=
             DesktopWidgetType::FileCategories &&
         widgets_[childIndex].type !=
             DesktopWidgetType::FolderMapping) ||
        std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            childId) ==
            widgets_[groupIndex].childWidgetIds.end())
        return;

    HMENU menu = CreatePopupMenu();
    if (!menu) return;
    AppendMenuW(menu, MF_STRING,
        kContextWidgetRename,
        _LW("app.menu.rename"));
    SetMenuItemIcon(
        menu, kContextWidgetRename, L"");
    SetForegroundWindow(hwnd_);
    const UINT command = TrackPopupMenuEx(
        menu,
        TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y,
        hwnd_, nullptr);
    FocusDesktopInputWindow();
    DestroyMenu(menu);
    ClearMenuIcons();

    if (command == kContextWidgetRename)
    {
        ClearSelection();
        widgets_[childIndex].selected = true;
        keyboardNavInsideWidget_ = true;
        keyboardNavWidgetIndex_ = groupIndex;
        const auto it = std::find(
            widgets_[groupIndex].childWidgetIds.begin(),
            widgets_[groupIndex].childWidgetIds.end(),
            childId);
        keyboardNavMemberIndex_ =
            it == widgets_[groupIndex].childWidgetIds.end()
                ? 0
                : static_cast<int>(
                    std::distance(
                        widgets_[groupIndex].
                            childWidgetIds.begin(), it));
        keyboardNavCollectionGroupTabs_ = true;
        BeginRenameSelected();
    }
}

inline void DesktopApp::ShowWidgetContextMenu(
    POINT screenPoint, size_t widgetIndex,
    std::optional<RECT> dockRenameAnchor)
{
    if (widgetIndex >= widgets_.size()) return;
    ClearMenuIcons();

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    const auto& widget = widgets_[widgetIndex];
    size_t effectiveSourceIndex = widgetIndex;
    if (widget.type == DesktopWidgetType::FileGroup)
    {
        effectiveSourceIndex =
            FindWidgetIndexById(widget.activeCategoryId);
        if (effectiveSourceIndex >= widgets_.size())
            effectiveSourceIndex = widgetIndex;
    }
    const DesktopWidget& effectiveSource =
        widgets_[effectiveSourceIndex];
    std::vector<LuaWidgetMenuItem> luaMenuItems;
    HMENU displayModeMenu = nullptr;
    HMENU hoverMenu = nullptr;
    HMENU privacyMenu = nullptr;

    if (widget.type == DesktopWidgetType::Collection)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpen, _LW("app.interact.open_all"));
        displayModeMenu = CreatePopupMenu();
        if (displayModeMenu)
        {
            AppendMenuW(displayModeMenu, MF_STRING | (!widget.scrollContainerMode ? MF_CHECKED : 0),
                kContextWidgetCollModeLargeFolder, _LW("app.interact.large_folder"));
            AppendMenuW(displayModeMenu, MF_STRING | (widget.scrollContainerMode ? MF_CHECKED : 0),
                kContextWidgetCollModeScrollContainer, _LW("app.interact.popup_container"));
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(displayModeMenu), _LW("app.interact.display_mode"));
        }
        if (widget.scrollContainerMode)
        {
            AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode,
                widget.listMode ? _LW("app.interact.icon_display") : _LW("app.interact.list_display"));
        }
    }
    else if (widget.type == DesktopWidgetType::CollectionGroup)
    {
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            widget.listMode
                ? _LW("app.interact.icon_display")
                : _LW("app.interact.list_display"));
        AppendMenuW(menu, MF_STRING | (widget.showSearchBox ? MF_CHECKED : 0),
            kContextWidgetToggleSearchBox,
            widget.showSearchBox
                ? _LW("app.interact.hide_search_box")
                : _LW("app.interact.show_search_box"));
    }
    else if (widget.type == DesktopWidgetType::FileGroup)
    {
        if (effectiveSource.type ==
            DesktopWidgetType::FileCategories)
        {
            AppendMenuW(menu, MF_STRING,
                kContextWidgetManualCollect,
                _LW("app.interact.collect_now"));
            AppendMenuW(menu,
                MF_STRING |
                    (effectiveSource.autoCollect
                        ? MF_CHECKED : 0),
                kContextWidgetToggleAutoCollect,
                _LW("app.interact.auto_collect"));
        }
        else if (effectiveSource.type ==
                 DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_STRING,
                kContextWidgetOpenFolder,
                _LW("app.interact.open_folder"));
        }
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleListMode,
            widget.listMode
                ? _LW("app.interact.icon_display")
                : _LW("app.interact.list_display"));
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleFileCategories,
            widget.showFileCategories
                ? _LW("app.interact.hide_file_categories")
                : _LW("app.interact.show_file_categories"));
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleSearchBox,
            widget.showSearchBox
                ? _LW("app.interact.hide_search_box")
                : _LW("app.interact.show_search_box"));
        AppendMenuW(menu, MF_STRING,
            kContextWidgetToggleDateGroup,
            widget.dateHeaders
                ? _LW("app.interact.hide_date_header")
                : _LW("app.interact.show_date_header"));
        if (effectiveSource.type ==
            DesktopWidgetType::FolderMapping)
        {
            AppendMenuW(menu, MF_STRING,
                kContextNewMenu, _LW("app.menu.new"));
            AppendMenuW(menu, MF_STRING,
                kContextMoreCommand,
                _LW("app.menu.more_options"));
        }
    }
    else if (widget.type == DesktopWidgetType::FileCategories)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetManualCollect, _LW("app.interact.collect_now"));
        AppendMenuW(menu, MF_STRING | (widget.autoCollect ? MF_CHECKED : 0), kContextWidgetToggleAutoCollect, _LW("app.interact.auto_collect"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode, widget.listMode ? _LW("app.interact.icon_display") : _LW("app.interact.list_display"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleDateGroup,
            widget.dateHeaders ? _LW("app.interact.hide_date_header") : _LW("app.interact.show_date_header"));
    }
    else if (widget.type == DesktopWidgetType::FolderMapping)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetOpenFolder, _LW("app.interact.open_folder"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleListMode, widget.listMode ? _LW("app.interact.icon_display") : _LW("app.interact.list_display"));
        AppendMenuW(menu, MF_STRING | (widget.showFileCategories ? MF_CHECKED : 0),
            kContextWidgetToggleFileCategories,
            widget.showFileCategories
                ? _LW("app.interact.hide_file_categories")
                : _LW("app.interact.show_file_categories"));
        AppendMenuW(menu, MF_STRING | (widget.showSearchBox ? MF_CHECKED : 0),
            kContextWidgetToggleSearchBox,
            widget.showSearchBox
                ? _LW("app.interact.hide_search_box")
                : _LW("app.interact.show_search_box"));
        AppendMenuW(menu, MF_STRING, kContextWidgetToggleDateGroup,
            widget.dateHeaders
                ? _LW("app.interact.hide_date_header")
                : _LW("app.interact.show_date_header"));
        AppendMenuW(menu, MF_STRING, kContextNewMenu, _LW("app.menu.new"));
        AppendMenuW(menu, MF_STRING, kContextMoreCommand, _LW("app.menu.more_options"));
    }
    else if (widget.type == DesktopWidgetType::LuaScript)
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetEdit, _LW("app.interact.detailed_settings"));
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
    if ((widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::Collection ||
        widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup) &&
        (widget.type == DesktopWidgetType::CollectionGroup ||
            widget.type == DesktopWidgetType::FileGroup ||
            !widget.dateHeaders))
    {
        sortMenu = CreatePopupMenu();
        if (sortMenu)
        {
            wNameMenu = CreatePopupMenu();
            if (wNameMenu)
            {
                AppendMenuW(wNameMenu, MF_STRING, kContextWidgetSortByName, _LW("app.menu.sort_asc"));
                AppendMenuW(wNameMenu, MF_STRING, kContextWidgetSortByNameDesc, _LW("app.menu.sort_desc"));
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wNameMenu), _LW("app.menu.sort_name"));
            }
            wTypeMenu = CreatePopupMenu();
            if (wTypeMenu)
            {
                AppendMenuW(wTypeMenu, MF_STRING, kContextWidgetSortByType, _LW("app.menu.sort_asc"));
                AppendMenuW(wTypeMenu, MF_STRING, kContextWidgetSortByTypeDesc, _LW("app.menu.sort_desc"));
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wTypeMenu), _LW("app.menu.sort_type"));
            }
            wDateMenu = CreatePopupMenu();
            if (wDateMenu)
            {
                AppendMenuW(wDateMenu, MF_STRING, kContextWidgetSortByDate, _LW("app.menu.sort_asc"));
                AppendMenuW(wDateMenu, MF_STRING, kContextWidgetSortByDateDesc, _LW("app.menu.sort_desc"));
                AppendMenuW(sortMenu, MF_POPUP, reinterpret_cast<UINT_PTR>(wDateMenu), _LW("app.interact.sort_date"));
            }
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(sortMenu), _LW("app.menu.sort_by"));
        }
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    if (CanRenameWidget(widget))
    {
        AppendMenuW(menu, MF_STRING, kContextWidgetRename, _LW("app.menu.rename"));
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    hoverMenu = CreatePopupMenu();
    if (hoverMenu)
    {
        AppendMenuW(hoverMenu, MF_STRING | (widget.showOnHoverOnly ? MF_CHECKED : 0),
            kContextWidgetShowOnHoverOn, _LW("app.interact.on"));
        AppendMenuW(hoverMenu, MF_STRING | (!widget.showOnHoverOnly ? MF_CHECKED : 0),
            kContextWidgetShowOnHoverOff, _LW("app.interact.off"));
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(hoverMenu), _LW("app.interact.hover_only"));
    }
    if (widget.type == DesktopWidgetType::Collection ||
        widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping ||
        widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup)
    {
        privacyMenu = CreatePopupMenu();
        if (privacyMenu)
        {
            AppendMenuW(privacyMenu, MF_STRING | (widget.privacyMode ? MF_CHECKED : 0),
                kContextWidgetPrivacyModeOn, _LW("app.interact.on"));
            AppendMenuW(privacyMenu, MF_STRING | (!widget.privacyMode ? MF_CHECKED : 0),
                kContextWidgetPrivacyModeOff, _LW("app.interact.off"));
            AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(privacyMenu), _LW("app.interact.privacy_mode"));
        }
    }
    AppendMenuW(menu, MF_STRING, kContextWidgetDelete, _LW("app.interact.delete_widget"));

    SetMenuItemIcon(menu, kContextWidgetOpen, L"");
    SetMenuItemIcon(menu, kContextWidgetManualCollect, L"");
    SetMenuItemIcon(menu, kContextWidgetToggleListMode, widget.listMode ? L"" : L"");
    SetMenuItemIcon(menu, kContextWidgetToggleDateGroup, L"");
    SetMenuItemIcon(menu, kContextWidgetOpenFolder, L"");
    SetMenuItemIcon(menu, kContextWidgetToggleFileCategories, L"");
    SetMenuItemIcon(menu, kContextWidgetToggleSearchBox, L"");
    SetMenuItemIcon(menu, kContextNewMenu, L"");
    SetMenuItemIcon(menu, kContextMoreCommand, L"");
    SetMenuItemIcon(menu, kContextWidgetEdit, L"");
    SetMenuItemIcon(menu, kContextWidgetRename, L"");
    SetMenuItemIcon(menu, kContextWidgetDelete, L"");
    if (hoverMenu)
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(hoverMenu), L"");
    if (privacyMenu)
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(privacyMenu), widget.privacyMode ? L"" : L"");
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
    if (displayModeMenu)
    {
        SetMenuItemIcon(menu, reinterpret_cast<UINT_PTR>(displayModeMenu), L"");
        SetMenuItemIcon(displayModeMenu, kContextWidgetCollModeLargeFolder, L"");
        SetMenuItemIcon(displayModeMenu, kContextWidgetCollModeScrollContainer, L"");
    }

    SetForegroundWindow(hwnd_);
    UINT command = TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON,
        screenPoint.x, screenPoint.y, hwnd_, nullptr);
    FocusDesktopInputWindow();
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
        if (effectiveSource.type ==
                DesktopWidgetType::FolderMapping &&
            !effectiveSource.sourceFolderPath.empty())
            ShellExecuteW(hwnd_, L"open",
                effectiveSource.sourceFolderPath.c_str(),
                nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kContextWidgetToggleListMode:
        widgets_[widgetIndex].listMode = !widgets_[widgetIndex].listMode;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[widgetIndex])
            {
                if (auto* group =
                        dynamic_cast<FileGroup*>(wc))
                    group->InvalidateHostedView();
                else
                    wc->InvalidateSlots();
                break;
            }
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleAutoCollect:
        if (effectiveSourceIndex >= widgets_.size() ||
            widgets_[effectiveSourceIndex].type !=
                DesktopWidgetType::FileCategories)
            break;
        widgets_[effectiveSourceIndex].autoCollect =
            !widgets_[effectiveSourceIndex].autoCollect;
        if (widgets_[effectiveSourceIndex].autoCollect)
        {
            EnforceSingleAutoCollectFileCategory(
                effectiveSourceIndex);
            CollectFileCategoryWidget(
                effectiveSourceIndex, false);
        }
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleDateGroup:
        widgets_[widgetIndex].dateHeaders = !widgets_[widgetIndex].dateHeaders;
        widgets_[widgetIndex].scrollOffset = 0;
        if (widgets_[widgetIndex].type ==
            DesktopWidgetType::FileGroup)
        {
            for (auto& c : containers_)
            {
                auto* group =
                    dynamic_cast<FileGroup*>(c.get());
                if (group &&
                    group->GetWidgetData() ==
                        &widgets_[widgetIndex])
                {
                    group->InvalidateHostedView();
                    break;
                }
            }
        }
        else if (widgets_[widgetIndex].type ==
                 DesktopWidgetType::FolderMapping)
        {
            for (auto& c : containers_)
            {
                auto* mapping = dynamic_cast<FolderMapping*>(c.get());
                if (mapping &&
                    mapping->GetWidgetData() == &widgets_[widgetIndex])
                {
                    mapping->InvalidateFilterCache();
                    break;
                }
            }
        }
        else
        {
            RebuildContainersAndItems();
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetToggleFileCategories:
        if (widgets_[widgetIndex].type ==
                DesktopWidgetType::FolderMapping ||
            widgets_[widgetIndex].type ==
                DesktopWidgetType::FileGroup)
        {
            widgets_[widgetIndex].showFileCategories =
                !widgets_[widgetIndex].showFileCategories;
            widgets_[widgetIndex].scrollOffset = 0;
            widgets_[widgetIndex].tabScrollOffset = 0;
            for (auto& c : containers_)
            {
                auto* scrolling =
                    dynamic_cast<ScrollingItemWidget*>(c.get());
                if (scrolling &&
                    scrolling->GetWidgetData() ==
                        &widgets_[widgetIndex])
                {
                    if (auto* mapping =
                            dynamic_cast<FolderMapping*>(
                                scrolling))
                        mapping->InvalidateFilterCache();
                    else if (auto* fileGroup =
                                 dynamic_cast<FileGroup*>(
                                     scrolling))
                        fileGroup->InvalidateHostedView();
                    break;
                }
            }
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextWidgetToggleSearchBox:
        if (widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping ||
            widgets_[widgetIndex].type == DesktopWidgetType::CollectionGroup ||
            widgets_[widgetIndex].type == DesktopWidgetType::FileGroup)
        {
            widgets_[widgetIndex].showSearchBox =
                !widgets_[widgetIndex].showSearchBox;
            widgets_[widgetIndex].scrollOffset = 0;
            for (auto& c : containers_)
            {
                auto* scrolling = dynamic_cast<ScrollingItemWidget*>(c.get());
                if (scrolling &&
                    scrolling->GetWidgetData() == &widgets_[widgetIndex])
                {
                    if (!widgets_[widgetIndex].showSearchBox)
                        scrolling->ClearSearchText();
                    if (auto* mapping = dynamic_cast<FolderMapping*>(scrolling))
                        mapping->InvalidateFilterCache();
                    else if (auto* group = dynamic_cast<CollectionGroup*>(scrolling))
                        group->InvalidateFilterCache();
                    else if (auto* fileGroup =
                                 dynamic_cast<FileGroup*>(
                                     scrolling))
                        fileGroup->InvalidateHostedView();
                    break;
                }
            }
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextWidgetManualCollect:
        if (!CollectFileCategoryWidget(
                effectiveSourceIndex, true))
            MessageBeep(MB_ICONINFORMATION);
        break;
    case kContextWidgetRename:
        if (CanRenameWidget(widgets_[widgetIndex]))
        {
            SelectWidgetOnly(widgetIndex);
            BeginRenameSelected(dockRenameAnchor);
        }
        break;
    case kContextWidgetEdit:
        ShowWidgetEditorHost(widgetIndex);
        break;
    case kContextWidgetDelete:
    {
        const std::wstring deletedWidgetId = widgets_[widgetIndex].id;
        if (widgets_[widgetIndex].type == DesktopWidgetType::CollectionGroup)
        {
            if (popupWidgetIndex_ < widgets_.size() &&
                std::find(
                    widgets_[widgetIndex].childWidgetIds.begin(),
                    widgets_[widgetIndex].childWidgetIds.end(),
                    widgets_[popupWidgetIndex_].id) !=
                    widgets_[widgetIndex].childWidgetIds.end())
                CloseCollectionPopup();
            ReleaseCollectionGroupChildren(widgetIndex);
        }
        else if (widgets_[widgetIndex].type ==
                 DesktopWidgetType::FileGroup)
        {
            ReleaseFileGroupChildren(widgetIndex);
        }
        if (widgets_[widgetIndex].type == DesktopWidgetType::LuaScript && widgetEngine_)
            widgetEngine_->UnloadWidget(widgets_[widgetIndex].id);
        for (auto& group : widgets_)
        {
            if (group.id == deletedWidgetId ||
                (group.type !=
                     DesktopWidgetType::CollectionGroup &&
                 group.type !=
                     DesktopWidgetType::FileGroup))
                continue;
            std::erase(
                group.childWidgetIds,
                deletedWidgetId);
            group.activeCategoryId =
                snowdesktop::collection_group_rules::
                    ResolveActiveItem(
                        group.childWidgetIds,
                        group.activeCategoryId);
        }
        // Move widget's itemKeys back to desktop by just removing the widget
        widgets_.erase(widgets_.begin() + static_cast<std::ptrdiff_t>(widgetIndex));
        std::erase_if(dockEntries_, [&](const DockEntry& entry) {
            return entry.type == DockEntryType::Collection &&
                entry.reference == deletedWidgetId;
        });
        EnsureNavTabOrder();
        // 删除组件可能使页面变空（溢出区空页后面有非空页时应立即清理顺延）
        ApplyPageMapping();
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    }
    case kContextNewMenu:
        if (effectiveSourceIndex < widgets_.size() &&
            widgets_[effectiveSourceIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[effectiveSourceIndex].
                sourceFolderPath.empty())
        {
            ShowNewMenuAndInvoke(screenPoint,
                widgets_[effectiveSourceIndex].
                    sourceFolderPath);
            RefreshFolderMappingWidget(
                effectiveSourceIndex);
            RebuildContainersAndItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        break;
    case kContextMoreCommand:
        if (effectiveSourceIndex < widgets_.size() &&
            widgets_[effectiveSourceIndex].type ==
                DesktopWidgetType::FolderMapping &&
            !widgets_[effectiveSourceIndex].
                sourceFolderPath.empty())
        {
            ShowShellContextMenuForPath(
                widgets_[effectiveSourceIndex].
                    sourceFolderPath, screenPoint);
        }
        break;
    case kContextWidgetSortByName:
        SortWidgetContents(effectiveSourceIndex, 0, true);
        break;
    case kContextWidgetSortByNameDesc:
        SortWidgetContents(effectiveSourceIndex, 0, false);
        break;
    case kContextWidgetSortByType:
        SortWidgetContents(effectiveSourceIndex, 1, true);
        break;
    case kContextWidgetSortByTypeDesc:
        SortWidgetContents(effectiveSourceIndex, 1, false);
        break;
    case kContextWidgetSortByDate:
        SortWidgetContents(effectiveSourceIndex, 2, true);
        break;
    case kContextWidgetSortByDateDesc:
        SortWidgetContents(effectiveSourceIndex, 2, false);
        break;
    case kContextWidgetShowOnHoverOn:
        widgets_[widgetIndex].showOnHoverOnly = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetShowOnHoverOff:
        widgets_[widgetIndex].showOnHoverOnly = false;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetPrivacyModeOn:
        widgets_[widgetIndex].privacyMode = true;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetPrivacyModeOff:
        widgets_[widgetIndex].privacyMode = false;
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetCollModeLargeFolder:
        widgets_[widgetIndex].scrollContainerMode = false;
        widgets_[widgetIndex].scrollOffset = 0;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[widgetIndex])
            { wc->InvalidateSlots(); break; }
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        break;
    case kContextWidgetCollModeScrollContainer:
        widgets_[widgetIndex].scrollContainerMode = true;
        for (auto& c : containers_)
        {
            auto* wc = dynamic_cast<WidgetContainer*>(c.get());
            if (wc && wc->GetWidgetData() == &widgets_[widgetIndex])
            { wc->InvalidateSlots(); break; }
        }
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
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
            { kTrayDesktopIconThisPC, kDesktopIconClsidThisPC, _LW("app.interact.computer") },
            { kTrayDesktopIconUserFiles, kDesktopIconClsidUserFiles, _LW("app.interact.user_files") },
            { kTrayDesktopIconNetwork, kDesktopIconClsidNetwork, _LW("app.interact.network") },
            { kTrayDesktopIconControlPanel, kDesktopIconClsidControlPanel, _LW("app.interact.control_panel") },
            { kTrayDesktopIconRecycleBin, kDesktopIconClsidRecycleBin, _LW("app.interact.recycle_bin") },
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
        AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(iconMenu), _LW("app.interact.desktop_icon_settings"));
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    {
        bool nativeActive = !customDesktopVisible_;
        AppendMenuW(menu, MF_STRING | (nativeActive ? MF_CHECKED : 0),
            kTraySwitchNativeCommand, _LW("app.interact.switch_native_desktop"));
        AppendMenuW(menu, MF_STRING | (nativeActive ? 0 : MF_CHECKED),
            kTraySwitchCustomCommand, _LW("app.interact.switch_software_desktop"));
    }

    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kTraySettingsCommand, _LW("app.menu.settings"));
    AppendMenuW(menu, MF_STRING, kTrayRestartExplorerCommand, _LW("app.interact.restart_explorer_menu"));
    AppendMenuW(menu, MF_STRING, kTrayRestartCommand, _LW("app.interact.restart_app"));
    AppendMenuW(menu, MF_STRING, kTrayExitCommand, _LW("app.interact.exit_app"));

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
        SetSoftwareDesktopEnabled(false, true);
        break;
    case kTraySwitchCustomCommand:
        SetSoftwareDesktopEnabled(true, true);
        break;
    case kTraySettingsCommand:
        ShowSettingsWindow();
        break;
    case kTrayRestartCommand:
        RequestRestart();
        break;
    case kTrayRestartExplorerCommand:
        if (!RestartWindowsExplorer())
            MessageBoxW(controlHwnd_ ? controlHwnd_ : hwnd_,
                _LW("app.interact.restart_explorer_fail"),
                L"SnowDesktop", MB_OK | MB_ICONWARNING);
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
