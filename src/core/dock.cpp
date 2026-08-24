#include "dock.h"

#include "app.h"
#include "constants.h"
#include "../dock_magnification.h"
#include "slot.h"
#include "../l10n.h"
#include "../item_location.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <shlwapi.h>

DockRunningItem::DockRunningItem(
    DesktopApp* app, Container* container, size_t runningIndex)
    : app_(app), container_(container), runningIndex_(runningIndex) {}

std::wstring DockRunningItem::GetTitle() const
{
    if (!app_ || runningIndex_ >= app_->dockUnpinnedRunningApps_.size())
        return L"";
    const DockRunningAppInfo& running =
        app_->dockUnpinnedRunningApps_[runningIndex_];
    if (!app_->generalSettings_.demoModeEnabled ||
        !app_->demoIdentityAssetsAvailable_)
        return running.title;
    const std::wstring_view identity = running.identityKey.empty()
        ? std::wstring_view(running.executablePath)
        : std::wstring_view(running.identityKey);
    return app_->GetDemoIdentityTitle(identity);
}

std::wstring DockRunningItem::GetPath() const
{
    return app_ && runningIndex_ < app_->dockUnpinnedRunningApps_.size()
        ? app_->dockUnpinnedRunningApps_[runningIndex_].executablePath : L"";
}

HBITMAP DockRunningItem::GetIconBitmap() const
{
    return app_ && runningIndex_ < app_->dockUnpinnedRunningApps_.size()
        ? app_->dockUnpinnedRunningApps_[runningIndex_].iconBitmap : nullptr;
}

RECT DockRunningItem::GetBounds() const { return bounds_; }
void DockRunningItem::SetBounds(RECT bounds) { bounds_ = bounds; }

bool DockRunningItem::IsSelected() const
{
    return false;
}

void DockRunningItem::SetSelected(bool selected)
{
    if (app_ && runningIndex_ < app_->dockUnpinnedRunningApps_.size())
        app_->dockUnpinnedRunningApps_[runningIndex_].selected = false;
    (void)selected;
}

Container* DockRunningItem::GetContainer() const { return container_; }

void DockRunningItem::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    if (app_ && runningIndex_ < app_->dockUnpinnedRunningApps_.size())
        app_->DrawDockRunningApp(
            context, app_->dockUnpinnedRunningApps_[runningIndex_], rect, state);
}

std::wstring DockRunningItem::GetIdentityKey() const
{
    return app_ && runningIndex_ < app_->dockUnpinnedRunningApps_.size()
        ? app_->dockUnpinnedRunningApps_[runningIndex_].identityKey : L"";
}

DockFrequentItem::DockFrequentItem(
    DesktopApp* app, Container* container, size_t itemIndex)
    : app_(app), container_(container), itemIndex_(itemIndex) {}

std::wstring DockFrequentItem::GetTitle() const
{
    if (!app_ || itemIndex_ >= app_->items_.size())
        return L"";
    const DesktopItem& item = app_->items_[itemIndex_];
    if (!app_->ShouldUseDemoIdentity(item))
        return item.name;
    const std::wstring_view identity = item.layoutKey.empty()
        ? std::wstring_view(item.parsingName)
        : std::wstring_view(item.layoutKey);
    return app_->GetDemoIdentityTitle(identity);
}

std::wstring DockFrequentItem::GetPath() const
{
    return app_ && itemIndex_ < app_->items_.size()
        ? app_->items_[itemIndex_].parsingName : L"";
}

HBITMAP DockFrequentItem::GetIconBitmap() const
{
    return app_ && itemIndex_ < app_->items_.size()
        ? app_->items_[itemIndex_].iconBitmap : nullptr;
}

RECT DockFrequentItem::GetBounds() const { return bounds_; }
void DockFrequentItem::SetBounds(RECT bounds) { bounds_ = bounds; }

bool DockFrequentItem::IsSelected() const
{
    return app_ && itemIndex_ < app_->items_.size() &&
        app_->GetDockWindowVisualState(itemIndex_) ==
            DockWindowVisualState::Closed &&
        app_->items_[itemIndex_].selected;
}

void DockFrequentItem::SetSelected(bool selected)
{
    if (!app_ || itemIndex_ >= app_->items_.size())
        return;
    app_->items_[itemIndex_].selected =
        selected &&
        app_->GetDockWindowVisualState(itemIndex_) ==
            DockWindowVisualState::Closed;
}

Container* DockFrequentItem::GetContainer() const { return container_; }

void DockFrequentItem::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    if (!app_ || itemIndex_ >= app_->items_.size()) return;
    DockEntry entry{ DockEntryType::DesktopItem,
        app_->items_[itemIndex_].layoutKey, true };
    app_->DrawDockEntry(context, entry, rect, state);
}

ComPtr<IDataObject> DockFrequentItem::CreateDataObject()
{
    if (!app_ || itemIndex_ >= app_->items_.size()) return nullptr;
    DesktopIcon icon(&app_->items_[itemIndex_], container_, app_);
    return icon.CreateDataObject();
}

DockEntryItem::DockEntryItem(DesktopApp* app, Container* container, size_t entryIndex)
    : app_(app), container_(container), entryIndex_(entryIndex) {}

const DockEntry* DockEntryItem::Entry() const
{
    return app_ && entryIndex_ < app_->dockEntries_.size()
        ? &app_->dockEntries_[entryIndex_] : nullptr;
}

DockEntryType DockEntryItem::GetEntryType() const
{
    const DockEntry* entry = Entry();
    return entry ? entry->type : DockEntryType::DesktopItem;
}

std::wstring DockEntryItem::GetReference() const
{
    const DockEntry* entry = Entry();
    return entry ? entry->reference : L"";
}

std::wstring DockEntryItem::GetTitle() const
{
    const DockEntry* entry = Entry();
    if (!entry || !app_) return L"";
    if (entry->type == DockEntryType::DesktopItem)
    {
        size_t index = app_->FindItemIndexByKey(entry->reference);
        if (index >= app_->items_.size()) return L"";
        const DesktopItem& item = app_->items_[index];
        if (!app_->ShouldUseDemoIdentity(item))
            return item.name;
        const std::wstring_view identity = item.layoutKey.empty()
            ? std::wstring_view(item.parsingName)
            : std::wstring_view(item.layoutKey);
        return app_->GetDemoIdentityTitle(identity);
    }
    auto it = std::find_if(app_->widgets_.begin(), app_->widgets_.end(),
        [&](const DesktopWidget& widget) { return widget.id == entry->reference; });
    if (it == app_->widgets_.end())
        return _LW("widget.collection");
    if (entry->type == DockEntryType::Collection &&
        app_->ShouldUseDemoCollectionIdentity(&*it))
        return app_->GetDemoCollectionCategoryTitle(*it);
    return it->title;
}

std::wstring DockEntryItem::GetPath() const
{
    const DockEntry* entry = Entry();
    if (!entry || !app_) return L"";
    if (app_->IsFolderDockEntry(*entry))
    {
        const auto target =
            app_->ResolveDockFolderTarget(*entry);
        return target.available ? target.path : L"";
    }
    if (entry->type != DockEntryType::DesktopItem)
        return L"";
    size_t index = app_->FindItemIndexByKey(entry->reference);
    return index < app_->items_.size() ? app_->items_[index].parsingName : L"";
}

HBITMAP DockEntryItem::GetIconBitmap() const
{
    const DockEntry* entry = Entry();
    if (!entry || !app_ || entry->type != DockEntryType::DesktopItem) return nullptr;
    size_t index = app_->FindItemIndexByKey(entry->reference);
    return index < app_->items_.size() ? app_->items_[index].iconBitmap : nullptr;
}

RECT DockEntryItem::GetBounds() const { return bounds_; }
void DockEntryItem::SetBounds(RECT bounds) { bounds_ = bounds; }

bool DockEntryItem::IsSelected() const
{
    const DockEntry* entry = Entry();
    if (!entry || !app_)
        return false;
    if (entry->type == DockEntryType::DesktopItem)
    {
        const size_t itemIndex =
            app_->FindItemIndexByKey(entry->reference);
        if (itemIndex < app_->items_.size() &&
            app_->GetDockWindowVisualState(itemIndex) !=
                DockWindowVisualState::Closed)
            return false;
    }
    return entry->selected;
}

void DockEntryItem::SetSelected(bool selected)
{
    if (!app_ || entryIndex_ >= app_->dockEntries_.size()) return;
    DockEntry& entry = app_->dockEntries_[entryIndex_];
    if (selected && entry.type == DockEntryType::DesktopItem)
    {
        const size_t itemIndex =
            app_->FindItemIndexByKey(entry.reference);
        if (itemIndex < app_->items_.size() &&
            app_->GetDockWindowVisualState(itemIndex) !=
                DockWindowVisualState::Closed)
        {
            entry.selected = false;
            return;
        }
    }
    entry.selected = selected;
}

Container* DockEntryItem::GetContainer() const { return container_; }

void DockEntryItem::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    const DockEntry* entry = Entry();
    if (entry && app_) app_->DrawDockEntry(context, *entry, rect, state);
}

ComPtr<IDataObject> DockEntryItem::CreateDataObject()
{
    const DockEntry* entry = Entry();
    if (!entry || !app_ || entry->type != DockEntryType::DesktopItem) return nullptr;
    size_t index = app_->FindItemIndexByKey(entry->reference);
    if (index >= app_->items_.size()) return nullptr;
    DesktopIcon icon(&app_->items_[index], container_, app_);
    return icon.CreateDataObject();
}

DockContainer::DockContainer(DesktopApp* app, std::vector<DockEntry>* entries, RECT area)
    : app_(app), entries_(entries), area_(area) {}

void DockContainer::SetReservedArea(RECT area)
{
    if (area_.left == area.left && area_.top == area.top &&
        area_.right == area.right && area_.bottom == area.bottom)
        return;

    area_ = area;
    InvalidateSlots();
    hoveredTitleBoundsCacheText_.clear();
    hoveredTitleBoundsCacheAnchor_ = {};
    hoveredTitleBoundsCache_ = {};
    magnificationFocusRect_ = {};
}

RECT DockContainer::GetDesktopItemVisualRect(
    size_t itemIndex, POINT pointer) const
{
    if (!app_ || itemIndex >= app_->items_.size())
        return {};
    for (const auto& item : entryItems_)
    {
        if (!item ||
            item->GetEntryType() != DockEntryType::DesktopItem ||
            app_->FindItemIndexByKey(item->GetReference()) != itemIndex)
            continue;
        return GetElementVisualRect(item->GetBounds(), pointer);
    }
    for (const auto& item : frequentItems_)
    {
        if (item && item->GetItemIndex() == itemIndex)
            return GetElementVisualRect(item->GetBounds(), pointer);
    }
    return {};
}

bool DockContainer::IsVertical() const
{
    return app_ && (app_->dockSettings_.position == DockPosition::Left ||
        app_->dockSettings_.position == DockPosition::Right);
}

bool DockContainer::IsEdgeAttached() const
{
    return app_ && app_->dockSettings_.edgeAttached;
}

void DockContainer::RefreshEntryGroupCounts() const
{
    const std::uint64_t generation =
        GetSlotGeneration();
    if (entryGroupCountGeneration_ == generation)
        return;

    mainEntryCount_ =
        app_ ? app_->DockMainEntryCount() : 0;
    folderEntryCount_ =
        app_ ? app_->DockFolderEntryCount() : 0;
    entryGroupCountGeneration_ = generation;
}

