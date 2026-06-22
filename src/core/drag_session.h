/**
 * @file drag_session.h
 * @brief 拖拽会话管理
 *
 * DragSession 类管理一次拖拽操作的完整生命周期，包括：
 * 拖拽源容器、被拖拽项、鼠标位置、目标容器/插槽，以及用于缓存失效的场景修订号。
 * 通过该会话对象，拖拽过程中的状态查询与更新可统一管理。
 */

#pragma once

#include "drop_model.h"

#include <cstdint>
#include <utility>

/**
 * @class DragSession
 * @brief 管理拖拽操作的活动状态与相关信息
 *
 * 维护拖拽会话中的源容器、被拖拽项列表、鼠标按下/当前位置、
 * 当前拖拽动作类型、目标容器/插槽/区域，以及用于缓存失效的静态场景修订号。
 * 拖拽开始时创建会话，拖拽过程中更新状态，拖拽结束后重置。
 */
class DragSession
{
public:
    /** @brief 判断当前是否处于拖拽激活状态 */
    bool IsActive() const { return active_; }

    /** @brief 获取拖拽源容器指针 */
    Container* Source() const { return source_; }

    /** @brief 获取被拖拽项的常量引用列表 */
    const std::vector<Item*>& Items() const { return items_; }

    /** @brief 获取被拖拽项的可变引用列表 */
    std::vector<Item*>& Items() { return items_; }

    /** @brief 获取拖拽源列表（常量引用） */
    const DragSourceList& SourceList() const { return sourceList_; }

    /** @brief 获取拖拽源列表（可变引用） */
    DragSourceList& SourceList() { return sourceList_; }

    /** @brief 获取鼠标按下时的屏幕坐标 */
    POINT MouseDownPoint() const { return mouseDownPoint_; }

    /** @brief 获取鼠标当前所在的屏幕坐标 */
    POINT CurrentPoint() const { return currentPoint_; }

    /** @brief 获取当前拖拽动作类型（如移动、复制等） */
    DropAction Action() const { return action_; }

    /** @brief 判断当前动作是否为移动操作 */
    bool IsMoveAction() const { return action_ == DropAction::Move; }

    /** @brief 获取目标容器指针 */
    Container* TargetContainer() const { return targetContainer_; }

    /** @brief 获取目标插槽指针 */
    Slot* TargetSlot() const { return targetSlot_; }

    /** @brief 获取目标命中区域 */
    HitRegion TargetRegion() const { return targetRegion_; }

    /** @brief 获取当前静态场景修订版本号，用于缓存一致性判断 */
    std::uint64_t StaticSceneRevision() const { return staticSceneRevision_; }

    /**
     * @brief 开始一次新的拖拽会话
     * @param source      拖拽源容器指针
     * @param items       被拖拽的 Item 列表
     * @param sourceList  拖拽源列表
     * @param mouseDown   鼠标按下时的坐标
     * @param current     鼠标当前坐标
     *
     * 将会话标记为激活状态，初始化所有字段，并调用 InvalidateStaticScene() 刷新场景版本号。
     */
    void Begin(Container* source, std::vector<Item*> items, DragSourceList sourceList,
        POINT mouseDown, POINT current)
    {
        active_ = true;
        source_ = source;
        items_ = std::move(items);
        sourceList_ = std::move(sourceList);
        mouseDownPoint_ = mouseDown;
        currentPoint_ = current;
        action_ = DropAction::Move;
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetRegion_ = HitRegion::None;
        InvalidateStaticScene();
    }

    /**
     * @brief 更新鼠标当前位置
     * @param current 鼠标当前坐标
     */
    void UpdatePoint(POINT current)
    {
        currentPoint_ = current;
    }

    /**
     * @brief 平移鼠标按下基准点（用于跨页迁移后保持视觉连续性）。
     * @param delta 基准点平移量。
     */
    void AdjustMouseDownPoint(POINT delta)
    {
        mouseDownPoint_.x += delta.x;
        mouseDownPoint_.y += delta.y;
    }

