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
#include "../item_visual_metrics.h"
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <memory>

struct DesktopWidget;
struct CategorySettings;
class DesktopApp;
class WidgetEngine;
struct GridPage;
namespace snowdesktop
{
class WidgetPreviewScene;
struct WidgetRenderOptions;
struct WidgetPreviewItem;
}

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
    DateHeaderToggleBtn, ///< FileCategories：日期表头开关按钮
    OpenFolderBtn,      ///< FolderMapping：打开源文件夹按钮
    SourceTab,          ///< FileGroup：来源组件标签
    CategoryTab,        ///< FileCategories / FolderMapping：分类标签页
    SearchBox,          ///< FileCategories / FolderMapping：搜索框
    CollectionOpenBtn,  ///< Collection：紧凑模式主体 / "全部" 马赛克按钮
    GuideAddWidgetBtn,  ///< Guide：打开“添加组件”菜单
    GuideDetailsBtn,    ///< Guide：展开或收起分页说明
    DetailsNameHeader,  ///< 详细信息：名称列表头
    DetailsModifiedHeader, ///< 详细信息：修改日期列表头
    DetailsTypeHeader,  ///< 详细信息：类型列表头
    DetailsSizeHeader,  ///< 详细信息：大小列表头
    DetailsModifiedDivider, ///< 详细信息：修改日期列宽分隔线
    DetailsTypeDivider, ///< 详细信息：类型列宽分隔线
    DetailsSizeDivider, ///< 详细信息：大小列宽分隔线
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
    virtual void DrawPreview(ID2D1DeviceContext* context, RECT frame,
        const snowdesktop::WidgetRenderOptions& options);
    ComPtr<IDataObject> CreateDataObject() override;

    DesktopWidget* GetWidgetData() const { return data_; }
    DesktopApp* GetApp() const { return app_; }
    float GetCellScale() const;
    float GetLayoutSpacingScale() const;
    virtual snowdesktop::PageItemVisualMetrics GetItemVisualMetrics() const;
    int Cu(float value) const;
    float FontCu(float value) const;
    IDWriteTextFormat* GetCuTextFormat(float value, bool bold, bool centered) const;
    IDWriteTextFormat* GetCuTextFormatWeight(float value, DWRITE_FONT_WEIGHT weight, bool centered) const;
    IDWriteTextFormat* GetCuFaTextFormat(float value) const;
    IDWriteTextFormat* GetCuFluentTextFormat(float value) const;
    float GetBarHeight() const;
    float GetBarScale() const;
    void SetRenderOptions(
        const snowdesktop::WidgetRenderOptions* options)
    {
        renderOptions_ = options;
    }
    const snowdesktop::WidgetRenderOptions* GetRenderOptions() const
    {
        return renderOptions_;
    }
    snowdesktop::WidgetPreviewScene* GetPreviewScene() const;
    bool IsPreviewRendering() const;
    bool IsPreviewInteractive() const;
    bool ShouldRegisterBackdrop() const;
    POINT GetRenderPointer() const;

protected:
    DesktopWidget* data_;
    DesktopApp* app_;
    const snowdesktop::WidgetRenderOptions* renderOptions_ = nullptr;
    mutable std::unordered_map<int, ComPtr<IDWriteTextFormat>> cuTextFormatCache_;
    mutable std::unordered_map<int, ComPtr<IDWriteTextFormat>> cuFaTextFormatCache_;
    mutable std::unordered_map<int, ComPtr<IDWriteTextFormat>> cuFluentTextFormatCache_;
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
    snowdesktop::PageItemVisualMetrics GetItemVisualMetrics() const override;
    RECT GetBodyRect() const;
    virtual RECT GetMemberLayoutRect(size_t index) const
    {
        (void)index;
        return {};
    }
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
    void DrawPreview(ID2D1DeviceContext* context, RECT frame,
        const snowdesktop::WidgetRenderOptions& options) override;

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
    virtual int GetBottomBarButtonCount() const { return 0; }

    // ── Scrollbar — subclasses override ──────────────────
    virtual int  GetScrollOffset() const { return 0; }
    virtual int  GetMaxScrollOffset() const { return 0; }
    virtual int  GetTotalContentHeight() const { return 0; }
    virtual int  GetVisibleContentHeight() const { return 0; }
    virtual void DrawScrollbar(ID2D1DeviceContext* context, bool hovered) const;
    void SetHostedFrame(const RECT* frame);
    bool IsHosted() const { return hostedFrameActive_; }

