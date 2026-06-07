/**
 * @file container.h
 * @brief 容器体系：桌面网格与组件列表的抽象基类及派生类
 *
 * 本文件定义了容器（Container）继承体系，用于管理桌面图标与 Widget 的布局：
 * - Container       —— 抽象基类，提供插槽缓存、拖放、绘制等通用接口
 * - GridContainer   —— 二维网格布局，专用于 DesktopGrid，可容纳任意 Item（含 Widget）
 * - ListContainer   —— 一维列表布局，专用于 Widget，仅接受普通 Item，提供三种渲染模式
 *
 * 核心设计：Container 通过 BuildSlots() 构建插槽（Slot）并缓存，直到 InvalidateSlots()
 * 被调用才会重建。派生类只需实现 BuildSlots() 即可自定义布局。
 */

#pragma once
#include <windows.h>
#include <d2d1_1.h>
#include <string>
#include <vector>
#include <memory>

class Item;
class Slot;
enum class HitRegion;

/** 拖放插入指示条的样式 */
enum class BarStyle
{
    VBar,    ///< 竖直条，用于 GridContainer（二维网格中指示列间插入位置）
    HBar     ///< 水平条，用于 ListContainer（一维列表中指示行间插入位置）
};

/**
 * @class Container
 * @brief 所有容器类的抽象基类
 *
 * Container 管理一个插槽（Slot）集合，每个插槽可容纳一个 Item。派生类通过
 * 实现 BuildSlots() 定义自己的布局逻辑，基类负责缓存插槽并在失效时重建。
 *
 * 职责包括：
 * - 插槽生命周期管理（BuildSlots / GetSlots / InvalidateSlots）
 * - 拖放目标判定（HitTestDrag）与视觉反馈（DrawDropPreview）
 * - 拖放源选择（GetSelectedItems）
 * - Chrome 层与内容层的分层绘制（DrawChrome / DrawContents）
 */
class Container
{
public:
    virtual ~Container() = default;

    /// 返回容器标题，用于标识或调试
    virtual std::wstring GetTitle() const = 0;

    // Build and cache slots. Returns stable references valid until InvalidateSlots.
    const std::vector<std::unique_ptr<Slot>>& GetSlots();
    void InvalidateSlots() { slotsValid_ = false; }

    /** @brief 构建插槽列表。由 GetSlots() 在缓存失效时调用，派生类必须实现 */
    virtual std::vector<std::unique_ptr<Slot>> BuildSlots() = 0;

    /** @brief 处理 Item 被拖放至本容器
     *  @param sourceItems  待放置的源 Item 列表
     *  @param origin       源容器指针
     *  @param targetSlot   目标插槽（可能为空，表示追加到末尾）
     *  @param region       拖放命中的区域（如插入到目标前/后/内部）
     *  @param mods         键盘修饰键状态
     */
    virtual void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) = 0;

    /** @brief 绘制容器的装饰层（如背景、边框、插入指示条）
     *  @param context  D2D 设备上下文
     *  @param mousePt  当前鼠标位置（用于高亮等交互反馈）
     */
    virtual void DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
    {
        (void)context;
        (void)mousePt;
    }

    /// 绘制容器内所有 Item 的内容
    virtual void DrawContents(ID2D1DeviceContext* context);

    /// 返回容器在屏幕上的边界矩形
    virtual RECT GetBounds() const = 0;

    /// 返回本容器使用的插入指示条样式
    virtual BarStyle GetInsertionStyle() const = 0;

    /** @brief 获取容器中当前被选中的 Item
     *  @return Item 指针向量。指针指向容器持有的临时对象，在下一次调用前有效
     */
    virtual std::vector<Item*> GetSelectedItems() const { return {}; }

    /** @brief 对指定点进行拖放命中测试
     *  @param pt      命中测试的屏幕坐标
     *  @param outSlot 输出参数，指向命中的插槽（末尾空白区域可能为 null）
     *  @return 命中的区域类型
     */
    virtual HitRegion HitTestDrag(POINT pt, Slot*& outSlot) = 0;

    /** @brief 获取拖放提示文本（如“移动到文件夹 XX”）
     *  @param slot         目标插槽
     *  @param region       命中区域
     *  @param sourceItems  源 Item 列表
     *  @param origin       源容器
     *  @param mods         键盘修饰键
     *  @return 提示字符串（空串表示无提示）
     */
    virtual std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const
    { (void)slot; (void)region; (void)sourceItems; (void)origin; (void)mods; return L""; }

    /** @brief 在目标插槽处绘制放置预览效果
     *  @param ctx     D2D 设备上下文
     *  @param slot    目标插槽
     *  @param region  命中区域
     */
    virtual void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot,
        HitRegion region) { (void)ctx; (void)slot; (void)region; }

    /** @brief 放置操作后是否需要重新加载 Shell Item
     *  @return true 表示应用应在放置后重新加载 shell 项
     */
    virtual bool NeedsShellReloadAfterDrop() const { return false; }

