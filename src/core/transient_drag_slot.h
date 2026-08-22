#pragma once

#include "slot.h"

#include <memory>

/**
 * @brief 创建或原地更新由宿主持有的临时拖拽 Slot。
 *
 * 临时 Slot 的地址可供现有放置、提示和绘制代码继续使用；其逻辑身份由
 * DragSession 按父容器、索引和几何值保存，不再依赖本次分配得到的地址。
 */
inline Slot* BindTransientDragSlot(std::unique_ptr<Slot>& storage,
    Container* parent, RECT bounds, size_t index, Item* item = nullptr)
{
    if (!storage)
    {
        storage = std::make_unique<Slot>(parent, bounds, index,
            SlotLifetime::TransientDragTarget);
    }
    storage->RebindTransientDragTarget(parent, bounds, index, item);
    return storage.get();
}
