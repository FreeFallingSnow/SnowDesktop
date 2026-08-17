#include "app.h"
#include "../widgets/collection_group_rules.h"
#include "../widgets/guide_widget_rules.h"

// Desktop page topology, grid settings and page-to-monitor mapping.

const GridPage* DesktopApp::GridPageFromPoint(POINT point) const
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
const GridPage* DesktopApp::GridPageFromScreenPoint(POINT screenPoint) const
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
void DesktopApp::AdjustGridRows(int delta)
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
    RefreshIconBitmapResolution();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 在右键菜单所在页面调整列数（增/减）。
 * @param delta 列数变化量（正数增加，负数减少）。
 */
void DesktopApp::AdjustGridColumns(int delta)
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
    RefreshIconBitmapResolution();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 将右键所在页面一次性设置为指定列数和行数。
 * @param columns 目标列数。
 * @param rows 目标行数。
 */
void DesktopApp::SetGridDimensions(int columns, int rows)
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
    RefreshIconBitmapResolution();
    InvalidateRect(hwnd_, nullptr, TRUE);
}

/**
 * @brief 根据显示器物理尺寸估算舒适的桌面网格行列数。
 *
 * 以 27 英寸为舒适密度基准，并对物理尺寸采用平方根弱缩放。
 * 因此小屏只适度减少行列，不会因对角线较小而把图标放得过大；
 * 27 英寸 16:9 显示器仍得到约 27 列 × 11 行。
 */