protected:
    mutable std::vector<std::unique_ptr<Item>> dragSourceCache_;
    mutable std::vector<std::unique_ptr<Item>> slotItemCache_;

    // ── Cached D2D resources (recreated only when frame/radius changes) ──
    Microsoft::WRL::ComPtr<ID2D1RoundedRectangleGeometry> cachedClipGeometry_;
    RECT cachedClipFrame_{ -1, -1, -1, -1 };
    float cachedClipRadius_ = 0.0f;
    RECT hostedFrame_{};
    bool hostedFrameActive_ = false;

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
struct ListItemDetails
{
    std::wstring typeName;
    std::optional<FILETIME> modifiedTime;
    std::optional<std::uint64_t> fileSize;
    bool isDirectory = false;
};

class ScrollingItemWidget : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;

    bool SingleColumn() const override;
    int GetScrollOffset() const override;
    int GetMaxScrollOffset() const override = 0;
    int GetTotalContentHeight() const override = 0;
    int GetVisibleContentHeight() const override = 0;
    RECT GetMemberLayoutRect(size_t index) const override = 0;

    void DrawListItem(ID2D1DeviceContext* context, RECT cell,
        HBITMAP iconBitmap, int sysIconIndex,
        const std::wstring& name, bool selected,
        bool iconIsMediaThumbnail,
        std::wstring_view demoIdentity = {},
        const DesktopWidget* demoCollection = nullptr,
        const ListItemDetails& details = {}) const;

    int GetListRowHeight() const;
    int GetDetailsHeaderHeight() const;
    bool IsDetailsVisible() const;
    RECT ApplyDetailsHeaderToViewport(RECT viewport) const;
    RECT GetDetailsHeaderRectFromViewport(RECT viewport) const;
    void DrawDetailsHeader(
        ID2D1DeviceContext* context, RECT itemViewport) const;
    WidgetHit HitTestDetailsHeader(
        POINT point, RECT itemViewport) const;
    virtual const DesktopWidget* GetDetailsSortData() const;

    void DrawPrivacyPlaceholder(ID2D1DeviceContext* context, RECT rect,
        const std::wstring& name, bool isDir, bool showLabel = true,
        bool centerIconVertically = false) const;

    const std::wstring& GetSearchText() const { return searchText_; }
    void SetSearchText(const std::wstring& text);
    void AppendSearchChar(wchar_t ch);
    void BackspaceSearchText();
    void DeleteSearchText();
    void ClearSearchText();
    bool IsSearchFocused() const { return searchFocused_; }
    void SetSearchFocused(bool focused);
    size_t GetSearchCursorPosition() const { return searchCursorPos_; }
    size_t GetSearchSelectionAnchor() const
    {
        return searchSelectionAnchor_;
    }
    const std::wstring& GetSearchCompositionText() const
    {
        return searchCompositionText_;
    }
    size_t GetSearchCompositionCursor() const
    {
        return searchCompositionCursor_;
    }
    void SetSearchCursorPosition(size_t position);
    void SetSearchEditingState(
        size_t cursor,
        size_t selectionAnchor,
        const std::wstring& compositionText,
        size_t compositionCursor);
    void MoveCursorLeft(bool extendSelection = false);
    void MoveCursorRight(bool extendSelection = false);
    void MoveCursorHome(bool extendSelection = false);
    void MoveCursorEnd(bool extendSelection = false);
    bool HandleSearchKey(WPARAM key);
    void BeginSearchPointerSelection(
        POINT point, bool extendSelection);
    void UpdateSearchPointerSelection(POINT point);
    void EndSearchPointerSelection();
    bool IsSearchPointerSelecting() const
    {
        return searchPointerSelecting_;
    }
    void SetSearchComposition(
        const std::wstring& text, size_t cursor);
    void CommitSearchComposition(
        const std::wstring& text);
    void ClearSearchComposition();
    bool GetSearchCaretRect(RECT& rect) const;
    virtual RECT GetSearchBoxRect() const { return {}; }
    bool IsSearchActive() const { return !searchText_.empty(); }
    void DrawSearchBox(ID2D1DeviceContext* context);
    /** @brief 分类滚动组件共用的搜索框布局。 */
    RECT GetCategorizedSearchBoxRect(bool visible) const;
    /** @brief 分类滚动组件共用的标签区布局。 */
    RECT GetCategorizedTabsRect(bool visible) const;
    /** @brief 分类滚动组件共用的标签条高度（cu）。 */
    float GetCategorizedTabHeight() const;
    /** @brief 按标签条高度联动的标签字号（cu）。 */
    float GetCategorizedTabFontSize() const;
    /** @brief 标签行距（cu），= 标签条高度 + 4。 */
    float GetCategorizedTabRowPitch() const;
    /** @brief 搜索框高度（cu），= 标签条高度 − 4。 */
    float GetCategorizedSearchBoxHeight() const;
    /** @brief 分类标签（桌面文件/映射文件夹）是否显示文件数量。 */
    bool ShowCategoryTabItemCounts() const;
    /** @brief 按共同字号测量并分配标签宽度。 */
    std::vector<int> BuildCategorizedTabWidths(
        const std::vector<std::wstring>& labels,
        int availableWidth) const;
    /**
     * @brief 绘制统一样式的单个分类标签。
     * @param visibleTabRect 裁剪后的背景与命中矩形。
     * @param layoutTabRect 未裁剪的文字布局矩形。
     */
    void DrawCategorizedTab(
        ID2D1DeviceContext* context,
        RECT visibleTabRect,
        RECT layoutTabRect,
        const std::wstring& label,
        bool active,
        bool hovered) const;
    void SetCategorizedHostOptions(
        int tabRowOffset,
        bool searchVisibilityOverrideActive,
        bool searchVisible,
        bool tabsVisibilityOverrideActive,
        bool tabsVisible,
        bool searchAllCategories);
    void ClearCategorizedHostOptions();
    bool SearchAllCategories() const
    {
        return categorizedSearchAllCategories_;
    }
    int GetCategorizedTabRowOffset() const
    {
        return categorizedTabRowOffset_;
    }
    virtual std::wstring CategoryIdAtPoint(POINT pt) const { (void)pt; return L""; }
    virtual bool TryScrollTabs(POINT pt, int delta) { (void)pt; (void)delta; return false; }

    BarStyle GetInsertionStyle() const override;

