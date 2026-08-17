#pragma once

#include "container.h"
#include "item.h"
#include "types.h"

#include <memory>
#include <vector>

class DesktopApp;

class DockRunningItem final : public Item
{
public:
    DockRunningItem(DesktopApp* app, Container* container, size_t runningIndex);

    std::wstring GetTitle() const override;
    std::wstring GetPath() const override;
    HBITMAP GetIconBitmap() const override;
    RECT GetBounds() const override;
    void SetBounds(RECT bounds) override;
    bool IsSelected() const override;
    void SetSelected(bool selected) override;
    Container* GetContainer() const override;
    void Draw(ID2D1DeviceContext* context, RECT rect, int state) override;
    ComPtr<IDataObject> CreateDataObject() override { return nullptr; }

    size_t GetRunningIndex() const { return runningIndex_; }
    std::wstring GetIdentityKey() const;

private:
    DesktopApp* app_ = nullptr;
    Container* container_ = nullptr;
    size_t runningIndex_ = static_cast<size_t>(-1);
    RECT bounds_{};
};

class DockFrequentItem final : public Item
{
public:
    DockFrequentItem(DesktopApp* app, Container* container, size_t itemIndex);

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

    size_t GetItemIndex() const { return itemIndex_; }

private:
    DesktopApp* app_ = nullptr;
    Container* container_ = nullptr;
    size_t itemIndex_ = static_cast<size_t>(-1);
    RECT bounds_{};
};

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

/** 单台显示器边缘的 Dock 容器；多个实例共享同一份 Dock 数据。 */
class DockContainer final : public Container
{
public:
    DockContainer(DesktopApp* app, std::vector<DockEntry>* entries, RECT area);

    std::wstring GetTitle() const override { return L"Dock"; }
    snowdesktop::slot_contract::SlotSurfaceKind
        GetSlotSurfaceKind() const override
    {
        return snowdesktop::slot_contract::
            SlotSurfaceKind::Dock;
    }
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
    bool ScrollByWheelDelta(POINT pointer, int wheelDelta);
    bool IsWindowsButtonPoint(POINT pt) const;
    bool IsSearchPoint(POINT pt) const;
    DockEntryItem* EntryAtPoint(POINT pt) const;
    DockRunningItem* RunningItemAtPoint(POINT pt) const;
    DockFrequentItem* FrequentItemAtPoint(POINT pt) const;
    RECT GetWindowsButtonRect() const;
    RECT GetSearchRect() const;
    RECT GetInteractiveBounds() const;
    bool ContainsInteractivePoint(POINT pt) const;
    RECT GetElementVisualRect(RECT baseRect, POINT pointer) const;
    RECT GetVisualPanelBounds(POINT pointer) const;
    RECT GetHoveredTitleBounds(POINT pointer) const;
    RECT GetDesktopItemVisualRect(
        size_t itemIndex, POINT pointer) const;
    void SetReservedArea(RECT area);
    size_t GetDropInsertIndex(Slot* slot, HitRegion region) const
    { return InsertIndexFor(slot, region); }
    size_t GetInsertIndexAtPoint(POINT pt) const;
    void DrawInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex) const;

private:
    enum class MagnificationZone
    {
        None,
        Leading,
        Trailing,
    };

    bool IsVertical() const;
    bool IsEdgeAttached() const;
    void RefreshEntryGroupCounts() const;
    size_t SortableEntryCount() const;
    size_t FolderEntryCount() const;
    size_t FolderEntryBegin() const;
    bool HasOnlyRecycleBinDragSource() const;
    bool HasOnlyFolderDragSource() const;
    int ItemIconSize() const;
    int ItemPitch() const;
    int ScaledSpacing() const;
    int ScaledSeparatorGap() const;
    int EdgeMargin() const;
    std::vector<RECT> GetLeadingMagnificationRects() const;
    std::vector<RECT> GetTrailingMagnificationRects() const;
    MagnificationZone GetMagnificationZone(
        const RECT& baseRect) const;
    std::vector<RECT> GetElementBaseRects() const;
    RECT ResolveMagnificationFocusRect(POINT pointer) const;
    float GetMagnificationScale(
        const RECT& baseRect, const RECT& focusRect,
        POINT pointer) const;
    int GetMagnificationAxisShift(
        const RECT& baseRect, const RECT& focusRect,
        POINT pointer) const;
    RECT CalculateTitleTooltipBounds(
        const std::wstring& title,
        const RECT& hoveredBounds,
        IDWriteTextFormat* measurementFormat =
            nullptr) const;
    RECT PositionTitleTooltipBounds(
        const RECT& hoveredBounds,
        int tooltipWidth,
        int tooltipHeight) const;
    bool IsFocusedElementRect(const RECT& baseRect, POINT pointer) const;
    RECT GetScrollViewport(const RECT& bounds) const;
    // Folder entries share the Dock's single scroll viewport/offset. These
    // accessors keep the semantic group boundary explicit for hit testing.
    int GetMaxScrollOffset(const RECT& bounds) const;
    bool IsPointInScrollViewport(POINT point) const;
    size_t InsertIndexFor(Slot* slot, HitRegion region) const;

    DesktopApp* app_ = nullptr;
    std::vector<DockEntry>* entries_ = nullptr;
    RECT area_{};
    mutable std::vector<std::unique_ptr<DockEntryItem>> entryItems_;
    mutable std::vector<std::unique_ptr<DockRunningItem>> runningItems_;
    mutable std::vector<std::unique_ptr<DockFrequentItem>> frequentItems_;
    mutable std::uint64_t entryGroupCountGeneration_ = 0;
    mutable size_t mainEntryCount_ = 0;
    mutable size_t folderEntryCount_ = 0;
    mutable int scrollOffset_ = 0;
    // Cache the measured tooltip size separately from its visual anchor so
    // pointer-driven magnification can move the chip without rebuilding a
    // DirectWrite layout for every WM_MOUSEMOVE.
    mutable std::wstring hoveredTitleBoundsCacheText_;
    mutable RECT hoveredTitleBoundsCacheAnchor_{};
    mutable RECT hoveredTitleBoundsCache_{};
    mutable int hoveredTitleBoundsCachePosition_ = -1;
    mutable bool hoveredTitleBoundsCacheLightTheme_ = false;
    // Magnification is a continuous pointer-distance field, but the semantic
    // hover owner needs spatial hysteresis at item and Dock boundaries.
    mutable RECT magnificationFocusRect_{};
};
