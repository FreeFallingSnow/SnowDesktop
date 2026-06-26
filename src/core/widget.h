/**
 * @file widget.h
 * @brief 桌面组件（Widget）体系：组件类型定义与工厂函数
 *
 * 组件层级关系（自上而下）：
 * - Widget（纯 Item，不可接收拖放）← LuaScript
 * - WidgetContainer（Widget + ListContainer，可接收拖放）
 *   - Collection（分类集合，固定大小缩略图网格）
 *   - ScrollingItemWidget（可滚动的列表/图标视图基类）
 *     - FileCategories（文件分类面板，带分类标签页）
 *     - FolderMapping（映射文件夹，文件列表/图标模式）
 *
 * 设计要点：
 * - Widget 继承自 Item，表示桌面上一个可渲染、可拖动的矩形区域
 * - WidgetContainer 同时继承 Widget 和 ListContainer，使其既能渲染又能接收子项拖放
 * - ScrollingItemWidget 提供滚动条管理，供需要纵向滚动的列表/图标视图使用
 * - LuaScript 是纯渲染 Widget，无容器能力，完全由 Lua 脚本驱动
 */

#pragma once
#include "item.h"
#include "container.h"
#include "slot.h"
#include <d2d1_1.h>
#include <wrl/client.h>
#include <string>
#include <unordered_map>
#include <vector>
#include <memory>

struct DesktopWidget;
class DesktopApp;
struct GridPage;

/**
 * @enum WidgetHit
 * @brief 组件 chrome 区域的精细化命中测试结果
 *
 * 用于判断鼠标点击落在组件边框/底栏的哪个功能区，从而触发不同的交互行为。
 */
enum class WidgetHit {
    None,               ///< 未命中任何有效区域
    Content,            ///< 成员项区域（item 列表/网格区）
    MoveHandle,         ///< 底栏（除右下角缩放角外）—— 拖拽移动组件
    ResizeHandle,       ///< 右下角 24px 缩放角 —— 拖拽调整组件大小
    ListToggleBtn,      ///< FolderMapping：列表/图标模式切换按钮
    OpenFolderBtn,      ///< FolderMapping：打开源文件夹按钮
    CategoryTab,        ///< FileCategories：分类标签页
    SearchBox,          ///< FileCategories：搜索框
    CollectionOpenBtn,  ///< Collection：紧凑模式主体 / "全部" 马赛克按钮
};

/**
 * @class Widget
 * @brief 纯 Item 组件，不具备容器（Container）能力
 *
 * Widget 是桌面上可渲染、可拖动的矩形区域，继承自 Item。
 * 它不继承 Container，因此不能作为拖放目标接收其他 Item。
 * 主要用于 LuaScript——由 Lua 脚本完全控制渲染和行为，
 * 不需要接收外部拖放。
 *
 * 职责范围：
 * - 提供标题、路径、图标等 Item 接口
 * - 管理组件位置（SetBounds/GetBounds）
 * - 通过 Draw 方法在 Direct2D 上下文中渲染自身
 * - 支持拖拽时创建数据对象（CreateDataObject）
 */
class Widget : public Item
{
public:
    Widget(DesktopWidget* data, DesktopApp* app);
    virtual ~Widget() = default;

    // Item interface
    std::wstring GetTitle() const override;
    std::wstring GetPath() const override;
    HBITMAP GetIconBitmap() const override;
    RECT GetBounds() const override;
    void SetBounds(RECT bounds) override;
    bool IsSelected() const override;
    void SetSelected(bool selected) override;
    Container* GetContainer() const override;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;
    ComPtr<IDataObject> CreateDataObject() override;

    DesktopWidget* GetWidgetData() const { return data_; }
    DesktopApp* GetApp() const { return app_; }
    float GetCellScale() const;
    int Cu(float value) const;
    float FontCu(float value) const;
    IDWriteTextFormat* GetCuTextFormat(float value, bool bold, bool centered) const;
    IDWriteTextFormat* GetCuFaTextFormat(float value) const;

protected:
    DesktopWidget* data_;
    DesktopApp* app_;
    mutable std::unordered_map<int, ComPtr<IDWriteTextFormat>> cuTextFormatCache_;
    mutable std::unordered_map<int, ComPtr<IDWriteTextFormat>> cuFaTextFormatCache_;
};

