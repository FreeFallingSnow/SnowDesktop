/**
 * @file widget_base.cpp
 * @brief Widget 基类、容器布局、滚动列表、组件 Chrome 绘制、滚动条绘制及组件工厂的实现。
 *
 * 本文件聚合了桌面组件系统中所有抽象基类的默认行为：
 * - Widget：所有组件的纯虚基类，提供标题、边界、选择状态等基本接口。
 * - WidgetContainer：容器类组件的通用布局引擎，负责网格化的 Slot 构建、
 *   帧区域计算、命中测试、拖放预览和通用 chrome 绘制。
 * - ScrollingItemWidget：支持滚动的列表/网格组件基类，提供列表项绘制、
 *   滚动偏移和插入指示器等共享逻辑。
 * - DrawScrollbarAt：被多个组件共享的滚动条绘制工具函数。
 * - CreateWidget：组件工厂函数，根据 DesktopWidgetType 创建对应的具体组件实例。
 */
#include "widget.h"
#include "types.h"
#include "constants.h"
#include "utils.h"
#include "app.h"
#include <d2d1_1.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>

using Microsoft::WRL::ComPtr;

// ── Widget base (Item only) ─────────────────────────────────

/**
 * @brief 构造 Widget 基类实例。
 * @param data 关联的桌面组件数据对象。
 * @param app  桌面应用实例指针，用于访问 D2D/DWrite 工厂等全局资源。
 */
Widget::Widget(DesktopWidget* data, DesktopApp* app)
    : data_(data), app_(app) {}

/**
 * @brief 获取组件标题
 * @return 标题字符串，无数据时返回空
 */
std::wstring Widget::GetTitle() const { return data_ ? data_->title : L""; }
/**
 * @brief 获取组件路径（Widget 基类不实现）
 * @return 空字符串
 */
std::wstring Widget::GetPath() const { return L""; }
/**
 * @brief 获取图标位图（Widget 基类不实现）
 * @return nullptr
 */
HBITMAP Widget::GetIconBitmap() const { return nullptr; }
/**
 * @brief 获取边界矩形
 * @return 组件边界，无数据时返回空矩形
 */
RECT Widget::GetBounds() const { return data_ ? data_->bounds : RECT{}; }
/**
 * @brief 设置边界矩形
 * @param bounds 新的边界矩形
 */
void Widget::SetBounds(RECT bounds) { if (data_) data_->bounds = bounds; }
/**
 * @brief 判断组件是否处于选中状态
 * @return 选中状态
 */
bool Widget::IsSelected() const { return data_ && data_->selected; }
/**
 * @brief 设置选中状态
 * @param selected 是否选中
 */
void Widget::SetSelected(bool selected) { if (data_) data_->selected = selected; }
/**
 * @brief 获取父容器指针（Widget 基类不属于容器）
 * @return nullptr
 */
Container* Widget::GetContainer() const { return nullptr; }

/**
 * @brief 绘制组件（Widget 基类为空操作，由子类覆盖）
 * @param context D2D 设备上下文
 * @param rect 绘制区域
 * @param state 绘制状态（0=普通, 1=悬停, 2=选中, 3=拖拽中）
 */
void Widget::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    (void)context;
    (void)rect;
    (void)state;
}

/**
 * @brief 创建数据对象（Widget 基类不实现 OLE 拖拽）
 * @return nullptr
 */
ComPtr<IDataObject> Widget::CreateDataObject()
{
    return nullptr;
}

float Widget::GetCellScale() const
{
    return data_ ? data_->cellScale : 1.0f;
}

int Widget::Cu(float value) const
{
    return ScaleWidgetCu(value, GetCellScale());
}

float Widget::FontCu(float value) const
{
    return ScaleWidgetFontCu(value, GetCellScale());
}

