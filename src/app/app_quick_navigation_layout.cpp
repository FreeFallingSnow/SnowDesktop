#include "app.h"
#include "quick_navigation_helpers.h"
#include "quick_navigation_rules.h"

// Quick-navigation geometry, tabs and content hit testing.

RECT DesktopApp::GetQuickNavigationRect() const
{
    RECT work{};
    bool foundWorkArea = false;
    POINT anchor = quickNavigationOpenPoint_;
    for (const auto& page : gridPages_)
    {
        if (PtInRect(&page.bounds, anchor) || PtInRect(&page.workArea, anchor))
        {
            work = page.workArea;
            foundWorkArea = true;
            break;
        }
    }

    if (!foundWorkArea)
    {
        POINT screenAnchor{ anchor.x + virtualLeft_, anchor.y + virtualTop_ };
        HMONITOR monitor = MonitorFromPoint(screenAnchor, MONITOR_DEFAULTTONEAREST);
        MONITORINFO monitorInfo{};
        monitorInfo.cbSize = sizeof(monitorInfo);
        if (monitor && GetMonitorInfoW(monitor, &monitorInfo))
        {
            work = MakeRect(
                monitorInfo.rcWork.left - virtualLeft_,
                monitorInfo.rcWork.top - virtualTop_,
                monitorInfo.rcWork.right - virtualLeft_,
                monitorInfo.rcWork.bottom - virtualTop_);
            foundWorkArea = true;
        }
    }

    if (!foundWorkArea)
        work = layoutWorkArea_;
    if (IsRectEmptyRect(work))
        work = MakeRect(0, 0, virtualWidth_, virtualHeight_);

    const int workWidth = std::max(1, static_cast<int>(work.right - work.left));
    const int workHeight = std::max(1, static_cast<int>(work.bottom - work.top));
    const int widthLimit = std::max(QuickNavScale(320), workWidth - QuickNavScale(48));
    const int heightLimit = std::max(QuickNavScale(280), workHeight - QuickNavScale(48));
    const int width = std::min(widthLimit, std::max(QuickNavScale(520), std::min(QuickNavScale(860), workWidth - QuickNavScale(120))));
    const int contentTop = QuickNavScale(100);
    const int cellH = QuickNavScale(kQuickNavigationCellHeight);
    const int rowGap = QuickNavScale(kQuickNavigationItemRowGap);
    const int contentPad = QuickNavScale(12);
    const int idealH = contentTop + QuickNavigationRowsHeight(1, cellH, rowGap) + contentPad;
    const int availH = workHeight - QuickNavScale(120);
    int rows = std::clamp(
        (availH - contentTop - contentPad + rowGap) / (cellH + rowGap),
        1, 6);
    const int height = std::min(heightLimit,
        std::max(idealH, contentTop + QuickNavigationRowsHeight(rows, cellH, rowGap) + contentPad));
    const int left = work.left + (workWidth - width) / 2;
    const int top = work.top + (workHeight - height) / 2;
    return MakeRect(left, top, left + width, top + height);
}

RECT DesktopApp::GetQuickNavigationSearchRect(const RECT& overlay) const
{
    return MakeRect(overlay.left + QuickNavScale(16), overlay.top + QuickNavScale(18),
        overlay.right - QuickNavScale(16), overlay.top + QuickNavScale(50));
}

RECT DesktopApp::GetQuickNavigationTabsRect(const RECT& overlay) const
{
    return MakeRect(overlay.left + QuickNavScale(16), overlay.top + QuickNavScale(58),
        overlay.right - QuickNavScale(16), overlay.top + QuickNavScale(88));
}

RECT DesktopApp::GetQuickNavigationViewModeButtonRect(
    const RECT& overlay) const
{
    if (!GetQuickNavigationEffectiveSearchText().empty() ||
        quickNavigationActiveWidgetIndex_ !=
            static_cast<size_t>(-1))
        return MakeRect(0, 0, 0, 0);

    const RECT tabs =
        GetQuickNavigationTabsRect(overlay);
    const int buttonSize = QuickNavScale(28);
    const int top = tabs.top +
        std::max(0,
            static_cast<int>(
                tabs.bottom - tabs.top -
                buttonSize)) / 2;
    return MakeRect(
        tabs.left, top,
        tabs.left + buttonSize,
        top + buttonSize);
}

int DesktopApp::GetQuickNavigationTabsStart(
    const RECT& overlay) const
{
    const RECT tabs =
        GetQuickNavigationTabsRect(overlay);
    const RECT button =
        GetQuickNavigationViewModeButtonRect(
            overlay);
    return snowdesktop::
        quick_navigation_rules::
            TabStripLabelStart(
                tabs.left,
                !IsRectEmpty(&button),
                QuickNavScale(28),
                QuickNavScale(8));
}

void DesktopApp::SetQuickNavigationDesktopViewMode(
    QuickNavigationDesktopViewMode mode)
{
    if (navigationSettings_.desktopViewMode == mode)
        return;
    navigationSettings_.desktopViewMode = mode;
    SaveNavigationSettings(
        GetNavigationSettingsPath().c_str(),
        navigationSettings_);
    if (settingsController_)
        (void)settingsController_->SynchronizeNavigation(
            navigationSettings_);
    quickNavigationScrollOffset_ = 0;
    quickNavigationInitialJumpOpen_ = false;
    ResetQuickNavigationKeyboardTarget();
    InvalidateQuickNavigationWindow();
}

bool DesktopApp::
TrySetQuickNavigationDesktopViewModeAtPoint(
    POINT point)
{
    if (!GetQuickNavigationEffectiveSearchText().empty() ||
        quickNavigationActiveWidgetIndex_ !=
            static_cast<size_t>(-1))
        return false;

    const RECT button =
        GetQuickNavigationViewModeButtonRect(
            quickNavigationRect_);
    if (PtInRect(&button, point))
    {
        SetQuickNavigationDesktopViewMode(
            snowdesktop::
                quick_navigation_rules::
                    NextQuickNavigationDesktopViewMode(
                        navigationSettings_.
                            desktopViewMode));
        return true;
    }
    return false;
}

