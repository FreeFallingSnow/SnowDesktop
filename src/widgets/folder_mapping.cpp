/**
 * @file folder_mapping.cpp
 * @brief 映射文件夹组件实现
 *
 * 该组件用于在桌面网格中显示磁盘上的映射文件夹内容，
 * 支持图标模式与列表模式切换，以及打开文件夹按钮。
 * 提供文件夹条目在桌面网格中的布局、绘制、
 * 拖放排序和滚动等功能。
 */

#include "widget.h"
#include "slot.h"
#include "item.h"
#include "types.h"
#include "app.h"
#include "drop_model.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>

static RECT FolderMappingItemRect(FolderMapping* widget, size_t linearIndex);

/**
 * @brief 计算映射文件夹内容区域的矩形
 * @param widget FolderMapping 组件指针
 * @return 内容区域的 RECT，已向内缩进 4 像素（水平）和 8 像素（垂直）
 */
static RECT FolderMappingContentRect(FolderMapping* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -4, -8);
    return body;
}

RECT FolderMapping::GetContentViewportRect() const
{
    return FolderMappingContentRect(const_cast<FolderMapping*>(this));
}

void FolderMapping::ApplyMarqueeSelection(const RECT& contentRect)
{
    if (!data_)
        return;

    const int scroll = GetScrollOffset();
    for (size_t i = 0; i < data_->folderEntries.size(); ++i)
    {
        RECT itemRect = FolderMappingItemRect(this, i);
        OffsetRect(&itemRect, 0, scroll);
        data_->folderEntries[i].selected = RectsIntersect(itemRect, contentRect);
    }
}

/**
 * @brief 获取映射文件夹图标模式下每个单元格的高度
 * @param widget FolderMapping 组件指针
 * @return 单元格高度（像素），失败时返回最小单元格高度 kMinCellHeight
 */
static int FolderMappingCellHeight(FolderMapping* widget)
{
    if (!widget || !widget->GetApp() || !widget->GetApp()->GetDesktopGrid())
        return kMinCellHeight;
    DesktopWidget* data = widget->GetWidgetData();
    for (const auto& page : widget->GetApp()->GetDesktopGrid()->GetPages())
        if (data && page.id == data->gridCell.pageId)
            return page.cellHeight;
    return kMinCellHeight;
}

/**
 * @brief 计算映射文件夹图标模式下的自适应纵向间距
 * @param widget FolderMapping 组件指针
 * @return 间距像素值。根据可视高度与单元格高度的余数，
 *         在可见行之间均分以消除底部不完整行。
 *         间距过大或过小则返回 0 回退到默认紧凑布局。
 */
static int FolderMappingAdaptiveGapY(FolderMapping* widget)
{
    if (!widget) return 0;
    DesktopWidget* data = widget->GetWidgetData();
    if (!data || data->listMode) return 0;

    RECT content = FolderMappingContentRect(widget);
    int visibleHeight = content.bottom - content.top;
    if (visibleHeight <= 0) return 0;

    int cellH = FolderMappingCellHeight(widget);
    if (cellH <= 0 || visibleHeight <= cellH) return 0;

    int visibleRows = visibleHeight / cellH;
    if (visibleRows <= 1) return 0;

    int extraSpace = visibleHeight - visibleRows * cellH;
    int gapY = extraSpace / (visibleRows - 1);
    if (gapY < 0) return 0;

    int maxGap = std::max(1, static_cast<int>(cellH * kGapPercentY));
    if (gapY > maxGap) return 0;

    return gapY;
}

/**
 * @brief 计算映射文件夹所有条目占用的总内容高度
 * @param widget    FolderMapping 组件指针
 * @param itemCount 条目数量
 * @return 总内容高度（像素）。列表模式下每行 38 像素；
 *         图标模式下按列数计算行数乘以单元格高度并计入自适应间距
 */
static int FolderMappingContentHeight(FolderMapping* widget, size_t itemCount)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    if (data->listMode)
        return static_cast<int>(itemCount) * 38;
    int columns = std::max(1, data->gridSpan.columns);
    int rows = static_cast<int>((itemCount + static_cast<size_t>(columns) - 1) / static_cast<size_t>(columns));
    if (rows <= 0) return 0;
    int cellH = FolderMappingCellHeight(widget);
    int gapY = FolderMappingAdaptiveGapY(widget);
    return rows * cellH + (rows - 1) * gapY;
}

/**
 * @brief 计算映射文件夹在垂直方向上的最大滚动偏移量
 * @param widget FolderMapping 组件指针
 * @return 最大滚动偏移量（像素），确保内容底部能够滚动到可视区域底部
 */
static int FolderMappingMaxScrollOffset(FolderMapping* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    RECT content = FolderMappingContentRect(widget);
    int contentHeight = std::max<int>(1, content.bottom - content.top);
    return std::max(0, FolderMappingContentHeight(widget, data->folderEntries.size()) - contentHeight + kMinCellHeight / 2);
}