    /**
     * @brief 根据修饰键状态更新拖拽动作
     * @param mods           修饰键掩码
     * @param defaultAction  默认动作，默认为 Move
     * @return 动作是否发生变更
     *
     * 调用 DropActionFromMods 计算新的动作类型，若与当前不同则更新并令场景版本失效。
     */
    bool UpdateActionFromMods(int mods, DropAction defaultAction = DropAction::Move)
    {
        DropAction next = DropActionFromMods(mods, defaultAction);
        if (next == action_) return false;
        action_ = next;
        InvalidateStaticScene();
        return true;
    }

    /**
     * @brief 更新拖拽目标和命中区域
     * @param targetContainer 目标容器指针
     * @param targetSlot      目标插槽指针
     * @param targetRegion    命中区域类型
     */
    void UpdateTarget(Container* targetContainer, Slot* targetSlot, HitRegion targetRegion)
    {
        targetContainer_ = targetContainer;
        targetSlot_ = targetSlot;
        targetRegion_ = targetRegion;
    }

    /**
     * @brief 重新绑定拖拽源（用于跨容器拖拽时的源切换等场景）
     * @param source      新的源容器指针
     * @param items       新的被拖拽项列表
     * @param sourceList  新的源列表
     *
     * 同时重置目标信息，并令场景版本失效。
     */
    void RebindSource(Container* source, std::vector<Item*> items, DragSourceList sourceList)
    {
        source_ = source;
        items_ = std::move(items);
        sourceList_ = std::move(sourceList);
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetRegion_ = HitRegion::None;
        InvalidateStaticScene();
    }

    /**
     * @brief 在运行时对象重建前解除所有裸指针绑定
     *
     * 保留拖拽动作、坐标以及 DragSourceList 中可用于重建来源的稳定元数据，
     * 但清除 Container / Item / Slot 等随对象树重建而失效的运行时指针。
     */
    void DetachRuntimeBindings()
    {
        if (!active_) return;

        source_ = nullptr;
        items_.clear();
        sourceList_.origin = nullptr;
        for (auto& entry : sourceList_.entries)
            entry.item = nullptr;
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetRegion_ = HitRegion::None;
        InvalidateStaticScene();
    }

    /**
     * @brief 使当前静态场景版本号失效（递增版本号）
     *
     * 版本号递增后若归零，则重置为 1，确保版本号始终为正数。
     */
    void InvalidateStaticScene()
    {
        ++staticSceneRevision_;
        if (staticSceneRevision_ == 0)
            staticSceneRevision_ = 1;
    }

    /**
     * @brief 结束拖拽会话，将所有字段重置为初始状态
     *
     * 清空源/目标信息、项列表、坐标等，并令场景版本失效。
     */
    void End()
    {
        active_ = false;
        source_ = nullptr;
        items_.clear();
        sourceList_ = {};
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetRegion_ = HitRegion::None;
        action_ = DropAction::Move;
        mouseDownPoint_ = {};
        currentPoint_ = {};
        InvalidateStaticScene();
    }

private:
    bool active_ = false;                    /**< 拖拽会话是否处于激活状态 */
    Container* source_ = nullptr;            /**< 拖拽源容器指针 */
    std::vector<Item*> items_;              /**< 被拖拽的 Item 指针列表 */
    DragSourceList sourceList_;              /**< 拖拽源列表 */
    POINT mouseDownPoint_{};                 /**< 鼠标按下时的屏幕坐标 */
    POINT currentPoint_{};                   /**< 鼠标当前的屏幕坐标 */
    DropAction action_ = DropAction::Move;   /**< 当前拖拽动作类型，默认为 Move */
    Container* targetContainer_ = nullptr;   /**< 目标容器指针 */
    Slot* targetSlot_ = nullptr;             /**< 目标插槽指针 */
    HitRegion targetRegion_ = HitRegion::None; /**< 目标命中区域类型 */
    std::uint64_t staticSceneRevision_ = 1;  /**< 静态场景修订版本号，用于拖拽缓存一致性判断 */
};