RECT DesktopApp::GetQuickNavigationContentRect(const RECT& overlay) const
{
    if (!GetQuickNavigationEffectiveSearchText().empty())
        return MakeRect(overlay.left + QuickNavScale(12), overlay.top + QuickNavScale(66),
            overlay.right - QuickNavScale(12), overlay.bottom - QuickNavScale(12));
    return MakeRect(overlay.left + QuickNavScale(12), overlay.top + QuickNavScale(100),
        overlay.right - QuickNavScale(12), overlay.bottom - QuickNavScale(12));
}

int DesktopApp::GetQuickNavigationTabStripContentWidth(const RECT& /*overlay*/) const
{
    const size_t n = quickNavTabWidths_.size();
    if (n <= 2) return 0;
    const int gap = QuickNavScale(8);
    int total = 0;
    for (size_t i = 2; i < n; ++i)
        total += quickNavTabWidths_[i] + gap;
    return total - gap; // remove trailing gap
}

int DesktopApp::GetQuickNavigationMaxTabScrollOffset(const RECT& overlay) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    const int tabsStart =
        GetQuickNavigationTabsStart(overlay);
    const auto& tw = quickNavTabWidths_;
    const int gap = QuickNavScale(8);
    const int sepGap = QuickNavScale(6);
    // 可滚动区起始于固定标签（0:桌面, 1:映射）+ 分隔线之后。
    const int fixedWidth = (tw.size() >= 2) ? (tw[0] + gap + tw[1]) : 0;
    const int scrollPad = sepGap + QuickNavScale(1) + gap;
    const int scrollLeft =
        tabsStart + fixedWidth + scrollPad;
    return snowdesktop::
        quick_navigation_rules::
            TabStripMaxScrollOffset(
                GetQuickNavigationTabStripContentWidth(
                    overlay),
                scrollLeft,
                tabs.right);
}

int DesktopApp::GetQuickNavigationTabWidth() const
{
    if (!quickNavTabWidths_.empty())
        return quickNavTabWidths_[0];
    return QuickNavScale(72);
}

void DesktopApp::EnsureNavTabOrder()
{
    auto isTabWidget = [](DesktopWidgetType t) {
        return t == DesktopWidgetType::Collection ||
               t == DesktopWidgetType::FileCategories ||
               t == DesktopWidgetType::FolderMapping;
    };
    std::unordered_set<std::wstring> orderSet(navTabOrder_.begin(), navTabOrder_.end());
    for (const auto& w : widgets_)
    {
        if (isTabWidget(w.type) &&
            !IsGroupedCollection(w) &&
            !orderSet.count(w.id))
        {
            navTabOrder_.push_back(w.id);
            orderSet.insert(w.id);
        }
    }
    navTabOrder_.erase(
        std::remove_if(navTabOrder_.begin(), navTabOrder_.end(),
            [this, &isTabWidget](const std::wstring& id) {
                for (const auto& w : widgets_)
                    if (isTabWidget(w.type) &&
                        !IsGroupedCollection(w) &&
                        w.id == id)
                        return false;
                return true;
            }),
        navTabOrder_.end());
}

RECT DesktopApp::GetQuickNavigationTabRect(const RECT& overlay, size_t tabIndex) const
{
    RECT tabs = GetQuickNavigationTabsRect(overlay);
    const int tabsStart =
        GetQuickNavigationTabsStart(overlay);
    const int gap = QuickNavScale(8);
    const int sepGap = QuickNavScale(6);
    const int scrollPad = sepGap + QuickNavScale(1) + gap; // separator + gap after

    const size_t n = quickNavTabWidths_.size();
    if (n == 0)
        return MakeRect(
            tabsStart, tabs.top,
            tabsStart + QuickNavScale(72),
            tabs.bottom);

    int left = tabsStart;
    int width = QuickNavScale(72);
    if (tabIndex == 0)
    {
        if (n > 0) width = quickNavTabWidths_[0];
    }
    else if (tabIndex == 1)
    {
        if (n > 1)
        {
            left = tabsStart +
                quickNavTabWidths_[0] + gap;
            width = quickNavTabWidths_[1];
        }
    }
    else if (tabIndex >= 2)
    {
        left = tabsStart;
        if (n > 1)
            left = tabsStart +
                quickNavTabWidths_[0] + gap +
                quickNavTabWidths_[1] +
                scrollPad;
        for (size_t i = 2; i < tabIndex && i < n; ++i)
            left += quickNavTabWidths_[i] + gap;
        left -= quickNavigationTabScrollOffset_;
        if (tabIndex < n) width = quickNavTabWidths_[tabIndex];
    }
    return MakeRect(left, tabs.top, left + width, tabs.bottom);
}

int DesktopApp::GetQuickNavigationColumnCount(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int contentWidth = std::max(1, static_cast<int>(content.right - content.left));
    if (contentWidth < cellW) return 1;
    int columns = contentWidth / cellW;
    if (columns <= 1) return 1;
    int gap = (contentWidth - columns * cellW) / (columns - 1);
    while (columns > 1 && gap < QuickNavScale(8))
    {
        --columns;
        gap = (contentWidth - columns * cellW) / (columns - 1);
    }
    return std::max(1, columns);
}

int DesktopApp::GetQuickNavigationGap(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int columns = GetQuickNavigationColumnCount(overlay);
    if (columns <= 0) return 0;
    const int contentWidth = std::max(1, static_cast<int>(content.right - content.left));
    int totalGaps = contentWidth - columns * cellW;
    return totalGaps / columns;
}

RECT DesktopApp::GetQuickNavigationItemRect(const RECT& overlay, size_t linearIndex) const
{
    const QuickNavigationContentModel model =
        BuildQuickNavigationContentModel();
    const std::vector<RECT> rects =
        GetQuickNavigationItemRects(
            overlay, model);
    if (linearIndex >= rects.size())
        return MakeRect(0, 0, 0, 0);
    return rects[linearIndex];
}

