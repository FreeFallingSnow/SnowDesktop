#pragma once
#include "item.h"
#include "container.h"
#include "slot.h"
#include <string>
#include <vector>
#include <memory>

struct DesktopWidget;
class DesktopApp;
struct GridPage;

// ── WidgetHit — granular hit-test result for chrome elements ──
enum class WidgetHit {
    None,
    Content,        // member item area
    MoveHandle,     // bottom bar (except resize corner) — drag to move
    ResizeHandle,   // bottom-right 24px corner — drag to resize
    ListToggleBtn,  // FolderMapping: toggle icon/list
    OpenFolderBtn,  // FolderMapping: open source folder
    CategoryTab,    // FileCategories: category tab
    CollectionOpenBtn, // Collection: compact body / "all" mosaic
};

// ── Widget : pure Item (draggable, renderable) ──────────────
// Does NOT inherit Container — used for LuaScript which doesn't accept drops.
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

protected:
    DesktopWidget* data_;
    DesktopApp* app_;
};

// ── WidgetContainer : Widget + ListContainer ─
// Widget that can receive drops. Uses 1D list layout (ListContainer).
// Does NOT accept other Widgets as drop targets.
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
    virtual Item* GetMemberItem(size_t memberIndex) const { return nullptr; }
    virtual std::vector<size_t> GetSelectedMemberIndices() const { return {}; }
    virtual void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) {}
    virtual size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const;
    virtual bool AllowsDesktopKey(const std::wstring& key) const { (void)key; return true; }

    // ── Content — subclasses override ────────────────────
    virtual void DrawContent(ID2D1DeviceContext* context, RECT body) {}
    virtual void DrawButtons(ID2D1DeviceContext* context, RECT handleRect, bool hovered) {}

    // ── Scrollbar — subclasses override ──────────────────
    virtual int  GetScrollOffset() const { return 0; }
    virtual int  GetMaxScrollOffset() const { return 0; }
    virtual int  GetTotalContentHeight() const { return 0; }
    virtual int  GetVisibleContentHeight() const { return 0; }
    void DrawScrollbar(ID2D1DeviceContext* context, bool hovered) const;

protected:
    mutable std::vector<std::unique_ptr<Item>> dragSourceCache_;
    mutable std::vector<std::unique_ptr<Item>> slotItemCache_;
};

// ── ScrollingItemWidget : WidgetContainer with list/icon toggle ─
// Shared base for FileCategories and FolderMapping.
// Provides SingleColumn (listMode-based), shared list-mode item
// rendering (DrawListItem), and scroll metrics boilerplate.
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
        HBITMAP iconBitmap, const std::wstring& name, bool selected) const;

    BarStyle GetInsertionStyle() const override;
};

// ── Concrete widget types ───────────────────────────────────

class Collection : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawContent(ID2D1DeviceContext* context, RECT body) override;
    WidgetHit HitTestWidget(POINT pt) const override;
    std::wstring CategoryIdAtPoint(POINT pt) const;
    std::vector<Item*> GetSelectedItems() const override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
    Item* GetMemberItem(size_t idx) const override;
    std::vector<size_t> GetSelectedMemberIndices() const override;
    void ReorderMembers(const std::vector<size_t>& indices, size_t insertBefore) override;
    size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const override;
    BarStyle GetInsertionStyle() const override { return BarStyle::VBar; }

    size_t GetSlotCount() const override;
    int  GetItemHeight() const override { return 136; }
    int  GetItemWidth()  const override { return 92; }
    Item* GetSlotItem(size_t idx) const override;

    // Returns the rect of the "all" mosaic button (empty if none).
    RECT GetAllButtonRect() const;

private:
    void DrawThumbnail(ID2D1DeviceContext* context, const DesktopItem& item,
        RECT rect, bool selected) const;
};

class FileCategories : public ScrollingItemWidget
{
public:
    using ScrollingItemWidget::ScrollingItemWidget;
    bool CollectTopLevelDesktopItems();
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
};

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
    size_t GetDropInsertIndex(Slot* targetSlot, HitRegion region) const override;

    size_t GetSlotCount() const override;
    int  GetItemHeight() const override;
    int  GetItemWidth()  const override;
    bool IncludeTrailingEmptySlot() const override { return true; }
    Item* GetSlotItem(size_t idx) const override;

    int GetMaxScrollOffset() const override;
    int GetTotalContentHeight() const override;
    int GetVisibleContentHeight() const override;
    bool NeedsShellReloadAfterDrop() const override { return false; }
};

// LuaScript: pure Widget, no Container
class LuaScript : public Widget
{
public:
    using Widget::Widget;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;
};

// Factory
std::unique_ptr<Widget> CreateWidget(DesktopWidget* data, DesktopApp* app);

// Shared scrollbar helper — used by WidgetContainer and Collection popup
void DrawScrollbarAt(ID2D1DeviceContext* context, RECT body, int contentHeight,
    int visibleHeight, int scrollOffset, bool hovered);
