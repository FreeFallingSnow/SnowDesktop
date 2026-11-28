#pragma once

#include "container.h"
#include "item.h"
#include "common/types.h"
#include "dock/dock_magnification.h"
#include "dock/dock_layout_settings.h"
#include "dock/dock_running_drag_rules.h"
#include "dock/dock_folder_rules.h"
#include "dock/dock_entry_removal.h"
#include "ui/render/native_tooltip_content.h"

#include <memory>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

struct DragSourceList;
struct DropPreviewList;

class DesktopApp;
namespace snowdesktop { class NativeTooltip; }

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
    bool CanStartDrag() const;

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
    DropReceipt OnItemsDropped(const ItemDrop& drop) override;
    void DrawChrome(ID2D1DeviceContext* context, POINT mousePt) override;
    void DrawContents(ID2D1DeviceContext* context) override;
    RECT GetBounds() const override;
    BarStyle GetInsertionStyle() const override;
    std::vector<Item*> GetSelectedItems() const override;
    HitRegion HitTestDrag(POINT pt, Slot*& outSlot) override;
    std::wstring GetDragHint(Slot* slot, HitRegion region,
        const std::vector<Item*>& sourceItems, Container* origin, int mods) const override;
    void DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region) override;

    static bool PreviewFreeOrderForDrag(DesktopApp& app);
    static void FinishFreeOrderForDrag(DesktopApp& app, bool dropped);
    HitRegion ResolveFreeOrderPreviewTarget(POINT point, Slot*& slot, HitRegion region);

    size_t Capacity() const;
    bool HasCapacity(size_t additional) const;
    bool ScrollByWheelDelta(POINT pointer, int wheelDelta);
    bool CanScrollAt(POINT pointer) const
    {
        return IsPointInScrollViewport(pointer) && GetMaxScrollOffset(GetBounds()) > 0;
    }
    struct KeyboardTarget
    {
        enum class Kind { Start, Search, Entry, Running, Frequent, WindowTitle };
        Kind kind;
        size_t index;
        std::wstring key;
        RECT bounds;
        bool focused;
        bool scrollable;
    };
    std::vector<KeyboardTarget> KeyboardTargets(bool alongDock = false);
    std::optional<KeyboardTarget> FocusedKeyboardTarget();
    void SetKeyboardFocus(const KeyboardTarget& target);
    bool ClearKeyboardFocus();
    bool IsWindowsButtonPoint(POINT pt) const;
    bool IsSearchPoint(POINT pt) const;
    DockEntryItem* EntryAtPoint(POINT pt) const;
    DockRunningItem* RunningItemAtPoint(POINT pt) const;
    DockFrequentItem* FrequentItemAtPoint(POINT pt) const;
    RECT GetWindowsButtonRect() const;
    RECT GetSearchRect() const;
    RECT GetInteractiveBounds() const;
    RECT GetMinimizedThumbnailAnchor(HWND window, POINT pointer, bool restoring) const;
    HMONITOR GetMonitor() const;
    RECT GetAnimationVisualBounds(bool reserveForLaunch = false) const;
    std::vector<RECT> GetOcclusionRects(POINT pointer) const;
    bool ContainsInteractivePoint(POINT pt) const;
    RECT GetElementVisualRect(RECT baseRect, POINT pointer) const;
    int GetElementIconSize(const RECT& baseRect, const RECT& visualRect) const;
    RECT GetVisualPanelBounds(POINT pointer) const;
    void ShowHoveredTitle(POINT pointer, HWND owner, snowdesktop::NativeTooltip& tooltip) const;
    RECT GetDesktopItemVisualRect(
        size_t itemIndex, POINT pointer) const;
    void SetReservedArea(RECT area);
    DockMonitorLayout MonitorLayout() const;
    DockPosition Position() const;
    bool ShowsRunningArea() const;
    std::span<const HWND> EntryTitleWindows(size_t index) const;
    std::span<const HWND> RunningTitleWindows(size_t index) const;
    RECT DrawWindowTitleGroup(ID2D1DeviceContext* context, RECT base, RECT visual,
        std::span<const HWND> windows, float opacity = 1.f) const;
    RECT WindowTitleIconRect(RECT visual, std::span<const HWND> windows) const;
    HWND WindowTitleAtPoint(POINT point, RECT* titleRect = nullptr) const;
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
        std::wstring keyboardFocusKey;
    };
    PresentationState CapturePresentationState() const;
    bool RestorePresentationState(const PresentationState& state);
    DropPreviewList FilePinPreview(const DragSourceList& sources, size_t insertIndex, POINT point) const;
    size_t GetDropInsertIndex(Slot* slot, HitRegion region) const
    { return InsertIndexFor(slot, region); }
    // A delayed external producer has not supplied its item types yet. Keep
    // the hit boundary until materialization can split the actual batch.
    size_t GetExternalDropInsertIndex(Slot* slot, HitRegion region) const;
    size_t GetInsertIndexAtPoint(POINT pt) const;
    std::optional<size_t> GetSeparatorInsertIndex(POINT point) const;
    DockEntrySection GetSectionAtPoint(POINT point) const;
    snowdesktop::dock_running_drag::Target GetRunningDragTarget(POINT pt) const;
    void DrawInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex) const;

