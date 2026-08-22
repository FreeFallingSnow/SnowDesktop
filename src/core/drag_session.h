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
#include "container.h"
#include "slot.h"

#include <cstddef>
#include <cstdint>
#include <utility>

namespace snowdesktop::drag_target
{
struct SlotFeedbackKey
{
    bool present = false;
    Container* parent = nullptr;
    RECT bounds{};
    std::size_t index = 0;
    bool hasItem = false;
    bool transient = false;

    static SlotFeedbackKey From(Slot* slot)
    {
        if (!slot) return {};
        return {
            true,
            slot->GetParent(),
            slot->GetBounds(),
            slot->GetIndex(),
            slot->GetItem() != nullptr,
            slot->IsTransientDragTarget(),
        };
    }

    bool operator==(const SlotFeedbackKey& other) const
    {
        return present == other.present &&
            parent == other.parent &&
            bounds.left == other.bounds.left &&
            bounds.top == other.bounds.top &&
            bounds.right == other.bounds.right &&
            bounds.bottom == other.bounds.bottom &&
            index == other.index &&
            hasItem == other.hasItem &&
            transient == other.transient;
    }
};
}

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

    /** @brief 自绘拖拽虚影当前是否应当显示。 */
    bool IsVisualVisible() const
    {
        return active_ && visualVisible_;
    }

    /** @brief 拖拽是否仍保留可供释放提交使用的坐标上下文。 */
    bool HasContext() const { return hasContext_; }

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
    Slot* TargetSlot() const
    {
        return TargetSlotGenerationIsCurrent()
            ? targetSlot_
            : nullptr;
    }

    /** @brief 获取目标命中区域 */
    HitRegion TargetRegion() const
    {
        return TargetSlotGenerationIsCurrent()
            ? targetRegion_
            : HitRegion::None;
    }

    /** @brief 获取当前静态场景修订版本号，用于缓存一致性判断 */
    std::uint64_t StaticSceneRevision() const { return staticSceneRevision_; }

    /** @brief 获取放置反馈修订号；纯指针位移不会改变此值。 */
    std::uint64_t PresentationRevision() const
    {
        return presentationRevision_;
    }

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
        visualVisible_ = true;
        hasContext_ = true;
        source_ = source;
        items_ = std::move(items);
        sourceList_ = std::move(sourceList);
        mouseDownPoint_ = mouseDown;
        visualMouseDownPoint_ = mouseDown;
        currentPoint_ = current;
        visualItemBounds_.clear();
        pointerAnchored_ = false;
        visualBoundsOffset_ = {};
        action_ = DropAction::Move;
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetSlotGeneration_ = 0;
        targetSlotFeedbackKey_ = {};
        targetRegion_ = HitRegion::None;
        presentationAnchorValid_ = false;
        presentationAnchorCell_ = {};
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
     * @brief 切换应用自绘虚影的可见性。
     *
     * 自拖拽进入其他应用后由 OLE 接管反馈，此时会话仍需保留源数据和
     * 坐标上下文，但 SnowDesktop 的虚影必须隐藏，直到重新 DragEnter。
     */
    bool SetVisualVisible(bool visible)
    {
        if (!active_ || visualVisible_ == visible)
            return false;
        visualVisible_ = visible;
        return true;
    }

    /** @brief 设置由应用层在真实渲染项上采集的拖拽视觉边界。 */
    void SetVisualItemBounds(std::vector<RECT> bounds)
    {
        visualItemBounds_ = std::move(bounds);
    }

    /**
     * @brief 将拖拽逻辑落点重定向到指针，并把指定视觉热点吸附到指针。
     * @param visualAnchor 拖拽开始时视觉热点的客户端坐标。
     *
     * 列表项的命中矩形包含整行文字，但拖拽态通常只绘制左侧图标。若继续
     * 保留按下点相对整行左上角的偏移，从文字区起拖时图标虚影与桌面落点
     * 都会固定偏离鼠标。该模式将逻辑落点改为真实指针，同时整体平移视觉
     * 快照，使主项目的图标热点始终位于指针处。
     */
    void AnchorToPointer(POINT visualAnchor)
    {
        pointerAnchored_ = true;
        visualBoundsOffset_ = {
            mouseDownPoint_.x - visualAnchor.x,
            mouseDownPoint_.y - visualAnchor.y
        };
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
     * @brief 拖拽组因跨屏翻页迁移时，按组原点的实际变化重设按下基准点。
     *
     * 命中坐标以拖拽组原点为锚点，因此这里不能使用任意单个项目的 bounds
     * 变化量。不同监视器的网格尺寸或单元格内边距不同时，两者并不相等。
     */
    void AdjustForGroupOriginChange(POINT previousOrigin, POINT nextOrigin)
    {
        AdjustMouseDownPoint({
            nextOrigin.x - previousOrigin.x,
            nextOrigin.y - previousOrigin.y
        });
    }

    /**
     * @brief 按当前会话的按下基准点，将拖拽组原点平移到当前指针位置。
     *
     * 跨屏翻页会通过 AdjustForGroupOriginChange 修正会话基准点；所有拖拽
     * 可视位置与桌面网格命中都必须复用这里的同一份坐标状态。
     */
    POINT ResolveTargetPoint(POINT groupOrigin, POINT current) const
    {
        if (pointerAnchored_)
            return current;
        return {
            groupOrigin.x + current.x - mouseDownPoint_.x,
            groupOrigin.y + current.y - mouseDownPoint_.y
        };
    }

    /**
     * @brief 根据拖拽开始时的视觉快照计算虚影位置。
     *
     * 翻页会迁移项目的模型 bounds。虚影不能再以迁移后的 bounds 为基准，
     * 否则跨屏时会额外叠加一次屏幕位移并留在错误的显示器上。
     */
    RECT ResolveDraggedBounds(
        size_t itemIndex, RECT fallbackBounds,
        POINT current) const
    {
        RECT base = fallbackBounds;
        if (itemIndex < visualItemBounds_.size())
        {
            const RECT snapshot = visualItemBounds_[itemIndex];
            if (snapshot.right > snapshot.left &&
                snapshot.bottom > snapshot.top)
                base = snapshot;
        }
        const LONG dx = current.x - visualMouseDownPoint_.x;
        const LONG dy = current.y - visualMouseDownPoint_.y;
        return {
            base.left + visualBoundsOffset_.x + dx,
            base.top + visualBoundsOffset_.y + dy,
            base.right + visualBoundsOffset_.x + dx,
            base.bottom + visualBoundsOffset_.y + dy
        };
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
    bool UpdateTarget(Container* targetContainer, Slot* targetSlot, HitRegion targetRegion)
    {
        const std::uint64_t nextGeneration =
            targetContainer && targetSlot &&
                !targetSlot->IsTransientDragTarget()
                ? targetContainer->GetSlotGeneration()
                : 0;
        const auto nextFeedbackKey =
            snowdesktop::drag_target::SlotFeedbackKey::From(targetSlot);
        const bool presentationChanged =
            targetContainer_ != targetContainer ||
            !(targetSlotFeedbackKey_ == nextFeedbackKey) ||
            targetRegion_ != targetRegion;
        targetContainer_ = targetContainer;
        targetSlot_ = targetSlot;
        targetSlotGeneration_ = nextGeneration;
        targetSlotFeedbackKey_ = nextFeedbackKey;
        targetRegion_ = targetRegion;
        if (presentationChanged)
            InvalidatePresentation();
        return presentationChanged;
    }

    /**
     * @brief 更新桌面放置反馈使用的请求网格。
     *
     * 鼠标命中槽使用指针位置，而桌面放置还会应用拖拽组热点偏移。
     * 两者可在不同时间跨格；请求网格必须独立进入呈现键，避免命中槽
     * 尚未变化时继续显示上一格的落点。
     */
    bool UpdatePresentationAnchor(const GridCell& cell)
    {
        if (cell.pageId.empty())
            return ClearPresentationAnchor();
        const bool changed =
            !presentationAnchorValid_ ||
            presentationAnchorCell_.pageId != cell.pageId ||
            presentationAnchorCell_.column != cell.column ||
            presentationAnchorCell_.row != cell.row;
        presentationAnchorValid_ = true;
        presentationAnchorCell_ = cell;
        if (changed)
            InvalidatePresentation();
        return changed;
    }

    /** @brief 清除非桌面目标不再使用的请求网格呈现键。 */
    bool ClearPresentationAnchor()
    {
        if (!presentationAnchorValid_)
            return false;
        presentationAnchorValid_ = false;
        presentationAnchorCell_ = {};
        InvalidatePresentation();
        return true;
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
        // A widget on the page being replaced can only recreate its logical
        // member wrappers after the turn; their new layout bounds are empty
        // because the source page is no longer visible.  Keep the drag-start
        // bounds on those transient wrappers so the existing visual snapshot
        // and hit-coordinate context remain usable until the drop completes.
        const size_t retainedBounds = std::min(
            items_.size(), visualItemBounds_.size());
        for (size_t i = 0; i < retainedBounds; ++i)
        {
            if (!items_[i])
                continue;
            const RECT currentBounds = items_[i]->GetBounds();
            const RECT retained = visualItemBounds_[i];
            const bool currentHasArea =
                currentBounds.right > currentBounds.left &&
                currentBounds.bottom > currentBounds.top;
            const bool retainedHasArea =
                retained.right > retained.left &&
                retained.bottom > retained.top;
            if (currentHasArea || !retainedHasArea)
            {
                continue;
            }
            items_[i]->SetBounds(retained);
        }
        sourceList_ = std::move(sourceList);
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetSlotGeneration_ = 0;
        targetSlotFeedbackKey_ = {};
        targetRegion_ = HitRegion::None;
        presentationAnchorValid_ = false;
        presentationAnchorCell_ = {};
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
        targetSlotGeneration_ = 0;
        targetSlotFeedbackKey_ = {};
        targetRegion_ = HitRegion::None;
        presentationAnchorValid_ = false;
        presentationAnchorCell_ = {};
        InvalidateStaticScene();
    }

    /**
     * @brief 在执行同步放置操作前结束交互态，但暂时保留放置上下文
     *
     * Shell 文件操作可能在返回前显示进度窗口并进入嵌套消息循环。
     * 此时拖拽交互必须已经结束，否则界面会继续绘制拖拽预览。
     * Items / Source / Target / CurrentPoint 会保留到 End()，供当前放置调用完成。
     */
    void DeactivateForDrop()
    {
        if (!active_) return;
        active_ = false;
        visualVisible_ = false;
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
        InvalidatePresentation();
    }

    /**
     * @brief 结束拖拽会话，将所有字段重置为初始状态
     *
     * 清空源/目标信息、项列表、坐标等，并令场景版本失效。
     */
    void End()
    {
        active_ = false;
        visualVisible_ = false;
        hasContext_ = false;
        source_ = nullptr;
        items_.clear();
        visualItemBounds_.clear();
        sourceList_ = {};
        targetContainer_ = nullptr;
        targetSlot_ = nullptr;
        targetSlotGeneration_ = 0;
        targetSlotFeedbackKey_ = {};
        targetRegion_ = HitRegion::None;
        presentationAnchorValid_ = false;
        presentationAnchorCell_ = {};
        action_ = DropAction::Move;
        mouseDownPoint_ = {};
        visualMouseDownPoint_ = {};
        currentPoint_ = {};
        pointerAnchored_ = false;
        visualBoundsOffset_ = {};
        InvalidateStaticScene();
    }

private:
    void InvalidatePresentation()
    {
        ++presentationRevision_;
        if (presentationRevision_ == 0)
            presentationRevision_ = 1;
    }

    bool TargetSlotGenerationIsCurrent() const
    {
        if (!targetSlot_)
            return true;
        if (targetSlot_->IsTransientDragTarget())
            return true;
        return targetContainer_ &&
            targetContainer_->GetSlotGeneration() ==
                targetSlotGeneration_;
    }

    bool active_ = false;                    /**< 拖拽会话是否处于激活状态 */
    bool visualVisible_ = false;             /**< 应用自绘拖拽虚影是否可见 */
    bool hasContext_ = false;                /**< 是否保留释放提交所需的拖拽上下文 */
    Container* source_ = nullptr;            /**< 拖拽源容器指针 */
    std::vector<Item*> items_;              /**< 被拖拽的 Item 指针列表 */
    DragSourceList sourceList_;              /**< 拖拽源列表 */
    POINT mouseDownPoint_{};                 /**< 鼠标按下时的屏幕坐标 */
    POINT visualMouseDownPoint_{};           /**< 虚影固定使用的原始按下坐标 */
    POINT currentPoint_{};                   /**< 鼠标当前的屏幕坐标 */
    std::vector<RECT> visualItemBounds_;     /**< 拖拽开始时的虚影边界快照 */
    bool pointerAnchored_ = false;           /**< 逻辑落点是否直接跟随真实指针 */
    POINT visualBoundsOffset_{};             /**< 将主视觉热点吸附到指针的快照平移量 */
    DropAction action_ = DropAction::Move;   /**< 当前拖拽动作类型，默认为 Move */
    Container* targetContainer_ = nullptr;   /**< 目标容器指针 */
    Slot* targetSlot_ = nullptr;             /**< 目标插槽指针 */
    std::uint64_t targetSlotGeneration_ = 0; /**< 目标插槽所属缓存代次 */
    snowdesktop::drag_target::SlotFeedbackKey targetSlotFeedbackKey_; /**< 值语义反馈键 */
    HitRegion targetRegion_ = HitRegion::None; /**< 目标命中区域类型 */
    bool presentationAnchorValid_ = false;     /**< 桌面请求网格是否参与当前呈现键 */
    GridCell presentationAnchorCell_;          /**< 应用拖拽热点偏移后的桌面请求网格 */
    std::uint64_t staticSceneRevision_ = 1;  /**< 静态场景修订版本号，用于拖拽缓存一致性判断 */
    std::uint64_t presentationRevision_ = 1; /**< 放置反馈修订号，不随纯指针移动递增 */
};