/**
 * @brief 根据线性索引计算文件夹条目的绘制矩形区域
 * @param widget      FolderMapping 组件指针
 * @param linearIndex 条目在线性列表中的序号
 * @return 条目所占的 RECT。列表模式下为单行水平条；
 *         图标模式下按网格列数计算行列位置
 */
static RECT FolderMappingItemRect(FolderMapping* widget, size_t linearIndex)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return {};
    RECT content = FolderMappingContentRect(widget);
    int scroll = std::clamp(data->scrollOffset, 0, FolderMappingMaxScrollOffset(widget));
    if (data->listMode)
    {
        RECT rect = MakeRect(content.left,
            content.top + static_cast<LONG>(linearIndex * 38) - scroll,
            content.right,
            content.top + static_cast<LONG>((linearIndex + 1) * 38) - scroll);
        InflateRect(&rect, -4, -2);
        return rect;
    }

    int columns = std::max(1, data->gridSpan.columns);
    int col = static_cast<int>(linearIndex % static_cast<size_t>(columns));
    int row = static_cast<int>(linearIndex / static_cast<size_t>(columns));
    int itemW = std::max<int>(1, (content.right - content.left) / columns);
    int cellH = FolderMappingCellHeight(widget);
    int gapY = FolderMappingAdaptiveGapY(widget);
    int rowStep = cellH + gapY;
    return MakeRect(
        content.left + col * itemW,
        content.top + row * rowStep - scroll,
        col + 1 == columns ? content.right : content.left + (col + 1) * itemW,
        content.top + row * rowStep + cellH - scroll);
}

/**
 * @brief 获取指定索引位置的槽位条目对象
 * @param idx 条目索引
 * @return 指向 Item 的指针，若索引无效则返回 nullptr
 *
 * 创建 FolderEntryIcon 作为条目图标并缓存到 slotItemCache_ 中，
 * 确保在槽位重建期间对象生命周期有效。
 */
Item* FolderMapping::GetSlotItem(size_t idx) const
{
    if (!data_ || idx >= data_->folderEntries.size()) return nullptr;
    auto icon = std::make_unique<FolderEntryIcon>(&data_->folderEntries[idx],
        const_cast<FolderMapping*>(this), app_);
    Item* result = icon.get();
    slotItemCache_.push_back(std::move(icon));
    return result;
}

/**
 * @brief 构建当前可见的槽位列表
 * @return 槽位独占指针的向量
 *
 * 遍历所有文件夹条目（及可选的末尾空槽位），计算每个条目
 * 在内容区域中的矩形位置，跳过完全不可见的条目以优化性能。
 */
std::vector<std::unique_ptr<Slot>> FolderMapping::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;

    size_t total = IncludeTrailingEmptySlot()
        ? data_->folderEntries.size() + 1
        : data_->folderEntries.size();
    for (size_t idx = 0; idx < total; ++idx)
    {
        RECT cell = FolderMappingItemRect(this, idx);
        if (IsRectEmptyRect(cell)) continue;
        auto slot = std::make_unique<Slot>(this, cell, idx);
        Item* item = GetSlotItem(idx);
        if (item) item->SetBounds(cell);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }
    return slots;
}

/**
 * @brief 获取指定索引位置的拖拽源条目对象
 * @param idx 条目索引
 * @return 指向 Item 的指针，索引无效时返回 nullptr
 *
 * 与 GetSlotItem 类似，但条目缓存于 dragSourceCache_ 中，
 * 用于拖放操作期间保持条目对象的有效性。
 */
Item* FolderMapping::GetMemberItem(size_t idx) const
{
    if (!data_ || idx >= data_->folderEntries.size()) return nullptr;
    auto icon = std::make_unique<FolderEntryIcon>(&data_->folderEntries[idx],
        const_cast<FolderMapping*>(this), app_);
    Item* result = icon.get();
    dragSourceCache_.push_back(std::move(icon));
    return result;
}

/**
 * @brief 获取当前选中的条目索引列表
 * @return 选中条目的索引向量
 */
std::vector<size_t> FolderMapping::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_) return result;
    for (size_t i = 0; i < data_->folderEntries.size(); ++i)
        if (data_->folderEntries[i].selected) result.push_back(i);
    return result;
}

/**
 * @brief 重新排序文件夹条目
 * @param indices      需要移动的条目索引列表（逆序处理以保持索引正确）
 * @param insertBefore 目标插入位置（移动完成后会调整以补偿移除的索引）
 *
 * 将指定索引的条目从原位置移除，再按顺序插入到 insertBefore 指定的位置，
 * 并同步更新 data_->itemKeys 使其与新的条目顺序保持一致。
 */