private:
    enum class MagnificationZone
    {
        None,
        Leading,
        Trailing,
    };

    struct MagnificationLayout;

    bool IsMagnificationSuppressed() const;
    bool UsesSpreadEdge() const;
    struct WindowTitleLayout;
    WindowTitleLayout* MeasureWindowTitle(HWND window, int icon) const;
    float WindowTitleExtent(HWND window, int icon, int spacing, bool vertical) const;
    float WindowTitleSlotUnits(std::span<const HWND> windows) const;
    std::vector<RECT> WindowTitleSegments(RECT visual, std::span<const HWND> windows) const;
    RECT EdgeChromeBounds(RECT bounds) const;
    float GetMaximumMagnificationScale() const;
    float GetCurrentMagnificationScale() const;
    int GetLaunchAnimationPadding(bool reserveForLaunch = false) const;
    bool IsVertical() const;
    bool IsEdgeAttached() const;
    void RefreshEntryGroupCounts() const;
    float FixedEntryUnits() const;
    float FolderEntryUnits() const;
    float SeparatorSlotUnits() const;
    size_t ModelEntryCount() const;
    bool HasImplicitSearchSlot() const;
    RECT FirstVisibleFileSlot() const;
    size_t SortableEntryCount() const;
    size_t FolderEntryCount() const;
    size_t FolderEntryBegin() const;
    size_t EmptyRunningDragFixedSlots() const;
    size_t EmptyRunningDragFileSlots() const;
    bool HasOnlyFolderDragSource() const;
    bool IsFolderDragSource(Item* source) const;
    bool HasMixedFolderDragSource() const;
    snowdesktop::dock_folder_rules::InsertRange GetDragInsertRange(POINT point) const;
    void DrawPartitionInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex,
        snowdesktop::dock_folder_rules::InsertRange range, bool folderSource) const;
    int ItemIconSize() const;
    int ItemPitch() const;
    size_t PendingFileCount(bool folders) const;
    float AdditionalSlotUnits(bool folders) const;
    std::vector<std::pair<RECT, std::shared_ptr<FolderEntry>>> pendingFileCells_;
    int ScaledSpacing() const;
    int ScaledSeparatorGap() const;
    float RunningSlotUnits() const;
    RECT BoundsForRunningUnits(float runningUnits) const;
    std::vector<std::wstring> EntryRemovalKeys() const;
    void ReconcileEntryRemovals() const;
    float EntryRemovalUnits(bool folder) const;
    void DrawEntryRemovals(ID2D1DeviceContext* context, bool fixedFolders) const;
    int EdgeMargin() const;
    MagnificationLayout GetMagnificationLayout() const;
    RECT GetWindowsButtonRect(const RECT& bounds) const;
    RECT ResolveMagnificationFocusRect(POINT pointer, const MagnificationLayout& layout) const;
    snowdesktop::dock_magnification::SingleFocusGeometry GetSingleMagnificationGeometry(
        const RECT& baseRect, const MagnificationLayout& layout) const;
    RECT MagnifyElementRect(const RECT& baseRect, const RECT& focusRect,
        POINT pointer, const MagnificationLayout& layout) const;
    RECT GetVisualPanelBounds(POINT pointer, const MagnificationLayout& layout, const RECT& focus) const;
    RECT GetHoveredTitleBounds(POINT pointer) const;
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
    bool IsFocusedElementRect(const RECT& baseRect, POINT pointer,
        const MagnificationLayout& layout, const RECT& focus) const;
    RECT GetScrollViewport(const RECT& bounds) const;
    RECT GetScrollViewport(const RECT& bounds, float runningUnits) const;
    RECT GetVisualScrollViewport(POINT pointer) const;
    RECT GetVisualScrollViewport(POINT pointer, const MagnificationLayout& layout, const RECT& focus) const;
    // Grouped files and utility controls keep their trailing layout.
    int GetMaxScrollOffset(const RECT& bounds) const;
    int GetMaxScrollOffset(const RECT& bounds, float runningUnits) const;
    bool IsPointInScrollViewport(POINT point) const;
    bool IsPointInScrollViewport(POINT point, RECT viewport) const;
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
    mutable size_t separatorEntryCount_ = 0;
    mutable size_t folderSeparatorCount_ = 0;
    mutable size_t quickSearchEntryIndex_ = static_cast<size_t>(-1);
    mutable int scrollOffset_ = 0;
    std::wstring keyboardFocusKey_;
    // Cache the measured tooltip size separately from its visual anchor so
    // pointer-driven magnification can move the chip without rebuilding a
    // DirectWrite layout for every WM_MOUSEMOVE.
    mutable std::wstring hoveredTitleBoundsCacheText_;
    mutable RECT hoveredTitleBoundsCacheAnchor_{};
    mutable RECT hoveredTitleBoundsCache_{};
    mutable int hoveredTitleBoundsCachePosition_ = -1;
    mutable bool hoveredTitleBoundsCacheLightTheme_ = false;
    mutable float hoveredTitleBoundsCacheFontSize_ = 0;
    mutable snowdesktop::NativeTooltipTextLayout hoveredTitleLayout_;
    mutable HWND titleMarqueeWindow_ = nullptr;
    mutable ULONGLONG titleMarqueeStart_ = 0;
    mutable bool titleMarqueeAnimating_ = false;
    struct WindowTitleLayout
    {
        std::wstring title;
        std::uint64_t fontRevision = 0;
        float fontSize = 0, fullWidth = 0;
        Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
        Microsoft::WRL::ComPtr<IDWriteTextLayout> trimmed, full;
    };
    mutable std::unordered_map<HWND, WindowTitleLayout> windowTitleLayouts_;
    // Magnification is a continuous pointer-distance field, but the semantic
    // hover owner needs spatial hysteresis at item and Dock boundaries.
    mutable RECT magnificationFocusRect_{};
    mutable snowdesktop::dock_magnification::HoverEntryAnimation
        magnificationEntry_;
    mutable snowdesktop::dock_magnification::SingleFocusAnimation
        singleMagnification_;
};