std::vector<RECT> DesktopApp::
GetQuickNavigationItemRects(
    const RECT& overlay,
    const QuickNavigationContentModel& model) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int cellW = QuickNavScale(kCellWidth);
    const int cellH = QuickNavScale(kQuickNavigationCellHeight);
    const int rowGap = QuickNavScale(kQuickNavigationItemRowGap);
    const int rowPitch = cellH + rowGap;
    const int columns = GetQuickNavigationColumnCount(overlay);
    const bool searching =
        !GetQuickNavigationEffectiveSearchText().empty();
    std::vector<snowdesktop::quick_navigation_rules::
        SectionLayout> sectionLayouts;
    if (!searching && model.IsSectioned())
    {
        std::vector<size_t> counts;
        counts.reserve(model.sections.size());
        for (const auto& section : model.sections)
            counts.push_back(section.entryCount);
        sectionLayouts =
            snowdesktop::quick_navigation_rules::
                BuildSectionLayouts(
                    counts, columns, cellH, rowGap,
                    QuickNavScale(28),
                    QuickNavScale(8),
                    QuickNavScale(12));
    }

    const int gap = GetQuickNavigationGap(overlay);
    const int halfPad = gap / 2;
    std::vector<RECT> result;
    result.reserve(model.entries.size());
    for (size_t linearIndex = 0;
        linearIndex < model.entries.size();
        ++linearIndex)
    {
        int col = static_cast<int>(
            linearIndex %
            static_cast<size_t>(columns));
        int row = static_cast<int>(
            linearIndex /
            static_cast<size_t>(columns));
        int itemTop = content.top;
        if (!searching && !sectionLayouts.empty())
        {
            snowdesktop::quick_navigation_rules::
                SectionItemCell cell;
            if (snowdesktop::quick_navigation_rules::
                    TryGetSectionItemCell(
                        sectionLayouts, linearIndex,
                        columns, cellH, rowGap,
                        cell))
            {
                col = cell.column;
                row = cell.row;
                itemTop = content.top +
                    cell.top - row * rowPitch;
            }
        }
        else if (searching)
        {
            itemTop +=
                QuickNavScale(28) +
                QuickNavScale(8);
        }
        result.push_back(MakeRect(
            content.left + halfPad + col * (cellW + gap),
            itemTop + row * rowPitch - quickNavigationScrollOffset_,
            content.left + halfPad + col * (cellW + gap) + cellW,
            itemTop + row * rowPitch + cellH - quickNavigationScrollOffset_));
    }
    return result;
}

DesktopApp::QuickNavigationPointerTarget
DesktopApp::HitTestQuickNavigationPointerTarget(
    POINT point) const
{
    for (auto it = quickNavigationHoverRegions_.rbegin();
        it != quickNavigationHoverRegions_.rend();
        ++it)
    {
        if (PtInRect(&it->bounds, point))
            return it->target;
    }
    return {};
}

RECT DesktopApp::GetQuickNavigationSectionHeaderRect(
    const RECT& overlay, size_t sectionIndex,
    const QuickNavigationContentModel& model) const
{
    const RECT content =
        GetQuickNavigationContentRect(overlay);
    if (sectionIndex >= model.sections.size())
        return MakeRect(0, 0, 0, 0);

    std::vector<size_t> counts;
    counts.reserve(model.sections.size());
    for (const auto& section : model.sections)
        counts.push_back(section.entryCount);
    const auto layouts =
        snowdesktop::quick_navigation_rules::
            BuildSectionLayouts(
                counts,
                GetQuickNavigationColumnCount(overlay),
                QuickNavScale(
                    kQuickNavigationCellHeight),
                QuickNavScale(
                    kQuickNavigationItemRowGap),
                QuickNavScale(28),
                QuickNavScale(8),
                QuickNavScale(12));
    if (sectionIndex >= layouts.size())
        return MakeRect(0, 0, 0, 0);

    const int top = content.top +
        layouts[sectionIndex].headerTop -
        quickNavigationScrollOffset_;
    return MakeRect(
        content.left + QuickNavScale(8), top,
        content.right - QuickNavScale(12),
        top + QuickNavScale(28));
}

RECT DesktopApp::
GetQuickNavigationInitialJumpBackRect(
    const RECT& overlay) const
{
    const RECT content =
        GetQuickNavigationContentRect(overlay);
    return MakeRect(
        content.right - QuickNavScale(88),
        content.top,
        content.right - QuickNavScale(8),
        content.top + QuickNavScale(30));
}

RECT DesktopApp::
GetQuickNavigationInitialJumpCellRect(
    const RECT& overlay,
    size_t bucketIndex) const
{
    if (bucketIndex >= 27)
        return MakeRect(0, 0, 0, 0);
    const RECT content =
        GetQuickNavigationContentRect(overlay);
    const int gridTop =
        content.top + QuickNavScale(38);
    const int availableHeight =
        std::max(1,
            static_cast<int>(
                content.bottom - gridTop -
                QuickNavScale(8)));
    const int columns =
        availableHeight >= QuickNavScale(252)
        ? 4
        : 7;
    const int rows =
        (27 + columns - 1) / columns;
    const int availableWidth =
        std::max(
            1,
            static_cast<int>(
                content.right - content.left -
                QuickNavScale(32)));
    const int gridWidth = std::min(
        availableWidth,
        QuickNavScale(columns * 68));
    const int cellWidth =
        std::max(1, gridWidth / columns);
    const int cellHeight = std::clamp(
        availableHeight / rows,
        QuickNavScale(28),
        QuickNavScale(44));
    const int gridLeft = content.left +
        (static_cast<int>(
            content.right - content.left) -
            cellWidth * columns) / 2;
    const int column =
        static_cast<int>(
            bucketIndex %
            static_cast<size_t>(columns));
    const int row =
        static_cast<int>(
            bucketIndex /
            static_cast<size_t>(columns));
    return MakeRect(
        gridLeft + column * cellWidth,
        gridTop + row * cellHeight,
        gridLeft + (column + 1) * cellWidth,
        gridTop + (row + 1) * cellHeight);
}

