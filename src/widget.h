#pragma once
#include "item.h"
#include "container.h"
#include "slot.h"
#include <string>
#include <vector>
#include <memory>

struct DesktopWidget;
class SnowDesktopApp;

// ── Widget : pure Item (draggable, renderable) ──────────────
// Does NOT inherit Container — used for LuaScript which doesn't accept drops.
class Widget : public Item
{
public:
    Widget(DesktopWidget* data, SnowDesktopApp* app);
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
    SnowDesktopApp* GetApp() const { return app_; }

protected:
    DesktopWidget* data_;
    SnowDesktopApp* app_;
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

    // Subclasses override BuildSlots + OnItemsDropped + DrawChrome
};

// ── Concrete widget types ───────────────────────────────────

class Collection : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;
};

class FileCategories : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;
};

class FolderMapping : public WidgetContainer
{
public:
    using WidgetContainer::WidgetContainer;
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;
};

// LuaScript: pure Widget, no Container
class LuaScript : public Widget
{
public:
    using Widget::Widget;
};

// Factory
std::unique_ptr<Widget> CreateWidget(DesktopWidget* data, SnowDesktopApp* app);