protected:
    std::wstring searchText_;
    size_t searchCursorPos_ = 0;
    size_t searchSelectionAnchor_ = 0;
    std::wstring searchCompositionText_;
    size_t searchCompositionCursor_ = 0;
    bool searchFocused_ = false;
    bool searchPointerSelecting_ = false;

private:
    size_t GetSearchSelectionStart() const;
    size_t GetSearchSelectionEnd() const;
    bool HasSearchSelection() const;
    bool EraseSearchSelection();
    void ReplaceSearchSelection(const std::wstring& text);
    size_t HitTestSearchTextPosition(POINT point) const;
    std::wstring BuildSearchDisplayText(
        size_t& displayCursor,
        size_t& compositionStart,
        size_t& compositionLength) const;
    void DrawListItemTitle(ID2D1DeviceContext* context, RECT cell,
        RECT iconRect, const std::wstring& title) const;
    snowdesktop::list_detail_rules::Columns GetDetailsColumns(
        int availableWidth) const;
    int categorizedTabRowOffset_ = 0;
    bool categorizedSearchVisibilityOverrideActive_ = false;
    bool categorizedSearchVisible_ = false;
    bool categorizedTabsVisibilityOverrideActive_ = false;
    bool categorizedTabsVisible_ = false;
    bool categorizedSearchAllCategories_ = false;
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
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SurfaceForWidgetType(
                DesktopWidgetType::Collection);
    }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) override;
    int GetBottomBarButtonCount() const override;
    WidgetHit HitTestWidget(POINT pt) const override;
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
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
    RECT GetMemberLayoutRect(size_t index) const override;
    bool SingleColumn() const override;
    BarStyle GetInsertionStyle() const override;
    RECT GetContentViewportRect() const override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;

    RECT GetAllButtonRect() const;