IDWriteTextFormat* Widget::GetCuTextFormat(float value, bool bold, bool centered) const
{
    if (!app_ || !app_->dwriteFactory_)
        return nullptr;
    const float size = FontCu(value);
    const int key = static_cast<int>(std::round(size * 100.0f)) |
        (bold ? 1 << 20 : 0) | (centered ? 1 << 21 : 0);
    auto found = cuTextFormatCache_.find(key);
    if (found != cuTextFormatCache_.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    app_->dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        bold ? DWRITE_FONT_WEIGHT_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
    if (!format)
        return nullptr;
    format->SetTextAlignment(centered
        ? DWRITE_TEXT_ALIGNMENT_CENTER
        : DWRITE_TEXT_ALIGNMENT_LEADING);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return cuTextFormatCache_.emplace(key, std::move(format)).first->second.Get();
}

IDWriteTextFormat* Widget::GetCuFaTextFormat(float value) const
{
    if (!app_ || !app_->dwriteFactory_)
        return nullptr;
    const float size = FontCu(value);
    const int key = static_cast<int>(std::round(size * 100.0f));
    auto found = cuFaTextFormatCache_.find(key);
    if (found != cuFaTextFormatCache_.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    format.Attach(CreateFaTextFormat(app_->dwriteFactory_.Get(), size));
    if (!format)
        return nullptr;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return cuFaTextFormatCache_.emplace(key, std::move(format)).first->second.Get();
}

// ── WidgetContainer geometry ──────────────────────────────────

/**
 * @brief WidgetContainer 构建插槽列表
 * @details 根据 GetBodyRect 返回的内容区域按行列网格化生成插槽，每个插槽通过 GetSlotItem 获取关联的 Item
 * @return 插槽列表
 */
std::vector<std::unique_ptr<Slot>> WidgetContainer::BuildSlots()
{
    slotItemCache_.clear();

    std::vector<std::unique_ptr<Slot>> slots;
    size_t count = GetSlotCount();
    if (count == 0 && !IncludeTrailingEmptySlot()) return slots;

    RECT body = GetBodyRect();
    int bodyW = std::max(1L, static_cast<LONG>(body.right - body.left));
    int itemW = GetItemWidth();
    int itemH = GetItemHeight();
    int cols = SingleColumn() ? 1 : std::max(1, bodyW / std::max(1, itemW));

    size_t total = IncludeTrailingEmptySlot() ? count + 1 : count;
    for (size_t idx = 0; idx < total; ++idx)
    {
        int col = static_cast<int>(idx) % cols;
        int row = static_cast<int>(idx) / cols;
        RECT cell = {
            body.left + col * itemW,
            body.top + row * itemH,
            body.left + std::min<LONG>(col * itemW + itemW, body.right),
            body.top + row * itemH + itemH
        };
        auto slot = std::make_unique<Slot>(this, cell, idx);
        Item* item = GetSlotItem(idx);
        if (item) item->SetBounds(cell);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }
    return slots;
}

/**
 * @brief 获取组件外框矩形（吸收半个网格间距使相邻组件视觉上保持固定间隙）
 * @return 外框边界矩形
 */
RECT WidgetContainer::GetFrameRect() const
{
    if (!data_) return {};
    RECT rect = data_->bounds;

    // Absorb half the grid gap on all four sides so widget frames have
    // consistent visual size regardless of grid position.
    if (app_ && app_->GetDesktopGrid())
    {
        const auto& pages = app_->GetDesktopGrid()->GetPages();
        const GridCell& cell = data_->gridCell;
        for (const auto& p : pages)
        {
            if (p.id == cell.pageId)
            {
                int halfGapX = std::max(Cu(2.0f), p.gapX / 2);
                int halfGapY = std::max(Cu(2.0f), p.gapY / 2);
                rect.left   -= halfGapX;
                rect.top    -= halfGapY;
                rect.right  += halfGapX;
                rect.bottom += halfGapY;
                break;
            }
        }
    }

    const int inset = Cu(4.0f);
    if (rect.right - rect.left > inset * 4 && rect.bottom - rect.top > inset * 4)
        InflateRect(&rect, -inset, -inset);
    return rect;
}

/**
 * @brief 获取内容区域矩形（去除底部操作栏区域）
 * @return 内容边界矩形
 */
RECT WidgetContainer::GetBodyRect() const
{
    RECT frame = GetFrameRect();
    if (data_->type == DesktopWidgetType::Collection && data_->gridSpan.rows <= 1)
        return frame;
    const int barReserve = Cu(22.0f);
    frame.bottom = std::max<LONG>(frame.top + barReserve, frame.bottom - barReserve);
    return frame;
}

/**
 * @brief 获取底部移动操作栏区域
 * @return 移动操作栏边界矩形
 */
RECT WidgetContainer::GetMoveHandleRect() const
{
    RECT frame = GetFrameRect();
    const int handleHeight = Cu(24.0f);
    return {
        frame.left + Cu(4.0f),
        std::max<LONG>(frame.top, frame.bottom - handleHeight - Cu(2.0f)),
        frame.right - Cu(4.0f),
        frame.bottom - Cu(2.0f)
    };
}

/**
 * @brief 获取右下角缩放手柄区域
 * @return 缩放手柄边界矩形（24x24 区域）
 */
RECT WidgetContainer::GetResizeHandleRect() const
{
    RECT handle = GetMoveHandleRect();
    const int handleWidth = Cu(24.0f);
    return {
        std::max<LONG>(handle.left, handle.right - handleWidth),
        handle.top,
        handle.right,
        handle.bottom
    };
}

/**
 * @brief 获取标题文字显示区域
 * @return 标题边界矩形
 */
RECT WidgetContainer::GetTitleRect() const
{
    RECT handle = GetMoveHandleRect();
    LONG left = handle.left + Cu(4.0f);
    const int reserved = Cu(data_->type == DesktopWidgetType::FolderMapping ? 60.0f : 26.0f);
    LONG right = std::max<LONG>(left + 1, handle.right - reserved);
    return { left, handle.top + Cu(2.0f), right, handle.bottom - Cu(2.0f) };
}

// ── Hit testing ───────────────────────────────────────────────

/**
 * @brief 判断点是否在缩放手柄区域内
 * @param pt 屏幕坐标点
 * @return 是否命中缩放手柄
 */
bool WidgetContainer::HitResizeHandle(POINT pt) const
{
    RECT r = GetResizeHandleRect();
    return PtInRect(&r, pt) != FALSE;
}

/**
 * @brief 组件级命中测试（缩放手柄 > 移动操作栏 > 内容区）
 * @param pt 屏幕坐标点
 * @return 命中的组件区域类型
 */
WidgetHit WidgetContainer::HitTestWidget(POINT pt) const
{
    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt)) return WidgetHit::None;

    if (HitResizeHandle(pt)) return WidgetHit::ResizeHandle;

    RECT move = GetMoveHandleRect();
    if (PtInRect(&move, pt)) return WidgetHit::MoveHandle;

    return WidgetHit::Content;
}

