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
#include "collection_group_rules.h"
#include "widget_chrome_rules.h"
#include "widget_preview_scene.h"
#include "../widget_item_layout.h"
#include <d2d1_1.h>
#include <wrl/client.h>
#include "../l10n.h"


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

float Widget::GetLayoutSpacingScale() const
{
    return app_ ? app_->GetLayoutSpacingScale() : 1.0f;
}

snowdesktop::PageItemVisualMetrics Widget::GetItemVisualMetrics() const
{
    return app_
        ? app_->GetItemVisualMetrics(GetBounds())
        : snowdesktop::ResolvePageItemVisualMetrics(
            kCellWidth, kMinCellHeight, kDefaultItemFontSizeCu);
}

int Widget::Cu(float value) const
{
    return ScaleWidgetCu(value, GetCellScale());
}

float Widget::GetBarHeight() const
{
    if (app_ && app_->settingsWindow_)
        return app_->settingsWindow_->GetPersonalization().barHeight;
    return 24.0f;
}

float Widget::GetBarScale() const
{
    return GetBarHeight() / 24.0f;
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

IDWriteTextFormat* Widget::GetCuTextFormatWeight(float value, DWRITE_FONT_WEIGHT weight, bool centered) const
{
    if (!app_ || !app_->dwriteFactory_)
        return nullptr;
    const float size = FontCu(value);
    const int key = static_cast<int>(std::round(size * 100.0f)) |
        (static_cast<int>(weight) << 14) | (centered ? 1 << 25 : 0);
    auto found = cuTextFormatCache_.find(key);
    if (found != cuTextFormatCache_.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    app_->dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr,
        weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"", &format);
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

void Widget::DrawPreview(ID2D1DeviceContext* context, RECT frame,
    const snowdesktop::WidgetRenderOptions& options)
{
    const auto* previous = renderOptions_;
    renderOptions_ = &options;
    Draw(context, frame, 0);
    renderOptions_ = previous;
}

snowdesktop::WidgetPreviewScene* Widget::GetPreviewScene() const
{
    return renderOptions_ ? renderOptions_->previewScene : nullptr;
}

bool Widget::IsPreviewRendering() const
{
    return GetPreviewScene() != nullptr ||
        (renderOptions_ && !renderOptions_->registerBackdrop);
}

bool Widget::IsPreviewInteractive() const
{
    return !renderOptions_ || renderOptions_->interactive;
}

bool Widget::ShouldRegisterBackdrop() const
{
    return !renderOptions_ || renderOptions_->registerBackdrop;
}

POINT Widget::GetRenderPointer() const
{
    return renderOptions_ ? renderOptions_->pointer : POINT{};
}

IDWriteTextFormat* Widget::GetCuFluentTextFormat(float value) const
{
    if (!app_ || !app_->dwriteFactory_)
        return nullptr;
    const float size = FontCu(value);
    const int key = static_cast<int>(std::round(size * 100.0f));
    auto found = cuFluentTextFormatCache_.find(key);
    if (found != cuFluentTextFormatCache_.end())
        return found->second.Get();

    ComPtr<IDWriteTextFormat> format;
    format.Attach(CreateFluentTextFormat(
        app_->dwriteFactory_.Get(), size));
    if (!format)
        return nullptr;
    format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    return cuFluentTextFormatCache_.emplace(
        key, std::move(format)).first->second.Get();
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
    if (hostedFrameActive_)
        return hostedFrame_;
    if (app_ && app_->IsGroupedWidget(*data_))
        return {};
    return app_
        ? app_->GetStandaloneWidgetFrameRect(*data_)
        : data_->bounds;
}

snowdesktop::PageItemVisualMetrics WidgetContainer::GetItemVisualMetrics() const
{
    return app_
        ? app_->GetItemVisualMetrics(GetFrameRect())
        : Widget::GetItemVisualMetrics();
}

/**
 * @brief 获取内容区域矩形（去除底部操作栏区域）
 * @return 内容边界矩形
 */
RECT WidgetContainer::GetBodyRect() const
{
    RECT frame = GetFrameRect();
    const int barReserve = Cu(GetBarHeight() - 2.0f);
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
    const int handleHeight = Cu(GetBarHeight());
    const float cornerRadius = app_ && app_->settingsWindow_
        ? app_->settingsWindow_->GetPersonalization().cornerRadius
        : 12.0f;
    const int sideInset = snowdesktop::widget_chrome_rules::BottomBarSideInset(
        Cu(cornerRadius), handleHeight, Cu(4.0f), Cu(2.0f));
    const int maxSideInset = std::max<int>(
        0, (frame.right - frame.left - 1) / 2);
    const int clampedSideInset = std::min(sideInset, maxSideInset);
    return {
        frame.left + clampedSideInset,
        std::max<LONG>(frame.top, frame.bottom - handleHeight - Cu(2.0f)),
        frame.right - clampedSideInset,
        frame.bottom - Cu(2.0f)
    };
}

/**
 * @brief 获取右下角缩放手柄区域
 * @return 缩放手柄边界矩形
 */
RECT WidgetContainer::GetResizeHandleRect() const
{
    RECT handle = GetMoveHandleRect();
    const int handleWidth = Cu(GetBarHeight());
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
    const float barScale = GetBarScale();
    LONG left = handle.left + Cu(2.0f * barScale);
    const int reserved = snowdesktop::widget_chrome_rules::
        BottomBarTitleTrailingReserve(
            GetBottomBarButtonCount(),
            Cu(14.0f * barScale),
            Cu(4.0f * barScale),
            Cu(4.0f * barScale),
            Cu(20.0f * barScale),
            Cu(2.0f * barScale));
    LONG right = std::max<LONG>(left + 1, handle.right - reserved);
    const float bh = GetBarHeight();
    return { left, handle.top + Cu(bh * 0.083f), right, handle.bottom - Cu(bh * 0.083f) };
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
    const bool verticalBar =
        GetInsertionStyle() == BarStyle::VBar;
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
                    region = verticalBar
                        ? (pt.x < r.left + (r.right - r.left) / 2
                            ? HitRegion::SortBefore
                            : HitRegion::SortAfter)
                        : (pt.y < r.top + (r.bottom - r.top) / 2
                            ? HitRegion::SortBefore
                            : HitRegion::SortAfter);
                }
            }

            // Canonicalize the trailing half of an item to the leading half
            // of its adjacent item. Together with the explicit gap hit below,
            // all three pointer zones resolve to one insertion index and one
            // indicator boundary.
            if (region == HitRegion::SortAfter && i + 1 < slots.size() &&
                snowdesktop::widget_item_layout::SharesInsertionBoundary(
                    slots[i]->GetBounds(), slots[i + 1]->GetBounds(),
                    verticalBar))
            {
                outSlot = slots[i + 1].get();
                return HitRegion::SortBefore;
            }
            return region;
        }
    }

    // Gaps are first-class insertion targets. Map the complete gap to the
    // following slot so it shares the same result as both neighboring halves.
    if (slots.size() >= 2)
    {
        for (size_t i = 0; i + 1 < slots.size(); ++i)
        {
            RECT upper = slots[i]->GetBounds();
            RECT lower = slots[i + 1]->GetBounds();
            if (snowdesktop::widget_item_layout::PointInInsertionGap(
                    upper, lower, pt, verticalBar))
            {
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
    if (region == HitRegion::None ||
        region == HitRegion::Blocked)
        return L"";
    DropAction action = DropActionFromMods(mods, sourceItems.empty() ? DropAction::Copy : DropAction::Move);
    if (data_ && data_->type == DesktopWidgetType::FileCategories)
    {
        auto isShortcutPath = [](const std::wstring& path) {
            return _wcsicmp(PathFindExtensionW(path.c_str()), L".lnk") == 0;
        };

        bool sourceHasShortcut = sourceItems.empty() && app_ &&
            app_->dragDropController_.IsExternalDragActive() &&
            app_->dragDropController_.ExternalSummary().hasShortcut;
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
            return _LW("widget.desktop.no_shortcut");
        if (action == DropAction::Link)
            return _LW("widget.desktop.no_create_shortcut");

        if (data_->dateHeaders &&
            origin == this && (region == HitRegion::SortBefore || region == HitRegion::SortAfter))
            return _LW("widget.desktop.sort_after_date");
    }

    auto actionText = [&]() -> std::wstring {
        switch (action)
        {
        case DropAction::Copy:
            return _LW("widget.base.copy_label");
        case DropAction::Link:
            return _LW("widget.base.create_shortcut");
        case DropAction::Move:
        default:
            return _LW("widget.base.move");
        }
    };

    if (region == HitRegion::SortBefore || region == HitRegion::SortAfter)
    {
        if (origin == this && action == DropAction::Move)
            return _LW("widget.base.release_reorder");
        return _LFW("widget.base.release_insert", actionText());
    }
    if (region == HitRegion::Empty)
        return _LFW("widget.base.release_move_here", actionText());
    if (slot)
        return slot->GetDropHint(region, sourceItems);
    return _LFW("widget.base.release_move_here", actionText());
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
    const bool verticalBar =
        GetInsertionStyle() == BarStyle::VBar;
    float itemPad = 0.0f;
    auto& slots = GetSlots();
    for (size_t i = 0; i < slots.size(); ++i)
    {
        if (slots[i].get() != slot) continue;
        if (region == HitRegion::SortBefore && i > 0)
        {
            itemPad = snowdesktop::widget_item_layout::
                InsertionBoundaryPad(
                    slots[i - 1]->GetBounds(),
                    slots[i]->GetBounds(), verticalBar);
        }
        else if (region == HitRegion::SortAfter && i + 1 < slots.size())
        {
            itemPad = snowdesktop::widget_item_layout::
                InsertionBoundaryPad(
                    slots[i]->GetBounds(),
                    slots[i + 1]->GetBounds(), verticalBar);
        }
        break;
    }
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

void WidgetContainer::SetHostedFrame(const RECT* frame)
{
    hostedFrameActive_ = frame != nullptr;
    hostedFrame_ = frame ? *frame : RECT{};
    InvalidateSlots();
}

RECT ScrollingItemWidget::GetCategorizedSearchBoxRect(
    bool visible) const
{
    if (categorizedSearchVisibilityOverrideActive_)
        visible = categorizedSearchVisible_;
    if (!visible) return {};
    RECT body = GetBodyRect();
    InflateRect(&body, -Cu(10.0f), -Cu(12.0f));
    if (IsRectEmptyRect(body)) return {};
    InflateRect(&body, -Cu(2.0f), 0);
    if (IsRectEmptyRect(body)) return {};
    const LONG bottom = std::min<LONG>(
        body.bottom, body.top + Cu(GetCategorizedSearchBoxHeight()));
    return bottom > body.top
        ? MakeRect(
            body.left, body.top,
            body.right, bottom)
        : RECT{};
}

RECT ScrollingItemWidget::GetCategorizedTabsRect(
    bool visible) const
{
    if (categorizedTabsVisibilityOverrideActive_)
        visible = categorizedTabsVisible_;
    if (!visible) return {};
    RECT body = GetBodyRect();
    InflateRect(&body, -Cu(10.0f), -Cu(8.0f));
    if (IsRectEmptyRect(body)) return {};
    const RECT search = GetSearchBoxRect();
    LONG top = IsRectEmptyRect(search)
        ? body.top
        : search.bottom + Cu(5.0f);
    top += categorizedTabRowOffset_ * Cu(GetCategorizedTabRowPitch());
    const LONG bottom = std::min<LONG>(
        body.bottom, top + Cu(GetCategorizedTabHeight()));
    return bottom > top
        ? MakeRect(body.left, top, body.right, bottom)
        : RECT{};
}

void ScrollingItemWidget::SetCategorizedHostOptions(
    int tabRowOffset,
    bool searchVisibilityOverrideActive,
    bool searchVisible,
    bool tabsVisibilityOverrideActive,
    bool tabsVisible,
    bool searchAllCategories)
{
    categorizedTabRowOffset_ = std::max(0, tabRowOffset);
    categorizedSearchVisibilityOverrideActive_ =
        searchVisibilityOverrideActive;
    categorizedSearchVisible_ = searchVisible;
    categorizedTabsVisibilityOverrideActive_ =
        tabsVisibilityOverrideActive;
    categorizedTabsVisible_ = tabsVisible;
    categorizedSearchAllCategories_ = searchAllCategories;
    InvalidateSlots();
}

void ScrollingItemWidget::ClearCategorizedHostOptions()
{
    categorizedTabRowOffset_ = 0;
    categorizedSearchVisibilityOverrideActive_ = false;
    categorizedSearchVisible_ = false;
    categorizedTabsVisibilityOverrideActive_ = false;
    categorizedTabsVisible_ = false;
    categorizedSearchAllCategories_ = false;
    InvalidateSlots();
}

float ScrollingItemWidget::
    GetCategorizedTabHeight() const
{
    return app_
        ? app_->GetCategorizedWidgetTabHeight()
        : 34.0f;
}

float ScrollingItemWidget::
    GetCategorizedTabFontSize() const
{
    return snowdesktop::collection_group_rules::
        ResolveCategorizedControlMetrics(
            GetCategorizedTabHeight()).fontSizeCu;
}

float ScrollingItemWidget::
    GetCategorizedTabRowPitch() const
{
    return snowdesktop::collection_group_rules::
        ResolveCategorizedControlMetrics(
            GetCategorizedTabHeight()).rowPitchCu;
}

float ScrollingItemWidget::
    GetCategorizedSearchBoxHeight() const
{
    return snowdesktop::collection_group_rules::
        ResolveCategorizedControlMetrics(
            GetCategorizedTabHeight()).searchBoxHeightCu;
}

bool ScrollingItemWidget::
    ShowCategoryTabItemCounts() const
{
    if (app_ && app_->settingsWindow_)
        return app_->settingsWindow_->GetPersonalization().
            showCategoryTabCounts;
    return true;
}

std::vector<int>
ScrollingItemWidget::BuildCategorizedTabWidths(
    const std::vector<std::wstring>& labels,
    int availableWidth) const
{
    std::vector<int> widths;
    widths.reserve(labels.size());
    IDWriteTextFormat* format =
        GetCuTextFormat(
            GetCategorizedTabFontSize(),
            true, true);
    IDWriteFactory* dwrite =
        app_ ? app_->GetDWriteFactory() : nullptr;
    for (const auto& label : labels)
    {
        int measured = 0;
        if (dwrite && format && !label.empty())
        {
            Microsoft::WRL::ComPtr<
                IDWriteTextLayout> layout;
            if (SUCCEEDED(dwrite->CreateTextLayout(
                    label.c_str(),
                    static_cast<UINT32>(label.size()),
                    format, 4096.0f, FontCu(24.0f),
                    &layout)) &&
                layout)
            {
                DWRITE_TEXT_METRICS metrics{};
                if (SUCCEEDED(
                        layout->GetMetrics(&metrics)))
                    measured = static_cast<int>(
                        std::max(
                            metrics.width,
                            metrics.
                                widthIncludingTrailingWhitespace) +
                        1.0f);
            }
        }
        if (measured <= 0)
            measured =
                static_cast<int>(label.size()) *
                Cu(8.0f);
        const int width = std::max(
            Cu(76.0f), measured + Cu(24.0f));
        widths.push_back(width);
    }

    return snowdesktop::collection_group_rules::
        DistributeWidthsToFill(
            std::move(widths), availableWidth);
}

void ScrollingItemWidget::DrawCategorizedTab(
    ID2D1DeviceContext* context,
    RECT visibleTabRect,
    RECT layoutTabRect,
    const std::wstring& label,
    bool active,
    bool hovered) const
{
    if (!context || !app_ ||
        IsRectEmptyRect(visibleTabRect) ||
        IsRectEmptyRect(layoutTabRect))
        return;
    const bool light = app_->IsLightContentTheme();
    app_->DrawD2DRoundedRectangle(
        context, visibleTabRect,
        static_cast<float>(Cu(8.0f)),
        active
            ? (light
                ? D2D1::ColorF(
                    0.0f, 0.0f, 0.0f, 0.12f)
                : D2D1::ColorF(
                    1.0f, 1.0f, 1.0f, 0.22f))
            : (hovered
                ? (light
                    ? D2D1::ColorF(
                        0.0f, 0.0f, 0.0f, 0.08f)
                    : D2D1::ColorF(
                        1.0f, 1.0f, 1.0f, 0.13f))
                : (light
                    ? D2D1::ColorF(
                        0.0f, 0.0f, 0.0f, 0.04f)
                    : D2D1::ColorF(
                        1.0f, 1.0f, 1.0f, 0.06f))),
        active
            ? (light
                ? D2D1::ColorF(
                    0.0f, 0.0f, 0.0f, 0.52f)
                : D2D1::ColorF(
                    1.0f, 1.0f, 1.0f, 0.62f))
            : (light
                ? D2D1::ColorF(
                    0.0f, 0.0f, 0.0f, 0.14f)
                : D2D1::ColorF(
                    1.0f, 1.0f, 1.0f, 0.20f)));

    // The background follows the visible segment so its remaining corners stay
    // rounded. Text keeps the full tab geometry and is clipped by the tab strip,
    // preventing a partially visible label from being re-centered.
    RECT textRect = layoutTabRect;
    InflateRect(&textRect, -Cu(7.0f), 0);
    IDWriteTextFormat* tabFormat =
        GetCuTextFormat(
            GetCategorizedTabFontSize(),
            true, true);
    app_->DrawD2DText(
        context, label, textRect,
        tabFormat
            ? tabFormat
            : (app_->fileCategoryTabTextFormat_
                ? app_->fileCategoryTabTextFormat_.Get()
                : app_->listItemTextFormat_.Get()),
        light
            ? D2D1::ColorF(
                0.0f, 0.0f, 0.0f,
                active ? 0.88f : 0.74f)
            : D2D1::ColorF(
                1.0f, 1.0f, 1.0f,
                active ? 0.98f : 0.78f));
}

size_t ScrollingItemWidget::GetSearchSelectionStart() const
{
    return std::min(
        std::min(searchSelectionAnchor_, searchText_.size()),
        std::min(searchCursorPos_, searchText_.size()));
}

size_t ScrollingItemWidget::GetSearchSelectionEnd() const
{
    return std::max(
        std::min(searchSelectionAnchor_, searchText_.size()),
        std::min(searchCursorPos_, searchText_.size()));
}

bool ScrollingItemWidget::HasSearchSelection() const
{
    return GetSearchSelectionStart() != GetSearchSelectionEnd();
}

bool ScrollingItemWidget::EraseSearchSelection()
{
    if (!HasSearchSelection())
        return false;
    const size_t start = GetSearchSelectionStart();
    const size_t end = GetSearchSelectionEnd();
    searchText_.erase(start, end - start);
    searchCursorPos_ = start;
    searchSelectionAnchor_ = start;
    return true;
}

void ScrollingItemWidget::ReplaceSearchSelection(
    const std::wstring& text)
{
    EraseSearchSelection();
    searchCursorPos_ =
        std::min(searchCursorPos_, searchText_.size());
    searchText_.insert(searchCursorPos_, text);
    searchCursorPos_ += text.size();
    searchSelectionAnchor_ = searchCursorPos_;
    InvalidateSlots();
}

void ScrollingItemWidget::SetSearchText(
    const std::wstring& text)
{
    searchText_ = text;
    searchCursorPos_ = searchText_.size();
    searchSelectionAnchor_ = searchCursorPos_;
    searchCompositionText_.clear();
    searchCompositionCursor_ = 0;
    InvalidateSlots();
}

void ScrollingItemWidget::AppendSearchChar(wchar_t ch)
{
    searchCompositionText_.clear();
    searchCompositionCursor_ = 0;
    ReplaceSearchSelection(std::wstring(1, ch));
}

void ScrollingItemWidget::BackspaceSearchText()
{
    searchCompositionText_.clear();
    searchCompositionCursor_ = 0;
    if (!EraseSearchSelection() && searchCursorPos_ > 0)
    {
        searchText_.erase(searchCursorPos_ - 1, 1);
        --searchCursorPos_;
        searchSelectionAnchor_ = searchCursorPos_;
    }
    InvalidateSlots();
}

void ScrollingItemWidget::DeleteSearchText()
{
    searchCompositionText_.clear();
    searchCompositionCursor_ = 0;
    if (!EraseSearchSelection() &&
        searchCursorPos_ < searchText_.size())
    {
        searchText_.erase(searchCursorPos_, 1);
        searchSelectionAnchor_ = searchCursorPos_;
    }
    InvalidateSlots();
}

void ScrollingItemWidget::ClearSearchText()
{
    searchText_.clear();
    searchCursorPos_ = 0;
    searchSelectionAnchor_ = 0;
    searchCompositionText_.clear();
    searchCompositionCursor_ = 0;
    searchFocused_ = false;
    searchPointerSelecting_ = false;
    InvalidateSlots();
}

void ScrollingItemWidget::SetSearchFocused(bool focused)
{
    if (focused && !searchFocused_)
    {
        searchCursorPos_ = searchText_.size();
        searchSelectionAnchor_ = searchCursorPos_;
    }
    searchFocused_ = focused;
    if (!focused)
    {
        searchSelectionAnchor_ = searchCursorPos_;
        searchCompositionText_.clear();
        searchCompositionCursor_ = 0;
        searchPointerSelecting_ = false;
    }
}

void ScrollingItemWidget::SetSearchCursorPosition(
    size_t position)
{
    searchCursorPos_ = std::min(position, searchText_.size());
    searchSelectionAnchor_ = searchCursorPos_;
}

void ScrollingItemWidget::SetSearchEditingState(
    size_t cursor,
    size_t selectionAnchor,
    const std::wstring& compositionText,
    size_t compositionCursor)
{
    searchCursorPos_ = std::min(cursor, searchText_.size());
    searchSelectionAnchor_ =
        std::min(selectionAnchor, searchText_.size());
    searchCompositionText_ = compositionText;
    searchCompositionCursor_ =
        std::min(compositionCursor, compositionText.size());
}

void ScrollingItemWidget::MoveCursorLeft(bool extendSelection)
{
    if (!extendSelection && HasSearchSelection())
        searchCursorPos_ = GetSearchSelectionStart();
    else if (searchCursorPos_ > 0)
        --searchCursorPos_;
    if (!extendSelection)
        searchSelectionAnchor_ = searchCursorPos_;
    ClearSearchComposition();
}

void ScrollingItemWidget::MoveCursorRight(bool extendSelection)
{
    if (!extendSelection && HasSearchSelection())
        searchCursorPos_ = GetSearchSelectionEnd();
    else if (searchCursorPos_ < searchText_.size())
        ++searchCursorPos_;
    if (!extendSelection)
        searchSelectionAnchor_ = searchCursorPos_;
    ClearSearchComposition();
}

void ScrollingItemWidget::MoveCursorHome(bool extendSelection)
{
    searchCursorPos_ = 0;
    if (!extendSelection)
        searchSelectionAnchor_ = searchCursorPos_;
    ClearSearchComposition();
}

void ScrollingItemWidget::MoveCursorEnd(bool extendSelection)
{
    searchCursorPos_ = searchText_.size();
    if (!extendSelection)
        searchSelectionAnchor_ = searchCursorPos_;
    ClearSearchComposition();
}

bool ScrollingItemWidget::HandleSearchKey(WPARAM key)
{
    if (!searchFocused_)
        return false;

    const bool control =
        (GetKeyState(VK_CONTROL) & 0x8000) != 0;
    const bool shift =
        (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    searchCursorPos_ =
        std::min(searchCursorPos_, searchText_.size());
    searchSelectionAnchor_ =
        std::min(searchSelectionAnchor_, searchText_.size());
    searchPointerSelecting_ = false;

    if (key == VK_ESCAPE)
    {
        ClearSearchText();
        return true;
    }
    if (control && key == 'A')
    {
        searchSelectionAnchor_ = 0;
        searchCursorPos_ = searchText_.size();
        ClearSearchComposition();
        return true;
    }
    if (control && (key == 'C' || key == 'X'))
    {
        bool copied = false;
        if (HasSearchSelection() && OpenClipboard(nullptr))
        {
            EmptyClipboard();
            const std::wstring selected =
                searchText_.substr(
                    GetSearchSelectionStart(),
                    GetSearchSelectionEnd() -
                        GetSearchSelectionStart());
            const SIZE_T bytes =
                (selected.size() + 1) * sizeof(wchar_t);
            HGLOBAL memory =
                GlobalAlloc(GMEM_MOVEABLE, bytes);
            if (memory)
            {
                if (void* destination = GlobalLock(memory))
                {
                    memcpy(destination, selected.c_str(), bytes);
                    GlobalUnlock(memory);
                    if (SetClipboardData(
                            CF_UNICODETEXT, memory))
                        copied = true;
                    else
                        GlobalFree(memory);
                }
                else
                    GlobalFree(memory);
            }
            CloseClipboard();
        }
        if (key == 'X' && copied)
        {
            EraseSearchSelection();
            InvalidateSlots();
        }
        ClearSearchComposition();
        return true;
    }
    if (control && key == 'V')
    {
        std::wstring pasted;
        if (OpenClipboard(nullptr))
        {
            if (HANDLE data =
                    GetClipboardData(CF_UNICODETEXT))
            {
                const wchar_t* source =
                    static_cast<const wchar_t*>(
                        GlobalLock(data));
                if (source)
                {
                    for (; *source; ++source)
                    {
                        if (*source == L'\r')
                            continue;
                        if (*source == L'\n' ||
                            *source == L'\t')
                            pasted.push_back(L' ');
                        else
                            pasted.push_back(*source);
                    }
                    GlobalUnlock(data);
                }
            }
            CloseClipboard();
        }
        if (!pasted.empty())
            ReplaceSearchSelection(pasted);
        ClearSearchComposition();
        return true;
    }
    if (key == VK_BACK)
    {
        BackspaceSearchText();
        return true;
    }
    if (key == VK_DELETE)
    {
        DeleteSearchText();
        return true;
    }
    if (key == VK_LEFT)
    {
        MoveCursorLeft(shift);
        return true;
    }
    if (key == VK_RIGHT)
    {
        MoveCursorRight(shift);
        return true;
    }
    if (key == VK_HOME)
    {
        MoveCursorHome(shift);
        return true;
    }
    if (key == VK_END)
    {
        MoveCursorEnd(shift);
        return true;
    }
    return false;
}

std::wstring ScrollingItemWidget::BuildSearchDisplayText(
    size_t& displayCursor,
    size_t& compositionStart,
    size_t& compositionLength) const
{
    const size_t cursor =
        std::min(searchCursorPos_, searchText_.size());
    if (searchCompositionText_.empty())
    {
        displayCursor = cursor;
        compositionStart = 0;
        compositionLength = 0;
        return searchText_;
    }

    const size_t start = GetSearchSelectionStart();
    const size_t end = GetSearchSelectionEnd();
    std::wstring display = searchText_.substr(0, start);
    display.append(searchCompositionText_);
    display.append(searchText_.substr(end));
    displayCursor = start +
        std::min(searchCompositionCursor_,
            searchCompositionText_.size());
    compositionStart = start;
    compositionLength = searchCompositionText_.size();
    return display;
}

size_t ScrollingItemWidget::HitTestSearchTextPosition(
    POINT point) const
{
    if (!app_)
        return 0;
    RECT searchRect = GetSearchBoxRect();
    if (IsRectEmptyRect(searchRect))
        return 0;
    RECT textRect = MakeRect(
        searchRect.left + Cu(10.0f), searchRect.top,
        searchRect.right - Cu(10.0f), searchRect.bottom);
    IDWriteFactory* dwrite = app_->GetDWriteFactory();
    IDWriteTextFormat* format =
        GetCuTextFormat(15.0f, false, false);
    if (!dwrite || !format)
        return point.x <= textRect.left
            ? 0 : searchText_.size();

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(
            searchText_.c_str(),
            static_cast<UINT32>(searchText_.size()),
            format,
            static_cast<float>(std::max<LONG>(
                1, textRect.right - textRect.left)),
            static_cast<float>(std::max<LONG>(
                1, textRect.bottom - textRect.top)),
            &layout)) ||
        !layout)
        return searchText_.size();

    BOOL trailing = FALSE;
    BOOL inside = FALSE;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestPoint(
            static_cast<float>(point.x - textRect.left),
            static_cast<float>(point.y - textRect.top),
            &trailing, &inside, &metrics)))
        return searchText_.size();
    size_t position = metrics.textPosition;
    if (trailing)
        position += std::max<UINT32>(1, metrics.length);
    return std::min(position, searchText_.size());
}