private:
    void DrawThumbnail(ID2D1DeviceContext* context, const DesktopItem& item,
        RECT rect, bool selected) const;
    void DrawTitlelessTooltip(ID2D1DeviceContext* context,
        const std::wstring& title, RECT anchor) const;
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
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SurfaceForWidgetType(
                DesktopWidgetType::FileCategories);
    }
    bool CollectTopLevelDesktopItems();
    bool PruneUncollectableItems();
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) override;
    int GetBottomBarButtonCount() const override { return 2; }
    WidgetHit HitTestWidget(POINT pt) const override;
    std::wstring CategoryIdAtPoint(POINT pt) const override;
    bool IsPointInTabsRect(POINT pt) const;
    bool TryScrollTabs(POINT pt, int delta) override;
    std::wstring GetCategoryDisplayLabel(const std::wstring& categoryId) const;
    void InvalidateCategoryCache();
    std::vector<Item*> GetSelectedItems() const override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) override;
    size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const override;
    bool AllowsDesktopKey(const std::wstring& key) const override;

    struct LayoutSegment
    {
        bool isHeader = false;
        std::wstring label;          // valid when isHeader
        size_t firstItemIndex = 0;   // valid when !isHeader: first index into active keys
        size_t itemCount = 0;        // valid when !isHeader: number of items in this segment
        LONG y = 0;                  // top offset relative to content.top
        LONG height = 0;             // segment height in pixels
    };

    void EnsureLayout() const;
    const std::vector<LayoutSegment>& GetLayoutCache() const { return layoutCache_; }

    size_t GetSlotCount() const override;
    int  GetItemHeight() const override;
    int  GetItemWidth() const override;
    Item* GetSlotItem(size_t idx) const override;

    int GetMaxScrollOffset() const override;
    int GetTotalContentHeight() const override;
    int GetVisibleContentHeight() const override;
    RECT GetMemberLayoutRect(size_t index) const override;
    RECT GetContentViewportRect() const override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;

    const std::vector<std::wstring>& CachedCategoryKeys(const std::wstring& categoryId) const;
    const std::vector<std::wstring>& CachedVisibleCategoryIds() const;
    std::wstring CachedActiveCategoryId() const;

    RECT GetSearchBoxRect() const override;
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
    mutable std::vector<LayoutSegment> layoutCache_;
    mutable std::wstring layoutCacheCategory_;
    mutable bool layoutCacheListMode_ = false;
    mutable int layoutCacheItemHeight_ = 0;
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
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SurfaceForWidgetType(
                DesktopWidgetType::FolderMapping);
    }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) override;
    int GetBottomBarButtonCount() const override { return 3; }
    WidgetHit HitTestWidget(POINT pt) const override;
    std::vector<Item*> GetSelectedItems() const override;
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) override;
    size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const override;

    size_t GetSlotCount() const override;
    int  GetItemHeight() const override;
    int  GetItemWidth()  const override;
    bool IncludeTrailingEmptySlot() const override { return true; }
    Item* GetSlotItem(size_t idx) const override;

    int GetMaxScrollOffset() const override;
    int GetTotalContentHeight() const override;
    int GetVisibleContentHeight() const override;
    RECT GetMemberLayoutRect(size_t index) const override;
    RECT GetContentViewportRect() const override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
    RECT GetSearchBoxRect() const override;
    std::wstring CategoryIdAtPoint(POINT pt) const override;
    bool TryScrollTabs(POINT pt, int delta) override;
    const std::vector<size_t>& GetVisibleEntryIndices() const;
    const std::vector<std::wstring>& GetVisibleCategoryIds() const;
    void InvalidateFilterCache();
    const CategorySettings& GetCategorySettingsForDisplay() const;

    struct DateLayoutSegment
    {
        bool isHeader = false;
        std::wstring label;
        size_t firstItemIndex = 0;
        size_t itemCount = 0;
        LONG y = 0;
        LONG height = 0;
    };

    void EnsureDateLayout() const;
    const std::vector<DateLayoutSegment>& GetDateLayoutCache() const
    {
        return dateLayoutCache_;
    }

