#pragma once

#include "container.h"
#include "item.h"
#include "common/types.h"
#include "dock/dock_magnification.h"
#include "dock/dock_running_drag_rules.h"
#include "dock/dock_folder_rules.h"
#include "dock/dock_entry_removal.h"
#include "ui/render/inline_tooltip_backdrop.h"

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

    size_t GetRunningIndex() const;
    std::wstring GetIdentityKey() const;

private:
    DesktopApp* app_ = nullptr;
    Container* container_ = nullptr;
    size_t runningIndex_ = static_cast<size_t>(-1);
    std::wstring identityKey_;
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
    void ClearTitleTooltipBackdrop();
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
    RECT GetAnimationVisualBounds(bool reserveForLaunch = false) const;
    std::vector<RECT> GetOcclusionRects(POINT pointer) const;
    bool ContainsInteractivePoint(POINT pt) const;
    RECT GetElementVisualRect(RECT baseRect, POINT pointer) const;
    int GetElementIconSize(const RECT& baseRect, const RECT& visualRect) const;
    RECT GetVisualPanelBounds(POINT pointer) const;
    RECT GetHoveredTitleBounds(POINT pointer) const;
    RECT GetDesktopItemVisualRect(
        size_t itemIndex, POINT pointer) const;
    void SetReservedArea(RECT area);
    // Stable layout relationship, including while the bar is fullscreen-hidden.
    bool IsMergedWithStatusBar() const;
    // Shared chrome is a layout relationship, independent of animation/input.
    bool SharesStatusBarAppearance() const;
    bool IsMagnificationAnimating() const;
    bool AdvanceMagnificationAnimation(double nowMilliseconds);
    void PrepareEntryRemoval(size_t entryIndex);
    void CancelEntryRemoval(const DockEntry& entry);
    bool IsEntryRemovalAnimating() const;
    bool AdvanceEntryRemovalAnimation(double nowMilliseconds);
    using EntryRemovalTimeline = snowdesktop::dock_entry_removal::Timeline<ComPtr<ID2D1Bitmap1>>;
    // Host runtime-tree rebuilds replace item wrappers, but a stationary
    // pointer must keep its presentation timeline. This snapshot owns no
    // Container, Item or Slot pointers.
    struct PresentationState
    {
        RECT reservedArea{};
        RECT focusRect{};
        int scrollOffset = 0;
        snowdesktop::dock_magnification::HoverEntryAnimation entry;
        snowdesktop::dock_magnification::SingleFocusAnimation single;
        std::shared_ptr<EntryRemovalTimeline> removals;
    };
    PresentationState CapturePresentationState() const;
    bool RestorePresentationState(const PresentationState& state);
    size_t GetDropInsertIndex(Slot* slot, HitRegion region) const
    { return InsertIndexFor(slot, region); }
    // A delayed external producer has not supplied its item types yet. Keep
    // the hit boundary until materialization can split the actual batch.
    size_t GetExternalDropInsertIndex(Slot* slot, HitRegion region) const;
    size_t GetInsertIndexAtPoint(POINT pt) const;
    snowdesktop::dock_running_drag::Target GetRunningDragTarget(POINT pt) const;
    void DrawInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex) const;

private:
    enum class MagnificationZone
    {
        None,
        Leading,
        Trailing,
    };

    bool IsMagnificationSuppressed() const;
    bool UsesEdgeAnchoredMagnification() const;
    float GetMaximumMagnificationScale() const;
    float GetCurrentMagnificationScale() const;
    int GetLaunchAnimationPadding(bool reserveForLaunch = false) const;
    bool IsVertical() const;
    bool IsEdgeAttached() const;
    void RefreshEntryGroupCounts() const;
    size_t SortableEntryCount() const;
    size_t FolderEntryCount() const;
    size_t FolderEntryBegin() const;
    size_t EmptyRunningDragFixedSlots() const;
    size_t EmptyRunningDragFileSlots() const;
    bool HasOnlyRecycleBinDragSource() const;
    bool HasOnlyFolderDragSource() const;
    bool IsFolderDragSource(Item* source) const;
    bool HasMixedFolderDragSource() const;
    snowdesktop::dock_folder_rules::InsertRange GetDragInsertRange() const;
    void DrawPartitionInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex,
        snowdesktop::dock_folder_rules::InsertRange range, bool folderSource) const;
    int ItemIconSize() const;
    int ItemPitch() const;
    int ScaledSpacing() const;
    int ScaledSeparatorGap() const;
    float RunningSlotUnits() const;
    std::vector<std::wstring> EntryRemovalKeys() const;
    void ReconcileEntryRemovals() const;
    float EntryRemovalUnits(bool folder) const;
    void DrawEntryRemovals(ID2D1DeviceContext* context, bool fixedFolders) const;
    int EdgeMargin() const;
    std::vector<RECT> GetLeadingMagnificationRects() const;
    std::vector<RECT> GetTrailingMagnificationRects() const;
    MagnificationZone GetMagnificationZone(
        const RECT& baseRect) const;
    std::vector<RECT> GetElementBaseRects() const;
    RECT ResolveMagnificationFocusRect(POINT pointer) const;
    snowdesktop::dock_magnification::SingleFocusGeometry
        GetSingleMagnificationGeometry(const RECT& baseRect) const;
    float GetMagnificationScale(
        const RECT& baseRect, const RECT& focusRect,
        POINT pointer) const;
    int GetMagnificationAxisShift(
        const RECT& baseRect, const RECT& focusRect,
        POINT pointer) const;
    RECT MagnifyElementRect(const RECT& baseRect, const RECT& focusRect, POINT pointer) const;
    float TitleTooltipScale() const;
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
    RECT GetVisualScrollViewport(POINT pointer) const;
    // Folder entries share the Dock's single scroll viewport/offset. These
    // accessors keep the semantic group boundary explicit for hit testing.
    int GetMaxScrollOffset(const RECT& bounds) const;
    bool IsPointInScrollViewport(POINT point) const;
    size_t InsertIndexFor(Slot* slot, HitRegion region) const;
    HitRegion RedirectDragToNearestInsertion(
        POINT point, Slot*& outSlot);

    DesktopApp* app_ = nullptr;
    std::vector<DockEntry>* entries_ = nullptr;
    RECT area_{};
    mutable std::vector<std::unique_ptr<DockEntryItem>> entryItems_;
    mutable std::vector<std::unique_ptr<DockRunningItem>> runningItems_;
    mutable std::vector<std::unique_ptr<DockFrequentItem>> frequentItems_;
    std::shared_ptr<EntryRemovalTimeline> entryRemovals_ = std::make_shared<EntryRemovalTimeline>();
    std::vector<std::pair<size_t, RECT>> entryRemovalCells_;
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
    mutable float hoveredTitleBoundsCacheFontSize_ = 0;
    snowdesktop::InlineTooltipBackdrop titleTooltipBackdrop_;
    // Magnification is a continuous pointer-distance field, but the semantic
    // hover owner needs spatial hysteresis at item and Dock boundaries.
    mutable RECT magnificationFocusRect_{};
    mutable snowdesktop::dock_magnification::HoverEntryAnimation
        magnificationEntry_;
    mutable snowdesktop::dock_magnification::SingleFocusAnimation
        singleMagnification_;
};