/**
 * @class WidgetContainer
 * @brief 可接收拖放的组件，Widget + ListContainer 的组合体
 *
 * WidgetContainer 同时继承 Widget（渲染和拖动能力）与 ListContainer（一维列表容器能力），
 * 使其桌面组件既能渲染自身 UI，又能作为拖放目标接收其他 Item。
 *
 * 重要约束：WidgetContainer 不接收其他 Widget 作为拖放目标（即不允许组件嵌套组件）。
 *
 * 主要职责：
 * - 定义组件 chrome 区域（边框、底栏、标题区、缩放角）的几何计算
 * - 提供命中测试（HitTestWidget/Handle），区分底栏拖拽与缩放
 * - 绘制组件边框、背景、标题、滚动条等 chrome 元素
 * - 拖放预览：HitTestDrag、DrawDropPreview 等虚拟接口
 * - 子类覆盖 GetMemberItem、DrawContent、DrawButtons 等实现具体内容
 */
class WidgetContainer : public Widget, public ListContainer
{
public:
    using Widget::Widget;

    // Forward Container pure virtuals to Widget implementations
    std::wstring GetTitle() const override { return Widget::GetTitle(); }
    RECT GetBounds() const override { return Widget::GetBounds(); }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;

    // ── Chrome geometry ──────────────────────────────────
    RECT GetFrameRect() const;
    RECT GetBodyRect() const;
    RECT GetMoveHandleRect() const;
    RECT GetResizeHandleRect() const;
    RECT GetTitleRect() const;
    virtual RECT GetContentViewportRect() const { return GetBodyRect(); }
    virtual void ApplyMarqueeSelection(const RECT& contentRect)
    {
        (void)contentRect;
    }

    // ── Hit testing ──────────────────────────────────────
    virtual WidgetHit HitTestWidget(POINT pt) const;
    bool HitResizeHandle(POINT pt) const;

    // ── Rendering ────────────────────────────────────────
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;

    // ── Container drag virtuals ──────────────────────────
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const override;
    void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region) override;
    bool NeedsShellReloadAfterDrop() const override { return true; }

    // ── Member access — subclasses override ──────────────
    virtual Item* GetMemberItem(size_t memberIndex) const { (void)memberIndex; return nullptr; }
    virtual std::vector<size_t> GetSelectedMemberIndices() const { return {}; }
    virtual void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore)
    {
        (void)indices;
        (void)insertBefore;
    }
    virtual size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const;
    virtual bool AllowsDesktopKey(const std::wstring& key) const { (void)key; return true; }

    // ── Content — subclasses override ────────────────────
    virtual void DrawContent(ID2D1DeviceContext* context, RECT body)
    {
        (void)context;
        (void)body;
    }
    virtual void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered)
    {
        (void)context;
        (void)handleRect;
        (void)hovered;
    }

    // ── Scrollbar — subclasses override ──────────────────
    virtual int  GetScrollOffset() const { return 0; }
    virtual int  GetMaxScrollOffset() const { return 0; }
    virtual int  GetTotalContentHeight() const { return 0; }
    virtual int  GetVisibleContentHeight() const { return 0; }
    virtual void DrawScrollbar(ID2D1DeviceContext* context, bool hovered) const;

protected:
    mutable std::vector<std::unique_ptr<Item>> dragSourceCache_;
    mutable std::vector<std::unique_ptr<Item>> slotItemCache_;

    // ── Cached D2D resources (recreated only when frame/radius changes) ──
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> cachedClipGeometry_;
    RECT cachedClipFrame_{ -1, -1, -1, -1 };
    float cachedClipRadius_ = 0.0f;

    /** @brief 获取或创建圆角矩形裁剪几何体，frame/radius 不变时跨帧复用。 */
    ID2D1RoundedRectangleGeometry* GetCachedClipGeometry(ID2D1Factory1* factory,
        const RECT& frame, float radius);
};