// ── Container drag virtuals ──────────────────────────────────────

/**
 * @brief 拖放命中测试 - 遍历插槽检查点坐标
 * @param pt 屏幕坐标点
 * @param outSlot [out] 命中的插槽指针
 * @return 命中区域类型
 */
HitRegion WidgetContainer::HitTestDrag(POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt)) return HitRegion::None;

    auto& slots = GetSlots();
    for (size_t i = 0; i < slots.size(); ++i)
    {
        HitRegion region = slots[i]->HitTest(pt);
        if (region != HitRegion::None)
        {
            outSlot = slots[i].get();
            if (region == HitRegion::Handoff)
            {
                Item* item = outSlot->GetItem();
                if (item && item->IsSelected())
                {
                    RECT r = outSlot->GetBounds();
                    region = (pt.y < r.top + (r.bottom - r.top) / 2)
                        ? HitRegion::SortBefore
                        : HitRegion::SortAfter;
                }
            }
            return region;
        }
    }

    // List mode: check gaps between items
    if (SingleColumn() && slots.size() >= 2)
    {
        int pad = Cu(2.0f);
        for (size_t i = 0; i + 1 < slots.size(); ++i)
        {
            RECT upper = slots[i]->GetBounds();
            RECT lower = slots[i + 1]->GetBounds();
            if (pt.y > upper.bottom && pt.y < lower.top)
            {
                int mid = upper.bottom + pad;
                if (pt.y < mid)
                {
                    outSlot = slots[i].get();
                    return HitRegion::SortAfter;
                }
                outSlot = slots[i + 1].get();
                return HitRegion::SortBefore;
            }
        }
    }

    // Mouse in frame but not on any slot — sort at end
    return HitRegion::SortAfter;
}

/**
 * @brief 获取拖放提示文本
 * @param slot 目标插槽
 * @param region 命中区域类型
 * @param sourceItems 拖拽源项目列表
 * @param origin 源容器
 * @param mods 键盘修饰键
 * @return 提示字符串
 */
std::wstring WidgetContainer::GetDragHint(Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems, Container* origin, int mods) const
{
    DropAction action = DropActionFromMods(mods, sourceItems.empty() ? DropAction::Copy : DropAction::Move);
    if (data_ && data_->type == DesktopWidgetType::FileCategories)
    {
        auto isShortcutPath = [](const std::wstring& path) {
            return _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
        };

        bool sourceHasShortcut = sourceItems.empty() && app_ && app_->externalDragActive_ &&
            app_->externalDropHasShortcut_;
        for (auto* item : sourceItems)
        {
            if (!item) continue;
            std::wstring path = item->GetPath();
            if (!path.empty() && isShortcutPath(path))
            {
                sourceHasShortcut = true;
                break;
            }
        }

        if (sourceHasShortcut)
            return L"桌面文件不支持收纳快捷方式";
        if (action == DropAction::Link)
            return L"桌面文件不支持创建快捷方式";

        if (data_->dateHeaders &&
            origin == this && (region == HitRegion::SortBefore || region == HitRegion::SortAfter))
            return L"请先关闭日期表头再进行排序";
    }

    auto actionText = [&]() -> std::wstring {
        switch (action)
        {
        case DropAction::Copy:
            return L"复制";
        case DropAction::Link:
            return L"创建快捷方式";
        case DropAction::Move:
        default:
            return L"移动";
        }
    };

    if (region == HitRegion::SortBefore || region == HitRegion::SortAfter)
    {
        if (origin == this && action == DropAction::Move)
            return L"释放：重新排序";
        return L"释放：" + actionText() + L"并插入到此处";
    }
    if (region == HitRegion::Empty)
        return L"释放：" + actionText() + L"到此处";
    if (slot)
        return slot->GetDropHint(region, sourceItems);
    return L"释放：" + actionText() + L"到此处";
}