bool DesktopApp::
ActivateQuickNavigationInitialJumpBucket(
    size_t bucketIndex)
{
    if (bucketIndex >= 27)
        return false;
    const wchar_t bucket =
        snowdesktop::quick_navigation_rules::
            InitialJumpBucketAt(bucketIndex);
    const std::wstring label(1, bucket);
    const QuickNavigationContentModel model =
        BuildQuickNavigationContentModel();
    for (size_t sectionIndex = 0;
        sectionIndex < model.sections.size();
        ++sectionIndex)
    {
        if (model.sections[sectionIndex].label !=
            label)
            continue;
        const RECT header =
            GetQuickNavigationSectionHeaderRect(
                quickNavigationRect_,
                sectionIndex, model);
        const RECT content =
            GetQuickNavigationContentRect(
                quickNavigationRect_);
        quickNavigationScrollOffset_ =
            std::clamp(
                quickNavigationScrollOffset_ +
                    static_cast<int>(
                        header.top - content.top),
                0,
                GetQuickNavigationMaxScrollOffset(
                    quickNavigationRect_));
        quickNavigationInitialJumpOpen_ = false;
        quickNavigationInitialJumpSelection_ =
            bucketIndex;
        ResetQuickNavigationKeyboardTarget();
        InvalidateQuickNavigationWindow();
        return true;
    }
    return false;
}

bool DesktopApp::
HandleQuickNavigationInitialJumpClick(
    POINT point)
{
    if (!quickNavigationInitialJumpOpen_)
        return false;
    const RECT back =
        GetQuickNavigationInitialJumpBackRect(
            quickNavigationRect_);
    if (PtInRect(&back, point))
    {
        quickNavigationInitialJumpOpen_ = false;
        InvalidateQuickNavigationWindow();
        return true;
    }
    for (size_t bucketIndex = 0;
        bucketIndex < 27; ++bucketIndex)
    {
        const RECT cell =
            GetQuickNavigationInitialJumpCellRect(
                quickNavigationRect_,
                bucketIndex);
        if (!PtInRect(&cell, point))
            continue;
        ActivateQuickNavigationInitialJumpBucket(
            bucketIndex);
        return true;
    }
    const RECT content =
        GetQuickNavigationContentRect(
            quickNavigationRect_);
    return PtInRect(&content, point) != FALSE;
}

bool DesktopApp::
HandleQuickNavigationInitialJumpKeyboardInput(
    WPARAM key)
{
    if (!quickNavigationInitialJumpOpen_)
        return false;
    if (key == VK_ESCAPE)
    {
        quickNavigationInitialJumpOpen_ = false;
        InvalidateQuickNavigationWindow();
        return true;
    }

    const QuickNavigationContentModel model =
        BuildQuickNavigationContentModel();
    std::array<bool, 27> available{};
    for (const auto& section : model.sections)
    {
        if (section.label.size() != 1)
            continue;
        available[
            snowdesktop::quick_navigation_rules::
                InitialJumpBucketIndex(
                    section.label.front())] = true;
    }
    if (key == VK_RETURN)
        return ActivateQuickNavigationInitialJumpBucket(
            quickNavigationInitialJumpSelection_);
    if (key != VK_LEFT && key != VK_RIGHT &&
        key != VK_UP && key != VK_DOWN)
        return false;

    const RECT currentRect =
        GetQuickNavigationInitialJumpCellRect(
            quickNavigationRect_,
            quickNavigationInitialJumpSelection_);
    const long long currentX =
        (static_cast<long long>(currentRect.left) +
            currentRect.right) / 2;
    const long long currentY =
        (static_cast<long long>(currentRect.top) +
            currentRect.bottom) / 2;
    long long bestScore =
        std::numeric_limits<long long>::max();
    size_t best = static_cast<size_t>(-1);
    for (size_t bucketIndex = 0;
        bucketIndex < available.size();
        ++bucketIndex)
    {
        if (!available[bucketIndex] ||
            bucketIndex ==
                quickNavigationInitialJumpSelection_)
            continue;
        const RECT candidate =
            GetQuickNavigationInitialJumpCellRect(
                quickNavigationRect_,
                bucketIndex);
        const long long x =
            (static_cast<long long>(
                candidate.left) +
                candidate.right) / 2;
        const long long y =
            (static_cast<long long>(
                candidate.top) +
                candidate.bottom) / 2;
        const long long dx = x - currentX;
        const long long dy = y - currentY;
        const bool inDirection =
            (key == VK_LEFT && dx < 0) ||
            (key == VK_RIGHT && dx > 0) ||
            (key == VK_UP && dy < 0) ||
            (key == VK_DOWN && dy > 0);
        if (!inDirection) continue;
        const long long primary =
            key == VK_LEFT || key == VK_RIGHT
            ? std::abs(dx)
            : std::abs(dy);
        const long long secondary =
            key == VK_LEFT || key == VK_RIGHT
            ? std::abs(dy)
            : std::abs(dx);
        const long long score =
            primary * 4 + secondary;
        if (score < bestScore)
        {
            bestScore = score;
            best = bucketIndex;
        }
    }
    if (best < available.size())
    {
        quickNavigationInitialJumpSelection_ =
            best;
        InvalidateQuickNavigationWindow();
    }
    return true;
}