size_t DockContainer::SortableEntryCount() const
{
    RefreshEntryGroupCounts();
    return mainEntryCount_;
}

size_t DockContainer::FolderEntryCount() const
{
    RefreshEntryGroupCounts();
    return folderEntryCount_;
}

size_t DockContainer::FolderEntryBegin() const
{
    return SortableEntryCount();
}

bool DockContainer::HasOnlyRecycleBinDragSource() const
{
    if (!app_)
        return false;

    const auto& sourceItems = app_->dragSession_.Items();
    if (sourceItems.empty())
        return false;

    return std::all_of(sourceItems.begin(), sourceItems.end(),
        [&](Item* source) {
            if (auto* icon = dynamic_cast<DesktopIcon*>(source))
            {
                const DesktopItem* item = icon->GetDesktopItem();
                return item &&
                    _wcsicmp(item->desktopIconClsid.c_str(),
                        kDesktopIconClsidRecycleBin) == 0;
            }

            if (auto* dockItem = dynamic_cast<DockEntryItem*>(source))
            {
                const size_t index = dockItem->GetEntryIndex();
                return index < app_->dockEntries_.size() &&
                    app_->IsRecycleBinDockEntry(
                        app_->dockEntries_[index]);
            }
            return false;
        });
}

bool DockContainer::HasOnlyFolderDragSource() const
{
    if (!app_) return false;
    const auto& sourceItems = app_->dragSession_.Items();
    if (sourceItems.empty())
    {
        if (app_->dragDropController_.
                IsExternalDragActive())
            return app_->dragDropController_.
                ExternalSummary().foldersOnly;
        return app_->widgetAction_ ==
                DesktopApp::WidgetAction::Move &&
            app_->mouseDownWidgetIndex_ <
                app_->widgets_.size() &&
            app_->widgets_[app_->
                mouseDownWidgetIndex_].type ==
                DesktopWidgetType::FolderMapping;
    }
    return std::all_of(sourceItems.begin(), sourceItems.end(),
        [&](Item* source) {
            if (auto* dockItem = dynamic_cast<DockEntryItem*>(source))
            {
                const size_t index = dockItem->GetEntryIndex();
                return index < app_->dockEntries_.size() &&
                    app_->IsFolderDockEntry(app_->dockEntries_[index]);
            }
            if (auto* widget = dynamic_cast<Widget*>(source))
            {
                const DesktopWidget* data = widget->GetWidgetData();
                return data &&
                    data->type == DesktopWidgetType::FolderMapping;
            }
            if (auto* groupEntry =
                    dynamic_cast<FileGroupEntryItem*>(
                        source))
            {
                const size_t widgetIndex =
                    app_->FindWidgetIndexById(
                        groupEntry->
                            GetChildWidgetId());
                return widgetIndex <
                        app_->widgets_.size() &&
                    app_->widgets_[widgetIndex].
                        type ==
                        DesktopWidgetType::
                            FolderMapping;
            }
            if (auto* icon = dynamic_cast<DesktopIcon*>(source))
            {
                const DesktopItem* item = icon->GetDesktopItem();
                if (!item) return false;
                const std::wstring& reference =
                    item->layoutKey.empty()
                    ? item->parsingName
                    : item->layoutKey;
                const DockEntry probe{
                    DockEntryType::DesktopItem,
                    reference,
                    true,
                };
                return app_->ResolveDockFolderTarget(
                    probe).kind !=
                        snowdesktop::item_location::FolderTargetKind::None;
            }
            if (auto* folderEntry =
                    dynamic_cast<FolderEntryIcon*>(source))
            {
                return snowdesktop::dock_drop_rules::
                    IsFolderSourceTarget(
                        snowdesktop::item_location::
                            ResolveFolderTarget(
                                folderEntry->GetPath()).kind);
            }
            return false;
        });
}

int DockContainer::ItemIconSize() const
{
    if (!app_) return kIconSize;
    const POINT center{
        (area_.left + area_.right) / 2,
        (area_.top + area_.bottom) / 2
    };
    const GridPage* page = app_->GridPageFromPoint(center);
    const int baseIconSize = page ? app_->GetGridPageItemIconSize(*page) : kIconSize;
    return std::max(1, static_cast<int>(std::round(
        baseIconSize * ClampDockScale(app_->dockSettings_.thicknessScale))));
}

int DockContainer::ItemPitch() const
{
    return std::max(1, ItemIconSize() + ScaledSpacing());
}

int DockContainer::ScaledSpacing() const
{
    const float scale = app_
        ? ClampDockScale(app_->dockSettings_.thicknessScale) : 1.0f;
    return std::max(1, static_cast<int>(std::round(kDockSpacing * scale)));
}

int DockContainer::ScaledSeparatorGap() const
{
    const float scale = app_
        ? ClampDockScale(app_->dockSettings_.thicknessScale) : 1.0f;
    return std::max(1, static_cast<int>(std::round(kDockSeparatorGap * scale)));
}

int DockContainer::EdgeMargin() const
{
    if (app_)
    {
        const POINT center{
            (area_.left + area_.right) / 2,
            (area_.top + area_.bottom) / 2
        };
        const GridPage* page = app_->GridPageFromPoint(center);
        if (page)
            return app_->GetComponentEdgeMargin(*page, IsVertical());
    }
    return IsVertical() ? kGridMarginX : kGridMarginY;
}

BarStyle DockContainer::GetInsertionStyle() const
{
    return IsVertical() ? BarStyle::HBar : BarStyle::VBar;
}

size_t DockContainer::Capacity() const
{
    return std::numeric_limits<size_t>::max();
}

bool DockContainer::HasCapacity(size_t additional) const
{
    return entries_ && additional <= Capacity() - entries_->size();
}

RECT DockContainer::GetBounds() const
{
    const size_t count = entries_ ? entries_->size() : 0;
    const size_t runningCount = app_
        ? app_->dockUnpinnedRunningApps_.size() : 0;
    const size_t frequentCount = app_
        ? app_->GetFrequentDockItemIndices().size() : 0;
    const size_t fixedCount = SortableEntryCount();
    const bool showWindowsButton = app_ && app_->dockSettings_.showWindowsButton;
    const int nonEmptyGroupCount = static_cast<int>(fixedCount > 0) +
        static_cast<int>(runningCount > 0) + static_cast<int>(frequentCount > 0);
    const int separatorCount = nonEmptyGroupCount +
        static_cast<int>(showWindowsButton);
    const bool vertical = IsVertical();
    const int iconSize = ItemIconSize();
    const int slotLength = ItemPitch();
    const int spacing = ScaledSpacing();
    const int separatorGap = ScaledSeparatorGap();
    const int desiredLength = static_cast<int>(
        (count + runningCount + frequentCount + 1 +
            static_cast<size_t>(showWindowsButton)) * slotLength) + spacing +
        separatorCount * separatorGap;
    const int areaWidth = std::max(1, static_cast<int>(area_.right - area_.left));
    const int areaHeight = std::max(1, static_cast<int>(area_.bottom - area_.top));
    const int maxLength = std::max(1,
        (vertical ? areaHeight : areaWidth) - spacing * 2);
    const int length = std::min(desiredLength, maxLength);
    const int desiredThickness = iconSize + spacing * 2;
    const int thickness = std::min(desiredThickness,
        vertical ? areaWidth : areaHeight);
    if (IsEdgeAttached())
    {
        switch (app_->dockSettings_.position)
        {
        case DockPosition::Top:
            return RECT{ area_.left, area_.top, area_.right, area_.top + thickness };
        case DockPosition::Left:
            return RECT{ area_.left, area_.top, area_.left + thickness, area_.bottom };
        case DockPosition::Right:
            return RECT{ area_.right - thickness, area_.top, area_.right, area_.bottom };
        case DockPosition::Bottom:
        default:
            return RECT{ area_.left, area_.bottom - thickness, area_.right, area_.bottom };
        }
    }
    const int edgeDistance = EdgeMargin();
    const int innerGap = 0;
    if (vertical)
    {
        const int left = app_ && app_->dockSettings_.position == DockPosition::Left
            ? area_.left + edgeDistance
            : area_.left + innerGap;
        const int top = area_.top + (areaHeight - length) / 2;
        return RECT{ left, top, left + thickness, top + length };
    }
    const int left = area_.left + (areaWidth - length) / 2;
    const int top = app_ && app_->dockSettings_.position == DockPosition::Top
        ? area_.top + edgeDistance
        : area_.top + innerGap;
    return RECT{ left, top, left + length, top + thickness };
}

std::vector<RECT> DockContainer::GetLeadingMagnificationRects() const
{
    std::vector<RECT> candidates;
    const RECT windowsButton = GetWindowsButtonRect();
    if (!IsRectEmpty(&windowsButton))
        candidates.push_back(windowsButton);

    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const size_t fixedCount = SortableEntryCount();
    candidates.reserve(candidates.size() + fixedCount +
        runningItems_.size() + frequentItems_.size());
    for (size_t index = 0;
        index < fixedCount && index < slots.size(); ++index)
    {
        if (!slots[index]) continue;
        const RECT bounds = slots[index]->GetBounds();
        if (!IsRectEmpty(&bounds))
            candidates.push_back(bounds);
    }
    for (const auto& item : runningItems_)
    {
        if (!item) continue;
        const RECT bounds = item->GetBounds();
        if (!IsRectEmpty(&bounds))
            candidates.push_back(bounds);
    }
    for (const auto& item : frequentItems_)
    {
        if (!item) continue;
        const RECT bounds = item->GetBounds();
        if (!IsRectEmpty(&bounds))
            candidates.push_back(bounds);
    }
    return candidates;
}

std::vector<RECT> DockContainer::GetTrailingMagnificationRects() const
{
    std::vector<RECT> candidates;
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const size_t folderBegin = FolderEntryBegin();
    const size_t folderEnd = folderBegin + FolderEntryCount();
    candidates.reserve(FolderEntryCount() + 2);
    for (size_t index = folderBegin;
         index < folderEnd && index < slots.size(); ++index)
    {
        if (!slots[index]) continue;
        const RECT bounds = slots[index]->GetBounds();
        if (!IsRectEmpty(&bounds))
            candidates.push_back(bounds);
    }

    const RECT search = GetSearchRect();
    if (!IsRectEmpty(&search))
        candidates.push_back(search);

    const size_t count = entries_ ? entries_->size() : 0;
    if (count > 0 && app_ &&
        app_->IsRecycleBinDockEntry(entries_->back()) &&
        count - 1 < slots.size() && slots[count - 1])
    {
        const RECT recycleBin = slots[count - 1]->GetBounds();
        if (!IsRectEmpty(&recycleBin))
            candidates.push_back(recycleBin);
    }
    return candidates;
}

DockContainer::MagnificationZone
DockContainer::GetMagnificationZone(const RECT& baseRect) const
{
    for (const RECT& candidate : GetLeadingMagnificationRects())
    {
        if (EqualRect(&candidate, &baseRect))
            return MagnificationZone::Leading;
    }
    for (const RECT& candidate : GetTrailingMagnificationRects())
    {
        if (EqualRect(&candidate, &baseRect))
            return MagnificationZone::Trailing;
    }
    return MagnificationZone::None;
}

