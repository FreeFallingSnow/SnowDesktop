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
#include "widget_preview_scene.h"
#include "../item_render_layer_rules.h"
#include "../widget_item_layout.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include "../l10n.h"


static RECT CollectionItemRect(Collection* widget, size_t linearIndex);

static size_t CollectionItemCount(Collection* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    return data ? data->itemKeys.size() : 0;
}

// ── Scroll container helpers (shared with draw/slot code) ─────

/**
 * @brief 获取滚动容器内容区域（同 FolderMapping 的 padding）
 */
static RECT CollectionScrollContentRect(Collection* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -widget->Cu(4.0f), -widget->Cu(8.0f));
    return widget->ApplyDetailsHeaderToViewport(body);
}

static snowdesktop::PageItemVisualMetrics CollectionVisualMetrics(
    Collection* widget)
{
    if (!widget || !widget->GetApp())
        return snowdesktop::ResolvePageItemVisualMetrics(
            kCellWidth, kMinCellHeight, kDefaultItemFontSizeCu);
    return widget->GetItemVisualMetrics();
}

static snowdesktop::widget_item_layout::Layout CollectionLocalLayout(
    Collection* widget, int fixedRows = 0)
{
    if (!widget || !widget->GetWidgetData()) return {};
    DesktopWidget* data = widget->GetWidgetData();
    RECT content{};
    if (data->scrollContainerMode)
        content = CollectionScrollContentRect(widget);
    else if (snowdesktop::widget_item_layout::CollectionUsesFullFrame(
            data->scrollContainerMode,
            data->gridSpan.columns, data->gridSpan.rows))
        // The large-folder action bar floats over the frame on hover. It does
        // not own layout space, so fixed rows use the complete widget height.
        content = widget->GetFrameRect();
    else
        content = widget->GetBodyRect();
    const auto metrics = CollectionVisualMetrics(widget);
    const float spacing = widget->GetLayoutSpacingScale();
    if (data->listMode)
    {
        return snowdesktop::widget_item_layout::ResolveList(
            content,
            std::max(widget->GetListRowHeight(),
                metrics.minimumListHeight),
            spacing);
    }
    return snowdesktop::widget_item_layout::ResolveGrid(
        content, std::max(1, data->gridSpan.columns), fixedRows,
        metrics.minimumGridWidth, metrics.minimumGridHeight,
        spacing);
}

/**
 * @brief 计算滚动容器内容总高度
 */
static int CollectionScrollContentHeight(Collection* widget, size_t itemCount)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data || !data->scrollContainerMode) return 0;
    return snowdesktop::widget_item_layout::ContentHeight(
        CollectionLocalLayout(widget), itemCount);
}

/**
 * @brief 计算滚动容器最大滚动偏移
 */
static int CollectionScrollMaxOffset(Collection* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data || !data->scrollContainerMode) return 0;
    RECT content = CollectionScrollContentRect(widget);
    int visibleHeight = std::max<int>(1, content.bottom - content.top);
    return std::max(0, CollectionScrollContentHeight(widget,
        CollectionItemCount(widget)) -
        visibleHeight + widget->Cu(kMinCellHeight / 2.0f));
}

/**
 * @brief 计算滚动容器中条目的绘制矩形
 */
static RECT CollectionItemRect(Collection* widget, size_t linearIndex)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data || !data->scrollContainerMode) return {};
    RECT content = CollectionScrollContentRect(widget);
    int scroll = std::clamp(data->scrollOffset, 0, CollectionScrollMaxOffset(widget));
    (void)content;
    return snowdesktop::widget_item_layout::ItemRect(
        CollectionLocalLayout(widget), linearIndex, scroll);
}

static std::pair<size_t, size_t> CollectionVisibleIndexRange(
    Collection* widget, size_t itemCount)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data || !data->scrollContainerMode || itemCount == 0)
        return { 0, 0 };

    const RECT content = CollectionScrollContentRect(widget);
    const int visibleHeight = std::max<int>(
        1, content.bottom - content.top);
    const int scroll = std::clamp(
        data->scrollOffset, 0, CollectionScrollMaxOffset(widget));

    return snowdesktop::widget_item_layout::VisibleRange(
        CollectionLocalLayout(widget), itemCount,
        scroll, visibleHeight);
}

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
    if (snowdesktop::widget_item_layout::IsCompactCollectionSpan(
            widget.gridSpan.columns, widget.gridSpan.rows)) return 4;
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
    if (snowdesktop::widget_item_layout::IsCompactCollectionSpan(
            widget.gridSpan.columns, widget.gridSpan.rows))
        return static_cast<size_t>(-1);
    return static_cast<size_t>(std::max(1, widget.gridSpan.columns) * std::max(1, widget.gridSpan.rows) - 1);
}