/**
 * @class ScrollingItemWidget
 * @brief 可滚动的列表/图标视图组件基类
 *
 * 作为 FileCategories 和 FolderMapping 的公共基类，
 * 封装了列表模式（listMode）与图标模式（SingleColumn）的切换逻辑，
 * 以及列表模式下统一的项目绘制方法（DrawListItem）。
 *
 * 滚动相关接口（GetScrollOffset、GetMaxScrollOffset 等）在此声明，
 * 由子类提供具体实现。滚动条绘制由 WidgetContainer::DrawScrollbar 统一处理。
 */
class ScrollingItemWidget : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;

    bool SingleColumn() const override;
    int GetScrollOffset() const override;
    int GetMaxScrollOffset() const override = 0;
    int GetTotalContentHeight() const override = 0;
    int GetVisibleContentHeight() const override = 0;

    void DrawListItem(ID2D1DeviceContext* context, RECT cell,
        HBITMAP iconBitmap, int sysIconIndex,
        const std::wstring& name, bool selected) const;

    BarStyle GetInsertionStyle() const override;
};

/**
 * @class Collection
 * @brief 分类集合组件，固定大小缩略图网格
 *
 * Collection 是一种不可滚动的 WidgetContainer，成员以固定大小（136x92）
 * 的缩略图网格形式排列。支持分类标签，通过 CategoryIdAtPoint 确定
 * 点击位置属于哪个分类。
 *
 * 特性：
 * - 固定 item 尺寸（136x92），无滚动条
 * - 支持分类标签页点击切换
 * - "全部"马赛克按钮（GetAllButtonRect），点击展开所有分类
 * - 拖放插入样式为 VBar（竖线指示器）
 * - 无需外壳刷新（NeedsShellReloadAfterDrop = false）
 */
class Collection : public ScrollingItemWidget
{
public:
    using ScrollingItemWidget::ScrollingItemWidget;
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) override;
    WidgetHit HitTestWidget(POINT pt) const override;
    std::wstring CategoryIdAtPoint(POINT pt) const;
    std::vector<Item*> GetSelectedItems() const override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) override;
    size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const override;

    size_t GetSlotCount() const override;
    int  GetItemHeight() const override;
    int  GetItemWidth()  const override;
    Item* GetSlotItem(size_t idx) const override;

    int  GetMaxScrollOffset() const override;
    int  GetTotalContentHeight() const override;
    int  GetVisibleContentHeight() const override;
    bool SingleColumn() const override;
    BarStyle GetInsertionStyle() const override;
    RECT GetContentViewportRect() const override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;

    RECT GetAllButtonRect() const;

private:
    void DrawThumbnail(ID2D1DeviceContext* context, const DesktopItem& item,
        RECT rect, bool selected) const;
};

/**
 * @class FileCategories
 * @brief 文件分类面板组件，带可滚动的分类标签页
 *
 * 继承自 ScrollingItemWidget，支持列表/图标模式切换。
 * 按文件类型分类（文档、图片、视频、音乐等），每个分类有独立的标签页，
 * 标签页支持横向滚动（TryScrollTabs）。
 *
 * 特性：
 * - 顶部分类标签栏，支持标签滚动
 * - 根据用户桌面文件自动分类（CollectTopLevelDesktopItems）
 * - 支持按分类过滤显示文件
 * - 列表模式与图标模式可通过 SingleColumn 切换
 * - 部分桌面键（DesktopKey）可被屏蔽（AllowsDesktopKey）
 */