std::vector<RECT> DockContainer::GetElementBaseRects() const
{
    std::vector<RECT> candidates =
        GetLeadingMagnificationRects();
    std::vector<RECT> trailing =
        GetTrailingMagnificationRects();
    candidates.insert(candidates.end(),
        trailing.begin(), trailing.end());
    return candidates;
}

bool DockContainer::IsMagnificationSuppressed() const
{
    if (!app_)
        return true;
    return snowdesktop::dock_magnification::
        ShouldSuppressMagnification(
            app_->dragSession_.IsActive(),
            app_->widgetAction_ ==
                DesktopApp::WidgetAction::Move,
            app_->widgetAction_ ==
                DesktopApp::WidgetAction::Resize);
}

RECT DockContainer::ResolveMagnificationFocusRect(POINT pointer) const
{
    if (IsMagnificationSuppressed())
    {
        magnificationFocusRect_ = {};
        return RECT{};
    }

    const std::vector<RECT> candidates = GetElementBaseRects();
    const auto previous = std::find_if(
        candidates.begin(), candidates.end(),
        [&](const RECT& candidate) {
            return EqualRect(
                &candidate,
                &magnificationFocusRect_) != FALSE;
        });
    const bool magnificationActive =
        previous != candidates.end();

    auto centerDistanceSquared = [&](const RECT& rect) {
        const long long dx = static_cast<long long>(pointer.x) -
            (static_cast<long long>(rect.left) + rect.right) / 2;
        const long long dy = static_cast<long long>(pointer.y) -
            (static_cast<long long>(rect.top) + rect.bottom) / 2;
        return dx * dx + dy * dy;
    };
    const auto resolveRawFocus = [&]() {
        RECT best{};
        long long bestDistance =
            std::numeric_limits<long long>::max();
        for (const RECT& candidate : candidates)
        {
            if (!PtInRect(&candidate, pointer))
                continue;
            const long long distance =
                centerDistanceSquared(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }
        if (!IsRectEmpty(&best))
            return best;

        const RECT separatorHoverBounds =
            snowdesktop::dock_magnification::
                ResolveFocusInteractionBounds(
                    GetBounds(),
                    app_->dockSettings_.position,
                    ItemIconSize(),
                    magnificationActive);
        if (PtInRect(&separatorHoverBounds, pointer))
        {
            RECT nearest{};
            long long nearestDistance =
                std::numeric_limits<long long>::max();
            int nearestAxisDistance =
                std::numeric_limits<int>::max();
            for (const RECT& candidate : candidates)
            {
                const int candidateAxisCenter =
                    IsVertical()
                        ? (candidate.top +
                            candidate.bottom) / 2
                        : (candidate.left +
                            candidate.right) / 2;
                const int axisDistance = std::abs(
                    candidateAxisCenter -
                    (IsVertical()
                        ? pointer.y : pointer.x));
                const long long distance =
                    centerDistanceSquared(candidate);
                if (distance < nearestDistance)
                {
                    nearest = candidate;
                    nearestDistance = distance;
                    nearestAxisDistance =
                        axisDistance;
                }
            }
            const int separatorReach =
                ItemPitch() / 2 +
                ScaledSeparatorGap();
            if (!IsRectEmpty(&nearest) &&
                (!IsEdgeAttached() ||
                    nearestAxisDistance <=
                        separatorReach))
            {
                return nearest;
            }
        }

        // The expanded visual bounds are a retention area, not an
        // acquisition area. Entering from the desktop must first reach the
        // base Dock; otherwise the icons appear to magnify at a distance.
        if (!magnificationActive)
            return RECT{};

        const RECT interactive =
            snowdesktop::dock_magnification::
                ExpandInteractionBounds(
                    GetBounds(),
                    app_->dockSettings_.position,
                    ItemIconSize());
        if (!PtInRect(&interactive, pointer))
            return RECT{};

        for (const RECT& candidate : candidates)
        {
            const RECT magnified =
                snowdesktop::dock_magnification::
                    MagnifyRect(
                        candidate,
                        app_->dockSettings_.position,
                        GetMagnificationScale(
                            candidate, candidate,
                            pointer),
                        ItemIconSize(),
                        GetMagnificationAxisShift(
                            candidate, candidate,
                            pointer));
            if (!PtInRect(&magnified, pointer))
                continue;
            const long long distance =
                centerDistanceSquared(candidate);
            if (distance < bestDistance)
            {
                best = candidate;
                bestDistance = distance;
            }
        }
        return best;
    };

    RECT nextFocus = resolveRawFocus();
    if (previous != candidates.end() &&
        EqualRect(&nextFocus, &*previous) == FALSE)
    {
        if (IsRectEmpty(&nextFocus))
        {
            const RECT previousVisual =
                snowdesktop::dock_magnification::
                    MagnifyRect(
                        *previous,
                        app_->dockSettings_.position,
                        GetMagnificationScale(
                            *previous, *previous,
                            pointer),
                        ItemIconSize(),
                        GetMagnificationAxisShift(
                            *previous, *previous,
                            pointer));
            const RECT retention =
                snowdesktop::dock_magnification::
                    ExpandFocusRetentionBounds(
                        previousVisual);
            if (PtInRect(&retention, pointer))
                nextFocus = *previous;
        }
        else
        {
            const bool vertical = IsVertical();
            const int previousCenter = vertical
                ? (previous->top + previous->bottom) / 2
                : (previous->left + previous->right) / 2;
            const int nextCenter = vertical
                ? (nextFocus.top + nextFocus.bottom) / 2
                : (nextFocus.left + nextFocus.right) / 2;
            const int pointerAxis =
                vertical ? pointer.y : pointer.x;
            if (!snowdesktop::dock_magnification::
                    HasCrossedFocusSwitchBoundary(
                        previousCenter, nextCenter,
                        pointerAxis, ItemPitch()))
            {
                nextFocus = *previous;
            }
        }
    }

    magnificationFocusRect_ = nextFocus;
    return nextFocus;
}

float DockContainer::GetMagnificationScale(
    const RECT& baseRect, const RECT& focusRect,
    POINT pointer) const
{
    if (!app_ || IsRectEmpty(&focusRect))
        return 1.0f;
    if (IsEdgeAttached())
    {
        const MagnificationZone baseZone =
            GetMagnificationZone(baseRect);
        const MagnificationZone focusZone =
            GetMagnificationZone(focusRect);
        if (baseZone == MagnificationZone::None ||
            baseZone != focusZone)
            return 1.0f;
    }
    const int baseCenter = IsVertical()
        ? (baseRect.top + baseRect.bottom) / 2
        : (baseRect.left + baseRect.right) / 2;
    return snowdesktop::dock_magnification::ScaleForAxisDistance(
        static_cast<float>(
            baseCenter -
            (IsVertical()
                ? pointer.y : pointer.x)),
        ItemPitch());
}

int DockContainer::GetMagnificationAxisShift(
    const RECT& baseRect, const RECT& focusRect,
    POINT pointer) const
{
    if (!app_ || IsRectEmpty(&focusRect))
        return 0;
    if (IsEdgeAttached())
    {
        const MagnificationZone baseZone =
            GetMagnificationZone(baseRect);
        const MagnificationZone focusZone =
            GetMagnificationZone(focusRect);
        if (baseZone == MagnificationZone::None ||
            baseZone != focusZone)
            return 0;

        const std::vector<RECT> zoneRects =
            baseZone == MagnificationZone::Leading
            ? GetLeadingMagnificationRects()
            : GetTrailingMagnificationRects();
        const auto baseIt = std::find_if(
            zoneRects.begin(), zoneRects.end(),
            [&](const RECT& candidate) {
                return EqualRect(&candidate, &baseRect) != FALSE;
            });
        if (baseIt == zoneRects.end())
            return 0;

        const int pointerAxis =
            IsVertical() ? pointer.y : pointer.x;
        std::vector<float> scales;
        scales.reserve(zoneRects.size());
        for (const RECT& candidate : zoneRects)
        {
            const int candidateCenter = IsVertical()
                ? (candidate.top + candidate.bottom) / 2
                : (candidate.left + candidate.right) / 2;
            scales.push_back(
                snowdesktop::dock_magnification::
                    ScaleForAxisDistance(
                        static_cast<float>(
                            candidateCenter -
                            pointerAxis),
                        ItemPitch()));
        }
        return snowdesktop::dock_magnification::PackedAxisShift(
            scales,
            static_cast<size_t>(
                std::distance(zoneRects.begin(), baseIt)),
            ItemIconSize(),
            baseZone == MagnificationZone::Leading);
    }
    const int baseCenter = IsVertical()
        ? (baseRect.top + baseRect.bottom) / 2
        : (baseRect.left + baseRect.right) / 2;
    return snowdesktop::dock_magnification::AxisShiftForDistance(
        baseCenter -
            (IsVertical()
                ? pointer.y : pointer.x),
        ItemPitch(), ItemIconSize());
}

RECT DockContainer::GetElementVisualRect(
    RECT baseRect, POINT pointer) const
{
    if (!app_)
        return baseRect;
    const RECT focus = ResolveMagnificationFocusRect(pointer);
    return snowdesktop::dock_magnification::MagnifyRect(
        baseRect, app_->dockSettings_.position,
        GetMagnificationScale(
            baseRect, focus, pointer),
        ItemIconSize(),
        GetMagnificationAxisShift(
            baseRect, focus, pointer));
}

RECT DockContainer::GetVisualPanelBounds(POINT pointer) const
{
    RECT panel = GetBounds();
    if (IsMagnificationSuppressed())
        return panel;

    const RECT focus = ResolveMagnificationFocusRect(pointer);
    if (IsRectEmpty(&focus))
        return panel;

    for (const RECT& candidate : GetElementBaseRects())
    {
        const RECT visual = snowdesktop::dock_magnification::MagnifyRect(
            candidate, app_->dockSettings_.position,
            GetMagnificationScale(
                candidate, focus, pointer),
            ItemIconSize(),
            GetMagnificationAxisShift(
                candidate, focus, pointer));
        panel = snowdesktop::dock_magnification::
            ExtendPanelAlongDockAxis(
                panel, visual, app_->dockSettings_.position,
                ScaledSpacing() / 2);
    }
    return panel;
}

RECT DockContainer::CalculateTitleTooltipBounds(
    const std::wstring& title,
    const RECT& hoveredBounds,
    IDWriteTextFormat* measurementFormat) const
{
    if (!app_ || title.empty() ||
        !app_->dwriteFactory_)
        return RECT{};

    ComPtr<IDWriteTextFormat> tooltipFormat;
    if (!measurementFormat)
    {
        app_->dwriteFactory_->CreateTextFormat(
            L"Segoe UI", nullptr,
            app_->IsLightContentTheme()
                ? DWRITE_FONT_WEIGHT_LIGHT
                : DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            13.0f, L"zh-CN", &tooltipFormat);
        measurementFormat = tooltipFormat.Get();
    }
    if (!measurementFormat)
        return RECT{};

    ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(app_->dwriteFactory_->
            CreateTextLayout(
                title.c_str(),
                static_cast<UINT32>(title.size()),
                measurementFormat,
                240.0f, 28.0f, &layout)) &&
        layout)
    {
        layout->GetMetrics(&metrics);
    }

    const int tooltipWidth = std::clamp(
        static_cast<int>(std::ceil(
            metrics.widthIncludingTrailingWhitespace)) + 20,
        48, 260);
    constexpr int tooltipHeight = 30;
    return PositionTitleTooltipBounds(
        hoveredBounds, tooltipWidth, tooltipHeight);
}

