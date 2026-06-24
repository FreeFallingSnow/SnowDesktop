/**
 * @file collection.cpp
 * @brief Collection（集合）控件实现
 *
 * 实现固定大小的缩略图网格控件，支持分类标签页和"全部"拼贴按钮。
 * Collection 是桌面上的一类特殊容器控件，以网格形式展示其子项缩略图，
 * 提供紧凑模式（2x2 网格）和标准模式（与桌面网格对齐）两种布局。
 * 当子项数量超过内联容量时，最后一个槽位变为"全部"拼贴按钮，
 * 以 2x2 缩略图形式展示剩余项目。
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
#include <unordered_set>

// ═══════════════════════════════════════════════════════════════
/// @name 静态辅助函数
/// 集合控件布局计算相关工具函数，非类成员。
/// @{
// ═══════════════════════════════════════════════════════════════

/**
 * @brief 获取集合的内联容量（直接显示的缩略图数量）
 *
 * 紧凑模式（单行单列）下返回 4（2x2 网格），
 * 非紧凑模式下返回 (列数 x 行数 - 1)，预留最后一个位置给"全部"按钮。
 * @param widget  桌面控件描述符
 * @return 内联可显示的缩略图数量
 */
static size_t GetCollectionInlineCapacity(const DesktopWidget& widget)
{
    if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1) return 4;
    return static_cast<size_t>(std::max(1, widget.gridSpan.columns) * std::max(1, widget.gridSpan.rows) - 1);
}

/**
 * @brief 获取"全部"拼贴按钮的槽位索引
 *
 * 紧凑模式下无"全部"按钮，返回 (size_t)-1。
 * 非紧凑模式下返回最后一个槽位的索引（= 总网格数 - 1）。
 * @param widget  桌面控件描述符
 * @return 槽位索引，若不存在"全部"按钮则返回 (size_t)-1
 */
static size_t GetCollectionAllButtonSlot(const DesktopWidget& widget)
{
    if (widget.gridSpan.columns <= 1 && widget.gridSpan.rows <= 1)
        return static_cast<size_t>(-1);
    return static_cast<size_t>(std::max(1, widget.gridSpan.columns) * std::max(1, widget.gridSpan.rows) - 1);
}

/**
 * @brief 计算集合中指定槽位的矩形区域（与桌面网格对齐）
 *
 * 紧凑模式（2x2 网格）下，槽位居中于 body 区域内；
 * 非紧凑模式下槽位与桌面网格的行列对齐，使用网格页面配置的 cellHeight 和 gapY。
 * 此函数向后兼容原有的 GetCollectionPreviewSlotRect。
 * @param widget  桌面控件描述符
 * @param slot    槽位序号
 * @param body    集合控件的 body 矩形
 * @param app     桌面应用指针，用于获取网格页面配置
 * @return 槽位的矩形区域，若无效则返回空矩形
 */
static RECT GetCollectionSlotRect(const Collection* collection, size_t slot, RECT body)
{
    if (!collection || IsRectEmptyRect(body)) return {};
    DesktopWidget* data = collection->GetWidgetData();
    DesktopApp* app = collection->GetApp();
    if (!data || !app) return {};

    bool compact = data->gridSpan.columns <= 1 && data->gridSpan.rows <= 1;
    if (compact)
    {
        // 2×2 grid centered in the body
        InflateRect(&body, -collection->Cu(6.0f), -collection->Cu(6.0f));
        int columns = 2;
        int bodyW = std::max<int>(1, (int)(body.right - body.left));
        int bodyH = std::max<int>(1, (int)(body.bottom - body.top));
        int gridSize = std::min(bodyW, bodyH);
        const auto& pages = app->GetDesktopGrid()->GetPages();
        const GridPage* page = nullptr;
        for (const auto& p : pages)
            if (p.id == data->gridCell.pageId) { page = &p; break; }
        int gapY = page ? page->gapY : 0;
        int gridTop = body.top + gapY / 2 - collection->Cu(10.0f);
        int slotSz = std::max<int>(1, gridSize / 2);
        int col = (int)(slot % (size_t)columns);
        int row = (int)(slot / (size_t)columns);
        RECT rect = { body.left + col * slotSz, gridTop + row * slotSz,
                      col + 1 == columns ? body.right : body.left + (col + 1) * slotSz,
                      row + 1 == 2 ? gridTop + gridSize : gridTop + (row + 1) * slotSz };
        InflateRect(&rect, -collection->Cu(1.0f), -collection->Cu(1.0f));
        return rect;
    }

    // Non-compact: grid-aligned slots matching desktop cell size
    InflateRect(&body, -collection->Cu(4.0f), -collection->Cu(4.0f));
    const auto& pages = app->GetDesktopGrid()->GetPages();
    const GridPage* page = nullptr;
    for (const auto& p : pages)
        if (p.id == data->gridCell.pageId) { page = &p; break; }
    int cellH = page ? page->cellHeight : 96;
    int gapY = page ? page->gapY : 0;
    int columns = std::max(1, data->gridSpan.columns);
    int rows = std::max(1, data->gridSpan.rows);
    int col = (int)(slot % (size_t)columns);
    int rowIdx = (int)(slot / (size_t)columns);
    if (rowIdx >= rows) return {};

    int width = std::max<int>(1, (int)(body.right - body.left) / columns);
    int startY = body.top + gapY / 2 - collection->Cu(8.0f);
    int rowStep = cellH + gapY;
    return { body.left + col * width, startY + rowIdx * rowStep,
             col + 1 == columns ? body.right : body.left + (col + 1) * width,
             startY + rowIdx * rowStep + cellH };
}