bool DesktopApp::TryGetQuickNavigationAppEntryAtPoint(
    POINT point, const QuickNavigationAppEntry*& outEntry) const
{
    outEntry = nullptr;
    if (!quickNavigationOpen_ || GetQuickNavigationEffectiveSearchText().empty())
        return false;
    if (quickNavigationAppResultIndices_.empty())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int firstRowTop = content.top + headerH + gap
        + desktopGridH
        + gap + headerH + gap - quickNavigationScrollOffset_;
    const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();

    for (size_t i = 0; i < visibleAppCount; ++i)
    {
        const int rowTop = firstRowTop + static_cast<int>(i) * rowH;
        RECT itemRect = MakeRect(content.left + QuickNavScale(8), rowTop,
            content.right - QuickNavScale(12), rowTop + rowH);
        RECT clipped = itemRect;
        clipped.top = std::max(clipped.top, content.top);
        clipped.bottom = std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
            continue;
        size_t entryIndex = quickNavigationAppResultIndices_[i];
        if (entryIndex >= quickNavigationAppEntries_.size())
            return false;
        outEntry = &quickNavigationAppEntries_[entryIndex];
        return true;
    }

    return false;
}

size_t DesktopApp::FindDockItemIndexForQuickNavigationApp(
    const QuickNavigationAppEntry& entry)
{
    const std::wstring parsingKey =
        ToUpperInvariant(entry.parsingName);
    if (!parsingKey.empty())
    {
        const size_t direct =
            FindItemIndexByKey(parsingKey);
        if (direct < items_.size())
            return direct;
        for (size_t i = 0; i < items_.size(); ++i)
        {
            if (ToUpperInvariant(items_[i].parsingName) ==
                parsingKey)
                return i;
        }
    }

    std::wstring appUserModelId = parsingKey;
    constexpr std::wstring_view appsFolderMarker =
        L"APPSFOLDER\\";
    const size_t appsFolder =
        appUserModelId.rfind(appsFolderMarker);
    if (appsFolder != std::wstring::npos)
    {
        appUserModelId = appUserModelId.substr(
            appsFolder + appsFolderMarker.size());
    }

    std::vector<size_t> candidates;
    candidates.reserve(
        dockEntries_.size() +
        static_cast<size_t>(
            std::max(
                0,
                dockSettings_.frequentItemCount)));
    for (const DockEntry& dockEntry : dockEntries_)
    {
        if (dockEntry.type !=
            DockEntryType::DesktopItem)
            continue;
        const size_t itemIndex =
            FindItemIndexByKey(
                dockEntry.reference);
        if (itemIndex < items_.size())
            candidates.push_back(itemIndex);
    }
    for (const size_t itemIndex :
        GetFrequentDockItemIndices())
    {
        if (std::find(
                candidates.begin(),
                candidates.end(),
                itemIndex) == candidates.end())
            candidates.push_back(itemIndex);
    }

    size_t nameMatch = static_cast<size_t>(-1);
    bool nameMatchAmbiguous = false;
    for (const size_t i : candidates)
    {
        const DockAppIdentity identity =
            ResolveDockAppIdentity(i);
        if (identity.kind ==
                DockAppIdentityKind::Applications &&
            !appUserModelId.empty() &&
            identity.appUserModelId ==
                appUserModelId)
        {
            return i;
        }
        if (identity.kind != DockAppIdentityKind::None &&
            !entry.name.empty() &&
            _wcsicmp(
                items_[i].name.c_str(),
                entry.name.c_str()) == 0)
        {
            if (nameMatch !=
                static_cast<size_t>(-1))
                nameMatchAmbiguous = true;
            nameMatch = i;
        }
    }
    return nameMatchAmbiguous
        ? static_cast<size_t>(-1)
        : nameMatch;
}

bool DesktopApp::LaunchQuickNavigationAppEntry(
    const QuickNavigationAppEntry& entry)
{
    if (!entry.absolutePidl.get())
        return false;

    Pidl launchPidl;
    launchPidl.reset(
        ILClone(entry.absolutePidl.get()));
    if (!launchPidl.get())
        return false;

    const size_t dockItemIndex =
        FindDockItemIndexForQuickNavigationApp(entry);
    const bool wasClosed =
        dockItemIndex < items_.size() &&
        GetDockWindowVisualState(dockItemIndex) ==
            DockWindowVisualState::Closed;

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_IDLIST;
    sei.lpIDList = launchPidl.get();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei))
        return false;

    if (dockItemIndex < items_.size())
    {
        RecordDockItemUsage(dockItemIndex);
        if (wasClosed)
            StartDockLaunchBounce(dockItemIndex);
    }
    return true;
}

bool DesktopApp::CloseQuickNavigationThenLaunchApp(
    const QuickNavigationAppEntry& entry)
{
    if (!entry.absolutePidl.get())
        return false;

    auto pending =
        std::make_shared<QuickNavigationAppEntry>();
    pending->name = entry.name;
    pending->parsingName = entry.parsingName;
    pending->systemIconIndex = entry.systemIconIndex;
    pending->absolutePidl.reset(
        ILClone(entry.absolutePidl.get()));
    if (!pending->absolutePidl.get())
        return false;

    CloseQuickNavigationThen(
        [this, pending]() {
            LaunchQuickNavigationAppEntry(*pending);
        });
    return true;
}

size_t DesktopApp::GetQuickNavigationVisibleAppResultCount() const
{
    if (quickNavigationAppsExpanded_)
        return quickNavigationAppResultIndices_.size();
    return std::min(quickNavigationAppResultIndices_.size(), kQuickNavigationAppCollapsedResultCount);
}

bool DesktopApp::HasQuickNavigationAppExpandButton() const
{
    return !quickNavigationAppsExpanded_ &&
        quickNavigationAppResultIndices_.size() > kQuickNavigationAppCollapsedResultCount;
}

bool DesktopApp::TryExpandQuickNavigationAppsAtPoint(POINT point)
{
    if (!quickNavigationOpen_ || GetQuickNavigationEffectiveSearchText().empty() ||
        !HasQuickNavigationAppExpandButton())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int buttonTop = content.top + headerH + gap
        + desktopGridH
        + gap + headerH + gap
        + static_cast<int>(GetQuickNavigationVisibleAppResultCount()) * rowH
        - quickNavigationScrollOffset_;
    RECT buttonRect = MakeRect(content.left + QuickNavScale(8), buttonTop,
        content.right - QuickNavScale(12), buttonTop + rowH);
    RECT clipped = buttonRect;
    clipped.top = std::max(clipped.top, content.top);
    clipped.bottom = std::min(clipped.bottom, content.bottom);
    if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
        return false;

    quickNavigationAppsExpanded_ = true;
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(quickNavigationRect_));
    InvalidateQuickNavigationWindow();
    return true;
}