RECT DockContainer::PositionTitleTooltipBounds(
    const RECT& hoveredBounds,
    int tooltipWidth,
    int tooltipHeight) const
{
    if (!app_ || IsRectEmpty(&hoveredBounds) ||
        tooltipWidth <= 0 || tooltipHeight <= 0)
        return RECT{};

    constexpr int tooltipGap = 8;
    RECT tooltip = snowdesktop::dock_magnification::
        AnchorTooltipBounds(
            hoveredBounds,
            app_->dockSettings_.position,
            tooltipWidth,
            tooltipHeight,
            tooltipGap);

    POINT dockCenter{
        (hoveredBounds.left + hoveredBounds.right) / 2,
        (hoveredBounds.top + hoveredBounds.bottom) / 2
    };
    for (const auto& page : app_->gridPages_)
    {
        if (!PtInRect(&page.bounds, dockCenter))
            continue;
        const int minLeft =
            static_cast<int>(page.bounds.left + 6);
        const int maxLeft = static_cast<int>(
            std::max<LONG>(
                page.bounds.left + 6,
                page.bounds.right -
                    tooltipWidth - 6));
        const int minTop =
            static_cast<int>(page.bounds.top + 6);
        const int maxTop = static_cast<int>(
            std::max<LONG>(
                page.bounds.top + 6,
                page.bounds.bottom -
                    tooltipHeight - 6));
        tooltip.left = std::clamp(
            static_cast<int>(tooltip.left),
            minLeft, maxLeft);
        tooltip.top = std::clamp(
            static_cast<int>(tooltip.top),
            minTop, maxTop);
        tooltip.right =
            tooltip.left + tooltipWidth;
        tooltip.bottom =
            tooltip.top + tooltipHeight;
        break;
    }
    return tooltip;
}

RECT DockContainer::GetHoveredTitleBounds(
    POINT pointer) const
{
    if (IsMagnificationSuppressed())
        return RECT{};

    std::wstring title;
    RECT baseBounds{};
    if (DockEntryItem* entry = EntryAtPoint(pointer))
    {
        title = entry->GetTitle();
        baseBounds = entry->GetBounds();
    }
    else if (DockRunningItem* running =
        RunningItemAtPoint(pointer))
    {
        title = running->GetTitle();
        baseBounds = running->GetBounds();
    }
    else if (DockFrequentItem* frequent =
        FrequentItemAtPoint(pointer))
    {
        title = frequent->GetTitle();
        baseBounds = frequent->GetBounds();
    }
    else if (IsWindowsButtonPoint(pointer))
    {
        title = _LW("app.dock.start_menu");
        baseBounds = GetWindowsButtonRect();
    }
    else if (IsSearchPoint(pointer))
    {
        title = _LW("app.dock.quick_search");
        baseBounds = GetSearchRect();
    }
    if (title.empty() || IsRectEmpty(&baseBounds))
        return RECT{};

    const RECT visualBounds =
        GetElementVisualRect(baseBounds, pointer);

    const int position = static_cast<int>(
        app_->dockSettings_.position);
    const bool lightTheme =
        app_->IsLightContentTheme();
    const bool cachedMeasurement =
        title == hoveredTitleBoundsCacheText_ &&
        position ==
            hoveredTitleBoundsCachePosition_ &&
        lightTheme ==
            hoveredTitleBoundsCacheLightTheme_ &&
        !IsRectEmpty(&hoveredTitleBoundsCache_);
    if (cachedMeasurement)
    {
        if (EqualRect(
                &visualBounds,
                &hoveredTitleBoundsCacheAnchor_))
        {
            return hoveredTitleBoundsCache_;
        }

        const int tooltipWidth =
            hoveredTitleBoundsCache_.right -
            hoveredTitleBoundsCache_.left;
        const int tooltipHeight =
            hoveredTitleBoundsCache_.bottom -
            hoveredTitleBoundsCache_.top;
        hoveredTitleBoundsCacheAnchor_ = visualBounds;
        hoveredTitleBoundsCache_ =
            PositionTitleTooltipBounds(
                visualBounds,
                tooltipWidth,
                tooltipHeight);
        return hoveredTitleBoundsCache_;
    }

    hoveredTitleBoundsCacheText_ = title;
    hoveredTitleBoundsCacheAnchor_ =
        visualBounds;
    hoveredTitleBoundsCachePosition_ =
        position;
    hoveredTitleBoundsCacheLightTheme_ =
        lightTheme;
    hoveredTitleBoundsCache_ =
        CalculateTitleTooltipBounds(
            title, visualBounds);
    return hoveredTitleBoundsCache_;
}

bool DockContainer::IsFocusedElementRect(
    const RECT& baseRect, POINT pointer) const
{
    const RECT focus = ResolveMagnificationFocusRect(pointer);
    if (IsRectEmpty(&focus))
        return PtInRect(&baseRect, pointer) != FALSE;
    if (EqualRect(&baseRect, &focus) == FALSE)
        return false;
    const RECT visualRect = GetElementVisualRect(baseRect, pointer);
    return PtInRect(&visualRect, pointer) != FALSE;
}

bool DockContainer::ContainsInteractivePoint(POINT pt) const
{
    const RECT bounds = GetInteractiveBounds();
    return PtInRect(&bounds, pt) != FALSE;
}

RECT DockContainer::GetInteractiveBounds() const
{
    const RECT bounds = GetBounds();
    if (IsMagnificationSuppressed())
        return bounds;
    return snowdesktop::dock_magnification::ExpandInteractionBounds(
        bounds, app_->dockSettings_.position, ItemIconSize());
}

RECT DockContainer::GetScrollViewport(const RECT& bounds) const
{
    RECT viewport = bounds;
    const int halfGap = ScaledSpacing() / 2;
    const int leadingLength = app_ && app_->dockSettings_.showWindowsButton
        ? ItemPitch() + ScaledSeparatorGap() : 0;
    const size_t count = entries_ ? entries_->size() : 0;
    const bool hasRecycleBin = count > 0 && app_ &&
        app_->IsRecycleBinDockEntry(entries_->back());
    const size_t runningCount = app_
        ? app_->dockUnpinnedRunningApps_.size() : 0;
    const size_t frequentCount = app_
        ? app_->GetFrequentDockItemIndices().size() : 0;
    const bool hasMainItems = SortableEntryCount() > 0 ||
        runningCount > 0 || frequentCount > 0;
    const bool hasFolders = FolderEntryCount() > 0;
    const int trailingLength = IsEdgeAttached()
        ? static_cast<int>(
            snowdesktop::dock_folder_rules::
                EdgeAttachedTrailingReserve(
                    FolderEntryCount(),
                    1 + static_cast<size_t>(hasRecycleBin),
                    hasMainItems,
                    ItemPitch(), ScaledSeparatorGap()))
        : static_cast<int>(1 + hasRecycleBin) * ItemPitch() +
            (hasMainItems && !hasFolders ? ScaledSeparatorGap() : 0);
    if (IsVertical())
    {
        viewport.top += halfGap + leadingLength;
        viewport.bottom -= halfGap + trailingLength;
        viewport.bottom = std::max(viewport.top, viewport.bottom);
    }
    else
    {
        viewport.left += halfGap + leadingLength;
        viewport.right -= halfGap + trailingLength;
        viewport.right = std::max(viewport.left, viewport.right);
    }
    return viewport;
}

int DockContainer::GetMaxScrollOffset(const RECT& bounds) const
{
    const size_t fixedCount = SortableEntryCount();
    const size_t folderCount = FolderEntryCount();
    const size_t runningCount = app_
        ? app_->dockUnpinnedRunningApps_.size() : 0;
    const size_t frequentCount = app_
        ? app_->GetFrequentDockItemIndices().size() : 0;
    const long long contentExtent =
        snowdesktop::dock_folder_rules::
            ScrollableExtentForLayout(
                IsEdgeAttached(),
                fixedCount, runningCount,
                frequentCount, folderCount,
                ItemPitch(),
                ScaledSeparatorGap());
    const RECT viewport = GetScrollViewport(bounds);
    const int viewportExtent = std::max(0, IsVertical()
        ? static_cast<int>(viewport.bottom - viewport.top)
        : static_cast<int>(viewport.right - viewport.left));
    return static_cast<int>(std::clamp<long long>(
        contentExtent - viewportExtent, 0, std::numeric_limits<int>::max()));
}

bool DockContainer::IsPointInScrollViewport(POINT point) const
{
    RECT viewport = GetScrollViewport(GetBounds());
    if (!IsMagnificationSuppressed())
    {
        viewport = snowdesktop::dock_magnification::
            ExpandPerpendicularBounds(viewport,
                app_->dockSettings_.position, ItemIconSize());
    }
    return PtInRect(&viewport, point) != FALSE;
}

bool DockContainer::ScrollByWheelDelta(POINT pointer, int wheelDelta)
{
    const RECT bounds = GetBounds();
    if (!IsPointInScrollViewport(pointer))
        return false;

    const int maxOffset = GetMaxScrollOffset(bounds);
    if (maxOffset <= 0)
    {
        if (scrollOffset_ != 0)
        {
            scrollOffset_ = 0;
            InvalidateSlots();
        }
        return false;
    }

    const int previousOffset = scrollOffset_;
    scrollOffset_ = std::clamp(scrollOffset_ - wheelDelta / 2, 0, maxOffset);
    if (scrollOffset_ != previousOffset)
        InvalidateSlots();
    return true;
}