/// @}

// ═══════════════════════════════════════════════════════════════
/// @name 绘制方法
/// 集合控件及其子项的缩略图绘制。
/// @{
// ═══════════════════════════════════════════════════════════════

/**
 * @brief 绘制单个桌面项的缩略图
 *
 * 在指定的矩形区域内绘制项的图标（Icon），若项处于选中状态则先绘制蓝色高亮背景。
 * 图标居中绘制，大小自适应（限制在 16px 到区域尺寸之间）。
 * @param context  Direct2D 设备上下文
 * @param item     要绘制的桌面项
 * @param rect     绘制区域矩形
 * @param selected 是否处于选中状态
 */
void Collection::DrawThumbnail(ID2D1DeviceContext* context,
    const DesktopItem& item, RECT rect, bool selected) const
{
    if (!app_ || !context || IsRectEmptyRect(rect)) return;

    if (selected)
    {
        app_->DrawD2DRoundedRectangle(context, rect, static_cast<float>(Cu(7.0f)),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.24f),
            D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.78f));
    }

    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;
    const int available = std::max(1,
        std::min(width - Cu(6.0f), height - Cu(4.0f)));
    const int iconSize = std::min(
        std::max(Cu(16.0f), available),
        std::max(1, std::min(width, height)));
    const int iconX = rect.left + (width - iconSize) / 2;
    const int iconY = rect.top + (height - iconSize) / 2;

    if (item.iconState == IconState::Loading)
    {
        RECT placeholderRect = { iconX, iconY, iconX + iconSize, iconY + iconSize };
        app_->DrawPlaceholderIcon(context, item.sysIconIndex, placeholderRect, 1.0f);
    }
    else if (item.iconBitmap)
    {
        ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(item.iconBitmap);
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(static_cast<float>(iconX), static_cast<float>(iconY),
                static_cast<float>(iconX + iconSize), static_cast<float>(iconY + iconSize));
            context->DrawBitmap(bmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        else
        {
            RECT placeholderRect = { iconX, iconY, iconX + iconSize, iconY + iconSize };
            app_->DrawPlaceholderIcon(context, item.sysIconIndex, placeholderRect, 1.0f);
        }
    }

    if (item.shortcutArrow && item.iconState != IconState::Loading)
    {
        RECT arrowRect = { iconX, iconY, iconX + iconSize, iconY + iconSize };
        app_->DrawShortcutArrowOverlay(context, arrowRect, 1.0f);
    }
}

/**
 * @brief 绘制集合控件的全部内容
 *
 * 遍历内联槽位绘制每个可见项。紧凑模式下调用 DrawThumbnail 绘制纯缩略图，
 * 非紧凑模式下以 DesktopIcon 方式绘制（含文本标签、悬浮高亮等效果）。
 * 当存在超出内联容量的剩余项时，在"全部"按钮位置绘制 2x2 拼贴缩略图。
 * 若该区域无有效剩余项且鼠标不在区域内，则跳过拼贴绘制以提升性能。
 * @param context  Direct2D 设备上下文
 * @param body     集合控件的 body 矩形
 */