static RECT GetCollectionCompactSlotRect(
    const Collection* collection, size_t slot, RECT body, int padding)
{
    if (!collection || IsRectEmptyRect(body) || slot >= 4) return {};

    constexpr int columns = 2;
    padding = std::max(0, padding);
    InflateRect(&body, -padding, -padding);
    const int bodyW = std::max<int>(1, body.right - body.left);
    const int bodyH = std::max<int>(1, body.bottom - body.top);
    const int itemSize = std::max(1, std::min(bodyW, bodyH) / columns);
    const int extraX = std::max(0, bodyW - itemSize * columns);
    const int extraY = std::max(0, bodyH - itemSize * columns);
    const int gapX = extraX >= 3 ? extraX / 3 : (extraX > 0 ? 1 : 0);
    const int gapY = extraY >= 3 ? extraY / 3 : (extraY > 0 ? 1 : 0);
    const int gridW = itemSize * columns + gapX;
    const int gridH = itemSize * columns + gapY;
    const int gridLeft = body.left + (bodyW - gridW) / 2;
    const int gridTop = body.top + (bodyH - gridH) / 2;
    const int col = static_cast<int>(slot % columns);
    const int row = static_cast<int>(slot / columns);
    return RECT{
        gridLeft + col * (itemSize + gapX),
        gridTop + row * (itemSize + gapY),
        col + 1 == columns ? gridLeft + gridW
                           : gridLeft + (col + 1) * itemSize + col * gapX,
        row + 1 == columns ? gridTop + gridH
                           : gridTop + (row + 1) * itemSize + row * gapY
    };
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

    bool compact = snowdesktop::widget_item_layout::IsCompactCollectionSpan(
        data->gridSpan.columns, data->gridSpan.rows);
    if (compact)
        return GetCollectionCompactSlotRect(collection, slot, body,
            collection->Cu(6.0f));

    int rows = std::max(1, data->gridSpan.rows);
    (void)body;
    return snowdesktop::widget_item_layout::ItemRect(
        CollectionLocalLayout(const_cast<Collection*>(collection), rows),
        slot);
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
    const RECT iconRect = {
        iconX, iconY, iconX + iconSize, iconY + iconSize
    };
    const bool useDemoIdentity =
        app_->ShouldUseDemoCollectionIdentity(data_);
    const std::wstring_view demoIdentity = item.layoutKey.empty()
        ? std::wstring_view(item.parsingName)
        : std::wstring_view(item.layoutKey);

    if (useDemoIdentity)
    {
        app_->DrawDemoCollectionIdentityIcon(
            context, *data_, demoIdentity, iconRect, 1.0f);
    }
    else if (item.iconState == IconState::Loading)
    {
        app_->DrawPlaceholderIcon(context, item.sysIconIndex, iconRect, 1.0f);
    }
    else
    {
        ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(
            item.iconBitmap,
            app_->ShouldBeautifyIconBitmap(item.iconIsMediaThumbnail));
        if (bmp)
        {
            D2D1_RECT_F dst = D2D1::RectF(static_cast<float>(iconX), static_cast<float>(iconY),
                static_cast<float>(iconX + iconSize), static_cast<float>(iconY + iconSize));
            context->DrawBitmap(bmp, dst, 1.0f, D2D1_INTERPOLATION_MODE_LINEAR);
        }
        else
        {
            app_->DrawPlaceholderIcon(context, item.sysIconIndex, iconRect, 1.0f);
        }
    }

    if (!useDemoIdentity &&
        app_->ShouldDrawShortcutArrow(item.isShortcut, item.isApplicationShortcut) &&
        item.iconState != IconState::Loading)
    {
        app_->DrawShortcutArrowOverlay(context, iconRect, 1.0f);
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
    if (data_->itemKeys.empty())
    {
        if (data_->scrollContainerMode)
            DrawDetailsHeader(context, GetContentViewportRect());
        return;
    }
    const bool preview = IsPreviewRendering();
    auto resolveItem = [&](const std::wstring& key) -> DesktopItem* {
        if (auto* scene = GetPreviewScene())
            return scene->FindDesktopItem(key);
        const size_t index = app_->FindItemIndexByKey(key);
        return index == static_cast<size_t>(-1)
            ? nullptr : &app_->GetDesktopItems()[index];
    };
    const bool lt = app_->IsLightContentTheme();

    bool privacyActive = data_->privacyMode &&
        !app_->dragSession_.IsActive() &&
        !app_->dragDropController_.IsExternalDragActive() &&
        !PtInRect(&data_->bounds, app_->lastMousePoint_);

    // ── Scroll container mode (like FolderMapping) ───────────
    if (data_->scrollContainerMode)
    {
        RECT content = GetContentViewportRect();
        DrawDetailsHeader(context, content);
        context->PushAxisAlignedClip(app_->ToD2DRect(content), D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

        auto& slots = GetSlots();
        std::vector<std::pair<Item*, RECT>>
            foregroundTitles;
        for (const auto& slot : slots)
        {
            if (!slot) continue;
            const size_t itemKeyIndex = slot->GetIndex();
            if (itemKeyIndex >= data_->itemKeys.size()) continue;
            RECT cell = slot->GetBounds();
            if (cell.bottom <= content.top || cell.top >= content.bottom) continue;

            auto* icon = dynamic_cast<DesktopIcon*>(slot->GetItem());
            DesktopItem* item = icon ? icon->GetDesktopItem() : nullptr;
            if (!item) continue;
            const DesktopItem& di = *item;

            if (!data_->listMode)
            {
                if (privacyActive)
                    DrawPrivacyPlaceholder(context, cell, di.name, false);
                else
                {
                    bool hovered = !preview && !di.selected && PtInRect(&cell, app_->lastMousePoint_) && PtInRect(&content, app_->lastMousePoint_);
                    const auto titleLayers =
                        snowdesktop::item_render_layer_rules::
                            ResolveTitleLayerPlan(di.selected);
                    icon->Draw(context, cell, di.selected ? 2 : (hovered ? 1 : 0),
                        lt, titleLayers.drawWithItem, false, data_);
                    if (titleLayers.drawInForeground)
                        foregroundTitles.emplace_back(icon, cell);
                }
            }
            else
            {
                if (privacyActive)
                    DrawPrivacyPlaceholder(context, cell, di.name, false);
                else
                {
                    const bool useDemoIdentity =
                        app_->ShouldUseDemoCollectionIdentity(data_);
                    const std::wstring_view demoIdentity =
                        !useDemoIdentity ? std::wstring_view{} :
                        (di.layoutKey.empty()
                            ? std::wstring_view(di.parsingName)
                            : std::wstring_view(di.layoutKey));
                    DrawListItem(context, cell, di.iconBitmap,
                        di.sysIconIndex, di.name, di.selected,
                        di.iconIsMediaThumbnail, demoIdentity, data_,
                        { di.typeName, di.modifiedTime,
                          di.fileSize, false });
                }
            }
        }
        for (const auto& [item, bounds] : foregroundTitles)
            item->DrawTitle(
                context, bounds, true, 1.0f, lt, data_);
        context->PopAxisAlignedClip();
        return;
    }

    // ── Original: large folder / compact grid mode ────────────
    bool compact = snowdesktop::widget_item_layout::IsCompactCollectionSpan(
        data_->gridSpan.columns, data_->gridSpan.rows);
    auto& slots = GetSlots();
    const size_t displayItemCount = CollectionItemCount(this);
    size_t inlineCapacity = std::min(
        GetCollectionInlineCapacity(*data_), displayItemCount);
    std::vector<std::pair<Item*, RECT>>
        foregroundTitles;

    for (size_t i = 0; i < inlineCapacity && i < slots.size(); ++i)
    {
        if (i >= data_->itemKeys.size()) break;
        if (!slots[i]) continue;
        auto* icon = dynamic_cast<DesktopIcon*>(slots[i]->GetItem());
        DesktopItem* item = icon ? icon->GetDesktopItem() : nullptr;
        if (!item) continue;
        const DesktopItem& di = *item;
        RECT slotRect = slots[i]->GetBounds();
        if (IsRectEmptyRect(slotRect)) continue;

        if (compact)
        {
            if (privacyActive)
                DrawPrivacyPlaceholder(context, slotRect, di.name, false, false);
            else
                DrawThumbnail(context, di, slotRect, di.selected);
        }
        else
        {
            if (privacyActive)
                DrawPrivacyPlaceholder(context, slotRect, di.name, false);
            else
            {
                RECT bodyRect = GetBodyRect();
                bool hovered = !preview && PtInRect(&slotRect, app_->lastMousePoint_) != FALSE && !di.selected && PtInRect(&bodyRect, app_->lastMousePoint_);
                const auto titleLayers =
                    snowdesktop::item_render_layer_rules::
                        ResolveTitleLayerPlan(di.selected);
                icon->Draw(context, slotRect, di.selected ? 2 : (hovered ? 1 : 0),
                    lt, titleLayers.drawWithItem, false, data_);
                if (titleLayers.drawInForeground)
                    foregroundTitles.emplace_back(icon, slotRect);
            }
        }
    }

    // "All" button: 2×2 mosaic of remaining items
    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot != static_cast<size_t>(-1) && !compact &&
        displayItemCount > inlineCapacity)
    {
        RECT allRect = GetCollectionSlotRect(this, allSlot, body);
        if (!IsRectEmptyRect(allRect))
        {
            bool hasRemainingIcon = false;
            for (size_t j = 0; j < 4; ++j)
            {
                size_t keyIdx = inlineCapacity + j;
                if (keyIdx < displayItemCount &&
                    resolveItem(data_->itemKeys[keyIdx]) != nullptr)
                {
                    hasRemainingIcon = true;
                    break;
                }
            }
            if (!hasRemainingIcon && !PtInRect(&allRect, app_->lastMousePoint_))
            {
                for (const auto& [item, bounds] : foregroundTitles)
                    item->DrawTitle(
                        context, bounds, true, 1.0f, lt, data_);
                return;
            }

            const RECT mosaicRect = app_->GetItemIconRect(allRect);
            for (int j = 0; j < 4; ++j)
            {
                RECT tile = GetCollectionCompactSlotRect(this,
                    static_cast<size_t>(j), mosaicRect, 0);

                size_t keyIdx = inlineCapacity + (size_t)j;
                if (keyIdx < displayItemCount)
                {
                    if (DesktopItem* item =
                            resolveItem(data_->itemKeys[keyIdx]))
                    {
                        const DesktopItem& di = *item;
                        if (privacyActive)
                            DrawPrivacyPlaceholder(context, tile, di.name, false);
                        else
                            DrawThumbnail(context, di, tile, di.selected);
                    }
                }
                else
                {
                    if (!hasRemainingIcon)
                    {
                        InflateRect(&tile, -Cu(2.0f), -Cu(2.0f));
                        app_->DrawD2DRoundedRectangle(context, tile, static_cast<float>(Cu(3.0f)),
                            lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.24f),
                            lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.20f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.32f));
                    }
                }
            }

            const std::wstring collectionTitle =
                app_->ShouldUseDemoCollectionIdentity(data_)
                ? app_->GetDemoCollectionCategoryTitle(*data_)
                : (data_->title.empty()
                    ? _LW("widget.collection") : data_->title);
            app_->DrawItemText(context, allRect, collectionTitle, false, 1.0f,
                app_->IsLightContentTheme());
        }
    }
    for (const auto& [item, bounds] : foregroundTitles)
        item->DrawTitle(
            context, bounds, true, 1.0f, lt, data_);
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

    // ── Scroll container mode: only materialize the visible rows ──
    if (data_->scrollContainerMode)
    {
        const auto [firstIndex, lastIndex] =
            CollectionVisibleIndexRange(
                this, CollectionItemCount(this));
        slots.reserve(lastIndex - firstIndex);
        for (size_t idx = firstIndex; idx < lastIndex; ++idx)
        {
            RECT cell = CollectionItemRect(this, idx);
            if (IsRectEmptyRect(cell)) continue;
            auto slot = std::make_unique<Slot>(this, cell, idx);
            Item* item = GetSlotItem(idx);
            if (item) item->SetBounds(cell);
            slot->SetItem(item);
            slots.push_back(std::move(slot));
        }
        return slots;
    }

    // ── Original: large folder mode ─────────────────────────
    size_t inlineCap = GetCollectionInlineCapacity(*data_);
    size_t visible = std::min(
        inlineCap, CollectionItemCount(this));
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

    if (data_->scrollContainerMode)
        return CollectionItemCount(
            const_cast<Collection*>(this));

    size_t inlineCap = GetCollectionInlineCapacity(*data_);
    size_t visible = std::min(inlineCap,
        CollectionItemCount(const_cast<Collection*>(this)));

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
    if (!data_ || !app_ ||
        idx >= CollectionItemCount(const_cast<Collection*>(this)))
        return nullptr;
    if (!data_->scrollContainerMode && idx >= GetCollectionInlineCapacity(*data_)) return nullptr;
    if (auto* scene = GetPreviewScene())
    {
        DesktopItem* sample = scene->FindDesktopItem(data_->itemKeys[idx]);
        if (!sample) return nullptr;
        auto icon = std::make_unique<DesktopIcon>(
            sample,
            const_cast<Collection*>(this), app_);
        Item* result = icon.get();
        slotItemCache_.push_back(std::move(icon));
        return result;
    }
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
    data_->contentSortColumn =
        snowdesktop::list_detail_rules::Column::None;
    if (app_)
        app_->demoCollectionIdentityCache_.erase(data_->id);
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
    if (data_->scrollContainerMode) return {};
    RECT body = GetBodyRect();
    bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    if (compact)
    {
        InflateRect(&body, -Cu(6.0f), -Cu(6.0f));
        return body;
    }
    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot == static_cast<size_t>(-1)) return {};
    if (CollectionItemCount(const_cast<Collection*>(this)) <=
        GetCollectionInlineCapacity(*data_))
        return {};
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
    if (base == WidgetHit::None || base == WidgetHit::ResizeHandle) return base;
    if (!data_ || !app_ || data_->type != DesktopWidgetType::Collection) return base;

    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt)) return WidgetHit::None;

    if (data_->scrollContainerMode)
    {
        const WidgetHit details = HitTestDetailsHeader(
            pt, GetContentViewportRect());
        if (details != WidgetHit::None) return details;
        if (base == WidgetHit::MoveHandle)
        {
            RECT handle = GetMoveHandleRect();
            const float bs = GetBarScale();
            const int btnSize = Cu(14.0f * bs);
            const int gap = Cu(4.0f * bs);
            const int resizeReserve = Cu(20.0f * bs);
            RECT toggleBtn = {
                handle.right - resizeReserve - gap - btnSize,
                handle.top + (handle.bottom - handle.top - btnSize) / 2,
                handle.right - resizeReserve - gap,
                handle.top + (handle.bottom - handle.top + btnSize) / 2
            };
            if (PtInRect(&toggleBtn, pt)) return WidgetHit::ListToggleBtn;
        }
        return base;
    }

    if (base == WidgetHit::MoveHandle) return base;

    const bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    if (compact)
        return base == WidgetHit::Content ? WidgetHit::CollectionOpenBtn : base;

    size_t allSlot = GetCollectionAllButtonSlot(*data_);
    if (allSlot != static_cast<size_t>(-1) &&
        CollectionItemCount(const_cast<Collection*>(this)) >
            GetCollectionInlineCapacity(*data_))
    {
        RECT allRect = GetCollectionSlotRect(this, allSlot, GetBodyRect());
        if (PtInRect(&allRect, pt)) return WidgetHit::CollectionOpenBtn;
    }
    return base;
}