std::vector<std::unique_ptr<Slot>> DockContainer::BuildSlots()
{
    std::vector<std::unique_ptr<Slot>> slots;
    entryItems_.clear();
    runningItems_.clear();
    frequentItems_.clear();
    RECT bounds = GetBounds();
    scrollOffset_ = std::clamp(scrollOffset_, 0, GetMaxScrollOffset(bounds));
    const size_t count = entries_ ? entries_->size() : 0;
    const size_t fixedCount = SortableEntryCount();
    const size_t folderCount = FolderEntryCount();
    const size_t folderBegin = FolderEntryBegin();
    const bool hasRecycleBin = count > 0 && app_ &&
        app_->IsRecycleBinDockEntry(entries_->back());
    const size_t runningCount = app_
        ? app_->dockUnpinnedRunningApps_.size() : 0;
    std::vector<size_t> frequentIndices = app_
        ? app_->GetFrequentDockItemIndices() : std::vector<size_t>{};
    const size_t frequentCount = frequentIndices.size();
    const int slotLength = ItemPitch();
    const int halfGap = ScaledSpacing() / 2;
    const int separatorGap = ScaledSeparatorGap();
    const bool showWindowsButton = app_ && app_->dockSettings_.showWindowsButton;
    const size_t leadingControlSlots = showWindowsButton ? 1 : 0;
    const int leadingControlOffset = showWindowsButton ? separatorGap : 0;
    int groupOffset = 0;
    if (runningCount > 0 && fixedCount > 0)
        groupOffset += separatorGap;
    const int runningOffset = groupOffset;
    if (frequentCount > 0 && (fixedCount > 0 || runningCount > 0))
        groupOffset += separatorGap;
    const int frequentOffset = groupOffset;
    const bool hasPreFolderItems =
        fixedCount > 0 || runningCount > 0 ||
        frequentCount > 0;
    const int folderOffset = groupOffset +
        (folderCount > 0 && hasPreFolderItems
            ? separatorGap : 0);

    auto makeCell = [&](size_t visualIndex, int axisOffset) {
        RECT cell{};
        if (IsVertical())
        {
            LONG top = bounds.top + halfGap +
                static_cast<LONG>(visualIndex * slotLength) + axisOffset -
                scrollOffset_;
            cell = RECT{ bounds.left,
                top,
                bounds.right,
                top + slotLength };
        }
        else
        {
            LONG left = bounds.left + halfGap +
                static_cast<LONG>(visualIndex * slotLength) + axisOffset -
                scrollOffset_;
            cell = RECT{ left,
                bounds.top,
                left + slotLength,
                bounds.bottom };
        }
        return cell;
    };
    const size_t trailingControlSlots = 1 + static_cast<size_t>(hasRecycleBin);
    auto makeTrailingCell = [&](size_t trailingIndex) {
        RECT cell{};
        if (IsVertical())
        {
            const LONG top = bounds.bottom - halfGap -
                static_cast<LONG>((trailingControlSlots - trailingIndex) * slotLength);
            cell = RECT{ bounds.left, top, bounds.right, top + slotLength };
        }
        else
        {
            const LONG left = bounds.right - halfGap -
                static_cast<LONG>((trailingControlSlots - trailingIndex) * slotLength);
            cell = RECT{ left, bounds.top, left + slotLength, bounds.bottom };
        }
        return cell;
    };
    auto makeFolderCell = [&](size_t folderIndex) {
        if (IsEdgeAttached())
        {
            const RECT searchCell = makeTrailingCell(0);
            RECT cell = searchCell;
            if (IsVertical())
            {
                cell.top = static_cast<LONG>(
                    snowdesktop::dock_folder_rules::
                        FolderAxisStartBeforeSearch(
                            searchCell.top, folderCount,
                            folderIndex, slotLength));
                cell.bottom = cell.top + slotLength;
            }
            else
            {
                cell.left = static_cast<LONG>(
                    snowdesktop::dock_folder_rules::
                        FolderAxisStartBeforeSearch(
                            searchCell.left, folderCount,
                            folderIndex, slotLength));
                cell.right = cell.left + slotLength;
            }
            return cell;
        }
        return makeCell(
            fixedCount + runningCount +
                frequentCount + folderIndex +
                leadingControlSlots,
            folderOffset + leadingControlOffset);
    };

    slots.reserve(count + runningCount + frequentCount + 1);
    for (size_t i = 0; i < count; ++i)
    {
        RECT cell{};
        if (hasRecycleBin && i + 1 == count)
            cell = makeTrailingCell(1);
        else if (i >= folderBegin && i < folderBegin + folderCount)
            cell = makeFolderCell(i - folderBegin);
        else
            cell = makeCell(i + leadingControlSlots, leadingControlOffset);
        auto slot = std::make_unique<Slot>(this, cell, i);
        auto item = std::make_unique<DockEntryItem>(app_, this, i);
        item->SetBounds(cell);
        slot->SetItem(item.get());
        entryItems_.push_back(std::move(item));
        slots.push_back(std::move(slot));
    }

    for (size_t i = 0; i < runningCount; ++i)
    {
        const size_t slotIndex = count + i;
        RECT cell = makeCell(fixedCount + i + leadingControlSlots,
            runningOffset + leadingControlOffset);
        auto slot = std::make_unique<Slot>(this, cell, slotIndex);
        auto item = std::make_unique<DockRunningItem>(app_, this, i);
        item->SetBounds(cell);
        slot->SetItem(item.get());
        runningItems_.push_back(std::move(item));
        slots.push_back(std::move(slot));
    }

    for (size_t i = 0; i < frequentCount; ++i)
    {
        const size_t slotIndex = count + runningCount + i;
        RECT cell = makeCell(
            fixedCount + runningCount + i + leadingControlSlots,
            frequentOffset + leadingControlOffset);
        auto slot = std::make_unique<Slot>(this, cell, slotIndex);
        auto item = std::make_unique<DockFrequentItem>(
            app_, this, frequentIndices[i]);
        item->SetBounds(cell);
        slot->SetItem(item.get());
        frequentItems_.push_back(std::move(item));
        slots.push_back(std::move(slot));
    }

    const size_t searchIndex = count + runningCount + frequentCount;
    RECT searchCell = makeTrailingCell(0);
    slots.push_back(std::make_unique<Slot>(this, searchCell, searchIndex));
    return slots;
}

size_t DockContainer::InsertIndexFor(Slot* slot, HitRegion region) const
{
    const bool folderSource = HasOnlyFolderDragSource();
    const auto range =
        snowdesktop::dock_folder_rules::
            GroupInsertRange(
                folderSource,
                SortableEntryCount(),
                FolderEntryCount());
    const size_t begin = range.begin;
    const size_t end = range.end;
    if (!slot) return end;
    size_t index = std::clamp(slot->GetIndex(), begin, end);
    if (region == HitRegion::SortAfter) ++index;
    return std::clamp(index, begin, end);
}

size_t DockContainer::GetInsertIndexAtPoint(POINT pt) const
{
    const bool folderSource = HasOnlyFolderDragSource();
    const auto range =
        snowdesktop::dock_folder_rules::
            GroupInsertRange(
                folderSource,
                SortableEntryCount(),
                FolderEntryCount());
    const size_t begin = range.begin;
    const size_t end = range.end;
    if (!IsPointInScrollViewport(pt) &&
        !(folderSource && IsEdgeAttached()))
        return end;
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    for (size_t i = begin; i < end && i < slots.size(); ++i)
    {
        RECT bounds = slots[i]->GetBounds();
        if (PtInRect(&bounds, pt))
            return IsVertical()
                ? (pt.y < (bounds.top + bounds.bottom) / 2 ? i : i + 1)
                : (pt.x < (bounds.left + bounds.right) / 2 ? i : i + 1);
    }
    return end;
}

void DockContainer::DrawInsertionPreview(
    ID2D1DeviceContext* context, size_t insertIndex) const
{
    if (!context) return;
    const bool folderSource = HasOnlyFolderDragSource();
    const auto range =
        snowdesktop::dock_folder_rules::
            GroupInsertRange(
                folderSource,
                SortableEntryCount(),
                FolderEntryCount());
    const size_t begin = range.begin;
    const size_t end = range.end;
    insertIndex = std::clamp(insertIndex, begin, end);
    RECT bounds = GetBounds();
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.95f), &brush);
    if (!brush) return;
    const RECT viewport = GetScrollViewport(bounds);
    if (folderSource)
    {
        const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
        RECT boundary = GetSearchRect();
        if (insertIndex < end && insertIndex < slots.size() && slots[insertIndex])
            boundary = slots[insertIndex]->GetBounds();
        else if (end > begin && end - 1 < slots.size() && slots[end - 1])
        {
            boundary = slots[end - 1]->GetBounds();
            if (IsVertical())
                boundary.top = boundary.bottom;
            else
                boundary.left = boundary.right;
        }
        const float axis = static_cast<float>(
            IsVertical() ? boundary.top : boundary.left);
        if (!IsEdgeAttached() &&
            ((IsVertical() &&
                (axis < viewport.top || axis > viewport.bottom)) ||
             (!IsVertical() &&
                (axis < viewport.left || axis > viewport.right))))
            return;
        if (IsVertical())
            context->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(static_cast<float>(bounds.left + 10), axis - 2.0f,
                    static_cast<float>(bounds.right - 10), axis + 2.0f),
                2.0f, 2.0f), brush.Get());
        else
            context->FillRoundedRectangle(D2D1::RoundedRect(
                D2D1::RectF(axis - 2.0f, static_cast<float>(bounds.top + 10),
                    axis + 2.0f, static_cast<float>(bounds.bottom - 10)),
                2.0f, 2.0f), brush.Get());
        return;
    }
    const int leadingOffset = app_ && app_->dockSettings_.showWindowsButton
        ? ItemPitch() + ScaledSeparatorGap() : 0;
    if (IsVertical())
    {
        const float y = static_cast<float>(bounds.top + ScaledSpacing() / 2 +
            leadingOffset + static_cast<LONG>(insertIndex * ItemPitch()) -
            scrollOffset_);
        if (y < viewport.top || y > viewport.bottom) return;
        context->FillRoundedRectangle(D2D1::RoundedRect(
            D2D1::RectF(static_cast<float>(bounds.left + 10), y - 2.0f,
                static_cast<float>(bounds.right - 10), y + 2.0f), 2.0f, 2.0f),
            brush.Get());
        return;
    }
    const float x = static_cast<float>(bounds.left + ScaledSpacing() / 2 +
        leadingOffset + static_cast<LONG>(insertIndex * ItemPitch()) -
        scrollOffset_);
    if (x < viewport.left || x > viewport.right) return;
    context->FillRoundedRectangle(D2D1::RoundedRect(
        D2D1::RectF(x - 2.0f, static_cast<float>(bounds.top + 10),
            x + 2.0f, static_cast<float>(bounds.bottom - 10)), 2.0f, 2.0f),
        brush.Get());
}

void DockContainer::OnItemsDropped(const std::vector<Item*>& sourceItems, Container* origin,
    Slot* targetSlot, HitRegion region, int mods)
{
    if (!app_ || region == HitRegion::Blocked) return;
    app_->CommitDockDrop(sourceItems, origin, this,
        InsertIndexFor(targetSlot, region), mods);
}

void DockContainer::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    if (!context) return;
    RECT bounds = GetVisualPanelBounds(mousePt);
    PersonalizationSettings p = PersonalizationSettings::DarkPreset();
    if (app_ && app_->renderingFloatingDock_)
        p = app_->floatingDockPersonalization_;
    else if (app_)
        p = app_->CurrentPersonalization();
    const float panelRadius = IsEdgeAttached() ? 0.0f : p.cornerRadius;
    const D2D1_COLOR_F fill = D2D1::ColorF(
        p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
    const D2D1_COLOR_F border = D2D1::ColorF(
        p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetBorderAlpha);
    if (app_)
        app_->DrawWidgetPanelBackground(context, bounds, panelRadius, fill,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), false, 1.0f, &p, true,
            reinterpret_cast<std::uintptr_t>(this));

    if (border.a > 0.0f)
    {
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(border, &borderBrush)) && borderBrush)
        {
            if (IsEdgeAttached())
            {
                D2D1_POINT_2F start{};
                D2D1_POINT_2F end{};
                switch (app_->dockSettings_.position)
                {
                case DockPosition::Top:
                    start = D2D1::Point2F(static_cast<float>(bounds.left),
                        static_cast<float>(bounds.bottom) - 0.5f);
                    end = D2D1::Point2F(static_cast<float>(bounds.right), start.y);
                    break;
                case DockPosition::Left:
                    start = D2D1::Point2F(static_cast<float>(bounds.right) - 0.5f,
                        static_cast<float>(bounds.top));
                    end = D2D1::Point2F(start.x, static_cast<float>(bounds.bottom));
                    break;
                case DockPosition::Right:
                    start = D2D1::Point2F(static_cast<float>(bounds.left) + 0.5f,
                        static_cast<float>(bounds.top));
                    end = D2D1::Point2F(start.x, static_cast<float>(bounds.bottom));
                    break;
                case DockPosition::Bottom:
                default:
                    start = D2D1::Point2F(static_cast<float>(bounds.left),
                        static_cast<float>(bounds.top) + 0.5f);
                    end = D2D1::Point2F(static_cast<float>(bounds.right), start.y);
                    break;
                }
                context->DrawLine(start, end, borderBrush.Get(), 1.0f);
            }
            else
            {
                D2D1_ROUNDED_RECT rr = D2D1::RoundedRect(
                    D2D1::RectF(static_cast<float>(bounds.left), static_cast<float>(bounds.top),
                        static_cast<float>(bounds.right), static_cast<float>(bounds.bottom)),
                    panelRadius, panelRadius);
                if (!p.glassEnabled ||
                    !app_->DrawGlassBorder(context, bounds, panelRadius, border, 1.0f))
                    context->DrawRoundedRectangle(rr, borderBrush.Get(), 1.0f);
            }
        }
    }

}