void Collection::DrawContent(ID2D1DeviceContext* context, RECT body)
{
    if (!data_ || !app_) return;
    if (data_->itemKeys.empty()) return;

    bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    const auto& items = app_->GetDesktopItems();
    auto& slots = GetSlots();
    size_t inlineCapacity = std::min(GetCollectionInlineCapacity(*data_), data_->itemKeys.size());

    for (size_t i = 0; i < inlineCapacity && i < slots.size(); ++i)
    {
        if (i >= data_->itemKeys.size()) break;
        size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[i]);
        if (itemIdx == static_cast<size_t>(-1)) continue;

        const DesktopItem& di = items[itemIdx];
        RECT slotRect = slots[i]->GetBounds();
        if (IsRectEmptyRect(slotRect)) continue;

        if (compact)
            DrawThumbnail(context, di, slotRect, di.selected);
        else
        {
            RECT bodyRect = GetBodyRect();
            bool hovered = PtInRect(&slotRect, app_->lastMousePoint_) != FALSE && !di.selected && PtInRect(&bodyRect, app_->lastMousePoint_);
            DesktopIcon icon(const_cast<DesktopItem*>(&di), const_cast<Collection*>(this), app_);
            icon.Draw(context, slotRect, di.selected ? 2 : (hovered ? 1 : 0));
        }
    }

    // "All" button: 2×2 mosaic of remaining items
    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot != static_cast<size_t>(-1) && !compact)
    {
        RECT allRect = GetCollectionSlotRect(this, allSlot, body);
        if (!IsRectEmptyRect(allRect))
        {
            bool hasRemainingIcon = false;
            for (size_t j = 0; j < 4; ++j)
            {
                size_t keyIdx = inlineCapacity + j;
                if (keyIdx < data_->itemKeys.size() &&
                    app_->FindItemIndexByKey(data_->itemKeys[keyIdx]) != static_cast<size_t>(-1))
                {
                    hasRemainingIcon = true;
                    break;
                }
            }
            if (!hasRemainingIcon && !PtInRect(&allRect, app_->lastMousePoint_))
                return;

            // Draw 2×2 thumbnail grid
            RECT inner = allRect;
            InflateRect(&inner, -Cu(8.0f), -Cu(8.0f));
            OffsetRect(&inner, 0, -Cu(4.0f));
            int tileW = std::max<int>(1, (int)(inner.right - inner.left) / 2);
            int tileH = std::max<int>(1, (int)(inner.bottom - inner.top) / 2);

            for (int j = 0; j < 4; ++j)
            {
                int col = j % 2;
                int row = j / 2;
                RECT tile = { inner.left + col * tileW, inner.top + row * tileH,
                              col + 1 == 2 ? inner.right : inner.left + (col + 1) * tileW,
                              row + 1 == 2 ? inner.bottom : inner.top + (row + 1) * tileH };
                InflateRect(&tile, -Cu(2.0f), -Cu(2.0f));

                size_t keyIdx = inlineCapacity + (size_t)j;
                if (keyIdx < data_->itemKeys.size())
                {
                    size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[keyIdx]);
                    if (itemIdx != static_cast<size_t>(-1))
                    {
                        const DesktopItem& di = items[itemIdx];
                        InflateRect(&tile, -Cu(4.0f), -Cu(4.0f));
                        DrawThumbnail(context, di, tile, di.selected);
                    }
                }
                else
                {
                    if (!hasRemainingIcon)
                    {
                        InflateRect(&tile, -Cu(2.0f), -Cu(2.0f));
                        app_->DrawD2DRoundedRectangle(context, tile, static_cast<float>(Cu(3.0f)),
                            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f),
                            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f));
                    }
                }
            }
        }
    }
}

/// @}

// ═══════════════════════════════════════════════════════════════
/// @name 槽管理
/// 集合内联槽位的构建与查询。
/// @{
// ═══════════════════════════════════════════════════════════════

/**
 * @brief 构建集合的所有内联槽位
 *
 * 根据集合的内联容量计算可见槽位数，为每个槽位计算矩形区域，
 * 创建 Slot 对象并关联对应的 Item。槽位 Item 临时缓存在 slotItemCache_ 中。
 * @return 包含所有可见槽位的 unique_ptr 容器
 */