void FolderMapping::ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
{
    if (!data_) return;
    std::vector<FolderEntry> moving;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        if (*it >= data_->folderEntries.size()) continue;
        moving.push_back(std::move(data_->folderEntries[*it]));
        data_->folderEntries.erase(data_->folderEntries.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    size_t adjusted = insertBefore;
    for (auto idx : indices)
        if (idx < insertBefore) --adjusted;
    if (adjusted > data_->folderEntries.size()) adjusted = data_->folderEntries.size();
    for (auto it = moving.rbegin(); it != moving.rend(); ++it)
        data_->folderEntries.insert(data_->folderEntries.begin() + static_cast<std::ptrdiff_t>(adjusted++), std::move(*it));
    data_->itemKeys.clear();
    data_->itemKeys.reserve(data_->folderEntries.size());
    for (const auto& entry : data_->folderEntries)
        data_->itemKeys.push_back(entry.fullPath);
    InvalidateSlots();
}

/**
 * @brief 获取文件夹条目总数
 * @return 条目数量
 */
size_t FolderMapping::GetSlotCount() const
{
    return data_ ? data_->folderEntries.size() : 0;
}

/**
 * @brief 获取每个条目的高度
 * @return 条目高度（像素）。列表模式下固定为 38 像素，
 *         图标模式下取当前单元格高度
 */
int FolderMapping::GetItemHeight() const
{
    return (data_ && data_->listMode) ? 38 : FolderMappingCellHeight(const_cast<FolderMapping*>(this));
}

/**
 * @brief 获取每个条目的宽度
 * @return 条目宽度（像素）。列表模式下返回父类默认宽度；
 *         图标模式下按内容区域宽度除以列数计算
 */
int FolderMapping::GetItemWidth() const
{
    if (!data_ || data_->listMode) return WidgetContainer::GetItemWidth();
    RECT content = FolderMappingContentRect(const_cast<FolderMapping*>(this));
    return std::max<int>(1, (content.right - content.left) / std::max(1, data_->gridSpan.columns));
}

/**
 * @brief 获取最大滚动偏移量
 * @return 最大滚动偏移值（像素），委托给 FolderMappingMaxScrollOffset
 */
int FolderMapping::GetMaxScrollOffset() const
{
    return FolderMappingMaxScrollOffset(const_cast<FolderMapping*>(this));
}

/**
 * @brief 获取所有条目的总内容高度
 * @return 总高度（像素），委托给 FolderMappingContentHeight
 */
int FolderMapping::GetTotalContentHeight() const
{
    return FolderMappingContentHeight(const_cast<FolderMapping*>(this), data_ ? data_->folderEntries.size() : 0);
}

/**
 * @brief 获取内容区域的可视高度
 * @return 可视区域高度（像素），即内容裁剪矩形的高度
 */
int FolderMapping::GetVisibleContentHeight() const
{
    RECT content = FolderMappingContentRect(const_cast<FolderMapping*>(this));
    return std::max(1, (int)(content.bottom - content.top));
}

/**
 * @brief 处理外部条目拖放到本组件时的回调
 * @param sourceItems 拖拽源条目列表
 * @param origin      源容器
 * @param targetSlot  目标槽位
 * @param region      命中区域
 * @param mods        键盘修饰键状态
 *
 * 通过 App 的拖放管道（BuildDragSourceList -> BuildDropPreviewList ->
 * ExecuteDropPipeline）执行完整的拖放处理流程。
 */
void FolderMapping::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

/**
 * @brief 绘制映射文件夹的内容区域
 * @param context D2D 设备上下文
 * @param body    组件主体矩形（未使用，内容区域由 FolderMappingContentRect 决定）
 *
 * 当文件夹为空时显示居中提示文字"空文件夹"。
 * 非空时根据当前模式（图标/列表）分别绘制条目，
 * 图标模式下绘制 FolderEntryIcon，列表模式下绘制列表项。
 * 绘制前会设置裁剪区域以确保内容不溢出。
 */
void FolderMapping::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    (void)body;

    if (data_->folderEntries.empty())
    {
        RECT empty = GetBodyRect();
        InflateRect(&empty, -12, -12);
        ComPtr<IDWriteTextFormat> centered;
        if (app_->dwriteFactory_)
        {
            app_->dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"", &centered);
            if (centered)
            {
                centered->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
                centered->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                centered->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            }
        }
        app_->DrawD2DText(context, L"空文件夹", empty,
            centered ? centered.Get() : app_->listItemTextFormat_.Get(),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f));
        return;
    }

    auto& slots = GetSlots();
    RECT content = FolderMappingContentRect(this);
    bool listMode = data_->listMode;

    context->PushAxisAlignedClip(app_->ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < slots.size() && i < data_->folderEntries.size(); ++i)
    {
        const FolderEntry& entry = data_->folderEntries[i];
        RECT cell = slots[i]->GetBounds();
        if (cell.bottom <= content.top || cell.top >= content.bottom) continue;

        if (!listMode)
        {
            RECT bodyRect = GetBodyRect();
            bool hovered = !entry.selected && PtInRect(&cell, app_->lastMousePoint_) && PtInRect(&bodyRect, app_->lastMousePoint_);
            FolderEntryIcon icon(const_cast<FolderEntry*>(&entry), this, app_);
            icon.Draw(context, cell, entry.selected ? 2 : (hovered ? 1 : 0));
            continue;
        }

        DrawListItem(context, cell, entry.iconBitmap, entry.name, entry.selected);
    }
    context->PopAxisAlignedClip();
}