private:
    void EnsureCategorySnapshot() const;
    std::wstring CachedActiveCategoryId() const;

    mutable std::vector<std::wstring> categorySnapshotPaths_;
    mutable std::unordered_map<std::wstring, std::vector<size_t>> entryIndicesByCategory_;
    mutable std::vector<std::wstring> visibleCategoryIds_;
    mutable std::vector<size_t> visibleEntryIndices_;
    mutable std::wstring visibleEntriesCategory_;
    mutable std::wstring visibleEntriesSearch_;
    mutable bool visibleEntriesDateHeaders_ = false;
    mutable bool categorySnapshotValid_ = false;
    mutable std::vector<DateLayoutSegment> dateLayoutCache_;
    mutable std::vector<size_t> dateLayoutSource_;
    mutable bool dateLayoutListMode_ = false;
    mutable int dateLayoutItemHeight_ = 0;
};

/**
 * @brief 集合组中的集合标签项。
 *
 * 该适配器不拥有集合数据，只把被收纳的 Collection 组件暴露为可选择、
 * 可拖拽的列表项。
 */
class CollectionGroupEntryItem : public Item
{
public:
    CollectionGroupEntryItem(DesktopWidget* collection, Container* container,
        DesktopApp* app);
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
    const std::wstring& GetCollectionId() const;

private:
    DesktopWidget* collection_ = nullptr;
    Container* container_ = nullptr;
    DesktopApp* app_ = nullptr;
    RECT bounds_{};
};

/**
 * @brief 集合组组件，以可拖拽标签和可滚动图标区组织多个 Collection 组件。
 */
class CollectionGroup : public ScrollingItemWidget
{
public:
    using ScrollingItemWidget::ScrollingItemWidget;
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SurfaceForWidgetType(
                DesktopWidgetType::CollectionGroup);
    }

    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) override;
    int GetBottomBarButtonCount() const override { return 1; }
    WidgetHit HitTestWidget(POINT pt) const override;
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
    void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot,
        HitRegion region) override;
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const override;
    std::vector<Item*> GetSelectedItems() const override;
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) override;
    size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const override;
    size_t GetSlotCount() const override;
    int GetItemHeight() const override;
    int GetItemWidth() const override;
    Item* GetSlotItem(size_t idx) const override;
    bool SingleColumn() const override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
    int GetMaxScrollOffset() const override;
    int GetTotalContentHeight() const override;
    int GetVisibleContentHeight() const override;
    RECT GetMemberLayoutRect(size_t index) const override;
    RECT GetContentViewportRect() const override;
    RECT GetSearchBoxRect() const override;
    const DesktopWidget* GetDetailsSortData() const override;
    std::wstring CategoryIdAtPoint(POINT pt) const override;
    bool TryScrollTabs(POINT pt, int delta) override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;
    const std::vector<std::wstring>& GetVisibleCollectionIds() const;
    const std::vector<std::wstring>& GetVisibleItemKeys() const;
    std::wstring GetActiveCollectionId() const;
    CollectionGroupEntryItem* GetTabItemAtPoint(POINT pt) const;
    RECT GetTabRectById(const std::wstring& collectionId) const;
    void EnsureTabVisible(size_t tabIndex);
    void InvalidateFilterCache();

