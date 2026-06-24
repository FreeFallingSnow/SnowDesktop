/**
 * @file slot.h
 * @brief Slot（槽位）类定义
 *
 * Slot 表示容器网格/列表中的单个位置，负责管理该位置上持有的 Item 指针、
 * 位置边界信息以及拖放命中测试。每个 Slot 在父 Container 调用
 * InvalidateSlots 之前保持有效。
 *
 * HitRegion 枚举定义了拖放操作的目标区域类型，用于精确描述
 * 插入、排序或移交等不同拖放行为。
 */

#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <string>
#include <vector>

class Container;
class Item;

/**
 * @brief 拖放命中区域枚举
 *
 * 当鼠标拖拽到 Slot 上时，HitRegion 描述了落点所在的逻辑区域，
 * 决定了后续执行何种拖放操作。
 */
enum class HitRegion
{
    None,       ///< 不在任何有效区域内，不执行操作
    Empty,      ///< 槽位为空，将拖拽项移动到此空位（"移动到此空位"）
    SortBefore, ///< 插入到当前槽位之前（上半部分/左侧），用于排序插入
    SortAfter,  ///< 插入到当前槽位之后（下半部分/右侧），用于排序插入
    Handoff,    ///< 鼠标位于图标矩形上，将拖拽项移交给当前槽位的持有者处理（"交给xx处理"）
};

/**
 * @brief 槽位，容器网格/列表中的单个位置
 *
 * Slot 是 Container 管理子项的基本单元，每个 Slot 持有指向一个 Item 的指针、
 * 在父容器中的索引以及屏幕上的边界矩形。Slot 负责：
 *   - 管理 Item 的挂载与卸载
 *   - 响应拖放命中测试，确定操作类型
 *   - 绘制拖放指示器，提供视觉反馈
 *
 * @note Slot 实例由父 Container 管理，在 Container::InvalidateSlots()
 *       调用之前保持有效。InvalidateSlots 之后所有 Slot 指针即失效，不应继续使用。
 */
class Slot
{
public:
    /**
     * @brief 构造函数
     * @param parent 所属的父 Container 指针
     * @param bounds 槽位在屏幕上的边界矩形（像素坐标）
     * @param index  当前槽位在父容器子槽位列表中的索引
     */
    Slot(Container* parent, RECT bounds, size_t index);

    /**
     * @brief 获取所属父容器
     * @return 父 Container 指针
     */
    Container* GetParent() const;

    /**
     * @brief 获取槽位边界矩形
     * @return 边界矩形（RECT 结构，屏幕像素坐标）
     */
    RECT GetBounds() const;

    /**
     * @brief 获取当前槽位持有的 Item
     * @return Item 指针，若槽位为空则返回 nullptr
     */
    Item* GetItem() const;

    /**
     * @brief 设置当前槽位持有的 Item
     * @param item 要设置的 Item 指针，传入 nullptr 表示清空槽位
     */
    void SetItem(Item* item);

    /**
     * @brief 获取当前槽位在父容器中的索引
     * @return 从 0 开始的索引值
     */
    size_t GetIndex() const;

    /**
     * @brief 检查槽位是否为空
     * @return true 表示槽位为空（无 Item）；false 表示已有 Item
     */
    bool IsEmpty() const;

    /**
     * @brief 获取 Item 图标的显示矩形
     * @return 图标矩形（RECT 结构），通常位于槽位内部，用于 HitTest 判断
     *         鼠标是否落在 "Handoff" 区域
     */
    RECT GetIconRect() const;

    // ── 拖放目标（替代原有的 DropZone 层级） ──

    /**
     * @brief 命中测试，判断拖拽落点属于哪个区域
     * @param pt 鼠标当前坐标（屏幕像素坐标）
     * @return HitRegion 枚举值，指示落点区域类型
     *
     * 根据鼠标位置相对于槽位边界和图标矩形的位置，判断属于
     * Empty / SortBefore / SortAfter / Handoff 中的哪一种操作。
     */
    HitRegion HitTest(POINT pt) const;

    /**
     * @brief 获取拖放操作的文字提示
     * @param region      命中区域类型
     * @param sourceItems 正在被拖拽的源 Item 列表
     * @return 本地化提示字符串（如 "移动到此空位"、"交给 xx 处理"）
     */
    std::wstring GetDropHint(HitRegion region, const std::vector<Item*>& sourceItems) const;

    /**
     * @brief 执行拖放操作
     * @param region      命中区域类型，决定执行何种操作
     * @param sourceItems 正在被拖拽的源 Item 列表
     * @param origin      拖拽来源 Container
     * @param mods        键盘修饰键状态（Ctrl/Shift 等）
     *
     * 根据 HitRegion 类型实际执行插入排序或移交操作：
     *   - SortBefore：将源项插入到当前槽位之前
     *   - SortAfter ：将源项插入到当前槽位之后
     *   - Handoff   ：将源项移交给当前槽位的 Item 处理
     */
    void ExecuteDrop(HitRegion region, const std::vector<Item*>& sourceItems, Container* origin, int mods);

    /**
     * @brief 绘制拖放指示器
     * @param ctx    D2D 设备上下文指针
     * @param region 当前命中区域类型，决定指示器的绘制位置和样式
     *
     * 在 SortBefore / SortAfter 时绘制插入分隔线，
     * 在 Handoff 时绘制高亮边框等视觉反馈。
     */
    void DrawDropIndicator(ID2D1DeviceContext* ctx, HitRegion region, float itemPad = 0.0f) const;

private:
    Container* parent_; ///< 所属父容器指针
    RECT bounds_;       ///< 槽位边界矩形（屏幕像素坐标）
    size_t index_;      ///< 在父容器中的索引
    Item* item_ = nullptr; ///< 持有的 Item 指针，nullptr 表示空槽位
};