class FileCategories : public ScrollingItemWidget
{
public:
    using ScrollingItemWidget::ScrollingItemWidget;
    bool CollectTopLevelDesktopItems();
    bool PruneUncollectableItems();
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) override;
    WidgetHit HitTestWidget(POINT pt) const override;
    std::wstring CategoryIdAtPoint(POINT pt) const;
    bool IsPointInTabsRect(POINT pt) const;
    bool TryScrollTabs(POINT pt, int delta);
    std::vector<Item*> GetSelectedItems() const override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) override;
    size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const override;
    bool AllowsDesktopKey(const std::wstring& key) const override;

    size_t GetSlotCount() const override;
    int  GetItemHeight() const override;
    int  GetItemWidth() const override;
    Item* GetSlotItem(size_t idx) const override;

    int GetMaxScrollOffset() const override;
    int GetTotalContentHeight() const override;
    int GetVisibleContentHeight() const override;
    RECT GetContentViewportRect() const override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;

    const std::vector<std::wstring>& CachedCategoryKeys(const std::wstring& categoryId) const;
    const std::vector<std::wstring>& CachedVisibleCategoryIds() const;
    std::wstring CachedActiveCategoryId() const;

    const std::wstring& GetSearchText() const { return searchText_; }
    void SetSearchText(const std::wstring& text) { searchText_ = text; searchCursorPos_ = searchText_.size(); InvalidateSlots(); }
    void AppendSearchChar(wchar_t ch) { searchText_.insert(searchCursorPos_, 1, ch); ++searchCursorPos_; InvalidateSlots(); }
    void BackspaceSearchText() { if (searchCursorPos_ > 0) { searchText_.erase(searchCursorPos_ - 1, 1); --searchCursorPos_; InvalidateSlots(); } }
    void DeleteSearchText() { if (searchCursorPos_ < searchText_.size()) { searchText_.erase(searchCursorPos_, 1); InvalidateSlots(); } }
    void ClearSearchText() { searchText_.clear(); searchCursorPos_ = 0; searchFocused_ = false; InvalidateSlots(); }
    bool IsSearchFocused() const { return searchFocused_; }
    void SetSearchFocused(bool focused) { searchFocused_ = focused; if (focused) searchCursorPos_ = searchText_.size(); }
    void MoveCursorLeft() { if (searchCursorPos_ > 0) --searchCursorPos_; }
    void MoveCursorRight() { if (searchCursorPos_ < searchText_.size()) ++searchCursorPos_; }
    void MoveCursorHome() { searchCursorPos_ = 0; }
    void MoveCursorEnd() { searchCursorPos_ = searchText_.size(); }
    RECT GetSearchBoxRect() const;
    bool IsSearchActive() const { return !searchText_.empty(); }
    const std::vector<std::wstring>& GetSearchResultKeys() const;

private:
    struct CategorySnapshot
    {
        bool valid = false;
        size_t desktopItemCount = 0;
        std::vector<std::wstring> sourceKeys;
        std::unordered_map<std::wstring, std::vector<std::wstring>> keysByCategory;
        std::vector<std::wstring> visibleCategoryIds;
    };

    void EnsureCategorySnapshot() const;
    void InvalidateCategorySnapshot() const;

    mutable CategorySnapshot categorySnapshot_;
    std::wstring searchText_;
    size_t searchCursorPos_ = 0;
    bool searchFocused_ = false;
    mutable std::vector<std::wstring> searchResultCache_;
};

/**
 * @class FolderMapping
 * @brief 映射文件夹组件，显示文件夹内容的列表/图标视图
 *
 * 继承自 ScrollingItemWidget，映射磁盘上的一个文件夹到桌面组件中。
 * 支持列表模式和图标模式切换（由 ListToggleBtn 触发）。
 *
 * 特性：
 * - 显示文件夹内文件的列表或图标视图
 * - 底栏包含列表/图标切换按钮和打开源文件夹按钮
 * - 文件可拖入/拖出进行复制或移动
 * - 末尾始终包含一个空插槽（IncludeTrailingEmptySlot = true）
 * - 支持纵向滚动
 */
class FolderMapping : public ScrollingItemWidget
{
public:
    using ScrollingItemWidget::ScrollingItemWidget;
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) override;
    WidgetHit HitTestWidget(POINT pt) const override;
    std::vector<Item*> GetSelectedItems() const override;
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) override;

    size_t GetSlotCount() const override;
    int  GetItemHeight() const override;
    int  GetItemWidth()  const override;
    bool IncludeTrailingEmptySlot() const override { return true; }
    Item* GetSlotItem(size_t idx) const override;

    int GetMaxScrollOffset() const override;
    int GetTotalContentHeight() const override;
    int GetVisibleContentHeight() const override;
    RECT GetContentViewportRect() const override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
};