bool DesktopApp::HasQuickNavigationEverythingLoadMoreButton() const
{
    return everythingSearchAvailable_ &&
        quickNavigationEverythingHasMore_ &&
        !quickNavigationEverythingResults_.empty() &&
        !GetQuickNavigationEffectiveSearchText().empty();
}

bool DesktopApp::TryLoadMoreQuickNavigationEverythingResultsAtPoint(POINT point)
{
    if (!quickNavigationOpen_ || !HasQuickNavigationEverythingLoadMoreButton())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int appSectionHeight = quickNavigationAppResultIndices_.empty()
        ? 0
        : headerH + gap + static_cast<int>(visibleAppCount) * rowH +
            (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
    const int buttonTop = content.top + headerH + gap
        + desktopGridH
        + gap + appSectionHeight + headerH + gap
        + static_cast<int>(quickNavigationEverythingResults_.size()) * rowH
        - quickNavigationScrollOffset_;
    RECT buttonRect = MakeRect(content.left + QuickNavScale(8), buttonTop,
        content.right - QuickNavScale(12), buttonTop + rowH);
    RECT clipped = buttonRect;
    clipped.top = std::max(clipped.top, content.top);
    clipped.bottom = std::min(clipped.bottom, content.bottom);
    if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
        return false;

    const int oldResultCount = static_cast<int>(quickNavigationEverythingResults_.size());
    const int buttonTopBefore = buttonRect.top;
    const DWORD maxLimit = std::numeric_limits<DWORD>::max();
    if (quickNavigationEverythingResultLimit_ > maxLimit - kQuickNavigationEverythingResultBatchSize)
        quickNavigationEverythingResultLimit_ = maxLimit;
    else
        quickNavigationEverythingResultLimit_ += kQuickNavigationEverythingResultBatchSize;

    const bool appsExpanded = quickNavigationAppsExpanded_;
    RefreshQuickNavigationEverythingResults();
    quickNavigationAppsExpanded_ = appsExpanded;
    const int maxScroll = GetQuickNavigationMaxScrollOffset(quickNavigationRect_);
    if (static_cast<int>(quickNavigationEverythingResults_.size()) > oldResultCount)
    {
        const int newDesktopCount = static_cast<int>(GetQuickNavigationEntries().size());
        const int newDesktopRows = newDesktopCount == 0 ? 0 :
            (newDesktopCount + columns - 1) / columns;
        const size_t newVisibleAppCount = GetQuickNavigationVisibleAppResultCount();
        const int newDesktopGridH = QuickNavigationRowsHeight(newDesktopRows,
            QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
        const int newAppSectionHeight = quickNavigationAppResultIndices_.empty()
            ? 0
            : headerH + gap + static_cast<int>(newVisibleAppCount) * rowH +
                (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
        const int firstNewRowTop = content.top + headerH + gap
            + newDesktopGridH
            + gap + newAppSectionHeight + headerH + gap
            + oldResultCount * rowH;
        quickNavigationScrollOffset_ = std::clamp(firstNewRowTop - buttonTopBefore, 0, maxScroll);
    }
    else
    {
        quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0, maxScroll);
    }
    InvalidateQuickNavigationWindow();
    return true;
}

bool DesktopApp::TryGetQuickNavigationEverythingEntryAtPoint(
    POINT point, QuickNavigationEverythingEntry& outEntry) const
{
    if (!quickNavigationOpen_ || GetQuickNavigationEffectiveSearchText().empty())
        return false;

    RECT overlay = quickNavigationRect_;
    RECT content = GetQuickNavigationContentRect(overlay);
    if (!PtInRect(&content, point))
        return false;

    const int columns = GetQuickNavigationColumnCount(overlay);
    const int desktopCount = static_cast<int>(GetQuickNavigationEntries().size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    const int appSectionHeight = quickNavigationAppResultIndices_.empty()
        ? 0
        : headerH + gap + static_cast<int>(visibleAppCount) * rowH +
            (HasQuickNavigationAppExpandButton() ? rowH : 0) + gap;
    const int firstRowTop = content.top + headerH + gap
        + desktopGridH
        + gap + appSectionHeight + headerH + gap - quickNavigationScrollOffset_;

    for (size_t i = 0; i < quickNavigationEverythingResults_.size(); ++i)
    {
        const int rowTop = firstRowTop + static_cast<int>(i) * rowH;
        RECT itemRect = MakeRect(content.left + QuickNavScale(8), rowTop,
            content.right - QuickNavScale(12), rowTop + rowH);
        RECT clipped = itemRect;
        clipped.top = std::max(clipped.top, content.top);
        clipped.bottom = std::min(clipped.bottom, content.bottom);
        if (clipped.bottom <= clipped.top || !PtInRect(&clipped, point))
            continue;
        outEntry = quickNavigationEverythingResults_[i];
        return true;
    }

    return false;
}

std::vector<DesktopApp::QuickNavigationKeyboardTarget>
DesktopApp::GetQuickNavigationKeyboardTargets() const
{
    std::vector<QuickNavigationKeyboardTarget> targets;
    if (!quickNavigationOpen_) return targets;

    const RECT overlay = quickNavigationRect_;
    const RECT content = GetQuickNavigationContentRect(overlay);
    const std::vector<QuickNavigationEntry> entries = GetQuickNavigationEntries();
    targets.reserve(entries.size() + GetQuickNavigationVisibleAppResultCount() +
        quickNavigationEverythingResults_.size() + 2);
    for (size_t i = 0; i < entries.size(); ++i)
        targets.push_back({ QuickNavigationKeyboardTargetKind::Item,
            i, GetQuickNavigationItemRect(overlay, i) });

    if (GetQuickNavigationEffectiveSearchText().empty())
        return targets;

    const int columns = std::max(1, GetQuickNavigationColumnCount(overlay));
    const int desktopRows = entries.empty() ? 0 :
        (static_cast<int>(entries.size()) + columns - 1) / columns;
    const int headerHeight = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowHeight = QuickNavScale(46);
    const int desktopGridHeight = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight),
        QuickNavScale(kQuickNavigationItemRowGap));
    const int appHeaderTop = content.top + headerHeight + gap +
        desktopGridHeight + gap - quickNavigationScrollOffset_;
    int everythingHeaderTop = appHeaderTop;

    if (!quickNavigationAppResultIndices_.empty())
    {
        const size_t visibleAppCount = GetQuickNavigationVisibleAppResultCount();
        const int appRowsTop = appHeaderTop + headerHeight + gap;
        for (size_t i = 0; i < visibleAppCount; ++i)
        {
            const int top = appRowsTop + static_cast<int>(i) * rowHeight;
            targets.push_back({ QuickNavigationKeyboardTargetKind::App, i,
                MakeRect(content.left + QuickNavScale(8), top,
                    content.right - QuickNavScale(12), top + rowHeight) });
        }

        int appRowsHeight = static_cast<int>(visibleAppCount) * rowHeight;
        if (HasQuickNavigationAppExpandButton())
        {
            const int top = appRowsTop + appRowsHeight;
            targets.push_back({ QuickNavigationKeyboardTargetKind::ExpandApps, 0,
                MakeRect(content.left + QuickNavScale(8), top,
                    content.right - QuickNavScale(12), top + rowHeight) });
            appRowsHeight += rowHeight;
        }
        everythingHeaderTop = appRowsTop + appRowsHeight + gap;
    }

    const int everythingRowsTop = everythingHeaderTop + headerHeight + gap;
    for (size_t i = 0; i < quickNavigationEverythingResults_.size(); ++i)
    {
        const int top = everythingRowsTop + static_cast<int>(i) * rowHeight;
        targets.push_back({ QuickNavigationKeyboardTargetKind::Everything, i,
            MakeRect(content.left + QuickNavScale(8), top,
                content.right - QuickNavScale(12), top + rowHeight) });
    }
    if (HasQuickNavigationEverythingLoadMoreButton())
    {
        const int top = everythingRowsTop +
            static_cast<int>(quickNavigationEverythingResults_.size()) * rowHeight;
        targets.push_back({ QuickNavigationKeyboardTargetKind::LoadMoreEverything, 0,
            MakeRect(content.left + QuickNavScale(8), top,
                content.right - QuickNavScale(12), top + rowHeight) });
    }
    return targets;
}

bool DesktopApp::IsQuickNavigationKeyboardTarget(
    QuickNavigationKeyboardTargetKind kind, size_t index) const
{
    return quickNavigationKeyboardTargetKind_ == kind &&
        quickNavigationKeyboardTargetIndex_ == index;
}

void DesktopApp::ResetQuickNavigationKeyboardTarget()
{
    quickNavigationKeyboardTargetKind_ = QuickNavigationKeyboardTargetKind::None;
    quickNavigationKeyboardTargetIndex_ = 0;
}

void DesktopApp::EnsureQuickNavigationKeyboardTargetVisible(
    const RECT& targetRect)
{
    const RECT content = GetQuickNavigationContentRect(quickNavigationRect_);
    const int margin = QuickNavScale(4);
    if (targetRect.top < content.top + margin)
        quickNavigationScrollOffset_ -= content.top + margin - targetRect.top;
    else if (targetRect.bottom > content.bottom - margin)
        quickNavigationScrollOffset_ += targetRect.bottom - (content.bottom - margin);
    quickNavigationScrollOffset_ = std::clamp(quickNavigationScrollOffset_, 0,
        GetQuickNavigationMaxScrollOffset(quickNavigationRect_));
}

bool DesktopApp::HandleQuickNavigationKeyboardInput(WPARAM key)
{
    if (quickNavigationInitialJumpOpen_)
        return
            HandleQuickNavigationInitialJumpKeyboardInput(
                key);
    const bool directionKey = key == VK_LEFT || key == VK_RIGHT ||
        key == VK_UP || key == VK_DOWN;
    if (!quickNavigationOpen_ || (!directionKey && key != VK_RETURN))
        return false;

    std::vector<QuickNavigationKeyboardTarget> targets =
        GetQuickNavigationKeyboardTargets();
    if (targets.empty())
        return true;

    auto findCurrent = [&]() -> size_t {
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (IsQuickNavigationKeyboardTarget(targets[i].kind, targets[i].index))
                return i;
        }
        return static_cast<size_t>(-1);
    };

    size_t current = findCurrent();
    if (key == VK_RETURN)
    {
        if (current >= targets.size()) current = 0;
        quickNavigationKeyboardTargetKind_ = targets[current].kind;
        quickNavigationKeyboardTargetIndex_ = targets[current].index;
        EnsureQuickNavigationKeyboardTargetVisible(targets[current].rect);

        targets = GetQuickNavigationKeyboardTargets();
        current = findCurrent();
        if (current >= targets.size()) return true;
        const RECT rect = targets[current].rect;
        const POINT point{ (rect.left + rect.right) / 2,
            (rect.top + rect.bottom) / 2 };
        ResetQuickNavigationKeyboardTarget();
        return HandleQuickNavigationClick(point);
    }

    size_t next = current;
    if (current >= targets.size())
    {
        next = (key == VK_LEFT || key == VK_UP) ? targets.size() - 1 : 0;
    }
    else
    {
        const RECT currentRect = targets[current].rect;
        const long long currentX =
            (static_cast<long long>(currentRect.left) + currentRect.right) / 2;
        const long long currentY =
            (static_cast<long long>(currentRect.top) + currentRect.bottom) / 2;
        long long bestScore = std::numeric_limits<long long>::max();
        size_t best = static_cast<size_t>(-1);
        for (size_t i = 0; i < targets.size(); ++i)
        {
            if (i == current) continue;
            const RECT candidateRect = targets[i].rect;
            const long long candidateX =
                (static_cast<long long>(candidateRect.left) + candidateRect.right) / 2;
            const long long candidateY =
                (static_cast<long long>(candidateRect.top) + candidateRect.bottom) / 2;
            const long long dx = candidateX - currentX;
            const long long dy = candidateY - currentY;
            const bool inDirection =
                (key == VK_LEFT && dx < 0) ||
                (key == VK_RIGHT && dx > 0) ||
                (key == VK_UP && dy < 0) ||
                (key == VK_DOWN && dy > 0);
            if (!inDirection) continue;

            const long long primary = (key == VK_LEFT || key == VK_RIGHT)
                ? std::abs(dx) : std::abs(dy);
            const long long secondary = (key == VK_LEFT || key == VK_RIGHT)
                ? std::abs(dy) : std::abs(dx);
            if (primary * 2 < secondary) continue;
            const long long score = primary * 4 + secondary;
            if (score < bestScore)
            {
                bestScore = score;
                best = i;
            }
        }
        if (best < targets.size())
            next = best;
        else if ((key == VK_LEFT || key == VK_UP) && current > 0)
            next = current - 1;
        else if ((key == VK_RIGHT || key == VK_DOWN) && current + 1 < targets.size())
            next = current + 1;
    }

    quickNavigationKeyboardTargetKind_ = targets[next].kind;
    quickNavigationKeyboardTargetIndex_ = targets[next].index;
    EnsureQuickNavigationKeyboardTargetVisible(targets[next].rect);
    InvalidateQuickNavigationWindow();
    return true;
}

