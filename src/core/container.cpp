/**
 * @file container.cpp
 * @brief Container 与 ListContainer 的实现
 * @details 提供容器类对 Slot（槽位）的管理，包括 Slot 的懒加载构建、
 *          内容绘制以及列表式容器的网格布局排版。
 */

#include "container.h"
#include "slot.h"
#include "item.h"
#include <algorithm>

/**
 * @brief 获取当前容器的所有槽位
 * @details 采用懒加载模式，当 slotsValid_ 标记失效时自动调用 BuildSlots()
 *          重建槽位列表并缓存。
 * @return 缓存后的槽位只读引用
 */
const std::vector<std::unique_ptr<Slot>>& Container::GetSlots()
{
    if (!slotsValid_)
    {
        cachedSlots_ = BuildSlots();
        slotsValid_ = true;
    }
    return cachedSlots_;
}

/**
 * @brief 绘制容器内所有槽位中的内容
 * @details 遍历所有槽位，获取每个槽位中的 Item 并调用其 Draw 方法进行绘制。
 *          当选中的 Item 时绘制边框（厚度为 2），否则不绘制边框（厚度为 0）。
 * @param context Direct2D 设备上下文指针，用于执行绘制操作
 */
void Container::DrawContents(ID2D1DeviceContext* context)
{
    auto& slots = GetSlots();
    for (auto& slot : slots)
    {
        if (Item* item = slot->GetItem())
        {
            bool selected = item->IsSelected();
            item->Draw(context, slot->GetBounds(), selected ? 2 : 0);
        }
    }
}

/**
 * @brief 构建列表容器的槽位网格
 * @details 根据容器边界、Item 尺寸和列数计算每个槽位的矩形区域，
 *          并按行优先顺序创建 Slot 对象。支持末尾空槽位的可选添加。
 * @return 构建完成的槽位智能指针向量
 */
std::vector<std::unique_ptr<Slot>> ListContainer::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    size_t count = GetSlotCount();
    if (count == 0 && !IncludeTrailingEmptySlot()) return slots;

    RECT bounds = GetBounds();
    // Content area = bounds minus 24px bottom chrome handle
    RECT body = {
        bounds.left + 4,
        bounds.top + 4,
        bounds.right - 4,
        std::max<LONG>(bounds.top + 28, bounds.bottom - 26)
    };
    int bodyW = std::max(1L, static_cast<LONG>(body.right - body.left));

    int itemW = GetItemWidth();
    int itemH = GetItemHeight();
    int cols = SingleColumn() ? 1 : std::max(1, bodyW / itemW);

    size_t total = IncludeTrailingEmptySlot() ? count + 1 : count;
    for (size_t idx = 0; idx < total; ++idx)
    {
        int col = static_cast<int>(idx) % cols;
        int row = static_cast<int>(idx) / cols;
        RECT cell = {
            body.left + col * itemW,
            body.top  + row * itemH,
            body.left + std::min<LONG>(col * itemW + itemW, body.right),
            body.top  + row * itemH + itemH
        };
        auto s = std::make_unique<Slot>(this, cell, idx);
        Item* item = GetSlotItem(idx);
        if (item) item->SetBounds(cell);
        s->SetItem(item);
        slots.push_back(std::move(s));
    }
    return slots;
}