void ScrollingItemWidget::BeginSearchPointerSelection(
    POINT point, bool extendSelection)
{
    SetSearchFocused(true);
    ClearSearchComposition();
    const size_t position =
        HitTestSearchTextPosition(point);
    if (!extendSelection)
        searchSelectionAnchor_ = position;
    searchCursorPos_ = position;
    searchPointerSelecting_ = true;
}

void ScrollingItemWidget::UpdateSearchPointerSelection(
    POINT point)
{
    if (!searchPointerSelecting_)
        return;
    searchCursorPos_ =
        HitTestSearchTextPosition(point);
}

void ScrollingItemWidget::EndSearchPointerSelection()
{
    searchPointerSelecting_ = false;
}

void ScrollingItemWidget::SetSearchComposition(
    const std::wstring& text, size_t cursor)
{
    if (!searchFocused_)
        return;
    searchCompositionText_ = text;
    searchCompositionCursor_ =
        std::min(cursor, text.size());
}

void ScrollingItemWidget::CommitSearchComposition(
    const std::wstring& text)
{
    if (!searchFocused_)
        return;
    ReplaceSearchSelection(text);
    searchCompositionText_.clear();
    searchCompositionCursor_ = 0;
}

void ScrollingItemWidget::ClearSearchComposition()
{
    searchCompositionText_.clear();
    searchCompositionCursor_ = 0;
}