int DesktopApp::GetQuickNavigationContentHeight(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int columns = GetQuickNavigationColumnCount(overlay);
    const QuickNavigationContentModel model =
        BuildQuickNavigationContentModel();
    const int desktopCount =
        static_cast<int>(model.entries.size());
    const int desktopRows = desktopCount == 0 ? 0 : (desktopCount + columns - 1) / columns;
    if (GetQuickNavigationEffectiveSearchText().empty())
    {
        if (model.IsSectioned())
        {
            std::vector<size_t> counts;
            counts.reserve(model.sections.size());
            for (const auto& section :
                model.sections)
                counts.push_back(
                    section.entryCount);
            const auto layouts =
                snowdesktop::quick_navigation_rules::
                    BuildSectionLayouts(
                        counts, columns,
                        QuickNavScale(
                            kQuickNavigationCellHeight),
                        QuickNavScale(
                            kQuickNavigationItemRowGap),
                        QuickNavScale(28),
                        QuickNavScale(8),
                        QuickNavScale(12));
            return snowdesktop::
                quick_navigation_rules::
                    SectionedContentHeight(
                        layouts,
                        QuickNavScale(12));
        }
        const int rows = desktopCount == 0 ? 1 : desktopRows;
        return QuickNavigationRowsHeight(rows, QuickNavScale(kQuickNavigationCellHeight),
            QuickNavScale(kQuickNavigationItemRowGap));
    }

    const int headerH = QuickNavScale(28);
    const int gap = QuickNavScale(8);
    const int rowH = QuickNavScale(46);
    const int desktopGridH = QuickNavigationRowsHeight(desktopRows,
        QuickNavScale(kQuickNavigationCellHeight), QuickNavScale(kQuickNavigationItemRowGap));
    int height = headerH + gap + desktopGridH
        + gap;
    if (!quickNavigationAppResultIndices_.empty())
    {
        height += headerH + gap
            + static_cast<int>(GetQuickNavigationVisibleAppResultCount()) * rowH
            + (HasQuickNavigationAppExpandButton() ? rowH : 0)
            + gap;
    }
    height += headerH + gap
        + static_cast<int>(quickNavigationEverythingResults_.size()) * rowH
        + (HasQuickNavigationEverythingLoadMoreButton() ? rowH : 0)
        + QuickNavScale(8);
    return std::max(height, std::max(1, static_cast<int>(content.bottom - content.top)));
}

