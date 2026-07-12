#pragma once

#include "container.h"
#include "item.h"
#include "types.h"

#include <memory>
#include <vector>

class DesktopApp;

/** Dock 中用于绘制和拖拽的轻量引用项。 */
class DockEntryItem final : public Item
{
public:
    DockEntryItem(DesktopApp* app, Container* container, size_t entryIndex);

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

    size_t GetEntryIndex() const { return entryIndex_; }
    DockEntryType GetEntryType() const;
    std::wstring GetReference() const;

private:
    const DockEntry* Entry() const;

    DesktopApp* app_ = nullptr;
    Container* container_ = nullptr;
    size_t entryIndex_ = static_cast<size_t>(-1);
    RECT bounds_{};
};

/** 首屏底部的单行 Dock 容器。 */
class DockContainer final : public Container
{
public:
    DockContainer(DesktopApp* app, std::vector<DockEntry>* entries, RECT area);

    std::wstring GetTitle() const override { return L"Dock"; }
    std::vector<std::unique_ptr<Slot>> BuildSlots() override;
    void OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
        Slot* targetSlot, HitRegion region, int mods) override;
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;
    void DrawContents(ID2D1DeviceContext* context) override;
    RECT GetBounds() const override;
    BarStyle GetInsertionStyle() const override;
    std::vector<Item*> GetSelectedItems() const override;
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const override;
    void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region) override;

    size_t Capacity() const;
    bool HasCapacity(size_t additional) const;
    bool IsSearchPoint(POINT pt) const;
    DockEntryItem* EntryAtPoint(POINT pt) const;
    RECT GetSearchRect() const;
    size_t GetDropInsertIndex(Slot* slot, HitRegion region) const
    { return InsertIndexFor(slot, region); }
    size_t GetInsertIndexAtPoint(POINT pt) const;
    void DrawInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex) const;

private:
    bool IsVertical() const;
    bool IsEdgeAttached() const;
    int ItemPitch() const;
    int EdgeMargin() const;
    size_t InsertIndexFor(Slot* slot, HitRegion region) const;

    DesktopApp* app_ = nullptr;
    std::vector<DockEntry>* entries_ = nullptr;
    RECT area_{};
    mutable std::vector<std::unique_ptr<DockEntryItem>> entryItems_;
};