void DockContainer::DrawContents(ID2D1DeviceContext* context)
{
    if (!context) return;
    const auto& slots = GetSlots();
    const size_t count = entries_ ? entries_->size() : 0;
    const size_t fixedCount = SortableEntryCount();
    const size_t folderBegin = FolderEntryBegin();
    const size_t folderCount = FolderEntryCount();
    const size_t folderEnd = folderBegin + folderCount;
    const bool hasRecycleBin = count > 0 && app_ &&
        app_->IsRecycleBinDockEntry(entries_->back());
    const bool lt = app_->IsLightContentTheme();
    std::wstring hoveredTitle;
    const RECT magnificationFocus =
        ResolveMagnificationFocusRect(app_->lastMousePoint_);
    auto isFocused = [&](const RECT& rect) {
        return !IsRectEmpty(&magnificationFocus) &&
            EqualRect(&rect, &magnificationFocus) != FALSE;
    };
    auto visualRectFor = [&](const RECT& rect) {
        return snowdesktop::dock_magnification::MagnifyRect(
            rect, app_->dockSettings_.position,
            GetMagnificationScale(
                rect, magnificationFocus,
                app_->lastMousePoint_),
            ItemIconSize(),
            GetMagnificationAxisShift(
                rect, magnificationFocus,
                app_->lastMousePoint_));
    };

    const RECT windowsButton = GetWindowsButtonRect();
    if (!IsRectEmpty(&windowsButton))
    {
        const bool hovered = isFocused(windowsButton);
        const RECT windowsVisual = visualRectFor(windowsButton);
        const int backgroundSize = std::max(1, static_cast<int>(std::round(
            ItemIconSize() *
            GetMagnificationScale(
                windowsButton,
                magnificationFocus,
                app_->lastMousePoint_))));
        RECT background{
            windowsVisual.left +
                (windowsVisual.right - windowsVisual.left - backgroundSize) / 2,
            windowsVisual.top +
                (windowsVisual.bottom - windowsVisual.top - backgroundSize) / 2,
            windowsVisual.left +
                (windowsVisual.right - windowsVisual.left + backgroundSize) / 2,
            windowsVisual.top +
                (windowsVisual.bottom - windowsVisual.top + backgroundSize) / 2
        };
        app_->DrawDockControlBackground(context, background, 0, !lt);

        const float logoSize = std::max(20.0f, backgroundSize * 0.58f);
        const float paneGap = std::max(1.5f, logoSize * 0.09f);
        const float paneSize = (logoSize - paneGap) * 0.5f;
        const float logoLeft = (background.left + background.right - logoSize) * 0.5f;
        const float logoTop = (background.top + background.bottom - logoSize) * 0.5f;
        ComPtr<ID2D1SolidColorBrush> windowsBrush;
        context->CreateSolidColorBrush(
            D2D1::ColorF(0.0f, 0.47f, 0.84f, 0.94f),
            &windowsBrush);
        if (windowsBrush)
        {
            context->FillRectangle(D2D1::RectF(logoLeft, logoTop,
                logoLeft + paneSize, logoTop + paneSize), windowsBrush.Get());
            context->FillRectangle(D2D1::RectF(logoLeft + paneSize + paneGap, logoTop,
                logoLeft + logoSize, logoTop + paneSize), windowsBrush.Get());
            context->FillRectangle(D2D1::RectF(logoLeft, logoTop + paneSize + paneGap,
                logoLeft + paneSize, logoTop + logoSize), windowsBrush.Get());
            context->FillRectangle(D2D1::RectF(
                logoLeft + paneSize + paneGap, logoTop + paneSize + paneGap,
                logoLeft + logoSize, logoTop + logoSize), windowsBrush.Get());
        }
        if (hovered && !IsMagnificationSuppressed())
        {
            hoveredTitle = _LW("app.dock.start_menu");
        }
    }

    const RECT search = GetSearchRect();
    const int maxScrollOffset = GetMaxScrollOffset(GetBounds());
    const bool hasOverflow = maxScrollOffset > 0;
    RECT scrollViewport = GetScrollViewport(GetBounds());
    if (hasOverflow)
    {
        RECT firstScrollable{};
        RECT lastScrollable{};
        auto includeScrollable = [&](const RECT& bounds) {
            if (IsRectEmpty(&bounds))
                return;
            if (IsRectEmpty(&firstScrollable))
                firstScrollable = bounds;
            lastScrollable = bounds;
        };
        for (size_t index = 0;
            index < fixedCount && index < slots.size(); ++index)
        {
            if (slots[index])
                includeScrollable(slots[index]->GetBounds());
        }
        for (const auto& item : runningItems_)
        {
            if (item)
                includeScrollable(item->GetBounds());
        }
        for (const auto& item : frequentItems_)
        {
            if (item)
                includeScrollable(item->GetBounds());
        }
        if (!IsEdgeAttached())
        {
            for (size_t index = folderBegin;
                index < folderEnd && index < slots.size(); ++index)
            {
                if (slots[index])
                    includeScrollable(
                        slots[index]->GetBounds());
            }
        }

        const MagnificationZone focusZone =
            GetMagnificationZone(magnificationFocus);
        const bool scrollWaveControlsViewport =
            !IsEdgeAttached() ||
            focusZone == MagnificationZone::Leading;
        if (scrollWaveControlsViewport &&
            !IsRectEmpty(&firstScrollable) &&
            !IsRectEmpty(&lastScrollable))
        {
            scrollViewport = snowdesktop::dock_magnification::
                MoveOverflowViewportWithScrollableVisuals(
                    scrollViewport, app_->dockSettings_.position,
                    firstScrollable,
                    visualRectFor(firstScrollable),
                    lastScrollable,
                    visualRectFor(lastScrollable));
        }
        else
        {
            const RECT leadingVisual =
                IsRectEmpty(&windowsButton)
                ? RECT{}
                : visualRectFor(windowsButton);
            RECT trailingFixed = search;
            if (IsEdgeAttached() && folderCount > 0 &&
                folderBegin < slots.size() && slots[folderBegin])
            {
                trailingFixed = slots[folderBegin]->GetBounds();
            }
            const RECT trailingVisual =
                visualRectFor(trailingFixed);
            scrollViewport = snowdesktop::dock_magnification::
                FitOverflowViewportToFixedVisuals(
                    scrollViewport,
                    app_->dockSettings_.position,
                    leadingVisual, trailingVisual,
                    ScaledSeparatorGap());
        }
    }
    const RECT scrollVisualViewport = snowdesktop::dock_magnification::
        ExpandPerpendicularBounds(scrollViewport, app_->dockSettings_.position,
            ItemIconSize());
    const D2D1_RECT_F scrollVisualViewportF = D2D1::RectF(
        static_cast<float>(scrollVisualViewport.left),
        static_cast<float>(scrollVisualViewport.top),
        static_cast<float>(scrollVisualViewport.right),
        static_cast<float>(scrollVisualViewport.bottom));
    bool scrollClipPushed = false;
    if (hasOverflow)
    {
        context->PushAxisAlignedClip(
            scrollVisualViewportF,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        scrollClipPushed = true;
    }

    const bool hasLeadingOverflow =
        hasOverflow && scrollOffset_ > 0;
    const bool hasTrailingOverflow =
        hasOverflow && scrollOffset_ < maxScrollOffset;
    ComPtr<ID2D1LinearGradientBrush> overflowMask;
    bool overflowMaskPushed = false;
    if (hasLeadingOverflow || hasTrailingOverflow)
    {
        const int viewportExtent = std::max(0, IsVertical()
            ? static_cast<int>(
                scrollViewport.bottom - scrollViewport.top)
            : static_cast<int>(
                scrollViewport.right - scrollViewport.left));
        const int fadeLength = std::min(
            std::clamp(ItemIconSize() / 3, 12, 28),
            viewportExtent / 2);
        if (fadeLength > 0 && viewportExtent > 0)
        {
            const float fadeFraction =
                static_cast<float>(fadeLength) /
                static_cast<float>(viewportExtent);
            D2D1_GRADIENT_STOP opacityStops[4]{};
            UINT32 stopCount = 0;
            opacityStops[stopCount++] = {
                0.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f,
                    hasLeadingOverflow ? 0.0f : 1.0f)
            };
            if (hasLeadingOverflow)
            {
                opacityStops[stopCount++] = {
                    fadeFraction,
                    D2D1::ColorF(
                        1.0f, 1.0f, 1.0f, 1.0f)
                };
            }
            if (hasTrailingOverflow)
            {
                opacityStops[stopCount++] = {
                    1.0f - fadeFraction,
                    D2D1::ColorF(
                        1.0f, 1.0f, 1.0f, 1.0f)
                };
            }
            opacityStops[stopCount++] = {
                1.0f,
                D2D1::ColorF(1.0f, 1.0f, 1.0f,
                    hasTrailingOverflow ? 0.0f : 1.0f)
            };

            ComPtr<ID2D1GradientStopCollection> stopCollection;
            if (SUCCEEDED(context->CreateGradientStopCollection(
                    opacityStops, stopCount, D2D1_GAMMA_2_2,
                    D2D1_EXTEND_MODE_CLAMP, &stopCollection)) &&
                stopCollection)
            {
                const D2D1_POINT_2F start = D2D1::Point2F(
                    static_cast<float>(scrollViewport.left),
                    static_cast<float>(scrollViewport.top));
                const D2D1_POINT_2F end = IsVertical()
                    ? D2D1::Point2F(
                        static_cast<float>(scrollViewport.left),
                        static_cast<float>(scrollViewport.bottom))
                    : D2D1::Point2F(
                        static_cast<float>(scrollViewport.right),
                        static_cast<float>(scrollViewport.top));
                if (SUCCEEDED(context->CreateLinearGradientBrush(
                        D2D1::LinearGradientBrushProperties(start, end),
                        stopCollection.Get(), &overflowMask)) &&
                    overflowMask)
                {
                    context->PushLayer(D2D1::LayerParameters(
                        scrollVisualViewportF, nullptr,
                        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                        D2D1::Matrix3x2F::Identity(), 1.0f,
                        overflowMask.Get()), nullptr);
                    overflowMaskPushed = true;
                }
            }
        }
    }
    auto isVisibleInViewport = [&](const RECT& rect) {
        RECT intersection{};
        return IntersectRect(&intersection, &rect, &scrollViewport) != FALSE;
    };
    auto drawScrollableItem = [&](Item* item, const RECT& itemBounds,
        bool focusedPass) {
        if (!item || !isVisibleInViewport(itemBounds))
            return;
        const bool hovered = isFocused(itemBounds);
        if (hovered != focusedPass)
            return;
        const RECT visualBounds = visualRectFor(itemBounds);
        item->Draw(context, visualBounds, item->IsSelected() ? 2 : 0);
        if (hovered && !IsMagnificationSuppressed())
        {
            hoveredTitle = item->GetTitle();
        }
    };
    auto drawScrollablePass = [&](bool focusedPass) {
        for (size_t i = 0; i < fixedCount && i < slots.size(); ++i)
            drawScrollableItem(
                slots[i]->GetItem(), slots[i]->GetBounds(), focusedPass);
        for (const auto& item : runningItems_)
            if (item)
                drawScrollableItem(
                    item.get(), item->GetBounds(), focusedPass);
        for (const auto& item : frequentItems_)
            if (item)
                drawScrollableItem(
                    item.get(), item->GetBounds(), focusedPass);
        if (!IsEdgeAttached())
        {
            for (size_t i = folderBegin;
                 i < folderEnd && i < slots.size(); ++i)
            {
                if (slots[i])
                    drawScrollableItem(
                        slots[i]->GetItem(),
                        slots[i]->GetBounds(),
                        focusedPass);
            }
        }
    };
    drawScrollablePass(false);
    drawScrollablePass(true);

    auto drawSeparatorBetween = [&](
        const RECT& precedingBounds,
        const RECT& followingBounds) {
        if (IsRectEmpty(&precedingBounds) ||
            IsRectEmpty(&followingBounds))
            return;
        const RECT precedingVisual =
            visualRectFor(precedingBounds);
        const RECT followingVisual = visualRectFor(followingBounds);
        ComPtr<ID2D1SolidColorBrush> separatorBrush;
        context->CreateSolidColorBrush(
            lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.18f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.28f), &separatorBrush);
        if (!separatorBrush) return;
        if (IsVertical())
        {
            const float y =
                (static_cast<float>(
                    precedingVisual.bottom) +
                    static_cast<float>(
                        followingVisual.top)) *
                0.5f;
            const float centerX =
                (followingBounds.left + followingBounds.right) / 2.0f;
            context->DrawLine(D2D1::Point2F(centerX - 14.0f, y),
                D2D1::Point2F(centerX + 14.0f, y), separatorBrush.Get(), 1.0f);
        }
        else
        {
            const float x =
                (static_cast<float>(
                    precedingVisual.right) +
                    static_cast<float>(
                        followingVisual.left)) *
                0.5f;
            const float centerY =
                (followingBounds.top + followingBounds.bottom) / 2.0f;
            context->DrawLine(D2D1::Point2F(x, centerY - 14.0f),
                D2D1::Point2F(x, centerY + 14.0f), separatorBrush.Get(), 1.0f);
        }
    };

    const bool hasScrollableItems = fixedCount > 0 ||
        !runningItems_.empty() || !frequentItems_.empty();
    if (fixedCount > 0 && !runningItems_.empty())
        drawSeparatorBetween(
            slots[fixedCount - 1]->GetBounds(),
            runningItems_.front()->GetBounds());
    if ((fixedCount > 0 || !runningItems_.empty()) && !frequentItems_.empty())
    {
        const RECT preceding =
            !runningItems_.empty()
                ? runningItems_.back()->GetBounds()
                : slots[fixedCount - 1]->GetBounds();
        drawSeparatorBetween(
            preceding,
            frequentItems_.front()->GetBounds());
    }
    if (!IsEdgeAttached() &&
        hasScrollableItems && folderCount > 0 &&
        folderBegin < slots.size() &&
        slots[folderBegin])
    {
        const RECT preceding =
            !frequentItems_.empty()
                ? frequentItems_.back()->GetBounds()
                : (!runningItems_.empty()
                    ? runningItems_.back()->GetBounds()
                    : slots[fixedCount - 1]->
                        GetBounds());
        drawSeparatorBetween(
            preceding,
            slots[folderBegin]->GetBounds());
    }
    if (overflowMaskPushed)
        context->PopLayer();
    if (scrollClipPushed)
        context->PopAxisAlignedClip();

    if (IsEdgeAttached())
    {
        auto drawFixedFolderPass = [&](bool focusedPass) {
            for (size_t i = folderBegin;
                 i < folderEnd && i < slots.size(); ++i)
            {
                if (!slots[i]) continue;
                Item* item = slots[i]->GetItem();
                if (!item) continue;
                const RECT itemBounds = slots[i]->GetBounds();
                const bool hovered = isFocused(itemBounds);
                if (hovered != focusedPass) continue;
                item->Draw(context, visualRectFor(itemBounds),
                    item->IsSelected() ? 2 : 0);
                if (hovered && !IsMagnificationSuppressed())
                    hoveredTitle = item->GetTitle();
            }
        };
        drawFixedFolderPass(false);
        drawFixedFolderPass(true);
    }

    if (!IsRectEmpty(&windowsButton))
    {
        RECT following{};
        if (fixedCount > 0 && !slots.empty() && slots[0])
            following = slots[0]->GetBounds();
        else if (!runningItems_.empty() && runningItems_.front())
            following = runningItems_.front()->GetBounds();
        else if (!frequentItems_.empty() && frequentItems_.front())
            following = frequentItems_.front()->GetBounds();
        else if (!IsEdgeAttached() && folderCount > 0 &&
            folderBegin < slots.size() &&
            slots[folderBegin])
            following =
                slots[folderBegin]->GetBounds();
        if (!IsRectEmpty(&following))
            drawSeparatorBetween(
                windowsButton, following);
    }
    if (hasScrollableItems &&
        (folderCount == 0 || IsEdgeAttached()))
    {
        RECT following = search;
        if (IsEdgeAttached() && folderCount > 0 &&
            folderBegin < slots.size() && slots[folderBegin])
        {
            following = slots[folderBegin]->GetBounds();
        }
        const RECT followingVisual = visualRectFor(following);
        ComPtr<ID2D1SolidColorBrush> separatorBrush;
        context->CreateSolidColorBrush(
            lt ? D2D1::ColorF(
                0.0f, 0.0f, 0.0f, 0.18f)
               : D2D1::ColorF(
                1.0f, 1.0f, 1.0f, 0.28f),
            &separatorBrush);
        if (separatorBrush)
        {
            const float halfSeparatorGap =
                static_cast<float>(
                    ScaledSeparatorGap()) * 0.5f;
            if (IsVertical())
            {
                const float y =
                    static_cast<float>(
                        followingVisual.top) -
                    halfSeparatorGap;
                const float centerX =
                    (following.left + following.right) /
                    2.0f;
                context->DrawLine(
                    D2D1::Point2F(
                        centerX - 14.0f, y),
                    D2D1::Point2F(
                        centerX + 14.0f, y),
                    separatorBrush.Get(), 1.0f);
            }
            else
            {
                const float x =
                    static_cast<float>(
                        followingVisual.left) -
                    halfSeparatorGap;
                const float centerY =
                    (following.top + following.bottom) /
                    2.0f;
                context->DrawLine(
                    D2D1::Point2F(
                        x, centerY - 14.0f),
                    D2D1::Point2F(
                        x, centerY + 14.0f),
                    separatorBrush.Get(), 1.0f);
            }
        }
    }

    if (hasRecycleBin && count - 1 < slots.size())
    {
        Item* recycleBin = slots[count - 1]->GetItem();
        if (recycleBin)
        {
            const RECT recycleBounds = slots[count - 1]->GetBounds();
            const bool hovered = isFocused(recycleBounds);
            const RECT recycleVisual = visualRectFor(recycleBounds);
            recycleBin->Draw(context, recycleVisual,
                recycleBin->IsSelected() ? 2 : 0);
            if (hovered && !IsMagnificationSuppressed())
            {
                hoveredTitle = recycleBin->GetTitle();
            }
        }
    }

    const bool searchHovered = isFocused(search);
    const RECT searchVisual = visualRectFor(search);
    const int backgroundSize = std::max(1, static_cast<int>(std::round(
        ItemIconSize() *
        GetMagnificationScale(
            search, magnificationFocus,
            app_->lastMousePoint_))));
    const float searchScale = static_cast<float>(backgroundSize) / 52.0f;
    RECT searchBackground{
        searchVisual.left +
            (searchVisual.right - searchVisual.left - backgroundSize) / 2,
        searchVisual.top +
            (searchVisual.bottom - searchVisual.top - backgroundSize) / 2,
        searchVisual.left +
            (searchVisual.right - searchVisual.left + backgroundSize) / 2,
        searchVisual.top +
            (searchVisual.bottom - searchVisual.top + backgroundSize) / 2
    };
    app_->DrawDockControlBackground(
        context, searchBackground, 0, !lt);

    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(
        lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f)
           : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f), &brush);
    if (brush)
    {
        ComPtr<ID2D1Factory> factory;
        ComPtr<ID2D1StrokeStyle> roundedStroke;
        context->GetFactory(&factory);
        if (factory)
        {
            const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
                D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
            factory->CreateStrokeStyle(properties, nullptr, 0, &roundedStroke);
        }
        const float centerX = (searchBackground.left + searchBackground.right) / 2.0f -
            2.7f * searchScale;
        const float centerY = (searchBackground.top + searchBackground.bottom) / 2.0f -
            2.7f * searchScale;
        const float searchStroke = std::max(1.5f, 2.35f * searchScale);
        context->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY),
            9.2f * searchScale, 9.2f * searchScale),
            brush.Get(), searchStroke, roundedStroke.Get());
        context->DrawLine(D2D1::Point2F(centerX + 6.6f * searchScale,
                centerY + 6.6f * searchScale),
            D2D1::Point2F(centerX + 13.5f * searchScale,
                centerY + 13.5f * searchScale),
            brush.Get(), searchStroke, roundedStroke.Get());
    }

    if (searchHovered && !IsMagnificationSuppressed())
    {
        hoveredTitle = _LW("app.dock.quick_search");
    }

    if (!hoveredTitle.empty() && app_->dwriteFactory_)
    {
        ComPtr<IDWriteTextFormat> tooltipFormat;
        app_->dwriteFactory_->CreateTextFormat(L"Segoe UI", nullptr,
            lt ? DWRITE_FONT_WEIGHT_LIGHT : DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 13.0f, L"zh-CN", &tooltipFormat);
        if (tooltipFormat)
        {
            tooltipFormat->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            tooltipFormat->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            const RECT tooltip =
                GetHoveredTitleBounds(
                    app_->lastMousePoint_);
            if (IsRectEmpty(&tooltip))
                return;

            app_->DrawD2DRoundedRectangle(context, tooltip, 7.0f,
                lt ? D2D1::ColorF(0.94f, 0.95f, 0.97f, 0.94f) : D2D1::ColorF(0.06f, 0.07f, 0.09f, 0.94f),
                lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.14f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.20f));
            app_->DrawD2DTextEllipsis(context, hoveredTitle, tooltip,
                tooltipFormat.Get(), lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.96f),
                DWRITE_TEXT_ALIGNMENT_CENTER,
                DWRITE_PARAGRAPH_ALIGNMENT_CENTER, true);
        }
    }
}

