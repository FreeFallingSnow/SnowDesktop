/**
 * @file desktop.cpp
 * @brief 桌面网格容器的实现
 * @details DesktopGrid 负责管理桌面图标的网格布局，包括网格插槽的构建、拖放操作、
 *          命中测试、选中项管理以及拖放预览绘制等功能。作为桌面容器，它将桌面项
 *          组织在由 GridPage 定义的多页网格中。
 */

#include "desktop.h"
#include "slot.h"
#include "item.h"
#include "widget.h"
#include "app.h"
#include "types.h"
#include "constants.h"
#include "utils.h"
#include <algorithm>
#include <shlobj.h>
#include <shlwapi.h>
#include <unordered_set>

/**
 * @brief 构造函数，初始化桌面网格
 * @param pages 网格页面列表指针
 * @param items 桌面项列表指针
 * @param app 桌面应用指针
 * @details 计算所有页面的整体边界矩形，将 bounds_ 扩展为包含所有页面的最小矩形。
 *          每个页面的行/列数、单元格尺寸和间隙由 GridPage 结构定义。
 */
DesktopGrid::DesktopGrid(std::vector<GridPage>* pages, std::vector<DesktopItem>* items, DesktopApp* app)
    : pages_(pages), items_(items), app_(app)
{
    if (pages_ && !pages_->empty())
    {
        bounds_ = pages_->front().bounds;
        for (const auto& p : *pages_)
        {
            bounds_.left   = std::min(bounds_.left,   p.bounds.left);
            bounds_.top    = std::min(bounds_.top,    p.bounds.top);
            bounds_.right  = std::max(bounds_.right,  p.bounds.right);
            bounds_.bottom = std::max(bounds_.bottom, p.bounds.bottom);
        }
    }
}

/**
 * @brief 默认析构函数
 */
DesktopGrid::~DesktopGrid() = default;

/**
 * @brief 获取网格标题
 * @return 返回固定字符串 L"Desktop"
 */
std::wstring DesktopGrid::GetTitle() const { return L"Desktop"; }

/**
 * @brief 获取网格整体边界矩形
 * @return 包含所有页面范围的边界矩形
 */
RECT DesktopGrid::GetBounds() const { return bounds_; }

/**
 * @brief 构建所有页面的网格插槽列表
 * @return 包含所有插槽唯一指针的向量
 * @details 遍历每个 GridPage，根据页面定义的行数、列数、边距、单元格尺寸和间隙，
 *          计算每个单元格的边界矩形，并为每个单元格创建一个 Slot 对象。
 *          插槽按页面顺序和行列顺序连续编号。
 */
std::vector<std::unique_ptr<Slot>> DesktopGrid::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    if (!pages_) return slots;

    for (const auto& page : *pages_)
    {
        for (int row = 0; row < page.rows; ++row)
        {
            for (int col = 0; col < page.columns; ++col)
            {
                const int x = GetGridAxisOffset(page, col, true);
                const int y = GetGridAxisOffset(page, row, false);
                RECT cellBounds = {
                    page.workArea.left + page.marginX + x,
                    page.workArea.top  + page.marginY + y,
                    page.workArea.left + page.marginX + x + page.cellWidth,
                    page.workArea.top  + page.marginY + y + page.cellHeight
                };
                slots.push_back(std::make_unique<Slot>(this, cellBounds, slots.size()));
            }
        }
    }
    return slots;
}

/**
 * @brief 命中测试：判断指定点所在的网格区域
 * @param pt 测试点的屏幕坐标
 * @param[out] outSlot 输出参数，指向命中的插槽指针（如未命中则为 nullptr）
 * @return 命中区域类型：None（未命中任何页面）、Empty（空插槽）或 SortBefore（有图标的插槽）
 * @details 首先检查点落在哪个 GridPage 的边界内，然后根据页面行列布局计算对应的
 *          列索引和行索引，定位到具体插槽。最后检查该插槽是否被某个桌面项占据，
 *          被收集的项（collectedKeysCache_ 中）会被跳过。
 */