GridSpan DesktopApp::CalculateRecommendedGridDimensions(
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
void DesktopApp::ToggleFirstPagePin(POINT screenPoint)
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
void DesktopApp::ToggleLastPagePin(POINT screenPoint)
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
void DesktopApp::PreviewIconSpacing(float value)
{
    const float clamped = snowdesktop::layout_spacing_rules::
        ClampScale(value);
    if (clamped == iconSpacingScale_) return;
    iconSpacingScale_ = clamped;
    for (auto& page : gridPages_)
        ApplyIconSpacingToPage(page);

    // A spacing preview changes only geometry. Existing runtime items point
    // at these model records, so updating their bounds avoids rebuilding all
    // containers for every one-percent slider step.
    for (auto& item : items_)
    {
        if (item.name.empty()) continue;
        item.bounds = GetGridRect(
            gridPages_, item.gridCell, item.gridSpan);
    }
    for (auto& widget : widgets_)
    {
        if (IsGroupedWidget(widget)) continue;
        const GridPage* page = FindGridPage(
            gridPages_, widget.gridCell.pageId);
        if (page)
            widget.cellScale = GetGridPageCuScale(*page);
        widget.bounds = GetGridRect(
            gridPages_, widget.gridCell, widget.gridSpan);
    }
    iconSpacingPreviewActive_ = true;
    InvalidateDragStaticScene();
    if (hwnd_)
    {
        InvalidateRect(hwnd_, nullptr, FALSE);
        // Settings-slider mouse moves can continuously occupy the queue, and
        // WM_PAINT is generated only after higher-priority input drains. Paint
        // and submit this preview synchronously so the retained DComp surface
        // cannot alternate between the committed value and a delayed preview.
        PresentDesktopPointerUpdate();
        FlushPendingCompositionCommit();
    }
}

void DesktopApp::SetIconSpacing(float value)
{
    const float clamped = snowdesktop::layout_spacing_rules::
        ClampScale(value);
    const bool changed = clamped != iconSpacingScale_;
    if (!changed && !iconSpacingPreviewActive_) return;
    iconSpacingScale_ = clamped;
    for (auto& page : gridPages_)
        ApplyIconSpacingToPage(page);
    ApplyDockWorkAreaReservation();
    LayoutItems();
    RefreshIconBitmapResolution();
    SaveLayoutSlots();
    iconSpacingPreviewActive_ = false;
    InvalidateRect(hwnd_, nullptr, FALSE);
}

/**
 * @brief 以增量方式调整图标间距比例。
 * @param delta 间距变化量。
 */
void DesktopApp::AdjustIconSpacing(float delta)
{
    const float newValue = snowdesktop::layout_spacing_rules::
        ClampScale(iconSpacingScale_ + delta);
    SetIconSpacing(newValue);
}

/**
 * @brief 设置图标标题字号，重新创建文本格式并刷新。
 * @param value 新的字号。
 */
void DesktopApp::SetItemFontSize(float valueCu)
{
    valueCu = std::clamp(valueCu,
        kMinimumItemFontSizeCu, kMaximumItemFontSizeCu);
    if (valueCu == itemFontSizeCu_) return;
    itemFontSizeCu_ = valueCu;
    RecreateItemTextFormat();

    // Dock icon geometry is derived from the grid icon size, which in turn
    // reserves space for the current two-line title height. Rebuild both the
    // work-area reservation and cached Dock slots before repainting; otherwise
    // the Dock keeps drawing its previous-size slots until another interaction.
    ApplyDockWorkAreaReservation();
    LayoutItems();
    RefreshIconBitmapResolution();
    InvalidateDockContainers();
    InvalidateDragStaticScene();
    SaveLayoutSlots();
    if (floatingDockVisible_)
        UpdateFloatingDockWindowBounds();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateDockRects(TRUE);
}

void DesktopApp::SetListItemFontSize(float valueCu)
{
    valueCu = std::clamp(valueCu,
        kMinimumItemFontSizeCu, kMaximumItemFontSizeCu);
    if (valueCu == listItemFontSizeCu_) return;
    listItemFontSizeCu_ = valueCu;
    RecreateComponentListTextFormat();
    for (auto& container : containers_)
    {
        if (auto* widget =
                dynamic_cast<WidgetContainer*>(container.get()))
            widget->InvalidateSlots();
    }
    InvalidateDragStaticScene();
    SaveLayoutSlots();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateFloatingDockWindow(false);
}

DWRITE_FONT_WEIGHT DesktopApp::GetItemFontWeight() const
{
    return itemFontWeight_;
}

void DesktopApp::SetItemFontWeight(DWRITE_FONT_WEIGHT weight)
{
    if (weight == itemFontWeight_) return;
    itemFontWeight_ = weight;
    RecreateItemTextFormat();
    RecreateComponentListTextFormat();
    InvalidateDragStaticScene();
    SaveLayoutSlots();
    if (hwnd_)
        InvalidateRect(hwnd_, nullptr, TRUE);
    InvalidateDockRects(TRUE);
}

void DesktopApp::SetShortcutArrowMode(int mode)
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

bool DesktopApp::ShouldDrawShortcutArrow(bool isShortcut, bool isApplicationShortcut) const
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

void DesktopApp::SetIconBeautifyEnabled(bool enabled)
{
    auto settings = iconBeautifySettings_;
    settings.enabled = enabled;
    SetIconBeautifySettings(settings);
}

void DesktopApp::SetIconBeautifySettings(
    const snowdesktop::IconBeautifySettings& rawSettings,
    snowdesktop::IconBeautifyUpdateKind updateKind)
{
    const auto settings = snowdesktop::icon_beautify::Normalize(rawSettings);
    if (snowdesktop::icon_beautify::Equal(settings, iconBeautifySettings_))
    {
        if (updateKind == snowdesktop::IconBeautifyUpdateKind::Commit)
            SaveLayoutSlots();
        return;
    }

    iconBeautifySettings_ = settings;

    d2dIconCache_.clear();
    ResetDemoIconLoader();
    placeholderIconCache_.clear();
    quickNavSysIconCache_.clear();
    quickNavAppIconCache_.clear();
    privacyFileIconBitmap_.Reset();
    privacyFolderIconBitmap_.Reset();
    InvalidateDragStaticScene();
    if (updateKind == snowdesktop::IconBeautifyUpdateKind::Commit)
        SaveLayoutSlots();
    InvalidateRect(hwnd_, nullptr, TRUE);
    if (quickNavigationOpen_)
        InvalidateQuickNavigationWindow();
}

/**
 * @brief 获取系统主显示器在 gridPages_ 中的索引（回退到 0）。
 * @return 页面索引。
 */
size_t DesktopApp::FirstMonitorOrderIndex() const
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
std::vector<size_t> DesktopApp::BuildMonitorRenderOrder() const
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

const GridPage* DesktopApp::GetFirstPageGridPage() const
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
bool DesktopApp::PageHasContent(const std::wstring& pageId) const
{
    if (pageId.empty() || pageId == kDockPageId) return false;
    for (const auto& item : items_)
        if (!item.name.empty() && item.gridCell.pageId == pageId) return true;
    for (const auto& w : widgets_)
        if (!IsGroupedWidget(w) &&
            w.gridCell.pageId == pageId) return true;
    return false;
}

bool DesktopApp::RemoveRedundantGuideWidgets()
{
    std::unordered_set<std::wstring> pagesWithVisibleItems;
    std::unordered_set<std::wstring> pagesWithOtherWidgets;

    for (const auto& item : items_)
    {
        if (!item.name.empty() &&
            !item.gridCell.pageId.empty() &&
            item.gridCell.pageId != kDockPageId &&
            !IsItemInAnyWidget(item))
        {
            pagesWithVisibleItems.insert(item.gridCell.pageId);
        }
    }
    for (const auto& widget : widgets_)
    {
        if (widget.type != DesktopWidgetType::Guide &&
            !IsGroupedWidget(widget) &&
            !widget.gridCell.pageId.empty() &&
            widget.gridCell.pageId != kDockPageId)
        {
            pagesWithOtherWidgets.insert(widget.gridCell.pageId);
        }
    }

    const size_t previousCount = widgets_.size();
    std::erase_if(widgets_, [&](const DesktopWidget& widget) {
        if (widget.type != DesktopWidgetType::Guide)
            return false;
        return snowdesktop::guide_widget_rules::ShouldRemove(
            pagesWithVisibleItems.contains(widget.gridCell.pageId),
            pagesWithOtherWidgets.contains(widget.gridCell.pageId));
    });
    return widgets_.size() != previousCount;
}

/**
 * @brief 从当前偏移位置沿指定方向查找下一个非空页面的偏移量。
 * @param fromOffset 起始偏移量。
 * @param direction 方向（1 向前 / -1 向后）。
 * @return 找到的偏移量，未找到则返回原偏移量。
 */
int DesktopApp::NextNonEmptyOffset(int fromOffset, int direction) const
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
int DesktopApp::MaxPageOffset() const
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

std::wstring DesktopApp::GetPageDisplayName(int index) const
{
    return _LFW("app.grid.page_label", std::to_wstring(index + 1));
}

void DesktopApp::NavigatePageOffset(int delta)
{
    if (delta < 0 && pageOffset_ <= 0) return;
    if (delta > 0 && pageOffset_ >= MaxPageOffset()) return;
    pageOffset_ = NextNonEmptyOffset(pageOffset_, delta);
    ApplyPageMapping();
    LayoutItems();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    RestoreDesktopWindowLayer();
}

void DesktopApp::JumpToPageOffset(int targetOffset)
{
    pageOffset_ = std::clamp(targetOffset, 0, MaxPageOffset());
    ApplyPageMapping();
    LayoutItems();
    if (hwnd_) InvalidateRect(hwnd_, nullptr, TRUE);
    RestoreDesktopWindowLayer();
}

void DesktopApp::AddNewPage()
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

void DesktopApp::PlaceGuideWidgetOnPage(const std::wstring& pageId)
{
    extern inline const GridPage* FindGridPage(const std::vector<GridPage>& pages, const std::wstring& pageId);

    DesktopWidget w;
    w.id = MakeNewWidgetId();
    w.type = DesktopWidgetType::Guide;
    w.title = _LW("app.guide.title");
    w.showTitle = false;
    w.bottomBarHover = true;

    const auto* page = FindGridPage(gridPages_, pageId);
    int cols = page ? page->columns : (savedPageColumns_.count(pageId) ? savedPageColumns_[pageId] : 4);
    int rows = page ? page->rows : (savedPageRows_.count(pageId) ? savedPageRows_[pageId] : 4);
    w.gridSpan = {
        std::clamp(4, 1, std::max(1, cols)),
        std::clamp(2, 1, std::max(1, rows)),
    };

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
    {
        GridCell centered{
            pageId,
            std::max(0, (cols - w.gridSpan.columns) / 2),
            std::max(0, (rows - w.gridSpan.rows) / 2),
        };
        if (!AreGridSlotsMarked(used, centered, w.gridSpan))
        {
            w.gridCell = centered;
            goto placed;
        }
    }
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
std::wstring DesktopApp::GeneratePageId() const
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
void DesktopApp::NormalizePageIds()
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
void DesktopApp::PruneEmptyOverflowPages()
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
void DesktopApp::PadPagesToMonitorCount()
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
void DesktopApp::CompactPageIds()
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
void DesktopApp::MapPagesToMonitors()
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
void DesktopApp::ApplyPageMapping()
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

    // The Dock reservation depends on the active page's grid dimensions.
    // Page navigation changes the mapped page without rebuilding monitor
    // geometry, so refresh the reservation before LayoutItems() rebuilds the
    // Dock containers from dockAreas_.
    ApplyDockWorkAreaReservation();
    RefreshIconBitmapResolution();
}

/**
 * @brief 在 usedSlots 集合中标记一个网格区域的所有格子被占用。
 * @param usedSlots 已占用格子集合。
 * @param cell 起始单元格。
 * @param span 跨度。
 */