std::vector<Item*> DockContainer::GetSelectedItems() const
{
    std::vector<Item*> selected;
    for (const auto& item : entryItems_)
        if (item && item->IsSelected()) selected.push_back(item.get());
    return selected;
}

HitRegion DockContainer::HitTestDrag(POINT pt, Slot*& outSlot)
{
    auto resetDwell = [&]() {
        if (!app_) return;
        app_->ResetDockHandoffDwell();
    };
    outSlot = nullptr;
    RECT dockBounds = GetBounds();
    if (!PtInRect(&dockBounds, pt))
    {
        resetDwell();
        return HitRegion::None;
    }
    if (IsWindowsButtonPoint(pt))
    {
        resetDwell();
        return HitRegion::Blocked;
    }
    const bool pointInScrollViewport = IsPointInScrollViewport(pt);
    const auto& slots = GetSlots();
    for (const auto& slot : slots)
    {
        RECT bounds = slot->GetBounds();
        if (!PtInRect(&bounds, pt)) continue;
        Item* targetItem = slot->GetItem();
        const bool searchSlot = !targetItem && slot.get() == slots.back().get();
        const auto* dockEntry = dynamic_cast<DockEntryItem*>(targetItem);
        const bool recycleBinSlot = dockEntry && app_ &&
            dockEntry->GetEntryIndex() < app_->dockEntries_.size() &&
            app_->IsRecycleBinDockEntry(
                app_->dockEntries_[dockEntry->GetEntryIndex()]);
        const bool folderSlot = dockEntry && app_ &&
            dockEntry->GetEntryIndex() < app_->dockEntries_.size() &&
            app_->IsFolderDockEntry(
                app_->dockEntries_[dockEntry->GetEntryIndex()]);
        const bool fixedTrailingSlot =
            searchSlot || recycleBinSlot ||
            (folderSlot && IsEdgeAttached());
        if (!pointInScrollViewport && !fixedTrailingSlot)
            continue;
        outSlot = slot.get();
        if (!targetItem)
        {
            // The only item-less Dock slot is Search. Dropping here inserts at
            // the end of the fixed entries; its preview is translated to that
            // real insertion boundary by DrawDropPreview.
            resetDwell();
            return HitRegion::SortAfter;
        }
        if (dynamic_cast<DockFrequentItem*>(targetItem) ||
            dynamic_cast<DockRunningItem*>(targetItem))
        {
            // Generated frequent and running items are not members of the
            // sortable Dock list. Block without passing through to the grid.
            resetDwell();
            return HitRegion::Blocked;
        }

        RECT handoffRect = bounds;
        InflateRect(&handoffRect, -16, -10);
        if (recycleBinSlot)
        {
            resetDwell();
            const auto& sourceItems = app_->dragSession_.Items();
            const bool isDragSource = std::find(sourceItems.begin(), sourceItems.end(),
                targetItem) != sourceItems.end();
            return isDragSource ? HitRegion::None : HitRegion::Handoff;
        }
        const auto* folderDockItem =
            dynamic_cast<DockEntryItem*>(
                targetItem);
        const bool folderTarget =
            folderDockItem &&
            folderDockItem->GetEntryIndex() <
                app_->dockEntries_.size() &&
            app_->IsFolderDockEntry(
                app_->dockEntries_[
                    folderDockItem->
                        GetEntryIndex()]);
        const bool dockMetadataReorder =
            snowdesktop::dock_drop_rules::
                ShouldPreferMetadataReorder(
                    dynamic_cast<DockContainer*>(
                        app_->dragSession_.Source()) != nullptr,
                    HasOnlyFolderDragSource(),
                    folderTarget);
        const bool collectionTarget =
            folderDockItem &&
            folderDockItem->GetEntryType() ==
                DockEntryType::Collection;
        const bool canHandoff = !dockMetadataReorder &&
            targetItem && !targetItem->IsSelected() &&
            snowdesktop::dock_drop_rules::
                SupportsHandoffTarget(
                    app_->dragSession_.SourceList().
                        hasWidgets,
                    folderTarget,
                    collectionTarget) &&
            PtInRect(&handoffRect, pt);
        if (canHandoff && app_)
        {
            const size_t index = slot->GetIndex();
            if (folderTarget)
            {
                const size_t entryIndex =
                    folderDockItem->
                        GetEntryIndex();
                const DockEntry& entry =
                    app_->dockEntries_[entryIndex];
                const std::wstring sourceId =
                    std::to_wstring(
                        static_cast<int>(
                            entry.type)) +
                    L":" +
                    ToUpperInvariant(
                        entry.reference);
                if (app_->
                        dockFolderPopupOpen_ &&
                    app_->
                        dockFolderPopupSourceId_ ==
                            sourceId)
                {
                    resetDwell();
                    return HitRegion::Handoff;
                }

                const DWORD now =
                    GetTickCount();
                if (app_->
                        dockHandoffDwellIndex_ !=
                    index)
                {
                    app_->
                        dockHandoffDwellIndex_ =
                            index;
                    app_->
                        dockHandoffDwellStartTick_ =
                            now;
                    app_->
                        dockHandoffDwellReady_ =
                            false;
                    if (app_->hwnd_)
                        SetTimer(
                            app_->hwnd_,
                            kDockHandoffDwellTimerId,
                            kDockHandoffDwellIntervalMs,
                            nullptr);
                }
                return HitRegion::Handoff;
            }
            const DWORD now = GetTickCount();
            if (app_->dockHandoffDwellIndex_ != index)
            {
                app_->dockHandoffDwellIndex_ = index;
                app_->dockHandoffDwellStartTick_ = now;
                app_->dockHandoffDwellReady_ = false;
                if (app_->hwnd_)
                    SetTimer(app_->hwnd_, kDockHandoffDwellTimerId,
                        kDockHandoffDwellIntervalMs, nullptr);
            }
            if (app_->dockHandoffDwellReady_ ||
                now - app_->dockHandoffDwellStartTick_ >= kDockHandoffDwellDelayMs)
            {
                app_->dockHandoffDwellReady_ = true;
                return HitRegion::Handoff;
            }
        }
        else
        {
            resetDwell();
        }
        if (IsVertical())
            return pt.y < (bounds.top + bounds.bottom) / 2
                ? HitRegion::SortBefore : HitRegion::SortAfter;
        return pt.x < (bounds.left + bounds.right) / 2
            ? HitRegion::SortBefore : HitRegion::SortAfter;
    }
    resetDwell();
    return HitRegion::Empty;
}