/**
 * @class GuideWidget
 * @brief 分页系统使用指南组件
 *
 * GuideWidget 继承 WidgetContainer，以可滚动 DWrite 文本展示
 * 多屏幕多分页系统的介绍和操作说明。无子项、不接受拖放，
 * 仅作为信息展示和页面占位用途。
 */
class GuideWidget : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;

    size_t GetSlotCount() const override { return 0; }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override { return {}; }
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawScrollbar(ID2D1DeviceContext* context, bool hovered) const override;
    int GetMaxScrollOffset() const override { return std::max(0, static_cast<int>(totalTextHeight_ - lastBodyHeight_)); }
    HitRegion HitTestDrag(POINT /*pt*/, Slot*& outSlot) override { outSlot = nullptr; return HitRegion::None; }
    std::wstring GetDragHint(Slot*, HitRegion, const std::vector<Item*>&, Container*, int) const override { return L""; }
    void OnItemsDropped(const std::vector<Item*>&, Container*, Slot*, HitRegion, int) override {}
    std::vector<Item*> GetSelectedItems() const override { return {}; }

private:
    static std::wstring BuildGuideText(const DesktopApp* app);
    mutable float totalTextHeight_ = 0;
    mutable LONG lastBodyHeight_ = 0;
};

/**
 * @class LuaScript
 * @brief Lua 脚本驱动的纯渲染组件
 *
 * LuaScript 继承自 Widget（纯 Item），不具备容器能力，
 * 不接受外部拖放。其所有渲染和行为完全由关联的 Lua 脚本控制。
 *
 * 与 WidgetContainer 系列不同，LuaScript：
 * - 没有底栏、边框等 chrome 元素
 * - 没有成员项列表
 * - 不可以作为拖放目标
 * - 完全由 Lua 脚本的 Draw 回调决定呈现内容
 */
class LuaScript : public Widget
{
public:
    using Widget::Widget;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;

private:
    struct WidgetLoadResult { bool ok = false; bool customStyle = false; };
    WidgetLoadResult SafeLoadWidget(const std::wstring& id, const std::wstring& scriptPath);
    bool SafeRenderWidget(const std::wstring& id, const std::wstring& scriptPath,
        ID2D1DeviceContext* context, RECT frame, int columns, int rows);
    bool SafeReadFlags(const std::wstring& scriptPath, bool& showTitle, bool& bottomBarHover);

    ID2D1RoundedRectangleGeometry* GetCachedClipGeometry(ID2D1Factory1* factory,
        const RECT& frame, float radius);

    // Cached clip geometry (recreated only when frame/radius changes)
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> cachedClipGeometry_;
    RECT cachedClipFrame_{ -1, -1, -1, -1 };
    float cachedClipRadius_ = 0.0f;
};

/**
 * @brief 创建组件实例的工厂函数
 *
 * 根据 DesktopWidget::Type 决定创建哪种组件子类：
 * - Type::Collection    → Collection
 * - Type::FileCategories → FileCategories
 * - Type::FolderMapping → FolderMapping
 * - Type::LuaScript     → LuaScript
 * - 其他                → WidgetContainer
 *
 * @param data  组件数据源（持久化配置）
 * @param app   桌面应用主对象指针
 * @return 新创建的组件实例，已确定具体子类类型
 */
std::unique_ptr<Widget> CreateWidget(DesktopWidget* data, DesktopApp* app);

/**
 * @brief 共享滚动条绘制辅助函数
 *
 * 在给定矩形区域内绘制纵向滚动条。被 WidgetContainer::DrawScrollbar
 * 和 Collection 弹窗共用，避免重复实现。
 *
 * @param context       Direct2D 绘制上下文
 * @param body          内容区域矩形（滚动条绘制在此区域右侧）
 * @param contentHeight 内容总高度（像素）
 * @param visibleHeight 可见区域高度（像素）
 * @param scrollOffset  当前滚动偏移量（像素）
 * @param hovered       鼠标是否悬停在滚动条区域
 */
void DrawScrollbarAt(ID2D1DeviceContext* context, RECT body, int contentHeight,
    int visibleHeight, int scrollOffset, bool hovered, float cellScale = 1.0f);