bool ScrollingItemWidget::GetSearchCaretRect(
    RECT& rect) const
{
    rect = {};
    if (!searchFocused_ || !app_)
        return false;
    RECT searchRect = GetSearchBoxRect();
    if (IsRectEmptyRect(searchRect))
        return false;
    RECT textRect = MakeRect(
        searchRect.left + Cu(10.0f), searchRect.top,
        searchRect.right - Cu(10.0f), searchRect.bottom);
    IDWriteFactory* dwrite = app_->GetDWriteFactory();
    IDWriteTextFormat* format =
        GetCuTextFormat(GetCategorizedTabFontSize(), false, false);
    if (!dwrite || !format)
        return false;

    size_t displayCursor = 0;
    size_t compositionStart = 0;
    size_t compositionLength = 0;
    const std::wstring display = BuildSearchDisplayText(
        displayCursor, compositionStart, compositionLength);
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(
            display.c_str(),
            static_cast<UINT32>(display.size()),
            format,
            static_cast<float>(std::max<LONG>(
                1, textRect.right - textRect.left)),
            static_cast<float>(std::max<LONG>(
                1, textRect.bottom - textRect.top)),
            &layout)) ||
        !layout)
        return false;

    const size_t safeCursor =
        std::min(displayCursor, display.size());
    UINT32 hitPosition = 0;
    BOOL trailing = FALSE;
    if (!display.empty())
    {
        if (safeCursor >= display.size())
        {
            hitPosition =
                static_cast<UINT32>(display.size() - 1);
            trailing = TRUE;
        }
        else
            hitPosition =
                static_cast<UINT32>(safeCursor);
    }
    float caretX = 0.0f;
    float caretY = 0.0f;
    DWRITE_HIT_TEST_METRICS metrics{};
    if (FAILED(layout->HitTestTextPosition(
            hitPosition, trailing, &caretX, &caretY,
            &metrics)))
        return false;

    rect.left = textRect.left +
        static_cast<LONG>(std::lround(caretX));
    rect.top = textRect.top +
        static_cast<LONG>(std::lround(caretY));
    rect.right = rect.left + 1;
    rect.bottom = rect.top +
        std::max<LONG>(1,
            static_cast<LONG>(std::lround(
                std::max(metrics.height, FontCu(GetCategorizedTabFontSize())))));
    return true;
}