/**
 * @brief 绘制拖放预览指示器
 * @param ctx D2D 设备上下文
 * @param slot 目标插槽
 * @param region 命中区域类型
 */
void WidgetContainer::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (!slot || !ctx) return;
    float itemPad = SingleColumn() ? static_cast<float>(Cu(2.0f)) : 0.0f;
    slot->DrawDropIndicator(ctx, region, itemPad);
}

/**
 * @brief 计算拖放插入索引位置
 * @param targetSlot 目标插槽
 * @param region 命中区域（决定插入前/后）
 * @return 插入位置的索引
 */
size_t WidgetContainer::GetDropInsertIndex(Slot* targetSlot, HitRegion region) const
{
    size_t insertAt = targetSlot ? targetSlot->GetIndex() : GetSlotCount();
    if (targetSlot && region == HitRegion::SortAfter)
        ++insertAt;
    return std::min(insertAt, GetSlotCount());
}

// ── ScrollingItemWidget shared helpers ─────────────────────────

/**
 * @brief 判断是否使用单列（列表）模式
 * @return 列表模式返回 true，网格模式返回 false
 */
bool ScrollingItemWidget::SingleColumn() const
{
    return data_ && data_->listMode;
}

/**
 * @brief 获取当前滚动偏移量（自动限制在有效范围内）
 * @return 非负滚动偏移值
 */
int ScrollingItemWidget::GetScrollOffset() const
{
    return data_ ? std::clamp(data_->scrollOffset, 0, GetMaxScrollOffset()) : 0;
}

/**
 * @brief 获取插入指示条样式
 * @return 列表模式使用水平条(HBar)，网格模式使用竖直条(VBar)
 */
BarStyle ScrollingItemWidget::GetInsertionStyle() const
{
    return data_ && data_->listMode ? BarStyle::HBar : BarStyle::VBar;
}

/**
 * @brief 绘制列表模式下的单个项目（图标+文字）
 * @param context D2D 设备上下文
 * @param cell 项目单元格区域
 * @param iconBitmap 图标位图
 * @param sysIconIndex 系统图标索引，用于位图不可用时的回退绘制
 * @param name 项目名称
 * @param selected 是否选中
 */
