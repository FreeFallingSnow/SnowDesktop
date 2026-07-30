/**
 * @file app_grid.h
 * @brief DesktopApp 的网格辅助、布局持久化、拖拽、控件窗口、位图缓存及数据加载等内联实现。
 *
 * 该文件在 app_oo.h 中类定义之后被包含，提供所有网格布局计算、布局文件的读写、
 * Shell 变更通知注册、拖拽操作、控件窗口消息处理、图标位图缓存及桌面项加载等功能。
 */
#pragma once

#include <ShObjIdl.h>

#include "drop_model.h"
#include "../widgets/collection_group_rules.h"

// ── 网格辅助函数 ──────────────────────────────────────────

/**
 * @brief 判断指定的网格区域是否在页面范围内。
 * @param page  目标网格页面。
 * @param cell  起始单元格。
 * @param span  跨度（列数 x 行数）。
 * @return 如果区域完全在页面边界内返回 true，否则返回 false。
 */
inline bool GridAreaFitsPage(const GridPage& page, const GridCell& cell, GridSpan span)
{
    if (span.columns < 1 || span.rows < 1) return false;
    if (cell.pageId != page.id) return false;
    if (cell.column < 0 || cell.row < 0) return false;
    return cell.column + span.columns <= page.columns &&
        cell.row + span.rows <= page.rows;
}

/**
 * @brief 根据屏幕坐标查找所在的网格页面。
 * @param point 客户区坐标。
 * @return 指向对应 GridPage 的指针，未找到时返回第一个页面或 nullptr。
 */
inline const GridPage* DesktopApp::GridPageFromPoint(POINT point) const
{
    const GridPage* fallback = GetFirstPageGridPage();
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, point) || PtInRect(&page.workArea, point))
            return &page;
    }
    return fallback;
}

/**
 * @brief 根据屏幕坐标通过 MonitorFromPoint 查找所在的网格页面。
 * @param screenPoint 屏幕坐标（非客户区坐标）。
 * @return 指向对应 GridPage 的指针，未找到时返回第一个页面或 nullptr。
 */
inline const GridPage* DesktopApp::GridPageFromScreenPoint(POINT screenPoint) const
{
    HMONITOR hMonitor = MonitorFromPoint(screenPoint, MONITOR_DEFAULTTONEAREST);
    if (!hMonitor)
        return GetFirstPageGridPage();
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(hMonitor, &monitorInfo))
        return GetFirstPageGridPage();
    std::wstring monitorId = monitorInfo.szDevice[0] != L'\0'
        ? monitorInfo.szDevice
        : L"";
    for (const auto& page : gridPages_)
    {
        if (page.monitorId == monitorId)
            return &page;
    }
    return GetFirstPageGridPage();
}

/**
 * @brief 在右键菜单所在页面调整行数（增/减）。
 * @param delta 行数变化量（正数增加，负数减少）。
 */
inline void DesktopApp::AdjustGridRows(int delta)
{
    if (gridPages_.empty()) return;
    POINT clientPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* found = GridPageFromPoint(clientPoint);
    if (!found) return;
    GridPage* targetPage = nullptr;
    for (auto& page : gridPages_)
        if (page.id == found->id) { targetPage = &page; break; }
    if (!targetPage) return;

    constexpr int kMinRows = 1;
    constexpr int kMaxRows = 50;
    const int newRows = std::clamp(targetPage->rows + delta, kMinRows, kMaxRows);
    if (newRows == targetPage->rows) return;

    targetPage->rows = newRows;
    ApplyIconSpacingToPage(*targetPage);
    savedPageColumns_[targetPage->id] = targetPage->columns;
    savedPageRows_[targetPage->id] = targetPage->rows;
    ApplyDockWorkAreaReservation();
    RelayoutDisplacedItems();
    SaveLayoutSlots();
    LayoutItems();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 在右键菜单所在页面调整列数（增/减）。
 * @param delta 列数变化量（正数增加，负数减少）。
 */
inline void DesktopApp::AdjustGridColumns(int delta)
{
    if (gridPages_.empty()) return;
    POINT clientPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* found = GridPageFromPoint(clientPoint);
    if (!found) return;
    GridPage* targetPage = nullptr;
    for (auto& page : gridPages_)
        if (page.id == found->id) { targetPage = &page; break; }
    if (!targetPage) return;

    constexpr int kMinColumns = 1;
    constexpr int kMaxColumns = 50;
    const int newColumns = std::clamp(targetPage->columns + delta, kMinColumns, kMaxColumns);
    if (newColumns == targetPage->columns) return;

    targetPage->columns = newColumns;
    ApplyIconSpacingToPage(*targetPage);
    savedPageColumns_[targetPage->id] = targetPage->columns;
    savedPageRows_[targetPage->id] = targetPage->rows;
    ApplyDockWorkAreaReservation();
    RelayoutDisplacedItems();
    SaveLayoutSlots();
    LayoutItems();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 将右键所在页面一次性设置为指定列数和行数。
 * @param columns 目标列数。
 * @param rows 目标行数。
 */
inline void DesktopApp::SetGridDimensions(int columns, int rows)
{
    if (gridPages_.empty()) return;

    POINT clientPoint = lastContextMenuScreenPoint_;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* found = GridPageFromPoint(clientPoint);
    if (!found) return;

    GridPage* targetPage = nullptr;
    for (auto& page : gridPages_)
    {
        if (page.id == found->id)
        {
            targetPage = &page;
            break;
        }
    }
    if (!targetPage) return;

    constexpr int kMinGridSize = 1;
    constexpr int kMaxGridSize = 50;
    const int newColumns = std::clamp(columns, kMinGridSize, kMaxGridSize);
    const int newRows = std::clamp(rows, kMinGridSize, kMaxGridSize);
    if (targetPage->columns == newColumns && targetPage->rows == newRows)
        return;

    targetPage->columns = newColumns;
    targetPage->rows = newRows;
    ApplyIconSpacingToPage(*targetPage);
    savedPageColumns_[targetPage->id] = targetPage->columns;
    savedPageRows_[targetPage->id] = targetPage->rows;
    ApplyDockWorkAreaReservation();
    RelayoutDisplacedItems();
    SaveLayoutSlots();
    LayoutItems();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 根据显示器物理尺寸估算舒适的桌面网格行列数。
 *
 * 以 27 英寸为舒适密度基准，并对物理尺寸采用平方根弱缩放。
 * 因此小屏只适度减少行列，不会因对角线较小而把图标放得过大；
 * 27 英寸 16:9 显示器仍得到约 27 列 × 11 行。
 */
inline GridSpan DesktopApp::CalculateRecommendedGridDimensions(
    int aspectWidth, int aspectHeight, float diagonalInches) const
{
    if (aspectWidth <= 0 || aspectHeight <= 0 || diagonalInches <= 0.0f)
        return {1, 1};

    constexpr float kReferenceDiagonalInches = 27.0f;
    constexpr float kComfortableCellWidthInches = 0.87f;
    constexpr float kComfortableCellHeightInches = 1.20f;
    const float effectiveDiagonal = kReferenceDiagonalInches * std::sqrt(
        diagonalInches / kReferenceDiagonalInches);
    const float aspectDiagonal = std::sqrt(
        static_cast<float>(aspectWidth * aspectWidth + aspectHeight * aspectHeight));
    const float physicalWidth =
        effectiveDiagonal * static_cast<float>(aspectWidth) / aspectDiagonal;
    const float physicalHeight =
        effectiveDiagonal * static_cast<float>(aspectHeight) / aspectDiagonal;

    GridSpan result;
    result.columns = std::clamp(
        static_cast<int>(std::round(physicalWidth / kComfortableCellWidthInches)),
        4, 50);
    result.rows = std::clamp(
        static_cast<int>(std::round(physicalHeight / kComfortableCellHeightInches)),
        3, 50);
    return result;
}

/**
 * @brief 从坐标点所在显示器切换首屏锁定（持久化，与末屏锁互斥平移）。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::ToggleFirstPagePin(POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* page = GridPageFromPoint(clientPoint);
    if (!page || page->monitorId.empty()) return;

    if (firstPageMonitorId_ == page->monitorId)
    {
        firstPageMonitorId_.clear();          // toggle off → 取消锁定
    }
    else
    {
        firstPageMonitorId_ = page->monitorId;
        if (lastPageMonitorId_ == page->monitorId)  // 互斥平移
            lastPageMonitorId_.clear();
    }
    pageOffset_ = 0;
    UpdateLayoutWorkArea();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    RestoreDesktopWindowLayer();
}

/**
 * @brief 从坐标点所在显示器切换末屏锁定（持久化，与首屏锁互斥平移）。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::ToggleLastPagePin(POINT screenPoint)
{
    POINT clientPoint = screenPoint;
    ScreenToClient(hwnd_, &clientPoint);
    const GridPage* page = GridPageFromPoint(clientPoint);
    if (!page || page->monitorId.empty()) return;

    if (lastPageMonitorId_ == page->monitorId)
    {
        lastPageMonitorId_.clear();           // toggle off → 取消锁定
    }
    else
    {
        lastPageMonitorId_ = page->monitorId;
        if (firstPageMonitorId_ == page->monitorId)  // 互斥平移
            firstPageMonitorId_.clear();
    }
    pageOffset_ = 0;
    UpdateLayoutWorkArea();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    RestoreDesktopWindowLayer();
}

/**
 * @brief 设置图标间距比例（0.5 ~ 2.0），并重新布局。
 * @param value 新的间距倍率。
 */
inline void DesktopApp::SetIconSpacing(float value)
{
    float clamped = std::clamp(value, 0.5f, 2.0f);
    if (clamped == iconSpacingScale_) return;
    iconSpacingScale_ = clamped;
    for (auto& page : gridPages_)
        ApplyIconSpacingToPage(page);
    ApplyDockWorkAreaReservation();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 以增量方式调整图标间距比例。
 * @param delta 间距变化量。
 */
inline void DesktopApp::AdjustIconSpacing(float delta)
{
    float newVal = std::clamp(iconSpacingScale_ + delta, 0.5f, 2.0f);
    if (newVal == iconSpacingScale_) return;
    iconSpacingScale_ = newVal;
    for (auto& page : gridPages_)
        ApplyIconSpacingToPage(page);
    ApplyDockWorkAreaReservation();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 设置图标标题字号，重新创建文本格式并刷新。
 * @param value 新的字号。
 */
inline void DesktopApp::SetItemFontSize(float value)
{
    if (value == itemFontSize_) return;
    itemFontSize_ = value;
    RecreateItemTextFormat();

    // Dock icon geometry is derived from the grid icon size, which in turn
    // reserves space for the current two-line title height. Rebuild both the
    // work-area reservation and cached Dock slots before repainting; otherwise
    // the Dock keeps drawing its previous-size slots until another interaction.
    ApplyDockWorkAreaReservation();
    LayoutItems();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    SaveLayoutSlots();
    if (floatingDockVisible_)
        UpdateFloatingDockWindowBounds();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateDockRects(TRUE);
}

inline DWRITE_FONT_WEIGHT DesktopApp::GetItemFontWeight() const
{
    return itemFontWeight_;
}

inline void DesktopApp::SetItemFontWeight(DWRITE_FONT_WEIGHT weight)
{
    if (weight == itemFontWeight_) return;
    itemFontWeight_ = weight;
    RecreateItemTextFormat();
    InvalidateDragStaticScene();
    SaveLayoutSlots();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateDockRects(TRUE);
}

inline void DesktopApp::SetShortcutArrowMode(int mode)
{
    mode = std::clamp(mode, 0, 2);
    if (mode == shortcutArrowMode_) return;
    shortcutArrowMode_ = mode;
    InvalidateDragStaticScene();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (quickNavigationOpen_)
        InvalidateQuickNavigationWindow();
}

inline bool DesktopApp::ShouldDrawShortcutArrow(bool isShortcut, bool isApplicationShortcut) const
{
    if (!isShortcut)
        return false;

    switch (shortcutArrowMode_)
    {
    case 1:
        return false;
    case 2:
        return true;
    default:
        return !isApplicationShortcut;
    }
}

inline void DesktopApp::SetIconBeautifyEnabled(bool enabled)
{
    SetIconBeautifySettings(enabled,
        iconBeautifyMode_,
        iconBeautifyBgOpacity_,
        iconBeautifyGradientEnabled_,
        iconBeautifyBgStartR_,
        iconBeautifyBgStartG_,
        iconBeautifyBgStartB_,
        iconBeautifyBgEndR_,
        iconBeautifyBgEndG_,
        iconBeautifyBgEndB_,
        iconBeautifyGradientDirection_);
}

inline void DesktopApp::SetIconBeautifySettings(bool enabled,
    int beautifyMode,
    float backgroundOpacity,
    bool gradientEnabled,
    float backgroundStartR,
    float backgroundStartG,
    float backgroundStartB,
    float backgroundEndR,
    float backgroundEndG,
    float backgroundEndB,
    int gradientDirection)
{
    beautifyMode = std::clamp(beautifyMode, 0, 1);
    backgroundOpacity = std::clamp(backgroundOpacity, 0.0f, 1.0f);
    backgroundStartR = std::clamp(backgroundStartR, 0.0f, 1.0f);
    backgroundStartG = std::clamp(backgroundStartG, 0.0f, 1.0f);
    backgroundStartB = std::clamp(backgroundStartB, 0.0f, 1.0f);
    backgroundEndR = std::clamp(backgroundEndR, 0.0f, 1.0f);
    backgroundEndG = std::clamp(backgroundEndG, 0.0f, 1.0f);
    backgroundEndB = std::clamp(backgroundEndB, 0.0f, 1.0f);
    gradientDirection = std::clamp(gradientDirection, 0, 3);

    auto differs = [](float lhs, float rhs) {
        return std::fabs(lhs - rhs) > 0.0005f;
    };

    if (enabled == iconBeautifyEnabled_ &&
        beautifyMode == iconBeautifyMode_ &&
        gradientEnabled == iconBeautifyGradientEnabled_ &&
        !differs(backgroundOpacity, iconBeautifyBgOpacity_) &&
        !differs(backgroundStartR, iconBeautifyBgStartR_) &&
        !differs(backgroundStartG, iconBeautifyBgStartG_) &&
        !differs(backgroundStartB, iconBeautifyBgStartB_) &&
        !differs(backgroundEndR, iconBeautifyBgEndR_) &&
        !differs(backgroundEndG, iconBeautifyBgEndG_) &&
        !differs(backgroundEndB, iconBeautifyBgEndB_) &&
        gradientDirection == iconBeautifyGradientDirection_)
    {
        return;
    }

    iconBeautifyEnabled_ = enabled;
    iconBeautifyMode_ = beautifyMode;
    iconBeautifyBgOpacity_ = backgroundOpacity;
    iconBeautifyGradientEnabled_ = gradientEnabled;
    iconBeautifyBgStartR_ = backgroundStartR;
    iconBeautifyBgStartG_ = backgroundStartG;
    iconBeautifyBgStartB_ = backgroundStartB;
    iconBeautifyBgEndR_ = backgroundEndR;
    iconBeautifyBgEndG_ = backgroundEndG;
    iconBeautifyBgEndB_ = backgroundEndB;
    iconBeautifyGradientDirection_ = gradientDirection;

    d2dIconCache_.clear();
    placeholderIconCache_.clear();
    quickNavSysIconCache_.clear();
    privacyFileIconBitmap_.Reset();
    privacyFolderIconBitmap_.Reset();
    InvalidateDragStaticScene();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (quickNavigationOpen_)
        InvalidateQuickNavigationWindow();
}

/**
 * @brief 获取系统主显示器在 gridPages_ 中的索引（回退到 0）。
 * @return 页面索引。
 */
inline size_t DesktopApp::FirstMonitorOrderIndex() const
{
    if (gridPages_.empty()) return 0;
    for (size_t i = 0; i < gridPages_.size(); ++i)
        if (!primaryMonitorId_.empty() && gridPages_[i].monitorId == primaryMonitorId_)
            return i;
    for (size_t i = 0; i < gridPages_.size(); ++i)
        if (gridPages_[i].isPrimary) return i;
    return 0;
}

/**
 * @brief 构建监控器渲染顺序（双锚点线性：首屏锁 → 中间按 left 升序 → 末屏锁）。
 *
 * 双锚点解析：先看持久化的 firstPageMonitorId_/lastPageMonitorId_ 是否在线；
 * 离线/无效时回退——首屏首选主屏，末屏首选最右屏；被对方占用时向左找最近替代。
 * 单屏时该屏同时担首屏与末屏。离线不清锁，重连自动恢复。
 * @return gridPages_ 索引的顺序列表。
 */
inline std::vector<size_t> DesktopApp::BuildMonitorRenderOrder() const
{
    std::vector<size_t> order;
    if (gridPages_.empty()) return order;
    const size_t N = gridPages_.size();
    order.reserve(N);

    // 按 bounds.left 升序的 gridPages_ 索引列表
    std::vector<size_t> sorted;
    sorted.reserve(N);
    for (size_t i = 0; i < N; ++i) sorted.push_back(i);
    std::sort(sorted.begin(), sorted.end(),
        [&](size_t a, size_t b) { return gridPages_[a].bounds.left < gridPages_[b].bounds.left; });

    auto findById = [&](const std::wstring& id) -> int {
        if (id.empty()) return -1;
        for (size_t k = 0; k < sorted.size(); ++k)
            if (gridPages_[sorted[k]].monitorId == id) return static_cast<int>(k);
        return -1;
    };

    int firstIdx = findById(firstPageMonitorId_);
    int lastIdx  = findById(lastPageMonitorId_);

    // 防御：两锁指向同一屏（旧/坏配置），令末屏锁休眠
    if (firstIdx >= 0 && lastIdx >= 0 && firstIdx == lastIdx) lastIdx = -1;

    const int primaryPos = static_cast<int>(FirstMonitorOrderIndex());
    const int rightmostPos = static_cast<int>(sorted.size()) - 1;

    // 从 prefer 开始，如果被 avoidIdx 占用则向左找最近的替代（符合"向左找"语义）
    auto resolveFallback = [&](int prefer, int avoidIdx) -> int {
        if (prefer != avoidIdx) return prefer;
        for (int d = 1; d < static_cast<int>(sorted.size()); ++d)
        {
            int left = prefer - d;
            int right = prefer + d;
            if (left >= 0 && left != avoidIdx) return left;
            if (right < static_cast<int>(sorted.size()) && right != avoidIdx) return right;
        }
        return avoidIdx;   // N>=2 时不会走到，仅兜底
    };

    if (N == 1)
    {
        order.push_back(sorted[0]);
        return order;
    }

    if (firstIdx >= 0 && lastIdx >= 0)
    {
        // case A：两锁均在线且不同屏
    }
    else if (firstIdx >= 0)
    {
        // case B：首屏锁在线，末屏回退到最右屏（被首屏锁占用则向左找替代）
        lastIdx = resolveFallback(rightmostPos, firstIdx);
    }
    else if (lastIdx >= 0)
    {
        // case C：末屏锁在线，首屏回退到主屏（被末屏锁占用则向左找替代）
        firstIdx = resolveFallback(primaryPos, lastIdx);
    }
    else
    {
        // case D：两锁均离线/空，首屏=主屏，末屏=最右屏
        firstIdx = resolveFallback(primaryPos, -1);
        lastIdx  = resolveFallback(rightmostPos, firstIdx);
    }

    order.push_back(sorted[firstIdx]);
    if (firstIdx != lastIdx)
    {
        for (size_t k = 0; k < sorted.size(); ++k)
            if (static_cast<int>(k) != firstIdx && static_cast<int>(k) != lastIdx)
                order.push_back(sorted[k]);
        order.push_back(sorted[lastIdx]);
    }
    else
    {
        // 合并分支（N>=2 时理论上不会走到，仅作兜底）
        for (size_t k = 0; k < sorted.size(); ++k)
            if (static_cast<int>(k) != firstIdx) order.push_back(sorted[k]);
    }
    return order;
}

inline const GridPage* DesktopApp::GetFirstPageGridPage() const
{
    if (gridPages_.empty()) return nullptr;

    if (!savedPageIds_.empty())
    {
        const std::wstring& firstPageId = savedPageIds_.front();
        for (const auto& page : gridPages_)
            if (page.id == firstPageId)
                return &page;
    }

    std::vector<size_t> order = BuildMonitorRenderOrder();
    if (!order.empty() && order.front() < gridPages_.size())
        return &gridPages_[order.front()];

    return &gridPages_.front();
}

/**
 * @brief 检查指定页面是否包含任何内容（项目或组件）。
 * @param pageId 页面 ID。
 * @return 有内容返回 true，否则 false。
 */
inline bool DesktopApp::PageHasContent(const std::wstring& pageId) const
{
    if (pageId.empty() || pageId == kDockPageId) return false;
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId == pageId) return true;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w) &&
            w.gridCell.pageId == pageId) return true;
    return false;
}

/**
 * @brief 从当前偏移位置沿指定方向查找下一个非空页面的偏移量。
 * @param fromOffset 起始偏移量。
 * @param direction 方向（1 向前 / -1 向后）。
 * @return 找到的偏移量，未找到则返回原偏移量。
 */
inline int DesktopApp::NextNonEmptyOffset(int fromOffset, int direction) const
{
    if (savedPageIds_.empty() || gridPages_.empty()) return fromOffset;
    const int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
    int offset = fromOffset;
    while (true)
    {
        offset += direction;
        if (offset < 0 || offset > static_cast<int>(savedPageIds_.size()) - visiblePageCount)
            return fromOffset;
        size_t pageIdx = static_cast<size_t>((visiblePageCount - 1) + offset);
        if (pageIdx < savedPageIds_.size() && PageHasContent(savedPageIds_[pageIdx]))
            return offset;
    }
}

/**
 * @brief 计算最大页面偏移量（最后一个有内容的页面位置）。
 * @return 最大偏移值。
 */
inline int DesktopApp::MaxPageOffset() const
{
    if (savedPageIds_.empty() || gridPages_.empty()) return 0;
    const int visiblePageCount = static_cast<int>(std::min(savedPageIds_.size(), gridPages_.size()));
    const int rawMax = std::max(0, static_cast<int>(savedPageIds_.size()) - visiblePageCount);
    int result = 0;
    for (int off = 1; off <= rawMax; ++off)
    {
        size_t pageIdx = static_cast<size_t>((visiblePageCount - 1) + off);
        if (pageIdx < savedPageIds_.size() && PageHasContent(savedPageIds_[pageIdx]))
            result = off;
    }
    return result;
}

inline std::wstring DesktopApp::GetPageDisplayName(int index) const
{
    return _LFW("app.grid.page_label", std::to_wstring(index + 1));
}

inline void DesktopApp::NavigatePageOffset(int delta)
{
    if (delta < 0 && pageOffset_ <= 0) return;
    if (delta > 0 && pageOffset_ >= MaxPageOffset()) return;
    pageOffset_ = NextNonEmptyOffset(pageOffset_, delta);
    ApplyPageMapping();
    LayoutItems();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    RestoreDesktopWindowLayer();
}

inline void DesktopApp::JumpToPageOffset(int targetOffset)
{
    pageOffset_ = std::clamp(targetOffset, 0, MaxPageOffset());
    ApplyPageMapping();
    LayoutItems();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    RestoreDesktopWindowLayer();
}

inline void DesktopApp::AddNewPage()
{
    if (gridPages_.empty()) return;
    const size_t N = gridPages_.size();

    // 1. 从前到后遍历，找第一个"空且当前正显示在某个显示器上"的页，直接放 Guide 占位。
    //    槽位页(index < N) 总是显示在前 N 个显示器上；末屏显示 savedPageIds_[N-1+pageOffset_]。
    const size_t lastDisplayedIdx = (N >= 1)
        ? (N - 1 + static_cast<size_t>(std::max(0, pageOffset_)))
        : 0;
    for (size_t i = 0; i < savedPageIds_.size(); ++i)
    {
        const bool isDisplayed = (i < N) || (i == lastDisplayedIdx);
        if (isDisplayed && !PageHasContent(savedPageIds_[i]))
        {
            PlaceGuideWidgetOnPage(savedPageIds_[i]);   // PlaceGuideWidgetOnPage 内部已 SaveLayoutSlots
            ApplyPageMapping();
            LayoutItems();
            ShowPageNotify(GetPageDisplayName(static_cast<int>(i)));
            if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
            return;
        }
    }

    // 2. 没有空显示页，在末尾新建溢出页
    auto monitorOrder = BuildMonitorRenderOrder();
    const GridPage& lastPage = gridPages_[monitorOrder.back()];

    std::wstring newPageId = GeneratePageId();
    savedPageIds_.push_back(newPageId);
    savedPageColumns_[newPageId] = lastPage.columns;
    savedPageRows_[newPageId] = lastPage.rows;
    RememberSavedPageId(newPageId);

    PlaceGuideWidgetOnPage(newPageId);   // Guide 占位 → 非空 → 不被清理

    pageOffset_ = MaxPageOffset();       // 跳到新页
    ApplyPageMapping();
    LayoutItems();

    // 显式触发换页通知（ApplyPageMapping 内的变化检测可能因 lastMonitorPageId_ 被清空而失效）
    auto it = std::ranges::find(savedPageIds_, newPageId);
    if (it != savedPageIds_.end())
        ShowPageNotify(GetPageDisplayName(static_cast<int>(it - savedPageIds_.begin())));

    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::PlaceGuideWidgetOnPage(const std::wstring& pageId)
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::Guide;
    w.title = _LW("app.guide.title");
    w.showTitle = true;
    w.bottomBarHover = true;
    w.gridSpan = { 4, 3 };

    const auto* page = FindGridPage(gridPages_, pageId);
    int cols = page ? page->columns : (savedPageColumns_.count(pageId) ? savedPageColumns_[pageId] : 4);
    int rows = page ? page->rows : (savedPageRows_.count(pageId) ? savedPageRows_[pageId] : 4);

    std::unordered_set<std::wstring> used;
    for (auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId == pageId)
            MarkGridArea(used, item.gridCell, item.gridSpan);
    for (auto& ow : widgets_)
        if (snowdesktop::collection_group_rules::
                ShouldOccupyDesktopGrid(
                    IsGroupedWidget(ow)) &&
            ow.gridCell.pageId == pageId)
            MarkGridArea(used, ow.gridCell, ow.gridSpan);

    w.gridCell.pageId = pageId;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
        {
            GridCell cell{ pageId, c, r };
            if (!AreGridSlotsMarked(used, cell, w.gridSpan))
            {
                w.gridCell = cell;
                goto placed;
            }
        }
    w.gridCell = { pageId, 0, 0 };
placed:
    widgets_.push_back(std::move(w));
    ConfigureWidgetGridLimits(widgets_.back());
    RebuildContainersAndItems();
    SaveLayoutSlots();
}

/**
 * @brief 将已保存的页面 ID 映射到当前网格页面，并应用保存的列/行数。
 */
inline std::wstring DesktopApp::GeneratePageId() const
{
    int maxNum = 0;
    for (auto& pid : savedPageIds_)
        if (pid.starts_with(L"__page:"))
            maxNum = (std::max)(maxNum, std::stoi(pid.substr(7)));
    return L"__page:" + std::to_wstring(maxNum + 1);
}

/**
 * @brief 每次加载后按 pages 列表顺序重新规整为 __page:1, __page:2, ...
 */
inline void DesktopApp::NormalizePageIds()
{
    std::erase(savedPageIds_, std::wstring(kDockPageId));
    savedPageColumns_.erase(kDockPageId);
    savedPageRows_.erase(kDockPageId);
    if (savedPageIds_.empty()) return;
    if (savedPageIds_.size() > 9999) return;   // 防御：编号爆炸

    // 按 pages 列表顺序严格分配 __page:1, __page:2, ...
    std::unordered_map<std::wstring, std::wstring> idMap;
    for (size_t i = 0; i < savedPageIds_.size(); ++i)
    {
        std::wstring newId = L"__page:" + std::to_wstring(i + 1);
        if (savedPageIds_[i] != newId)
            idMap[savedPageIds_[i]] = newId;
    }
    if (idMap.empty()) return;

    // savedPageIds_
    for (auto& pid : savedPageIds_)
        if (idMap.contains(pid)) pid = idMap[pid];

    // layoutRecords_: keys and cell.pageId（用 try_emplace 防止 key 冲突覆盖）
    std::unordered_map<std::wstring, LayoutRecord> newRecords;
    for (auto& [key, rec] : layoutRecords_)
    {
        if (idMap.contains(rec.cell.pageId)) rec.cell.pageId = idMap[rec.cell.pageId];
        std::wstring newKey = idMap.contains(key) ? idMap[key] : key;
        newRecords.try_emplace(newKey, std::move(rec));
    }
    layoutRecords_ = std::move(newRecords);

    // widgets_
    for (auto& w : widgets_)
        if (idMap.contains(w.gridCell.pageId)) w.gridCell.pageId = idMap[w.gridCell.pageId];

    // items_
    for (auto& item : items_)
        if (idMap.contains(item.gridCell.pageId)) item.gridCell.pageId = idMap[item.gridCell.pageId];

    // savedPageColumns_ / savedPageRows_
    std::unordered_map<std::wstring, int> newCols, newRows;
    for (auto& [k, v] : savedPageColumns_)
        newCols.try_emplace(idMap.contains(k) ? idMap[k] : k, v);
    for (auto& [k, v] : savedPageRows_)
        newRows.try_emplace(idMap.contains(k) ? idMap[k] : k, v);
    savedPageColumns_ = std::move(newCols);
    savedPageRows_ = std::move(newRows);
}

/**
 * @brief 清理空页，按三类分别处理：
 *
 *   1. 槽位页（index < N-1）：前 N-1 个显示器各占一个，永远保留（即便空）。
 *   2. 末屏默认页（index == N-1）：末屏 pageOffset=0 时显示的页。
 *      - 非空 → 保留
 *      - 空 + 后面有非空页 → 清理，让后面递补上来
 *      - 空 + 后面无非空页 → 保留作末屏占位（避免末屏空网格）
 *   3. 溢出区其余页（index > N-1）：末屏翻页区。
 *      - 非空 → 保留
 *      - 空 → 一律清理（pageOffset 钳制使末屏回退到前面非空页）
 */
inline void DesktopApp::PruneEmptyOverflowPages()
{
    const size_t N = gridPages_.size();
    if (N == 0 || savedPageIds_.empty()) return;

    // 预计算：每个 index 之后是否存在非空页
    const size_t total = savedPageIds_.size();
    std::vector<bool> hasNonEmptyAfter(total, false);
    {
        bool seen = false;
        for (size_t i = total; i-- > 0; )
        {
            hasNonEmptyAfter[i] = seen;
            if (PageHasContent(savedPageIds_[i]))
                seen = true;
        }
    }

    std::vector<std::wstring> keep;
    for (size_t i = 0; i < total; ++i)
    {
        const bool isSlotPage = (N >= 2) && (i < N - 1);        // 前 N-1 槽位页
        const bool isLastDefault = (i == N - 1);                // 末屏默认页（pageOffset=0 时显示）
        const bool isEmpty = !PageHasContent(savedPageIds_[i]);

        if (isSlotPage)
            keep.push_back(savedPageIds_[i]);                   // 槽位页永远保留
        else if (!isEmpty)
            keep.push_back(savedPageIds_[i]);                   // 非空保留
        else if (isLastDefault && !hasNonEmptyAfter[i])
            keep.push_back(savedPageIds_[i]);                   // 末屏默认页空 + 后面无非空 → 保留作占位
        else
        {
            // 末屏默认页空 + 后面有非空 → 清理递补
            // 溢出区其余页空 → 清理
            savedPageColumns_.erase(savedPageIds_[i]);
            savedPageRows_.erase(savedPageIds_[i]);
        }
    }
    savedPageIds_ = std::move(keep);
}

/**
 * @brief 按显示器数量补齐前 N 个槽位页（每个显示器都有一个占位页）。
 */
inline void DesktopApp::PadPagesToMonitorCount()
{
    const size_t N = gridPages_.size();
    if (N == 0) return;
    const size_t target = N;   // 补齐到 N 个：每个显示器一个槽位页
    std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
    while (savedPageIds_.size() < target)
    {
        std::wstring newId = GeneratePageId();
        const size_t refIdx = savedPageIds_.size();
        const GridPage* refPage = (refIdx < monitorOrder.size() && monitorOrder[refIdx] < gridPages_.size())
            ? &gridPages_[monitorOrder[refIdx]]
            : GetFirstPageGridPage();
        if (!refPage) return;
        savedPageIds_.push_back(newId);
        savedPageColumns_[newId] = std::max(1, refPage->columns);
        savedPageRows_[newId] = std::max(1, refPage->rows);
    }
}

/**
 * @brief 若页面编号不连续则重排为 __page:1,2,3...（封装 NormalizePageIds 的早退判断）。
 */
inline void DesktopApp::CompactPageIds()
{
    if (savedPageIds_.empty()) return;
    bool needCompact = false;
    for (size_t i = 0; i < savedPageIds_.size(); ++i)
    {
        if (savedPageIds_[i] != L"__page:" + std::to_wstring(i + 1))
        { needCompact = true; break; }
    }
    if (needCompact) NormalizePageIds();
}

/**
 * @brief 将 savedPageIds_ 映射到各显示器：前 N-1 显示器固定，末屏翻页 + pageOffset 钳制。
 */
inline void DesktopApp::MapPagesToMonitors()
{
    const std::wstring oldLastPageId = lastMonitorPageId_;
    lastMonitorPageId_.clear();
    if (gridPages_.empty()) return;

    pageOffset_ = std::clamp(pageOffset_, 0, MaxPageOffset());
    std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
    const size_t numMonitors = monitorOrder.size();
    for (size_t i = 0; i < numMonitors; ++i)
    {
        GridPage& page = gridPages_[monitorOrder[i]];
        const bool isLast = (i == numMonitors - 1);
        const size_t pageIdx = i + (isLast ? static_cast<size_t>(pageOffset_) : 0);
        if (pageIdx < savedPageIds_.size())
            page.id = savedPageIds_[pageIdx];
        else
            page.id = L"";   // 越界：末屏渲染空网格
        if (isLast)
            lastMonitorPageId_ = page.id;
    }
    // 仅当存在溢出页时保留 lastMonitorPageId_，否则清空（gfx 据此决定末屏箭头）
    if (!lastMonitorPageId_.empty() && savedPageIds_.size() <= gridPages_.size())
        lastMonitorPageId_.clear();

    // 末屏显示页变化时触发换页通知（类似电视台换台角标）
    // 仅当 oldLastPageId 非空时才触发——避免启动/初始化时误弹通知
    if (!oldLastPageId.empty() && !lastMonitorPageId_.empty() && lastMonitorPageId_ != oldLastPageId)
    {
        auto it = std::ranges::find(savedPageIds_, lastMonitorPageId_);
        if (it != savedPageIds_.end())
        {
            const int pageIdx = static_cast<int>(it - savedPageIds_.begin());
            ShowPageNotify(GetPageDisplayName(pageIdx));
        }
    }
}

/**
 * @brief 应用页面到显示器的映射（编排：清理 → 补齐 → 重排 → 映射 → 应用保存的网格尺寸）。
 */
inline void DesktopApp::ApplyPageMapping()
{
    if (gridPages_.empty()) { lastMonitorPageId_.clear(); return; }

    // 首屏兜底：若 savedPageIds_ 为空，确保至少有一个首屏页
    if (savedPageIds_.empty())
    {
        std::wstring firstId = GeneratePageId();
        savedPageIds_.push_back(firstId);
        const GridPage* ref = GetFirstPageGridPage();
        if (!ref) return;
        savedPageColumns_[firstId] = std::max(1, ref->columns);
        savedPageRows_[firstId] = std::max(1, ref->rows);
    }

    PruneEmptyOverflowPages();
    PadPagesToMonitorCount();
    CompactPageIds();
    MapPagesToMonitors();
    ApplySavedGridDimensions();
}

/**
 * @brief 在 usedSlots 集合中标记一个网格区域的所有格子被占用。
 * @param usedSlots 已占用格子集合。
 * @param cell 起始单元格。
 * @param span 跨度。
 */
inline void DesktopApp::MarkGridArea(std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
{
    for (int c = cell.column; c < cell.column + span.columns; ++c)
        for (int r = cell.row; r < cell.row + span.rows; ++r)
            usedSlots.insert(cell.pageId + L":" + std::to_wstring(c) + L"," + std::to_wstring(r));
}

/**
 * @brief 检查某个网格区域是否有任何格子已被标记。
 * @param usedSlots 已占用格子集合。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 如果有任何格子被标记返回 true。
 */
inline bool DesktopApp::AreGridSlotsMarked(const std::unordered_set<std::wstring>& usedSlots, const GridCell& cell, GridSpan span)
{
    for (int c = cell.column; c < cell.column + span.columns; ++c)
        for (int r = cell.row; r < cell.row + span.rows; ++r)
            if (usedSlots.count(cell.pageId + L":" + std::to_wstring(c) + L"," + std::to_wstring(r)))
                return true;
    return false;
}

/**
 * @brief 判断网格区域是否合法（跨度 >=1，行列非负）。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 合法返回 true。
 */
inline bool DesktopApp::IsGridAreaValid(const GridCell& cell, GridSpan span)
{
    if (span.columns < 1 || span.rows < 1) return false;
    if (cell.column < 0 || cell.row < 0) return false;
    return true;
}

/**
 * @brief 尝试在网格中查找一个空闲单元格以放置指定跨度的项目。
 * @param span 所需跨度。
 * @param usedSlots 已占用的格子集合。
 * @param result 输出参数，找到的空闲单元格。
 * @param preferredPageId 首选页面 ID。
 * @param preferredStartSlot 首选起始槽位。
 * @return 找到返回 true。
 */
inline bool DesktopApp::TryFindFreeCell(
    GridSpan span, std::unordered_set<std::wstring>& usedSlots, GridCell& result,
    const std::wstring& preferredPageId, int preferredStartSlot) const
{
    // Automatic placement follows the configured first-page monitor order,
    // not the physical left-to-right monitor order.
    std::vector<size_t> pageOrder = BuildMonitorRenderOrder();
    if (pageOrder.empty())
    {
        pageOrder.reserve(gridPages_.size());
        for (size_t i = 0; i < gridPages_.size(); ++i)
            pageOrder.push_back(i);
    }

    auto tryPage = [&](const GridPage& page, int startSlot, GridCell& found) -> bool {
        const int capacity = std::max(1, page.columns * page.rows);
        for (int slot = std::clamp(startSlot, 0, capacity - 1); slot < capacity; ++slot)
        {
            GridCell candidate;
            candidate.pageId = page.id;
            candidate.column = slot / std::max(1, page.rows);
            candidate.row = slot % std::max(1, page.rows);
            if (GridAreaFitsPage(page, candidate, span) && !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                found = candidate;
                return true;
            }
        }
        return false;
    };

    if (!preferredPageId.empty())
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == preferredPageId && tryPage(page, preferredStartSlot, result))
                return true;
        }
    }

    for (size_t pageIndex : pageOrder)
    {
        if (pageIndex >= gridPages_.size()) continue;
        const auto& page = gridPages_[pageIndex];
        if (!preferredPageId.empty() && page.id == preferredPageId) continue;
        if (tryPage(page, 0, result))
            return true;
    }

    if (!preferredPageId.empty())
    {
        for (const auto& page : gridPages_)
        {
            if (page.id == preferredPageId && tryPage(page, 0, result))
                return true;
        }
    }
    return false;
}

/**
 * @brief 重新放置所有因页面尺寸变化而被移出边界的项目和组件。
 *
 * 对于无法放入原位置的项目，自动扩展页面或寻找空闲单元格安置。
 */
inline void DesktopApp::RelayoutDisplacedItems()
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    std::unordered_set<std::wstring> usedSlots;
    // Track newly created virtual pages and their next free slot index
    std::unordered_map<std::wstring, int> newPageSlots;

    auto tryPlaceOnPage = [&](const std::wstring& pageId, int columns, int rows,
                               int& nextSlot, GridSpan span, GridCell& found) -> bool {
        const int capacity = std::max(1, columns * rows);
        for (int slot = nextSlot; slot < capacity; ++slot)
        {
            GridCell candidate;
            candidate.pageId = pageId;
            candidate.column = slot / std::max(1, rows);
            candidate.row    = slot % std::max(1, rows);
            if (candidate.column + span.columns <= columns &&
                candidate.row + span.rows <= rows &&
                !AreGridSlotsMarked(usedSlots, candidate, span))
            {
                found = candidate;
                nextSlot = slot + 1;
                return true;
            }
        }
        return false;
    };

    // Build a quick-lookup set of page IDs currently visible in gridPages_
    std::unordered_set<std::wstring> visiblePageIds;
    for (const auto& gp : gridPages_)
        visiblePageIds.insert(gp.id);

    auto findFreeCellOrGrow = [&](GridSpan span, GridCell& result, const std::wstring& preferredPageId) -> bool {
        if (TryFindFreeCell(span, usedSlots, result, preferredPageId))
            return true;

        // Search all saved pages that aren't currently visible (virtual pages at other offsets)
        for (const auto& pageId : savedPageIds_)
        {
            if (visiblePageIds.count(pageId)) continue;   // already tried via TryFindFreeCell
            if (!savedPageColumns_.count(pageId) || !savedPageRows_.count(pageId)) continue;
            int cols = savedPageColumns_[pageId];
            int rows = savedPageRows_[pageId];
            int dummySlot = 0;
            if (tryPlaceOnPage(pageId, cols, rows, dummySlot, span, result))
                return true;
        }

        // Try previously-created new pages in this batch before creating another
        for (auto& [pageId, nextSlot] : newPageSlots)
        {
            int cols = savedPageColumns_.count(pageId) ? savedPageColumns_[pageId] : 1;
            int rows = savedPageRows_.count(pageId) ? savedPageRows_[pageId] : 1;
            if (tryPlaceOnPage(pageId, cols, rows, nextSlot, span, result))
                return true;
        }

        // No space anywhere — create a new virtual page on the last monitor
        std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
        if (monitorOrder.empty()) return false;

        GridPage& lastPage = gridPages_[monitorOrder.back()];

        std::wstring newPageId = GeneratePageId();
        RememberSavedPageId(newPageId);
        savedPageColumns_[newPageId] = lastPage.columns;
        savedPageRows_[newPageId]    = lastPage.rows;

        result.pageId = newPageId;
        result.column = 0;
        result.row    = 0;
        newPageSlots[newPageId] = 1; // slot 0 taken
        return true;
    };

    std::vector<size_t> displacedWidgets;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        auto& widget = widgets_[i];
        if (IsGroupedWidget(widget))
            continue;
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (!page)
        {
            // Widget is on a saved page not in current gridPages_ (different pageOffset).
            // If its position is still valid per saved dimensions, keep it in place.
            const std::wstring& pid = widget.gridCell.pageId;
            if (!pid.empty() && savedPageColumns_.count(pid) && savedPageRows_.count(pid))
            {
                int cols = savedPageColumns_[pid];
                int rows = savedPageRows_[pid];
                widget.gridSpan = ClampWidgetGridSpan(widget, widget.gridSpan, cols, rows);
                if (widget.gridCell.column >= 0 && widget.gridCell.row >= 0 &&
                    widget.gridCell.column + widget.gridSpan.columns <= cols &&
                    widget.gridCell.row + widget.gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, widget.gridCell, widget.gridSpan))
                {
                    MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
                    continue;
                }
            }
            displacedWidgets.push_back(i);
            continue;
        }

        widget.gridSpan = ClampWidgetGridSpan(widget, widget.gridSpan,
            page->columns, page->rows);
        if (GridAreaFitsPage(*page, widget.gridCell, widget.gridSpan) &&
            !AreGridSlotsMarked(usedSlots, widget.gridCell, widget.gridSpan))
        {
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
        }
        else
        {
            displacedWidgets.push_back(i);
        }
    }

    for (size_t widgetIndex : displacedWidgets)
    {
        auto& widget = widgets_[widgetIndex];
        GridCell freeCell;
        if (findFreeCellOrGrow(widget.gridSpan, freeCell, widget.gridCell.pageId))
        {
            widget.gridCell = freeCell;
            MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);
        }
    }

    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        const GridPage* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (page)
        {
            item.gridSpan.columns = std::clamp(item.gridSpan.columns, 1, std::max(1, page->columns));
            item.gridSpan.rows = std::clamp(item.gridSpan.rows, 1, std::max(1, page->rows));
            if (GridAreaFitsPage(*page, item.gridCell, item.gridSpan) &&
                !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan))
            {
                MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                continue;
            }
        }
        else if (!item.gridCell.pageId.empty())
        {
            // Item is on a saved page not in current gridPages_ (different pageOffset).
            // Mark its slot so searching those pages won't see it as free.
            const std::wstring& pid = item.gridCell.pageId;
            if (savedPageColumns_.count(pid) && savedPageRows_.count(pid))
            {
                int cols = savedPageColumns_[pid];
                int rows = savedPageRows_[pid];
                if (item.gridCell.column >= 0 && item.gridCell.row >= 0 &&
                    item.gridCell.column + item.gridSpan.columns <= cols &&
                    item.gridCell.row + item.gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan))
                {
                    MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                    continue;
                }
            }
        }

        GridCell freeCell;
        if (findFreeCellOrGrow(item.gridSpan, freeCell, item.gridCell.pageId))
        {
            item.gridCell = freeCell;
            item.slot = SlotFromCell(gridPages_, freeCell);
            MarkGridArea(usedSlots, freeCell, item.gridSpan);
        }
    }
}

/**
 * @brief 按名称对桌面图标排序（在每个页面内）。
 */
inline void DesktopApp::SortIconsByName(bool ascending)
{
    auto sortForPage = [&](const GridPage& page) {
        const GridPage* firstPage = GetFirstPageGridPage();
        std::vector<size_t> order;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].name.empty() || IsItemInAnyWidget(items_[i])) continue;
            if (items_[i].gridCell.pageId.empty())
                items_[i].gridCell.pageId = firstPage ? firstPage->id : L"";
            if (items_[i].gridCell.pageId == page.id)
                order.push_back(i);
        }

        std::sort(order.begin(), order.end(), [this, ascending](size_t a, size_t b) {
            int cmp = ToUpperInvariant(items_[a].name).compare(ToUpperInvariant(items_[b].name));
            return ascending ? (cmp < 0) : (cmp > 0);
        });

        std::unordered_set<std::wstring> usedSlots;
        for (const auto& widget : widgets_)
            if (!IsGroupedWidget(widget) &&
                widget.gridCell.pageId == page.id)
                MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);

        int searchSlot = 0;
        for (size_t itemIndex : order)
        {
            items_[itemIndex].gridSpan = { 1, 1 };
            bool placed = false;
            for (int slot = searchSlot; slot < page.columns * page.rows; ++slot)
            {
                GridCell cell{ page.id, slot / std::max(1, page.rows), slot % std::max(1, page.rows) };
                if (cell.column >= page.columns || cell.row >= page.rows) continue;
                if (AreGridSlotsMarked(usedSlots, cell, items_[itemIndex].gridSpan)) continue;
                items_[itemIndex].gridCell = cell;
                items_[itemIndex].slot = cell.column * std::max(1, page.rows) + cell.row;
                MarkGridArea(usedSlots, cell, items_[itemIndex].gridSpan);
                searchSlot = slot + 1;
                placed = true;
                break;
            }
            if (!placed)
                MarkGridArea(usedSlots, items_[itemIndex].gridCell, items_[itemIndex].gridSpan);
        }
    };

    for (const auto& page : gridPages_)
        sortForPage(page);

    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 按类型名称对桌面图标排序，相同类型内按名称排序。
 */
inline void DesktopApp::SortIconsByType(bool ascending)
{
    auto sortForPage = [&](const GridPage& page) {
        const GridPage* firstPage = GetFirstPageGridPage();
        std::vector<size_t> order;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (items_[i].name.empty() || IsItemInAnyWidget(items_[i])) continue;
            if (items_[i].gridCell.pageId.empty())
                items_[i].gridCell.pageId = firstPage ? firstPage->id : L"";
            if (items_[i].gridCell.pageId == page.id)
                order.push_back(i);
        }

        std::sort(order.begin(), order.end(), [this, ascending](size_t a, size_t b) {
            int cmp = ToUpperInvariant(items_[a].typeName).compare(ToUpperInvariant(items_[b].typeName));
            if (cmp != 0) return ascending ? (cmp < 0) : (cmp > 0);
            cmp = ToUpperInvariant(items_[a].name).compare(ToUpperInvariant(items_[b].name));
            return ascending ? (cmp < 0) : (cmp > 0);
        });

        std::unordered_set<std::wstring> usedSlots;
        for (const auto& widget : widgets_)
            if (!IsGroupedWidget(widget) &&
                widget.gridCell.pageId == page.id)
                MarkGridArea(usedSlots, widget.gridCell, widget.gridSpan);

        int searchSlot = 0;
        for (size_t itemIndex : order)
        {
            items_[itemIndex].gridSpan = { 1, 1 };
            bool placed = false;
            for (int slot = searchSlot; slot < page.columns * page.rows; ++slot)
            {
                GridCell cell{ page.id, slot / std::max(1, page.rows), slot % std::max(1, page.rows) };
                if (cell.column >= page.columns || cell.row >= page.rows) continue;
                if (AreGridSlotsMarked(usedSlots, cell, items_[itemIndex].gridSpan)) continue;
                items_[itemIndex].gridCell = cell;
                items_[itemIndex].slot = cell.column * std::max(1, page.rows) + cell.row;
                MarkGridArea(usedSlots, cell, items_[itemIndex].gridSpan);
                searchSlot = slot + 1;
                placed = true;
                break;
            }
            if (!placed)
                MarkGridArea(usedSlots, items_[itemIndex].gridCell, items_[itemIndex].gridSpan);
        }
    };

    for (const auto& page : gridPages_)
        sortForPage(page);

    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 对指定组件（文件夹映射/桌面文件/集合）中的内容排序。
 * @param widgetIndex 组件索引。
 * @param mode 排序模式：0 按名称，1 按类型，2 按修改时间。
 */
inline void DesktopApp::SortWidgetContents(size_t widgetIndex, int mode, bool ascending)
{
    if (widgetIndex >= widgets_.size()) return;
    DesktopWidget& w = widgets_[widgetIndex];

    if (w.type == DesktopWidgetType::CollectionGroup)
    {
        std::wstring active = w.activeCategoryId;
        if (std::find(
                w.childWidgetIds.begin(),
                w.childWidgetIds.end(), active) ==
            w.childWidgetIds.end())
            active = w.childWidgetIds.empty()
                ? L""
                : w.childWidgetIds.front();
        const size_t activeIndex =
            FindWidgetIndexById(active);
        if (activeIndex < widgets_.size() &&
            widgets_[activeIndex].type ==
                DesktopWidgetType::Collection)
            SortWidgetContents(
                activeIndex, mode, ascending);
        return;
    }

    if (w.type == DesktopWidgetType::FolderMapping)
    {
        w.folderSortMode =
            snowdesktop::folder_sort_rules::
                NormalizeMode(mode);
        w.folderSortAscending = ascending;
        snowdesktop::folder_sort_rules::StableSort(
            w.folderEntries,
            w.folderSortMode,
            w.folderSortAscending);
        snowdesktop::folder_sort_rules::
            RewriteOrderKeys(
                w.folderEntries, w.itemKeys);
        RefreshFolderMappingWidget(widgetIndex);
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    else if (w.type == DesktopWidgetType::FileCategories)
    {
        std::vector<std::wstring> keys;
        std::unordered_set<std::wstring> seen;
        for (const auto& rawKey : w.itemKeys)
        {
            std::wstring nk = ToUpperInvariant(rawKey);
            if (seen.insert(nk).second)
                keys.push_back(rawKey);
        }

        std::sort(keys.begin(), keys.end(),
            [this, mode, ascending](const std::wstring& ka, const std::wstring& kb) {
                size_t ia = FindItemIndexByKey(ka);
                size_t ib = FindItemIndexByKey(kb);
                if (ia == static_cast<size_t>(-1) || ib == static_cast<size_t>(-1)) return false;
                int cmp = 0;
                if (mode == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                else if (mode == 1)
                {
                    cmp = _wcsicmp(items_[ia].typeName.c_str(), items_[ib].typeName.c_str());
                    if (cmp == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                else if (mode == 2)
                {
                    WIN32_FILE_ATTRIBUTE_DATA da{}, db{};
                    if (GetFileAttributesExW(items_[ia].parsingName.c_str(), GetFileExInfoStandard, &da) &&
                        GetFileAttributesExW(items_[ib].parsingName.c_str(), GetFileExInfoStandard, &db))
                    {
                        int timeCmp = CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime);
                        if (timeCmp != 0) cmp = timeCmp;
                        else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                    }
                    else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                return ascending ? (cmp < 0) : (cmp > 0);
            });

        w.itemKeys = std::move(keys);
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    else if (w.type == DesktopWidgetType::Collection)
    {
        std::vector<std::wstring> keys;
        std::unordered_set<std::wstring> seen;
        for (const auto& rawKey : w.itemKeys)
        {
            std::wstring nk = ToUpperInvariant(rawKey);
            if (seen.insert(nk).second)
                keys.push_back(rawKey);
        }

        std::sort(keys.begin(), keys.end(),
            [this, mode, ascending](const std::wstring& ka, const std::wstring& kb) {
                size_t ia = FindItemIndexByKey(ka);
                size_t ib = FindItemIndexByKey(kb);
                if (ia == static_cast<size_t>(-1) || ib == static_cast<size_t>(-1)) return false;
                int cmp = 0;
                if (mode == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                else if (mode == 1)
                {
                    cmp = _wcsicmp(items_[ia].typeName.c_str(), items_[ib].typeName.c_str());
                    if (cmp == 0) cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                else if (mode == 2)
                {
                    WIN32_FILE_ATTRIBUTE_DATA da{}, db{};
                    if (GetFileAttributesExW(items_[ia].parsingName.c_str(), GetFileExInfoStandard, &da) &&
                        GetFileAttributesExW(items_[ib].parsingName.c_str(), GetFileExInfoStandard, &db))
                    {
                        int timeCmp = CompareFileTime(&da.ftLastWriteTime, &db.ftLastWriteTime);
                        if (timeCmp != 0) cmp = timeCmp;
                        else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                    }
                    else cmp = _wcsicmp(items_[ia].name.c_str(), items_[ib].name.c_str());
                }
                return ascending ? (cmp < 0) : (cmp > 0);
            });

        w.itemKeys = std::move(keys);
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

/**
 * @brief 更新所有桌面项的剪切状态（从剪贴板读取 DROPEFFECT_MOVE）。
 */
inline void DesktopApp::UpdateCutState()
{
    std::unordered_set<std::wstring> clipCutPaths;

    ComPtr<IDataObject> clipObj;
    if (SUCCEEDED(OleGetClipboard(&clipObj)) && clipObj)
    {
        CLIPFORMAT cfPreferred = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        FORMATETC fmtPref{ cfPreferred, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM medPref{};
        bool isMove = false;

        if (SUCCEEDED(clipObj->GetData(&fmtPref, &medPref)) && medPref.hGlobal)
        {
            DWORD* pEffect = static_cast<DWORD*>(GlobalLock(medPref.hGlobal));
            if (pEffect)
            {
                if (*pEffect & DROPEFFECT_MOVE)
                    isMove = true;
                GlobalUnlock(medPref.hGlobal);
            }
            ReleaseStgMedium(&medPref);
        }

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
                        clipCutPaths.insert(ToUpperInvariant(path));
                }
                ReleaseStgMedium(&medDrop);
            }
        }
    }

    for (auto& item : items_)
    {
        item.isCut = false;
        if (item.desktopIconClsid.empty() == false) continue;
        wchar_t path[MAX_PATH]{};
        if (SHGetPathFromIDListW(item.absolutePidl.get(), path))
        {
            if (clipCutPaths.contains(ToUpperInvariant(path)))
                item.isCut = true;
        }
    }

    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping)
            continue;
        for (auto& entry : widget.folderEntries)
        {
            entry.isCut = false;
            if (!entry.fullPath.empty() &&
                clipCutPaths.contains(ToUpperInvariant(entry.fullPath)))
                entry.isCut = true;
        }
    }
    if (dockFolderPopupOpen_)
    {
        for (auto& entry :
             dockFolderPopupWidget_.
                folderEntries)
        {
            entry.isCut = false;
            if (!entry.fullPath.empty() &&
                clipCutPaths.contains(
                    ToUpperInvariant(
                        entry.fullPath)))
                entry.isCut = true;
        }
    }
}

// ── Shell 变更通知 ──────────────────────────────────────────

/**
 * @brief 注册 Shell 变更通知（文件创建、删除、重命名、属性变更等），用于实时刷新桌面。
 */
inline void DesktopApp::RegisterShellChangeNotifications()
{
    if (shellChangeRegId_ != 0)
    {
        SHChangeNotifyDeregister(shellChangeRegId_);
        shellChangeRegId_ = 0;
    }
    SHChangeNotifyEntry entries[2]{};
    entries[0].pidl = desktopPidl_.get();
    entries[0].fRecursive = FALSE;
    if (!recycleBinPidl_.get())
    {
        PIDLIST_ABSOLUTE rbPidl = nullptr;
        if (SUCCEEDED(SHGetSpecialFolderLocation(nullptr, CSIDL_BITBUCKET, &rbPidl)))
            recycleBinPidl_.reset(rbPidl);
    }
    int entryCount = 1;
    if (recycleBinPidl_.get())
    {
        entries[1].pidl = recycleBinPidl_.get();
        entries[1].fRecursive = TRUE;
        entryCount = 2;
    }
    shellChangeRegId_ = SHChangeNotifyRegister(hwnd_,
        SHCNRF_ShellLevel | SHCNRF_InterruptLevel | SHCNRF_NewDelivery,
        SHCNE_CREATE | SHCNE_DELETE | SHCNE_MKDIR | SHCNE_RMDIR |
        SHCNE_RENAMEITEM | SHCNE_RENAMEFOLDER | SHCNE_UPDATEITEM |
        SHCNE_UPDATEDIR | SHCNE_ATTRIBUTES | SHCNE_ASSOCCHANGED,
        kShellChangeMessage, entryCount, entries);
}

// ── 过滤与键值 ───────────────────────────────────────────────

/**
 * @brief 获取稳定的布局键值，优先级：桌面图标 CLSID > 文件路径 > 解析名称。
 * @param pidl 绝对 PIDL。
 * @param parsingName 解析名称。
 * @param desktopIconClsid 桌面图标 CLSID。
 * @return 规范化为大写的布局键。
 */
inline std::wstring DesktopApp::GetStableLayoutKey(
    PCIDLIST_ABSOLUTE pidl,
    const std::wstring& parsingName,
    const std::wstring& desktopIconClsid)
{
    if (!desktopIconClsid.empty())
        return ToUpperInvariant(desktopIconClsid);

    wchar_t path[MAX_PATH]{};
    if (SHGetPathFromIDListW(pidl, path) && path[0] != L'\0')
        return ToUpperInvariant(path);

    return ToUpperInvariant(parsingName);
}

/**
 * @brief 给快捷方式的位图左下角绘制小箭头图标。
 * @param bitmap 目标位图。
 * @param bitmapSize 位图尺寸。
 */
inline void DesktopApp::ApplyShortcutArrowToBitmap(HBITMAP bitmap, SIZE bitmapSize)
{
    if (!bitmap) return;
    SHSTOCKICONINFO sii{};
    sii.cbSize = sizeof(sii);
    if (FAILED(SHGetStockIconInfo(SIID_LINK, SHGSI_ICON, &sii)) || !sii.hIcon)
        return;
    HDC hdc = CreateCompatibleDC(nullptr);
    HBITMAP oldBmp = static_cast<HBITMAP>(SelectObject(hdc, bitmap));
    int arrowSz = static_cast<int>(bitmapSize.cy * 30.0 / 64.0 + 0.5);
    if (arrowSz < 10) arrowSz = 10;
    int arrowX = static_cast<int>(bitmapSize.cx * 5.0 / 64.0 + 0.5);
    int arrowY = bitmapSize.cy - arrowSz;
    DrawIconEx(hdc, arrowX, arrowY, sii.hIcon, arrowSz, arrowSz, 0, nullptr, DI_NORMAL);
    SelectObject(hdc, oldBmp);
    DeleteDC(hdc);
    DestroyIcon(sii.hIcon);
}

// ── 布局持久化 ──────────────────────────────────────────────

/**
 * @brief 获取布局文件的完整路径（exe\data 下的 SnowDesktop.layout.json）。
 * @return 布局文件路径。
 */
inline std::wstring DesktopApp::GetLayoutPath() const
{
    return GetDataFilePath(L"SnowDesktop.layout.json");
}

/**
 * @brief 从 JSON 文本中解析保存的页面信息（ID、行数、列数）。
 * @param text JSON 格式的布局文本。
 */
inline void DesktopApp::LoadSavedPagesFromJson(const std::string& text)
{
    size_t pagesName = text.find("\"pages\"");
    if (pagesName == std::string::npos) return;

    size_t arrayStart = text.find('[', pagesName);
    size_t arrayEnd = text.find(']', arrayStart == std::string::npos ? pagesName : arrayStart + 1);
    if (arrayStart == std::string::npos || arrayEnd == std::string::npos || arrayEnd <= arrayStart) return;

    size_t pos = arrayStart + 1;
    while ((pos = text.find('{', pos)) != std::string::npos && pos < arrayEnd)
    {
        size_t objectEnd = text.find('}', pos);
        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;

        std::string objectText = text.substr(pos, objectEnd - pos + 1);
        std::string pageUtf8;
        if (ReadJsonStringField(objectText, "id", pageUtf8))
        {
            std::wstring pageId = Utf8ToWide(pageUtf8);
            if (pageId == kDockPageId)
            {
                pos = objectEnd + 1;
                continue;
            }
            if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
                savedPageIds_.push_back(pageId);
            int columns = 0, rows = 0;
            if (ReadJsonIntField(objectText, "columns", columns) && columns > 0)
                savedPageColumns_[pageId] = columns;
            if (ReadJsonIntField(objectText, "rows", rows) && rows > 0)
                savedPageRows_[pageId] = rows;
        }
        pos = objectEnd + 1;
    }
}

/**
 * @brief 记录页面 ID 到已保存页面列表（去重）。
 * @param pageId 页面 ID。
 */
inline void DesktopApp::RememberSavedPageId(const std::wstring& pageId)
{
    if (pageId.empty() || pageId == kDockPageId) return;
    if (std::find(savedPageIds_.begin(), savedPageIds_.end(), pageId) == savedPageIds_.end())
        savedPageIds_.push_back(pageId);
}

/**
 * @brief 从布局 JSON 文件加载所有页面、组件和项目的网格位置信息。
 *
 * 解析内容包括：首选监视器、页面 ID/行列数、每个项目的网格位置及组件定义。
 */
inline void DesktopApp::LoadLayoutSlots()
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    dockFolderTargetCache_.clear();
    dockFolderIconIndexCache_.clear();
    struct PreservedFolderEntries
    {
        std::wstring sourceFolderPath;
        std::vector<FolderEntry> entries;
    };
    std::unordered_map<std::wstring, PreservedFolderEntries> preservedFolderEntries;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FolderMapping || widget.id.empty())
            continue;
        PreservedFolderEntries preserved;
        preserved.sourceFolderPath = widget.sourceFolderPath;
        preserved.entries = std::move(widget.folderEntries);
        preservedFolderEntries.emplace(ToUpperInvariant(widget.id), std::move(preserved));
    }
    auto releasePreservedEntries = [this, &preservedFolderEntries]()
    {
        for (auto& [id, preserved] : preservedFolderEntries)
        {
            for (auto& entry : preserved.entries)
            {
                if (entry.iconBitmap)
                    EraseD2DIconCacheForBitmap(entry.iconBitmap);
            }
        }
        preservedFolderEntries.clear();
    };

    layoutRecords_.clear();
    widgets_.clear();
    dockEntries_.clear();
    savedPageIds_.clear();
    savedPageColumns_.clear();
    savedPageRows_.clear();

    std::ifstream file(GetLayoutPath(), std::ios::binary);
    if (!file)
    {
        releasePreservedEntries();
        return;
    }
    std::stringstream buf;
    buf << file.rdbuf();
    std::string text = buf.str();

    int widgetTitleSchemaVersion = 0;
    ReadJsonIntField(text, "widgetTitleSchemaVersion", widgetTitleSchemaVersion);
    const bool hasTrustedWidgetTitleMode = widgetTitleSchemaVersion >= 1;

    std::string firstPageMonitorUtf8;
    if (ReadJsonStringField(text, "firstPageMonitor", firstPageMonitorUtf8))
        firstPageMonitorId_ = Utf8ToWide(firstPageMonitorUtf8);

    std::string lastPageMonitorUtf8;
    if (ReadJsonStringField(text, "lastPageMonitor", lastPageMonitorUtf8))
        lastPageMonitorId_ = Utf8ToWide(lastPageMonitorUtf8);

    bool loadedDockEnabled = false;
    if (ReadJsonBoolField(text, "dockEnabled", loadedDockEnabled))
        generalSettings_.dockEnabled = loadedDockEnabled;

    float loadedFontSize = 0;
    if (ReadJsonFloatField(text, "itemFontSize", loadedFontSize) &&
        loadedFontSize >= 10.0f && loadedFontSize <= 24.0f)
        itemFontSize_ = loadedFontSize;

    float loadedFontWeight = 0;
    if (ReadJsonFloatField(text, "itemFontWeight", loadedFontWeight) &&
        loadedFontWeight >= 100 && loadedFontWeight <= 950)
        itemFontWeight_ = static_cast<DWRITE_FONT_WEIGHT>(static_cast<int>(loadedFontWeight));

    float loadedIconSpacing = 0;
    if (ReadJsonFloatField(text, "iconSpacing", loadedIconSpacing) &&
        loadedIconSpacing >= 0.5f && loadedIconSpacing <= 2.0f)
        iconSpacingScale_ = loadedIconSpacing;

    int loadedShortcutArrowMode = 0;
    if (ReadJsonIntField(text, "shortcutArrowMode", loadedShortcutArrowMode))
        shortcutArrowMode_ = std::clamp(loadedShortcutArrowMode, 0, 2);

    bool loadedIconBeautify = false;
    if (ReadJsonBoolField(text, "iconBeautifyEnabled", loadedIconBeautify))
        iconBeautifyEnabled_ = loadedIconBeautify;

    int loadedIconBeautifyMode = 0;
    if (ReadJsonIntField(text, "iconBeautifyMode", loadedIconBeautifyMode))
        iconBeautifyMode_ = std::clamp(loadedIconBeautifyMode, 0, 1);

    float loadedIconBeautifyFloat = 0.0f;
    if (ReadJsonFloatField(text, "iconBeautifyBgOpacity", loadedIconBeautifyFloat))
        iconBeautifyBgOpacity_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    bool loadedIconBeautifyGradient = false;
    if (ReadJsonBoolField(text, "iconBeautifyGradientEnabled", loadedIconBeautifyGradient))
        iconBeautifyGradientEnabled_ = loadedIconBeautifyGradient;
    int loadedIconBeautifyDirection = 0;
    if (ReadJsonIntField(text, "iconBeautifyGradientDirection", loadedIconBeautifyDirection))
        iconBeautifyGradientDirection_ = std::clamp(loadedIconBeautifyDirection, 0, 3);
    if (ReadJsonFloatField(text, "iconBeautifyBgStartR", loadedIconBeautifyFloat))
        iconBeautifyBgStartR_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgStartG", loadedIconBeautifyFloat))
        iconBeautifyBgStartG_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgStartB", loadedIconBeautifyFloat))
        iconBeautifyBgStartB_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgEndR", loadedIconBeautifyFloat))
        iconBeautifyBgEndR_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgEndG", loadedIconBeautifyFloat))
        iconBeautifyBgEndG_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);
    if (ReadJsonFloatField(text, "iconBeautifyBgEndB", loadedIconBeautifyFloat))
        iconBeautifyBgEndB_ = std::clamp(loadedIconBeautifyFloat, 0.0f, 1.0f);

    LoadSavedPagesFromJson(text);

    size_t pos = 0;
    while ((pos = text.find("\"key\"", pos)) != std::string::npos)
    {
        size_t objStart = text.rfind('{', pos);
        if (objStart == std::string::npos) break;
        size_t objEnd = objStart + 1;
        int depth = 1;
        for (size_t i = objStart + 1; i < text.size() && depth > 0; ++i)
        {
            if (text[i] == '{') ++depth;
            else if (text[i] == '}') --depth;
            objEnd = i;
        }
        if (depth != 0) break;

        std::string objText = text.substr(objStart, objEnd - objStart + 1);
        std::string keyUtf8;
        if (!ReadJsonStringField(objText, "key", keyUtf8)) { pos = objEnd + 1; continue; }

        LayoutRecord record;
        std::string pageUtf8;
        int x = 0, y = 0, w = 1, h = 1;
        if (ReadJsonStringField(objText, "page", pageUtf8) &&
            ReadJsonIntField(objText, "x", x) && ReadJsonIntField(objText, "y", y))
        {
            record.cell.pageId = Utf8ToWide(pageUtf8);
            record.cell.column = x;
            record.cell.row = y;
            RememberSavedPageId(record.cell.pageId);
            ReadJsonIntField(objText, "w", w);
            ReadJsonIntField(objText, "h", h);
            record.span.columns = std::max(1, w);
            record.span.rows = std::max(1, h);
            record.hasGrid = true;
            record.legacySlot = SlotFromCell(gridPages_, record.cell);
        }
        layoutRecords_[ToUpperInvariant(Utf8ToWide(keyUtf8))] = record;
        pos = objEnd + 1;
    }

    // Load widgets
    {
        size_t widgetsName = text.find("\"widgets\"");
        if (widgetsName != std::string::npos)
        {
            size_t arrayStart = text.find('[', widgetsName);
            if (arrayStart != std::string::npos)
            {
                size_t arrayEnd = FindJsonArrayEnd(text, arrayStart);
                if (arrayEnd != std::string::npos && arrayEnd > arrayStart)
                {
                    size_t wp = arrayStart + 1;
                    while ((wp = text.find('{', wp)) != std::string::npos && wp < arrayEnd)
                    {
                        size_t objectEnd = FindJsonObjectEnd(text, wp);
                        if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
                        std::string obj = text.substr(wp, objectEnd - wp + 1);
                        std::string idUtf8, typeUtf8, titleUtf8, customTitleUtf8,
                            titleModeUtf8, sourceUtf8, packageIdUtf8, scriptUtf8,
                            activeCategoryUtf8, pageUtf8;
                        int x = 0, y = 0, w = 1, h = 1, scrollOffset = 0,
                            tabScrollOffset = 0,
                            folderSortMode =
                                snowdesktop::folder_sort_rules::kManual;
                        bool autoCollect = false, listMode = false, dateHeaders = false,
                            showFileCategories = false, showSearchBox = false,
                            showOnHoverOnly = false, privacyMode = false,
                            scrollContainerMode = false, showTitle = false,
                            bottomBarHover = false, userRenamed = false,
                            folderSortAscending = true;
                        if (!ReadJsonStringField(obj, "id", idUtf8) ||
                            !ReadJsonStringField(obj, "page", pageUtf8) ||
                            !ReadJsonIntField(obj, "x", x) ||
                            !ReadJsonIntField(obj, "y", y))
                        {
                            wp = objectEnd + 1;
                            continue;
                        }
                        ReadJsonStringField(obj, "type", typeUtf8);
                        ReadJsonStringField(obj, "title", titleUtf8);
                        const bool hasCustomTitle =
                            ReadJsonStringField(obj, "customTitle", customTitleUtf8);
                        const bool hasTitleMode =
                            ReadJsonStringField(obj, "titleMode", titleModeUtf8);
                        ReadJsonStringField(obj, "sourceFolderPath", sourceUtf8);
                        ReadJsonStringField(obj, "packageId", packageIdUtf8);
                        ReadJsonStringField(obj, "scriptPath", scriptUtf8);
                        if (scriptUtf8.empty())
                            ReadJsonStringField(obj, "legacyScriptPath", scriptUtf8);
                        ReadJsonStringField(obj, "activeCategory", activeCategoryUtf8);
                        ReadJsonIntField(obj, "w", w);
                        ReadJsonIntField(obj, "h", h);
                        ReadJsonIntField(obj, "scrollOffset", scrollOffset);
ReadJsonIntField(obj, "tabScrollOffset", tabScrollOffset);
                        ReadJsonIntField(
                            obj, "folderSortMode",
                            folderSortMode);
                        ReadJsonBoolField(
                            obj, "folderSortAscending",
                            folderSortAscending);
                        ReadJsonBoolField(obj, "autoCollect", autoCollect);
                        ReadJsonBoolField(obj, "listMode", listMode);
                        ReadJsonBoolField(obj, "dateHeaders", dateHeaders);
                        ReadJsonBoolField(obj, "showFileCategories", showFileCategories);
                        ReadJsonBoolField(obj, "showSearchBox", showSearchBox);
                        ReadJsonBoolField(obj, "showOnHoverOnly", showOnHoverOnly);
                        ReadJsonBoolField(obj, "privacyMode", privacyMode);
                        ReadJsonBoolField(obj, "scrollContainerMode", scrollContainerMode);

                        DesktopWidget widget;
                        widget.id = Utf8ToWide(idUtf8);
                        widget.type = WidgetTypeFromJson(Utf8ToWide(typeUtf8));
                        widget.sourceFolderPath = Utf8ToWide(sourceUtf8);
                        widget.packageId = Utf8ToWide(packageIdUtf8);
                        if (widget.packageId.empty())
                            widget.legacyScriptPath = Utf8ToWide(scriptUtf8);
                        if (widget.packageId.empty() &&
                            !widget.legacyScriptPath.empty())
                        {
                            if (const auto migrated =
                                WidgetEngine::ResolveLegacyWidgetPackage(
                                    widget.legacyScriptPath))
                            {
                                widget.packageId = *migrated;
                                widget.legacyScriptPath.clear();
                                legacyWidgetLayoutMigrationPending_ = true;
                            }
                        }
                        if (titleUtf8.empty())
                        {
                            if (widget.type == DesktopWidgetType::LuaScript)
                            {
                                widget.title = WidgetEngine::GetWidgetDisplayName(widget.packageId);
                                if (widget.title.empty())
                                    widget.title = !widget.legacyScriptPath.empty()
                                        ? widget.legacyScriptPath : widget.packageId;
                            }
                            else if (widget.type == DesktopWidgetType::Guide)
                            {
                                widget.title = _LW("app.guide.title");
                            }
                            else if (widget.type == DesktopWidgetType::CollectionGroup)
                            {
                                widget.title = _LW("widget.collection_group");
                            }
                            else if (widget.type == DesktopWidgetType::FileGroup)
                            {
                                widget.title = _LW("widget.file_group");
                            }
                            else
                            {
                                widget.title = widget.type == DesktopWidgetType::FileCategories ? _LW("widget.desktop_files")
                                    : widget.type == DesktopWidgetType::FolderMapping ? _LW("widget.folder_mapping")
                                    : _LW("widget.collection");
                            }
                        }
                        else
                        {
                            widget.title = Utf8ToWide(titleUtf8);
                        }
                        widget.gridCell.pageId = Utf8ToWide(pageUtf8);
                        widget.gridCell.column = x;
                        widget.gridCell.row = y;
                        widget.gridSpan.columns = std::max(1, w);
                        widget.gridSpan.rows = std::max(1, h);
                        widget.autoCollect = autoCollect;
                        widget.listMode = listMode;
                        widget.dateHeaders =
                            widget.type == DesktopWidgetType::CollectionGroup
                                ? false
                                : dateHeaders;
                        widget.showFileCategories = showFileCategories;
                        widget.showSearchBox = showSearchBox;
                        widget.showOnHoverOnly = showOnHoverOnly;
                        widget.privacyMode = privacyMode;
                        widget.scrollContainerMode = scrollContainerMode;
                        showTitle = widget.type != DesktopWidgetType::LuaScript;
                        bottomBarHover = (widget.type == DesktopWidgetType::Collection ||
                            widget.type == DesktopWidgetType::LuaScript ||
                            widget.type == DesktopWidgetType::Guide);
                        ReadJsonBoolField(obj, "showTitle", showTitle);
                        ReadJsonBoolField(obj, "bottomBarHover", bottomBarHover);
                        const bool hasUserRenamed =
                            ReadJsonBoolField(obj, "userRenamed", userRenamed);
                        widget.showTitle = showTitle;
                        widget.bottomBarHover = bottomBarHover;
                        if (hasTrustedWidgetTitleMode && hasTitleMode)
                        {
                            if (titleModeUtf8 == "custom")
                            {
                                widget.customTitle = Utf8ToWide(
                                    hasCustomTitle ? customTitleUtf8 : titleUtf8);
                                widget.title = widget.customTitle;
                            }
                            else
                            {
                                widget.customTitle.clear();
                            }
                        }
                        else if (hasUserRenamed && userRenamed)
                        {
                            // Legacy layouts only set this flag reliably when it
                            // is true. Older versions wrote false even for
                            // user-named widgets, so false must still go through
                            // title-content inference below.
                            widget.customTitle = Utf8ToWide(
                                hasCustomTitle ? customTitleUtf8 : titleUtf8);
                            widget.title = widget.customTitle;
                        }
                        else if (!widget.title.empty())
                        {
                            bool usesDefaultTitle = false;
                            switch (widget.type)
                            {
                            case DesktopWidgetType::Collection:
                                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                                    L10N_KEY("widget.collection"), widget.title);
                                break;
                            case DesktopWidgetType::CollectionGroup:
                                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                                    L10N_KEY("widget.collection_group"), widget.title);
                                break;
                            case DesktopWidgetType::FileGroup:
                                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                                    L10N_KEY("widget.file_group"), widget.title);
                                break;
                            case DesktopWidgetType::FileCategories:
                                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                                    L10N_KEY("widget.desktop_files"), widget.title);
                                break;
                            case DesktopWidgetType::Guide:
                                usesDefaultTitle = Locale::Instance().IsTranslationValue(
                                    L10N_KEY("app.guide.title"), widget.title);
                                break;
                            case DesktopWidgetType::LuaScript:
                                usesDefaultTitle = WidgetEngine::IsWidgetDefaultName(
                                    widget.packageId, widget.title);
                                break;
                            case DesktopWidgetType::FolderMapping:
                            default:
                                break;
                            }
                            if (!usesDefaultTitle)
                                widget.customTitle = widget.title;
                            else if (widget.type == DesktopWidgetType::LuaScript &&
                                widget.title != WidgetEngine::GetWidgetDisplayName(
                                    widget.packageId))
                                widget.scriptTitle = widget.title;
                        }
                        widget.userRenamed = !widget.customTitle.empty();
                        if (widget.customTitle.empty() &&
                            widget.type == DesktopWidgetType::LuaScript &&
                            widget.scriptTitle.empty() && !widget.title.empty() &&
                            !WidgetEngine::IsWidgetDefaultName(
                                widget.packageId, widget.title))
                        {
                            widget.scriptTitle = widget.title;
                        }
                        widget.scrollOffset = std::max(0, scrollOffset);
                        widget.tabScrollOffset =
                            std::max(0, tabScrollOffset);
                        widget.folderSortMode =
                            snowdesktop::folder_sort_rules::
                                NormalizeMode(
                                    folderSortMode);
                        widget.folderSortAscending =
                            folderSortAscending;
                        widget.activeCategoryId = Utf8ToWide(activeCategoryUtf8);
                        ReadJsonStringArrayField(obj, "items", widget.itemKeys);
                        ReadJsonStringArrayField(obj, "childWidgets", widget.childWidgetIds);
                        ConfigureWidgetGridLimits(widget);
                        {
                            std::unordered_set<std::wstring> seen;
                            std::vector<std::wstring> unique;
                            for (auto& key : widget.itemKeys)
                            {
                                key = ToUpperInvariant(key);
                                if (!key.empty() && seen.insert(key).second)
                                    unique.push_back(key);
                            }
                            widget.itemKeys = std::move(unique);
                        }

                        widgets_.push_back(std::move(widget));
                        if (widgets_.back().type == DesktopWidgetType::FolderMapping &&
                            !widgets_.back().sourceFolderPath.empty())
                        {
                            auto preservedIt = preservedFolderEntries.find(
                                ToUpperInvariant(widgets_.back().id));
                            if (preservedIt != preservedFolderEntries.end() &&
                                _wcsicmp(preservedIt->second.sourceFolderPath.c_str(),
                                    widgets_.back().sourceFolderPath.c_str()) == 0)
                            {
                                widgets_.back().folderEntries =
                                    std::move(preservedIt->second.entries);
                                preservedFolderEntries.erase(preservedIt);
                            }
                            EnumerateFolderMappingEntries(widgets_.back());
                        }
                        wp = objectEnd + 1;
                    }
                }
            }
        }
    }

    // Normalize grouped-widget membership after every referenced widget is loaded.
    {
        std::unordered_set<std::wstring> claimedCollections;
        std::unordered_set<std::wstring> claimedFileSources;
        for (auto& group : widgets_)
        {
            if (group.type == DesktopWidgetType::CollectionGroup)
            {
                std::vector<std::wstring> validChildren;
                for (const auto& childId : group.childWidgetIds)
                {
                    const size_t childIndex = FindWidgetIndexById(childId);
                    if (childIndex >= widgets_.size() ||
                        widgets_[childIndex].type != DesktopWidgetType::Collection ||
                        !claimedCollections.insert(childId).second)
                        continue;
                    validChildren.push_back(childId);
                }
                group.childWidgetIds = std::move(validChildren);
                group.activeCategoryId =
                    snowdesktop::collection_group_rules::ResolveActiveItem(
                        group.childWidgetIds, group.activeCategoryId);
                continue;
            }
            if (group.type == DesktopWidgetType::FileGroup)
            {
                std::vector<std::wstring> validChildren;
                for (const auto& childId : group.childWidgetIds)
                {
                    const size_t childIndex = FindWidgetIndexById(childId);
                    if (childIndex >= widgets_.size())
                        continue;
                    const DesktopWidgetType type =
                        widgets_[childIndex].type;
                    if ((type != DesktopWidgetType::FileCategories &&
                         type != DesktopWidgetType::FolderMapping) ||
                        !claimedFileSources.insert(childId).second)
                        continue;
                    validChildren.push_back(childId);
                }
                group.childWidgetIds = std::move(validChildren);
                group.activeCategoryId =
                    snowdesktop::collection_group_rules::ResolveActiveItem(
                        group.childWidgetIds, group.activeCategoryId);
                continue;
            }
            group.childWidgetIds.clear();
        }
    }

    // Ensure widget-owned items have layout records (they're not in the JSON items array)
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    for (auto& w : widgets_)
    {
        for (auto& key : w.itemKeys)
        {
            auto upper = ToUpperInvariant(key);
            if (layoutRecords_.count(upper) == 0)
            {
                LayoutRecord rec;
                rec.cell = w.gridCell;
                rec.span = {1, 1};
                rec.hasGrid = true;
                rec.legacySlot = SlotFromCell(gridPages_, w.gridCell);
                layoutRecords_[upper] = rec;
            }
        }
    }

    // Load Dock references. "ref" intentionally differs from desktop item
    // "key" so the legacy item scanner cannot mistake Dock entries for layout records.
    {
        size_t dockName = text.find("\"dockEntries\"");
        if (dockName != std::string::npos)
        {
            size_t arrayStart = text.find('[', dockName);
            size_t arrayEnd = arrayStart == std::string::npos
                ? std::string::npos : FindJsonArrayEnd(text, arrayStart);
            size_t dp = arrayStart == std::string::npos ? 0 : arrayStart + 1;
            while (arrayEnd != std::string::npos &&
                (dp = text.find('{', dp)) != std::string::npos && dp < arrayEnd)
            {
                size_t objectEnd = FindJsonObjectEnd(text, dp);
                if (objectEnd == std::string::npos || objectEnd > arrayEnd) break;
                std::string object = text.substr(dp, objectEnd - dp + 1);
                std::string typeUtf8, referenceUtf8;
                bool keepOnDesktop = false;
                bool folderSortAscending = true;
                int folderSortMode =
                    snowdesktop::folder_sort_rules::kName;
                std::vector<std::wstring> folderItemKeys;
                if (ReadJsonStringField(object, "type", typeUtf8) &&
                    ReadJsonStringField(object, "ref", referenceUtf8))
                {
                    ReadJsonBoolField(object, "keepOnDesktop", keepOnDesktop);
                    ReadJsonIntField(
                        object, "folderSortMode",
                        folderSortMode);
                    ReadJsonBoolField(
                        object, "folderSortAscending",
                        folderSortAscending);
                    ReadJsonStringArrayField(
                        object, "folderItems",
                        folderItemKeys);
                    DockEntry entry;
                    if (typeUtf8 == "collection")
                        entry.type = DockEntryType::Collection;
                    else if (typeUtf8 == "folderMapping")
                        entry.type = DockEntryType::FolderMapping;
                    else
                        entry.type = DockEntryType::DesktopItem;
                    entry.reference = Utf8ToWide(referenceUtf8);
                    if (entry.type == DockEntryType::DesktopItem)
                        entry.reference = ToUpperInvariant(entry.reference);
                    entry.keepOnDesktop = keepOnDesktop;
                    entry.folderSortMode =
                        snowdesktop::folder_sort_rules::
                            NormalizeMode(
                                folderSortMode);
                    entry.folderSortAscending =
                        folderSortAscending;
                    entry.folderItemKeys =
                        std::move(folderItemKeys);
                    if (!entry.reference.empty() &&
                        !(entry.type ==
                                DockEntryType::
                                    DesktopItem &&
                            snowdesktop::
                                shell_item_visibility::
                                    IsAlwaysHidden(
                                        entry.reference)))
                        dockEntries_.push_back(
                            std::move(entry));
                }
                dp = objectEnd + 1;
            }
        }
    }

    std::erase_if(dockEntries_, [&](const DockEntry& entry) {
        if (entry.type != DockEntryType::Collection &&
            entry.type != DockEntryType::FolderMapping)
            return false;
        const size_t widgetIndex =
            FindWidgetIndexById(entry.reference);
        if (widgetIndex >= widgets_.size())
            return true;
        if (entry.type ==
                DockEntryType::FolderMapping)
        {
            for (auto& group : widgets_)
            {
                if (group.type !=
                        DesktopWidgetType::FileGroup)
                    continue;
                std::erase(
                    group.childWidgetIds,
                    entry.reference);
                group.activeCategoryId =
                    snowdesktop::
                        collection_group_rules::
                            ResolveActiveItem(
                                group.childWidgetIds,
                                group.activeCategoryId);
            }
            return false;
        }
        return IsGroupedWidget(
            widgets_[widgetIndex]);
    });
    NormalizeDockRecycleBinPosition();

    // Dock coordinates are not desktop pages. Migrate both current Dock
    // entries and layouts previously polluted by a normalized Dock pseudo-page.
    std::unordered_set<std::wstring> legacyDockPageCandidates;
    for (auto& entry : dockEntries_)
    {
        if (entry.type == DockEntryType::Collection ||
            entry.type == DockEntryType::FolderMapping)
        {
            entry.keepOnDesktop = false;
            size_t widgetIndex = FindWidgetIndexById(entry.reference);
            if (widgetIndex >= widgets_.size()) continue;
            DesktopWidget& widget = widgets_[widgetIndex];
            if (!widget.gridCell.pageId.empty() && widget.gridCell.pageId != kDockPageId)
                legacyDockPageCandidates.insert(widget.gridCell.pageId);
            widget.gridCell = { kDockPageId, 0, 0 };
            for (const auto& key : widget.itemKeys)
            {
                auto record = layoutRecords_.find(ToUpperInvariant(key));
                if (record == layoutRecords_.end()) continue;
                if (!record->second.cell.pageId.empty() &&
                    record->second.cell.pageId != kDockPageId)
                    legacyDockPageCandidates.insert(record->second.cell.pageId);
                record->second.cell = { kDockPageId, 0, 0 };
                record->second.span = { 1, 1 };
                record->second.hasGrid = true;
            }
            continue;
        }

        if (entry.keepOnDesktop) continue;
        auto record = layoutRecords_.find(ToUpperInvariant(entry.reference));
        if (record == layoutRecords_.end()) continue;
        if (!record->second.cell.pageId.empty() &&
            record->second.cell.pageId != kDockPageId)
            legacyDockPageCandidates.insert(record->second.cell.pageId);
        record->second.cell = { kDockPageId, 0, 0 };
        record->second.span = { 1, 1 };
        record->second.hasGrid = true;
    }

    std::unordered_set<std::wstring> widgetOwnedKeys;
    for (const auto& widget : widgets_)
        for (const auto& key : widget.itemKeys)
            widgetOwnedKeys.insert(ToUpperInvariant(key));

    for (const auto& candidate : legacyDockPageCandidates)
    {
        if (candidate.empty() || candidate == kDockPageId) continue;
        bool hasDesktopContent = std::any_of(widgets_.begin(), widgets_.end(),
            [&](const DesktopWidget& widget) {
                return widget.gridCell.pageId == candidate;
            });
        if (!hasDesktopContent)
        {
            hasDesktopContent = std::any_of(layoutRecords_.begin(), layoutRecords_.end(),
                [&](const auto& pair) {
                    return !widgetOwnedKeys.contains(pair.first) &&
                        pair.second.hasGrid && pair.second.cell.pageId == candidate;
                });
        }
        if (hasDesktopContent) continue;
        std::erase(savedPageIds_, candidate);
        savedPageColumns_.erase(candidate);
        savedPageRows_.erase(candidate);
    }

    {
        std::vector<std::wstring> savedOrder;
        ReadJsonStringArrayField(text, "navTabOrder", savedOrder);
        navTabOrder_ = std::move(savedOrder);
    }
    EnsureNavTabOrder();
    NormalizePageIds();
    releasePreservedEntries();
}

/**
 * @brief 将所有项目、组件和页面的网格布局信息持久化到 JSON 文件。
 *
 * 写入内容包括：首选监视器、页面列表、桌面项（排除组件所属项）以及所有组件的完整定义。
 */
inline void DesktopApp::SaveLayoutSlots()
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    layoutRecords_.clear();
    for (const auto& item : items_)
    {
        if (!item.parsingName.empty())
        {
            RememberSavedPageId(item.gridCell.pageId);
            LayoutRecord record;
            record.cell = item.gridCell;
            record.span = item.gridSpan;
            record.hasGrid = true;
            record.legacySlot = item.slot;
            layoutRecords_[item.layoutKey] = record;
        }
    }

    std::vector<const DesktopItem*> sorted;
    for (const auto& item : items_) sorted.push_back(&item);
    std::sort(sorted.begin(), sorted.end(), [](const DesktopItem* a, const DesktopItem* b) {
        if (a->gridCell.pageId != b->gridCell.pageId) return a->gridCell.pageId < b->gridCell.pageId;
        if (a->gridCell.column != b->gridCell.column) return a->gridCell.column < b->gridCell.column;
        return a->gridCell.row < b->gridCell.row;
    });

    for (const auto& page : gridPages_)
    {
        savedPageColumns_[page.id] = page.columns;
        savedPageRows_[page.id] = page.rows;
    }

    std::vector<std::wstring> pagesToWrite;
    pagesToWrite.reserve(savedPageIds_.size());
    for (const auto& pageId : savedPageIds_)
        if (!pageId.empty() && pageId != kDockPageId)
            pagesToWrite.push_back(pageId);
    if (pagesToWrite.empty() && !gridPages_.empty())
    {
        const GridPage* firstPage = GetFirstPageGridPage();
        if (firstPage) pagesToWrite.push_back(firstPage->id);
    }

    std::ofstream file(GetLayoutPath(), std::ios::binary | std::ios::trunc);
    if (!file) return;

    file << "{\n  \"widgetTitleSchemaVersion\": 1"
         << ",\n  \"firstPageMonitor\": \"" << JsonEscapeUtf8(firstPageMonitorId_)
         << "\",\n  \"lastPageMonitor\": \""  << JsonEscapeUtf8(lastPageMonitorId_)
         << "\",\n  \"dockEnabled\": " << (generalSettings_.dockEnabled ? "true" : "false")
         << ",\n  \"itemFontSize\": " << itemFontSize_
         << ",\n  \"itemFontWeight\": " << static_cast<int>(itemFontWeight_)
         << ",\n  \"iconSpacing\": " << iconSpacingScale_
         << ",\n  \"shortcutArrowMode\": " << shortcutArrowMode_
         << ",\n  \"iconBeautifyEnabled\": " << (iconBeautifyEnabled_ ? "true" : "false")
         << ",\n  \"iconBeautifyMode\": " << iconBeautifyMode_
         << ",\n  \"iconBeautifyBgOpacity\": " << iconBeautifyBgOpacity_
         << ",\n  \"iconBeautifyGradientEnabled\": " << (iconBeautifyGradientEnabled_ ? "true" : "false")
         << ",\n  \"iconBeautifyGradientDirection\": " << iconBeautifyGradientDirection_
         << ",\n  \"iconBeautifyBgStartR\": " << iconBeautifyBgStartR_
         << ",\n  \"iconBeautifyBgStartG\": " << iconBeautifyBgStartG_
         << ",\n  \"iconBeautifyBgStartB\": " << iconBeautifyBgStartB_
         << ",\n  \"iconBeautifyBgEndR\": " << iconBeautifyBgEndR_
         << ",\n  \"iconBeautifyBgEndG\": " << iconBeautifyBgEndG_
         << ",\n  \"iconBeautifyBgEndB\": " << iconBeautifyBgEndB_
         << ",\n  \"pages\": [\n";
    for (size_t i = 0; i < pagesToWrite.size(); ++i)
    {
        const GridPage* page = FindGridPage(gridPages_, pagesToWrite[i]);
        file << "    { \"id\": \"" << JsonEscapeUtf8(pagesToWrite[i]) << "\", \"monitor\": \"";
        file << JsonEscapeUtf8(page != nullptr ? page->monitorId : L"");
        int columns = page != nullptr ? page->columns : 0;
        int rows = page != nullptr ? page->rows : 0;
        if (page == nullptr)
        {
            auto colIt = savedPageColumns_.find(pagesToWrite[i]);
            auto rowIt = savedPageRows_.find(pagesToWrite[i]);
            if (colIt != savedPageColumns_.end()) columns = colIt->second;
            if (rowIt != savedPageRows_.end()) rows = rowIt->second;
        }
        file << "\", \"columns\": " << std::max(1, columns) <<
            ", \"rows\": " << std::max(1, rows) << " }";
        file << (i + 1 == pagesToWrite.size() ? "\n" : ",\n");
    }
    file << "  ],\n  \"items\": [\n";
    // Collect widget-owned keys — items in widgets should not be saved
    // to the desktop items array (they belong to their widget's items list)
    std::unordered_set<std::wstring> widgetOwnedKeys;
    for (auto& w : widgets_)
        for (auto& k : w.itemKeys)
            if (!k.empty())
                widgetOwnedKeys.insert(ToUpperInvariant(k));

    bool firstItem = true;
    for (size_t i = 0; i < sorted.size(); ++i)
    {
        const auto* it = sorted[i];
        if (widgetOwnedKeys.count(ToUpperInvariant(it->layoutKey))) continue;
        if (!firstItem) file << ",\n";
        firstItem = false;
        file << "    { \"key\": \"" << JsonEscapeUtf8(it->layoutKey)
             << "\", \"page\": \"" << JsonEscapeUtf8(it->gridCell.pageId)
             << "\", \"x\": " << it->gridCell.column
             << ", \"y\": " << it->gridCell.row
             << ", \"w\": " << std::max(1, it->gridSpan.columns)
             << ", \"h\": " << std::max(1, it->gridSpan.rows)
             << ", \"slot\": " << it->slot << " }";
    }
    if (!firstItem) file << "\n";
    file << "  ],\n  \"widgets\": [\n";
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        const DesktopWidget& w = widgets_[i];
        const bool hasCustomTitle = !w.customTitle.empty();
        file << "    { \"id\": \"" << JsonEscapeUtf8(w.id)
             << "\", \"type\": \"" << JsonEscapeUtf8(WidgetTypeToJson(w.type))
             << "\", \"title\": \"" << JsonEscapeUtf8(w.title)
             << "\", \"titleMode\": \"" << (hasCustomTitle ? "custom" : "auto")
             << "\", \"customTitle\": \"" << JsonEscapeUtf8(w.customTitle)
             << "\", \"sourceFolderPath\": \"" << JsonEscapeUtf8(w.sourceFolderPath)
             << "\", \"packageId\": \"" << JsonEscapeUtf8(w.packageId)
             << "\", \"legacyScriptPath\": \"" << JsonEscapeUtf8(w.legacyScriptPath)
             << "\", \"activeCategory\": \"" << JsonEscapeUtf8(w.activeCategoryId)
             << "\", \"page\": \"" << JsonEscapeUtf8(w.gridCell.pageId)
             << "\", \"x\": " << w.gridCell.column
             << ", \"y\": " << w.gridCell.row
             << ", \"w\": " << std::max(1, w.gridSpan.columns)
             << ", \"h\": " << std::max(1, w.gridSpan.rows)
             << ", \"autoCollect\": " << (w.autoCollect ? "true" : "false")
             << ", \"listMode\": " << (w.listMode ? "true" : "false")
             << ", \"dateHeaders\": " << (w.dateHeaders ? "true" : "false")
             << ", \"showFileCategories\": " << (w.showFileCategories ? "true" : "false")
             << ", \"showSearchBox\": " << (w.showSearchBox ? "true" : "false")
             << ", \"showOnHoverOnly\": " << (w.showOnHoverOnly ? "true" : "false")
             << ", \"privacyMode\": " << (w.privacyMode ? "true" : "false")
             << ", \"scrollContainerMode\": " << (w.scrollContainerMode ? "true" : "false")
             << ", \"showTitle\": " << (w.showTitle ? "true" : "false")
             << ", \"bottomBarHover\": " << (w.bottomBarHover ? "true" : "false")
             << ", \"userRenamed\": " << (hasCustomTitle ? "true" : "false")
             << ", \"scrollOffset\": " << std::max(0, w.scrollOffset)
             << ", \"tabScrollOffset\": " << std::max(0, w.tabScrollOffset)
             << ", \"folderSortMode\": "
             << snowdesktop::folder_sort_rules::
                    NormalizeMode(w.folderSortMode)
             << ", \"folderSortAscending\": "
             << (w.folderSortAscending ? "true" : "false")
             << ", \"items\": [";
        for (size_t j = 0; j < w.itemKeys.size(); ++j)
        {
            file << "\"" << JsonEscapeUtf8(w.itemKeys[j]) << "\"";
            if (j + 1 != w.itemKeys.size()) file << ", ";
        }
        file << "], \"childWidgets\": [";
        for (size_t j = 0; j < w.childWidgetIds.size(); ++j)
        {
            file << "\"" << JsonEscapeUtf8(w.childWidgetIds[j]) << "\"";
            if (j + 1 != w.childWidgetIds.size()) file << ", ";
        }
        file << "] }";
        file << (i + 1 == widgets_.size() ? "\n" : ",\n");
    }
    file << "  ],\n  \"dockEntries\": [\n";
    for (size_t i = 0; i < dockEntries_.size(); ++i)
    {
        const DockEntry& entry = dockEntries_[i];
        file << "    { \"type\": \""
             << (entry.type == DockEntryType::Collection
                    ? "collection"
                    : (entry.type == DockEntryType::FolderMapping
                        ? "folderMapping" : "item"))
             << "\", \"ref\": \"" << JsonEscapeUtf8(entry.reference)
             << "\", \"keepOnDesktop\": " << (entry.keepOnDesktop ? "true" : "false")
             << ", \"folderSortMode\": "
             << snowdesktop::folder_sort_rules::
                    NormalizeMode(
                        entry.folderSortMode)
             << ", \"folderSortAscending\": "
             << (entry.folderSortAscending
                    ? "true" : "false")
             << ", \"folderItems\": [";
        for (size_t j = 0;
            j < entry.folderItemKeys.size(); ++j)
        {
            file << "\""
                 << JsonEscapeUtf8(
                        entry.folderItemKeys[j])
                 << "\"";
            if (j + 1 !=
                entry.folderItemKeys.size())
                file << ", ";
        }
        file << "] }"
             << (i + 1 == dockEntries_.size()
                    ? "\n" : ",\n");
    }
    file << "  ],\n  \"navTabOrder\": [";
    for (size_t i = 0; i < navTabOrder_.size(); ++i)
    {
        file << "\"" << JsonEscapeUtf8(navTabOrder_[i]) << "\"";
        if (i + 1 != navTabOrder_.size()) file << ", ";
    }
    file << "]\n}\n";
}

/**
 * @brief 从 JSON 对象文本中读取字符串字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，UTF-8 编码的值。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonStringField(const std::string& objectText, const char* fieldName, std::string& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t quote = objectText.find('"', colon == std::string::npos ? name + marker.size() : colon + 1);
    size_t end = 0;
    return quote != std::string::npos && ParseJsonStringAt(objectText, quote, value, end);
}

/**
 * @brief 从 JSON 对象文本中读取整数字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，整数值。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonIntField(const std::string& objectText, const char* fieldName, int& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t numberStart = objectText.find_first_of("-0123456789", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (numberStart == std::string::npos) return false;
    try { value = std::stoi(objectText.substr(numberStart)); return true; }
    catch (...) { return false; }
}

/**
 * @brief 从 JSON 对象文本中读取布尔字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，布尔值。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonBoolField(const std::string& objectText, const char* fieldName, bool& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t valueStart = objectText.find_first_not_of(" \t\r\n", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (valueStart == std::string::npos) return false;
    if (objectText.compare(valueStart, 4, "true") == 0) { value = true; return true; }
    if (objectText.compare(valueStart, 5, "false") == 0) { value = false; return true; }
    return false;
}

/**
 * @brief 从 JSON 对象文本中读取浮点字段值。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param value 输出参数，浮点值。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonFloatField(const std::string& objectText, const char* fieldName, float& value) const
{
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t numberStart = objectText.find_first_of("-.0123456789", colon == std::string::npos ? name + marker.size() : colon + 1);
    if (numberStart == std::string::npos) return false;
    try { value = std::stof(objectText.substr(numberStart)); return true; }
    catch (...) { return false; }
}

/**
 * @brief 在 JSON 文本中查找匹配的闭合括号位置（支持字符串内转义）。
 * @param text JSON 文本。
 * @param start 起始位置（应为 '{' 或 '['）。
 * @param open 起始括号字符。
 * @param close 闭合括号字符。
 * @return 闭合位置，未找到返回 npos。
 */
inline size_t DesktopApp::FindJsonContainerEnd(const std::string& text, size_t start, char open, char close) const
{
    if (start >= text.size() || text[start] != open) return std::string::npos;
    int depth = 1;
    bool inString = false;
    for (size_t i = start + 1; i < text.size(); ++i)
    {
        char ch = text[i];
        if (ch == '"' && (i == 0 || text[i - 1] != '\\')) inString = !inString;
        else if (!inString)
        {
            if (ch == open) ++depth;
            else if (ch == close) { --depth; if (depth == 0) return i; }
        }
    }
    return std::string::npos;
}

/**
 * @brief 在 JSON 文本中查找对象结束位置。
 */
inline size_t DesktopApp::FindJsonObjectEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '{', '}'); }

/**
 * @brief 在 JSON 文本中查找数组结束位置。
 */
inline size_t DesktopApp::FindJsonArrayEnd(const std::string& text, size_t start) const
    { return FindJsonContainerEnd(text, start, '[', ']'); }

/**
 * @brief 从 JSON 对象文本中读取字符串数组字段。
 * @param objectText JSON 对象文本。
 * @param fieldName 字段名。
 * @param values 输出参数，宽字符串数组。
 * @return 读取成功返回 true。
 */
inline bool DesktopApp::ReadJsonStringArrayField(const std::string& objectText, const char* fieldName, std::vector<std::wstring>& values) const
{
    values.clear();
    std::string marker = std::string("\"") + fieldName + "\"";
    size_t name = objectText.find(marker);
    if (name == std::string::npos) return false;
    size_t colon = objectText.find(':', name + marker.size());
    size_t arrayStart = objectText.find('[', colon == std::string::npos ? name + marker.size() : colon + 1);
    if (arrayStart == std::string::npos) return false;
    size_t arrayEnd = FindJsonArrayEnd(objectText, arrayStart);
    if (arrayEnd == std::string::npos) return false;
    size_t pos = arrayStart + 1;
    while (pos < arrayEnd)
    {
        size_t quote = objectText.find('"', pos);
        if (quote == std::string::npos || quote >= arrayEnd) break;
        std::string utf8;
        size_t end = 0;
        if (!ParseJsonStringAt(objectText, quote, utf8, end)) break;
        values.push_back(Utf8ToWide(utf8));
        pos = end;
    }
    return true;
}

/**
 * @brief 将 JSON 字符串转换为组件类型枚举。
 * @param type 类型字符串（不区分大小写）。
 * @return 对应的 DesktopWidgetType 枚举值。
 */
inline DesktopWidgetType DesktopApp::WidgetTypeFromJson(const std::wstring& type) const
{
    std::wstring n = ToUpperInvariant(type);
    if (n == L"FILECATEGORIES" || n == L"FILE_CATEGORIES") return DesktopWidgetType::FileCategories;
    if (n == L"FOLDERMAPPING" || n == L"FOLDER_MAPPING") return DesktopWidgetType::FolderMapping;
    if (n == L"COLLECTIONGROUP" || n == L"COLLECTION_GROUP") return DesktopWidgetType::CollectionGroup;
    if (n == L"FILEGROUP" || n == L"FILE_GROUP") return DesktopWidgetType::FileGroup;
    if (n == L"LUA" || n == L"LUASCRIPT" || n == L"LUA_SCRIPT") return DesktopWidgetType::LuaScript;
    if (n == L"GUIDE") return DesktopWidgetType::Guide;
    if (n == L"COLLECTION") return DesktopWidgetType::Collection;
    return DesktopWidgetType::Collection;
}

/**
 * @brief 将组件类型枚举转换为 JSON 字符串。
 * @param type 组件类型。
 * @return 对应的字符串表示。
 */
inline std::wstring DesktopApp::WidgetTypeToJson(DesktopWidgetType type) const
{
    switch (type)
    {
    case DesktopWidgetType::CollectionGroup: return L"collectionGroup";
    case DesktopWidgetType::FileGroup:       return L"fileGroup";
    case DesktopWidgetType::FileCategories: return L"fileCategories";
    case DesktopWidgetType::FolderMapping:  return L"folderMapping";
    case DesktopWidgetType::LuaScript:      return L"lua";
    case DesktopWidgetType::Guide:          return L"guide";
    case DesktopWidgetType::Collection:
    default:                                return L"collection";
    }
}

// ── 控件窗口 ──────────────────────────────────────────

/**
 * @brief 控件窗口的消息处理函数（静态回调），将消息转发到 HandleControlMessage。
 * @param hwnd 窗口句柄。
 * @param msg 消息 ID。
 * @param wp wParam。
 * @param lp lParam。
 * @return 消息处理结果。
 */
inline LRESULT CALLBACK DesktopApp::ControlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    DesktopApp* app = nullptr;
    if (msg == WM_NCCREATE)
    {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        app = static_cast<DesktopApp*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    else
    {
        app = reinterpret_cast<DesktopApp*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (app) return app->HandleControlMessage(hwnd, msg, wp, lp);
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 处理控件窗口的消息：任务栏重启、托盘回调、定时器、命令、关闭、销毁等。
 * @param hwnd 窗口句柄。
 * @param msg 消息 ID。
 * @param wp wParam。
 * @param lp lParam。
 * @return 消息处理结果。
 */
inline LRESULT DesktopApp::HandleControlMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (systemTaskbarTaskViewStateMsg_ &&
        msg == systemTaskbarTaskViewStateMsg_)
    {
        const bool visible = wp != 0;
        if (systemTaskbarTaskViewActive_ != visible)
        {
            systemTaskbarTaskViewActive_ = visible;
            systemTaskbarWindowStateChangedTick_.fetch_add(1,
                std::memory_order_relaxed);
        }
        return 0;
    }
    if (taskbarRestartMsg_ && msg == taskbarRestartMsg_)
    {
        NotifySystemTaskbarCreated();
        systemTaskbarBackdropRefreshTick_ = 0;
        systemTaskbarTaskViewActive_ = false;
        systemTaskbarWindows_.clear();
        RestartSystemTaskbarShellVisibilityDetectors();
        systemTaskbarWindowStateChangedTick_.fetch_add(1,
            std::memory_order_relaxed);
        DWORD currentExplorerProcessId = 0;
        if (HWND taskbar = FindWindowW(L"Shell_TrayWnd", nullptr))
            GetWindowThreadProcessId(taskbar, &currentExplorerProcessId);
        // Explorer can broadcast TaskbarCreated more than once while its shell
        // windows settle. Rebuild the desktop pipeline once per Explorer PID.
        if (!currentExplorerProcessId ||
            currentExplorerProcessId != desktopHostExplorerProcessId_)
            explorerDesktopRecreatePending_ = true;
        RecoverDesktopHostAfterExplorerRestart();
        return 0;
    }
    switch (msg)
    {
    case WM_DISPLAYCHANGE:
        ScheduleDisplayTopologyRefresh();
        return 0;
    case WM_DEVICECHANGE:
        switch (wp)
        {
        case DBT_DEVNODES_CHANGED:
        case DBT_CONFIGCHANGED:
        case DBT_DEVICEARRIVAL:
        case DBT_DEVICEREMOVECOMPLETE:
            ScheduleDisplayTopologyRefresh();
            break;
        default:
            break;
        }
        return TRUE;
    case kTrayCallbackMessage:
        OnTrayCallback(lp);
        return 0;
    case WM_TIMER:
        OnTimer(wp);
        return 0;
    case WM_HOTKEY:
        if (static_cast<int>(wp) == kQuickNavigationHotkeyId)
        {
            ToggleQuickNavigation();
            return 0;
        }
        if (static_cast<int>(wp) ==
            kFloatingDockHotkeyId)
        {
            ToggleFloatingDock();
            return 0;
        }
        break;
    case WM_COMMAND:
        return 0;
    case WM_CLOSE:
        RequestExit();
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, kDisplayTopologyRefreshTimerId);
        if (floatingDockHotkeyHwnd_ == hwnd)
        {
            floatingDockHotkeyHwnd_ = nullptr;
            floatingDockHotkeyRegistered_ = false;
        }
        if (floatingDockEdgeSwipeHwnd_ == hwnd)
        {
            floatingDockEdgeSwipeHwnd_ = nullptr;
            floatingDockEdgeSwipeDetector_.Reset();
        }
        controlHwnd_ = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/**
 * @brief 重新加载桌面项，可选择是否重新从磁盘读取布局。
 * @param reloadLayoutFromDisk 是否重新加载布局文件。
 */
inline void DesktopApp::ReloadItems(bool reloadLayoutFromDisk)
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);
    if (reloading_) return;
    reloading_ = true;
    dockAppIdentityCache_.clear();
    dockRunningWindows_.clear();
    dockFolderTargetCache_.clear();
    dockFolderIconIndexCache_.clear();
    BeginIconLoadGeneration();
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
    if (reloadLayoutFromDisk)
    {
        LoadLayoutSlots();
        // The file has just populated savedPageColumns_/savedPageRows_. Do not
        // overwrite those restored values with the pre-reload runtime grid.
        UpdateLayoutWorkArea(false);
        if (widgetEngine_)
            widgetEngine_->ReloadStorage();
    }
    else
    {
        for (auto& widget : widgets_)
        {
            if (widget.type == DesktopWidgetType::FolderMapping)
                EnumerateFolderMappingEntries(widget);
        }
    }
    LoadDesktopItems();
    // LoadLayoutSlots may normalize Dock entries before the freshly
    // enumerated desktop items are available. Discard those provisional
    // resolutions so paths and shortcut targets are classified from the new
    // item snapshot.
    dockFolderTargetCache_.clear();
    dockFolderIconIndexCache_.clear();
    // A Shell delete removes the desktop item, but its persisted Dock mapping
    // otherwise survives and still consumes a slot.  Only prune references
    // that are confirmed missing on disk: hidden files and temporarily
    // unenumerated Shell items must remain pinned.
    std::erase_if(dockEntries_, [this](const DockEntry& entry) {
        if (entry.type != DockEntryType::DesktopItem)
            return false;
        if (IsRecycleBinDockEntry(entry))
            return FindItemIndexByKey(entry.reference) == static_cast<size_t>(-1);

        const std::wstring& path = entry.reference;
        const bool driveAbsolute = path.size() >= 3 &&
            ((path[0] >= L'A' && path[0] <= L'Z') ||
             (path[0] >= L'a' && path[0] <= L'z')) &&
            path[1] == L':' && (path[2] == L'\\' || path[2] == L'/');
        const bool uncAbsolute = path.starts_with(L"\\\\");
        if (!driveAbsolute && !uncAbsolute)
            return false;

        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
            return false;
        const DWORD error = GetLastError();
        return error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND ||
            error == ERROR_INVALID_NAME;
    });
    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    if (!generalSettings_.dockEnabled && !dockEntries_.empty())
        RestoreDockEntriesToDesktop();
    ApplyAutoCollectFileCategoryWidgets();

    // Mark widgets as used
    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    // Mark items with valid existing positions as used; flag unslotted items
    std::unordered_set<std::wstring> placedKeys;
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;

        auto* page = item.gridCell.pageId.empty()
            ? nullptr
            : FindGridPage(gridPages_, item.gridCell.pageId);
        if (page == nullptr)
        {
            // Item belongs to a page not currently visible — mark its slots as used
            const std::wstring& pid = item.gridCell.pageId;
            if (!pid.empty() && savedPageColumns_.count(pid) && savedPageRows_.count(pid))
            {
                int cols = savedPageColumns_[pid];
                int rows = savedPageRows_[pid];
                if (item.gridCell.column >= 0 && item.gridCell.row >= 0 &&
                    item.gridCell.column + item.gridSpan.columns <= cols &&
                    item.gridCell.row + item.gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan) &&
                    !placedKeys.contains(item.layoutKey))
                {
                    MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
                    placedKeys.insert(item.layoutKey);
                }
            }
            continue;
        }

        bool validSlot = page != nullptr &&
            item.gridCell.column + item.gridSpan.columns <= page->columns &&
            item.gridCell.row + item.gridSpan.rows <= page->rows &&
            !AreGridSlotsMarked(usedSlots, item.gridCell, item.gridSpan) &&
            !placedKeys.contains(item.layoutKey);

        if (validSlot)
        {
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
            placedKeys.insert(item.layoutKey);
        }
        else
        {
            item.gridCell = {};
            item.gridSpan = {1, 1};
        }
    }

    // Assign free cells to unslotted items
    std::vector<DesktopItem*> unslotted;
    for (auto& item : items_)
    {
        if (!item.name.empty() && !IsItemInAnyWidget(item) && item.gridCell.pageId.empty())
            unslotted.push_back(&item);
    }

    std::sort(unslotted.begin(), unslotted.end(), [](const DesktopItem* a, const DesktopItem* b) {
        bool aDesk = !a->desktopIconClsid.empty();
        bool bDesk = !b->desktopIconClsid.empty();
        if (aDesk != bDesk) return aDesk;
        return ToUpperInvariant(a->name) < ToUpperInvariant(b->name);
    });

    // Track newly created virtual pages for overflow items
    std::unordered_map<std::wstring, int> overflowSlots;
    // Build quick-lookup of page IDs currently visible in gridPages_
    std::unordered_set<std::wstring> visiblePageIds2;
    for (const auto& gp : gridPages_)
        visiblePageIds2.insert(gp.id);

    for (auto* item : unslotted)
    {
        GridCell freeCell;
        if (TryFindFreeCell(item->gridSpan, usedSlots, freeCell))
        {
            item->gridCell = freeCell;
            MarkGridArea(usedSlots, freeCell, item->gridSpan);
            continue;
        }

        // Search all saved pages that aren't currently visible
        bool placedInSavedPage = false;
        for (const auto& pageId : savedPageIds_)
        {
            if (visiblePageIds2.count(pageId)) continue;
            if (!savedPageColumns_.count(pageId) || !savedPageRows_.count(pageId)) continue;
            int cols = savedPageColumns_[pageId];
            int rows = savedPageRows_[pageId];
            int capacity = std::max(1, cols * rows);
            for (int slot = 0; slot < capacity; ++slot)
            {
                GridCell candidate;
                candidate.pageId = pageId;
                candidate.column = slot / std::max(1, rows);
                candidate.row    = slot % std::max(1, rows);
                if (candidate.column + item->gridSpan.columns <= cols &&
                    candidate.row + item->gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, candidate, item->gridSpan))
                {
                    item->gridCell = candidate;
                    MarkGridArea(usedSlots, candidate, item->gridSpan);
                    placedInSavedPage = true;
                    break;
                }
            }
            if (placedInSavedPage) break;
        }
        if (placedInSavedPage) continue;

        // Try previously-created overflow pages
        bool placedInNewPage = false;
        for (auto& [pageId, nextSlot] : overflowSlots)
        {
            int cols = savedPageColumns_.count(pageId) ? savedPageColumns_[pageId] : 1;
            int rows = savedPageRows_.count(pageId) ? savedPageRows_[pageId] : 1;
            int capacity = std::max(1, cols * rows);
            for (int slot = nextSlot; slot < capacity; ++slot)
            {
                GridCell candidate;
                candidate.pageId = pageId;
                candidate.column = slot / std::max(1, rows);
                candidate.row    = slot % std::max(1, rows);
                if (candidate.column + item->gridSpan.columns <= cols &&
                    candidate.row + item->gridSpan.rows <= rows &&
                    !AreGridSlotsMarked(usedSlots, candidate, item->gridSpan))
                {
                    item->gridCell = candidate;
                    MarkGridArea(usedSlots, candidate, item->gridSpan);
                    nextSlot = slot + 1;
                    placedInNewPage = true;
                    break;
                }
            }
            if (placedInNewPage) break;
        }
        if (placedInNewPage) continue;

        // No space anywhere — create a new virtual page on the last monitor
        if (!gridPages_.empty())
        {
            std::vector<size_t> monitorOrder = BuildMonitorRenderOrder();
            GridPage& lastPage = gridPages_[monitorOrder.back()];

            std::wstring newPageId = GeneratePageId();
            RememberSavedPageId(newPageId);
            savedPageColumns_[newPageId] = lastPage.columns;
            savedPageRows_[newPageId]    = lastPage.rows;

            item->gridCell.pageId = newPageId;
            item->gridCell.column = 0;
            item->gridCell.row    = 0;
            MarkGridArea(usedSlots, item->gridCell, item->gridSpan);
            overflowSlots[newPageId] = 1;
        }
    }

    // Loading new files may add virtual overflow pages, while deleting files
    // may remove the last usable offset. Refresh the runtime page mapping in
    // this same reload pass instead of waiting for the next manual refresh.
    ApplyPageMapping();
    LayoutItems();
    ApplyPendingPlacement();
    UpdateCutState();

    // Prune desktop-backed widget itemKeys that no longer exist (file was deleted from outside).
    // FolderMapping keys are mapped-folder paths, not desktop layout keys.
    std::unordered_set<std::wstring> allKeys;
    for (auto& item : items_)
        if (!item.layoutKey.empty())
            allKeys.insert(ToUpperInvariant(item.layoutKey));
    for (auto& w : widgets_)
    {
        if (w.type == DesktopWidgetType::FolderMapping)
            continue;
        auto it = std::remove_if(w.itemKeys.begin(), w.itemKeys.end(),
            [&](const std::wstring& key) {
                return allKeys.count(ToUpperInvariant(key)) == 0;
            });
        w.itemKeys.erase(it, w.itemKeys.end());
    }

    SaveLayoutSlots();
    RebuildContainersAndItems();
    reloading_ = false;
    RefreshDockRunningWindows(false);
    if (widgetEngine_)
        widgetEngine_->NotifyDesktopChanged("reload");
    InvalidateRect(hwnd_, nullptr, TRUE);
}

inline void DesktopApp::EnqueueIconLoad(IconLoadTask task)
{
    if (task.requestKey.empty())
    {
        const std::wstring& identity = task.isDesktopItem ? task.layoutKey : task.folderPath;
        task.requestKey = std::to_wstring(task.serial) + L"\n" +
            (task.isDesktopItem ? L"D\n" : L"F\n") + task.widgetId + L"\n" +
            ToUpperInvariant(identity) + L"\n" +
            (task.phase == IconLoadPhase::Phase1 ? L"1" : L"2");
    }
    {
        std::lock_guard<std::mutex> lock(iconLoaderMutex_);
        if (!iconLoaderPendingKeys_.insert(task.requestKey).second)
            return;
        iconLoaderQueue_.push_back(std::move(task));
    }
    iconLoaderCv_.notify_one();
}

inline void DesktopApp::OnIconLoaded(WPARAM /*wParam*/, LPARAM lParam)
{
    auto* result = reinterpret_cast<IconLoadResult*>(lParam);
    if (!result) return;

    std::unique_ptr<IconLoadResult> resultGuard(result);
    {
        std::lock_guard<std::mutex> lock(iconLoaderMutex_);
        iconLoaderPendingKeys_.erase(result->requestKey);
    }
    if (result->serial != iconLoadSerial_)
    {
        if (result->bitmap) DeleteObject(result->bitmap);
        result->bitmap = nullptr;
        return;
    }
    bool matched = false;

    if (result->isDesktopItem)
    {
        for (auto& item : items_)
        {
            if (ToUpperInvariant(item.layoutKey) == ToUpperInvariant(result->layoutKey))
            {
                if (result->bitmap)
                {
                    if (item.iconBitmap) { EraseD2DIconCacheForBitmap(item.iconBitmap); DeleteObject(item.iconBitmap); }
                    item.iconBitmap = result->bitmap;
                    item.iconBitmapSize = result->bitmapSize;
                    result->bitmap = nullptr;
                }
                matched = true;
                if (result->phase == IconLoadPhase::Phase1)
                {
                    item.iconState = IconState::IconReady;
                    item.shortcutArrow = result->shortcutArrow;
                    item.isShortcut = result->isShortcut;
                    item.isApplicationShortcut = result->isApplicationShortcut;
                    IconLoadTask phase2;
                    phase2.serial = result->serial;
                    phase2.layoutKey = item.layoutKey;
                    phase2.absolutePidl.reset(ILClone(item.absolutePidl.get()));
                    phase2.sysIconIndex = item.sysIconIndex;
                    phase2.parsingName = item.parsingName;
                    phase2.isDesktopItem = true;
                    phase2.phase = IconLoadPhase::Phase2;
                    EnqueueIconLoad(std::move(phase2));
                }
                else
                {
                    item.iconState = IconState::FullQuality;
                }
                InvalidateRect(hwnd_, nullptr, FALSE);
                if (quickNavigationOpen_)
                    InvalidateQuickNavigationWindow();
                break;
            }
        }
    }
    else
    {
        for (auto& widget : widgets_)
        {
            if (widget.id != result->widgetId || widget.sourceFolderPath.empty()) continue;
            for (auto& entry : widget.folderEntries)
            {
                if (ToUpperInvariant(entry.fullPath) == ToUpperInvariant(result->folderPath))
                {
                    if (result->bitmap)
                    {
                        if (entry.iconBitmap) { EraseD2DIconCacheForBitmap(entry.iconBitmap); DeleteObject(entry.iconBitmap); }
                        entry.iconBitmap = result->bitmap;
                        entry.iconBitmapSize = result->bitmapSize;
                        result->bitmap = nullptr;
                    }
                    matched = true;
                    if (result->phase == IconLoadPhase::Phase1)
                    {
                        entry.iconState = IconState::IconReady;
                        entry.shortcutArrow = result->shortcutArrow;
                        entry.isShortcut = result->isShortcut;
                        entry.isApplicationShortcut = result->isApplicationShortcut;
                        IconLoadTask phase2;
                        phase2.serial = result->serial;
                        phase2.widgetId = widget.id;
                        phase2.folderPath = entry.fullPath;
                        phase2.sysIconIndex = entry.sysIconIndex;
                        phase2.isDesktopItem = false;
                        phase2.phase = IconLoadPhase::Phase2;
                        PIDLIST_ABSOLUTE pidl = nullptr;
                        if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                        {
                            phase2.absolutePidl.reset(pidl);
                            EnqueueIconLoad(std::move(phase2));
                        }
                    }
                    else
                    {
                        entry.iconState = IconState::FullQuality;
                    }
                    InvalidateRect(hwnd_, nullptr, FALSE);
                    if (quickNavigationOpen_)
                        InvalidateQuickNavigationWindow();
                    break;
                }
            }
            break;
        }
    }

    if (!matched && result->bitmap)
        DeleteObject(result->bitmap);
}

/**
 * @brief 枚举桌面文件夹中的所有项，构建 DesktopItem 列表，包含图标、布局键和网格位置。
 *
 * 会依据 Windows“隐藏的项目”设置过滤隐藏项，同时过滤非桌面路径项，
 * 并为 .lnk 文件检测快捷方式箭头。
 */
inline void DesktopApp::LoadDesktopItems()
{
    extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);

    struct OldIcon {
        HBITMAP bitmap = nullptr;
        SIZE size{};
        int sysIconIndex = -1;
        bool shortcutArrow = false;
        bool isShortcut = false;
        bool isApplicationShortcut = false;
        IconState iconState = IconState::Loading;
    };
    std::unordered_map<std::wstring, OldIcon> oldIconCache;
    for (auto& item : items_) {
        if (!item.layoutKey.empty() && item.iconBitmap) {
            OldIcon old;
            old.bitmap = item.iconBitmap;
            old.size = item.iconBitmapSize;
            old.sysIconIndex = item.sysIconIndex;
            old.shortcutArrow = item.shortcutArrow;
            old.isShortcut = item.isShortcut;
            old.isApplicationShortcut = item.isApplicationShortcut;
            old.iconState = item.iconState;
            oldIconCache.emplace(ToUpperInvariant(item.layoutKey), std::move(old));
            item.iconBitmap = nullptr;
        }
    }
items_.clear();
    itemIndexByKeyCache_.clear();
    itemTextLayoutCache_.clear();
    itemTextShadowCache_.clear();
    WriteCrashLogEntry(L"LoadItems start");

    HRESULT hr = SHGetDesktopFolder(&desktopFolder_);
    if (FAILED(hr) || !desktopFolder_) { WriteCrashLogEntry(L"SHGetDesktopFolder FAILED"); return; }
    WriteCrashLogEntry(L"DesktopFolder ok");

    LPITEMIDLIST raw = nullptr;
    hr = SHGetSpecialFolderLocation(nullptr, CSIDL_DESKTOP, &raw);
    if (FAILED(hr) || !raw) { WriteCrashLogEntry(L"SHGetSpecialFolderLocation FAILED"); return; }
    desktopPidl_.reset(raw);
    WriteCrashLogEntry(L"DesktopPidl ok");

    wchar_t userDesktopPath[MAX_PATH]{};
    wchar_t commonDesktopPath[MAX_PATH]{};
    wchar_t userProfilePath[MAX_PATH]{};
    SHGetSpecialFolderPathW(nullptr, userDesktopPath, CSIDL_DESKTOPDIRECTORY, FALSE);
    SHGetSpecialFolderPathW(nullptr, commonDesktopPath, CSIDL_COMMON_DESKTOPDIRECTORY, FALSE);
    SHGetSpecialFolderPathW(nullptr, userProfilePath, CSIDL_PROFILE, FALSE);
    size_t userDesktopLen = wcslen(userDesktopPath);
    size_t commonDesktopLen = wcslen(commonDesktopPath);

    const bool showHiddenItems = AreExplorerHiddenItemsVisible();
    SHCONTF enumFlags = SHCONTF_FOLDERS | SHCONTF_NONFOLDERS;
    if (showHiddenItems)
        enumFlags = static_cast<SHCONTF>(enumFlags | SHCONTF_INCLUDEHIDDEN);

    ComPtr<IEnumIDList> enumerator;
    hr = desktopFolder_->EnumObjects(hwnd_, enumFlags, &enumerator);
    if (FAILED(hr) || !enumerator) { WriteCrashLogEntry(L"EnumObjects FAILED"); return; }
    WriteCrashLogEntry(L"EnumObjects ok");

    PITEMID_CHILD child = nullptr;
    ULONG fetched = 0;
    int count = 0;
    wchar_t buf[64];
    std::unordered_set<std::wstring> seenKeys;
    while (enumerator->Next(1, &child, &fetched) == S_OK)
    {
        PIDLIST_ABSOLUTE absolute = ILCombine(desktopPidl_.get(), child);
        if (!absolute) { ILFree(child); continue; }

        // Get parsing name (used for CLSID detection)
        std::wstring parsingName = StrRetToString(
            desktopFolder_.Get(), reinterpret_cast<PCUITEMID_CHILD>(child), SHGDN_FORPARSING);

        // Get file system path
        wchar_t itemPath[MAX_PATH]{};
        std::wstring itemPathStr;
        if (SHGetPathFromIDListW(absolute, itemPath) && itemPath[0])
            itemPathStr = itemPath;

        if (snowdesktop::shell_item_visibility::
                IsAlwaysHidden(
                    itemPathStr.empty()
                        ? parsingName
                        : itemPathStr))
        {
            ILFree(absolute);
            ILFree(child);
            continue;
        }

        std::wstring clsid = ResolveDesktopIconClsid(parsingName, itemPathStr, userProfilePath);
        bool isDesktopIcon = !clsid.empty();

        // Non-desktop-icon: always skip non-enumerated shell items, and follow
        // Explorer's "Hidden items" setting for ordinary hidden entries.
        if (!isDesktopIcon)
        {
            SFGAOF attrs = SFGAO_HIDDEN | SFGAO_NONENUMERATED;
            LPCITEMIDLIST childConst = child;
            if (SUCCEEDED(desktopFolder_->GetAttributesOf(1, &childConst, &attrs)))
            {
                if ((attrs & SFGAO_NONENUMERATED) ||
                    (!showHiddenItems && (attrs & SFGAO_HIDDEN)))
                { ILFree(absolute); ILFree(child); continue; }
            }
        }

        // Get display name and icon
        SHFILEINFOW info{};
        SHGetFileInfoW(reinterpret_cast<LPCWSTR>(absolute), 0, &info, sizeof(info),
            SHGFI_PIDL | SHGFI_SYSICONINDEX | SHGFI_DISPLAYNAME | SHGFI_TYPENAME);

        // Check visibility (applies to all items)
        if (!IsVisibleByDesktopIconSettings(clsid, settingsIconVisibility_))
        { ILFree(absolute); ILFree(child); continue; }

        // Non-desktop-icon: must be physically on desktop
        if (!isDesktopIcon && !itemPathStr.empty())
        {
            bool underUser = itemPathStr.size() > userDesktopLen &&
                _wcsnicmp(itemPathStr.c_str(), userDesktopPath, userDesktopLen) == 0 &&
                itemPathStr[userDesktopLen] == L'\\';
            bool underCommon = itemPathStr.size() > commonDesktopLen &&
                _wcsnicmp(itemPathStr.c_str(), commonDesktopPath, commonDesktopLen) == 0 &&
                itemPathStr[commonDesktopLen] == L'\\';
            if (!underUser && !underCommon)
            { ILFree(absolute); ILFree(child); continue; }
        }

        DesktopItem item;
        item.absolutePidl.reset(absolute);
        item.childPidl.reset(reinterpret_cast<PIDLIST_ABSOLUTE>(child));
        item.parsingName = std::move(parsingName);
        item.desktopIconClsid = std::move(clsid);
        item.name = info.szDisplayName[0] ? info.szDisplayName
            : StrRetToString(desktopFolder_.Get(), reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()), SHGDN_NORMAL);
        item.typeName = info.szTypeName;
        item.sysIconIndex = info.iIcon;
        item.layoutKey = GetStableLayoutKey(item.absolutePidl.get(), item.parsingName, item.desktopIconClsid);

auto oldIt = oldIconCache.find(ToUpperInvariant(item.layoutKey));
        if (oldIt != oldIconCache.end() && oldIt->second.sysIconIndex == item.sysIconIndex) {
            item.iconBitmap = oldIt->second.bitmap;
            item.iconBitmapSize = oldIt->second.size;
            item.shortcutArrow = oldIt->second.shortcutArrow;
            item.isShortcut = oldIt->second.isShortcut;
            item.isApplicationShortcut = oldIt->second.isApplicationShortcut;
            item.iconState = oldIt->second.iconState;
            oldIt->second.bitmap = nullptr;
            oldIconCache.erase(oldIt);
            if (item.iconState == IconState::IconReady)
            {
                IconLoadTask phase2;
                phase2.serial = iconLoadSerial_;
                phase2.layoutKey = item.layoutKey;
                phase2.absolutePidl.reset(ILClone(item.absolutePidl.get()));
                phase2.sysIconIndex = item.sysIconIndex;
                phase2.parsingName = item.parsingName;
                phase2.isDesktopItem = true;
                phase2.phase = IconLoadPhase::Phase2;
                EnqueueIconLoad(std::move(phase2));
            }
        } else {
            if (oldIt != oldIconCache.end()) {
                if (oldIt->second.bitmap) {
                    EraseD2DIconCacheForBitmap(oldIt->second.bitmap);
                    DeleteObject(oldIt->second.bitmap);
                }
                oldIconCache.erase(oldIt);
            }
            item.iconBitmap = nullptr;
            item.iconState = IconState::Loading;

            IconLoadTask task;
            task.serial = iconLoadSerial_;
            task.layoutKey = item.layoutKey;
            task.absolutePidl.reset(ILClone(item.absolutePidl.get()));
            task.sysIconIndex = item.sysIconIndex;
            task.parsingName = item.parsingName;
            task.isDesktopItem = true;
            task.phase = IconLoadPhase::Phase1;
            EnqueueIconLoad(std::move(task));
        }

        if (seenKeys.contains(item.layoutKey))
        { ILFree(absolute); ILFree(child); continue; }
        seenKeys.insert(item.layoutKey);

        auto knownRecord = layoutRecords_.find(item.layoutKey);
        if (knownRecord != layoutRecords_.end() && knownRecord->second.hasGrid)
        {
            item.gridCell = knownRecord->second.cell;
            item.gridSpan = knownRecord->second.span;
            item.slot = SlotFromCell(gridPages_, item.gridCell);
        }
        else
        {
            item.gridCell = {};
            item.gridSpan = {1, 1};
            item.slot = -1;
        }

        items_.push_back(std::move(item));
        ++count;
    }
    // child PIDL ownership transferred to last DesktopItem — do NOT ILFree

    for (auto& [key, old] : oldIconCache) {
        if (old.bitmap) {
            EraseD2DIconCacheForBitmap(old.bitmap);
            DeleteObject(old.bitmap);
        }
    }

    wsprintfW(buf, L"Loaded %d items", count);
    WriteCrashLogEntry(buf);
    RefreshDesktopItemIndexCache();
}

/**
 * @brief 捕获当前活动显示器拓扑的稳定签名。
 *
 * 签名包含虚拟桌面范围，以及每台活动显示器的设备名、屏幕范围、
 * 工作区、主屏标记和有效 DPI。排序后再拼接，避免枚举顺序变化导致误判。
 */
inline std::wstring DesktopApp::CaptureDisplayTopologySignature() const
{
    struct DisplayRecord
    {
        std::wstring deviceName;
        RECT monitor{};
        RECT work{};
        DWORD flags = 0;
        UINT dpiX = 0;
        UINT dpiY = 0;
    };

    std::vector<DisplayRecord> records;
    auto callback = [](HMONITOR monitor, HDC, LPRECT, LPARAM param) -> BOOL {
        auto* output = reinterpret_cast<std::vector<DisplayRecord>*>(param);
        if (!output)
            return FALSE;

        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitor, &info))
            return TRUE;

        DisplayRecord record;
        record.deviceName = info.szDevice;
        record.monitor = info.rcMonitor;
        record.work = info.rcWork;
        record.flags = info.dwFlags;
        GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &record.dpiX, &record.dpiY);
        output->push_back(std::move(record));
        return TRUE;
    };
    EnumDisplayMonitors(nullptr, nullptr, callback, reinterpret_cast<LPARAM>(&records));

    std::sort(records.begin(), records.end(), [](const DisplayRecord& a, const DisplayRecord& b) {
        const int nameOrder = _wcsicmp(a.deviceName.c_str(), b.deviceName.c_str());
        if (nameOrder != 0) return nameOrder < 0;
        if (a.monitor.left != b.monitor.left) return a.monitor.left < b.monitor.left;
        if (a.monitor.top != b.monitor.top) return a.monitor.top < b.monitor.top;
        if (a.monitor.right != b.monitor.right) return a.monitor.right < b.monitor.right;
        return a.monitor.bottom < b.monitor.bottom;
    });

    std::wstring signature =
        std::to_wstring(GetSystemMetrics(SM_XVIRTUALSCREEN)) + L"," +
        std::to_wstring(GetSystemMetrics(SM_YVIRTUALSCREEN)) + L"," +
        std::to_wstring(GetSystemMetrics(SM_CXVIRTUALSCREEN)) + L"," +
        std::to_wstring(GetSystemMetrics(SM_CYVIRTUALSCREEN));

    for (const auto& record : records)
    {
        signature += L"|";
        signature += record.deviceName;
        signature += L":" + std::to_wstring(record.monitor.left);
        signature += L"," + std::to_wstring(record.monitor.top);
        signature += L"," + std::to_wstring(record.monitor.right);
        signature += L"," + std::to_wstring(record.monitor.bottom);
        signature += L":" + std::to_wstring(record.work.left);
        signature += L"," + std::to_wstring(record.work.top);
        signature += L"," + std::to_wstring(record.work.right);
        signature += L"," + std::to_wstring(record.work.bottom);
        signature += L":" + std::to_wstring(record.flags);
        signature += L":" + std::to_wstring(record.dpiX);
        signature += L"," + std::to_wstring(record.dpiY);
    }
    return signature;
}

/**
 * @brief 防抖调度显示器拓扑复查。
 *
 * 重复设备通知只会重置同一个一次性定时器，不会重复枚举或重建布局。
 */
inline void DesktopApp::ScheduleDisplayTopologyRefresh()
{
    HWND timerWindow = controlHwnd_ && IsWindow(controlHwnd_)
        ? controlHwnd_
        : (hwnd_ && IsWindow(hwnd_) ? hwnd_ : nullptr);
    if (timerWindow)
        SetTimer(timerWindow, kDisplayTopologyRefreshTimerId,
            kDisplayTopologyRefreshDebounceMs, nullptr);
}

/**
 * @brief 在显示器拓扑实际变化后调整桌面覆盖层并重建布局。
 */
inline void DesktopApp::RefreshDisplayTopologyIfChanged()
{
    if (exitRequested_)
        return;

    const std::wstring currentSignature = CaptureDisplayTopologySignature();
    if (currentSignature == displayTopologySignature_)
        return;

    if (reloading_)
    {
        ScheduleDisplayTopologyRefresh();
        return;
    }

    const int newVirtualLeft = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int newVirtualTop = GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int newVirtualWidth = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int newVirtualHeight = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (newVirtualWidth <= 0 || newVirtualHeight <= 0)
    {
        ScheduleDisplayTopologyRefresh();
        return;
    }

    virtualLeft_ = newVirtualLeft;
    virtualTop_ = newVirtualTop;
    virtualWidth_ = newVirtualWidth;
    virtualHeight_ = newVirtualHeight;

    if (hwnd_ && IsWindow(hwnd_))
    {
        HWND parent = GetParent(hwnd_);
        POINT origin{ virtualLeft_, virtualTop_ };
        if (parent && IsWindow(parent))
            ScreenToClient(parent, &origin);

        updatingDisplayTopology_ = true;
        SetWindowPos(hwnd_, HWND_TOP, origin.x, origin.y,
            virtualWidth_, virtualHeight_, SWP_NOACTIVATE);
        updatingDisplayTopology_ = false;

        dcompSurface_.Reset();
        compositionWidth_ = 0;
        compositionHeight_ = 0;
    }

    UpdateLayoutWorkArea();
    LayoutItems();
    displayTopologySignature_ = currentSignature;

    if (hwnd_ && IsWindow(hwnd_))
        InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 更新布局工作区，枚举显示器并创建对应 GridPage，然后应用页面映射。
 */
inline void DesktopApp::UpdateLayoutWorkArea(bool preserveActiveDimensions)
{
    layoutWorkArea_ = MakeRect(0, 0, virtualWidth_, virtualHeight_);
    // Preserve the active page dimensions before rebuilding monitor geometry.
    // DPI, resolution and work-area changes may resize cells, but must not
    // replace an already established row/column count.
    if (preserveActiveDimensions)
    {
        for (const auto& page : gridPages_)
        {
            if (page.id.empty()) continue;
            savedPageColumns_[page.id] = std::max(1, page.columns);
            savedPageRows_[page.id] = std::max(1, page.rows);
        }
    }

    // The pages below are rebuilt from MONITORINFO::rcWork, so their work
    // areas no longer contain our previous Dock reservation. Discard the old
    // rectangles before ApplyDockWorkAreaReservation() runs; otherwise it
    // "restores" that stale reservation into the fresh work area and can
    // expand it across the Windows taskbar.
    dockAreas_.clear();
    gridPages_.clear();

    MonitorEnumContext ctx{};
    ctx.virtualLeft = virtualLeft_;
    ctx.virtualTop = virtualTop_;
    ctx.pages = &gridPages_;
    EnumDisplayMonitors(nullptr, nullptr, EnumGridPageMonitorProc, reinterpret_cast<LPARAM>(&ctx));

    if (gridPages_.empty())
    {
        GridPage fb;
        fb.id = L"Primary"; fb.monitorId = fb.id; fb.isPrimary = true;
        fb.bounds = layoutWorkArea_; fb.workArea = layoutWorkArea_;
        gridPages_.push_back(fb);
    }

    // 从枚举结果提取系统主屏 monitorId（供双锚点回退解析使用）
    primaryMonitorId_.clear();
    for (const auto& p : gridPages_)
        if (p.isPrimary) { primaryMonitorId_ = p.monitorId; break; }
    if (primaryMonitorId_.empty() && !gridPages_.empty())
        primaryMonitorId_ = gridPages_.front().monitorId;

    std::sort(gridPages_.begin(), gridPages_.end(), [](const GridPage& a, const GridPage& b) {
        return a.bounds.left < b.bounds.left;
    });

    for (auto& page : gridPages_)
    {
        page.workArea.left   = std::clamp<LONG>(page.workArea.left,   0, static_cast<LONG>(virtualWidth_));
        page.workArea.top    = std::clamp<LONG>(page.workArea.top,    0, static_cast<LONG>(virtualHeight_));
        page.workArea.right  = std::clamp<LONG>(page.workArea.right,  page.workArea.left, static_cast<LONG>(virtualWidth_));
        page.workArea.bottom = std::clamp<LONG>(page.workArea.bottom, page.workArea.top,  static_cast<LONG>(virtualHeight_));
        ConfigureGridPage(page);
        ApplyIconSpacingToPage(page);
    }

    ApplyPageMapping();
    ApplyDockWorkAreaReservation();
}

/**
 * @brief 根据工作区尺寸配置网格页面的默认列数与行数。
 * @param page 待配置的网格页面。
 */
inline void DesktopApp::ConfigureGridPage(GridPage& page) const
{
    const int marginX = kGridMarginX;
    const int marginY = kGridMarginY;
    // The work area is already in physical pixels. Default rows and columns are
    // derived from the physical screen area only, so changing Windows DPI does
    // not change the page grid.
    const int cw = kCellWidth;
    const int ch = kMinCellHeight;
    const int w  = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
    const int h  = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));
    const int uw = std::max(1, w - marginX * 2);
    const int uh = std::max(1, h - marginY * 2);
    page.columns   = std::max(4, uw / cw);
    page.rows      = std::max(3, uh / ch);
}

/**
 * @brief 将保存的列/行数设置应用到当前网格页面。
 */
inline void DesktopApp::ApplySavedGridDimensions()
{
    for (auto& page : gridPages_)
    {
        auto colIt = savedPageColumns_.find(page.id);
        auto rowIt = savedPageRows_.find(page.id);
        if (colIt != savedPageColumns_.end() && rowIt != savedPageRows_.end() &&
            colIt->second >= 1 && rowIt->second >= 1)
        {
            page.columns = colIt->second;
            page.rows = rowIt->second;
            ApplyIconSpacingToPage(page);
        }
    }
}

/**
 * @brief 根据固定行列数与图标间距比例重新计算页面布局。
 * @param page 目标网格页面。
 */
inline void DesktopApp::ApplyIconSpacingToPage(GridPage& page)
{
    page.columns = std::max(1, page.columns);
    page.rows = std::max(1, page.rows);

    const int pageW = static_cast<int>(std::max<LONG>(1, page.workArea.right - page.workArea.left));
    const int pageH = static_cast<int>(std::max<LONG>(1, page.workArea.bottom - page.workArea.top));

    const float pageCellScale = std::max(0.1f, std::min(
        static_cast<float>(pageW) /
            static_cast<float>(page.columns * kCellWidth),
        static_cast<float>(pageH) /
            static_cast<float>(page.rows * kMinCellHeight)));
    const int baseMarginX = std::max(1, static_cast<int>(
        std::round(kGridMarginX * pageCellScale)));
    const int baseMarginY = std::max(1, static_cast<int>(
        std::round(kGridMarginY * pageCellScale)));

    auto calculateAxis = [this](int extent, int count, int baseMargin,
        float gapPercent, int& margin, int& cellSize, int& gap)
    {
        const int innerExtent = std::max(count, extent - baseMargin * 2);
        if (count <= 1)
        {
            margin = baseMargin;
            cellSize = std::max(1, innerExtent);
            gap = 0;
            return;
        }

        const float pitch = static_cast<float>(innerExtent) /
            static_cast<float>(count);
        const int maxGap = std::max(0,
            (innerExtent - count) / count);
        const int targetGap = std::clamp(
            static_cast<int>(std::round(
                pitch * gapPercent * iconSpacingScale_)),
            0, maxGap);

        // Half an internal gap is retained at each page edge. The remaining
        // area is divided without ever changing the requested row/column count.
        margin = baseMargin + targetGap / 2;
        const int usableExtent = std::max(count, extent - margin * 2);
        cellSize = std::max(1,
            (usableExtent - targetGap * (count - 1)) / count);
        const int remainingGapSpace = std::max(0,
            usableExtent - count * cellSize);
        gap = (remainingGapSpace + (count - 1) / 2) / (count - 1);
    };

    calculateAxis(pageW, page.columns, baseMarginX, kGapPercentX,
        page.marginX, page.cellWidth, page.gapX);
    calculateAxis(pageH, page.rows, baseMarginY, kGapPercentY,
        page.marginY, page.cellHeight, page.gapY);
}

// ── 拖拽辅助函数 ──────────────────────────────────────────────

extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);
extern inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal);
extern inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span);
extern inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell);

/**
 * @brief 根据坐标计算网格轴上的索引位置（采用最近距离法）。
 * @param page 网格页面。
 * @param coordinate 像素坐标。
 * @param horizontal true 为水平轴，false 为垂直轴。
 * @return 最近的轴索引。
 */
inline int DesktopApp::GetGridAxisIndexFromPoint(const GridPage& page, int coordinate, bool horizontal)
{
    const int count = horizontal ? page.columns : page.rows;
    if (count <= 1) return 0;
    int bestIndex = 0;
    int bestDistance = INT_MAX;
    for (int i = 0; i < count; ++i)
    {
        const int left = (horizontal ? page.workArea.left : page.workArea.top) +
            (horizontal ? page.marginX : page.marginY) + GetGridAxisOffset(page, i, horizontal);
        const int center = left + (horizontal ? page.cellWidth : page.cellHeight) / 2;
        const int distance = std::abs(coordinate - center);
        if (distance < bestDistance) { bestDistance = distance; bestIndex = i; }
    }
    return bestIndex;
}

/**
 * @brief 根据点坐标获取对应的网格单元格（页面 ID + 行列）。
 * @param point 客户区坐标。
 * @return 对应的 GridCell。
 */
inline GridCell DesktopApp::CellFromPoint(POINT point) const
{
    const GridPage* page = GridPageFromPoint(point);
    GridCell cell;
    if (!page) return cell;
    cell.pageId = page->id;
    cell.column = GetGridAxisIndexFromPoint(*page, point.x, true);
    cell.row = GetGridAxisIndexFromPoint(*page, point.y, false);
    return cell;
}

/**
 * @brief 拖拽放置用的网格命中：按左上角边界包含而非中心距离，
 *        使图标左上角越过单元格边界即命中该格，消除半格偏移。
 */
inline GridCell DesktopApp::CellFromPointForDrag(POINT point) const
{
    const GridPage* page = GridPageFromPoint(point);
    GridCell cell;
    if (!page) return cell;
    cell.pageId = page->id;

    auto axisIndexForDrag = [&](bool horizontal) -> int {
        const int count = horizontal ? page->columns : page->rows;
        const int coordinate = horizontal ? point.x : point.y;
        const int margin = horizontal ? page->marginX : page->marginY;
        const int cellSize = horizontal ? page->cellWidth : page->cellHeight;
        const int origin = (horizontal ? page->workArea.left : page->workArea.top) + margin;

        for (int i = 0; i < count; ++i)
        {
            const int cellLeft = origin + GetGridAxisOffset(*page, i, horizontal);
            const int cellRight = cellLeft + cellSize;
            if (coordinate < cellRight)
                return i;
        }
        return count - 1;
    };

    cell.column = axisIndexForDrag(true);
    cell.row = axisIndexForDrag(false);
    return cell;
}

/**
 * @brief 检查网格区域是否被未选中的项目或组件占据。
 * @param cell 起始单元格。
 * @param span 跨度。
 * @return 被占据返回 true。
 */
inline bool DesktopApp::IsGridAreaOccupiedByUnselected(const GridCell& cell, GridSpan span) const
{
    for (const auto& item : items_)
    {
        if (item.selected || item.name.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (item.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = item.gridCell.column + std::max(1, item.gridSpan.columns);
        const int bottom2 = item.gridCell.row + std::max(1, item.gridSpan.rows);
        if (cell.column < right2 && right1 > item.gridCell.column &&
            cell.row < bottom2 && bottom1 > item.gridCell.row)
            return true;
    }
    for (const auto& w : widgets_)
    {
        if (!snowdesktop::collection_group_rules::
                ShouldOccupyDesktopGrid(
                    IsGroupedWidget(w)))
            continue;
        if (w.gridCell.pageId != cell.pageId) continue;
        const int right1 = cell.column + std::max(1, span.columns);
        const int bottom1 = cell.row + std::max(1, span.rows);
        const int right2 = w.gridCell.column + std::max(1, w.gridSpan.columns);
        const int bottom2 = w.gridCell.row + std::max(1, w.gridSpan.rows);
        if (cell.column < right2 && right1 > w.gridCell.column &&
            cell.row < bottom2 && bottom1 > w.gridCell.row)
            return true;
    }
    return false;
}

/**
 * @brief 构建选中项目的移动计划（将选中项目平移到目标单元格并保持相对位置）。
 * @param targetCell 目标单元格。
 * @return 移动计划列表，无法移动时返回空列表。
 */
inline std::vector<DesktopApp::PendingGridMove> DesktopApp::BuildSelectedMove(GridCell targetCell) const
{
    std::vector<PendingGridMove> moves;
    std::vector<size_t> selectedIndexes;
    int minColumn = INT_MAX, minRow = INT_MAX;
    int maxColumn = 0, maxRow = 0;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].selected)
        {
            selectedIndexes.push_back(i);
            minColumn = std::min(minColumn, items_[i].gridCell.column);
            minRow = std::min(minRow, items_[i].gridCell.row);
            maxColumn = std::max(maxColumn, items_[i].gridCell.column + std::max(1, items_[i].gridSpan.columns));
            maxRow = std::max(maxRow, items_[i].gridCell.row + std::max(1, items_[i].gridSpan.rows));
        }
    }
    if (selectedIndexes.empty()) return moves;

    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return moves;

    const int groupColumns = std::max(1, maxColumn - minColumn);
    const int groupRows = std::max(1, maxRow - minRow);
    const bool stacked = (groupColumns == 1 && groupRows == 1 && selectedIndexes.size() > 1);
    const int spreadCols = stacked ? std::min(static_cast<int>(selectedIndexes.size()), page->columns) : groupColumns;
    targetCell.column = std::clamp(targetCell.column, 0, std::max(0, page->columns - spreadCols));
    targetCell.row = std::clamp(targetCell.row, 0, std::max(0, page->rows - groupRows));

    int seqIndex = 0;
    std::unordered_set<std::wstring> usedSlots;
    for (size_t itemIndex : selectedIndexes)
    {
        GridCell movedCell = targetCell;
        if (stacked)
        {
            for (int attempt = 0; attempt < page->columns * page->rows; ++attempt)
            {
                int col = seqIndex / page->rows;
                int row = seqIndex % page->rows;
                GridCell probe = targetCell;
                probe.column += col;
                probe.row += row;
                ++seqIndex;
                std::wstring slotKey = probe.pageId + L":" + std::to_wstring(SlotFromCell(gridPages_, probe));
                if (probe.column <= page->columns - 1 && probe.row <= page->rows - 1 &&
                    !usedSlots.contains(slotKey) &&
                    !IsGridAreaOccupiedByUnselected(probe, items_[itemIndex].gridSpan))
                {
                    movedCell = probe;
                    usedSlots.insert(slotKey);
                    break;
                }
            }
        }
        else
        {
            movedCell.column += items_[itemIndex].gridCell.column - minColumn;
            movedCell.row += items_[itemIndex].gridCell.row - minRow;
        }

        if (!IsGridAreaValid(movedCell, items_[itemIndex].gridSpan) ||
            IsGridAreaOccupiedByUnselected(movedCell, items_[itemIndex].gridSpan))
        {
            moves.clear();
            return moves;
        }
        moves.push_back({ itemIndex, movedCell });
    }
    return moves;
}

/**
 * @brief 寻找最佳的放置单元格（优先沿拖拽方向查找空闲位置）。
 * @param targetCell 初始目标单元格。
 * @return 最佳的可用单元格。
 */
inline GridCell DesktopApp::FindBestDropCell(GridCell targetCell) const
{
    if (!BuildSelectedMove(targetCell).empty()) return targetCell;

    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return targetCell;
    const int maxCol = page->columns - 1;
    const int maxRow = page->rows - 1;

    POINT current = dragSession_.CurrentPoint();
    POINT mouseDown = dragSession_.MouseDownPoint();
    int dx = current.x - mouseDown.x;
    int dy = current.y - mouseDown.y;
    int primaryCol = 0, primaryRow = 0;
    if (std::abs(dx) >= std::abs(dy))
        primaryCol = (dx >= 0) ? 1 : -1;
    else
        primaryRow = (dy >= 0) ? 1 : -1;
    if (primaryCol == 0 && primaryRow == 0) primaryCol = 1;

    for (int dist = 1; dist <= 8; ++dist)
    {
        GridCell probe = targetCell;
        probe.column += primaryCol * dist;
        probe.row += primaryRow * dist;
        if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow) break;
        if (!BuildSelectedMove(probe).empty()) return probe;
    }

    int oppCol = -primaryCol, oppRow = -primaryRow;
    for (int dist = 1; dist <= 8; ++dist)
    {
        GridCell probe = targetCell;
        probe.column += oppCol * dist;
        probe.row += oppRow * dist;
        if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow) break;
        if (!BuildSelectedMove(probe).empty()) return probe;
    }

    for (int dist = 1; dist <= 6; ++dist)
    {
        for (int dc = -dist; dc <= dist; ++dc)
        {
            for (int dr = -dist; dr <= dist; ++dr)
            {
                if (std::abs(dc) != dist && std::abs(dr) != dist) continue;
                GridCell probe = targetCell;
                probe.column += dc;
                probe.row += dr;
                if (probe.column < 0 || probe.column > maxCol || probe.row < 0 || probe.row > maxRow) continue;
                if (!BuildSelectedMove(probe).empty()) return probe;
            }
        }
    }
    return targetCell;
}

/**
 * @brief 将选中的项目移动到目标网格单元格。
 * @param targetCell 目标单元格。
 */
inline void DesktopApp::MoveSelectedItemsToCell(GridCell targetCell)
{
    std::vector<PendingGridMove> moves = BuildSelectedMove(std::move(targetCell));
    if (moves.empty()) return;
    for (const auto& move : moves)
    {
        items_[move.index].gridCell = move.cell;
        items_[move.index].slot = SlotFromCell(gridPages_, move.cell);
    }
    LayoutItems();
    SaveLayoutSlots();
}

/**
 * @brief 更新拖拽组的原点位置（用于计算拖拽时的偏移）。
 */
inline void DesktopApp::UpdateDragGroupOrigin()
{
    int minCol = INT_MAX, minRow = INT_MAX;
    std::wstring anchorPageId;
    for (const auto& item : items_)
    {
        if (item.selected)
        {
            if (anchorPageId.empty()) anchorPageId = item.gridCell.pageId;
            minCol = std::min(minCol, item.gridCell.column);
            minRow = std::min(minRow, item.gridCell.row);
        }
    }
    GridCell groupOrigin;
    const GridPage* firstPage = GetFirstPageGridPage();
    groupOrigin.pageId = anchorPageId.empty()
        ? (firstPage ? firstPage->id : L"")
        : anchorPageId;
    groupOrigin.column = minCol != INT_MAX ? minCol : 0;
    groupOrigin.row = minRow != INT_MAX ? minRow : 0;
    RECT groupRect = GetGridRect(gridPages_, groupOrigin, GridSpan{});
    dragGroupOriginX_ = groupRect.left;
    dragGroupOriginY_ = groupRect.top;
}

/**
 * @brief 将选中的项目迁移到最后一个监视器页面。
 */
inline void DesktopApp::MigrateSelectedItemsToLastMonitorPage()
{
    if (gridPages_.empty() || lastMonitorPageId_.empty()) return;
    const GridPage* targetPage = FindGridPage(gridPages_, lastMonitorPageId_);
    if (!targetPage) return;

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& item : items_)
    {
        if (item.selected) continue;
        if (item.name.empty()) continue;
        if (item.gridCell.pageId == lastMonitorPageId_)
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);

    for (auto& item : items_)
    {
        if (!item.selected) continue;
        if (item.gridCell.pageId == lastMonitorPageId_) continue;

        GridCell newCell;
        newCell.pageId = lastMonitorPageId_;
        newCell.column = std::min(item.gridCell.column, std::max(0, targetPage->columns - 1));
        newCell.row = std::min(item.gridCell.row, std::max(0, targetPage->rows - 1));

        GridSpan span = item.gridSpan;
        span.columns = std::clamp(span.columns, 1, std::max(1, targetPage->columns));
        span.rows = std::clamp(span.rows, 1, std::max(1, targetPage->rows));

        if (!AreGridSlotsMarked(usedSlots, newCell, span))
        {
            MarkGridArea(usedSlots, newCell, span);
            item.gridCell = newCell;
            item.gridSpan = span;
            continue;
        }

        bool found = false;
        for (int r = 0; r < targetPage->rows && !found; ++r)
        {
            for (int c = 0; c < targetPage->columns && !found; ++c)
            {
                GridCell tryCell{ lastMonitorPageId_, c, r };
                if (!AreGridSlotsMarked(usedSlots, tryCell, span))
                {
                    MarkGridArea(usedSlots, tryCell, span);
                    item.gridCell = tryCell;
                    item.gridSpan = span;
                    found = true;
                }
            }
        }
    }
}

/**
 * @brief 获取拖拽目标点的屏幕坐标。
 * @param current 当前鼠标位置。
 * @return 拖拽目标点。
 */
inline POINT DesktopApp::GetDragTargetPoint(POINT current) const
{
    return {
        dragGroupOriginX_ + (current.x - mouseDownPoint_.x),
        dragGroupOriginY_ + (current.y - mouseDownPoint_.y)
    };
}

/**
 * @brief 为选中的桌面项创建 IDataObject（用于拖拽/剪贴板）。
 * @return COM 数据对象，失败返回 nullptr。
 */
inline ComPtr<IDataObject> DesktopApp::CreateSelectedDataObject() const
{
    std::vector<PCUITEMID_CHILD> pidls;
    for (const auto& item : items_)
    {
        if (item.selected)
            pidls.push_back(reinterpret_cast<PCUITEMID_CHILD>(item.childPidl.get()));
    }
    if (pidls.empty()) return nullptr;

    ComPtr<IDataObject> dataObject;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, static_cast<UINT>(pidls.size()), pidls.data(),
        IID_IDataObject, nullptr,
        reinterpret_cast<void**>(dataObject.GetAddressOf()));
    if (FAILED(hr)) return nullptr;
    return dataObject;
}

/**
 * @brief 为指定文件路径列表创建文件拖拽数据对象。
 * @param paths 文件路径列表。
 * @return COM 数据对象，失败返回 nullptr。
 */
inline ComPtr<IDataObject> DesktopApp::CreateFileDropDataObject(const std::vector<std::wstring>& paths)
{
    if (paths.empty()) return nullptr;

    std::vector<PIDLIST_ABSOLUTE> pidls;
    pidls.reserve(paths.size());
    for (const auto& path : paths)
    {
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) && pidl)
            pidls.push_back(pidl);
    }

    if (pidls.empty()) return nullptr;

    auto freePidls = [&]() {
        for (auto* pidl : pidls)
            ILFree(pidl);
    };

    ComPtr<IShellFolder> parentFolder;
    PCUITEMID_CHILD unusedChild = nullptr;
    HRESULT hr = SHBindToParent(pidls.front(), IID_IShellFolder,
        reinterpret_cast<void**>(parentFolder.GetAddressOf()), &unusedChild);
    if (FAILED(hr) || !parentFolder)
    {
        freePidls();
        return nullptr;
    }

    std::vector<PCUITEMID_CHILD> children;
    children.reserve(pidls.size());
    for (auto* pidl : pidls)
        children.push_back(ILFindLastID(pidl));

    ComPtr<IDataObject> dataObject;
    hr = parentFolder->GetUIObjectOf(nullptr, static_cast<UINT>(children.size()), children.data(),
        IID_IDataObject, nullptr, reinterpret_cast<void**>(dataObject.GetAddressOf()));
    freePidls();

    if (FAILED(hr)) return nullptr;
    return dataObject;
}

/**
 * @brief 根据源项目列表创建数据对象（桌面图标优先，否则用文件路径）。
 * @param sourceItems 源项目列表。
 * @return COM 数据对象，失败返回 nullptr。
 */
inline ComPtr<IDataObject> DesktopApp::CreateDataObjectForItems(
    const std::vector<Item*>& sourceItems) const
{
    DropPayload payload = DropPayload::From(sourceItems);
    if (payload.hasDesktopIcons)
    {
        if (ComPtr<IDataObject> desktopObject = CreateSelectedDataObject())
            return desktopObject;
    }
    return CreateFileDropDataObject(payload.filePaths);
}

/**
 * @brief 将选中项目拖拽放置到目标桌面项上（调用 Shell IDropTarget 接口）。
 * @param targetIndex 目标桌面项的索引。
 */
inline void DesktopApp::DropSelectedItemsOnTarget(int targetIndex)
{
    if (targetIndex < 0 || static_cast<size_t>(targetIndex) >= items_.size()) return;
    auto& targetItem = items_[targetIndex];

    ComPtr<IDataObject> dataObj = CreateSelectedDataObject();
    if (!dataObj) return;

    PCUITEMID_CHILD child = reinterpret_cast<PCUITEMID_CHILD>(targetItem.childPidl.get());
    ComPtr<IDropTarget> dropTarget;
    HRESULT hr = desktopFolder_->GetUIObjectOf(
        hwnd_, 1, &child, IID_IDropTarget, nullptr,
        reinterpret_cast<void**>(dropTarget.GetAddressOf()));
    if (FAILED(hr) || !dropTarget) return;

    POINT screenPt = dragSession_.CurrentPoint();
    ClientToScreen(hwnd_, &screenPt);
    POINTL screenPtL{ screenPt.x, screenPt.y };

    DWORD effect = DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK;
    hr = dropTarget->DragEnter(dataObj.Get(), MK_LBUTTON, screenPtL, &effect);
    if (SUCCEEDED(hr))
        hr = dropTarget->DragOver(MK_LBUTTON, screenPtL, &effect);
    if (SUCCEEDED(hr))
        hr = dropTarget->Drop(dataObj.Get(), MK_LBUTTON, screenPtL, &effect);
    else
        dropTarget->DragLeave();
}

/**
 * @brief 根据布局键查找项目索引。
 * @param key 项目布局键。
 * @return 项目索引，未找到返回 -1。
 */
inline size_t DesktopApp::FindItemIndexByKey(const std::wstring& key) const
{
    std::wstring normalized = ToUpperInvariant(key);
    auto it = itemIndexByKeyCache_.find(normalized);
    if (it != itemIndexByKeyCache_.end() && it->second < items_.size())
        return it->second;
    return static_cast<size_t>(-1);
}

inline void DesktopApp::RefreshDesktopItemIndexCache()
{
    itemIndexByKeyCache_.clear();
    itemIndexByKeyCache_.reserve(items_.size());
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (!items_[i].layoutKey.empty())
            itemIndexByKeyCache_.emplace(ToUpperInvariant(items_[i].layoutKey), i);
    }
}

inline void DesktopApp::RefreshCollectedKeysCache()
{
    collectedKeysCache_.clear();
    for (const auto& widget : widgets_)
    {
        for (const auto& key : widget.itemKeys)
            if (!key.empty())
                collectedKeysCache_.insert(ToUpperInvariant(key));
    }
    for (const auto& entry : dockEntries_)
    {
        if (entry.type == DockEntryType::DesktopItem && !entry.keepOnDesktop &&
            !entry.reference.empty())
            collectedKeysCache_.insert(ToUpperInvariant(entry.reference));
    }
}

/**
 * @brief 从所有组件中移除指定桌面键。
 * @param keys 要移除的布局键列表。
 */
inline void DesktopApp::RemoveDesktopKeysFromWidgets(const std::vector<std::wstring>& keys)
{
    if (keys.empty()) return;

    std::vector<std::wstring> normalizedKeys;
    normalizedKeys.reserve(keys.size());
    for (const auto& key : keys)
        normalizedKeys.push_back(ToUpperInvariant(key));

    for (auto& widget : widgets_)
    {
        if (widget.type == DesktopWidgetType::FolderMapping)
            continue;
        widget.itemKeys.erase(
            std::remove_if(widget.itemKeys.begin(), widget.itemKeys.end(),
                [&](const std::wstring& existing) {
                    std::wstring normalizedExisting = ToUpperInvariant(existing);
                    return std::find(normalizedKeys.begin(), normalizedKeys.end(),
                        normalizedExisting) != normalizedKeys.end();
                }),
            widget.itemKeys.end());
    }
    RefreshCollectedKeysCache();
}

/**
 * @brief 快照当前所有桌面项的布局键。
 * @return 布局键的集合。
 */
inline std::unordered_set<std::wstring> DesktopApp::SnapshotDesktopKeys() const
{
    std::unordered_set<std::wstring> keys;
    for (const auto& item : items_)
        if (!item.layoutKey.empty())
            keys.insert(ToUpperInvariant(item.layoutKey));
    return keys;
}

/**
 * @brief 获取自快照以来新增的桌面项布局键。
 * @param existingKeys 之前的键快照。
 * @return 新增的键列表。
 */
inline std::vector<std::wstring> DesktopApp::NewDesktopKeysSince(
    const std::unordered_set<std::wstring>& existingKeys) const
{
    std::vector<std::wstring> keys;
    for (const auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!key.empty() && !existingKeys.contains(key))
            keys.push_back(key);
    }
    return keys;
}

/**
 * @brief 构建桌面放置列表，为拖拽源中的每个条目分配网格位置。
 * @param sourceList 拖拽源列表。
 * @param targetCell 目标网格单元格。
 * @param internalMove 是否为内部移动。
 * @return 放置操作列表。
 */
inline std::vector<DropLanding> DesktopApp::BuildDesktopLandings(
    const DragSourceList& sourceList, GridCell targetCell, bool internalMove) const
{
    std::vector<DropLanding> landings;
    if (sourceList.Empty()) return landings;

    if (targetCell.pageId.empty())
    {
        const GridPage* firstPage = GetFirstPageGridPage();
        if (firstPage) targetCell.pageId = firstPage->id;
    }
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return landings;

    bool desktopToDesktopMove = internalMove && sourceList.hasDesktopIcons &&
        !containers_.empty() && sourceList.origin == containers_.front().get();
    if (desktopToDesktopMove)
    {
        std::vector<PendingGridMove> moves = BuildSelectedMove(targetCell);
        if (moves.empty()) return landings;

        for (const auto& entry : sourceList.entries)
        {
            auto it = std::find_if(moves.begin(), moves.end(),
                [&](const PendingGridMove& move) { return move.index == entry.desktopIndex; });
            if (it == moves.end()) continue;
            DropLanding landing;
            landing.kind = DropLandingKind::DesktopCell;
            landing.sourceIndex = entry.sourceIndex;
            landing.cell = it->cell;
            landing.span = entry.originalSpan;
            landings.push_back(landing);
        }
        return landings;
    }

    // 起始列：拖放目标列，换行时从该列另起一行而非从 0 开始
    const int startCol = std::clamp(targetCell.column, 0, std::max(0, page->columns - 1));

    auto advanceCell = [&](GridCell cell, GridSpan span) {
        // 查找 cell 所在页的维度（支持跨页后 cursor 切到新页）
        int cols = page->columns, rows = page->rows;
        if (cell.pageId != page->id)
        {
            auto colIt = savedPageColumns_.find(cell.pageId);
            auto rowIt = savedPageRows_.find(cell.pageId);
            if (colIt != savedPageColumns_.end()) cols = colIt->second;
            if (rowIt != savedPageRows_.end()) rows = rowIt->second;
        }
        cell.column += std::max(1, span.columns);
        if (cell.column + span.columns > cols)
        {
            cell.column = startCol;
            cell.row += std::max(1, span.rows);
            // 到底后绕回起始行上方（搜索阶段会跳过已占位置）
            if (cell.row + span.rows > rows)
                cell.row = 0;
        }
        return cell;
    };

    std::unordered_set<std::wstring> sourceKeys;
    for (const auto& entry : sourceList.entries)
        if (!entry.desktopKey.empty())
            sourceKeys.insert(ToUpperInvariant(entry.desktopKey));

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || item.gridCell.pageId.empty()) continue;
        if (IsItemInAnyWidget(item)) continue;
        if (internalMove && sourceKeys.contains(ToUpperInvariant(item.layoutKey))) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell cursor = targetCell;

    // 阶段式搜索：1) 从 cursor 向右 + 下方行从 startCol 开始
    //             1b) 上方行从 startCol 开始（绕回页面顶部填间隙）
    //             1c) 下方/上方行从列 0..startCol-1 补扫（覆盖左侧空位）
    //             2) 全页行优先搜索（兜底）
    //             3) TryFindFreeCell 跨页搜索
    auto tryPlaceRightward = [&](GridSpan span, GridCell fromCell, GridCell& outCell) -> bool {
        // 获取 fromCell 所在页的维度（支持跨页后 cursor 切到新页）
        int pageCols = page->columns, pageRows = page->rows;
        if (fromCell.pageId != page->id)
        {
            auto colIt = savedPageColumns_.find(fromCell.pageId);
            auto rowIt = savedPageRows_.find(fromCell.pageId);
            if (colIt != savedPageColumns_.end()) pageCols = colIt->second;
            if (rowIt != savedPageRows_.end()) pageRows = rowIt->second;
        }

        // 当前行从 fromCell.column 向右找
        for (int c = fromCell.column; c + span.columns <= pageCols; ++c)
        {
            GridCell candidate{ fromCell.pageId, c, fromCell.row };
            if (!AreGridSlotsMarked(usedSlots, candidate, span))
            {
                outCell = candidate;
                return true;
            }
        }

        // 下方行：先从 startCol 向右，再从 0..startCol-1 补扫
        for (int r = fromCell.row + 1; r + span.rows <= pageRows; ++r)
        {
            for (int c = startCol; c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
            for (int c = 0; c < startCol && c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
        }

        // 上方行：先从 startCol 向右，再从 0..startCol-1 补扫
        for (int r = 0; r < fromCell.row && r + span.rows <= pageRows; ++r)
        {
            for (int c = startCol; c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
            for (int c = 0; c < startCol && c + span.columns <= pageCols; ++c)
            {
                GridCell candidate{ fromCell.pageId, c, r };
                if (!AreGridSlotsMarked(usedSlots, candidate, span))
                {
                    outCell = candidate;
                    return true;
                }
            }
        }
        return false;
    };

    for (const auto& entry : sourceList.entries)
    {
        GridSpan span = entry.originalSpan;
        span.columns = std::max(1, span.columns);
        span.rows = std::max(1, span.rows);

        GridCell cell{};
        bool found = false;

        // 获取 cursor 所在页的维度（支持跨页后 cursor 切到新页）
        auto getPageSize = [&](const std::wstring& pid) -> std::pair<int,int> {
            if (pid == page->id) return { page->columns, page->rows };
            auto colIt = savedPageColumns_.find(pid);
            auto rowIt = savedPageRows_.find(pid);
            int c = (colIt != savedPageColumns_.end()) ? colIt->second : page->columns;
            int r = (rowIt != savedPageRows_.end()) ? rowIt->second : page->rows;
            return { c, r };
        };

        // 阶段 1：从 cursor 向右 + 下方行从 startCol
        auto [curCols, curRows] = getPageSize(cursor.pageId);
        if (IsGridAreaValid(cursor, span) && cursor.column + span.columns <= curCols &&
            cursor.row + span.rows <= curRows &&
            !AreGridSlotsMarked(usedSlots, cursor, span))
        {
            cell = cursor;
            found = true;
        }
        else
        {
            found = tryPlaceRightward(span, cursor, cell);
        }

        // 阶段 2：全页行优先搜索（兜底，优先本页）
        if (!found)
        {
            for (int r = 0; r + span.rows <= curRows && !found; ++r)
            {
                for (int c = 0; c + span.columns <= curCols && !found; ++c)
                {
                    GridCell candidate{ cursor.pageId, c, r };
                    if (!AreGridSlotsMarked(usedSlots, candidate, span))
                    {
                        cell = candidate;
                        found = true;
                    }
                }
            }
        }

        // 阶段 3：跨页搜索（TryFindFreeCell 内部遍历其他显示页）
        if (!found)
        {
            found = TryFindFreeCell(span, usedSlots, cell, cursor.pageId, 0);
        }

        // 阶段 4：所有现有页都满了 → 预分配新溢出页
        if (!found)
        {
            std::wstring newPageId = GeneratePageId();
            // GeneratePageId 保证唯一，savedPageColumns_ 不会有该页
            if (!savedPageColumns_.count(newPageId))
            {
                cell = { newPageId, 0, 0 };
                found = true;
                // 新页维度由 ApplyPendingPlacement 创建时参考末屏设置
            }
        }

        if (!found) continue;

        DropLanding landing;
        landing.kind = DropLandingKind::DesktopCell;
        landing.sourceIndex = entry.sourceIndex;
        landing.cell = cell;
        landing.span = span;
        landings.push_back(landing);

        MarkGridArea(usedSlots, cell, span);
        cursor = advanceCell(cell, span);
    }
    return landings;
}

/**
 * @brief 根据源项目和所属容器构建拖拽源列表。
 * @param sourceItems 源项目指针列表。
 * @param origin 来源容器。
 * @return 拖拽源列表。
 */
inline DragSourceList DesktopApp::BuildDragSourceList(
    const std::vector<Item*>& sourceItems, Container* origin) const
{
    DragSourceList list;
    list.origin = origin;
    if (origin)
    {
        list.originSurface =
            origin->GetSlotSurfaceKind();
        list.hasOriginSurface = true;
    }

    WidgetContainer* originWidget = dynamic_cast<WidgetContainer*>(origin);
    FileGroup* fileGroupOrigin =
        dynamic_cast<FileGroup*>(originWidget);
    const bool fileGroupLabelDrag =
        std::any_of(
            sourceItems.begin(), sourceItems.end(),
            [](Item* item) {
                return dynamic_cast<
                    FileGroupEntryItem*>(item) != nullptr;
            });
    if (fileGroupOrigin && !fileGroupLabelDrag)
    {
        ScrollingItemWidget* logicalSource =
            !sourceItems.empty()
                ? fileGroupOrigin->
                    GetSourceContainerForItem(
                        sourceItems.front())
                : nullptr;
        if (!logicalSource)
            logicalSource =
                fileGroupOrigin->
                    GetActiveSourceContainer();
        if (logicalSource)
        {
            originWidget = logicalSource;
            list.origin = logicalSource;
        }
    }
    DesktopWidget* originData = originWidget ? originWidget->GetWidgetData() : nullptr;
    if (originData)
    {
        list.hasOriginWidget = true;
        list.originWidgetId = originData->id;
        list.originWidgetType = originData->type;
    }

    for (auto* src : sourceItems)
    {
        if (!src) continue;
        DragSourceEntry entry;
        entry.item = src;
        entry.sourceIndex = list.entries.size();
        entry.displayName = src->GetTitle();
        entry.filePath = src->GetPath();

        if (auto* dockItem = dynamic_cast<DockEntryItem*>(src))
        {
            entry.fromDock = true;
            entry.dockReference = dockItem->GetReference();
            entry.dockEntryType = dockItem->GetEntryType();
            if (entry.dockEntryType == DockEntryType::DesktopItem)
            {
                entry.kind = DropSourceKind::DesktopIcon;
                entry.desktopKey = entry.dockReference;
                entry.desktopIndex = FindItemIndexByKey(entry.desktopKey);
                list.hasDesktopIcons = true;
                if (entry.desktopIndex < items_.size())
                {
                    const DesktopItem& item = items_[entry.desktopIndex];
                    entry.filePath = item.parsingName;
                    entry.originalCell = item.gridCell;
                    entry.originalSpan = item.gridSpan;
                    entry.protectedDesktopIcon = IsProtectedDesktopIcon(item);
                }
            }
            else
            {
                entry.kind = DropSourceKind::Widget;
                list.hasWidgets = true;
                size_t widgetIndex = FindWidgetIndexById(entry.dockReference);
                if (widgetIndex < widgets_.size())
                {
                    entry.originalCell = widgets_[widgetIndex].gridCell;
                    entry.originalSpan = widgets_[widgetIndex].gridSpan;
                }
            }
        }
        else if (auto* frequentItem = dynamic_cast<DockFrequentItem*>(src))
        {
            entry.fromDock = true;
            entry.kind = DropSourceKind::DesktopIcon;
            entry.desktopIndex = frequentItem->GetItemIndex();
            list.hasDesktopIcons = true;
            if (entry.desktopIndex < items_.size())
            {
                const DesktopItem& item = items_[entry.desktopIndex];
                entry.desktopKey = item.layoutKey;
                entry.filePath = item.parsingName;
                entry.originalCell = item.gridCell;
                entry.originalSpan = item.gridSpan;
            }
        }
        else if (dynamic_cast<Widget*>(src))
        {
            entry.kind = DropSourceKind::Widget;
            list.hasWidgets = true;
        }
        else if (auto* groupEntry =
            dynamic_cast<CollectionGroupEntryItem*>(src))
        {
            entry.kind = DropSourceKind::CollectionGroupEntry;
            entry.widgetId = groupEntry->GetCollectionId();
            list.hasCollectionGroupEntries = true;
            const size_t widgetIndex =
                FindWidgetIndexById(entry.widgetId);
            if (widgetIndex < widgets_.size())
            {
                entry.originalCell = widgets_[widgetIndex].gridCell;
                entry.originalSpan = widgets_[widgetIndex].gridSpan;
            }
        }
        else if (auto* fileGroupEntry =
            dynamic_cast<FileGroupEntryItem*>(src))
        {
            entry.widgetId =
                fileGroupEntry->GetChildWidgetId();
            const size_t widgetIndex =
                FindWidgetIndexById(entry.widgetId);
            if (widgetIndex < widgets_.size())
            {
                entry.originalCell = widgets_[widgetIndex].gridCell;
                entry.originalSpan = widgets_[widgetIndex].gridSpan;
                if (widgets_[widgetIndex].type ==
                        DesktopWidgetType::FolderMapping)
                {
                    entry.kind = DropSourceKind::Widget;
                    entry.dockEntryType =
                        DockEntryType::FolderMapping;
                    list.hasWidgets = true;
                }
                else
                {
                    entry.kind =
                        DropSourceKind::FileGroupEntry;
                    list.hasFileGroupEntries = true;
                }
            }
        }
        else if (auto* icon = dynamic_cast<DesktopIcon*>(src))
        {
            entry.kind = DropSourceKind::DesktopIcon;
            list.hasDesktopIcons = true;
            if (originData &&
                originData->type == DesktopWidgetType::CollectionGroup)
            {
                auto* group =
                    dynamic_cast<CollectionGroup*>(
                        originWidget);
                entry.widgetId = group
                    ? group->GetActiveCollectionId()
                    : originData->activeCategoryId;
            }
            else if (fileGroupOrigin)
            {
                if (auto* logicalSource =
                        fileGroupOrigin->
                            GetSourceContainerForItem(src))
                {
                    if (DesktopWidget* logicalData =
                            logicalSource->
                                GetWidgetData())
                        entry.widgetId =
                            logicalData->id;
                }
            }
            if (DesktopItem* item = icon->GetDesktopItem())
            {
                entry.desktopKey = item->layoutKey;
                entry.desktopIndex = FindItemIndexByKey(item->layoutKey);
                entry.originalCell = item->gridCell;
                entry.originalSpan = item->gridSpan;
                entry.protectedDesktopIcon = IsProtectedDesktopIcon(*item);
                if (entry.filePath.empty() && !entry.protectedDesktopIcon)
                    entry.filePath = icon->GetPath();
            }
        }
        else if (dynamic_cast<FolderEntryIcon*>(src))
        {
            entry.kind = DropSourceKind::FolderEntry;
            list.hasFolderEntries = true;
            if (fileGroupOrigin)
            {
                if (auto* logicalSource =
                        fileGroupOrigin->
                            GetSourceContainerForItem(src))
                {
                    if (DesktopWidget* logicalData =
                            logicalSource->
                                GetWidgetData())
                        entry.widgetId =
                            logicalData->id;
                }
            }
        }
        else if (dynamic_cast<ExternalFileItem*>(src))
        {
            entry.kind = DropSourceKind::ExternalFile;
            list.hasExternalFiles = true;
        }

        if (originData)
        {
            if (originData->type == DesktopWidgetType::FolderMapping)
            {
                auto it = std::find_if(originData->folderEntries.begin(), originData->folderEntries.end(),
                    [&](const FolderEntry& folderEntry) {
                        return PathsEqualInsensitive(folderEntry.fullPath, entry.filePath);
                    });
                if (it != originData->folderEntries.end())
                    entry.memberIndex = static_cast<size_t>(std::distance(originData->folderEntries.begin(), it));
            }
            else if (originData->type ==
                DesktopWidgetType::CollectionGroup)
            {
                if (entry.kind ==
                        DropSourceKind::CollectionGroupEntry &&
                    !entry.widgetId.empty())
                {
                    auto it = std::find(
                        originData->childWidgetIds.begin(),
                        originData->childWidgetIds.end(),
                        entry.widgetId);
                    if (it != originData->childWidgetIds.end())
                        entry.memberIndex = static_cast<size_t>(
                            std::distance(
                                originData->childWidgetIds.begin(), it));
                }
                else if (entry.kind ==
                             DropSourceKind::DesktopIcon &&
                         !entry.widgetId.empty() &&
                         !entry.desktopKey.empty())
                {
                    const size_t childIndex =
                        FindWidgetIndexById(entry.widgetId);
                    if (childIndex < widgets_.size())
                    {
                        const auto& childKeys =
                            widgets_[childIndex].itemKeys;
                        auto it = std::find_if(
                            childKeys.begin(), childKeys.end(),
                            [&](const std::wstring& key) {
                                return ToUpperInvariant(key) ==
                                    ToUpperInvariant(entry.desktopKey);
                            });
                        if (it != childKeys.end())
                            entry.memberIndex =
                                static_cast<size_t>(
                                    std::distance(
                                        childKeys.begin(), it));
                    }
                }
            }
            else if (originData->type ==
                DesktopWidgetType::FileGroup)
            {
                if (entry.kind ==
                        DropSourceKind::FileGroupEntry &&
                    !entry.widgetId.empty())
                {
                    auto it = std::find(
                        originData->childWidgetIds.begin(),
                        originData->childWidgetIds.end(),
                        entry.widgetId);
                    if (it != originData->childWidgetIds.end())
                        entry.memberIndex =
                            static_cast<size_t>(
                                std::distance(
                                    originData->childWidgetIds.begin(),
                                    it));
                }
            }
            else if (!entry.desktopKey.empty())
            {
                auto it = std::find_if(originData->itemKeys.begin(), originData->itemKeys.end(),
                    [&](const std::wstring& key) {
                        return ToUpperInvariant(key) == ToUpperInvariant(entry.desktopKey);
                    });
                if (it != originData->itemKeys.end())
                    entry.memberIndex = static_cast<size_t>(std::distance(originData->itemKeys.begin(), it));
            }
        }

        if (entry.originalSpan.columns <= 0) entry.originalSpan.columns = 1;
        if (entry.originalSpan.rows <= 0) entry.originalSpan.rows = 1;
        list.entries.push_back(entry);
    }
    return list;
}

/**
 * @brief 判断拖拽操作是否需要文件系统支持（复制/链接到文件夹映射等场景）。
 * @param sourceList 拖拽源列表。
 * @param targetKind 目标类型。
 * @param action 拖拽动作。
 * @return 需要文件系统支持返回 true。
 */
inline bool DesktopApp::IsDropFileBacked(const DragSourceList& sourceList,
    DropTargetKind targetKind, DropAction action) const
{
    if (sourceList.Empty()) return false;
    if (targetKind == DropTargetKind::FolderMapping) return true;
    if (action == DropAction::Copy || action == DropAction::Link) return true;
    return sourceList.hasExternalFiles || sourceList.hasFolderEntries;
}

inline bool DesktopApp::IsAutoCollectFileCategorySource(
    const DragSourceList& sourceList) const
{
    if (!sourceList.hasOriginWidget ||
        sourceList.originWidgetType != DesktopWidgetType::FileCategories)
        return false;

    auto it = std::find_if(widgets_.begin(), widgets_.end(),
        [&](const DesktopWidget& widget) {
            return widget.id == sourceList.originWidgetId;
        });
    return it != widgets_.end() && it->autoCollect;
}

/**
 * @brief 构建拖拽预览列表，计算放置目标、动作和落点。
 * @param sourceList 拖拽源列表。
 * @param target 目标容器。
 * @param targetSlot 目标槽位。
 * @param region 命中区域。
 * @param mods 修饰键。
 * @param dropPoint 放置点坐标。
 * @return 拖拽预览列表。
 */
inline DropPreviewList DesktopApp::BuildDropPreviewList(const DragSourceList& sourceList,
    Container* target, Slot* targetSlot, HitRegion region, int mods, POINT dropPoint) const
{
    DropPreviewList preview;
    preview.targetContainer = target;
    if (sourceList.Empty() || !target || sourceList.hasWidgets || region == HitRegion::Handoff)
        return preview;
    if (!AcceptsSlotSurfaceDrop(target, sourceList))
        return preview;

    DropAction defaultAction = sourceList.hasExternalFiles ? DropAction::Copy : DropAction::Move;
    preview.action = DropActionFromMods(mods, defaultAction);

    if (!containers_.empty() && target == containers_.front().get())
    {
        preview.targetKind = DropTargetKind::Desktop;
        if (sourceList.hasCollectionGroupEntries ||
            sourceList.hasFileGroupEntries)
        {
            preview.action = DropAction::Move;
            preview.fileBacked = false;
            GridCell targetCell =
                CellFromPointForDrag(dropPoint);
            preview.anchorCell = targetCell;
            const GridPage* page =
                FindGridPage(gridPages_, targetCell.pageId);
            if (!page) return preview;

            std::unordered_set<std::wstring> usedSlots;
            for (const auto& widget : widgets_)
                if (!IsGroupedWidget(widget))
                    MarkGridArea(
                        usedSlots, widget.gridCell,
                        widget.gridSpan);
            for (const auto& item : items_)
                if (!item.name.empty() &&
                    !IsItemInAnyWidget(item))
                    MarkGridArea(
                        usedSlots, item.gridCell,
                        item.gridSpan);

            std::vector<const DragSourceEntry*>
                groupEntries;
            std::vector<
                snowdesktop::collection_group_rules::Span>
                requestedSpans;
            for (const auto& entry : sourceList.entries)
            {
                const bool matchingEntry =
                    sourceList.hasCollectionGroupEntries
                        ? entry.kind ==
                            DropSourceKind::CollectionGroupEntry
                        : entry.kind ==
                            DropSourceKind::FileGroupEntry;
                if (!matchingEntry)
                    continue;
                groupEntries.push_back(&entry);
                requestedSpans.push_back({
                    entry.originalSpan.columns,
                    entry.originalSpan.rows
                });
            }
            const auto placements =
                snowdesktop::collection_group_rules::
                    PlanExactPlacements(
                        page->columns, page->rows,
                        targetCell.column,
                        targetCell.row,
                        requestedSpans,
                        [&](const auto& placement) {
                            return AreGridSlotsMarked(
                                usedSlots,
                                {
                                    targetCell.pageId,
                                    placement.column,
                                    placement.row
                                },
                                {
                                    placement.span.columns,
                                    placement.span.rows
                                });
                        },
                        [&](const auto& placement) {
                            MarkGridArea(
                                usedSlots,
                                {
                                    targetCell.pageId,
                                    placement.column,
                                    placement.row
                                },
                                {
                                    placement.span.columns,
                                    placement.span.rows
                                });
                        });
            if (!placements)
                return preview;

            for (size_t i = 0;
                i < placements->size(); ++i)
            {
                const auto& placement =
                    (*placements)[i];
                DropLanding landing;
                landing.kind =
                    DropLandingKind::DesktopCell;
                landing.sourceIndex =
                    groupEntries[i]->sourceIndex;
                landing.cell = {
                    targetCell.pageId,
                    placement.column,
                    placement.row
                };
                landing.span = {
                    placement.span.columns,
                    placement.span.rows
                };
                preview.landings.push_back(landing);
            }
            return preview;
        }
        if (preview.action == DropAction::Move &&
            IsAutoCollectFileCategorySource(sourceList))
            return preview;

        const bool usePointerCell =
            sourceList.hasOriginWidget &&
            sourceList.originWidgetType ==
                DesktopWidgetType::CollectionGroup;
        POINT adjusted = !sourceList.origin || usePointerCell
            ? dropPoint
            : GetDragTargetPoint(dropPoint);
        GridCell targetCell = CellFromPointForDrag(adjusted);
        bool internalMove = !IsDropFileBacked(sourceList, preview.targetKind, preview.action);
        if (internalMove)
            targetCell = FindBestDropCell(targetCell);
        preview.anchorCell = targetCell;
        preview.fileBacked = !internalMove;
        preview.landings = BuildDesktopLandings(sourceList, targetCell, internalMove);
        return preview;
    }

    if (auto* widget = dynamic_cast<WidgetContainer*>(target))
    {
        preview.targetWidget = widget->GetWidgetData();
        if (sourceList.hasCollectionGroupEntries ||
            sourceList.hasFileGroupEntries)
        {
            const DesktopWidgetType expectedGroup =
                sourceList.hasCollectionGroupEntries
                    ? DesktopWidgetType::CollectionGroup
                    : DesktopWidgetType::FileGroup;
            if (!preview.targetWidget ||
                preview.targetWidget->type != expectedGroup)
                return preview;
            preview.targetKind = DropTargetKind::KeyedWidget;
            preview.action = DropAction::Move;
            preview.fileBacked = false;
            preview.insertIndex =
                widget->GetDropInsertIndex(targetSlot, region);
            const bool emptyGroupTabPoint =
                (dynamic_cast<CollectionGroup*>(widget) &&
                    dynamic_cast<CollectionGroup*>(widget)->
                        CategoryIdAtPoint(dropPoint).empty()) ||
                (dynamic_cast<FileGroup*>(widget) &&
                    dynamic_cast<FileGroup*>(widget)->
                        SourceIdAtPoint(dropPoint).empty());
            if (emptyGroupTabPoint)
                preview.insertIndex =
                    preview.targetWidget->childWidgetIds.size();
            for (const auto& entry : sourceList.entries)
            {
                const bool matchingEntry =
                    sourceList.hasCollectionGroupEntries
                        ? entry.kind ==
                            DropSourceKind::CollectionGroupEntry
                        : entry.kind ==
                            DropSourceKind::FileGroupEntry;
                if (!matchingEntry)
                    continue;
                DropLanding landing;
                landing.kind = DropLandingKind::WidgetIndex;
                landing.sourceIndex = entry.sourceIndex;
                landing.widget = preview.targetWidget;
                landing.widgetId = preview.targetWidget->id;
                landing.insertIndex =
                    preview.insertIndex + preview.landings.size();
                preview.landings.push_back(landing);
            }
            return preview;
        }
        if (preview.targetWidget &&
            preview.targetWidget->type ==
                DesktopWidgetType::CollectionGroup)
        {
            auto* group =
                dynamic_cast<CollectionGroup*>(widget);
            std::wstring activeId = group
                ? group->CategoryIdAtPoint(dropPoint)
                : L"";
            if (activeId.empty() && group)
                activeId = group->GetActiveCollectionId();
            const size_t activeIndex =
                FindWidgetIndexById(activeId);
            if (activeIndex >= widgets_.size() ||
                widgets_[activeIndex].type !=
                    DesktopWidgetType::Collection)
                return preview;
            // 集合组只是当前集合的可视代理。普通图标拖放仍落到
            // 激活标签对应的 Collection 数据中。
            preview.targetWidget = const_cast<DesktopWidget*>(
                &widgets_[activeIndex]);
        }
        if (preview.targetWidget &&
            preview.targetWidget->type ==
                DesktopWidgetType::FileGroup)
        {
            auto* group = dynamic_cast<FileGroup*>(widget);
            std::wstring activeId = group
                ? group->GetActiveSourceId()
                : preview.targetWidget->activeCategoryId;
            const size_t activeIndex =
                FindWidgetIndexById(activeId);
            if (activeIndex >= widgets_.size() ||
                (widgets_[activeIndex].type !=
                    DesktopWidgetType::FileCategories &&
                 widgets_[activeIndex].type !=
                    DesktopWidgetType::FolderMapping))
                return preview;
            preview.targetWidget =
                const_cast<DesktopWidget*>(
                    &widgets_[activeIndex]);
        }
        if (preview.targetWidget &&
            preview.targetWidget->type == DesktopWidgetType::FileCategories)
        {
            auto isShortcutPath = [](const std::wstring& path) {
                return !path.empty() && _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
            };
            bool sourceHasShortcut = std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) {
                    return isShortcutPath(entry.filePath) || isShortcutPath(entry.displayName);
                });
            if (preview.action == DropAction::Link || sourceHasShortcut)
            {
                preview.targetKind = DropTargetKind::KeyedWidget;
                return preview;
            }
        }
        preview.targetKind = preview.targetWidget &&
            preview.targetWidget->type == DesktopWidgetType::FolderMapping
                ? DropTargetKind::FolderMapping
                : DropTargetKind::KeyedWidget;
        const bool sourceFromDock = std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
            [](const DragSourceEntry& entry) { return entry.fromDock; });
        if (preview.targetKind == DropTargetKind::FolderMapping && sourceFromDock &&
            preview.action == DropAction::Move)
        {
            // Dock entries are layout references. A logical move out of Dock
            // must not physically remove the backing desktop file/shortcut.
            preview.action = DropAction::Copy;
            preview.consumeDockSource = true;
        }
        Container* resolvedTarget = target;
        if (auto* group = dynamic_cast<FileGroup*>(widget))
            if (auto* active = group->GetActiveSourceContainer())
                resolvedTarget = active;
        if (auto* group =
                dynamic_cast<FileGroup*>(widget);
            group && group->GetWidgetData() &&
            group->GetWidgetData()->dateHeaders &&
            sourceList.origin == resolvedTarget &&
            preview.action == DropAction::Move)
            return preview;
        if (preview.targetWidget &&
            preview.targetWidget->dateHeaders &&
            sourceList.origin == resolvedTarget &&
            preview.action == DropAction::Move)
            return preview;
        preview.fileBacked = !(
            sourceList.origin == resolvedTarget &&
            preview.action == DropAction::Move) &&
            IsDropFileBacked(sourceList, preview.targetKind, preview.action);
        preview.insertIndex = widget->GetDropInsertIndex(targetSlot, region);
        if (auto* group =
                dynamic_cast<CollectionGroup*>(widget);
            group && !group->CategoryIdAtPoint(dropPoint).empty() &&
            preview.targetWidget)
            preview.insertIndex =
                preview.targetWidget->itemKeys.size();
        for (const auto& entry : sourceList.entries)
        {
            DropLanding landing;
            landing.kind = preview.targetKind == DropTargetKind::FolderMapping
                ? DropLandingKind::Folder
                : DropLandingKind::WidgetIndex;
            landing.sourceIndex = entry.sourceIndex;
            landing.widget = preview.targetWidget;
            if (preview.targetWidget)
                landing.widgetId = preview.targetWidget->id;
            landing.insertIndex = preview.insertIndex + preview.landings.size();
            if (preview.targetWidget)
                landing.cell = preview.targetWidget->gridCell;
            preview.landings.push_back(landing);
        }
        return preview;
    }

    return preview;
}

/**
 * @brief 构建外部文件拖入桌面的放置预览列表。
 * @param targetCell 目标网格单元格。
 * @param count 外部文件数量。
 * @return 拖拽预览列表。
 */
inline DropPreviewList DesktopApp::BuildExternalDesktopPreviewList(GridCell targetCell, size_t count) const
{
    DragSourceList list;
    list.hasExternalFiles = true;
    for (size_t i = 0; i < count; ++i)
    {
        DragSourceEntry entry;
        entry.kind = DropSourceKind::ExternalFile;
        entry.sourceIndex = i;
        entry.originalSpan = {1, 1};
        list.entries.push_back(entry);
    }

    DropPreviewList preview;
    preview.targetKind = DropTargetKind::Desktop;
    preview.action = DropAction::Copy;
    preview.fileBacked = true;
    preview.anchorCell = targetCell;
    preview.landings = BuildDesktopLandings(list, targetCell, false);
    return preview;
}

/**
 * @brief 执行拖拽管线的完整流程（文件落地或内部移动）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
inline bool DesktopApp::ExecuteDropPipeline(const DragSourceList& sourceList,
    const DropPreviewList& preview)
{
    const bool sourceFromDock = std::any_of(sourceList.entries.begin(), sourceList.entries.end(),
        [](const DragSourceEntry& entry) { return entry.fromDock; });
    if (sourceList.Empty()) return false;
    // A completely full desktop produces no visible landing preview. File
    // drops must still be materialized; ReloadItems will allocate virtual
    // overflow pages for the newly created desktop entries.
    if (preview.Empty() &&
        !(preview.fileBacked && preview.targetKind == DropTargetKind::Desktop))
        return false;
    if (preview.targetKind == DropTargetKind::Desktop &&
        preview.action == DropAction::Move &&
        IsAutoCollectFileCategorySource(sourceList))
        return false;
    bool executed = preview.fileBacked
        ? ExecuteFileBackedDropPlan(sourceList, preview)
        : ExecuteInternalDropPlan(sourceList, preview);
    if (executed && (preview.action == DropAction::Move || preview.consumeDockSource) && sourceFromDock)
    {
        std::unordered_set<std::wstring> moved;
        for (const auto& entry : sourceList.entries)
            if (entry.fromDock && !entry.dockReference.empty())
                moved.insert(std::to_wstring(static_cast<int>(entry.dockEntryType)) + L":" +
                    ToUpperInvariant(entry.dockReference));
        std::erase_if(dockEntries_, [&](const DockEntry& entry) {
            const std::wstring key = std::to_wstring(static_cast<int>(entry.type)) + L":" +
                ToUpperInvariant(entry.reference);
            return moved.contains(key);
        });
        RefreshCollectedKeysCache();
    }
    return executed;
}

/**
 * @brief 执行内部拖拽放置计划（桌面间移动或组件间重排）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
inline bool DesktopApp::ExecuteInternalDropPlan(const DragSourceList& sourceList,
    const DropPreviewList& preview)
{
    auto sourceMemberIndices = [&]() {
        std::vector<size_t> indices;
        for (const auto& entry : sourceList.entries)
            if (entry.memberIndex != static_cast<size_t>(-1))
                indices.push_back(entry.memberIndex);
        std::sort(indices.begin(), indices.end());
        indices.erase(std::unique(indices.begin(), indices.end()), indices.end());
        return indices;
    };

    if (preview.targetKind == DropTargetKind::Desktop)
    {
        if (sourceList.hasCollectionGroupEntries ||
            sourceList.hasFileGroupEntries)
        {
            bool changed = false;
            for (const auto& landing : preview.landings)
            {
                auto it = std::find_if(sourceList.entries.begin(),
                    sourceList.entries.end(),
                    [&](const DragSourceEntry& entry) {
                        return entry.sourceIndex == landing.sourceIndex;
                    });
                if (it == sourceList.entries.end() ||
                    it->widgetId.empty())
                    continue;
                changed =
                    (sourceList.hasCollectionGroupEntries
                        ? ReleaseCollectionFromGroup(
                            it->widgetId, landing.cell)
                        : ReleaseWidgetFromFileGroup(
                            it->widgetId, landing.cell)) ||
                    changed;
            }
            return changed;
        }
        RemoveDesktopKeysFromWidgets(sourceList.DesktopKeys());
        bool changed = false;
        for (const auto& landing : preview.landings)
        {
            auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
            if (it == sourceList.entries.end() || it->desktopIndex >= items_.size()) continue;
            items_[it->desktopIndex].gridCell = landing.cell;
            items_[it->desktopIndex].slot = SlotFromCell(gridPages_, landing.cell);
            changed = true;
        }
        if (changed)
        {
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
        }
        return changed;
    }

    if (preview.targetKind == DropTargetKind::KeyedWidget && preview.targetWidget)
    {
        WidgetContainer* targetWidget =
            dynamic_cast<WidgetContainer*>(
                preview.targetContainer);
        if (targetWidget &&
            targetWidget->GetWidgetData() !=
                preview.targetWidget)
            targetWidget = nullptr;
        for (auto& container : containers_)
        {
            if (targetWidget) break;
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            if (widget && widget->GetWidgetData() == preview.targetWidget)
            {
                targetWidget = widget;
                break;
            }
        }
        CollectionGroup* collectionGroupProxy = nullptr;
        if (!targetWidget)
        {
            collectionGroupProxy =
                dynamic_cast<CollectionGroup*>(
                    preview.targetContainer);
            DesktopWidget* groupData =
                collectionGroupProxy
                    ? collectionGroupProxy->GetWidgetData()
                    : nullptr;
            if (groupData &&
                std::find(
                    groupData->childWidgetIds.begin(),
                    groupData->childWidgetIds.end(),
                    preview.targetWidget->id) !=
                    groupData->childWidgetIds.end())
                targetWidget = collectionGroupProxy;
        }
        if (!targetWidget) return false;

        if ((sourceList.hasCollectionGroupEntries &&
             preview.targetWidget->type ==
                DesktopWidgetType::CollectionGroup) ||
            (sourceList.hasFileGroupEntries &&
             preview.targetWidget->type ==
                DesktopWidgetType::FileGroup))
        {
            std::vector<std::wstring> movingIds;
            for (const auto& entry : sourceList.entries)
            {
                const bool matchingEntry =
                    sourceList.hasCollectionGroupEntries
                        ? entry.kind ==
                            DropSourceKind::CollectionGroupEntry
                        : entry.kind ==
                            DropSourceKind::FileGroupEntry;
                if (matchingEntry &&
                    !entry.widgetId.empty())
                    movingIds.push_back(entry.widgetId);
            }
            if (movingIds.empty()) return false;

            DesktopWidget& targetGroup =
                *preview.targetWidget;
            const std::wstring previousTargetActive =
                targetGroup.activeCategoryId;
            size_t removedBefore = 0;
            for (const auto& id : movingIds)
            {
                auto it = std::find(
                    targetGroup.childWidgetIds.begin(),
                    targetGroup.childWidgetIds.end(), id);
                if (it != targetGroup.childWidgetIds.end() &&
                    static_cast<size_t>(std::distance(
                        targetGroup.childWidgetIds.begin(), it)) <
                        preview.insertIndex)
                    ++removedBefore;
            }

            for (auto& widget : widgets_)
            {
                if (widget.type !=
                    preview.targetWidget->type)
                    continue;
                for (const auto& id : movingIds)
                {
                    std::erase(widget.childWidgetIds, id);
                    if (widget.activeCategoryId == id)
                        widget.activeCategoryId =
                            widget.childWidgetIds.empty()
                                ? L""
                                : widget.childWidgetIds.front();
                }
            }
            size_t insertAt =
                preview.insertIndex > removedBefore
                    ? preview.insertIndex - removedBefore
                    : 0;
            insertAt = std::min(
                insertAt, targetGroup.childWidgetIds.size());
            targetGroup.childWidgetIds.insert(
                targetGroup.childWidgetIds.begin() +
                    static_cast<std::ptrdiff_t>(insertAt),
                movingIds.begin(), movingIds.end());
            targetGroup.activeCategoryId =
                snowdesktop::collection_group_rules::
                    ResolveActiveItem(
                        targetGroup.childWidgetIds,
                        previousTargetActive);
            for (auto& container : containers_)
            {
                if (auto* group =
                        dynamic_cast<CollectionGroup*>(
                            container.get()))
                    group->InvalidateFilterCache();
                else if (auto* fileGroup =
                             dynamic_cast<FileGroup*>(
                                 container.get()))
                    fileGroup->InvalidateHostedView();
            }
            EnsureNavTabOrder();
            LayoutItems();
            SaveLayoutSlots();
            InvalidateRect(hwnd_, nullptr, TRUE);
            return true;
        }

        const bool reorderInsideCollectionGroup =
            collectionGroupProxy &&
            targetWidget->GetWidgetData() !=
                preview.targetWidget &&
            collectionGroupProxy->GetActiveCollectionId() ==
                preview.targetWidget->id;
        if (sourceList.origin == targetWidget &&
            preview.action == DropAction::Move &&
            (!collectionGroupProxy ||
                reorderInsideCollectionGroup))
        {
            if (collectionGroupProxy &&
                targetWidget->GetWidgetData() !=
                    preview.targetWidget)
            {
                std::unordered_set<std::wstring> movingKeys;
                for (const auto& key :
                    sourceList.DesktopKeys())
                    movingKeys.insert(
                        ToUpperInvariant(key));
                if (movingKeys.empty()) return false;

                auto& targetKeys =
                    preview.targetWidget->itemKeys;
                std::vector<std::wstring> moving;
                size_t removedBefore = 0;
                const bool moveBetweenCollections =
                    std::any_of(
                        sourceList.entries.begin(),
                        sourceList.entries.end(),
                        [&](const DragSourceEntry& entry) {
                            return entry.kind ==
                                    DropSourceKind::DesktopIcon &&
                                !entry.widgetId.empty() &&
                                entry.widgetId !=
                                    preview.targetWidget->id;
                        });

                if (moveBetweenCollections)
                {
                    for (const auto& entry :
                        sourceList.entries)
                    {
                        if (entry.kind !=
                                DropSourceKind::DesktopIcon ||
                            entry.widgetId.empty() ||
                            entry.desktopKey.empty())
                            continue;
                        const size_t sourceIndex =
                            FindWidgetIndexById(entry.widgetId);
                        if (sourceIndex >= widgets_.size() ||
                            widgets_[sourceIndex].type !=
                                DesktopWidgetType::Collection)
                            continue;
                        auto& sourceKeys =
                            widgets_[sourceIndex].itemKeys;
                        auto sourceIt = std::find_if(
                            sourceKeys.begin(), sourceKeys.end(),
                            [&](const std::wstring& key) {
                                return ToUpperInvariant(key) ==
                                    ToUpperInvariant(
                                        entry.desktopKey);
                            });
                        if (sourceIt == sourceKeys.end())
                            continue;
                        moving.push_back(*sourceIt);
                        sourceKeys.erase(sourceIt);
                    }
                }
                else
                {
                    for (size_t i = 0;
                        i < targetKeys.size(); ++i)
                    {
                        if (!movingKeys.contains(
                                ToUpperInvariant(
                                    targetKeys[i])))
                            continue;
                        if (i < preview.insertIndex)
                            ++removedBefore;
                        moving.push_back(targetKeys[i]);
                    }
                }
                if (moving.empty()) return false;

                std::erase_if(
                    targetKeys,
                    [&](const std::wstring& key) {
                        return movingKeys.contains(
                            ToUpperInvariant(key));
                    });
                size_t insertAt =
                    preview.insertIndex > removedBefore
                        ? preview.insertIndex - removedBefore
                        : 0;
                insertAt = std::min(
                    insertAt, targetKeys.size());
                targetKeys.insert(
                    targetKeys.begin() +
                        static_cast<std::ptrdiff_t>(insertAt),
                    moving.begin(), moving.end());
                collectionGroupProxy->InvalidateFilterCache();
                RefreshCollectedKeysCache();
                SaveLayoutSlots();
                InvalidateRect(hwnd_, nullptr, TRUE);
                return true;
            }

            std::vector<size_t> indices = sourceMemberIndices();
            if (indices.empty())
                indices = targetWidget->GetSelectedMemberIndices();
            targetWidget->ReorderMembers(indices, preview.insertIndex);
            if (targetWidget ==
                dockFolderPopupContainer_.get())
                CommitDockFolderPopupStateToSource();
            targetWidget->InvalidateSlots();
            return true;
        }

        WidgetContainer* originWidget = dynamic_cast<WidgetContainer*>(sourceList.origin);
        DesktopWidget* originData = originWidget ? originWidget->GetWidgetData() : nullptr;
        size_t inserted = 0;
        for (const auto& landing : preview.landings)
        {
            auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
                [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
            if (it == sourceList.entries.end() || it->desktopKey.empty()) continue;
            std::wstring key = ToUpperInvariant(it->desktopKey);
            if (!targetWidget->AllowsDesktopKey(key)) continue;

            if (preview.action == DropAction::Move)
            {
                if (originData)
                    RemoveDesktopKeysFromWidgets({key});
                else
                    RemoveDesktopKeysFromWidgets({key});
            }

            auto exists = std::find_if(preview.targetWidget->itemKeys.begin(),
                preview.targetWidget->itemKeys.end(),
                [&](const std::wstring& existing) { return ToUpperInvariant(existing) == key; });
            if (exists == preview.targetWidget->itemKeys.end())
            {
                size_t insertAt = std::min(preview.insertIndex + inserted, preview.targetWidget->itemKeys.size());
                preview.targetWidget->itemKeys.insert(
                    preview.targetWidget->itemKeys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
                ++inserted;
            }
            size_t itemIndex = FindItemIndexByKey(key);
            if (itemIndex != static_cast<size_t>(-1))
                items_[itemIndex].gridCell = preview.targetWidget->gridCell;
        }
        if (originWidget) originWidget->InvalidateSlots();
        targetWidget->InvalidateSlots();
        if (GetDesktopGrid()) GetDesktopGrid()->InvalidateSlots();
        RefreshCollectedKeysCache();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
        return true;
    }

    if (preview.targetKind == DropTargetKind::FolderMapping &&
        sourceList.origin == preview.targetContainer && preview.action == DropAction::Move)
    {
        auto* targetWidget = dynamic_cast<WidgetContainer*>(preview.targetContainer);
        if (!targetWidget) return false;
        std::vector<size_t> indices = sourceMemberIndices();
        if (indices.empty())
            indices = targetWidget->GetSelectedMemberIndices();
        targetWidget->ReorderMembers(indices, preview.insertIndex);
        if (targetWidget ==
            dockFolderPopupContainer_.get())
            CommitDockFolderPopupStateToSource();
        return true;
    }

    return false;
}

/**
 * @brief 将文件实际复制/移动/创建快捷方式到桌面目录。
 * @param sourceList 拖拽源列表。
 * @param action 拖拽动作（复制/移动/链接）。
 * @param duplicateDesktopCopyNames 是否对已在桌面的文件生成副本名称。
 * @param createdPathsBySource 输出参数，记录每个源索引对应的创建路径。
 * @return 操作成功返回 true。
 */
inline bool DesktopApp::MaterializeFilesToDesktop(const DragSourceList& sourceList,
    DropAction action, bool duplicateDesktopCopyNames,
    std::unordered_map<size_t, std::wstring>* createdPathsBySource)
{
    std::vector<std::wstring> paths = sourceList.FilePaths();
    if (paths.empty()) return false;
    if (createdPathsBySource)
        createdPathsBySource->clear();

    wchar_t desktopPathRaw[MAX_PATH]{};
    if (!SHGetSpecialFolderPathW(nullptr, desktopPathRaw, CSIDL_DESKTOPDIRECTORY, FALSE))
        return false;
    std::wstring desktopPath = TrimTrailingPathSeparators(desktopPathRaw);

    auto doubleNull = [](const std::wstring& value) {
        std::wstring result = value;
        result.push_back(L'\0');
        result.push_back(L'\0');
        return result;
    };

    auto sameParentAsDesktop = [&](const std::wstring& path) -> bool {
        wchar_t parent[MAX_PATH]{};
        wcscpy_s(parent, path.c_str());
        if (!PathRemoveFileSpecW(parent)) return false;
        return PathsEqualInsensitive(parent, desktopPath);
    };

    auto makeUniqueCopyPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        DWORD attrs = GetFileAttributesW(path.c_str());
        bool isDir = attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);

        std::wstring stem = fileName ? fileName : L"";
        std::wstring ext;
        if (!isDir)
        {
            wchar_t stemBuf[MAX_PATH]{};
            wcscpy_s(stemBuf, stem.c_str());
            PathRemoveExtensionW(stemBuf);
            stem = stemBuf;
            const wchar_t* extPtr = PathFindExtensionW(fileName);
            ext = extPtr ? extPtr : L"";
        }

        for (int i = 1; i < 1000; ++i)
        {
            std::wstring name = i <= 1
                    ? stem + _LW("app.grid.copy_suffix") + ext
                : stem + _LFW("app.grid.copy_suffix_num", std::to_wstring(i)) + ext;
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES)
                return std::wstring(dst);
        }
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath.c_str(), (stem + _LW("app.grid.copy_suffix_1000") + ext).c_str());
        return std::wstring(fallback);
    };

    auto makeUniqueShortcutPath = [&](const std::wstring& path) {
        const wchar_t* fileName = PathFindFileNameW(path.c_str());
        wchar_t stemBuf[MAX_PATH]{};
        wcscpy_s(stemBuf, fileName ? fileName : L"");
        PathRemoveExtensionW(stemBuf);
        std::wstring stem = stemBuf[0] != L'\0' ? stemBuf : _LW("widget.shortcut");

        for (int i = 1; i < 1000; ++i)
        {
            std::wstring name = i <= 1
                ? stem + L".lnk"
                : stem + L" (" + std::to_wstring(i) + L").lnk";
            wchar_t dst[MAX_PATH]{};
            PathCombineW(dst, desktopPath.c_str(), name.c_str());
            if (GetFileAttributesW(dst) == INVALID_FILE_ATTRIBUTES)
                return std::wstring(dst);
        }
        wchar_t fallback[MAX_PATH]{};
        PathCombineW(fallback, desktopPath.c_str(), (stem + L" (1000).lnk").c_str());
        return std::wstring(fallback);
    };

    auto shellOperateOne = [&](UINT func, const std::wstring& fromPath, const std::wstring& toPath) {
        std::wstring from = doubleNull(fromPath);
        std::wstring to = doubleNull(toPath);
        SHFILEOPSTRUCTW op{};
        op.hwnd = ShellDialogOwnerHwnd();
        op.wFunc = func;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    };

    auto shellOperateManyToDesktop = [&](UINT func, const std::vector<std::wstring>& sourcePaths) {
        std::wstring from;
        for (const auto& path : sourcePaths)
        {
            from += path;
            from.push_back(L'\0');
        }
        from.push_back(L'\0');

        std::wstring to = desktopPath;
        to.push_back(L'\0');
        SHFILEOPSTRUCTW op{};
        op.hwnd = ShellDialogOwnerHwnd();
        op.wFunc = func;
        op.pFrom = from.c_str();
        op.pTo = to.c_str();
        op.fFlags = FOF_NOCONFIRMATION | FOF_NOCONFIRMMKDIR | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
        return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
    };

    bool operated = false;
    if (action == DropAction::Link)
    {
        for (const auto& source : sourceList.entries)
        {
            const auto& path = source.filePath;
            if (path.empty()) continue;

            std::wstring dst = makeUniqueShortcutPath(path);
            ComPtr<IShellLinkW> shellLink;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
            {
                shellLink->SetPath(path.c_str());
                shellLink->SetWorkingDirectory(desktopPath.c_str());
                ComPtr<IPersistFile> persistFile;
                if (SUCCEEDED(shellLink.As(&persistFile)) &&
                    SUCCEEDED(persistFile->Save(dst.c_str(), TRUE)))
                {
                    if (createdPathsBySource)
                        (*createdPathsBySource)[source.sourceIndex] = dst;
                    operated = true;
                }
            }
        }
    }
    else if (action == DropAction::Copy)
    {
        std::vector<std::wstring> normalCopies;
        for (const auto& path : paths)
        {
            if (duplicateDesktopCopyNames && sameParentAsDesktop(path))
                operated = shellOperateOne(FO_COPY, path, makeUniqueCopyPath(path)) || operated;
            else
                normalCopies.push_back(path);
        }
        if (!normalCopies.empty())
            operated = shellOperateManyToDesktop(FO_COPY, normalCopies) || operated;
    }
    else
    {
        operated = shellOperateManyToDesktop(FO_MOVE, paths);
    }
    return operated;
}

/**
 * @brief 将文件实际复制/移动/创建快捷方式到指定文件夹。
 * @param sourceList 拖拽源列表。
 * @param folderPath 目标文件夹路径。
 * @param action 拖拽动作。
 * @return 操作成功返回 true。
 */
inline bool DesktopApp::MaterializeFilesToFolder(const DragSourceList& sourceList,
    const std::wstring& folderPath, DropAction action) const
{
    std::vector<std::wstring> paths = sourceList.FilePaths();
    if (paths.empty() || folderPath.empty()) return false;

    std::wstring folder = folderPath;
    if (!folder.empty() && folder.back() != L'\\') folder += L'\\';

    if (action == DropAction::Link)
    {
        bool createdAny = false;
        for (const auto& path : paths)
        {
            std::wstring name = PathFindFileNameW(path.c_str());
            std::wstring stem = name;
            if (stem.size() > 4 && _wcsicmp(stem.c_str() + stem.size() - 4, L".lnk") == 0)
                stem = stem.substr(0, stem.size() - 4);

            std::wstring linkPath;
            for (int i = 1; i < 1000; ++i)
            {
                linkPath = folder + stem + (i == 1 ? L".lnk" : L" (" + std::to_wstring(i) + L").lnk");
                if (GetFileAttributesW(linkPath.c_str()) == INVALID_FILE_ATTRIBUTES)
                    break;
            }

            ComPtr<IShellLinkW> shellLink;
            if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                IID_IShellLinkW, reinterpret_cast<void**>(shellLink.GetAddressOf()))))
                continue;

            shellLink->SetPath(path.c_str());
            shellLink->SetWorkingDirectory(folder.c_str());
            ComPtr<IPersistFile> persistFile;
            if (SUCCEEDED(shellLink.As(&persistFile)) &&
                SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE)))
                createdAny = true;
        }
        return createdAny;
    }

    std::wstring from;
    for (const auto& path : paths)
    {
        from += path;
        from += L'\0';
    }
    from += L'\0';

    std::wstring to = folder;
    to += L'\0';
    SHFILEOPSTRUCTW op{};
    op.hwnd = ShellDialogOwnerHwnd();
    op.wFunc = action == DropAction::Move ? FO_MOVE : FO_COPY;
    op.pFrom = from.c_str();
    op.pTo = to.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_RENAMEONCOLLISION;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

/**
 * @brief 缓存待处理的放置结果，供后续 ApplyPendingPlacement 使用。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @param existingKeys 放置前的桌面键快照。
 * @param createdPathsBySource 可选参数，记录每个源索引对应的创建路径。
 */
inline void DesktopApp::StorePendingLandingCache(const DragSourceList& sourceList,
    const DropPreviewList& preview, const std::unordered_set<std::wstring>& existingKeys,
    const std::unordered_map<size_t, std::wstring>* createdPathsBySource)
{
    pendingLandingCache_.Clear();
    pendingLandingCache_.existingDesktopKeys = existingKeys;
    pendingLandingCache_.tick = GetTickCount();

    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell &&
            landing.kind != DropLandingKind::WidgetIndex)
            continue;
        auto it = std::find_if(sourceList.entries.begin(), sourceList.entries.end(),
            [&](const DragSourceEntry& entry) { return entry.sourceIndex == landing.sourceIndex; });
        if (it == sourceList.entries.end()) continue;

        PendingLandingEntry entry;
        entry.sourceIndex = it->sourceIndex;
        entry.action = preview.action;
        entry.kind = landing.kind;
        entry.sourcePath = it->filePath;
        entry.sourceName = !it->filePath.empty() ? FileNameFromPath(it->filePath) : it->displayName;
        if (createdPathsBySource)
        {
            auto created = createdPathsBySource->find(it->sourceIndex);
            if (created != createdPathsBySource->end())
                entry.createdPath = created->second;
        }
        entry.cell = landing.kind == DropLandingKind::DesktopCell ? landing.cell : landing.cell;
        entry.insertIndex = landing.insertIndex;
        entry.widget = landing.widget;
        entry.widgetId = landing.widgetId;
        pendingLandingCache_.entries.push_back(entry);
    }
    pendingLandingCache_.active = !pendingLandingCache_.entries.empty();
}

/**
 * @brief 执行基于文件系统的拖拽放置计划（复制/移动到桌面或文件夹映射）。
 * @param sourceList 拖拽源列表。
 * @param preview 拖拽预览列表。
 * @return 执行成功返回 true。
 */
inline bool DesktopApp::ExecuteFileBackedDropPlan(const DragSourceList& sourceList,
    const DropPreviewList& preview)
{
    auto refreshFolderMappingById = [&](const std::wstring& widgetId) {
        if (widgetId.empty()) return;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (widgets_[i].id == widgetId)
            {
                RefreshFolderMappingWidget(i);
                break;
            }
        }
    };
    auto refreshSourceFolderMappings = [&]() {
        std::unordered_set<std::wstring> sourceIds;
        if (sourceList.hasOriginWidget &&
            sourceList.originWidgetType ==
                DesktopWidgetType::FolderMapping)
            sourceIds.insert(
                sourceList.originWidgetId);
        for (const auto& entry : sourceList.entries)
            if (entry.kind ==
                    DropSourceKind::FolderEntry &&
                !entry.widgetId.empty())
                sourceIds.insert(entry.widgetId);
        for (const auto& sourceId : sourceIds)
            refreshFolderMappingById(sourceId);
    };
    auto removeDesktopItemsByKeys = [&](const std::vector<std::wstring>& keys) {
        if (keys.empty()) return false;

        std::unordered_set<std::wstring> normalizedKeys;
        normalizedKeys.reserve(keys.size());
        for (const auto& key : keys)
            if (!key.empty())
                normalizedKeys.insert(ToUpperInvariant(key));
        if (normalizedKeys.empty()) return false;

        size_t oldSize = items_.size();
        items_.erase(
            std::remove_if(items_.begin(), items_.end(),
                [&](const DesktopItem& item) {
                    return !item.layoutKey.empty() &&
                        normalizedKeys.contains(ToUpperInvariant(item.layoutKey));
                }),
            items_.end());
        if (items_.size() == oldSize) return false;

        d2dIconCache_.clear();
        itemTextLayoutCache_.clear();
        itemTextShadowCache_.clear();
        RefreshDesktopItemIndexCache();
        if (GetDesktopGrid())
            GetDesktopGrid()->InvalidateSlots();
        return true;
    };

    if (preview.targetKind == DropTargetKind::FolderMapping && preview.targetWidget)
    {
        size_t targetWidgetIndex = static_cast<size_t>(-1);
        std::unordered_set<std::wstring> targetExistingPaths;
        for (size_t i = 0; i < widgets_.size(); ++i)
        {
            if (&widgets_[i] != preview.targetWidget) continue;
            targetWidgetIndex = i;
            for (const auto& entry : widgets_[i].folderEntries)
                targetExistingPaths.insert(ToUpperInvariant(entry.fullPath));
            break;
        }

        bool operated = MaterializeFilesToFolder(sourceList, preview.targetWidget->sourceFolderPath,
            preview.action);
        if (!operated) return false;
        if (preview.action == DropAction::Move)
        {
            RemoveDesktopKeysFromWidgets(sourceList.DesktopKeys());
            removeDesktopItemsByKeys(sourceList.DesktopKeys());
        }

        refreshSourceFolderMappings();
        if (targetWidgetIndex != static_cast<size_t>(-1))
        {
            RefreshFolderMappingWidget(targetWidgetIndex);
            auto& target = widgets_[targetWidgetIndex];
            std::vector<FolderEntry> inserted;
            for (auto it = target.folderEntries.begin(); it != target.folderEntries.end(); )
            {
                if (targetExistingPaths.contains(ToUpperInvariant(it->fullPath)))
                {
                    ++it;
                    continue;
                }
                inserted.push_back(std::move(*it));
                it = target.folderEntries.erase(it);
            }
            if (!inserted.empty())
            {
                size_t insertAt = std::min(preview.insertIndex, target.folderEntries.size());
                target.folderEntries.insert(target.folderEntries.begin() + static_cast<std::ptrdiff_t>(insertAt),
                    std::make_move_iterator(inserted.begin()), std::make_move_iterator(inserted.end()));
                target.itemKeys.clear();
                target.itemKeys.reserve(target.folderEntries.size());
                for (const auto& entry : target.folderEntries)
                    target.itemKeys.push_back(entry.fullPath);
            }
            for (auto& c : containers_)
            {
                auto* wc = dynamic_cast<WidgetContainer*>(c.get());
                if (wc && wc->GetWidgetData() == &target) { wc->InvalidateSlots(); break; }
            }
        }
        return true;
    }

    bool duplicateCopyNames = preview.action == DropAction::Copy && sourceList.hasDesktopIcons &&
        !sourceList.hasExternalFiles;
    std::unordered_set<std::wstring> existingKeys = SnapshotDesktopKeys();
    std::unordered_map<size_t, std::wstring> createdPathsBySource;
    std::unordered_map<size_t, std::wstring>* createdPaths =
        preview.action == DropAction::Link ? &createdPathsBySource : nullptr;
    bool operated = MaterializeFilesToDesktop(sourceList, preview.action, duplicateCopyNames,
        createdPaths);
    if (!operated)
    {
        pendingLandingCache_.Clear();
        return false;
    }
    StorePendingLandingCache(sourceList, preview, existingKeys, createdPaths);

    ReloadItems(false);
    if (preview.action == DropAction::Move && sourceList.hasDesktopIcons)
        RemoveDesktopKeysFromWidgets(sourceList.DesktopKeys());
    refreshSourceFolderMappings();
    return true;
}

/**
 * @brief 在 D2D 设备上下文上绘制拖拽放置预览（高亮目标区域）。
 * @param ctx D2D 设备上下文。
 * @param preview 拖拽预览列表。
 */
inline void DesktopApp::DrawDesktopDropPreviewList(ID2D1DeviceContext* ctx,
    const DropPreviewList& preview)
{
    if (!ctx) return;
    for (const auto& landing : preview.landings)
    {
        if (landing.kind != DropLandingKind::DesktopCell) continue;
        GridSpan span{
            std::max(1, landing.span.columns),
            std::max(1, landing.span.rows)
        };
        RECT targetRect = GetGridRect(gridPages_, landing.cell, span);
        DrawD2DRoundedRectangle(ctx, targetRect, 6.0f,
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.12f),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.50f), 2.0f);
    }
}

/**
 * @brief 获取或重建缓存的桌面放置预览。
 *
 * 拖拽渲染每帧调用 DrawDropPreview → BuildDropPreviewList → BuildDesktopLandings，
 * 后者遍历全部 items/widgets 搜索空位。当鼠标位置/动作/目标不变时复用缓存，
 * 避免每帧重建导致卡顿（尤其阶段2-4全页/跨页/新建页搜索）。
 */
inline const DropPreviewList& DesktopApp::GetCachedDesktopDropPreview(
    bool hasItemDrag, const DragSourceList& sourceList,
    Container* target, Slot* slot, HitRegion region, int mods, POINT dragPoint)
{
    const size_t sourceCount = sourceList.entries.size();
    // 判断缓存是否有效：位置、动作、目标、源数量均未变
    const bool cacheValid = !cachedDropPreview_.landings.empty() &&
        cachedDropPreviewHasItems_ == hasItemDrag &&
        cachedDropPreviewPoint_.x == dragPoint.x &&
        cachedDropPreviewPoint_.y == dragPoint.y &&
        cachedDropPreviewMods_ == mods &&
        cachedDropPreviewTarget_ == target &&
        cachedDropPreviewSlot_ == slot &&
        cachedDropPreviewRegion_ == region &&
        cachedDropPreviewSourceCount_ == sourceCount;

    if (!cacheValid)
    {
        if (hasItemDrag)
        {
            cachedDropPreview_ = BuildDropPreviewList(sourceList, target, slot, region, mods, dragPoint);
        }
        else
        {
            GridCell targetCell = CellFromPoint(dragPoint);
            if (targetCell.pageId.empty())
                cachedDropPreview_ = {};
            else
                cachedDropPreview_ = BuildExternalDesktopPreviewList(targetCell,
                    static_cast<size_t>(std::max(1, externalDropFileCount_)));
        }
        cachedDropPreviewPoint_ = dragPoint;
        cachedDropPreviewMods_ = mods;
        cachedDropPreviewTarget_ = target;
        cachedDropPreviewSlot_ = slot;
        cachedDropPreviewRegion_ = region;
        cachedDropPreviewHasItems_ = hasItemDrag;
        cachedDropPreviewSourceCount_ = sourceCount;
    }
    return cachedDropPreview_;
}

/**
 * @brief 应用缓存的放置结果，将新创建的文件分配到正确的网格位置或组件中。
 */
inline void DesktopApp::ApplyPendingPlacement()
{
    if (!pendingLandingCache_.active) return;
    if (GetTickCount() - pendingLandingCache_.tick > 10000)
    {
        pendingLandingCache_.Clear();
        return;
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (!key.empty() && !pendingLandingCache_.existingDesktopKeys.contains(key))
            continue;
        if (!item.name.empty() && !IsItemInAnyWidget(item))
            MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    auto findWidgetContainer = [&](const std::wstring& widgetId) -> WidgetContainer* {
        for (auto& container : containers_)
        {
            auto* widget = dynamic_cast<WidgetContainer*>(container.get());
            DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
            if (data && data->id == widgetId)
                return widget;
        }
        const size_t groupedIndex =
            FindCollectionGroupIndexForChild(widgetId);
        if (groupedIndex < widgets_.size())
            for (auto& container : containers_)
            {
                auto* widget =
                    dynamic_cast<WidgetContainer*>(
                        container.get());
                if (widget &&
                    widget->GetWidgetData() ==
                        &widgets_[groupedIndex])
                    return widget;
            }
        return nullptr;
    };

    std::vector<bool> entryUsed(pendingLandingCache_.entries.size(), false);
    bool changed = false;
    for (size_t itemIndex = 0; itemIndex < items_.size(); ++itemIndex)
    {
        auto& item = items_[itemIndex];
        std::wstring key = ToUpperInvariant(item.layoutKey);
        if (key.empty() || pendingLandingCache_.existingDesktopKeys.contains(key))
            continue;

        for (size_t e = 0; e < pendingLandingCache_.entries.size(); ++e)
        {
            if (entryUsed[e]) continue;
            const auto& landing = pendingLandingCache_.entries[e];
            bool matchesLanding = false;
            if (!landing.createdPath.empty())
            {
                matchesLanding =
                    PathsEqualInsensitive(item.parsingName, landing.createdPath) ||
                    PathsEqualInsensitive(FileNameFromPath(item.parsingName),
                        FileNameFromPath(landing.createdPath)) ||
                    PathsEqualInsensitive(item.name, FileNameFromPath(landing.createdPath));
            }
            if (!matchesLanding)
            {
                matchesLanding =
                    MatchPendingName(item.name, landing.sourceName) ||
                    (!item.parsingName.empty() &&
                     MatchPendingName(FileNameFromPath(item.parsingName), landing.sourceName));
            }
            if (!matchesLanding) continue;

            if (landing.kind == DropLandingKind::WidgetIndex && !landing.widgetId.empty())
            {
                WidgetContainer* widget = findWidgetContainer(landing.widgetId);
                const size_t widgetIndex =
                    FindWidgetIndexById(landing.widgetId);
                DesktopWidget* widgetData =
                    widgetIndex < widgets_.size()
                        ? &widgets_[widgetIndex]
                        : nullptr;
                if (!widgetData) break;

                item.gridCell = widgetData->gridCell;
                bool allowKey = !widget || landing.action == DropAction::Link || widget->AllowsDesktopKey(key);
                if (allowKey)
                {
                    auto exists = std::find_if(widgetData->itemKeys.begin(), widgetData->itemKeys.end(),
                        [&](const std::wstring& existing) { return ToUpperInvariant(existing) == key; });
                    if (exists == widgetData->itemKeys.end())
                    {
                        size_t insertAt = std::min(landing.insertIndex, widgetData->itemKeys.size());
                        widgetData->itemKeys.insert(
                            widgetData->itemKeys.begin() + static_cast<std::ptrdiff_t>(insertAt), key);
                    }
                    if (widget) widget->InvalidateSlots();
                }
            }
            else if (landing.kind == DropLandingKind::DesktopCell)
            {
                GridSpan span = item.gridSpan;
                span.columns = std::max(1, span.columns);
                span.rows = std::max(1, span.rows);

                GridCell cell = landing.cell;

                // 预分配的新溢出页：若 pageId 不在 savedPageIds_ 里，先创建
                if (!cell.pageId.empty() &&
                    std::find(savedPageIds_.begin(), savedPageIds_.end(), cell.pageId) == savedPageIds_.end())
                {
                    RememberSavedPageId(cell.pageId);
                    // 参考末屏显示器的网格维度
                    auto monitorOrder = BuildMonitorRenderOrder();
                    const GridPage* refPage = !monitorOrder.empty()
                        ? &gridPages_[monitorOrder.back()] : GetFirstPageGridPage();
                    if (!refPage) break;
                    savedPageColumns_[cell.pageId] = std::max(1, refPage->columns);
                    savedPageRows_[cell.pageId] = std::max(1, refPage->rows);
                }

                bool found = false;
                if (IsGridAreaValid(cell, span) && !AreGridSlotsMarked(usedSlots, cell, span))
                {
                    found = true;
                }
                else
                {
                    found = TryFindFreeCell(span, usedSlots, cell, landing.cell.pageId,
                        SlotFromCell(gridPages_, landing.cell));
                }
                if (!found) break;
                item.gridCell = cell;
                item.slot = SlotFromCell(gridPages_, cell);
                item.selected = true;
                MarkGridArea(usedSlots, cell, span);
            }

            entryUsed[e] = true;
            changed = true;
            break;
        }
    }

    std::vector<PendingLandingEntry> remaining;
    for (size_t i = 0; i < pendingLandingCache_.entries.size(); ++i)
        if (!entryUsed[i])
            remaining.push_back(pendingLandingCache_.entries[i]);

    pendingLandingCache_.entries = std::move(remaining);
    pendingLandingCache_.active = !pendingLandingCache_.entries.empty();
    if (!pendingLandingCache_.active)
        pendingLandingCache_.existingDesktopKeys.clear();

    if (changed)
    {
        LayoutItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
}

// ── 网格全局函数 ──────────────────────────────────────────

/**
 * @brief 根据页面 ID 在页面列表中查找对应的网格页面。
 * @param pages 页面列表。
 * @param pageId 页面 ID。
 * @return 找到的页面指针，未找到返回 nullptr。
 */
inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId)
{
    for (auto& p : pages) if (p.id == pageId) return &p;
    return nullptr;
}

/**
 * @brief 获取网格轴上指定索引的偏移像素值。
 * @param page 网格页面。
 * @param index 索引。
 * @param horizontal true 为水平轴，false 为垂直轴。
 * @return 偏移像素值。
 */
inline int GetGridAxisOffset(const GridPage& page, int index, bool horizontal)
{
    const int count = horizontal ? page.columns : page.rows;
    const int cellSize = horizontal ? page.cellWidth : page.cellHeight;
    if (index <= 0 || count <= 1) return std::max(0, index) * cellSize;

    const int extent = horizontal
        ? static_cast<int>(page.workArea.right - page.workArea.left)
        : static_cast<int>(page.workArea.bottom - page.workArea.top);
    const int margin = horizontal ? page.marginX : page.marginY;
    const int gapSpace = std::max(0, extent - margin * 2 - count * cellSize);
    const int gapCount = count - 1;

    // Use a cumulative ratio so integer remainders are spread across all
    // internal gaps instead of being absorbed by the two outer margins.
    const int distributedGap = (index * gapSpace + gapCount / 2) / gapCount;
    return index * cellSize + distributedGap;
}

/**
 * @brief 根据网格页面和单元格计算对应的矩形区域。
 * @param pages 页面列表。
 * @param cell 起始单元格。
 * @param span 跨度（默认 {1,1}）。
 * @return 计算出的 RECT。
 */
inline RECT GetGridRect(const std::vector<GridPage>& pages, const GridCell& cell, GridSpan span = {})
{
    auto* page = FindGridPage(pages, cell.pageId);
    if (!page) return MakeRect(0, 0, 0, 0);
    int col = std::clamp(cell.column, 0, std::max(0, page->columns - 1));
    int row = std::clamp(cell.row,    0, std::max(0, page->rows    - 1));
    int sc  = std::clamp(span.columns, 1, std::max(1, page->columns - col));
    int sr  = std::clamp(span.rows,    1, std::max(1, page->rows    - row));
    int x = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col, true);
    int y = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row, false);
    int r = page->workArea.left + page->marginX + GetGridAxisOffset(*page, col + sc - 1, true)  + page->cellWidth;
    int b = page->workArea.top  + page->marginY + GetGridAxisOffset(*page, row + sr - 1, false) + page->cellHeight;
    return MakeRect(x, y, r, b);
}

/**
 * @brief 根据网格页面和单元格计算槽位索引。
 * @param pages 页面列表。
 * @param cell 单元格。
 * @return 槽位索引。
 */
inline int SlotFromCell(const std::vector<GridPage>& pages, const GridCell& cell)
{
    auto* page = FindGridPage(pages, cell.pageId);
    int rows = page ? page->rows : 1;
    return std::max(0, cell.column) * std::max(1, rows) + std::max(0, cell.row);
}

/**
 * @brief 对所有桌面项和组件执行网格布局计算，更新每个项的边界矩形和槽位。
 */
inline void DesktopApp::LayoutItems()
{
    for (auto& item : items_)
    {
        if (item.name.empty()) { item.bounds = {}; continue; }
        if (!gridPages_.empty() && item.gridCell.pageId.empty())
        {
            const GridPage* firstPage = GetFirstPageGridPage();
            if (firstPage) item.gridCell.pageId = firstPage->id;
        }
        auto* page = FindGridPage(gridPages_, item.gridCell.pageId);
        if (page)
        {
            item.gridSpan.columns = std::clamp(item.gridSpan.columns, 1, std::max(1, page->columns));
            item.gridSpan.rows    = std::clamp(item.gridSpan.rows,    1, std::max(1, page->rows));
            item.gridCell.column  = std::clamp(item.gridCell.column,  0, std::max(0, page->columns - item.gridSpan.columns));
            item.gridCell.row     = std::clamp(item.gridCell.row,     0, std::max(0, page->rows    - item.gridSpan.rows));
        }
        item.slot   = SlotFromCell(gridPages_, item.gridCell);
        item.bounds = GetGridRect(gridPages_, item.gridCell, item.gridSpan);
    }

    // Layout widgets
    for (auto& widget : widgets_)
    {
        if (IsGroupedWidget(widget))
        {
            widget.bounds = {};
            widget.cellScale = 1.0f;
            continue;
        }
        if (!gridPages_.empty() && widget.gridCell.pageId.empty())
        {
            const GridPage* firstPage = GetFirstPageGridPage();
            if (firstPage) widget.gridCell.pageId = firstPage->id;
        }
        const GridPage* page = FindGridPage(gridPages_, widget.gridCell.pageId);
        if (page)
        {
            widget.gridSpan = ClampWidgetGridSpan(widget, widget.gridSpan,
                page->columns, page->rows);
            widget.gridCell.column  = std::clamp(widget.gridCell.column,  0, std::max(0, page->columns - widget.gridSpan.columns));
            widget.gridCell.row     = std::clamp(widget.gridCell.row,     0, std::max(0, page->rows    - widget.gridSpan.rows));
            widget.cellScale = CalculateWidgetCellScale(page->cellWidth, page->cellHeight);
        }
        else
            widget.cellScale = 1.0f;
        widget.bounds = GetGridRect(gridPages_, widget.gridCell, widget.gridSpan);
    }

    RebuildContainersAndItems();
}

/**
 * @brief 重建容器和项目对象层次结构（DesktopGrid、DesktopIcon、WidgetContainer 等）。
 *
 * 根据 items_ 和 widgets_ 数据重建运行时的 Container 和 Item 对象，
 * 并重新绑定拖拽源。
 */
inline void DesktopApp::RebuildContainersAndItems()
{
    const bool wasDragging = dragSession_.IsActive();
    if (wasDragging)
        dragSession_.DetachRuntimeBindings();

    // All of these point into the runtime object tree that is about to be
    // destroyed. Clear them before releasing containers and item wrappers.
    mouseDownHit_ = nullptr;
    pendingCtrlToggleWidgetItem_ = nullptr;
    dockPressedContainer_ = nullptr;
    widgetDockTargetContainer_ = nullptr;
    popupDragTargetSlot_.reset();
    popupMouseDownItem_.reset();

    containers_.clear();
    items_oo_.clear();

    // Collect keys of items that belong to widgets.
    RefreshCollectedKeysCache();
    const auto& collectedKeys = collectedKeysCache_;

    // DesktopGrid
    auto grid = std::make_unique<DesktopGrid>(&gridPages_, &items_, this);
    containers_.push_back(std::move(grid));

    // DesktopIcon for each NON-collected DesktopItem
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        if (collectedKeys.contains(ToUpperInvariant(item.layoutKey))) continue;
        auto icon = std::make_unique<DesktopIcon>(&item, containers_.back().get(), this);
        items_oo_.push_back(std::move(icon));
    }

    // Widgets
    for (auto& w : widgets_)
    {
        // Collection popups need their WidgetContainer for selection, sorting
        // and drops. Keep grouped collections in the runtime tree at all
        // times; their empty model bounds keep them off the desktop. Other
        // grouped source widgets remain hosted exclusively by their group.
        if (IsGroupedWidget(w) &&
            w.type != DesktopWidgetType::Collection)
            continue;
        const bool dockExclusive = IsDockExclusiveWidgetId(w.id);
        auto widget = CreateWidget(&w, this);
        if (!widget) continue;

        if (auto* wc = dynamic_cast<WidgetContainer*>(widget.get()))
        {
            // Dock-exclusive collections still need a runtime container so
            // their popup can participate in selection, reorder and drops.
            // Their saved Dock page has no grid rect, so normal widget drawing
            // remains suppressed by the empty bounds.
            widget.release();
            containers_.push_back(std::unique_ptr<Container>(wc));
        }
        else if (!dockExclusive)
        {
            items_oo_.push_back(std::move(widget));
        }
    }

    if (generalSettings_.dockEnabled)
    {
        for (const RECT& dockArea : dockAreas_)
        {
            if (!IsRectEmptyRect(dockArea))
                containers_.push_back(
                    std::make_unique<DockContainer>(this, &dockEntries_, dockArea));
        }
    }
    if (floatingDockVisible_)
    {
        floatingDockContainer_ =
            SelectFloatingDockContainerForMonitor(
                floatingDockMonitor_);
        if (floatingDockContainer_)
            UpdateFloatingDockWindowBounds();
        else
            CloseFloatingDock();
    }
    RebindDragSourceAfterRebuild();
    if (wasDragging && !dragSession_.IsActive())
    {
        mouseDown_ = false;
        mouseDownWidgetIndex_ = static_cast<size_t>(-1);
        HideDragHintWindow();
        ReleaseCapture();
    }
    InvalidateDragStaticScene();
}

/**
 * @brief 枚举文件夹映射组件对应的物理目录，填充 folderEntries 列表。
 * @param widget 目标组件。
 */
inline void DesktopApp::EnumerateFolderMappingEntries(
    DesktopWidget& widget, bool enqueueIconLoads)
{
    const bool showHiddenItems = AreExplorerHiddenItemsVisible();

    struct OldFolderIcon {
        HBITMAP bitmap = nullptr;
        SIZE size{};
        int sysIconIndex = -1;
        bool shortcutArrow = false;
        bool isShortcut = false;
        bool isApplicationShortcut = false;
        IconState iconState = IconState::Loading;
    };
    std::unordered_map<std::wstring, OldFolderIcon> oldFolderIconCache;

    // Preserve the current entry state and bitmap across directory enumeration.
    for (auto& entry : widget.folderEntries) {
        if (!entry.fullPath.empty()) {
            OldFolderIcon old;
            old.bitmap = entry.iconBitmap;
            old.size = entry.iconBitmapSize;
            old.sysIconIndex = entry.sysIconIndex;
            old.shortcutArrow = entry.shortcutArrow;
            old.isShortcut = entry.isShortcut;
            old.isApplicationShortcut = entry.isApplicationShortcut;
            old.iconState = entry.iconState;
            oldFolderIconCache.emplace(ToUpperInvariant(entry.fullPath), std::move(old));
            entry.iconBitmap = nullptr;
        } else if (entry.iconBitmap) {
            DeleteObject(entry.iconBitmap);
        }
    }

    widget.folderEntries.clear();
    if (widget.sourceFolderPath.empty()) {
        for (auto& [key, old] : oldFolderIconCache) {
            if (old.bitmap) {
                EraseD2DIconCacheForBitmap(old.bitmap);
                DeleteObject(old.bitmap);
            }
        }
        return;
    }
    std::wstring search = widget.sourceFolderPath + L"\\*";
    WIN32_FIND_DATAW fd{};
    HANDLE hFind = FindFirstFileW(search.c_str(), &fd);
    if (hFind == INVALID_HANDLE_VALUE) {
        for (auto& [key, old] : oldFolderIconCache) {
            if (old.bitmap) {
                EraseD2DIconCacheForBitmap(old.bitmap);
                DeleteObject(old.bitmap);
            }
        }
        return;
    }
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (snowdesktop::shell_item_visibility::
                IsAlwaysHidden(fd.cFileName))
            continue;
        if (!showHiddenItems && (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        FolderEntry entry;
        entry.name = fd.cFileName;
        entry.fullPath = widget.sourceFolderPath + L"\\" + fd.cFileName;
        entry.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        entry.lastWriteTime = fd.ftLastWriteTime;
        SHFILEINFOW info{};
        SHGetFileInfoW(entry.fullPath.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX);
        entry.sysIconIndex = info.iIcon;

        auto oldIt = oldFolderIconCache.find(ToUpperInvariant(entry.fullPath));
        if (oldIt != oldFolderIconCache.end() && oldIt->second.sysIconIndex == entry.sysIconIndex) {
            entry.iconBitmap = oldIt->second.bitmap;
            entry.iconBitmapSize = oldIt->second.size;
            entry.shortcutArrow = oldIt->second.shortcutArrow;
            entry.isShortcut = oldIt->second.isShortcut;
            entry.isApplicationShortcut = oldIt->second.isApplicationShortcut;
            entry.iconState = oldIt->second.iconState;
            oldIt->second.bitmap = nullptr;
            oldFolderIconCache.erase(oldIt);
            if (enqueueIconLoads &&
                entry.iconState == IconState::IconReady)
            {
                IconLoadTask phase2;
                phase2.serial = iconLoadSerial_;
                phase2.widgetId = widget.id;
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                {
                    phase2.absolutePidl.reset(pidl);
                    phase2.folderPath = entry.fullPath;
                    phase2.sysIconIndex = entry.sysIconIndex;
                    phase2.isDesktopItem = false;
                    phase2.phase = IconLoadPhase::Phase2;
                    EnqueueIconLoad(std::move(phase2));
                }
            }
            else if (enqueueIconLoads &&
                entry.iconState == IconState::Loading)
            {
                IconLoadTask phase1;
                phase1.serial = iconLoadSerial_;
                phase1.widgetId = widget.id;
                phase1.folderPath = entry.fullPath;
                phase1.sysIconIndex = entry.sysIconIndex;
                phase1.parsingName = entry.name;
                phase1.isDesktopItem = false;
                phase1.phase = IconLoadPhase::Phase1;
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                {
                    phase1.absolutePidl.reset(pidl);
                    EnqueueIconLoad(std::move(phase1));
                }
            }
        } else {
            if (oldIt != oldFolderIconCache.end()) {
                if (oldIt->second.bitmap) {
                    EraseD2DIconCacheForBitmap(oldIt->second.bitmap);
                    DeleteObject(oldIt->second.bitmap);
                }
                oldFolderIconCache.erase(oldIt);
            }
            entry.iconBitmap = nullptr;
            entry.iconState = IconState::Loading;

            if (enqueueIconLoads)
            {
                IconLoadTask task;
                task.serial = iconLoadSerial_;
                task.widgetId = widget.id;
                task.layoutKey = ToUpperInvariant(entry.fullPath);
                PIDLIST_ABSOLUTE pidl = nullptr;
                if (SUCCEEDED(SHParseDisplayName(entry.fullPath.c_str(), nullptr, &pidl, 0, nullptr)))
                {
                    task.absolutePidl.reset(pidl);
                    task.sysIconIndex = entry.sysIconIndex;
                    task.parsingName = entry.name;
                    task.isDesktopItem = false;
                    task.folderPath = entry.fullPath;
                    task.phase = IconLoadPhase::Phase1;
                    EnqueueIconLoad(std::move(task));
                }
            }
        }
        widget.folderEntries.push_back(std::move(entry));
    } while (FindNextFileW(hFind, &fd));
    FindClose(hFind);
    for (auto& [key, old] : oldFolderIconCache) {
        if (old.bitmap) {
            EraseD2DIconCacheForBitmap(old.bitmap);
            DeleteObject(old.bitmap);
        }
    }
    std::sort(widget.folderEntries.begin(), widget.folderEntries.end(),
        [](const FolderEntry& a, const FolderEntry& b) {
            if (a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
            return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
        });
    if (snowdesktop::folder_sort_rules::
            NormalizeMode(widget.folderSortMode) !=
        snowdesktop::folder_sort_rules::kManual)
    {
        snowdesktop::folder_sort_rules::StableSort(
            widget.folderEntries,
            widget.folderSortMode,
            widget.folderSortAscending);
    }
    else if (!widget.itemKeys.empty())
    {
        std::unordered_map<std::wstring, size_t> order;
        for (size_t i = 0; i < widget.itemKeys.size(); ++i)
            order[ToUpperInvariant(widget.itemKeys[i])] = i;
        std::stable_sort(widget.folderEntries.begin(), widget.folderEntries.end(),
            [&](const FolderEntry& a, const FolderEntry& b) {
                auto ia = order.find(ToUpperInvariant(a.fullPath));
                auto ib = order.find(ToUpperInvariant(b.fullPath));
                bool ha = ia != order.end();
                bool hb = ib != order.end();
                if (ha != hb) return ha;
                if (ha && hb) return ia->second < ib->second;
                return false;
            });
    }
    snowdesktop::folder_sort_rules::RewriteOrderKeys(
        widget.folderEntries, widget.itemKeys);
}

/**
 * @brief 刷新文件夹映射组件的内容（重新枚举目录）。
 * @param widgetIndex 组件索引。
 */
inline void DesktopApp::RefreshFolderMappingWidget(size_t widgetIndex)
{
    if (widgetIndex >= widgets_.size()) return;
    auto& w = widgets_[widgetIndex];
    EnumerateFolderMappingEntries(w);
    for (auto& c : containers_)
    {
        auto* wc = dynamic_cast<WidgetContainer*>(c.get());
        if (wc && wc->GetWidgetData() == &w)
        {
            if (auto* mapping = dynamic_cast<FolderMapping*>(wc))
                mapping->InvalidateFilterCache();
            else
                wc->InvalidateSlots();
            break;
        }
    }
    // NOTE: caller must RebuildContainersAndItems + SaveLayoutSlots + InvalidateDesktop
}

/**
 * @brief 收集桌面文件到文件分类组件中。
 * @param widgetIndex 组件索引。
 * @param persist 是否立即持久化布局。
 * @return 有变化返回 true。
 */
inline bool DesktopApp::CollectFileCategoryWidget(size_t widgetIndex, bool persist)
{
    if (widgetIndex >= widgets_.size() ||
        widgets_[widgetIndex].type != DesktopWidgetType::FileCategories)
        return false;

    FileCategories collector(&widgets_[widgetIndex], this);
    bool changed = collector.CollectTopLevelDesktopItems();
    if (!changed) return false;
    RefreshCollectedKeysCache();

    if (persist)
    {
        LayoutItems();
        RebuildContainersAndItems();
        SaveLayoutSlots();
        InvalidateRect(hwnd_, nullptr, TRUE);
    }
    return true;
}

/**
 * @brief 确保只有一个文件分类组件开启自动收集模式。
 * @param activeWidgetIndex 当前激活的组件索引。
 */
inline void DesktopApp::EnforceSingleAutoCollectFileCategory(size_t activeWidgetIndex)
{
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i != activeWidgetIndex && widgets_[i].type == DesktopWidgetType::FileCategories)
            widgets_[i].autoCollect = false;
    }
}

/**
 * @brief 应用所有开启了 autoCollect 的文件分类组件的自动收集。
 */
inline void DesktopApp::ApplyAutoCollectFileCategoryWidgets()
{
    size_t active = static_cast<size_t>(-1);
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (widgets_[i].type != DesktopWidgetType::FileCategories)
            continue;

        FileCategories collector(&widgets_[i], this);
        collector.PruneUncollectableItems();

        if (widgets_[i].autoCollect)
        {
            if (active == static_cast<size_t>(-1))
                active = i;
            else
                widgets_[i].autoCollect = false;
        }
    }
    RefreshCollectedKeysCache();
    if (active != static_cast<size_t>(-1))
        CollectFileCategoryWidget(active, false);
}

// ── 组件创建辅助函数 ──────────────────────────────────

/**
 * @brief 生成一个新的唯一组件 ID。
 * @return 组件 ID 字符串。
 */
inline std::wstring DesktopApp::MakeNewWidgetId() const
{
    return L"widget-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(widgets_.size() + 1);
}

inline void DesktopApp::ConfigureWidgetGridLimits(DesktopWidget& widget) const
{
    widget.minGridSpan = { 1, 1 };
    widget.maxGridSpan = { 0, 0 };

    if (widget.type == DesktopWidgetType::CollectionGroup ||
        widget.type == DesktopWidgetType::FileGroup ||
        widget.type == DesktopWidgetType::FileCategories ||
        widget.type == DesktopWidgetType::FolderMapping)
    {
        widget.minGridSpan = { 2, 2 };
    }
    else if (widget.type == DesktopWidgetType::LuaScript && !widget.packageId.empty())
    {
        LuaWidgetManifest manifest = WidgetEngine::GetWidgetManifest(widget.packageId);
        widget.minGridSpan = {
            std::max(1, manifest.minColumns),
            std::max(1, manifest.minRows)
        };
        widget.maxGridSpan = {
            std::max(0, manifest.maxColumns),
            std::max(0, manifest.maxRows)
        };
    }
}

inline GridSpan DesktopApp::ClampWidgetGridSpan(const DesktopWidget& widget, GridSpan span,
    int availableColumns, int availableRows) const
{
    const int pageMaxColumns = std::max(1, availableColumns);
    const int pageMaxRows = std::max(1, availableRows);
    const int maxColumns = widget.maxGridSpan.columns > 0
        ? std::min(pageMaxColumns, widget.maxGridSpan.columns)
        : pageMaxColumns;
    const int maxRows = widget.maxGridSpan.rows > 0
        ? std::min(pageMaxRows, widget.maxGridSpan.rows)
        : pageMaxRows;
    const int minColumns = std::min(maxColumns, std::max(1, widget.minGridSpan.columns));
    const int minRows = std::min(maxRows, std::max(1, widget.minGridSpan.rows));

    span.columns = std::clamp(span.columns, minColumns, maxColumns);
    span.rows = std::clamp(span.rows, minRows, maxRows);
    return span;
}

/**
 * @brief 将组件添加到网格中，自动查找空闲位置。
 * @param widget 组件对象（移动语义）。
 * @param span 组件跨度。
 */
inline void DesktopApp::AddWidgetToGrid(DesktopWidget&& widget, GridSpan span)
{
    ConfigureWidgetGridLimits(widget);
    const GridPage* page = GridPageFromScreenPoint(lastContextMenuScreenPoint_);
    GridCell cell;
    if (page)
    {
        cell.pageId = page->id;
        POINT clientPoint = lastContextMenuScreenPoint_;
        ScreenToClient(hwnd_, &clientPoint);
        cell.column = GetGridAxisIndexFromPoint(*page, clientPoint.x, true);
        cell.row = GetGridAxisIndexFromPoint(*page, clientPoint.y, false);
    }
    if (cell.pageId.empty())
    {
        if (const GridPage* firstPage = GetFirstPageGridPage())
            cell = { firstPage->id, 0, 0 };
        if (cell.pageId.empty()) return;
    }

    std::unordered_set<std::wstring> usedSlots;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w))
            MarkGridArea(usedSlots, w.gridCell, w.gridSpan);
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell freeCell;
    const GridPage* cellPage = FindGridPage(gridPages_, cell.pageId);
    if (cellPage)
        span = ClampWidgetGridSpan(widget, span, cellPage->columns, cellPage->rows);
    bool needSearch = AreGridSlotsMarked(usedSlots, cell, span) ||
        !IsGridAreaValid(cell, span);
    if (!needSearch && cellPage)
    {
        if (cell.column + span.columns > cellPage->columns ||
            cell.row + span.rows > cellPage->rows)
            needSearch = true;
    }
    if (needSearch)
    {
        int startSlot = 0;
        const GridPage* searchPage = FindGridPage(gridPages_, cell.pageId);
        if (searchPage)
            startSlot = cell.column * std::max(1, searchPage->rows) + cell.row;
        if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, startSlot))
        {
            if (!TryFindFreeCell(span, usedSlots, freeCell, cell.pageId, 0))
                TryFindFreeCell(span, usedSlots, freeCell, L"", 0);
        }
        if (freeCell.pageId.empty()) return;
        cell = freeCell;
    }

    widget.gridCell = cell;
    widget.gridSpan = span;
    widgets_.push_back(std::move(widget));
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 在右键菜单位置添加集合组件。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::AddCollectionWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::Collection;
    w.title = _LW("widget.collection");
    w.showTitle = true;
    w.bottomBarHover = true;
    AddWidgetToGrid(std::move(w), { 1, 1 });
    ShowWidgetAddedHint();
}

/**
 * @brief 在右键菜单位置添加集合组组件。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::AddCollectionGroupWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget widget;
    widget.id = MakeNewWidgetId();
    widget.type = DesktopWidgetType::CollectionGroup;
    widget.title = _LW("widget.collection_group");
    widget.showTitle = true;
    AddWidgetToGrid(std::move(widget), { 2, 2 });
    ShowWidgetAddedHint();
}

inline void DesktopApp::AddFileGroupWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget widget;
    widget.id = MakeNewWidgetId();
    widget.type = DesktopWidgetType::FileGroup;
    widget.title = _LW("widget.file_group");
    widget.showTitle = true;
    widget.showFileCategories = true;
    widget.showSearchBox = false;
    widget.listMode = false;
    widget.dateHeaders = false;
    AddWidgetToGrid(std::move(widget), { 2, 2 });
    ShowWidgetAddedHint();
}

inline size_t DesktopApp::HitTestCollectionGroupIndex(
    POINT point, size_t excludeWidgetIndex) const
{
    for (size_t i = widgets_.size(); i-- > 0;)
    {
        if (i == excludeWidgetIndex ||
            widgets_[i].type != DesktopWidgetType::CollectionGroup)
            continue;
        RECT bounds = widgets_[i].bounds;
        if (!IsRectEmptyRect(bounds) && PtInRect(&bounds, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

inline size_t DesktopApp::HitTestFileGroupIndex(
    POINT point, size_t excludeWidgetIndex) const
{
    for (size_t i = widgets_.size(); i-- > 0;)
    {
        if (i == excludeWidgetIndex ||
            widgets_[i].type != DesktopWidgetType::FileGroup)
            continue;
        RECT bounds = widgets_[i].bounds;
        if (!IsRectEmptyRect(bounds) && PtInRect(&bounds, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

inline bool DesktopApp::AddWidgetToFileGroup(
    size_t childIndex, size_t groupIndex, size_t insertIndex)
{
    if (childIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        childIndex == groupIndex ||
        widgets_[groupIndex].type != DesktopWidgetType::FileGroup)
        return false;
    const DesktopWidgetType childType = widgets_[childIndex].type;
    const auto childKind =
        childType == DesktopWidgetType::FileCategories
            ? snowdesktop::collection_group_rules::
                FileGroupChildKind::DesktopFileCategories
            : (childType ==
                    DesktopWidgetType::FolderMapping
                ? snowdesktop::collection_group_rules::
                    FileGroupChildKind::FolderMapping
                : snowdesktop::collection_group_rules::
                    FileGroupChildKind::Collection);
    if (!snowdesktop::collection_group_rules::
            AcceptsFileGroupChild(childKind))
        return false;

    const std::wstring childId = widgets_[childIndex].id;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::FileGroup) continue;
        std::erase(widget.childWidgetIds, childId);
        if (widget.activeCategoryId == childId)
            widget.activeCategoryId =
                widget.childWidgetIds.empty()
                    ? L""
                    : widget.childWidgetIds.front();
    }

    DesktopWidget& group = widgets_[groupIndex];
    if (insertIndex == static_cast<size_t>(-1))
        insertIndex = group.childWidgetIds.size();
    insertIndex = std::min(insertIndex, group.childWidgetIds.size());
    group.childWidgetIds.insert(
        group.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(insertIndex),
        childId);
    if (group.activeCategoryId.empty())
        group.activeCategoryId = childId;
    widgets_[childIndex].selected = false;
    group.scrollOffset = 0;
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline bool DesktopApp::MoveFolderMappingsToFileGroup(
    const std::vector<Item*>& sourceItems,
    size_t groupIndex, size_t insertIndex)
{
    if (sourceItems.empty() ||
        groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type !=
            DesktopWidgetType::FileGroup)
        return false;

    std::vector<std::wstring> movingIds;
    for (Item* source : sourceItems)
    {
        std::wstring id;
        if (auto* dockItem =
                dynamic_cast<DockEntryItem*>(source))
        {
            if (dockItem->GetEntryType() !=
                    DockEntryType::FolderMapping)
                return false;
            id = dockItem->GetReference();
        }
        else if (auto* groupEntry =
                     dynamic_cast<
                         FileGroupEntryItem*>(
                         source))
        {
            id = groupEntry->GetChildWidgetId();
        }
        else if (auto* widget =
                     dynamic_cast<Widget*>(source))
        {
            DesktopWidget* data =
                widget->GetWidgetData();
            if (!data ||
                data->type !=
                    DesktopWidgetType::
                        FolderMapping)
                return false;
            id = data->id;
        }
        else
        {
            return false;
        }

        const size_t childIndex =
            FindWidgetIndexById(id);
        if (childIndex >= widgets_.size() ||
            widgets_[childIndex].type !=
                DesktopWidgetType::FolderMapping)
            return false;
        if (std::find(
                movingIds.begin(),
                movingIds.end(), id) ==
            movingIds.end())
            movingIds.push_back(
                std::move(id));
    }
    if (movingIds.empty())
        return false;

    DesktopWidget& target =
        widgets_[groupIndex];
    const std::wstring previousActive =
        target.activeCategoryId;
    insertIndex = std::min(
        insertIndex,
        target.childWidgetIds.size());
    size_t removedBefore = 0;
    for (const auto& id : movingIds)
    {
        const auto existing =
            std::find(
                target.childWidgetIds.begin(),
                target.childWidgetIds.end(),
                id);
        if (existing !=
                target.childWidgetIds.end() &&
            static_cast<size_t>(
                std::distance(
                    target.childWidgetIds.begin(),
                    existing)) < insertIndex)
            ++removedBefore;
    }

    for (auto& group : widgets_)
    {
        if (group.type !=
                DesktopWidgetType::FileGroup)
            continue;
        for (const auto& id : movingIds)
            std::erase(
                group.childWidgetIds, id);
        group.activeCategoryId =
            snowdesktop::
                collection_group_rules::
                    ResolveActiveItem(
                        group.childWidgetIds,
                        group.activeCategoryId);
    }

    const size_t insertAt =
        std::min(
            insertIndex > removedBefore
                ? insertIndex - removedBefore
                : 0,
            target.childWidgetIds.size());
    target.childWidgetIds.insert(
        target.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(
                insertAt),
        movingIds.begin(), movingIds.end());
    target.activeCategoryId =
        snowdesktop::
            collection_group_rules::
                ResolveActiveItem(
                    target.childWidgetIds,
                    previousActive);

    const std::unordered_set<std::wstring>
        movingSet(
            movingIds.begin(), movingIds.end());
    std::erase_if(
        dockEntries_,
        [&](const DockEntry& entry) {
            return entry.type ==
                    DockEntryType::
                        FolderMapping &&
                movingSet.contains(
                    entry.reference);
        });
    for (const auto& id : movingIds)
    {
        const size_t childIndex =
            FindWidgetIndexById(id);
        if (childIndex < widgets_.size())
            widgets_[childIndex].
                selected = false;
    }

    NormalizeDockRecycleBinPosition();
    RefreshCollectedKeysCache();
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateDragStaticScene();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline bool DesktopApp::ReleaseWidgetFromFileGroup(
    const std::wstring& childId, GridCell preferredCell)
{
    const size_t childIndex = FindWidgetIndexById(childId);
    const size_t groupIndex = FindFileGroupIndexForChild(childId);
    if (childIndex >= widgets_.size() ||
        groupIndex >= widgets_.size())
        return false;
    const DesktopWidgetType childType = widgets_[childIndex].type;
    if (childType != DesktopWidgetType::FileCategories &&
        childType != DesktopWidgetType::FolderMapping)
        return false;

    DesktopWidget& group = widgets_[groupIndex];
    auto membership = std::find(
        group.childWidgetIds.begin(),
        group.childWidgetIds.end(), childId);
    const size_t membershipIndex =
        membership == group.childWidgetIds.end()
            ? group.childWidgetIds.size()
            : static_cast<size_t>(std::distance(
                group.childWidgetIds.begin(), membership));
    const std::wstring previousActive = group.activeCategoryId;
    std::erase(group.childWidgetIds, childId);
    if (group.activeCategoryId == childId)
        group.activeCategoryId =
            group.childWidgetIds.empty()
                ? L""
                : group.childWidgetIds[
                    std::min(membershipIndex,
                        group.childWidgetIds.size() - 1)];

    DesktopWidget& child = widgets_[childIndex];
    child.selected = false;
    const GridPage* page =
        FindGridPage(gridPages_, preferredCell.pageId);
    if (!page)
    {
        preferredCell = group.gridCell;
        page = FindGridPage(gridPages_, preferredCell.pageId);
    }
    auto restoreMembership = [&]() {
        group.childWidgetIds.insert(
            group.childWidgetIds.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(membershipIndex,
                        group.childWidgetIds.size())),
            childId);
        group.activeCategoryId = previousActive;
    };
    if (!page)
    {
        restoreMembership();
        return false;
    }

    GridSpan span = ClampWidgetGridSpan(
        child, child.gridSpan, page->columns, page->rows);
    preferredCell.column = std::clamp(
        preferredCell.column, 0,
        std::max(0, page->columns - span.columns));
    preferredCell.row = std::clamp(
        preferredCell.row, 0,
        std::max(0, page->rows - span.rows));

    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == childIndex || IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(
            usedSlots, widgets_[i].gridCell,
            widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell landing = preferredCell;
    if (!IsGridAreaValid(landing, span) ||
        AreGridSlotsMarked(usedSlots, landing, span))
    {
        const int startSlot =
            SlotFromCell(gridPages_, preferredCell);
        if (!TryFindFreeCell(
                span, usedSlots, landing,
                preferredCell.pageId, startSlot) &&
            !TryFindFreeCell(
                span, usedSlots, landing, L"", 0))
        {
            restoreMembership();
            return false;
        }
    }

    child.gridCell = landing;
    child.gridSpan = span;
    EnsureNavTabOrder();
    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline bool DesktopApp::AddCollectionToGroup(
    size_t collectionIndex, size_t groupIndex, size_t insertIndex)
{
    if (collectionIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        collectionIndex == groupIndex ||
        widgets_[collectionIndex].type != DesktopWidgetType::Collection ||
        widgets_[groupIndex].type != DesktopWidgetType::CollectionGroup)
        return false;

    const std::wstring collectionId = widgets_[collectionIndex].id;
    for (auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::CollectionGroup) continue;
        std::erase(widget.childWidgetIds, collectionId);
        if (widget.activeCategoryId == collectionId)
            widget.activeCategoryId =
                widget.childWidgetIds.empty()
                    ? L""
                    : widget.childWidgetIds.front();
    }

    DesktopWidget& group = widgets_[groupIndex];
    if (insertIndex == static_cast<size_t>(-1))
        insertIndex = group.childWidgetIds.size();
    insertIndex = std::min(insertIndex, group.childWidgetIds.size());
    group.childWidgetIds.insert(
        group.childWidgetIds.begin() +
            static_cast<std::ptrdiff_t>(insertIndex),
        collectionId);
    if (group.activeCategoryId.empty())
        group.activeCategoryId = collectionId;

    std::erase_if(dockEntries_, [&](const DockEntry& entry) {
        return entry.type == DockEntryType::Collection &&
            entry.reference == collectionId;
    });
    if (popupWidgetIndex_ == collectionIndex)
        CloseCollectionPopup();
    widgets_[collectionIndex].selected = false;
    group.scrollOffset = std::clamp(
        group.scrollOffset, 0, INT_MAX);
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline bool DesktopApp::ReleaseCollectionFromGroup(
    const std::wstring& collectionId, GridCell preferredCell)
{
    const size_t collectionIndex = FindWidgetIndexById(collectionId);
    const size_t groupIndex =
        FindCollectionGroupIndexForChild(collectionId);
    if (collectionIndex >= widgets_.size() ||
        groupIndex >= widgets_.size() ||
        widgets_[collectionIndex].type != DesktopWidgetType::Collection)
        return false;

    DesktopWidget& group = widgets_[groupIndex];
    auto membership = std::find(
        group.childWidgetIds.begin(),
        group.childWidgetIds.end(), collectionId);
    const size_t membershipIndex =
        membership == group.childWidgetIds.end()
            ? group.childWidgetIds.size()
            : static_cast<size_t>(
                std::distance(group.childWidgetIds.begin(), membership));
    const std::wstring previousActiveCategory =
        group.activeCategoryId;
    std::erase(group.childWidgetIds, collectionId);
    if (group.activeCategoryId == collectionId)
        group.activeCategoryId =
            group.childWidgetIds.empty()
                ? L""
                : group.childWidgetIds[
                    std::min(
                        membershipIndex,
                        group.childWidgetIds.size() - 1)];
    DesktopWidget& collection = widgets_[collectionIndex];
    collection.selected = false;

    const GridPage* page =
        FindGridPage(gridPages_, preferredCell.pageId);
    if (!page)
    {
        preferredCell = group.gridCell;
        page = FindGridPage(gridPages_, preferredCell.pageId);
    }
    if (!page)
    {
        group.childWidgetIds.insert(
            group.childWidgetIds.begin() +
                static_cast<std::ptrdiff_t>(
                    std::min(membershipIndex,
                        group.childWidgetIds.size())),
            collectionId);
        group.activeCategoryId = previousActiveCategory;
        return false;
    }

    GridSpan span = ClampWidgetGridSpan(collection, collection.gridSpan,
        page->columns, page->rows);
    preferredCell.column = std::clamp(
        preferredCell.column, 0,
        std::max(0, page->columns - span.columns));
    preferredCell.row = std::clamp(
        preferredCell.row, 0,
        std::max(0, page->rows - span.rows));

    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == collectionIndex || IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(usedSlots,
            widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    GridCell landing = preferredCell;
    if (!IsGridAreaValid(landing, span) ||
        AreGridSlotsMarked(usedSlots, landing, span))
    {
        const int startSlot =
            SlotFromCell(gridPages_, preferredCell);
        if (!TryFindFreeCell(span, usedSlots, landing,
            preferredCell.pageId, startSlot) &&
            !TryFindFreeCell(span, usedSlots, landing, L"", 0))
        {
            group.childWidgetIds.insert(
                group.childWidgetIds.begin() +
                    static_cast<std::ptrdiff_t>(
                        std::min(membershipIndex,
                            group.childWidgetIds.size())),
                collectionId);
            group.activeCategoryId =
                previousActiveCategory;
            return false;
        }
    }

    collection.gridCell = landing;
    collection.gridSpan = span;
    EnsureNavTabOrder();
    LayoutItems();
    SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    return true;
}

inline void DesktopApp::ReleaseCollectionGroupChildren(size_t groupIndex)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type != DesktopWidgetType::CollectionGroup)
        return;

    DesktopWidget& group = widgets_[groupIndex];
    const std::vector<std::wstring> childIds =
        snowdesktop::collection_group_rules::
            TakeAllForRelease(
                group.childWidgetIds,
                group.activeCategoryId);

    std::unordered_set<std::wstring> childSet(
        childIds.begin(), childIds.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == groupIndex ||
            childSet.contains(widgets_[i].id) ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(usedSlots,
            widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    const std::wstring preferredPage = group.gridCell.pageId;
    int startSlot = SlotFromCell(gridPages_, group.gridCell);
    for (const auto& childId : childIds)
    {
        const size_t childIndex = FindWidgetIndexById(childId);
        if (childIndex >= widgets_.size() ||
            widgets_[childIndex].type != DesktopWidgetType::Collection)
            continue;
        DesktopWidget& child = widgets_[childIndex];
        GridCell landing;
        if (!TryFindFreeCell(child.gridSpan, usedSlots, landing,
            preferredPage, startSlot) &&
            !TryFindFreeCell(child.gridSpan, usedSlots, landing, L"", 0))
            continue;
        child.gridCell = landing;
        child.selected = false;
        MarkGridArea(usedSlots, landing, child.gridSpan);
        startSlot = SlotFromCell(gridPages_, landing) + 1;
    }
}

inline void DesktopApp::ReleaseFileGroupChildren(size_t groupIndex)
{
    if (groupIndex >= widgets_.size() ||
        widgets_[groupIndex].type != DesktopWidgetType::FileGroup)
        return;

    DesktopWidget& group = widgets_[groupIndex];
    const std::vector<std::wstring> childIds =
        snowdesktop::collection_group_rules::
            TakeAllForRelease(
                group.childWidgetIds,
                group.activeCategoryId);

    std::unordered_set<std::wstring> childSet(
        childIds.begin(), childIds.end());
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == groupIndex ||
            childSet.contains(widgets_[i].id) ||
            IsGroupedWidget(widgets_[i]))
            continue;
        MarkGridArea(
            usedSlots, widgets_[i].gridCell,
            widgets_[i].gridSpan);
    }
    for (const auto& item : items_)
    {
        if (item.name.empty() || IsItemInAnyWidget(item)) continue;
        MarkGridArea(usedSlots, item.gridCell, item.gridSpan);
    }

    const std::wstring preferredPage = group.gridCell.pageId;
    int startSlot = SlotFromCell(gridPages_, group.gridCell);
    for (const auto& childId : childIds)
    {
        const size_t childIndex = FindWidgetIndexById(childId);
        if (childIndex >= widgets_.size())
            continue;
        DesktopWidget& child = widgets_[childIndex];
        if (child.type != DesktopWidgetType::FileCategories &&
            child.type != DesktopWidgetType::FolderMapping)
            continue;
        GridCell landing;
        if (!TryFindFreeCell(
                child.gridSpan, usedSlots, landing,
                preferredPage, startSlot) &&
            !TryFindFreeCell(
                child.gridSpan, usedSlots, landing, L"", 0) &&
            !FindDockReturnCell(
                usedSlots, preferredPage, startSlot,
                child.gridSpan, landing))
            continue;
        child.gridCell = landing;
        child.selected = false;
        MarkGridArea(usedSlots, landing, child.gridSpan);
        startSlot = FindGridPage(
                gridPages_, landing.pageId)
            ? SlotFromCell(gridPages_, landing) + 1
            : 0;
    }
}

/**
 * @brief 在右键菜单位置添加桌面文件分类组件。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::AddFileCategoryWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;
    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::FileCategories;
    w.title = _LW("widget.desktop_files");
    w.showTitle = true;
    AddWidgetToGrid(std::move(w), { 2, 2 });
    ShowWidgetAddedHint();
}

/**
 * @brief 在右键菜单位置添加文件夹映射组件（弹出文件夹选择对话框）。
 * @param screenPoint 屏幕坐标点。
 */
inline void DesktopApp::AddFolderMappingWidgetAt(POINT screenPoint)
{
    lastContextMenuScreenPoint_ = screenPoint;

    // Pick source folder via modern file-explorer-style dialog
    std::wstring folderPath;
    {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        IFileOpenDialog* pfd = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                        IID_PPV_ARGS(&pfd))))
        {
            pfd->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
            pfd->SetTitle(_LW("app.interact.select_folder"));
            if (SUCCEEDED(pfd->Show(hwnd_)))
            {
                IShellItem* psi = nullptr;
                if (SUCCEEDED(pfd->GetResult(&psi)))
                {
                    PWSTR pszPath = nullptr;
                    if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath)))
                    {
                        folderPath = pszPath;
                        CoTaskMemFree(pszPath);
                    }
                    psi->Release();
                }
            }
            pfd->Release();
        }
        CoUninitialize();
    }
    if (folderPath.empty()) return;
    std::wstring title = folderPath;
    if (!title.empty() && title.back() == L'\\') title.pop_back();
    size_t lastSep = title.find_last_of(L"\\/");
    title = (lastSep != std::wstring::npos) ? title.substr(lastSep + 1) : title;

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::FolderMapping;
    w.title = title;
    w.showTitle = true;
    w.sourceFolderPath = folderPath;
    AddWidgetToGrid(std::move(w), { 2, 2 });

    // Enumerate entries
    size_t idx = widgets_.size() - 1;
    EnumerateFolderMappingEntries(widgets_[idx]);
    RebuildContainersAndItems();
    ShowWidgetAddedHint();
}

/**
 * @brief 在右键菜单位置添加 Lua 脚本组件。
 * @param screenPoint 屏幕坐标点。
 * @param scriptFilename 脚本文件名。
 */
inline void DesktopApp::AddLuaWidgetAt(POINT screenPoint, const std::wstring& packageId)
{
    if (packageId.empty()) return;
    lastContextMenuScreenPoint_ = screenPoint;

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::LuaScript;
    w.title = WidgetEngine::GetWidgetDisplayName(packageId);
    if (w.title.empty())
    {
        w.title = packageId;
    }
    w.packageId = packageId;
    w.bottomBarHover = true;
    if (widgetEngine_)
    {
        widgetEngine_->EnsureWidgetLoaded(w.id, packageId);
        w.showTitle = widgetEngine_->ReadBoolFlag(packageId, "showTitle", false);
        w.bottomBarHover = widgetEngine_->ReadBoolFlag(packageId, "bottomBarHover", true);
    }
    int defaultColumns = 1;
    int defaultRows = 1;
    WidgetEngine::GetWidgetDefaultSpan(packageId, defaultColumns, defaultRows);
    AddWidgetToGrid(std::move(w), { defaultColumns, defaultRows });
    ShowWidgetAddedHint();
}

/**
 * @brief 将组件放置到指定网格位置，并重新安置被挤占的桌面项。
 * @param widgetIndex 组件索引。
 * @param targetCell 目标单元格。
 * @param targetSpan 目标跨度。
 * @param isMove 是否为移动操作（而非缩放）。
 */
inline void DesktopApp::PlaceWidgetWithDisplacement(size_t widgetIndex, GridCell targetCell, GridSpan targetSpan, bool isMove)
{
    if (widgetIndex >= widgets_.size()) return;
    const GridPage* page = FindGridPage(gridPages_, targetCell.pageId);
    if (!page) return;

    targetSpan = ClampWidgetGridSpan(widgets_[widgetIndex], targetSpan,
        page->columns - targetCell.column, page->rows - targetCell.row);

    // Widget-widget collision check
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex) continue;
        if (IsGroupedWidget(widgets_[i])) continue;
        if (widgets_[i].gridCell.pageId != targetCell.pageId) continue;
        if (targetCell.column + targetSpan.columns <= widgets_[i].gridCell.column) continue;
        if (widgets_[i].gridCell.column + widgets_[i].gridSpan.columns <= targetCell.column) continue;
        if (targetCell.row + targetSpan.rows <= widgets_[i].gridCell.row) continue;
        if (widgets_[i].gridCell.row + widgets_[i].gridSpan.rows <= targetCell.row) continue;
        return; // overlaps another widget, reject
    }

    // Collect items displaced by this placement (at target location)
    std::vector<size_t> displaced;
    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        if (items_[i].gridCell.pageId != targetCell.pageId) continue;
        if (targetCell.column + targetSpan.columns <= items_[i].gridCell.column) continue;
        if (items_[i].gridCell.column + items_[i].gridSpan.columns <= targetCell.column) continue;
        if (targetCell.row + targetSpan.rows <= items_[i].gridCell.row) continue;
        if (items_[i].gridCell.row + items_[i].gridSpan.rows <= targetCell.row) continue;
        displaced.push_back(i);
    }

    // The widget's old cell
    GridCell oldCell = widgets_[widgetIndex].gridCell;
    GridSpan oldSpan = widgets_[widgetIndex].gridSpan;

    // Build occupied slot set (all widgets except self + non-displaced items)
    std::unordered_set<std::wstring> usedSlots;
    for (size_t i = 0; i < widgets_.size(); ++i)
    {
        if (i == widgetIndex) continue;
        if (IsGroupedWidget(widgets_[i])) continue;
        MarkGridArea(usedSlots, widgets_[i].gridCell, widgets_[i].gridSpan);
    }
    // Mark the new target area as occupied
    MarkGridArea(usedSlots, targetCell, targetSpan);

    for (size_t i = 0; i < items_.size(); ++i)
    {
        if (items_[i].name.empty()) continue;
        bool isDisplaced = std::find(displaced.begin(), displaced.end(), i) != displaced.end();
        if (!isDisplaced)
        {
            if (isMove)
            {
                // For move: also free items that overlap the old widget area,
                // so displaced items can be placed there.
                if (items_[i].gridCell.pageId == oldCell.pageId &&
                    !(items_[i].gridCell.column + items_[i].gridSpan.columns <= oldCell.column) &&
                    !(oldCell.column + oldSpan.columns <= items_[i].gridCell.column) &&
                    !(items_[i].gridCell.row + items_[i].gridSpan.rows <= oldCell.row) &&
                    !(oldCell.row + oldSpan.rows <= items_[i].gridCell.row))
                    continue;
            }
            MarkGridArea(usedSlots, items_[i].gridCell, items_[i].gridSpan);
        }
    }

    if (isMove)
    {
        // For move: displaced items go to the widget's old position area
        auto byGrid = [this](size_t a, size_t b) {
            if (items_[a].gridCell.pageId != items_[b].gridCell.pageId)
                return items_[a].gridCell.pageId < items_[b].gridCell.pageId;
            int sa = SlotFromCell(gridPages_, items_[a].gridCell);
            int sb = SlotFromCell(gridPages_, items_[b].gridCell);
            return sa < sb;
        };
        std::sort(displaced.begin(), displaced.end(), byGrid);

        widgets_[widgetIndex].gridCell = targetCell;
        widgets_[widgetIndex].gridSpan = targetSpan;

        // Quick-lookup of visible page IDs
        std::unordered_set<std::wstring> visiblePageIds;
        for (const auto& gp : gridPages_)
            visiblePageIds.insert(gp.id);

        int oldAreaSlot = SlotFromCell(gridPages_, oldCell);
        for (size_t idx : displaced)
        {
            GridCell freeCell;
            if (TryFindFreeCell(items_[idx].gridSpan, usedSlots, freeCell, oldCell.pageId, oldAreaSlot))
            {
                items_[idx].gridCell = freeCell;
                items_[idx].slot = SlotFromCell(gridPages_, freeCell);
                MarkGridArea(usedSlots, freeCell, items_[idx].gridSpan);
            }
            else if (TryFindFreeCell(items_[idx].gridSpan, usedSlots, freeCell, targetCell.pageId, 0))
            {
                items_[idx].gridCell = freeCell;
                items_[idx].slot = SlotFromCell(gridPages_, freeCell);
                MarkGridArea(usedSlots, freeCell, items_[idx].gridSpan);
            }
            else
            {
                // No visible cell — search all saved pages at other offsets
                bool foundAny = false;
                for (const auto& pageId : savedPageIds_)
                {
                    if (visiblePageIds.count(pageId)) continue;
                    if (!savedPageColumns_.count(pageId) || !savedPageRows_.count(pageId)) continue;
                    int cols = savedPageColumns_[pageId];
                    int rows = savedPageRows_[pageId];
                    int capacity = std::max(1, cols * rows);
                    for (int slot = 0; slot < capacity; ++slot)
                    {
                        GridCell candidate;
                        candidate.pageId = pageId;
                        candidate.column = slot / std::max(1, rows);
                        candidate.row    = slot % std::max(1, rows);
                        if (candidate.column + items_[idx].gridSpan.columns <= cols &&
                            candidate.row + items_[idx].gridSpan.rows <= rows &&
                            !AreGridSlotsMarked(usedSlots, candidate, items_[idx].gridSpan))
                        {
                            items_[idx].gridCell = candidate;
                            items_[idx].slot = SlotFromCell(gridPages_, candidate);
                            MarkGridArea(usedSlots, candidate, items_[idx].gridSpan);
                            foundAny = true;
                            break;
                        }
                    }
                    if (foundAny) break;
                }
            }
        }
    }
    else
    {
        // For resize: push displaced items to new free cells
        auto byGrid = [this](size_t a, size_t b) {
            if (items_[a].gridCell.pageId != items_[b].gridCell.pageId)
                return items_[a].gridCell.pageId < items_[b].gridCell.pageId;
            int sa = SlotFromCell(gridPages_, items_[a].gridCell);
            int sb = SlotFromCell(gridPages_, items_[b].gridCell);
            return sa < sb;
        };
        std::sort(displaced.begin(), displaced.end(), byGrid);

        widgets_[widgetIndex].gridCell = targetCell;
        widgets_[widgetIndex].gridSpan = targetSpan;

        // Quick-lookup of visible page IDs
        std::unordered_set<std::wstring> visiblePageIds;
        for (const auto& gp : gridPages_)
            visiblePageIds.insert(gp.id);

        int searchStart = SlotFromCell(gridPages_, targetCell) + std::max(1, targetSpan.rows);
        for (size_t idx : displaced)
        {
            GridCell freeCell;
            if (TryFindFreeCell(items_[idx].gridSpan, usedSlots, freeCell, targetCell.pageId, searchStart))
            {
                items_[idx].gridCell = freeCell;
                items_[idx].slot = SlotFromCell(gridPages_, freeCell);
                MarkGridArea(usedSlots, freeCell, items_[idx].gridSpan);
            }
            else
            {
                // No visible cell — search all saved pages at other offsets
                bool foundAny = false;
                for (const auto& pageId : savedPageIds_)
                {
                    if (visiblePageIds.count(pageId)) continue;
                    if (!savedPageColumns_.count(pageId) || !savedPageRows_.count(pageId)) continue;
                    int cols = savedPageColumns_[pageId];
                    int rows = savedPageRows_[pageId];
                    int capacity = std::max(1, cols * rows);
                    for (int slot = 0; slot < capacity; ++slot)
                    {
                        GridCell candidate;
                        candidate.pageId = pageId;
                        candidate.column = slot / std::max(1, rows);
                        candidate.row    = slot % std::max(1, rows);
                        if (candidate.column + items_[idx].gridSpan.columns <= cols &&
                            candidate.row + items_[idx].gridSpan.rows <= rows &&
                            !AreGridSlotsMarked(usedSlots, candidate, items_[idx].gridSpan))
                        {
                            items_[idx].gridCell = candidate;
                            items_[idx].slot = SlotFromCell(gridPages_, candidate);
                            MarkGridArea(usedSlots, candidate, items_[idx].gridSpan);
                            foundAny = true;
                            break;
                        }
                    }
                    if (foundAny) break;
                }
            }
        }
    }

    if (widgets_[widgetIndex].type == DesktopWidgetType::Collection &&
        widgets_[widgetIndex].scrollContainerMode &&
        (targetSpan.columns < 2 || targetSpan.rows < 2))
    {
        widgets_[widgetIndex].scrollContainerMode = false;
        widgets_[widgetIndex].scrollOffset = 0;
    }

    LayoutItems();
    RebuildContainersAndItems();
    SaveLayoutSlots();
}

namespace
{
    struct IconPixelBuffer
    {
        int width = 0;
        int height = 0;
        std::vector<std::uint32_t> pixels;
    };

    struct IconVisibleBounds
    {
        bool hasVisiblePixels = false;
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
    };

    struct IconBackgroundColor
    {
        int r = 246;
        int g = 247;
        int b = 250;
    };

    inline std::uint8_t PixelA(std::uint32_t pixel)
    {
        return static_cast<std::uint8_t>((pixel >> 24) & 0xff);
    }

    inline std::uint32_t PackBgra(int b, int g, int r, int a)
    {
        return (static_cast<std::uint32_t>(std::clamp(a, 0, 255)) << 24) |
            (static_cast<std::uint32_t>(std::clamp(r, 0, 255)) << 16) |
            (static_cast<std::uint32_t>(std::clamp(g, 0, 255)) << 8) |
            static_cast<std::uint32_t>(std::clamp(b, 0, 255));
    }

    inline std::uint32_t PackPremultipliedRgb(int r, int g, int b, int a)
    {
        return PackBgra(
            (b * a + 127) / 255,
            (g * a + 127) / 255,
            (r * a + 127) / 255,
            a);
    }

    inline void NormalizePremultipliedBgra(std::vector<std::uint32_t>& pixels)
    {
        bool hasAlpha = false;
        bool hasVisibleColor = false;
        for (std::uint32_t pixel : pixels)
        {
            if (PixelA(pixel) != 0)
                hasAlpha = true;
            if ((pixel & 0x00ffffff) != 0)
                hasVisibleColor = true;
        }

        if (!hasAlpha && hasVisibleColor)
        {
            for (std::uint32_t& pixel : pixels)
            {
                if ((pixel & 0x00ffffff) != 0)
                    pixel |= 0xff000000;
            }
        }

        bool needsPremultiply = false;
        for (std::uint32_t pixel : pixels)
        {
            const int a = PixelA(pixel);
            if (a == 0 || a == 255) continue;
            if (((pixel >> 16) & 0xff) > static_cast<std::uint32_t>(a) ||
                ((pixel >> 8) & 0xff) > static_cast<std::uint32_t>(a) ||
                (pixel & 0xff) > static_cast<std::uint32_t>(a))
            {
                needsPremultiply = true;
                break;
            }
        }

        for (std::uint32_t& pixel : pixels)
        {
            const int a = PixelA(pixel);
            if (a == 0)
            {
                pixel = 0;
                continue;
            }
            if (!needsPremultiply || a == 255)
                continue;

            const int r = static_cast<int>((pixel >> 16) & 0xff);
            const int g = static_cast<int>((pixel >> 8) & 0xff);
            const int b = static_cast<int>(pixel & 0xff);
            pixel = PackBgra(
                (b * a + 127) / 255,
                (g * a + 127) / 255,
                (r * a + 127) / 255,
                a);
        }
    }

    inline bool ReadHBitmapPixels(HBITMAP hbm, IconPixelBuffer& out)
    {
        BITMAP bm{};
        if (!hbm || GetObjectW(hbm, sizeof(bm), &bm) == 0)
            return false;

        const int width = bm.bmWidth;
        const int height = std::abs(bm.bmHeight);
        if (width <= 0 || height <= 0)
            return false;

        out.width = width;
        out.height = height;
        out.pixels.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

        if (bm.bmBits != nullptr && bm.bmBitsPixel == 32)
        {
            const auto* src = static_cast<const std::uint8_t*>(bm.bmBits);
            const int stride = std::abs(bm.bmWidthBytes);
            for (int y = 0; y < height; ++y)
            {
                std::memcpy(out.pixels.data() + static_cast<size_t>(y) * width,
                    src + static_cast<size_t>(y) * stride,
                    static_cast<size_t>(width) * sizeof(std::uint32_t));
            }
            NormalizePremultipliedBgra(out.pixels);
            return true;
        }

        HDC screenDc = GetDC(nullptr);
        if (!screenDc)
            return false;

        BITMAPINFO bitmapInfo{};
        bitmapInfo.bmiHeader.biSize = sizeof(bitmapInfo.bmiHeader);
        bitmapInfo.bmiHeader.biWidth = width;
        bitmapInfo.bmiHeader.biHeight = -height;
        bitmapInfo.bmiHeader.biPlanes = 1;
        bitmapInfo.bmiHeader.biBitCount = 32;
        bitmapInfo.bmiHeader.biCompression = BI_RGB;

        const bool ok = GetDIBits(screenDc, hbm, 0, static_cast<UINT>(height),
            out.pixels.data(), &bitmapInfo, DIB_RGB_COLORS) != 0;
        ReleaseDC(nullptr, screenDc);
        if (!ok)
            return false;

        NormalizePremultipliedBgra(out.pixels);
        return true;
    }

    inline IconVisibleBounds AnalyzeIconVisibleBounds(const std::vector<std::uint32_t>& pixels,
        int width, int height)
    {
        IconVisibleBounds bounds{};
        bounds.left = width;
        bounds.top = height;
        bounds.right = -1;
        bounds.bottom = -1;

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const std::uint8_t a = PixelA(pixels[static_cast<size_t>(y) * width + x]);
                if (a > 16)
                {
                    bounds.hasVisiblePixels = true;
                    bounds.left = std::min(bounds.left, x);
                    bounds.top = std::min(bounds.top, y);
                    bounds.right = std::max(bounds.right, x);
                    bounds.bottom = std::max(bounds.bottom, y);
                }
            }
        }

        return bounds;
    }

    inline IconBackgroundColor StraightIconColor(std::uint32_t pixel)
    {
        const int a = PixelA(pixel);
        if (a <= 0)
            return IconBackgroundColor{ 0, 0, 0 };

        return IconBackgroundColor{
            std::clamp((((static_cast<int>(pixel >> 16) & 0xff) * 255) + a / 2) / a, 0, 255),
            std::clamp((((static_cast<int>(pixel >> 8) & 0xff) * 255) + a / 2) / a, 0, 255),
            std::clamp((((static_cast<int>(pixel) & 0xff) * 255) + a / 2) / a, 0, 255)
        };
    }

    inline int IconColorDistanceSq(const IconBackgroundColor& lhs, const IconBackgroundColor& rhs)
    {
        const int dr = lhs.r - rhs.r;
        const int dg = lhs.g - rhs.g;
        const int db = lhs.b - rhs.b;
        return dr * dr + dg * dg + db * db;
    }

    inline bool DetectSolidEdgeBackground(const std::vector<std::uint32_t>& pixels,
        int width, int height, IconBackgroundColor& color)
    {
        if (width <= 2 || height <= 2)
            return false;

        constexpr int kEdgeAlpha = 16;
        constexpr int kReliableAlpha = 160;
        constexpr int kMaxInnerProbe = 4;
        constexpr int kColorBucketSize = 24;
        constexpr int kEdgeColorToleranceSq = 30 * 30 * 3;
        constexpr float kMinimumFillRatio = 0.992f;
        constexpr float kStrongEdgeDominantRatio = 0.86f;
        constexpr int kStrongEdgeSectorCount = 7;
        constexpr float kGradientEdgeDominantRatio = 0.55f;
        constexpr int kGradientEdgeSectorCount = 6;
        constexpr float kShapePlateExtentRatio = 0.80f;
        constexpr float kShapePlateStabilityRatio = 0.78f;
        constexpr float kShapePlateMaxAspectRatio = 1.12f;
        constexpr float kRoundedPlateMinCapRatio = 0.64f;
        constexpr float kShapePlateMaxCapRatio = 1.18f;
        constexpr float kGradientPlateMinCapRatio = 0.76f;
        constexpr float kGradientPlateMaxCapDelta = 0.12f;
        constexpr float kCirclePlateMaxAspectRatio = 1.08f;
        constexpr float kCirclePlateMaxCapRatio = 0.72f;
        constexpr float kCirclePlateMaxCapDelta = 0.14f;
        constexpr int kSectorCount = 8;
        constexpr float kPi = 3.14159265358979323846f;

        std::vector<int> left(static_cast<size_t>(height), -1);
        std::vector<int> right(static_cast<size_t>(height), -1);
        std::vector<int> top(static_cast<size_t>(width), -1);
        std::vector<int> bottom(static_cast<size_t>(width), -1);

        auto isVisible = [&](int x, int y) {
            return PixelA(pixels[static_cast<size_t>(y) * width + x]) > kEdgeAlpha;
        };

        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                if (isVisible(x, y))
                {
                    left[static_cast<size_t>(y)] = x;
                    break;
                }
            }
            for (int x = width - 1; x >= 0; --x)
            {
                if (isVisible(x, y))
                {
                    right[static_cast<size_t>(y)] = x;
                    break;
                }
            }
        }

        for (int x = 0; x < width; ++x)
        {
            for (int y = 0; y < height; ++y)
            {
                if (isVisible(x, y))
                {
                    top[static_cast<size_t>(x)] = y;
                    break;
                }
            }
            for (int y = height - 1; y >= 0; --y)
            {
                if (isVisible(x, y))
                {
                    bottom[static_cast<size_t>(x)] = y;
                    break;
                }
            }
        }

        int boundsLeft = width;
        int boundsRight = -1;
        int boundsTop = height;
        int boundsBottom = -1;
        for (int y = 0; y < height; ++y)
        {
            if (left[static_cast<size_t>(y)] < 0)
                continue;
            boundsLeft = std::min(boundsLeft, left[static_cast<size_t>(y)]);
            boundsRight = std::max(boundsRight, right[static_cast<size_t>(y)]);
            boundsTop = std::min(boundsTop, y);
            boundsBottom = std::max(boundsBottom, y);
        }

        if (boundsRight < boundsLeft || boundsBottom < boundsTop)
            return false;

        const int extentW = boundsRight - boundsLeft + 1;
        const int extentH = boundsBottom - boundsTop + 1;
        const float extentRatio = std::min(
            static_cast<float>(extentW) / static_cast<float>(width),
            static_cast<float>(extentH) / static_cast<float>(height));

        int filledPixels = 0;
        int expectedPixels = 0;
        for (int y = boundsTop; y <= boundsBottom; ++y)
        {
            const int rowLeft = left[static_cast<size_t>(y)];
            const int rowRight = right[static_cast<size_t>(y)];
            if (rowLeft < 0 || rowRight < rowLeft)
                return false;

            for (int x = rowLeft; x <= rowRight; ++x)
            {
                ++expectedPixels;
                if (!isVisible(x, y))
                    return false;
                ++filledPixels;
            }
        }
        const float fillRatio = expectedPixels > 0
            ? static_cast<float>(filledPixels) / static_cast<float>(expectedPixels)
            : 0.0f;
        if (fillRatio < kMinimumFillRatio)
            return false;

        auto stableEdgeColor = [&](int x, int y, int dx, int dy) {
            int bestX = x;
            int bestY = y;
            int bestAlpha = PixelA(pixels[static_cast<size_t>(y) * width + x]);

            for (int step = 1; step <= kMaxInnerProbe; ++step)
            {
                const int nx = x + dx * step;
                const int ny = y + dy * step;
                if (nx < 0 || ny < 0 || nx >= width || ny >= height)
                    break;

                const int alpha = PixelA(pixels[static_cast<size_t>(ny) * width + nx]);
                if (alpha <= kEdgeAlpha)
                    break;
                if (alpha > bestAlpha)
                {
                    bestAlpha = alpha;
                    bestX = nx;
                    bestY = ny;
                }
                if (alpha >= kReliableAlpha)
                    break;
            }

            return StraightIconColor(pixels[static_cast<size_t>(bestY) * width + bestX]);
        };

        struct IconEdgeSample
        {
            IconBackgroundColor color;
            int x = 0;
            int y = 0;
        };

        struct IconColorBucket
        {
            long long sumR = 0;
            long long sumG = 0;
            long long sumB = 0;
            int count = 0;
        };

        std::vector<IconEdgeSample> edgeSamples;
        edgeSamples.reserve(static_cast<size_t>((width + height) * 2));
        std::unordered_map<int, IconColorBucket> buckets;

        auto addSample = [&](IconBackgroundColor sample, int x, int y) {
            edgeSamples.push_back(IconEdgeSample{ sample, x, y });

            const int key =
                (std::clamp(sample.r / kColorBucketSize, 0, 255) << 16) |
                (std::clamp(sample.g / kColorBucketSize, 0, 255) << 8) |
                std::clamp(sample.b / kColorBucketSize, 0, 255);
            IconColorBucket& bucket = buckets[key];
            bucket.sumR += sample.r;
            bucket.sumG += sample.g;
            bucket.sumB += sample.b;
            ++bucket.count;
        };

        for (int y = boundsTop; y <= boundsBottom; ++y)
        {
            const int rowLeft = left[static_cast<size_t>(y)];
            const int rowRight = right[static_cast<size_t>(y)];
            addSample(stableEdgeColor(rowLeft, y, 1, 0), rowLeft, y);
            if (rowRight != rowLeft)
                addSample(stableEdgeColor(rowRight, y, -1, 0), rowRight, y);
        }

        for (int x = boundsLeft; x <= boundsRight; ++x)
        {
            const int colTop = top[static_cast<size_t>(x)];
            const int colBottom = bottom[static_cast<size_t>(x)];
            if (colTop < 0 || colBottom < colTop)
                return false;

            addSample(stableEdgeColor(x, colTop, 0, 1), x, colTop);
            if (colBottom != colTop)
                addSample(stableEdgeColor(x, colBottom, 0, -1), x, colBottom);
        }

        if (edgeSamples.empty())
            return false;

        const IconColorBucket* dominantBucket = nullptr;
        for (const auto& [_, bucket] : buckets)
        {
            if (!dominantBucket || bucket.count > dominantBucket->count)
                dominantBucket = &bucket;
        }
        if (!dominantBucket || dominantBucket->count <= 0)
            return false;

        const IconBackgroundColor dominant{
            std::clamp(static_cast<int>(
                (dominantBucket->sumR + dominantBucket->count / 2) / dominantBucket->count), 0, 255),
            std::clamp(static_cast<int>(
                (dominantBucket->sumG + dominantBucket->count / 2) / dominantBucket->count), 0, 255),
            std::clamp(static_cast<int>(
                (dominantBucket->sumB + dominantBucket->count / 2) / dominantBucket->count), 0, 255)
        };

        const float centerX = (static_cast<float>(boundsLeft + boundsRight) + 1.0f) * 0.5f;
        const float centerY = (static_cast<float>(boundsTop + boundsBottom) + 1.0f) * 0.5f;
        auto sectorForPoint = [&](int x, int y) {
            float angle = std::atan2(
                (static_cast<float>(y) + 0.5f) - centerY,
                (static_cast<float>(x) + 0.5f) - centerX);
            if (angle < 0.0f)
                angle += kPi * 2.0f;
            return std::clamp(
                static_cast<int>(std::floor(angle / (kPi * 2.0f) * static_cast<float>(kSectorCount))),
                0,
                kSectorCount - 1);
        };

        unsigned dominantSectors = 0;
        int closeCount = 0;
        for (const IconEdgeSample& sample : edgeSamples)
        {
            if (IconColorDistanceSq(sample.color, dominant) <= kEdgeColorToleranceSq)
            {
                ++closeCount;
                dominantSectors |= 1u << sectorForPoint(sample.x, sample.y);
            }
        }

        const int sampleCount = static_cast<int>(edgeSamples.size());
        const float edgeDominantRatio = sampleCount > 0
            ? static_cast<float>(closeCount) / static_cast<float>(sampleCount)
            : 0.0f;

        int sectorCount = 0;
        for (int i = 0; i < kSectorCount; ++i)
        {
            if ((dominantSectors & (1u << i)) != 0)
                ++sectorCount;
        }

        std::vector<int> rowWidths;
        rowWidths.reserve(static_cast<size_t>(extentH));
        for (int y = boundsTop; y <= boundsBottom; ++y)
        {
            if (left[static_cast<size_t>(y)] >= 0 && right[static_cast<size_t>(y)] >= left[static_cast<size_t>(y)])
                rowWidths.push_back(right[static_cast<size_t>(y)] - left[static_cast<size_t>(y)] + 1);
        }

        std::vector<int> columnHeights;
        columnHeights.reserve(static_cast<size_t>(extentW));
        for (int x = boundsLeft; x <= boundsRight; ++x)
        {
            if (top[static_cast<size_t>(x)] >= 0 && bottom[static_cast<size_t>(x)] >= top[static_cast<size_t>(x)])
                columnHeights.push_back(bottom[static_cast<size_t>(x)] - top[static_cast<size_t>(x)] + 1);
        }

        auto centeredStability = [](const std::vector<int>& values) {
            if (values.empty())
                return 0.0f;

            const size_t start = values.size() >= 4 ? values.size() / 4 : 0;
            const size_t end = values.size() >= 4 ? (values.size() * 3) / 4 : values.size();
            int minValue = values[start];
            int maxValue = values[start];
            for (size_t i = start + 1; i < end; ++i)
            {
                minValue = std::min(minValue, values[i]);
                maxValue = std::max(maxValue, values[i]);
            }

            return maxValue > 0
                ? static_cast<float>(minValue) / static_cast<float>(maxValue)
                : 0.0f;
        };

        auto averageSpan = [](const std::vector<int>& values, size_t start, size_t end) {
            if (values.empty() || start >= end)
                return 0.0f;

            long long sum = 0;
            for (size_t i = start; i < end; ++i)
                sum += values[i];
            return static_cast<float>(sum) / static_cast<float>(end - start);
        };

        const size_t capRows = std::max<size_t>(1, rowWidths.size() / 8);
        const size_t midStart = rowWidths.size() >= 4 ? rowWidths.size() / 4 : 0;
        const size_t midEnd = rowWidths.size() >= 4 ? (rowWidths.size() * 3) / 4 : rowWidths.size();
        const float midWidthAverage = averageSpan(rowWidths, midStart, midEnd);
        const float topCapRatio = midWidthAverage > 0.0f
            ? averageSpan(rowWidths, 0, std::min(capRows, rowWidths.size())) / midWidthAverage
            : 0.0f;
        const float bottomCapRatio = midWidthAverage > 0.0f
            ? averageSpan(rowWidths, rowWidths.size() - std::min(capRows, rowWidths.size()), rowWidths.size()) /
                midWidthAverage
            : 0.0f;

        const float aspectRatio = std::max(
            static_cast<float>(extentW) / static_cast<float>(extentH),
            static_cast<float>(extentH) / static_cast<float>(extentW));
        const float rowStability = centeredStability(rowWidths);
        const float columnStability = centeredStability(columnHeights);

        const bool roundedRectPlate =
            extentRatio >= kShapePlateExtentRatio &&
            aspectRatio <= kShapePlateMaxAspectRatio &&
            rowStability >= kShapePlateStabilityRatio &&
            columnStability >= kShapePlateStabilityRatio &&
            topCapRatio >= kRoundedPlateMinCapRatio &&
            bottomCapRatio >= kRoundedPlateMinCapRatio &&
            topCapRatio <= kShapePlateMaxCapRatio &&
            bottomCapRatio <= kShapePlateMaxCapRatio;

        const bool circlePlate =
            extentRatio >= kShapePlateExtentRatio &&
            aspectRatio <= kCirclePlateMaxAspectRatio &&
            rowStability >= kShapePlateStabilityRatio &&
            columnStability >= kShapePlateStabilityRatio &&
            topCapRatio <= kCirclePlateMaxCapRatio &&
            bottomCapRatio <= kCirclePlateMaxCapRatio &&
            std::abs(topCapRatio - bottomCapRatio) <= kCirclePlateMaxCapDelta;

        const bool strongEdgeColor =
            edgeDominantRatio >= kStrongEdgeDominantRatio &&
            sectorCount >= kStrongEdgeSectorCount;
        const bool gradientPlateEdgeColor =
            roundedRectPlate &&
            topCapRatio >= kGradientPlateMinCapRatio &&
            bottomCapRatio >= kGradientPlateMinCapRatio &&
            std::abs(topCapRatio - bottomCapRatio) <= kGradientPlateMaxCapDelta &&
            edgeDominantRatio >= kGradientEdgeDominantRatio &&
            sectorCount >= kGradientEdgeSectorCount;

        if ((!roundedRectPlate && !circlePlate) ||
            (!strongEdgeColor && !gradientPlateEdgeColor))
        {
            return false;
        }

        color = dominant;
        return true;
    }

    inline int RoundedRectMaskAlpha(int x, int y, int width, int height, float radius)
    {
        const float px = static_cast<float>(x) + 0.5f;
        const float py = static_cast<float>(y) + 0.5f;
        const float left = radius;
        const float top = radius;
        const float right = static_cast<float>(width) - radius;
        const float bottom = static_cast<float>(height) - radius;

        float dx = 0.0f;
        if (px < left) dx = left - px;
        else if (px > right) dx = px - right;

        float dy = 0.0f;
        if (py < top) dy = top - py;
        else if (py > bottom) dy = py - bottom;

        // Superellipse corner: the larger radius offsets the softer continuous curve.
        const float distance = std::pow(
            std::pow(dx, kIconBeautifyCornerExponent) +
                std::pow(dy, kIconBeautifyCornerExponent),
            1.0f / kIconBeautifyCornerExponent);
        const float coverage = std::clamp(radius + 0.5f - distance, 0.0f, 1.0f);
        return static_cast<int>(std::round(coverage * 255.0f));
    }

    inline std::uint32_t ScalePremultipliedPixel(std::uint32_t pixel, int scale)
    {
        if (scale <= 0 || PixelA(pixel) == 0)
            return 0;
        if (scale >= 255)
            return pixel;

        const int b = static_cast<int>(pixel & 0xff);
        const int g = static_cast<int>((pixel >> 8) & 0xff);
        const int r = static_cast<int>((pixel >> 16) & 0xff);
        const int a = static_cast<int>((pixel >> 24) & 0xff);
        return PackBgra(
            (b * scale + 127) / 255,
            (g * scale + 127) / 255,
            (r * scale + 127) / 255,
            (a * scale + 127) / 255);
    }

    inline std::uint32_t SourceOverPremultiplied(std::uint32_t src, std::uint32_t dst)
    {
        const int sa = PixelA(src);
        if (sa == 0) return dst;
        if (sa == 255) return src;

        const int inv = 255 - sa;
        const int sb = static_cast<int>(src & 0xff);
        const int sg = static_cast<int>((src >> 8) & 0xff);
        const int sr = static_cast<int>((src >> 16) & 0xff);
        const int db = static_cast<int>(dst & 0xff);
        const int dg = static_cast<int>((dst >> 8) & 0xff);
        const int dr = static_cast<int>((dst >> 16) & 0xff);
        const int da = PixelA(dst);

        return PackBgra(
            sb + (db * inv + 127) / 255,
            sg + (dg * inv + 127) / 255,
            sr + (dr * inv + 127) / 255,
            sa + (da * inv + 127) / 255);
    }

    inline std::uint32_t SampleBgraBilinear(const std::vector<std::uint32_t>& pixels,
        int width, int height, float x, float y)
    {
        x = std::clamp(x, 0.0f, static_cast<float>(std::max(0, width - 1)));
        y = std::clamp(y, 0.0f, static_cast<float>(std::max(0, height - 1)));

        const int x0 = static_cast<int>(std::floor(x));
        const int y0 = static_cast<int>(std::floor(y));
        const int x1 = std::min(width - 1, x0 + 1);
        const int y1 = std::min(height - 1, y0 + 1);
        const float fx = x - static_cast<float>(x0);
        const float fy = y - static_cast<float>(y0);

        const std::uint32_t p00 = pixels[static_cast<size_t>(y0) * width + x0];
        const std::uint32_t p10 = pixels[static_cast<size_t>(y0) * width + x1];
        const std::uint32_t p01 = pixels[static_cast<size_t>(y1) * width + x0];
        const std::uint32_t p11 = pixels[static_cast<size_t>(y1) * width + x1];

        auto channel = [&](int shift) {
            const float c00 = static_cast<float>((p00 >> shift) & 0xff);
            const float c10 = static_cast<float>((p10 >> shift) & 0xff);
            const float c01 = static_cast<float>((p01 >> shift) & 0xff);
            const float c11 = static_cast<float>((p11 >> shift) & 0xff);
            const float top = c00 + (c10 - c00) * fx;
            const float bottom = c01 + (c11 - c01) * fx;
            return static_cast<int>(std::round(top + (bottom - top) * fy));
        };

        return PackBgra(channel(0), channel(8), channel(16), channel(24));
    }

    struct IconBackgroundPaint
    {
        IconBackgroundColor start{};
        IconBackgroundColor end{};
        IconBackgroundColor border{};
        int opacity = 255;
        bool gradient = false;
        int gradientDirection = 0;
    };

    inline int IconColorLuma(const IconBackgroundColor& color)
    {
        return (color.r * 299 + color.g * 587 + color.b * 114) / 1000;
    }

    inline IconBackgroundColor MixIconColor(
        const IconBackgroundColor& start,
        const IconBackgroundColor& end,
        float amount)
    {
        amount = std::clamp(amount, 0.0f, 1.0f);
        return IconBackgroundColor{
            std::clamp(static_cast<int>(std::round(
                static_cast<float>(start.r) + static_cast<float>(end.r - start.r) * amount)), 0, 255),
            std::clamp(static_cast<int>(std::round(
                static_cast<float>(start.g) + static_cast<float>(end.g - start.g) * amount)), 0, 255),
            std::clamp(static_cast<int>(std::round(
                static_cast<float>(start.b) + static_cast<float>(end.b - start.b) * amount)), 0, 255)
        };
    }

    inline IconBackgroundColor AutoIconBorderColor(
        const IconBackgroundColor& start,
        const IconBackgroundColor& end)
    {
        const IconBackgroundColor mid = MixIconColor(start, end, 0.5f);
        const int delta = IconColorLuma(mid) >= 128 ? -34 : 34;
        return IconBackgroundColor{
            std::clamp(mid.r + delta, 0, 255),
            std::clamp(mid.g + delta, 0, 255),
            std::clamp(mid.b + delta, 0, 255)
        };
    }

    inline IconBackgroundColor IconColorFromFloats(float r, float g, float b)
    {
        return IconBackgroundColor{
            std::clamp(static_cast<int>(std::round(std::clamp(r, 0.0f, 1.0f) * 255.0f)), 0, 255),
            std::clamp(static_cast<int>(std::round(std::clamp(g, 0.0f, 1.0f) * 255.0f)), 0, 255),
            std::clamp(static_cast<int>(std::round(std::clamp(b, 0.0f, 1.0f) * 255.0f)), 0, 255)
        };
    }

    inline void FillRoundedIconBackground(std::vector<std::uint32_t>& output,
        int width,
        int height,
        float radius,
        const IconBackgroundPaint& paint)
    {
        for (int y = 0; y < height; ++y)
        {
            const float yT = height > 1
                ? static_cast<float>(y) / static_cast<float>(height - 1)
                : 0.0f;
            for (int x = 0; x < width; ++x)
            {
                const float xT = width > 1
                    ? static_cast<float>(x) / static_cast<float>(width - 1)
                    : 0.0f;
                float gradientT = 0.0f;
                if (paint.gradient)
                {
                    switch (paint.gradientDirection)
                    {
                    case 1: gradientT = xT; break;
                    case 2: gradientT = (xT + yT) * 0.5f; break;
                    case 3: gradientT = (xT + (1.0f - yT)) * 0.5f; break;
                    default: gradientT = yT; break;
                    }
                }
                const IconBackgroundColor fill = MixIconColor(paint.start, paint.end, gradientT);
                const int mask = RoundedRectMaskAlpha(x, y, width, height, radius);
                const int innerWidth = std::max(1, width - 2);
                const int innerHeight = std::max(1, height - 2);
                const int innerMask = RoundedRectMaskAlpha(
                    x - 1, y - 1, innerWidth, innerHeight, std::max(1.0f, radius - 1.0f));
                const float borderMix = mask > 0
                    ? static_cast<float>(std::clamp(mask - innerMask, 0, 255)) / static_cast<float>(mask)
                    : 0.0f;
                const int r = static_cast<int>(std::round(
                    static_cast<float>(fill.r) + static_cast<float>(paint.border.r - fill.r) * borderMix));
                const int g = static_cast<int>(std::round(
                    static_cast<float>(fill.g) + static_cast<float>(paint.border.g - fill.g) * borderMix));
                const int b = static_cast<int>(std::round(
                    static_cast<float>(fill.b) + static_cast<float>(paint.border.b - fill.b) * borderMix));
                const int alpha = (mask * paint.opacity + 127) / 255;
                output[static_cast<size_t>(y) * width + x] = PackPremultipliedRgb(r, g, b, alpha);
            }
        }
    }

    inline void ApplyRoundedIconOutline(std::vector<std::uint32_t>& output,
        int width,
        int height,
        float radius,
        IconBackgroundColor stroke,
        int opacity)
    {
        const int innerWidth = std::max(1, width - 2);
        const int innerHeight = std::max(1, height - 2);
        for (int y = 0; y < height; ++y)
        {
            for (int x = 0; x < width; ++x)
            {
                const int mask = RoundedRectMaskAlpha(x, y, width, height, radius);
                if (mask <= 0)
                    continue;

                const int innerMask = RoundedRectMaskAlpha(
                    x - 1, y - 1, innerWidth, innerHeight, std::max(1.0f, radius - 1.0f));
                const int edgeAlpha = std::clamp(mask - innerMask, 0, 255);
                if (edgeAlpha <= 0)
                    continue;

                const int alpha = (edgeAlpha * opacity + 127) / 255;
                std::uint32_t& dst = output[static_cast<size_t>(y) * width + x];
                dst = SourceOverPremultiplied(
                    PackPremultipliedRgb(stroke.r, stroke.g, stroke.b, alpha),
                    dst);
            }
        }
    }

    struct IconShadowPass
    {
        int dx = 0;
        int dy = 0;
        int opacity = 0;
    };

    inline std::uint32_t MakeIconSourceShadow(std::uint32_t sampled, int mask, int opacity)
    {
        if (mask <= 0 || opacity <= 0)
            return 0;

        const int alpha = PixelA(sampled);
        if (alpha <= 0)
            return 0;

        const int shadowAlpha = (alpha * mask * opacity + 255 * 255 / 2) / (255 * 255);
        return PackPremultipliedRgb(48, 58, 72, shadowAlpha);
    }

    inline std::vector<std::uint32_t> BeautifyIconPixels(
        const std::vector<std::uint32_t>& source,
        int width,
        int height,
        const IconBackgroundPaint& backgroundPaint,
        float cornerRadius,
        bool smartRecognitionEnabled)
    {
        const IconVisibleBounds bounds = AnalyzeIconVisibleBounds(source, width, height);
        if (!bounds.hasVisiblePixels)
            return source;

        const float radius = cornerRadius;
        IconBackgroundColor edgeFill{};
        const bool clipWithEdgeFill = smartRecognitionEnabled &&
            DetectSolidEdgeBackground(source, width, height, edgeFill);
        std::vector<std::uint32_t> output(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

        if (clipWithEdgeFill)
        {
            const std::uint32_t background = PackPremultipliedRgb(edgeFill.r, edgeFill.g, edgeFill.b, 255);
            for (int y = 0; y < height; ++y)
            {
                for (int x = 0; x < width; ++x)
                {
                    const int mask = RoundedRectMaskAlpha(x, y, width, height, radius);
                    const std::uint32_t composed = SourceOverPremultiplied(
                        source[static_cast<size_t>(y) * width + x], background);
                    output[static_cast<size_t>(y) * width + x] =
                        ScalePremultipliedPixel(composed, mask);
                }
            }
            if (IconColorLuma(edgeFill) >= 232)
            {
                ApplyRoundedIconOutline(output, width, height, radius,
                    IconBackgroundColor{ 190, 199, 214 }, 150);
            }
            return output;
        }

        FillRoundedIconBackground(output, width, height, radius, backgroundPaint);

        const int sourceW = std::max(1, bounds.right - bounds.left + 1);
        const int sourceH = std::max(1, bounds.bottom - bounds.top + 1);
        const int padding = std::max(5, static_cast<int>(std::round(std::min(width, height) * 0.16f)));
        const int maxW = std::max(1, width - padding * 2);
        const int maxH = std::max(1, height - padding * 2);
        const float scale = std::min(
            static_cast<float>(maxW) / static_cast<float>(sourceW),
            static_cast<float>(maxH) / static_cast<float>(sourceH));
        const int destW = std::max(1, static_cast<int>(std::round(sourceW * scale)));
        const int destH = std::max(1, static_cast<int>(std::round(sourceH * scale)));
        const int destLeft = (width - destW) / 2;
        const int destTop = (height - destH) / 2;

        constexpr IconShadowPass kShadowPasses[] = {
            { 0, 1, 42 },
            { -1, 1, 22 },
            { 1, 1, 22 },
            { 0, 2, 18 },
            { -1, 0, 14 },
            { 1, 0, 14 },
            { 0, -1, 10 },
        };

        for (int y = 0; y < destH; ++y)
        {
            for (int x = 0; x < destW; ++x)
            {
                const float sx = static_cast<float>(bounds.left) +
                    ((static_cast<float>(x) + 0.5f) / static_cast<float>(destW)) *
                    static_cast<float>(sourceW) - 0.5f;
                const float sy = static_cast<float>(bounds.top) +
                    ((static_cast<float>(y) + 0.5f) / static_cast<float>(destH)) *
                    static_cast<float>(sourceH) - 0.5f;
                const std::uint32_t sampled = SampleBgraBilinear(source, width, height, sx, sy);
                if (PixelA(sampled) == 0)
                    continue;

                const int outX = destLeft + x;
                const int outY = destTop + y;
                for (const IconShadowPass& pass : kShadowPasses)
                {
                    const int shadowX = outX + pass.dx;
                    const int shadowY = outY + pass.dy;
                    if (shadowX < 0 || shadowY < 0 || shadowX >= width || shadowY >= height)
                        continue;

                    const int mask = RoundedRectMaskAlpha(shadowX, shadowY, width, height, radius);
                    std::uint32_t shadow = MakeIconSourceShadow(sampled, mask, pass.opacity);
                    std::uint32_t& dst = output[static_cast<size_t>(shadowY) * width + shadowX];
                    dst = SourceOverPremultiplied(shadow, dst);
                }
            }
        }

        for (int y = 0; y < destH; ++y)
        {
            for (int x = 0; x < destW; ++x)
            {
                const float sx = static_cast<float>(bounds.left) +
                    ((static_cast<float>(x) + 0.5f) / static_cast<float>(destW)) *
                    static_cast<float>(sourceW) - 0.5f;
                const float sy = static_cast<float>(bounds.top) +
                    ((static_cast<float>(y) + 0.5f) / static_cast<float>(destH)) *
                    static_cast<float>(sourceH) - 0.5f;
                std::uint32_t sampled = SampleBgraBilinear(source, width, height, sx, sy);

                const int outX = destLeft + x;
                const int outY = destTop + y;
                if (outX < 0 || outY < 0 || outX >= width || outY >= height)
                    continue;

                const int mask = RoundedRectMaskAlpha(outX, outY, width, height, radius);
                sampled = ScalePremultipliedPixel(sampled, mask);
                std::uint32_t& dst = output[static_cast<size_t>(outY) * width + outX];
                dst = SourceOverPremultiplied(sampled, dst);
            }
        }

        return output;
    }
}

inline std::uintptr_t DesktopApp::GetD2DIconCacheKey(HBITMAP hbm, bool beautified) const
{
    std::uintptr_t key = reinterpret_cast<std::uintptr_t>(hbm);
    if (!beautified)
        return key;

    if constexpr (sizeof(std::uintptr_t) >= 8)
        return key ^ static_cast<std::uintptr_t>(0x9e3779b97f4a7c15ull);
    else
        return key ^ static_cast<std::uintptr_t>(0x9e3779b9u);
}

inline void DesktopApp::EraseD2DIconCacheForBitmap(HBITMAP hbm)
{
    if (!hbm) return;
    d2dIconCache_.erase(GetD2DIconCacheKey(hbm, false));
    d2dIconCache_.erase(GetD2DIconCacheKey(hbm, true));
}

inline ComPtr<ID2D1Bitmap1> DesktopApp::CreateD2DBitmapFromHBitmap(
    HBITMAP hbm, bool beautify)
{
    if (!hbm || !d2dContext_)
        return nullptr;

    IconPixelBuffer buffer;
    if (!ReadHBitmapPixels(hbm, buffer))
        return nullptr;

    if (beautify)
    {
        IconBackgroundPaint backgroundPaint{};
        backgroundPaint.start = IconColorFromFloats(
            iconBeautifyBgStartR_, iconBeautifyBgStartG_, iconBeautifyBgStartB_);
        backgroundPaint.end = IconColorFromFloats(
            iconBeautifyBgEndR_, iconBeautifyBgEndG_, iconBeautifyBgEndB_);
        backgroundPaint.border = AutoIconBorderColor(backgroundPaint.start, backgroundPaint.end);
        backgroundPaint.opacity = std::clamp(
            static_cast<int>(std::round(iconBeautifyBgOpacity_ * 255.0f)), 0, 255);
        backgroundPaint.gradient = iconBeautifyGradientEnabled_;
        backgroundPaint.gradientDirection = iconBeautifyGradientDirection_;
        if (!backgroundPaint.gradient)
            backgroundPaint.end = backgroundPaint.start;
        buffer.pixels = BeautifyIconPixels(
            buffer.pixels, buffer.width, buffer.height, backgroundPaint,
            GetBeautifiedIconCornerRadius(buffer.width, buffer.height),
            iconBeautifyMode_ == 0);
    }

    D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_NONE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));

    ComPtr<ID2D1Bitmap1> bitmap;
    if (FAILED(d2dContext_->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(buffer.width), static_cast<UINT32>(buffer.height)),
            buffer.pixels.data(),
            static_cast<UINT32>(buffer.width * sizeof(std::uint32_t)),
            &props,
            &bitmap)))
    {
        return nullptr;
    }

    return bitmap;
}

/**
 * @brief 获取或创建 HBITMAP 对应的 Direct2D 位图（带缓存）。
 * @param hbm HBITMAP 句柄。
 * @return ID2D1Bitmap1 指针，失败返回 nullptr。
 */
inline ID2D1Bitmap1* DesktopApp::GetOrCreateD2DBitmap(HBITMAP hbm)
{
    return GetOrCreateD2DBitmap(hbm, iconBeautifyEnabled_);
}

inline ID2D1Bitmap1* DesktopApp::GetOrCreateD2DBitmap(HBITMAP hbm, bool beautify)
{
    if (!hbm) return nullptr;
    const auto key = GetD2DIconCacheKey(hbm, beautify);
    auto it = d2dIconCache_.find(key);
    if (it != d2dIconCache_.end()) return it->second.Get();

    ComPtr<ID2D1Bitmap1> bitmap = CreateD2DBitmapFromHBitmap(hbm, beautify);
    if (!bitmap)
        return nullptr;

    auto* result = bitmap.Get();
    d2dIconCache_[key] = std::move(bitmap);
    return result;
}

inline ID2D1Bitmap* DesktopApp::GetOrCreateD2DBitmap(ID2D1RenderTarget* target, HBITMAP hbm)
{
    if (!target || !hbm) return nullptr;

    // 快捷导航改走 DComp 后，target 必为 ID2D1DeviceContext（与桌面同源 d2dDevice_），
    // 统一走 d2dIconCache_；非 device-context 路径已废弃。
    ComPtr<ID2D1DeviceContext> deviceContext;
    if (FAILED(target->QueryInterface(IID_PPV_ARGS(&deviceContext))) || !deviceContext)
        return nullptr;
    return GetOrCreateD2DBitmap(hbm);
}