/**
 * @brief 绘制可滚动组件共用的搜索框。
 */
void ScrollingItemWidget::DrawSearchBox(ID2D1DeviceContext* context)
{
    if (!context || !app_) return;
    RECT searchRect = GetSearchBoxRect();
    if (IsRectEmptyRect(searchRect)) return;

    const bool light = app_->IsLightContentTheme();
    const bool hovered =
        PtInRect(&searchRect, app_->lastMousePoint_) != FALSE;
    const D2D1_COLOR_F foreground = light
        ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.90f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.97f);
    const D2D1_COLOR_F placeholder = light
        ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.52f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.64f);
    const D2D1_COLOR_F stateColor = light
        ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
    app_->DrawD2DRoundedRectangle(
        context, searchRect,
        static_cast<float>(Cu(8.0f)),
        D2D1::ColorF(
            stateColor.r, stateColor.g, stateColor.b,
            searchFocused_
                ? (light ? 0.10f : 0.12f)
                : (hovered
                    ? (light ? 0.06f : 0.08f)
                    : (light ? 0.035f : 0.05f))),
        D2D1::ColorF(
            stateColor.r, stateColor.g, stateColor.b,
            searchFocused_
                ? (light ? 0.52f : 0.62f)
                : (light ? 0.14f : 0.20f)));

    IDWriteTextFormat* format =
        GetCuTextFormat(GetCategorizedTabFontSize(), false, false);
    IDWriteFactory* dwrite = app_->GetDWriteFactory();
    if (!format || !dwrite)
        return;
    RECT textRect = MakeRect(
        searchRect.left + Cu(10.0f), searchRect.top,
        searchRect.right - Cu(10.0f), searchRect.bottom);

    size_t displayCursor = 0;
    size_t compositionStart = 0;
    size_t compositionLength = 0;
    const bool showingPlaceholder =
        searchText_.empty() && !searchFocused_;
    const std::wstring display = showingPlaceholder
        ? _LW("widget.categories.search_hint")
        : BuildSearchDisplayText(
            displayCursor,
            compositionStart,
            compositionLength);

    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(dwrite->CreateTextLayout(
            display.c_str(),
            static_cast<UINT32>(display.size()),
            format,
            static_cast<float>(std::max<LONG>(
                1, textRect.right - textRect.left)),
            static_cast<float>(std::max<LONG>(
                1, textRect.bottom - textRect.top)),
            &layout)) ||
        !layout)
        return;

    auto getBrush =
        [&](const D2D1_COLOR_F& color)
            -> ID2D1SolidColorBrush* {
        const auto key = D2DColorBrushKey(color);
        auto found = app_->brushCache_.find(key);
        if (found == app_->brushCache_.end())
        {
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(context->CreateSolidColorBrush(
                    color, &brush)) ||
                !brush)
                return nullptr;
            found = app_->brushCache_.emplace(
                key, std::move(brush)).first;
        }
        return found->second.Get();
    };

    context->PushAxisAlignedClip(
        D2D1::RectF(
            static_cast<float>(textRect.left),
            static_cast<float>(textRect.top),
            static_cast<float>(textRect.right),
            static_cast<float>(textRect.bottom)),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    if (searchFocused_ &&
        searchCompositionText_.empty() &&
        HasSearchSelection())
    {
        UINT32 count = 0;
        const UINT32 start =
            static_cast<UINT32>(GetSearchSelectionStart());
        const UINT32 length = static_cast<UINT32>(
            GetSearchSelectionEnd() -
                GetSearchSelectionStart());
        layout->HitTestTextRange(
            start, length, 0.0f, 0.0f,
            nullptr, 0, &count);
        if (count > 0)
        {
            std::vector<DWRITE_HIT_TEST_METRICS>
                metrics(count);
            if (SUCCEEDED(layout->HitTestTextRange(
                    start, length, 0.0f, 0.0f,
                    metrics.data(), count, &count)))
            {
                ID2D1SolidColorBrush* selectionBrush =
                    getBrush(D2D1::ColorF(
                        stateColor.r,
                        stateColor.g,
                        stateColor.b,
                        light ? 0.18f : 0.24f));
                if (selectionBrush)
                {
                    for (UINT32 i = 0; i < count; ++i)
                    {
                        const auto& hit = metrics[i];
                        context->FillRectangle(
                            D2D1::RectF(
                                textRect.left + hit.left,
                                textRect.top + hit.top,
                                textRect.left + hit.left +
                                    hit.width,
                                textRect.top + hit.top +
                                    hit.height),
                            selectionBrush);
                    }
                }
            }
        }
    }

    if (ID2D1SolidColorBrush* textBrush =
            getBrush(showingPlaceholder
                ? placeholder : foreground))
    {
        context->DrawTextLayout(
            D2D1::Point2F(
                static_cast<float>(textRect.left),
                static_cast<float>(textRect.top)),
            layout.Get(), textBrush,
            D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    if (searchFocused_ && compositionLength > 0)
    {
        UINT32 count = 0;
        layout->HitTestTextRange(
            static_cast<UINT32>(compositionStart),
            static_cast<UINT32>(compositionLength),
            0.0f, 0.0f, nullptr, 0, &count);
        if (count > 0)
        {
            std::vector<DWRITE_HIT_TEST_METRICS>
                metrics(count);
            if (SUCCEEDED(layout->HitTestTextRange(
                    static_cast<UINT32>(compositionStart),
                    static_cast<UINT32>(compositionLength),
                    0.0f, 0.0f,
                    metrics.data(), count, &count)))
            {
                ID2D1SolidColorBrush* underlineBrush =
                    getBrush(foreground);
                if (underlineBrush)
                {
                    for (UINT32 i = 0; i < count; ++i)
                    {
                        const auto& hit = metrics[i];
                        const float y = textRect.top +
                            hit.top +
                            std::max(1.0f,
                                hit.height - 1.5f);
                        context->FillRectangle(
                            D2D1::RectF(
                                textRect.left + hit.left,
                                y,
                                textRect.left + hit.left +
                                    std::max(1.5f, hit.width),
                                y + 1.5f),
                            underlineBrush);
                    }
                }
            }
        }
    }

    if (searchFocused_)
    {
        const size_t safeCursor =
            std::min(displayCursor, display.size());
        UINT32 hitPosition = 0;
        BOOL trailing = FALSE;
        if (!display.empty())
        {
            if (safeCursor >= display.size())
            {
                hitPosition = static_cast<UINT32>(
                    display.size() - 1);
                trailing = TRUE;
            }
            else
                hitPosition =
                    static_cast<UINT32>(safeCursor);
        }
        float caretX = 0.0f;
        float caretY = 0.0f;
        DWRITE_HIT_TEST_METRICS metrics{};
        if (SUCCEEDED(layout->HitTestTextPosition(
                hitPosition, trailing,
                &caretX, &caretY, &metrics)))
        {
            ID2D1SolidColorBrush* caretBrush =
                getBrush(foreground);
            if (caretBrush)
            {
                context->FillRectangle(
                    D2D1::RectF(
                        textRect.left + caretX,
                        textRect.top + caretY,
                        textRect.left + caretX +
                            std::max(1.0f,
                                static_cast<float>(
                                    Cu(1.5f))),
                        textRect.top + caretY +
                            std::max(
                                metrics.height,
                                FontCu(15.0f))),
                    caretBrush);
            }
        }
    }
    context->PopAxisAlignedClip();
}

int ScrollingItemWidget::GetListRowHeight() const
{
    const float currentFont = app_
        ? FontCu(app_->listItemFontSizeCu_)
        : FontCu(kDefaultItemFontSizeCu);
    const float defaultFont = FontCu(kItemFontSize);
    const int textHeight = snowdesktop::list_detail_rules::RowHeight(
        Cu(36.0f), Cu(38.0f), currentFont, defaultFont);
    const int iconHeight = app_
        ? app_->GetItemVisualMetrics(GetFrameRect()).minimumListHeight
        : Cu(38.0f);
    return std::max(textHeight, iconHeight);
}

int ScrollingItemWidget::GetDetailsHeaderHeight() const
{
    const auto metrics =
        snowdesktop::collection_group_rules::
            ResolveCategorizedControlMetrics(
                GetCategorizedTabHeight());
    return Cu(metrics.detailsHeaderHeightCu);
}

bool ScrollingItemWidget::IsDetailsVisible() const
{
    return data_ && data_->listMode &&
        snowdesktop::list_detail_rules::HasMetadataColumns(
            data_->detailShowModified,
            data_->detailShowType,
            data_->detailShowSize);
}

RECT ScrollingItemWidget::ApplyDetailsHeaderToViewport(
    RECT viewport) const
{
    if (!IsDetailsVisible() || IsRectEmptyRect(viewport))
        return viewport;
    viewport.top = std::min<LONG>(
        viewport.bottom,
        viewport.top + GetDetailsHeaderHeight());
    return viewport;
}

RECT ScrollingItemWidget::GetDetailsHeaderRectFromViewport(
    RECT viewport) const
{
    if (!IsDetailsVisible()) return {};
    const LONG bottom = viewport.top;
    const LONG top = bottom - GetDetailsHeaderHeight();
    return MakeRect(viewport.left, top, viewport.right, bottom);
}

snowdesktop::list_detail_rules::Columns
ScrollingItemWidget::GetDetailsColumns(
    int availableWidth) const
{
    return snowdesktop::list_detail_rules::BuildColumns(
        std::max(1, availableWidth),
        data_ && data_->detailShowModified,
        data_ && data_->detailShowType,
        data_ && data_->detailShowSize,
        data_ ? data_->detailModifiedPosition :
            snowdesktop::list_detail_rules::kDefaultModifiedPosition,
        data_ ? data_->detailTypePosition :
            snowdesktop::list_detail_rules::kDefaultTypePosition,
        data_ ? data_->detailSizePosition :
            snowdesktop::list_detail_rules::kDefaultSizePosition);
}

const DesktopWidget* ScrollingItemWidget::GetDetailsSortData() const
{
    return data_;
}

void ScrollingItemWidget::DrawDetailsHeader(
    ID2D1DeviceContext* context, RECT itemViewport) const
{
    if (!app_ || !context || !IsDetailsVisible()) return;
    RECT header = GetDetailsHeaderRectFromViewport(itemViewport);
    if (IsRectEmptyRect(header)) return;

    const bool light = app_->IsLightContentTheme();
    RECT separator = MakeRect(
        header.left, header.bottom - 1,
        header.right, header.bottom);
    app_->DrawD2DFilledRectangle(
        context, separator,
        light
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f),
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    const auto columns = GetDetailsColumns(
        header.right - header.left);
    LONG left = header.left;
    RECT name = MakeRect(
        left, header.top, left + columns.nameWidth, header.bottom);
    left = name.right;
    RECT modified = columns.showModified
        ? MakeRect(left, header.top,
            left + columns.modifiedWidth, header.bottom)
        : RECT{};
    left = modified.right > modified.left ? modified.right : left;
    RECT type = columns.showType
        ? MakeRect(left, header.top,
            left + columns.typeWidth, header.bottom)
        : RECT{};
    left = type.right > type.left ? type.right : left;
    RECT size = columns.showSize
        ? MakeRect(left, header.top, header.right, header.bottom)
        : RECT{};

    const D2D1_COLOR_F separatorColor = light
        ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
    const auto drawDivider = [&](const RECT& column) {
        if (IsRectEmptyRect(column)) return;
        const RECT divider = MakeRect(
            column.left, header.top + Cu(5.0f),
            column.left + 1, header.bottom - Cu(5.0f));
        app_->DrawD2DFilledRectangle(
            context, divider, separatorColor,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
    };
    drawDivider(modified);
    drawDivider(type);
    drawDivider(size);

    const DesktopWidget* sortData = GetDetailsSortData();
    const auto sorted = sortData
        ? sortData->contentSortColumn
        : snowdesktop::list_detail_rules::Column::None;
    const bool ascending = sortData
        ? sortData->contentSortAscending
        : true;
    const auto label = [&](const char* key,
                           snowdesktop::list_detail_rules::Column column) {
        std::wstring value = _LW(key);
        if (sorted == column)
            value += ascending ? L" \u25B2" : L" \u25BC";
        return value;
    };

    IDWriteTextFormat* format = GetCuTextFormatWeight(
        GetCategorizedTabFontSize(),
        DWRITE_FONT_WEIGHT_SEMI_BOLD, false);
    if (!format) format = app_->componentListTextFormat_.Get();
    const D2D1_COLOR_F color = light
        ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.72f);
    const int pad = Cu(7.0f);
    name.left += pad;
    name.right -= pad;
    app_->DrawD2DTextEllipsis(
        context,
        label("widget.details.name",
            snowdesktop::list_detail_rules::Column::Name),
        name, format, color,
        DWRITE_TEXT_ALIGNMENT_LEADING,
        DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    if (!IsRectEmptyRect(modified))
    {
        modified.left += pad;
        modified.right -= pad;
        app_->DrawD2DTextEllipsis(
            context,
            label("widget.details.modified",
                snowdesktop::list_detail_rules::Column::Modified),
            modified, format, color,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    if (!IsRectEmptyRect(type))
    {
        type.left += pad;
        type.right -= pad;
        app_->DrawD2DTextEllipsis(
            context,
            label("widget.details.type",
                snowdesktop::list_detail_rules::Column::Type),
            type, format, color,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    if (!IsRectEmptyRect(size))
    {
        size.left += pad;
        size.right -= pad;
        app_->DrawD2DTextEllipsis(
            context,
            label("widget.details.size",
                snowdesktop::list_detail_rules::Column::Size),
            size, format, color,
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
}

WidgetHit ScrollingItemWidget::HitTestDetailsHeader(
    POINT point, RECT itemViewport) const
{
    RECT header = GetDetailsHeaderRectFromViewport(itemViewport);
    if (!IsDetailsVisible() ||
        IsRectEmptyRect(header) || !PtInRect(&header, point))
        return WidgetHit::None;
    const auto columns = GetDetailsColumns(
        header.right - header.left);
    switch (snowdesktop::list_detail_rules::HitDivider(
        columns, point.x - header.left, Cu(4.0f)))
    {
    case snowdesktop::list_detail_rules::Column::Modified:
        return WidgetHit::DetailsModifiedDivider;
    case snowdesktop::list_detail_rules::Column::Type:
        return WidgetHit::DetailsTypeDivider;
    case snowdesktop::list_detail_rules::Column::Size:
        return WidgetHit::DetailsSizeDivider;
    default:
        break;
    }
    switch (snowdesktop::list_detail_rules::HitColumn(
        columns, point.x - header.left))
    {
    case snowdesktop::list_detail_rules::Column::Modified:
        return WidgetHit::DetailsModifiedHeader;
    case snowdesktop::list_detail_rules::Column::Type:
        return WidgetHit::DetailsTypeHeader;
    case snowdesktop::list_detail_rules::Column::Size:
        return WidgetHit::DetailsSizeHeader;
    default:
        return WidgetHit::DetailsNameHeader;
    }
}

static std::wstring FormatListModifiedTime(
    const std::optional<FILETIME>& value)
{
    if (!value ||
        (value->dwLowDateTime == 0 && value->dwHighDateTime == 0))
        return L"";
    FILETIME local{};
    SYSTEMTIME time{};
    if (!FileTimeToLocalFileTime(&*value, &local) ||
        !FileTimeToSystemTime(&local, &time))
        return L"";
    wchar_t date[96]{};
    wchar_t clock[64]{};
    if (!GetDateFormatEx(
            LOCALE_NAME_USER_DEFAULT, DATE_SHORTDATE,
            &time, nullptr, date,
            static_cast<int>(std::size(date)), nullptr) ||
        !GetTimeFormatEx(
            LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS,
            &time, nullptr, clock,
            static_cast<int>(std::size(clock))))
        return L"";
    return std::wstring(date) + L" " + clock;
}

static std::wstring FormatListFileSize(
    const std::optional<std::uint64_t>& value)
{
    if (!value) return L"";
    wchar_t buffer[64]{};
    return StrFormatByteSizeW(
        static_cast<LONGLONG>(*value),
        buffer, static_cast<UINT>(std::size(buffer)))
        ? std::wstring(buffer)
        : std::to_wstring(*value);
}

void ScrollingItemWidget::DrawListItemTitle(ID2D1DeviceContext* context,
    RECT cell, RECT iconRect, const std::wstring& title) const
{
    if (!app_ || !context || title.empty()) return;
    RECT textRect = MakeRect(iconRect.right + Cu(6.0f), cell.top + Cu(2.0f),
        cell.right - Cu(6.0f), cell.bottom - Cu(2.0f));
    if (textRect.right <= textRect.left) return;

    const float width = static_cast<float>(
        std::max<LONG>(1, textRect.right - textRect.left));
    const float height = static_cast<float>(
        std::max<LONG>(1, textRect.bottom - textRect.top));
    const float layoutScale = GetCellScale();
    const float fontSize = FontCu(app_->listItemFontSizeCu_);
    const int scaleKey = static_cast<int>(std::round(layoutScale * 1000.0f));
    std::wstring layoutKey = L"list\x1f" + title + L"\x1f" +
        std::to_wstring(textRect.right - textRect.left) + L"x" +
        std::to_wstring(textRect.bottom - textRect.top) + L"@" +
        std::to_wstring(scaleKey);
    auto layoutIt = app_->componentListTextLayoutCache_.find(layoutKey);
    if (layoutIt == app_->componentListTextLayoutCache_.end())
    {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(app_->dwriteFactory_->CreateTextLayout(
            title.c_str(), static_cast<UINT32>(title.size()),
            app_->componentListTextFormat_.Get(),
            width, height, &layout)) && layout)
        {
            const DWRITE_TEXT_RANGE fullRange{
                0, static_cast<UINT32>(title.size())
            };
            layout->SetFontSize(fontSize, fullRange);
            layout->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
            layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            layout->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            layout->SetLineSpacing(DWRITE_LINE_SPACING_METHOD_UNIFORM,
                fontSize * 7.0f / 6.0f,
                fontSize * 5.0f / 6.0f);
            DWRITE_TRIMMING trimming{};
            trimming.granularity = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
            ComPtr<IDWriteInlineObject> ellipsis;
            if (SUCCEEDED(app_->dwriteFactory_->
                    CreateEllipsisTrimmingSign(
                        app_->componentListTextFormat_.Get(),
                        &ellipsis)) && ellipsis)
                layout->SetTrimming(&trimming, ellipsis.Get());
            layoutIt = app_->componentListTextLayoutCache_.emplace(
                std::move(layoutKey), std::move(layout)).first;
        }
    }
    if (layoutIt != app_->componentListTextLayoutCache_.end())
        app_->DrawStyledItemTextLayout(context, layoutIt->second.Get(),
            layoutIt->first,
            D2D1::Point2F(static_cast<float>(textRect.left),
                static_cast<float>(textRect.top)),
            D2D1::SizeF(width, height), layoutScale, 1.0f,
            app_->IsLightContentTheme(), true);
}

/**
 * @brief 绘制列表模式下的单个项目（图标+文字）
 * @param context D2D 设备上下文
 * @param cell 项目单元格区域
 * @param iconBitmap 图标位图
 * @param sysIconIndex 系统图标索引，用于位图不可用时的回退绘制
 * @param name 项目名称
 * @param selected 是否选中
 * @param iconIsMediaThumbnail 当前位图是否为应保持原样的内容型缩略图
 */
void ScrollingItemWidget::DrawListItem(ID2D1DeviceContext* context, RECT cell,
    HBITMAP iconBitmap, int sysIconIndex,
    const std::wstring& name, bool selected,
    bool iconIsMediaThumbnail, std::wstring_view demoIdentity,
    const DesktopWidget* demoCollection,
    const ListItemDetails& details) const
{
    if (!app_ || !context || IsRectEmptyRect(cell)) return;

    bool hovered = PtInRect(&cell, app_->lastMousePoint_) != FALSE;
    if (hovered && !selected)
    {
        const bool lt = app_->IsLightContentTheme();
        app_->DrawD2DRoundedRectangle(context, cell, static_cast<float>(Cu(6.0f)),
            lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f),
            lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
    }
    if (selected)
        app_->DrawD2DRoundedRectangle(context, cell, static_cast<float>(Cu(6.0f)),
            D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.30f),
            D2D1::ColorF(0.78f, 0.78f, 0.78f, 0.48f));

    RECT nameCell = cell;
    const auto columns = GetDetailsColumns(
        cell.right - cell.left);
    if (IsDetailsVisible())
        nameCell.right = std::min<LONG>(
            cell.right, cell.left + columns.nameWidth);

    const auto visualMetrics = GetItemVisualMetrics();
    RECT iconRect = snowdesktop::ResolveListItemIconRect(
        nameCell, nameCell.left + Cu(4.0f), visualMetrics);

    if (!demoIdentity.empty())
    {
        if (demoCollection)
            app_->DrawDemoCollectionIdentityIcon(
                context, *demoCollection, demoIdentity, iconRect);
        else
            app_->DrawDemoIdentityIcon(context, demoIdentity, iconRect);
    }
    else if (ID2D1Bitmap1* bmp = app_->GetOrCreateD2DBitmap(
            iconBitmap,
            app_->ShouldBeautifyIconBitmap(iconIsMediaThumbnail)))
    {
        context->DrawBitmap(bmp, app_->ToD2DRect(iconRect), 1.0f,
            D2D1_INTERPOLATION_MODE_LINEAR);
    }
    else
    {
        app_->DrawPlaceholderIcon(context, sysIconIndex, iconRect, 1.0f);
    }

    const std::wstring title = demoIdentity.empty()
        ? name
        : (demoCollection
            ? app_->GetDemoCollectionIdentityTitle(
                *demoCollection, demoIdentity)
            : app_->GetDemoIdentityTitle(demoIdentity));
    DrawListItemTitle(context, nameCell, iconRect, title);

    if (!IsDetailsVisible() || !demoIdentity.empty()) return;

    LONG left = nameCell.right;
    RECT modified = columns.showModified
        ? MakeRect(left, cell.top,
            left + columns.modifiedWidth, cell.bottom)
        : RECT{};
    left = modified.right > modified.left ? modified.right : left;
    RECT type = columns.showType
        ? MakeRect(left, cell.top,
            left + columns.typeWidth, cell.bottom)
        : RECT{};
    left = type.right > type.left ? type.right : left;
    RECT size = columns.showSize
        ? MakeRect(left, cell.top, cell.right, cell.bottom)
        : RECT{};

    IDWriteTextFormat* format = GetCuTextFormatWeight(
        app_->listItemFontSizeCu_, app_->itemFontWeight_, false);
    if (!format) format = app_->componentListTextFormat_.Get();
    const bool light = app_->IsLightContentTheme();
    const D2D1_COLOR_F color = light
        ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.66f)
        : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.66f);
    const int pad = Cu(7.0f);
    if (!IsRectEmptyRect(modified))
    {
        modified.left += pad;
        modified.right -= pad;
        app_->DrawD2DTextEllipsis(
            context, FormatListModifiedTime(details.modifiedTime),
            modified, format, color,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    if (!IsRectEmptyRect(type))
    {
        type.left += pad;
        type.right -= pad;
        app_->DrawD2DTextEllipsis(
            context, details.typeName,
            type, format, color,
            DWRITE_TEXT_ALIGNMENT_LEADING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
    if (!IsRectEmptyRect(size))
    {
        size.left += pad;
        size.right -= pad;
        app_->DrawD2DTextEllipsis(
            context, details.isDirectory
                ? L"" : FormatListFileSize(details.fileSize),
            size, format, color,
            DWRITE_TEXT_ALIGNMENT_TRAILING,
            DWRITE_PARAGRAPH_ALIGNMENT_CENTER, false);
    }
}

void ScrollingItemWidget::DrawPrivacyPlaceholder(ID2D1DeviceContext* context, RECT rect,
    const std::wstring& name, bool isDir, bool showLabel) const
{
    if (!app_ || !context || IsRectEmptyRect(rect)) return;
    (void)name;

    const std::wstring label = isDir ? _LW("widget.base.folder_type") : _LW("widget.base.file_type");
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    if (width > height * 2)
    {
        const auto visualMetrics = GetItemVisualMetrics();
        RECT iconRect = snowdesktop::ResolveListItemIconRect(
            rect, rect.left + Cu(4.0f), visualMetrics);
        app_->DrawPrivacyFaIcon(context, iconRect, isDir);
        if (showLabel)
            DrawListItemTitle(context, rect, iconRect, label);
        return;
    }

    const float layoutScale = app_->GetItemLayoutScale(rect);
    const int regularLayoutThreshold = static_cast<int>(
        std::round(50.0f * layoutScale));
    if (height >= regularLayoutThreshold)
    {
        const RECT iconRect = app_->GetItemIconRect(rect);
        app_->DrawPrivacyFaIcon(context, iconRect, isDir);
        if (showLabel)
            app_->DrawItemText(context, rect, label, false, 1.0f,
                app_->IsLightContentTheme());
        return;
    }

    if (!showLabel)
    {
        const int iconSize = std::max(1, std::min(width - Cu(4.0f), height - Cu(4.0f)));
        const RECT iconRect = MakeRect(
            rect.left + (width - iconSize) / 2,
            rect.top + (height - iconSize) / 2,
            rect.left + (width + iconSize) / 2,
            rect.top + (height + iconSize) / 2);
        app_->DrawPrivacyFaIcon(context, iconRect, isDir);
        return;
    }

    const int titleHeight = std::max(1, std::min(Cu(14.0f), height / 3));
    const int iconSize = std::max(1, std::min(
        std::max(1, width - Cu(4.0f)),
        std::max(1, height - titleHeight - Cu(2.0f))));
    const int iconTop = rect.top + std::max(0,
        (height - titleHeight - iconSize - Cu(2.0f)) / 2);
    RECT iconRect = MakeRect(rect.left + (width - iconSize) / 2,
        iconTop, rect.left + (width + iconSize) / 2, iconTop + iconSize);
    app_->DrawPrivacyFaIcon(context, iconRect, isDir);

    RECT titleRect = MakeRect(rect.left + Cu(1.0f), iconRect.bottom + Cu(2.0f),
        rect.right - Cu(1.0f), rect.bottom);
    const bool lt = app_->IsLightContentTheme();
    IDWriteTextFormat* titleFormat = lt
        ? GetCuTextFormatWeight(12.0f, DWRITE_FONT_WEIGHT_LIGHT, true)
        : GetCuTextFormat(12.0f, false, true);
    app_->DrawD2DText(context, label, titleRect,
        titleFormat ? titleFormat : app_->listItemTextFormat_.Get(),
        lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.88f));
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
    int visibleHeight, int scrollOffset, bool hovered, bool lightTheme, float cellScale)
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
    context->CreateSolidColorBrush(
        lightTheme ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &trackBrush);
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
    context->CreateSolidColorBrush(
        lightTheme ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f), &thumbBrush);
    if (thumbBrush)
    {
        D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
            D2D1::RectF((float)thumbRect.left, (float)thumbRect.top,
                        (float)thumbRect.right, (float)thumbRect.bottom),
            (float)trackWidth / 2.0f, (float)trackWidth / 2.0f);
        context->FillRoundedRectangle(rr, thumbBrush.Get());
    }
}

void DrawHorizontalScrollbarAt(ID2D1DeviceContext* context, RECT body,
    int contentWidth, int visibleWidth, int scrollOffset, bool hovered,
    bool lightTheme, float cellScale)
{
    if (contentWidth <= visibleWidth || visibleWidth <= 0 || !hovered) return;

    const int maxScroll = std::max(0, contentWidth - visibleWidth);
    if (maxScroll <= 0) return;

    const int trackHeight = std::max(
        2, static_cast<int>(std::round(5.0f * cellScale)));
    const int trackMargin = std::max(
        1, static_cast<int>(std::round(2.0f * cellScale)));
    const int trackLeft = body.left + std::max(
        1, static_cast<int>(std::round(4.0f * cellScale)));
    const int trackRight = body.right - std::max(
        1, static_cast<int>(std::round(4.0f * cellScale)));
    const int trackTop = body.bottom - trackHeight - trackMargin;
    const int trackWidth = std::max(1, trackRight - trackLeft);

    ComPtr<ID2D1SolidColorBrush> trackBrush;
    context->CreateSolidColorBrush(
        lightTheme ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f)
                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f),
        &trackBrush);
    if (trackBrush)
    {
        const auto track = D2D1::RoundedRect(D2D1::RectF(
            static_cast<float>(trackLeft), static_cast<float>(trackTop),
            static_cast<float>(trackRight),
            static_cast<float>(trackTop + trackHeight)),
            static_cast<float>(trackHeight) * 0.5f,
            static_cast<float>(trackHeight) * 0.5f);
        context->FillRoundedRectangle(track, trackBrush.Get());
    }

    const float ratio = std::clamp(
        static_cast<float>(visibleWidth) / static_cast<float>(contentWidth),
        0.08f, 1.0f);
    const float scrollRatio = std::clamp(
        static_cast<float>(scrollOffset) / static_cast<float>(maxScroll),
        0.0f, 1.0f);
    const int thumbWidth = std::min(trackWidth, std::max(
        std::max(8, static_cast<int>(std::round(20.0f * cellScale))),
        static_cast<int>(trackWidth * ratio)));
    const int thumbLeft = trackLeft + static_cast<int>(
        (trackWidth - thumbWidth) * scrollRatio);

    ComPtr<ID2D1SolidColorBrush> thumbBrush;
    context->CreateSolidColorBrush(
        lightTheme ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.28f)
                   : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.35f),
        &thumbBrush);
    if (thumbBrush)
    {
        const auto thumb = D2D1::RoundedRect(D2D1::RectF(
            static_cast<float>(thumbLeft), static_cast<float>(trackTop),
            static_cast<float>(thumbLeft + thumbWidth),
            static_cast<float>(trackTop + trackHeight)),
            static_cast<float>(trackHeight) * 0.5f,
            static_cast<float>(trackHeight) * 0.5f);
        context->FillRoundedRectangle(thumb, thumbBrush.Get());
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
        GetVisibleContentHeight(), GetScrollOffset(), hovered, app_->IsLightContentTheme(), GetCellScale());
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
    const bool hovered = IsPreviewInteractive() &&
        PtInRect(&frame, mousePt) != FALSE;
    const bool fixedGuideAppearance =
        data_->type == DesktopWidgetType::Guide;
    const bool lightTheme = fixedGuideAppearance
        ? true : app_->IsLightContentTheme();

    D2D1::ColorF fillColor(0.08f, 0.10f, 0.13f, 0.36f);
    D2D1::ColorF borderColor(1.0f, 1.0f, 1.0f, 0.40f);
    float gradientEndA = 0.65f;
    float cornerRadiusCu = 12.0f;
    PersonalizationSettings guideAppearance;
    const PersonalizationSettings* appearanceOverride = nullptr;
    if (fixedGuideAppearance)
    {
        guideAppearance = PersonalizationSettings::AcrylicLightPreset();
        appearanceOverride = &guideAppearance;
        fillColor = D2D1::ColorF(
            guideAppearance.widgetBgR,
            guideAppearance.widgetBgG,
            guideAppearance.widgetBgB,
            guideAppearance.widgetAlpha);
        borderColor = D2D1::ColorF(
            guideAppearance.widgetBorderR,
            guideAppearance.widgetBorderG,
            guideAppearance.widgetBorderB,
            guideAppearance.widgetBorderAlpha);
        gradientEndA = guideAppearance.gradientEndA;
        cornerRadiusCu = guideAppearance.cornerRadius;
    }
    else if (app_->settingsWindow_)
    {
        const auto& p = app_->settingsWindow_->GetPersonalization();
        fillColor = D2D1::ColorF(p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
        borderColor = D2D1::ColorF(p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetBorderAlpha);
        gradientEndA = p.gradientEndA;
        cornerRadiusCu = p.cornerRadius;
    }

    float radius = static_cast<float>(Cu(cornerRadiusCu));
    float strokeW = selected ? 1.6f : 1.0f;

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
    app_->DrawWidgetPanelBackground(context, frame, radius, fillColor, borderColor,
        selected, strokeW, appearanceOverride, ShouldRegisterBackdrop());

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
        data_->type == DesktopWidgetType::CollectionGroup ||
        data_->type == DesktopWidgetType::FileGroup ||
        (data_->type == DesktopWidgetType::Collection && data_->scrollContainerMode);

    // ── 3. Gradient bottom bar (reuses cached geometry for clip) ──
    bool showGradient = persistentBottomBar ||
        !data_->bottomBarHover || hovered;
    if (showGradient)
    {
        RECT gradRect = { frame.left, std::max<LONG>(body.top, frame.bottom - Cu(GetBarHeight() * 1.5f)),
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
    bool showHandle = persistentBottomBar ||
        (data_->bottomBarHover ? hovered : true);

    if (showHandle)
    {
        // Title text (with shadow)
        const bool demoCollectionTitle =
            app_->ShouldUseDemoCollectionIdentity(data_);
        const std::wstring displayTitle = demoCollectionTitle
            ? app_->GetDemoCollectionCategoryTitle(*data_)
            : data_->title;
        if (!displayTitle.empty() && data_->showTitle)
        {
            RECT titleRect = GetTitleRect();
            LONG tw = titleRect.right - titleRect.left;
            LONG th = titleRect.bottom - titleRect.top;
            if (tw > 0 && th > 0 && app_->GetDWriteFactory())
            {
                auto* dwrite = app_->GetDWriteFactory();
                auto titleWeight = static_cast<DWRITE_FONT_WEIGHT>(
                    std::max<int>(100, static_cast<int>(app_->GetItemFontWeight()) - (lightTheme ? 200 : 0)));
                IDWriteTextFormat* fmt = GetCuTextFormatWeight(GetBarHeight() * 0.542f, titleWeight, false);
                if (fmt)
                {
                    ComPtr<IDWriteTextLayout> layout;
                    dwrite->CreateTextLayout(displayTitle.c_str(),
                        static_cast<UINT32>(displayTitle.size()), fmt,
                        (float)tw, (float)th, &layout);
                    if (layout)
                    {
                        if (!lightTheme)
                        {
                            if (auto* shadowBrush = getBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f)))
                                context->DrawTextLayout(
                                    D2D1::Point2F((float)titleRect.left + Cu(1.0f), (float)titleRect.top + Cu(1.0f)),
                                    layout.Get(), shadowBrush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
                        }

                        if (auto* textBrush = getBrush(lightTheme
                            ? D2D1::ColorF(0.11f, 0.13f, 0.17f, 0.82f)
                            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f)))
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
            const int dot = Cu(GetBarHeight() * 0.333f);
            int cx = rh.left + (rh.right - rh.left) / 2;
            int cy = rh.top + (rh.bottom - rh.top) / 2;
            D2D1_ROUNDED_RECT pill = D2D1::RoundedRect(
                D2D1::RectF((float)(cx - dot/2), (float)(cy - dot/2),
                             (float)(cx + dot/2), (float)(cy + dot/2)),
                static_cast<float>(Cu(4.0f * GetBarScale())), static_cast<float>(Cu(4.0f * GetBarScale())));
            D2D1::ColorF dotFill = selected
                ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.62f)
                : (lightTheme
                    ? D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.34f)
                    : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.34f));
            D2D1::ColorF dotStroke = lightTheme
                ? D2D1::ColorF(0.06f, 0.08f, 0.12f, 0.50f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.50f);

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

void WidgetContainer::DrawPreview(ID2D1DeviceContext* context, RECT frame,
    const snowdesktop::WidgetRenderOptions& options)
{
    const auto* previous = renderOptions_;
    renderOptions_ = &options;
    SetHostedFrame(&frame);
    DrawChrome(context, options.pointer);
    SetHostedFrame(nullptr);
    renderOptions_ = previous;
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
    case DesktopWidgetType::CollectionGroup:
        return std::make_unique<CollectionGroup>(data, app);
    case DesktopWidgetType::FileGroup:
        return std::make_unique<FileGroup>(data, app);
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