void ScrollingItemWidget::DrawListItem(ID2D1DeviceContext* context, RECT cell,
    HBITMAP iconBitmap, int sysIconIndex,
    const std::wstring& name, bool selected) const
{
    if (!app_ || !context || IsRectEmptyRect(cell)) return;

    bool hovered = PtInRect(&cell, app_->lastMousePoint_) != FALSE;
    if (hovered && !selected)
        app_->DrawD2DRoundedRectangle(context, cell, static_cast<float>(Cu(6.0f)),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
    if (selected)
        app_->DrawD2DRoundedRectangle(context, cell, static_cast<float>(Cu(6.0f)),
            D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.30f),
            D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.48f));

    int itemH = std::max<int>(1, cell.bottom - cell.top);
    int iconSz = std::max(1, std::min(Cu(32.0f),
        std::max(1, itemH - Cu(4.0f))));
    RECT iconRect = MakeRect(cell.left + Cu(4.0f), cell.top + (itemH - iconSz) / 2,
        cell.left + Cu(4.0f) + iconSz, cell.top + (itemH + iconSz) / 2);

    if (ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(iconBitmap))
    {
        context->DrawBitmap(bmp, app_->ToD2DRect(iconRect), 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR);
    }
    else
    {
        app_->DrawPlaceholderIcon(context, sysIconIndex, iconRect, 1.0f);
    }

    RECT textRect = MakeRect(iconRect.right + Cu(6.0f), cell.top + Cu(2.0f),
        cell.right - Cu(6.0f), cell.bottom - Cu(2.0f));
    if (textRect.right > textRect.left && !name.empty())
    {
        float tw = static_cast<float>(std::max<LONG>(1, textRect.right - textRect.left));
        float th = static_cast<float>(std::max<LONG>(1, textRect.bottom - textRect.top));
        const float layoutScale = app_->GetItemLayoutScale(cell);
        const int scaleKey = static_cast<int>(std::round(layoutScale * 1000.0f));
        std::wstring layoutKey = L"list\x1f" + name + L"\x1f" +
            std::to_wstring(textRect.right - textRect.left) + L"x" +
            std::to_wstring(textRect.bottom - textRect.top) + L"@" +
            std::to_wstring(scaleKey);
        auto layoutIt = app_->itemTextLayoutCache_.find(layoutKey);
        if (layoutIt == app_->itemTextLayoutCache_.end())
        {
            ComPtr<IDWriteTextLayout> layout;
            if (SUCCEEDED(app_->dwriteFactory_->CreateTextLayout(
                name.c_str(), static_cast<UINT32>(name.size()),
                app_->itemTextFormat_.Get(), tw, th, &layout)) && layout)
            {
                const DWRITE_TEXT_RANGE fullRange{
                    0, static_cast<UINT32>(name.size())
                };
                layout->SetFontSize(app_->itemFontSize_ * layoutScale, fullRange);
                layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
                layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
                layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
                layout->SetLineSpacing(
                    DWRITE_LINE_SPACING_METHOD_UNIFORM,
                    app_->itemFontSize_ * 7.0f / 6.0f * layoutScale,
                    app_->itemFontSize_ * 5.0f / 6.0f * layoutScale);
                layoutIt = app_->itemTextLayoutCache_.emplace(
                    std::move(layoutKey), std::move(layout)).first;
            }
        }
        if (layoutIt != app_->itemTextLayoutCache_.end())
            app_->DrawStyledItemTextLayout(
                context, layoutIt->second.Get(), layoutIt->first,
                D2D1::Point2F(
                    static_cast<float>(textRect.left),
                    static_cast<float>(textRect.top)),
                D2D1::SizeF(tw, th), layoutScale, 1.0f);
    }
}

void ScrollingItemWidget::DrawPrivacyPlaceholder(ID2D1DeviceContext* context, RECT rect,
    const std::wstring& name, bool isDir) const
{
    if (!app_ || !context || IsRectEmptyRect(rect)) return;
    (void)name;

    const std::wstring glyph = isDir ? L"" : L"";
    const std::wstring label = isDir ? L"文件夹" : L"文件";

    int w = rect.right - rect.left;
    int h = rect.bottom - rect.top;
    int iconSz = std::min(w, h) - Cu(2.0f);

    if (w > h * 2)
    {
        // List-like: icon on left, text on right
        int listIconSz = h - Cu(2.0f);
        RECT iconRect = {
            rect.left + Cu(4.0f),
            rect.top + (h - listIconSz) / 2,
            rect.left + Cu(4.0f) + listIconSz,
            rect.top + (h + listIconSz) / 2
        };

        IDWriteTextFormat* faFormat = GetCuFaTextFormat(static_cast<float>(listIconSz) * 0.88f);
        app_->DrawD2DText(context, glyph, iconRect,
            faFormat ? faFormat : (app_->faTextFormat_ ? app_->faTextFormat_.Get() : app_->listItemTextFormat_.Get()),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f));

        RECT nameRect = { iconRect.right + Cu(6.0f), rect.top + Cu(2.0f), rect.right - Cu(6.0f), rect.bottom - Cu(2.0f) };
        if (nameRect.right > nameRect.left)
        {
            IDWriteTextFormat* textFormat = GetCuTextFormat(12.0f, false, false);
            app_->DrawD2DText(context, label, nameRect,
                textFormat ? textFormat : app_->listItemTextFormat_.Get(),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f));
        }
    }
    else
    {
        // Grid-like: icon centered at top, text below
        int iconH = iconSz;
        if (iconH > h - Cu(16.0f)) iconH = h - Cu(16.0f);
        if (iconH < Cu(4.0f)) iconH = Cu(4.0f);

        RECT iconRect = {
            rect.left + (w - iconH) / 2,
            rect.top + std::max(0, (h - iconH - Cu(14.0f)) / 2),
            rect.left + (w + iconH) / 2,
            rect.top + std::max(0, (h + iconH - Cu(14.0f)) / 2)
        };

        IDWriteTextFormat* faFormat = GetCuFaTextFormat(static_cast<float>(iconH) * 0.88f);
        app_->DrawD2DText(context, glyph, iconRect,
            faFormat ? faFormat : (app_->faTextFormat_ ? app_->faTextFormat_.Get() : app_->listItemTextFormat_.Get()),
            D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.42f));

        RECT nameRect = { rect.left + Cu(4.0f), iconRect.bottom + Cu(2.0f), rect.right - Cu(4.0f), rect.bottom - Cu(2.0f) };
        if (nameRect.right > nameRect.left)
        {
            IDWriteTextFormat* textFormat = GetCuTextFormat(12.0f, false, true);
            app_->DrawD2DText(context, label, nameRect,
                textFormat ? textFormat : app_->listItemTextFormat_.Get(),
                D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f));
        }
    }
}