HitRegion DesktopGrid::HitTestAtPoint(POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    if (!pages_ || pages_->empty()) return HitRegion::None;

    const GridPage* page = nullptr;
    for (const auto& p : *pages_)
    {
        if (PtInRect(&p.bounds, pt)) { page = &p; break; }
    }
    if (!page) return HitRegion::None;

    auto axisIndex = [](const GridPage& pg, int coord, bool horizontal) -> int {
        const int count = horizontal ? pg.columns : pg.rows;
        int margin = horizontal ? pg.marginX : pg.marginY;
        int cellSize = horizontal ? pg.cellWidth : pg.cellHeight;
        int origin = horizontal ? pg.workArea.left : pg.workArea.top;
        int bestIndex = 0;
        int bestDistance = INT_MAX;
        for (int i = 0; i < count; ++i)
        {
            int center = origin + margin + GetGridAxisOffset(pg, i, horizontal) + cellSize / 2;
            int distance = std::abs(coord - center);
            if (distance < bestDistance)
            {
                bestDistance = distance;
                bestIndex = i;
            }
        }
        return bestIndex;
    };

    int col = axisIndex(*page, pt.x, true);
    int row = axisIndex(*page, pt.y, false);

    size_t idx = 0;
    for (const auto& p : *pages_)
    {
        if (p.id == page->id) { idx += col + row * p.columns; break; }
        idx += p.columns * p.rows;
    }

    auto& slots = GetSlots();
    if (idx >= slots.size()) return HitRegion::Empty;
    outSlot = slots[idx].get();

    DesktopItem* occupiedBy = nullptr;
    for (auto& item : *items_)
    {
        if (app_ && app_->collectedKeysCache_.count(ToUpperInvariant(item.layoutKey))) continue;
        if (item.gridCell.pageId == page->id &&
            item.gridCell.column == col && item.gridCell.row == row &&
            item.bounds.left < item.bounds.right && item.bounds.top < item.bounds.bottom)
        {
            occupiedBy = &item;
            break;
        }
    }

    if (!occupiedBy) return HitRegion::Empty;
    return HitRegion::SortBefore;
}

/**
 * @brief 处理项目被拖放到桌面网格上的事件
 * @param sourceItems 拖动的源项目列表
 * @param origin 拖放操作的来源容器（可为 nullptr，表示外部来源）
 * @param targetSlot 目标插槽
 * @param region 命中区域类型
 * @param mods 键盘修饰键状态
 * @details 包含两种处理路径：
 *          - Handoff 模式：将项目委托给目标图标处理（调用 DropSelectedItemsOnTarget）
 *          - 常规模式：通过 BuildDragSourceList / BuildDropPreviewList /
 *            ExecuteDropPipeline 三步完成拖放管道处理
 */
void DesktopGrid::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_) return;

    // Handoff: delegate to shell via DropSelectedItemsOnTarget
    if (region == HitRegion::Handoff)
    {
        int hit = app_->HitTestItem(app_->dragSession_.CurrentPoint());
        if (hit >= 0 && !(*items_)[hit].selected)
            app_->DropSelectedItemsOnTarget(hit);
        app_->SaveLayoutSlots();
        app_->ClearSelection();
        app_->ReloadItems();
        return;
    }

    DragSourceList sourceList = app_->BuildDragSourceList(sourceItems, origin);
    DropPreviewList preview = app_->BuildDropPreviewList(sourceList, this, targetSlot, region, mods,
        app_->dragSession_.CurrentPoint());
    app_->ExecuteDropPipeline(sourceList, preview);
}

/**
 * @brief 获取当前选中的桌面项列表
 * @return 选中项的 Item 指针向量
 * @details 遍历应用的所有项对象（itemsOO_），筛选出 DesktopIcon 类型且对应的
 *          DesktopItem 处于选中状态（selected=true）且名称非空的项。
 *          dragSourceCache_ 在此处仅用于确保其存在（作为拖放源缓存的预留）。
 */
std::vector<Item*> DesktopGrid::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!app_) return result;

    for (auto& oo : app_->GetItemsOO())
    {
        auto* icon = dynamic_cast<DesktopIcon*>(oo.get());
        if (!icon) continue;
        DesktopItem* di = icon->GetDesktopItem();
        if (di && di->selected && !di->name.empty())
            result.push_back(icon);
    }
    (void)dragSourceCache_; // items_oo_ owns the icons, no temp wrappers needed
    return result;
}

/**
 * @brief 拖放操作中的命中测试，包含 Handoff 检测
 * @param pt 测试点的屏幕坐标
 * @param[out] outSlot 输出参数，指向命中的插槽指针
 * @return 命中区域类型，可能包含 Handoff（可委托处理）
 * @details 在 HitTestAtPoint 的基础上增加 Handoff 逻辑：如果鼠标位于一个未选中
 *          图标的图标矩形区域内，则返回 HitRegion::Handoff，表示可将拖放操作
 *          委托给该目标图标处理。
 */
HitRegion DesktopGrid::HitTestDrag(POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    HitRegion region = HitTestAtPoint(pt, outSlot);
    if (region == HitRegion::SortBefore && app_)
    {
        // Check for Handoff: mouse on an unselected icon
        int hit = app_->HitTestItem(pt);
        if (hit >= 0 && !(*items_)[hit].selected)
        {
            RECT iconRect = app_->GetItemIconRect((*items_)[hit].bounds);
            RECT hf = { iconRect.left - 4, iconRect.top - 2,
                        iconRect.right + 4, iconRect.bottom + 4 };
            if (PtInRect(&hf, pt))
                region = HitRegion::Handoff;
        }
    }
    return region;
}