HitRegion Collection::HitTestDrag(POINT pt, Slot*& outSlot)
{
    HitRegion result = WidgetContainer::HitTestDrag(pt, outSlot);
    if (!data_ || result != HitRegion::Handoff) return result;
    bool compact = data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    if (!compact) return result;
    if (!outSlot) return HitRegion::SortAfter;
    RECT bounds = outSlot->GetBounds();
    int cellH = bounds.bottom - bounds.top;
    return (pt.y < bounds.top + cellH / 2)
        ? HitRegion::SortBefore
        : HitRegion::SortAfter;
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

// ═══════════════════════════════════════════════════════════════
/// @name 滚动容器方法
/// Collection 在 scrollContainerMode 下的滚动、尺寸、布局方法。
/// @{
// ═══════════════════════════════════════════════════════════════

int Collection::GetMaxScrollOffset() const
{
    return CollectionScrollMaxOffset(const_cast<Collection*>(this));
}

int Collection::GetTotalContentHeight() const
{
    if (!data_ || !data_->scrollContainerMode) return 0;
    return CollectionScrollContentHeight(const_cast<Collection*>(this),
        CollectionItemCount(const_cast<Collection*>(this)));
}

int Collection::GetVisibleContentHeight() const
{
    RECT content = CollectionScrollContentRect(const_cast<Collection*>(this));
    return std::max(1, (int)(content.bottom - content.top));
}

bool Collection::SingleColumn() const
{
    if (!data_ || !data_->scrollContainerMode) return false;
    return data_->listMode;
}

BarStyle Collection::GetInsertionStyle() const
{
    if (!data_ || !data_->scrollContainerMode) return BarStyle::VBar;
    return data_->listMode ? BarStyle::HBar : BarStyle::VBar;
}

int Collection::GetItemHeight() const
{
    if (!data_ || !data_->scrollContainerMode) return Cu(136.0f);
    return CollectionLocalLayout(const_cast<Collection*>(this)).vertical.cell;
}

RECT Collection::GetMemberLayoutRect(size_t index) const
{
    return CollectionItemRect(
        const_cast<Collection*>(this), index);
}

int Collection::GetItemWidth() const
{
    if (!data_ || !data_->scrollContainerMode) return Cu(92.0f);
    return CollectionLocalLayout(
        const_cast<Collection*>(this)).horizontal.cell;
}

void Collection::DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
{
    if (!data_ || !app_ || !data_->scrollContainerMode) return;

    const float bs = GetBarScale();
    const int btnSize = Cu(14.0f * bs);
    const int gap = Cu(4.0f * bs);
    const int resizeReserve = Cu(20.0f * bs);
    RECT toggleBtn = {
        handleRect.right - resizeReserve - gap - btnSize,
        handleRect.top + (handleRect.bottom - handleRect.top - btnSize) / 2,
        handleRect.right - resizeReserve - gap,
        handleRect.top + (handleRect.bottom - handleRect.top + btnSize) / 2
    };

    IDWriteTextFormat* fluentFormat =
        GetCuFluentTextFormat(14.0f * bs);

    bool hot = PtInRect(&toggleBtn, app_->lastMousePoint_) != FALSE;
    app_->DrawD2DText(context,
        data_->listMode ? L"\uF462" : L"\uF4ED", toggleBtn,
        fluentFormat ? fluentFormat :
            (app_->fluentIconTextFormat_
                ? app_->fluentIconTextFormat_.Get()
                : app_->listItemTextFormat_.Get()),
        app_->IsLightContentTheme()
            ? (hot ? D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.85f) : D2D1::ColorF(0.10f, 0.12f, 0.16f, 0.50f))
            : (hot ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.95f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.60f)));
    (void)hovered;
}

RECT Collection::GetContentViewportRect() const
{
    if (!data_ || !data_->scrollContainerMode)
        return GetBodyRect();
    return CollectionScrollContentRect(const_cast<Collection*>(this));
}

void Collection::ApplyMarqueeSelection(const RECT& contentRect)
{
    if (!data_ || !app_) return;
    if (!data_->scrollContainerMode) return;
    const int scroll = GetScrollOffset();
    for (size_t i = 0; i < data_->itemKeys.size(); ++i)
    {
        RECT itemRect = CollectionItemRect(const_cast<Collection*>(this), i);
        OffsetRect(&itemRect, 0, scroll);
        size_t itemIdx = app_->FindItemIndexByKey(data_->itemKeys[i]);
        if (itemIdx != static_cast<size_t>(-1))
            app_->GetDesktopItems()[itemIdx].selected = RectsIntersect(itemRect, contentRect);
    }
}

/// @}