/**
 * @brief 绘制标题栏上的切换视图和打开文件夹按钮
 * @param context    D2D 设备上下文
 * @param handleRect 标题栏矩形
 * @param hovered    标题栏是否处于悬停状态（当前未使用）
 *
 * 在标题栏右侧绘制两个按钮：
 * - 切换按钮：在图标模式（网格）和列表模式之间切换
 * - 打开文件夹按钮：打开当前映射的磁盘文件夹
 * 按钮使用 Font Awesome 图标，并具有悬停高亮效果。
 */
void FolderMapping::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    if (!data_ || !app_) return;

    constexpr int btnSize = 14;
    constexpr int gap = 4;
    constexpr int gapBetween = 7;
    constexpr int resizeReserve = 20;
    RECT toggleBtn = {
        handleRect.right - resizeReserve - gap - btnSize - gapBetween - btnSize,
        handleRect.top + 5,
        handleRect.right - resizeReserve - gap - btnSize - gapBetween,
        handleRect.bottom - 3
    };
    RECT openBtn = {
        handleRect.right - resizeReserve - gap - btnSize,
        handleRect.top + 5,
        handleRect.right - resizeReserve - gap,
        handleRect.bottom - 3
    };

    auto drawFaButton = [&](RECT rect, const std::wstring& glyph) {
        bool hot = PtInRect(&rect, app_->lastMousePoint_) != FALSE;
        app_->DrawD2DRoundedRectangle(context, rect, 4.0f,
            hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.0f));
        app_->DrawD2DText(context, glyph, rect,
            app_->faTextFormat_ ? app_->faTextFormat_.Get() : app_->listItemTextFormat_.Get(),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.85f));
    };

    drawFaButton(toggleBtn, data_->listMode ? L"" : L"");
    drawFaButton(openBtn, L"");
    (void)hovered;
}

/**
 * @brief 测试鼠标点击位置命中了哪个组件区域
 * @param pt 鼠标点击坐标
 * @return 命中类型。若命中了标题栏，进一步判断是否命中
 *         视图切换按钮或打开文件夹按钮
 */
WidgetHit FolderMapping::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base != WidgetHit::MoveHandle || !data_) return base;

    RECT handle = GetMoveHandleRect();
    constexpr int btnSize = 14;
    constexpr int gap = 4;
    constexpr int gapBetween = 7;
    constexpr int resizeReserve = 20;
    RECT toggleBtn = {
        handle.right - resizeReserve - gap - btnSize - gapBetween - btnSize,
        handle.top + 5,
        handle.right - resizeReserve - gap - btnSize - gapBetween,
        handle.bottom - 3
    };
    RECT openBtn = {
        handle.right - resizeReserve - gap - btnSize,
        handle.top + 5,
        handle.right - resizeReserve - gap,
        handle.bottom - 3
    };
    if (PtInRect(&toggleBtn, pt)) return WidgetHit::ListToggleBtn;
    if (PtInRect(&openBtn, pt)) return WidgetHit::OpenFolderBtn;
    return base;
}

/**
 * @brief 获取当前选中的所有条目对象
 * @return 选中条目的 Item 指针向量
 *
 * 遍历当前槽位，收集所有被选中（entry.selected == true）的条目，
 * 创建对应的 FolderEntryIcon 并缓存到 dragSourceCache_ 中
 * 以支持拖放操作。
 */
std::vector<Item*> FolderMapping::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_) return result;

    for (const auto& slot : const_cast<FolderMapping*>(this)->GetSlots())
    {
        if (!slot) continue;
        Item* slotItem = slot->GetItem();
        auto* slotIcon = dynamic_cast<FolderEntryIcon*>(slotItem);
        FolderEntry* entry = slotIcon ? slotIcon->GetFolderEntry() : nullptr;
        if (!entry || !entry->selected) continue;

        auto icon = std::make_unique<FolderEntryIcon>(entry, const_cast<FolderMapping*>(this), app_);
        icon->SetBounds(slot->GetBounds());
        result.push_back(icon.get());
        dragSourceCache_.push_back(std::move(icon));
    }
    return result;
}