/**
 * @brief 获取拖放操作的提示文本
 * @param slot 目标插槽（当前未使用）
 * @param region 命中区域类型
 * @param sourceItems 拖动的源项目列表（当前未使用）
 * @param origin 拖放来源容器
 * @param mods 键盘修饰键状态（Ctrl/Alt/Shift）
 * @return 提示字符串，根据拖放类型和修饰键返回不同的中文提示
 * @details 根据以下情况生成合适的拖放提示：
 *          - Handoff 模式：提示交给目标应用处理
 *          - 外部拖放（origin 为空）：根据修饰键提示创建快捷方式/移动/复制/放置
 *          - 内部拖放：根据修饰键和最佳放置位置提示创建快捷方式/复制/移动
 */
std::wstring DesktopGrid::GetDragHint(Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems, Container* origin, int mods) const
{
    (void)slot;
    (void)sourceItems;

    if (!app_) return L"";

    bool ctrlDown = (mods & MK_CONTROL) != 0;
    bool altDown  = (mods & MK_ALT) != 0;
    bool shiftDown = (mods & MK_SHIFT) != 0;

    POINT dragPoint = app_->dragSession_.CurrentPoint();

    if (region == HitRegion::Handoff)
    {
        int hit = app_->HitTestItem(dragPoint);
        if (hit >= 0 && !(*items_)[hit].selected)
        {
            if (origin)
                return L"释放：交给「" + (*items_)[hit].name + L"」处理";
            else
                return L"释放：拖入「" + (*items_)[hit].name + L"」";
        }
    }

    // External drag — simple hint
    if (!origin)
    {
        if (altDown)   return L"释放：创建快捷方式";
        if (shiftDown) return L"释放：移动到桌面";
        if (ctrlDown)  return L"释放：复制到桌面";
        return L"释放：放置到桌面";
    }

    if (!ctrlDown && !altDown)
    {
        auto* originWidget = dynamic_cast<WidgetContainer*>(origin);
        DesktopWidget* originData = originWidget ? originWidget->GetWidgetData() : nullptr;
        if (originData &&
            originData->type == DesktopWidgetType::FileCategories &&
            originData->autoCollect)
            return L"自动收集已开启，请先关闭后再移到自由桌面";
    }

    // Internal drag
    if (altDown)  return L"释放：创建快捷方式到此空位";
    if (ctrlDown) return L"释放：复制到此空位";

    GridCell bestCell = app_->FindBestDropCell(
        app_->CellFromPoint(app_->GetDragTargetPoint(dragPoint)));

    // When dragging from a widget (not from desktop itself), the selected items
    // are not in the desktop items_ — check cell occupancy directly instead.
    if (origin != app_->GetDesktopGrid())
    {
        if (app_->IsGridAreaOccupiedByUnselected(bestCell, {1, 1}))
            return L"释放：当前位置已有图标";
        return L"释放：移动到此空位";
    }

    if (app_->BuildSelectedMove(bestCell).empty())
        return L"释放：当前位置已有图标";

    return L"释放：移动到此空位";
}

/**
 * @brief 绘制拖放预览效果
 * @param ctx D2D 设备上下文指针
 * @param slot 目标插槽
 * @param region 命中区域类型
 * @details 根据拖放操作的状态绘制预览：
 *          - Handoff 模式跳过绘制（由 RenderFrame 统一处理）
 *          - 有项目拖放时：调用 BuildDropPreviewList 构建预览列表并绘制
 *          - 无项目拖放（外部文件拖放）时：根据鼠标位置构建外部桌面预览列表并绘制
 */
void DesktopGrid::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (!app_ || !ctx) return;

    // Handoff now unified in RenderFrame — skip here
    if (region == HitRegion::Handoff) return;

    const bool hasItemDrag = app_->dragSession_.IsActive() && !app_->dragSession_.Items().empty();
    POINT dragPoint = app_->dragSession_.CurrentPoint();

    int mods = 0;
    if (hasItemDrag)
        mods = DropActionToMods(app_->dragSession_.Action());

    // 使用缓存避免每帧重建 BuildDropPreviewList（位置/动作/目标不变时复用）
    const DropPreviewList& preview = app_->GetCachedDesktopDropPreview(
        hasItemDrag,
        hasItemDrag ? app_->dragSession_.SourceList() : DragSourceList{},
        this, slot, region, mods, dragPoint);
    app_->DrawDesktopDropPreviewList(ctx, preview);
}

/**
 * @brief 绘制桌面网格的装饰层
 * @param context D2D 设备上下文指针
 * @param mousePt 当前鼠标位置（当前未使用）
 * @details 桌面网格的装饰层当前为空操作，无额外绘制内容。
 */
void DesktopGrid::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    (void)context;
    (void)mousePt;
}