private:
    mutable std::vector<std::wstring> visibleCollectionIds_;
    mutable std::vector<std::wstring> visibleItemKeys_;
    mutable std::wstring visibleSearchText_;
    mutable std::wstring visibleCategoryId_;
    mutable std::unique_ptr<CollectionGroupEntryItem> tabItemCache_;
    mutable std::unique_ptr<Slot> tabDropSlot_;
};

/**
 * @brief 文件组中的来源组件标签项。
 *
 * 标签只引用 FileCategories 或 FolderMapping 组件，不拥有或复制文件数据。
 */
class FileGroupEntryItem : public Item
{
public:
    FileGroupEntryItem(DesktopWidget* child, Container* container,
        DesktopApp* app);
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
    const std::wstring& GetChildWidgetId() const;

private:
    DesktopWidget* child_ = nullptr;
    Container* container_ = nullptr;
    DesktopApp* app_ = nullptr;
    RECT bounds_{};
};

/**
 * @brief 文件组组件，以来源标签托管桌面文件分类与映射文件夹。
 */
class FileGroup : public ScrollingItemWidget
{
public:
    using ScrollingItemWidget::ScrollingItemWidget;
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SurfaceForWidgetType(
                DesktopWidgetType::FileGroup);
    }

    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems,
        Container* origin, Slot* targetSlot, HitRegion region,
        int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    void DrawButtons(ID2D1DeviceContext* context, RECT handleRect,
        bool hovered) override;
    int GetBottomBarButtonCount() const override;
    WidgetHit HitTestWidget(POINT pt) const override;
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
    void DrawDropPreview(ID2D1DeviceContext* context, Slot* slot,
        HitRegion region) override;
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin,
        int mods) const override;
    std::vector<Item*> GetSelectedItems() const override;
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices,
        size_t insertBefore) override;
    size_t GetDropInsertIndex(Slot* targetSlot,
        HitRegion region) const override;
    size_t GetSlotCount() const override;
    int GetItemHeight() const override;
    int GetItemWidth() const override;
    Item* GetSlotItem(size_t idx) const override;
    bool SingleColumn() const override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
    int GetMaxScrollOffset() const override;
    int GetTotalContentHeight() const override;
    int GetVisibleContentHeight() const override;
    RECT GetMemberLayoutRect(size_t index) const override;
    RECT GetContentViewportRect() const override;
    RECT GetSearchBoxRect() const override;
    const DesktopWidget* GetDetailsSortData() const override;
    std::wstring CategoryIdAtPoint(POINT pt) const override;
    bool TryScrollTabs(POINT pt, int delta) override;
    void ApplyMarqueeSelection(const RECT& contentRect) override;

    const std::vector<std::wstring>& GetVisibleSourceIds() const;
    std::wstring GetActiveSourceId() const;
    std::wstring SourceIdAtPoint(POINT pt) const;
    FileGroupEntryItem* GetSourceTabItemAtPoint(POINT pt) const;
    RECT GetSourceTabRectById(const std::wstring& childId) const;
    void EnsureSourceTabVisible(size_t tabIndex);
    void InvalidateHostedView();
    ScrollingItemWidget* GetActiveSourceContainer() const;
    ScrollingItemWidget* GetSourceContainerById(
        const std::wstring& childId) const;
    std::vector<Item*> GetHostedSelectedItemsForSource(
        const std::wstring& childId) const;
    std::vector<std::wstring> GetHostedVisibleCategoryIds() const;
    std::vector<std::wstring> GetHostedVisibleItemKeys() const;
    std::vector<size_t> GetHostedVisibleFolderIndices() const;
    bool IsGroupSearchActive() const;
    ScrollingItemWidget* GetSourceContainerForItem(
        const Item* item) const;