// ── Scrollbar helper (free function, shared by WidgetContainer and popup) ─

/**
 * @brief 共享滚动条绘制函数（被 WidgetContainer 和 Collection 弹出面板共用）
 * @param context D2D 设备上下文
 * @param body 内容区域边界
 * @param contentHeight 内容总高度
 * @param visibleHeight 可见区域高度
 * @param scrollOffset 当前滚动偏移
 * @param hovered 鼠标是否悬停在滚动区域
 */
void DrawScrollbarAt(ID2D1DeviceContext* context, RECT body, int contentHeight,
    int visibleHeight, int scrollOffset, bool hovered, float cellScale)
{
    if (contentHeight <= visibleHeight || visibleHeight <= 0) return;
    if (!hovered) return;

    int maxScroll = std::max(0, contentHeight - visibleHeight);
    if (maxScroll <= 0) return;

    const int trackWidth = std::max(2, static_cast<int>(std::round(5.0f * cellScale)));
    const int trackMargin = std::max(1, static_cast<int>(std::round(2.0f * cellScale)));
    int trackLeft = body.right - trackWidth - trackMargin;
    int trackTop = body.top + std::max(1, static_cast<int>(std::round(4.0f * cellScale)));
    int trackBottom = body.bottom - std::max(1, static_cast<int>(std::round(4.0f * cellScale)));
    int trackHeight = std::max(1, trackBottom - trackTop);

    // Track background
    RECT trackRect = MakeRect(trackLeft, trackTop, trackLeft + trackWidth, trackBottom);
    ComPtr<ID2D1SolidColorBrush> trackBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &trackBrush);
    if (trackBrush)
    {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)trackRect.left, (float)trackRect.top,
                        (float)trackRect.right, (float)trackRect.bottom),
            (float)trackWidth / 2.0f, (float)trackWidth / 2.0f);
        context->FillRoundedRectangle(rr, trackBrush.Get());
    }

    // Thumb
    float ratio = std::clamp((float)visibleHeight / (float)contentHeight, 0.08f, 1.0f);
    float scrollRatio = std::clamp((float)scrollOffset / (float)maxScroll, 0.0f, 1.0f);
    int thumbHeight = std::max(
        std::max(8, static_cast<int>(std::round(20.0f * cellScale))),
        (int)(trackHeight * ratio));
    int thumbTravel = trackHeight - thumbHeight;
    int thumbTop = trackTop + (int)(thumbTravel * scrollRatio);
    RECT thumbRect = MakeRect(trackLeft, thumbTop, trackLeft + trackWidth, thumbTop + thumbHeight);

    ComPtr<ID2D1SolidColorBrush> thumbBrush;
    context->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f), &thumbBrush);
    if (thumbBrush)
    {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)thumbRect.left, (float)thumbRect.top,
                        (float)thumbRect.right, (float)thumbRect.bottom),
            (float)trackWidth / 2.0f, (float)trackWidth / 2.0f);
        context->FillRoundedRectangle(rr, thumbBrush.Get());
    }
}

/**
 * @brief 绘制组件滚动条（通过 DrawScrollbarAt 实现）
 * @param context D2D 设备上下文
 * @param hovered 鼠标是否悬停在组件上
 */
void WidgetContainer::DrawScrollbar(ID2D1DeviceContext* context, bool hovered) const
{
    RECT body = GetBodyRect();
    DrawScrollbarAt(context, body, GetTotalContentHeight(),
        GetVisibleContentHeight(), GetScrollOffset(), hovered, GetCellScale());
}

// ── Cached clip geometry ─────────────────────────────────────

ID2D1RoundedRectangleGeometry* WidgetContainer::GetCachedClipGeometry(
    ID2D1Factory1* factory, const RECT& frame, float radius)
{
    if (!factory) return nullptr;
    if (cachedClipGeometry_ &&
        cachedClipFrame_.left == frame.left &&
        cachedClipFrame_.top == frame.top &&
        cachedClipFrame_.right == frame.right &&
        cachedClipFrame_.bottom == frame.bottom &&
        cachedClipRadius_ == radius)
        return cachedClipGeometry_.Get();

    ComPtr<ID2D1RoundedRectangleGeometry> geo;
    if (FAILED(factory->CreateRoundedRectangleGeometry(
            D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(frame.left), static_cast<float>(frame.top),
                    static_cast<float>(frame.right), static_cast<float>(frame.bottom)),
                radius, radius), &geo)) || !geo)
        return nullptr;
    cachedClipGeometry_ = std::move(geo);
    cachedClipFrame_ = frame;
    cachedClipRadius_ = radius;
    return cachedClipGeometry_.Get();
}