std::wstring DockContainer::GetDragHint(Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems, Container* origin, int mods) const
{
    if (region == HitRegion::Blocked) return L"";
    if (region == HitRegion::Handoff && slot && slot->GetItem())
        return _LFW("core.drag.release_handle", slot->GetItem()->GetTitle());
    if (origin != this && !HasCapacity(sourceItems.empty() ? 1 : sourceItems.size()))
        return _LW("core.drag.dock_full");
    if (origin == this) return _LW("core.drag.release_adjust_order");
    if (app_ && app_->dragDropController_.
            IsExternalDragActive())
        return _LW("core.dock.release_dock_map_full");
    return (mods & MK_CONTROL)
        ? _LW("core.dock.release_dock_map_full")
        : _LW("core.dock.release_move_dock_ctrl");
}

void DockContainer::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (!slot || !ctx || region == HitRegion::Blocked) return;
    if (region != HitRegion::Handoff &&
        !snowdesktop::dock_drop_rules::
            ShouldDrawSortableInsertionIndicator(
                HasOnlyRecycleBinDragSource()))
        return;
    const auto& slots = GetSlots();
    if (!slots.empty() && slot == slots.back().get())
    {
        DrawInsertionPreview(
            ctx,
            HasOnlyFolderDragSource()
                ? FolderEntryBegin() +
                    FolderEntryCount()
                : SortableEntryCount());
        return;
    }
    if (region == HitRegion::Handoff)
    {
        RECT bounds = slot->GetBounds();
        app_->DrawD2DRoundedRectangle(ctx, bounds, 12.0f,
            D2D1::ColorF(0.28f, 0.80f, 0.48f, 0.18f),
            D2D1::ColorF(0.28f, 0.90f, 0.52f, 0.88f), 2.0f);
        return;
    }
    DrawInsertionPreview(
        ctx, InsertIndexFor(slot, region));
}

RECT DockContainer::GetSearchRect() const
{
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    return slots.empty() ? RECT{} : slots.back()->GetBounds();
}

RECT DockContainer::GetWindowsButtonRect() const
{
    if (!app_ || !app_->dockSettings_.showWindowsButton)
        return RECT{};
    const RECT bounds = GetBounds();
    const int halfGap = ScaledSpacing() / 2;
    const int slotLength = ItemPitch();
    if (IsVertical())
        return RECT{ bounds.left, bounds.top + halfGap,
            bounds.right, bounds.top + halfGap + slotLength };
    return RECT{ bounds.left + halfGap, bounds.top,
        bounds.left + halfGap + slotLength, bounds.bottom };
}

bool DockContainer::IsWindowsButtonPoint(POINT pt) const
{
    const RECT rect = GetWindowsButtonRect();
    return !IsRectEmpty(&rect) && IsFocusedElementRect(rect, pt);
}

bool DockContainer::IsSearchPoint(POINT pt) const
{
    RECT rect = GetSearchRect();
    return IsFocusedElementRect(rect, pt);
}

DockEntryItem* DockContainer::EntryAtPoint(POINT pt) const
{
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const size_t count = entries_ ? entries_->size() : 0;
    const size_t fixedCount = SortableEntryCount();
    if (IsPointInScrollViewport(pt))
    {
        for (size_t i = 0; i < fixedCount && i < slots.size(); ++i)
        {
            RECT bounds = slots[i]->GetBounds();
            if (IsFocusedElementRect(bounds, pt))
                return dynamic_cast<DockEntryItem*>(slots[i]->GetItem());
        }
    }
    if (IsPointInScrollViewport(pt) || IsEdgeAttached())
    {
        const size_t end = FolderEntryBegin() + FolderEntryCount();
        for (size_t i = FolderEntryBegin();
             i < end && i < slots.size(); ++i)
        {
            RECT bounds = slots[i]->GetBounds();
            if (IsFocusedElementRect(bounds, pt))
                return dynamic_cast<DockEntryItem*>(slots[i]->GetItem());
        }
    }
    if (count > 0 && app_ &&
        app_->IsRecycleBinDockEntry(entries_->back()) &&
        count - 1 < slots.size())
    {
        RECT bounds = slots[count - 1]->GetBounds();
        if (IsFocusedElementRect(bounds, pt))
            return dynamic_cast<DockEntryItem*>(
                slots[count - 1]->GetItem());
    }
    return nullptr;
}

DockRunningItem* DockContainer::RunningItemAtPoint(POINT pt) const
{
    if (!IsPointInScrollViewport(pt)) return nullptr;
    const_cast<DockContainer*>(this)->GetSlots();
    for (const auto& item : runningItems_)
    {
        if (!item) continue;
        RECT bounds = item->GetBounds();
        if (IsFocusedElementRect(bounds, pt)) return item.get();
    }
    return nullptr;
}

DockFrequentItem* DockContainer::FrequentItemAtPoint(POINT pt) const
{
    if (!IsPointInScrollViewport(pt)) return nullptr;
    const_cast<DockContainer*>(this)->GetSlots();
    for (const auto& item : frequentItems_)
    {
        if (!item) continue;
        RECT bounds = item->GetBounds();
        if (IsFocusedElementRect(bounds, pt)) return item.get();
    }
    return nullptr;
}