private:
    struct SearchResultRef
    {
        std::wstring sourceId;
        bool folderMapping = false;
        std::wstring desktopKey;
        size_t folderEntryIndex = static_cast<size_t>(-1);
    };

    const std::vector<SearchResultRef>&
        GetGroupSearchResults() const;
    Item* CreateGroupSearchItem(
        size_t index, bool dragCache) const;

    mutable std::vector<std::wstring> visibleSourceIds_;
    mutable std::unique_ptr<FileGroupEntryItem> sourceTabItemCache_;
    mutable std::unique_ptr<Slot> sourceTabDropSlot_;
    mutable std::unique_ptr<Slot> hostedDropSlot_;
    mutable std::unique_ptr<Item> hostedDropItem_;
    mutable RECT dropPreviewBounds_{};
    mutable size_t dropPreviewIndex_ = 0;
    mutable float dropPreviewItemPad_ = 0.0f;
    mutable bool dropPreviewValid_ = false;
    mutable bool dropPreviewSourceTab_ = false;
    mutable std::vector<SearchResultRef> groupSearchResults_;
    mutable std::wstring groupSearchCacheQuery_;
    mutable bool groupSearchCacheValid_ = false;
    mutable std::unordered_map<
        std::wstring,
        std::unique_ptr<ScrollingItemWidget>>
        hostedSourceCache_;
};

/**
 * @class GuideWidget
 * @brief 分页系统使用指南组件
 *
 * GuideWidget 继承 WidgetContainer，作为新页面的欢迎卡片和操作入口。
 * 无子项、不接受拖放；删除卡片后，现有空页清理流程会移除该页面。
 */
class GuideWidget : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;

    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SurfaceForWidgetType(
                DesktopWidgetType::Guide);
    }
    size_t GetSlotCount() const override { return 0; }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override { return {}; }
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    WidgetHit HitTestWidget(POINT pt) const override;
    int GetMaxScrollOffset() const override { return 0; }
    HitRegion HitTestDrag(POINT /*pt*/, Slot*& outSlot) override { outSlot = nullptr; return HitRegion::None; }
    std::wstring GetDragHint(Slot*, HitRegion, const std::vector<Item*>&, Container*, int) const override { return L""; }
    void OnItemsDropped(const std::vector<Item*>&, Container*, Slot*, HitRegion, int) override {}
    std::vector<Item*> GetSelectedItems() const override { return {}; }
    void ToggleDetails() { detailsExpanded_ = !detailsExpanded_; }

private:
    RECT GetPrimaryButtonRect(RECT body) const;
    RECT GetSecondaryButtonRect(RECT body) const;
    bool detailsExpanded_ = false;
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
    void DrawPreview(ID2D1DeviceContext* context, RECT frame,
        const snowdesktop::WidgetRenderOptions& options) override;

private:
    struct WidgetLoadResult { bool ok = false; bool customStyle = false; };
    WidgetLoadResult SafeLoadWidget(WidgetEngine* engine,
        const std::wstring& id, const std::wstring& scriptPath,
        bool preview);
    bool SafeRenderWidget(const std::wstring& id, const std::wstring& scriptPath,
        WidgetEngine* engine, ID2D1DeviceContext* context,
        RECT frame, int columns, int rows);
    bool SafeReadFlags(WidgetEngine* engine, const std::wstring& scriptPath,
        bool& showTitle, bool& bottomBarHover);
    void DrawInternal(ID2D1DeviceContext* context, RECT rect, int state,
        WidgetEngine* engine, bool preview);

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
    int visibleHeight, int scrollOffset, bool hovered, bool lightTheme, float cellScale = 1.0f);

void DrawHorizontalScrollbarAt(ID2D1DeviceContext* context, RECT body,
    int contentWidth, int visibleWidth, int scrollOffset, bool hovered,
    bool lightTheme, float cellScale = 1.0f);