int DesktopApp::GetQuickNavigationMaxScrollOffset(const RECT& overlay) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    const int contentHeight = GetQuickNavigationContentHeight(overlay);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    return std::max(0, contentHeight - visibleHeight);
}

bool DesktopApp::GetQuickNavigationScrollbarGeometry(
    const RECT& overlay,
    RECT& outTrack, RECT& outThumb, int& outMaxScroll, int& outContentHeight) const
{
    RECT content = GetQuickNavigationContentRect(overlay);
    outContentHeight = GetQuickNavigationContentHeight(overlay);
    const int visibleHeight = std::max(1, static_cast<int>(content.bottom - content.top));
    if (outContentHeight <= visibleHeight)
    {
        ZeroMemory(&outTrack, sizeof(outTrack));
        ZeroMemory(&outThumb, sizeof(outThumb));
        outMaxScroll = 0;
        return false;
    }
    const int trackW = QuickNavScale(5);
    outTrack = MakeRect(content.right - trackW - QuickNavScale(2), content.top + QuickNavScale(4),
        content.right - QuickNavScale(2), content.bottom - QuickNavScale(4));
    const int trackH = std::max<LONG>(1, outTrack.bottom - outTrack.top);
    const int thumbH = std::clamp(visibleHeight * trackH / outContentHeight, QuickNavScale(20), trackH);
    outMaxScroll = std::max(1, outContentHeight - visibleHeight);
    const int thumbTop = outTrack.top + quickNavigationScrollOffset_ * (trackH - thumbH) / outMaxScroll;
    outThumb = MakeRect(outTrack.left, thumbTop, outTrack.right, thumbTop + thumbH);
    return true;
}

/**
 * @brief 注销快捷导航热键
 */