std::vector<std::unique_ptr<Slot>> Collection::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;

    size_t inlineCap = GetCollectionInlineCapacity(*data_);
    size_t visible = std::min(inlineCap, data_->itemKeys.size());
    RECT body = GetBodyRect();
    for (size_t idx = 0; idx < visible; ++idx)
    {
        RECT cell = GetCollectionSlotRect(this, idx, body);
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
 * @brief 获取当前可见的槽位数量
 *
 * 取内联容量与项总数的较小值，即实际显示的缩略图数量。
 * @return 可见槽位数，无数据时返回 0
 */
size_t Collection::GetSlotCount() const
{
    if (!data_) return 0;
    if (data_->itemKeys.empty()) return 0;

    size_t inlineCap = GetCollectionInlineCapacity(*data_);
    size_t visible = std::min(inlineCap, data_->itemKeys.size());

    return visible;
}

/**
 * @brief 获取指定索引处的槽位关联项
 *
 * 创建 DesktopIcon 作为该槽位的显示项，并缓存到 slotItemCache_ 中，
 * 供后续绘制使用。
 * @param idx 项在集合中的索引
 * @return 关联的 Item 指针，无效索引或超出内联容量时返回 nullptr
 */
Item* Collection::GetSlotItem(size_t idx) const
{
    if (!data_ || idx >= data_->itemKeys.size() || !app_) return nullptr;
    if (idx >= GetCollectionInlineCapacity(*data_)) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<Collection*>(this), app_);
    Item* result = icon.get();
    slotItemCache_.push_back(std::move(icon));
    return result;
}

/// @}

// ═══════════════════════════════════════════════════════════════
/// @name 成员访问与拖拽
/// 集合成员的查询、选择、重排序以及拖拽源项获取。
/// @{
// ═══════════════════════════════════════════════════════════════

/**
 * @brief 获取集合中指定索引处的成员项（用于拖拽操作）
 *
 * 与 GetSlotItem 类似，但创建的 DesktopIcon 缓存在 dragSourceCache_ 中，
 * 供拖拽过程中使用，避免与常规槽位显示缓存冲突。
 * @param idx 项在集合中的索引
 * @return 成员项的 Item 指针，无效时返回 nullptr
 */
Item* Collection::GetMemberItem(size_t idx) const
{
    if (!data_ || idx >= data_->itemKeys.size() || !app_) return nullptr;
    size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[idx]);
    if (itemIdx == static_cast<size_t>(-1)) return nullptr;
    auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[itemIdx],
        const_cast<Collection*>(this), app_);
    Item* result = icon.get();
    dragSourceCache_.push_back(std::move(icon));
    return result;
}

/**
 * @brief 获取集合中所有处于选中状态的成员索引
 *
 * 遍历集合的全部项，筛选出 app 层面标记为 selected 的项。
 * @return 选中项的索引列表
 */
std::vector<size_t> Collection::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_ || !app_) return result;
    for (size_t i = 0; i < data_->itemKeys.size(); ++i)
    {
        size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[i]);
        if (itemIdx == static_cast<size_t>(-1)) continue;
        if (app_->GetDesktopItems()[itemIdx].selected)
            result.push_back(i);
    }
    return result;
}

/**
 * @brief 重排序集合成员
 *
 * 将指定索引的成员移动到目标位置之前。内部从后往前依次移除待移动项以避免索引偏移，
 * 再在 adjusted 位置依次插入。操作完成后使槽位失效以触发重建。
 * @param indices      要移动的项索引列表（由大到小）
 * @param insertBefore 插入位置的目标索引
 */
void Collection::ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
{
    if (!data_) return;
    std::vector<std::wstring> moving;
    for (auto it = indices.rbegin(); it != indices.rend(); ++it)
    {
        if (*it >= data_->itemKeys.size()) continue;
        moving.push_back(data_->itemKeys[*it]);
        data_->itemKeys.erase(data_->itemKeys.begin() + static_cast<std::ptrdiff_t>(*it));
    }
    size_t adjusted = insertBefore;
    for (auto idx : indices)
        if (idx < insertBefore) --adjusted;
    if (adjusted > data_->itemKeys.size()) adjusted = data_->itemKeys.size();
    for (auto it = moving.rbegin(); it != moving.rend(); ++it)
        data_->itemKeys.insert(data_->itemKeys.begin() + static_cast<std::ptrdiff_t>(adjusted++), *it);
    InvalidateSlots();
}

/**
 * @brief 获取集合中所有选中项的 Item 指针（用于拖拽操作）
 *
 * 遍历集合的全部项，为每个选中项创建 DesktopIcon 并计算其可见边界，
 * 缓存在 dragSourceCache_ 中。调用时清空之前的拖拽缓存。
 * @return 选中项的 Item 指针列表
 */