// ── DrawChrome ────────────────────────────────────────────────

/**
 * @brief 绘制组件装饰层（背景、边框、内容、渐变底栏、标题、缩放手柄、按钮、滚动条）
 * @param context D2D 设备上下文
 * @param mousePt 当前鼠标位置
 */
void WidgetContainer::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    if (!data_ || !context || !app_) return;

    RECT frame = GetFrameRect();
    RECT body = GetBodyRect();
    if (frame.right <= frame.left || body.bottom <= body.top) return;

    const bool selected = data_->selected;
    const bool hovered = PtInRect(&frame, mousePt) != FALSE;

    D2D1::ColorF fillColor(0.08f, 0.10f, 0.13f, 0.36f);
    D2D1::ColorF borderColor(1.0f, 1.0f, 1.0f, 0.40f);
    float gradientEndA = 0.65f;
    if (app_->settingsWindow_)
    {
        const auto& p = app_->settingsWindow_->GetPersonalization();
        fillColor = D2D1::ColorF(p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
        borderColor = D2D1::ColorF(p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetAlpha);
        gradientEndA = p.gradientEndA;
    }

    float radius = static_cast<float>(Cu(12.0f));
    float strokeW = selected ? 1.6f : 1.0f;
    D2D1::ColorF selBorder(0.39f, 0.66f, 1.0f, 0.90f);

    auto getBrush = [&](const D2D1_COLOR_F& c) -> ID2D1SolidColorBrush* {
        const auto key = D2DColorBrushKey(c);
        auto it = app_->brushCache_.find(key);
        if (it == app_->brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> b;
            if (FAILED(context->CreateSolidColorBrush(c, &b)) || !b) return nullptr;
            it = app_->brushCache_.emplace(key, std::move(b)).first;
        }
        return it->second.Get();
    };

    // ── 1. Background + border ────────────────────────────────
    {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
            radius, radius);
        if (auto* fillBrush = getBrush(fillColor))
            context->FillRoundedRectangle(rr, fillBrush);
        if (auto* strokeBrush = getBrush(selected ? selBorder : borderColor))
            context->DrawRoundedRectangle(rr, strokeBrush, strokeW);
    }

    // ── 2. Content (clipped to rounded frame via cached geometry) ──
    {
        auto* factory = app_->GetD2DFactory();
        ID2D1RoundedRectangleGeometry* clipGeo = GetCachedClipGeometry(factory, frame, radius);
        if (clipGeo)
            context->PushLayer(D2D1::LayerParameters(
                D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
                clipGeo), nullptr);

        DrawContent(context, body);

        if (clipGeo) context->PopLayer();
    }

    const bool tinyCollection = data_->type == DesktopWidgetType::Collection &&
        data_->gridSpan.columns <= 1 && data_->gridSpan.rows <= 1;
    const bool persistentBottomBar = tinyCollection ||
        data_->type == DesktopWidgetType::FileCategories ||
        data_->type == DesktopWidgetType::FolderMapping ||
        data_->type == DesktopWidgetType::Guide ||
        (data_->type == DesktopWidgetType::Collection && data_->scrollContainerMode);

    // ── 3. Gradient bottom bar (reuses cached geometry for clip) ──
    bool showGradient = persistentBottomBar || !data_->bottomBarHover || hovered;
    if (showGradient)
    {
        RECT gradRect = { frame.left, std::max<LONG>(body.top, frame.bottom - Cu(36.0f)),
                          frame.right, frame.bottom };
        if (gradRect.bottom > gradRect.top && !IsRectEmptyRect(gradRect))
        {
            ComPtr<ID2D1GradientStopCollection> stops;
            D2D1_GRADIENT_STOP sd[] = {
                { 0.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, 0.0f) },
                { 1.0f, D2D1::ColorF(fillColor.r, fillColor.g, fillColor.b, gradientEndA) },
            };
            if (SUCCEEDED(context->CreateGradientStopCollection(sd, 2, D2D1_GAMMA_2_2, D2D1_EXTEND_MODE_CLAMP, &stops)) && stops)
            {
                ComPtr<ID2D1LinearGradientBrush> brush;
                if (SUCCEEDED(context->CreateLinearGradientBrush(
                    D2D1::LinearGradientBrushProperties(
                        D2D1::Point2F(0.0f, (float)gradRect.top),
                        D2D1::Point2F(0.0f, (float)gradRect.bottom)),
                    stops.Get(), &brush)) && brush)
                {
                    auto* factory = app_->GetD2DFactory();
                    ID2D1RoundedRectangleGeometry* clipGeo = GetCachedClipGeometry(factory, frame, radius);
                    bool pushed = false;
                    if (clipGeo)
                    {
                        context->PushLayer(D2D1::LayerParameters(
                            D2D1::RectF((float)frame.left, (float)frame.top, (float)frame.right, (float)frame.bottom),
                            clipGeo), nullptr);
                        pushed = true;
                    }
                    context->FillRectangle(
                        D2D1::RectF((float)gradRect.left, (float)gradRect.top,
                                     (float)gradRect.right, (float)gradRect.bottom),
                        brush.Get());
                    if (pushed) context->PopLayer();
                }
            }
        }
    }

    // ── 4. Bottom-bar items (on top of gradient, visibility per bottomBarHover) ──
    bool showHandle = persistentBottomBar || (data_->bottomBarHover ? hovered : true);

    if (showHandle)
    {
        // Title text (with shadow)
        if (!data_->title.empty() && data_->showTitle)
        {
            RECT titleRect = GetTitleRect();
            LONG tw = titleRect.right - titleRect.left;
            LONG th = titleRect.bottom - titleRect.top;
            if (tw > 0 && th > 0 && app_->GetDWriteFactory())
            {
                auto* dwrite = app_->GetDWriteFactory();
                IDWriteTextFormat* fmt = GetCuTextFormat(13.0f, false, false);
                if (fmt)
                {
                    ComPtr<IDWriteTextLayout> layout;
                    dwrite->CreateTextLayout(data_->title.c_str(),
                        static_cast<UINT32>(data_->title.size()), fmt,
                        (float)tw, (float)th, &layout);
                    if (layout)
                    {
                        if (auto* shadowBrush = getBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f)))
                            context->DrawTextLayout(
                                D2D1::Point2F((float)titleRect.left + Cu(1.0f), (float)titleRect.top + Cu(1.0f)),
                                layout.Get(), shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);

                        if (auto* textBrush = getBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f)))
                            context->DrawTextLayout(
                                D2D1::Point2F((float)titleRect.left, (float)titleRect.top),
                                layout.Get(), textBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                    }
                }
            }
        }

        // Resize handle dot
        {
            RECT rh = GetResizeHandleRect();
            const int dot = Cu(8.0f);
            int cx = rh.left + (rh.right - rh.left) / 2;
            int cy = rh.top + (rh.bottom - rh.top) / 2;
            D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                D2D1::RectF((float)(cx - dot/2), (float)(cy - dot/2),
                             (float)(cx + dot/2), (float)(cy + dot/2)),
                static_cast<float>(Cu(4.0f)), static_cast<float>(Cu(4.0f)));
            D2D1::ColorF dotFill = selected
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.62f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f);
            D2D1::ColorF dotStroke(1.0f, 1.0f, 1.0f, 0.50f);

            if (auto* b = getBrush(dotFill))
                context->FillRoundedRectangle(pill, b);
            if (auto* b = getBrush(dotStroke))
                context->DrawRoundedRectangle(pill, b, 1.0f);
        }

        // Subclass buttons
        {
            RECT handle = GetMoveHandleRect();
            DrawButtons(context, handle, hovered);
        }
    }

    // ── Scrollbar (on top of everything, hover only) ──────────
    DrawScrollbar(context, hovered);
}

// ── Factory ─────────────────────────────────────────────────

/**
 * @brief 组件工厂函数，根据类型创建对应的具体组件实例
 * @param data 桌面组件数据
 * @param app 桌面应用实例
 * @return 组件实例的唯一指针，类型不匹配时返回 nullptr
 */
std::unique_ptr<Widget> CreateWidget(DesktopWidget* data, DesktopApp* app)
{
    if (!data) return nullptr;
    switch (data->type)
    {
    case DesktopWidgetType::Collection:
        return std::make_unique<Collection>(data, app);
    case DesktopWidgetType::FileCategories:
        return std::make_unique<FileCategories>(data, app);
    case DesktopWidgetType::FolderMapping:
        return std::make_unique<FolderMapping>(data, app);
    case DesktopWidgetType::LuaScript:
        return std::make_unique<LuaScript>(data, app);
    case DesktopWidgetType::Guide:
        return std::make_unique<GuideWidget>(data, app);
    }
    return nullptr;
}