protected:
    /** 缓存的插槽列表，由 BuildSlots() 构建，调用 InvalidateSlots() 后标记为失效 */
    std::vector<std::unique_ptr<Slot>> cachedSlots_;
    /** 缓存有效性标志；false 时 GetSlots() 将调用 BuildSlots() 重建 */
    bool slotsValid_ = false;
};

/**
 * @class GridContainer
 * @brief 二维网格布局容器
 *
 * 仅由 DesktopGrid 使用，将 Item 排列为固定行列的网格。可容纳任何类型
 * 的 Item（包括 Widget）。插入指示条样式固定为 VBar（竖直条），表示在
 * 列与列之间插入。
 *
 * 布局细节由 DesktopGrid 中的 BuildSlots() 实现控制（网格列数、间距等）。
 */
class GridContainer : public Container
{
public:
    BarStyle GetInsertionStyle() const override { return BarStyle::VBar; }
};

/**
 * @class ListContainer
 * @brief 一维列表布局容器
 *
 * 专用于 Widget 容器，将 Item 排列为单行或单列的线性列表。仅接受
 * 普通 Item，不接受嵌套 Widget。
 *
 * 提供三种运行时渲染模式，由 RenderMode 枚举控制：
 * - Fixed          —— 固定尺寸平铺
 * - ScrollingIcon  —— 可滚动的图标模式
 * - ScrollingRow   —— 可滚动的行模式
 *
 * 派生类通过重写 GetSlotCount()、GetItemHeight()、GetItemWidth()、
 * SingleColumn() 等虚方法自定义布局参数。插入指示条样式固定为 HBar
 * （水平条），表示在行与行之间插入。
 */
class ListContainer : public Container
{
public:
    BarStyle GetInsertionStyle() const override { return BarStyle::HBar; }

    /// 根据当前渲染模式构建插槽列表
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;

    /** 渲染模式 */
    enum class RenderMode
    {
        Fixed,          ///< 固定尺寸平铺模式
        ScrollingIcon,  ///< 可滚动的图标模式
        ScrollingRow    ///< 可滚动的行模式
    };

    /// 返回当前渲染模式
    virtual RenderMode GetRenderMode() const { return RenderMode::Fixed; }

    // ── 布局参数（子类重写以控制布局） ──────────────────

    /** @brief 返回插槽总数 */
    virtual size_t GetSlotCount() const = 0;

    /** @brief 返回每个 Item 的高度（像素），默认 32 */
    virtual int  GetItemHeight() const { return 32; }

    /** @brief 返回每个 Item 的宽度（像素），默认 92 */
    virtual int  GetItemWidth()  const { return 92; }

    /** @brief 是否为单列布局
     *  @return true 表示单列（垂直滚动），false 表示多列
     */
    virtual bool SingleColumn() const { return false; }

    /** @brief 是否在末尾包含一个空白插槽（用于放置操作）
     *  @return true 表示在列表末尾保留一个空位
     */
    virtual bool IncludeTrailingEmptySlot() const { return false; }

    /** @brief 获取指定索引处的 Item 指针
     *  @param idx 插槽索引
     *  @return Item 指针；越界或无 Item 时返回 nullptr
     */
    virtual Item* GetSlotItem(size_t idx) const { (void)idx; return nullptr; }
};