std::vector<Item*> Collection::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_ || !app_) return result;

    for (const auto& key : data_->itemKeys)
    {
        size_t idx = app_->FindItemIndexByKey(key);
        if (idx == static_cast<size_t>(-1)) continue;
        DesktopItem& di = app_->GetDesktopItems()[idx];
        if (!di.selected) continue;
        RECT bounds = app_->GetVisibleCollectionItemBounds(idx);
        if (IsRectEmptyRect(bounds)) continue;

        auto icon = std::make_unique<DesktopIcon>(&app_->GetDesktopItems()[idx], const_cast<Collection*>(this), app_);
        icon->SetBounds(bounds);
        result.push_back(icon.get());
        dragSourceCache_.push_back(std::move(icon));
    }
    return result;
}

/// @}

// ═══════════════════════════════════════════════════════════════
/// @name 交互与命中测试
/// 集合控件的点击检测、"全部"按钮区域计算以及拖放事件处理。
/// @{
// ═══════════════════════════════════════════════════════════════

/**
 * @brief 获取"全部"拼贴按钮的矩形区域
 *
 * 紧凑模式下直接返回 body 区域（压缩内边距后），
 * 非紧凑模式下计算最后一个槽位的矩形。
 * @return "全部"按钮的矩形区域，无按钮时返回空矩形
 */
RECT Collection::GetAllButtonRect() const
{
    if (!data_ || !app_) return {};
    RECT body = GetBodyRect();
    bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    if (compact)
    {
        InflateRect(&body, -Cu(6.0f), -Cu(6.0f));
        return body;
    }
    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot == static_cast<size_t>(-1)) return {};
    return GetCollectionSlotRect(this, allSlot, body);
}

/**
 * @brief 对指定点执行命中测试
 *
 * 先调用基类的命中测试，然后根据集合特性进一步判断：
 * - 紧凑模式下点击内容区域视为点击"全部"按钮（CollectionOpenBtn）
 * - 非紧凑模式下检测是否点击了"全部"拼贴按钮槽位
 * @param pt 测试点的屏幕坐标
 * @return 命中类型，未命中则返回 WidgetHit::None
 */
WidgetHit Collection::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base == WidgetHit::None || base == WidgetHit::ResizeHandle || base == WidgetHit::MoveHandle) return base;
    if (!data_ || !app_ || data_->type != DesktopWidgetType::Collection) return base;

    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt)) return WidgetHit::None;

    const bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    if (compact)
        return base == WidgetHit::Content ? WidgetHit::CollectionOpenBtn : base;

    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot != static_cast<size_t>(-1))
    {
        RECT allRect = GetCollectionSlotRect(this, allSlot, GetBodyRect());
        if (PtInRect(&allRect, pt)) return WidgetHit::CollectionOpenBtn;
    }
    return base;
}

/**
 * @brief 处理项被拖放到集合中的事件
 *
 * 将拖拽源项列表通过 app 构建拖拽源列表（DragSourceList）和放置预览列表（DropPreviewList），
 * 最终由 ExecuteDropPipeline 完成实际的跨容器放置操作。
 * @param sourceItems  被拖拽的源项指针列表
 * @param origin       拖拽来源容器
 * @param targetSlot   拖放目标槽位（可为 nullptr，表示拖放到末尾）
 * @param region       拖放区域（SortBefore / SortAfter）
 * @param mods         键盘修饰键状态
 */
void Collection::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

/**
 * @brief 计算拖放操作的目标插入索引
 *
 * 根据目标槽位和放置区域确定项应插入到 data_->itemKeys 中的位置。
 * @param targetSlot  拖放目标槽位，为 nullptr 时表示插入到末尾
 * @param region      放置区域（SortAfter 表示插入到目标之后）
 * @return 插入位置的索引（范围在 0 到集合总项数之间）
 */
size_t Collection::GetDropInsertIndex(Slot* targetSlot, HitRegion region) const
{
    size_t insertAt = targetSlot ? targetSlot->GetIndex() : (data_ ? data_->itemKeys.size() : 0);
    if (targetSlot && region == HitRegion::SortAfter)
        ++insertAt;
    return data_ ? std::min(insertAt, data_->itemKeys.size()) : insertAt;
}

/// @}
