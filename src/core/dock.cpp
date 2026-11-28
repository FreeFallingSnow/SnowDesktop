#include "ui/render/app_font.h"
#include "dock.h"

#include "app/app.h"
#include "app/navigation/quick_navigation_state.h"
#include "app/dock/persistent_dock_host.h"
#include "app/dock/dock_tray_popup_state.h"
#include "app/dock/dock_running_drag_state.h"
#include "common/constants.h"
#include "common/utils.h"
#include "dock/dock_folder_rules.h"
#include "dock/dock_drop_model.h"
#include "dock/dock_launch_animation.h"
#include "dock/dock_magnification.h"
#include "dock/dock_settings.h"
#include "dock/dock_window_preview.h"
#include "dock/dock_window_thumbnail.h"
#include "dock/merged_dock_presentation.h"
#include "drag_drop/drag_hint_rules.h"
#include "system/status_bar/status_bar_appearance.h"
#include "settings/animation_settings.h"
#include "slot.h"
#include "common/l10n.h"
#include "layout/item_location.h"
#include "ui/render/icon_backdrop_clip.h"
#include "ui/render/scroll_content_clip.h"
#include "ui/render/smooth_corner_geometry.h"
#include "ui/render/ui_animation_scheduler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <shlwapi.h>
#include <span>
#include <utility>

namespace
{
float ThumbnailSlotUnits(float iconFraction)
{
    return 1.0f + iconFraction *
        (snowdesktop::dock_window_thumbnail::kHorizontalCardAspectRatio - 1.0f);
}

float RunningItemSlotUnits(const DockRunningAppInfo& running, float iconFraction)
{
    const float units = running.minimizedThumbnail ? ThumbnailSlotUnits(iconFraction) : 1.0f;
    return units * running.presence.Amount();
}

RECT DockAxisCell(const RECT& bounds, bool vertical, int pitch, int spacing,
    int separatorGap, int scrollOffset, const snowdesktop::dock_entry_removal::AxisGroup& offset,
    float begin, float amount)
{
    using snowdesktop::dock_running_animation::AxisLength;
    const int start = spacing / 2 + AxisLength(offset.begin + begin, pitch) +
        AxisLength(offset.gaps, separatorGap) - scrollOffset;
    const int length = AxisLength(begin + amount, pitch) - AxisLength(begin, pitch);
    if (vertical)
        return {bounds.left, bounds.top + start, bounds.right, bounds.top + start + length};
    return {bounds.left + start, bounds.top, bounds.left + start + length, bounds.bottom};
}

float RunningDividerAmount(const std::vector<DockRunningAppInfo>& runningApps)
{
    float icons = 0.0f, thumbnails = 0.0f;
    for (const auto& app : runningApps)
        (app.minimizedThumbnail ? thumbnails : icons) += app.presence.Amount();
    return std::min({1.0f, icons, thumbnails});
}

size_t SortableFileEnd(const std::vector<DockEntry>* entries, bool freeOrder)
{
    namespace model = snowdesktop::dock_drop_model;
    return entries ? model::SortableEnd(*entries, freeOrder, model::IsRecycleBinEntry) : 0;
}

bool IsDockFrequentDrag(const std::vector<Item*>& items)
{
    return !items.empty() && dynamic_cast<const DockFrequentItem*>(items.front()) != nullptr;
}

const DragSourceEntry* DraggedSeparator(const DragSourceList& sources)
{
    if (sources.entries.size() != 1) return nullptr;
    const auto& source = sources.entries.front();
    return source.fromDock && source.dockEntryType == DockEntryType::Separator ? &source : nullptr;
}

struct DockSeparatorMove
{
    size_t source;
    size_t boundary;
    LONG width;

    void Shift(RECT& rect, size_t index, bool vertical) const
    {
        LONG offset = 0;
        if (index > source && index < boundary) offset = -width;
        if (index >= boundary && index < source) offset = width;
        if (offset) OffsetRect(&rect, vertical ? 0 : offset, vertical ? offset : 0);
    }
};

std::optional<DockSeparatorMove> PreviewSeparatorMove(const DragSourceEntry* drag,
    const std::vector<DockEntry>& entries, snowdesktop::dock_folder_rules::InsertRange range,
    size_t boundary, LONG width)
{
    if (!drag) return std::nullopt;
    for (size_t i = range.begin; i < range.end && i < entries.size(); ++i)
        if (entries[i].type == DockEntryType::Separator && entries[i].reference == drag->dockReference)
            return DockSeparatorMove{i, boundary, width};
    return std::nullopt;
}

void DrawDockSeparator(ID2D1DeviceContext* context, float x, float y, bool vertical, bool light, float opacity)
{
    ComPtr<ID2D1SolidColorBrush> brush;
    context->CreateSolidColorBrush(light ? D2D1::ColorF(0, 0, 0, 0.18f * opacity) :
        D2D1::ColorF(1, 1, 1, 0.28f * opacity), &brush);
    if (!brush) return;
    const float dx = vertical ? 14.0f : 0.0f;
    const float dy = vertical ? 0.0f : 14.0f;
    context->DrawLine(D2D1::Point2F(x - dx, y - dy), D2D1::Point2F(x + dx, y + dy), brush.Get(), 1.0f);
}

int ClampPointerAxisToCenters(
    int pointerAxis, std::span<const RECT> rects, bool vertical)
{
    if (rects.empty()) return pointerAxis;
    int first = std::numeric_limits<int>::max();
    int last = std::numeric_limits<int>::min();
    for (const RECT& rect : rects)
    {
        const int center = vertical
            ? (rect.top + rect.bottom) / 2 : (rect.left + rect.right) / 2;
        first = std::min(first, center);
        last = std::max(last, center);
    }
    return std::clamp(pointerAxis, first, last);
}

int FitScrollMagnificationShift(int leadingGrowth, int trailingGrowth,
    int offset, int maximum)
{
    // Use the hidden distance to absorb growth while there is room to scroll.
    // At an endpoint, grow inward so the end item remains inside the clip.
    const int leading = std::max(0, leadingGrowth - offset);
    const int trailing = std::max(0, trailingGrowth - (maximum - offset));
    if (leading == 0 || trailing == 0) return leading - trailing;
    // A lane with only a few pixels of overflow can need room at both ends.
    const float progress = static_cast<float>(offset) / static_cast<float>(maximum);
    return static_cast<int>(std::lround(std::lerp(
        static_cast<float>(leading), -static_cast<float>(trailing), progress)));
}

float DockPreviewScale(const RECT& base, const RECT& visual, int iconSize)
{
    return static_cast<float>(snowdesktop::dock_magnification::
        IconSizeForVisualRect(base, visual, iconSize)) / static_cast<float>(std::max(1, iconSize));
}

void DrawDockInsertionIndicator(ID2D1DeviceContext* context,
    const RECT& base, const RECT& visual, float axis, bool vertical, int iconSize)
{
    const float scale = DockPreviewScale(base, visual, iconSize);
    const float center = vertical
        ? static_cast<float>(visual.left + visual.right) / 2.0f : static_cast<float>(visual.top + visual.bottom) / 2.0f;
    const float length = static_cast<float>(std::max(0L,
        (vertical ? base.right - base.left : base.bottom - base.top) - 20)) * scale;
    const float radius = 2.0f * scale;
    const D2D1_RECT_F line = vertical
        ? D2D1::RectF(center - length / 2, axis - radius, center + length / 2, axis + radius)
        : D2D1::RectF(axis - radius, center - length / 2, axis + radius, center + length / 2);
    ComPtr<ID2D1SolidColorBrush> brush;
    if (SUCCEEDED(context->CreateSolidColorBrush(D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.95f), &brush)))
        context->FillRoundedRectangle(D2D1::RoundedRect(line, radius, radius), brush.Get());
}
}

DockRunningItem::DockRunningItem(
    DesktopApp* app, Container* container, size_t runningIndex)
    : app_(app), container_(container), runningIndex_(runningIndex)
{
    if (app_ && runningIndex < app_->dockUnpinnedRunningApps_.size())
        identityKey_ = app_->dockUnpinnedRunningApps_[runningIndex].identityKey;
}

size_t DockRunningItem::GetRunningIndex() const
{
    if (!app_) return static_cast<size_t>(-1);
    const auto& apps = app_->dockUnpinnedRunningApps_;
    if (runningIndex_ < apps.size() && apps[runningIndex_].identityKey == identityKey_)
        return runningIndex_;
    const auto found = std::find_if(apps.begin(), apps.end(), [&](const auto& app) {
        return app.identityKey == identityKey_;
    });
    return found == apps.end() ? static_cast<size_t>(-1) :
        static_cast<size_t>(found - apps.begin());
}

std::wstring DockRunningItem::GetTitle() const
{
    const size_t index = GetRunningIndex();
    if (!app_ || index >= app_->dockUnpinnedRunningApps_.size())
        return L"";
    const DockRunningAppInfo& running =
        app_->dockUnpinnedRunningApps_[index];
    if (!running.presence.Interactive()) return L"";
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
    const size_t index = GetRunningIndex();
    return app_ && index < app_->dockUnpinnedRunningApps_.size() &&
        app_->dockUnpinnedRunningApps_[index].presence.Interactive()
        ? app_->dockUnpinnedRunningApps_[index].executablePath : L"";
}

HBITMAP DockRunningItem::GetIconBitmap() const
{
    const size_t index = GetRunningIndex();
    return app_ && index < app_->dockUnpinnedRunningApps_.size()
        ? app_->dockUnpinnedRunningApps_[index].iconBitmap : nullptr;
}

RECT DockRunningItem::GetBounds() const { return bounds_; }
void DockRunningItem::SetBounds(RECT bounds) { bounds_ = bounds; }

bool DockRunningItem::IsSelected() const
{
    return false;
}

void DockRunningItem::SetSelected(bool selected)
{
    const size_t index = GetRunningIndex();
    if (app_ && index < app_->dockUnpinnedRunningApps_.size())
        app_->dockUnpinnedRunningApps_[index].selected = false;
    (void)selected;
}

Container* DockRunningItem::GetContainer() const { return container_; }

void DockRunningItem::Draw(ID2D1DeviceContext* context, RECT rect, int state)
{
    const size_t index = GetRunningIndex();
    if (app_ && index < app_->dockUnpinnedRunningApps_.size())
    {
        const auto* dock = dynamic_cast<const DockContainer*>(container_);
        const auto& running = app_->dockUnpinnedRunningApps_[index];
        const auto titles = dock ? dock->RunningTitleGroup(index) : DockContainer::WindowTitleGroup{};
        const RECT icon = titles.Visible() ? dock->DrawWindowTitleGroup(context, bounds_, rect, titles) : rect;
        const int iconSize = dock ? dock->GetElementIconSize(bounds_, rect) : 0;
        app_->DrawDockRunningApp(context, running, icon, state,
            iconSize, dock ? dock->GetMonitor() : nullptr);
    }
}

std::wstring DockRunningItem::GetIdentityKey() const
{
    return identityKey_;
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
    const auto* dock = dynamic_cast<const DockContainer*>(container_);
    app_->DrawDockEntry(context, entry, rect, state,
        dock ? dock->GetElementIconSize(bounds_, rect) : 0, nullptr, nullptr, dock ? dock->GetMonitor() : nullptr);
}

ComPtr<IDataObject> DockFrequentItem::CreateDataObject()
{
    if (!app_ || itemIndex_ >= app_->items_.size()) return nullptr;
    DesktopIcon icon(&app_->items_[itemIndex_], container_, app_);
    return icon.CreateDataObject();
}

DockEntryItem::DockEntryItem(DesktopApp* app, Container* container, size_t entryIndex)
    : app_(app), container_(container), entryIndex_(entryIndex) {}

bool DockEntryItem::CanStartDrag() const
{
    namespace model = snowdesktop::dock_drop_model;
    const auto* entry = Entry();
    return entry && !model::IsTrailingControl(*entry, app_->dockSettings_.freeItemOrder, model::IsRecycleBinEntry);
}

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
    if (entry->type == DockEntryType::Separator) return L"";
    if (entry->type == DockEntryType::QuickSearch) return _LW("app.dock.quick_search");
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
    if (!context) return;
    const DockEntry* entry = Entry();
    if (entry && app_)
    {
        const auto* dock = dynamic_cast<const DockContainer*>(container_);
        if (entry->type == DockEntryType::QuickSearch) return;
        if (entry->type == DockEntryType::Separator)
        {
            if (!dock) return;
            const auto& session = app_->dragSession_;
            const auto* dragged = session.IsActive() ? DraggedSeparator(session.SourceList()) : nullptr;
            if (dragged && dragged->dockReference == entry->reference &&
                dynamic_cast<DockContainer*>(session.TargetContainer())) return;
            const bool vertical = dock->GetInsertionStyle() == BarStyle::HBar;
            const bool light = app_->CurrentDockAppearance(dock->GetMonitor()).contentTheme == 1;
            // The stroke stays centered in the Dock and uses the same artwork as an automatic divider.
            const RECT xBounds = vertical ? bounds_ : rect;
            const RECT yBounds = vertical ? rect : bounds_;
            DrawDockSeparator(context, (xBounds.left + xBounds.right) * 0.5f, (yBounds.top + yBounds.bottom) * 0.5f, vertical, light, 1.0f);
            return;
        }
        if (dock && entry->type == DockEntryType::DesktopItem &&
            app_->FindItemIndexByKey(entry->reference) >= app_->items_.size())
        {
            const auto presentation = dock->CapturePresentationState();
            const auto key = std::to_wstring(static_cast<int>(entry->type)) + L":" + ToUpperInvariant(entry->reference);
            const auto& pending = presentation.removals->Entries();
            const auto retained = std::find_if(pending.begin(), pending.end(), [&](const auto& removal) {
                return !removal.removed && removal.key == key;
            });
            if (retained != pending.end() && retained->snapshot)
            {
                // A complete Shell read can precede independent deletion
                // confirmation. Keep its last artwork until pruning commits.
                const int size = dock->GetElementIconSize(bounds_, rect);
                const float left = (rect.left + rect.right - size) * 0.5f;
                const float top = (rect.top + rect.bottom - size) * 0.5f;
                context->DrawBitmap(retained->snapshot.Get(), D2D1::RectF(left, top, left + size, top + size),
                    1.0f, D2D1_INTERPOLATION_MODE_HIGH_QUALITY_CUBIC);
                return;
            }
        }
        const auto titles = dock ? dock->EntryTitleGroup(entryIndex_) : DockContainer::WindowTitleGroup{};
        const RECT icon = titles.Visible() ? dock->DrawWindowTitleGroup(context, bounds_, rect, titles) : rect;
        const int iconSize = dock ? dock->GetElementIconSize(bounds_, rect) : 0;
        app_->DrawDockEntry(context, *entry, icon, state,
            iconSize,
            nullptr, nullptr, dock ? dock->GetMonitor() : nullptr);
    }
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

struct DockContainer::PresentationMotion
{
    struct HoverMotion
    {
        RECT focus{};
        double axis = 0.0, requested = 0.0, sampledAt = 0.0;
        bool tracking = false;

        bool Moving() const { return std::abs(axis - requested) >= .25; }

        void Advance(double now, double response)
        {
            if (!Moving() || now <= sampledAt) return;
            axis = std::lerp(axis, requested, 1.0 - std::exp(-(now - sampledAt) / response));
            if (!Moving()) axis = requested;
            sampledAt = now;
        }

        void Observe(const RECT& bounds, int pointerAxis, double now, double response, bool resume)
        {
            if (IsRectEmpty(&bounds))
            {
                tracking = false;
                return;
            }
            if (IsRectEmpty(&focus) || (!tracking && !resume)) axis = requested = pointerAxis;
            else Advance(now, response);
            requested = pointerAxis;
            sampledAt = now;
            focus = bounds;
            tracking = true;
        }

        int AxisOr(int pointerAxis) const
        { return IsRectEmpty(&focus) ? pointerAxis : static_cast<int>(std::lround(axis)); }
    };

    struct Group
    {
        std::vector<HWND> windows;
        snowdesktop::dock_running_animation::Presence presence;
        std::uint64_t layoutRevision = 0;
    };
    std::unordered_map<std::wstring, Group> groups;
    std::uint64_t layoutRevision = 0;
    LONG widestItemExtra = 0;
    HoverMotion hover;
    bool primed = false;
};

DockContainer::DockContainer(DesktopApp* app, std::vector<DockEntry>* entries, RECT area)
    : app_(app), entries_(entries), area_(area),
      presentationMotion_(std::make_shared<PresentationMotion>()) {}

HMONITOR DockContainer::GetMonitor() const
{
    if (!app_) return nullptr;
    RECT screen = area_;
    OffsetRect(&screen, app_->virtualLeft_, app_->virtualTop_);
    return MonitorFromRect(&screen, MONITOR_DEFAULTTONEAREST);
}

DockMonitorLayout DockContainer::MonitorLayout() const
{
    return app_ ? app_->DockLayoutForMonitor(GetMonitor()) : DockMonitorLayout{};
}

DockPosition DockContainer::Position() const
{
    return MonitorLayout().position;
}

bool DockContainer::ShowsRunningArea() const
{
    return MonitorLayout().showRunningArea;
}

DockContainer::WindowTitleGroup DockContainer::ResolveTitleGroup(const std::wstring& key,
    std::span<const HWND> windows, float itemPresence) const
{
    auto& transitions = *presentationMotion_;
    const bool visible = !windows.empty();
    auto found = transitions.groups.find(key);
    if (found == transitions.groups.end())
    {
        if (!visible) return {};
        found = transitions.groups.try_emplace(key).first;
    }
    auto& state = found->second;
    state.layoutRevision = transitions.layoutRevision;
    if (visible && !std::ranges::equal(state.windows, windows))
        state.windows.assign(windows.begin(), windows.end());

    const bool animate = transitions.primed && snowdesktop::animation::RuntimeAnimationsEnabled() &&
        app_->IsDockContainerInteractionVisible(this);
    if (state.presence.Visible() != visible || (!animate && state.presence.IsAnimating()))
    {
        state.presence.SetVisible(visible, snowdesktop::UiAnimationScheduler::MonotonicMilliseconds(),
            animate, snowdesktop::animation::RuntimeDurationScale());
        if (state.presence.IsAnimating()) app_->EnsureUiAnimationFrame();
    }
    if (state.presence.IsHidden()) return {};
    return {state.windows, state.presence.Amount(), itemPresence, state.presence.Interactive()};
}

DockContainer::WindowTitleGroup DockContainer::EntryTitleGroup(size_t index) const
{
    if (!app_ || !ShowsRunningArea() ||
        (!app_->dockSettings_.showWindowTitles && presentationMotion_->groups.empty()) ||
        !entries_ || index >= SortableEntryCount() || (*entries_)[index].type != DockEntryType::DesktopItem)
        return {};
    const auto item = app_->FindItemIndexByKey((*entries_)[index].reference);
    if (item >= app_->items_.size()) return {};
    const auto key = DockItemWindowKey(app_->items_[item]);
    const auto state = app_->dockRunningWindows_.find(key);
    std::span<const HWND> windows;
    if (state != app_->dockRunningWindows_.end() && !app_->dockTaskWindowTitles_.empty())
    {
        const auto& candidates = state->second.documentWindows.empty() ?
            state->second.trackedWindows : state->second.documentWindows;
        if (app_->dockSettings_.ShowsWindowTitles(candidates.size())) windows = candidates;
    }
    return ResolveTitleGroup(key, windows);
}

DockContainer::WindowTitleGroup DockContainer::RunningTitleGroup(size_t index) const
{
    if (!app_ || !ShowsRunningArea() ||
        (!app_->dockSettings_.showWindowTitles && presentationMotion_->groups.empty()) ||
        index >= app_->dockUnpinnedRunningApps_.size()) return {};
    const auto& running = app_->dockUnpinnedRunningApps_[index];
    if (running.minimizedThumbnail) return {};
    std::span<const HWND> windows;
    const auto& candidates = running.documentWindows.empty() ? running.trackedWindows : running.documentWindows;
    if (!app_->dockTaskWindowTitles_.empty() && app_->dockSettings_.ShowsWindowTitles(candidates.size()))
        windows = candidates;
    return ResolveTitleGroup(running.identityKey, windows, running.presence.Amount());
}

DockContainer::WindowTitleLayout* DockContainer::MeasureWindowTitle(HWND window, int icon) const
{
    if (!app_ || !app_->dwriteFactory_) return nullptr;
    const auto found = app_->dockTaskWindowTitles_.find(window);
    if (found == app_->dockTaskWindowTitles_.end() || found->second.empty())
    {
        const auto cached = windowTitleLayouts_.find(window);
        return cached != windowTitleLayouts_.end() && cached->second.full ? &cached->second : nullptr;
    }
    const float fontSize = std::clamp(icon * .29f, 10.f, 20.f);
    const auto revision = snowdesktop::app_fonts::revision.load();
    auto& text = windowTitleLayouts_[window];
    if (text.title == found->second && text.fontSize == fontSize && text.fontRevision == revision)
        return text.full ? &text : nullptr;
    text = {};
    text.title = found->second;
    text.fontSize = fontSize;
    text.fontRevision = revision;
    const auto language = Locale::Instance().GetEffectiveLanguage();
    const std::wstring locale(language.begin(), language.end());
    auto* factory = app_->dwriteFactory_.Get();
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(snowdesktop::app_fonts::CreateTextFormat(factory, L"Segoe UI",
        DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
        fontSize, locale.empty() ? L"en-US" : locale.c_str(), &format))) return nullptr;
    format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    if (FAILED(factory->CreateTextLayout(text.title.c_str(), static_cast<UINT32>(text.title.size()),
        format.Get(), 65536.f, 100.f, &text.full))) return nullptr;
    DWRITE_TEXT_METRICS metrics{};
    if (SUCCEEDED(text.full->GetMetrics(&metrics))) text.fullWidth = metrics.width;
    return &text;
}

int DockContainer::WindowTitlePadding() const
{
    return std::clamp(ScaledSpacing() / 2, 3, 6);
}

float DockContainer::WindowTitleWidthLimit() const
{
    return static_cast<float>(std::clamp(ItemIconSize() * 5 / 2, 84, 158) - 2 * WindowTitlePadding());
}

float DockContainer::WindowTitleExtent(HWND window) const
{
    const int icon = ItemIconSize();
    if (IsVertical()) return static_cast<float>(std::max(18, icon / 2 + ScaledSpacing()));
    const auto* text = MeasureWindowTitle(window, icon);
    const float width = text ? std::ceil(text->fullWidth) : 0.f;
    return std::min(width, WindowTitleWidthLimit()) + 2 * WindowTitlePadding();
}

float DockContainer::WindowTitleSlotUnits(WindowTitleGroup group) const
{
    if (!group.Visible()) return 0.f;
    float extent = static_cast<float>(2 * WindowTitlePadding());
    for (const auto window : group.windows) extent += WindowTitleExtent(window);
    return extent * group.amount / ItemPitch();
}

DockContainer::WindowTitleGeometry DockContainer::LayoutWindowTitleGroup(
    RECT base, RECT visual, WindowTitleGroup group) const
{
    if (!group.Visible() || IsRectEmpty(&visual)) return {{}, visual, {}, 1.f};
    const int magnifiedIcon = GetElementIconSize(base, visual);
    const float zoom = static_cast<float>(magnifiedIcon) / ItemIconSize();
    const int size = std::max(1, static_cast<int>(std::lround(magnifiedIcon * group.itemPresence)));
    const int spacing = static_cast<int>(std::lround((ItemPitch() - ItemIconSize()) * group.itemPresence));
    const int padding = static_cast<int>(std::lround(WindowTitlePadding() * zoom * group.amount * group.itemPresence));
    // The slot supplies the same external gap as every icon-only item.
    // The capsule then owns the icon's internal inset on all three exposed edges.
    if (IsVertical())
    {
        const LONG left = visual.left + (visual.right - visual.left - size) / 2;
        const LONG top = visual.top + spacing / 2;
        const RECT plate{left - padding, top, left + size + padding, visual.bottom - (spacing + 1) / 2};
        const RECT icon{left, top + padding, left + size, top + padding + size};
        return {plate, icon, {plate.left, icon.bottom, plate.right, plate.bottom - padding}, zoom};
    }
    const LONG top = visual.top + (visual.bottom - visual.top - size) / 2;
    const LONG left = visual.left + spacing / 2;
    const RECT plate{left, top - padding, visual.right - (spacing + 1) / 2, top + size + padding};
    const RECT icon{left + padding, top, left + padding + size, top + size};
    return {plate, icon, {icon.right, plate.top, plate.right - padding, plate.bottom}, zoom};
}

RECT DockContainer::WindowTitleIconRect(RECT base, RECT visual, WindowTitleGroup group) const
{
    return LayoutWindowTitleGroup(base, visual, group).icon;
}

std::vector<RECT> DockContainer::WindowTitleSegments(
    const WindowTitleGeometry& geometry, WindowTitleGroup group) const
{
    if (!group.Visible() || IsRectEmpty(&geometry.titles)) return {};
    const auto& area = geometry.titles;
    const bool vertical = IsVertical();
    std::vector<float> lengths;
    lengths.reserve(group.windows.size());
    float total = 0.f;
    for (const auto window : group.windows)
    {
        lengths.push_back(WindowTitleExtent(window));
        total += lengths.back();
    }
    const LONG leading = vertical ? area.top : area.left;
    const float scale = (vertical ? area.bottom - leading : area.right - leading) / total;
    float position = 0.f;
    std::vector<RECT> segments;
    segments.reserve(group.windows.size());
    for (const float length : lengths)
    {
        const LONG begin = leading + std::lround(position * scale);
        position += length;
        const LONG end = leading + std::lround(position * scale);
        if (vertical) segments.push_back({area.left, begin, area.right, end});
        else segments.push_back({begin, area.top, end, area.bottom});
    }
    return segments;
}

HWND DockContainer::WindowTitleAtPoint(POINT point, RECT* titleRect) const
{
    if (!app_ || !app_->dockSettings_.showWindowTitles || !ShowsRunningArea() ||
        !IsPointInScrollViewport(point)) return nullptr;
    const auto hit = [&](RECT base, WindowTitleGroup group) -> HWND {
        if (!group.interactive) return nullptr;
        const RECT visual = GetElementVisualRect(base, point);
        const auto segments = WindowTitleSegments(LayoutWindowTitleGroup(base, visual, group), group);
        for (size_t index = 0; index < segments.size(); ++index)
        {
            const RECT segment = segments[index];
            if (PtInRect(&segment, point))
            {
                if (titleRect) *titleRect = segment;
                return group.windows[index];
            }
        }
        return nullptr;
    };
    if (const auto* entry = EntryAtPoint(point))
        return hit(entry->GetBounds(), EntryTitleGroup(entry->GetEntryIndex()));
    if (const auto* running = RunningItemAtPoint(point))
        return hit(running->GetBounds(), RunningTitleGroup(running->GetRunningIndex()));
    return nullptr;
}

RECT DockContainer::DrawWindowTitleGroup(ID2D1DeviceContext* context, RECT base, RECT visual,
    WindowTitleGroup group) const
{
    if (!context || !app_ || !app_->dwriteFactory_ || !group.Visible() || IsRectEmpty(&visual)) return visual;
    const auto geometry = LayoutWindowTitleGroup(base, visual, group);
    const bool vertical = IsVertical();
    const auto monitor = GetMonitor();
    const auto appearance = app_->CurrentDockAppearance(monitor);
    const bool light = appearance.contentTheme == 1;
    const auto& beauty = app_->iconBeautifySettings_;
    const float opacity = group.itemPresence * group.amount;
    const float surface = light ? .9f : .13f;
    const float material = beauty.backgroundStartR * .2126f + beauty.backgroundStartG * .7152f +
        beauty.backgroundStartB * .0722f;
    const bool lightPlate = beauty.enabled ? std::lerp(surface, material, beauty.backgroundOpacity * opacity) > .52f : light;
    const float ink = lightPlate ? .07f : .98f;
    app_->DrawDockControlBackground(context, geometry.plate, 0, !light,
        reinterpret_cast<std::uintptr_t>(group.windows.front()), monitor, true, opacity);
    ComPtr<ID2D1SolidColorBrush> brush;
    const auto color = D2D1::ColorF(ink, ink, ink, opacity);
    if (FAILED(context->CreateSolidColorBrush(color, &brush))) return geometry.icon;

    const POINT pointer = app_->lastMousePoint_;
    const auto segments = WindowTitleSegments(geometry, group);
    const int textPadding = static_cast<int>(std::lround(WindowTitlePadding() * geometry.zoom));
    const int baseIconSize = ItemIconSize();
    D2D1_MATRIX_3X2_F transform{};
    context->GetTransform(&transform);
    for (size_t index = 0; index < segments.size(); ++index)
    {
        const HWND window = group.windows[index];
        if (!group.interactive && app_->dockSettings_.showWindowTitles && !app_->dockTaskWindowTitles_.contains(window))
            continue;
        auto* text = MeasureWindowTitle(window, baseIconSize);
        if (!text) continue;
        RECT bounds = segments[index];
        if (IsRectEmpty(&bounds)) continue;
        if (index > 0)
        {
            brush->SetColor(D2D1::ColorF(ink, ink, ink, .23f * opacity));
            if (vertical && bounds.right - bounds.left > 10)
                context->DrawLine(D2D1::Point2F(static_cast<float>(bounds.left + 5), static_cast<float>(bounds.top)),
                    D2D1::Point2F(static_cast<float>(bounds.right - 5), static_cast<float>(bounds.top)), brush.Get());
            if (!vertical && bounds.bottom - bounds.top > 12)
                context->DrawLine(D2D1::Point2F(static_cast<float>(bounds.left), static_cast<float>(bounds.top + 6)),
                    D2D1::Point2F(static_cast<float>(bounds.left), static_cast<float>(bounds.bottom - 6)), brush.Get());
        }
        const bool hovered = group.interactive && group.amount >= .98f &&
            PtInRect(&bounds, pointer) && IsPointInScrollViewport(pointer);
        bounds.left += textPadding;
        bounds.right -= textPadding;
        const float width = std::min(static_cast<float>(bounds.right - bounds.left),
            WindowTitleWidthLimit() * geometry.zoom);
        const float height = static_cast<float>(bounds.bottom - bounds.top);
        if (width < 4.f || height < 8.f) continue;

        const float zoom = geometry.zoom;
        const float unscaledHeight = height / zoom;
        auto* layout = text->full.Get();
        if (layout->GetMaxHeight() != unscaledHeight) layout->SetMaxHeight(unscaledHeight);
        const float fullWidth = text->fullWidth * zoom;
        const float overflow = fullWidth - width;
        const float cycle = fullWidth + std::max(16.f, 2.f * text->fontSize) * zoom;
        float travel = 0.f;
        if (hovered && overflow > 1.f)
        {
            const auto now = GetTickCount64();
            if (titleMarqueeWindow_ != window)
            {
                titleMarqueeWindow_ = window;
                titleMarqueeStart_ = now;
            }
            constexpr float delayMs = 500.f;
            constexpr float speed = .045f; // Pixels per millisecond at the normal size.
            const float elapsed = static_cast<float>(now - titleMarqueeStart_);
            travel = std::max(0.f, elapsed - delayMs) * speed * zoom;
            titleMarqueeAnimating_ = true;
            app_->EnsureUiAnimationFrame();
        }
        else if (titleMarqueeWindow_ == window)
        {
            titleMarqueeWindow_ = nullptr;
            titleMarqueeAnimating_ = false;
        }
        const float offset = std::fmod(travel, cycle);
        brush->SetColor(color);
        const float left = static_cast<float>(bounds.left);
        const float top = static_cast<float>(bounds.top);
        const float centerOffset = vertical ? std::max(0.f, width - fullWidth) * .5f : 0.f;
        const float origin = left + centerOffset - offset;
        ComPtr<ID2D1LinearGradientBrush> fade;
        if (overflow > 1.f)
        {
            const float ramp = std::min(12.f * zoom, width * .25f) / width;
            auto leading = color;
            auto trailing = color;
            leading.a *= 1.f - std::min(1.f, travel / (3.f * zoom));
            trailing.a *= 1.f - std::min(1.f, (overflow + travel) / (3.f * zoom));
            const D2D1_GRADIENT_STOP stops[]{{0.f, leading}, {ramp, color},
                {1.f - ramp, color}, {1.f, trailing}};
            ComPtr<ID2D1GradientStopCollection> collection;
            if (SUCCEEDED(context->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &collection)))
                context->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(
                    D2D1::Point2F((left - origin) / zoom, 0.f),
                    D2D1::Point2F((left + width - origin) / zoom, 0.f)), collection.Get(), &fade);
        }
        context->PushAxisAlignedClip(D2D1::RectF(left, top, left + width, top + height), D2D1_ANTIALIAS_MODE_ALIASED);
        context->SetTransform(D2D1::Matrix3x2F::Scale(zoom, zoom) * D2D1::Matrix3x2F::Translation(origin, top) * transform);
        ID2D1Brush* inkBrush = fade ? static_cast<ID2D1Brush*>(fade.Get()) : brush.Get();
        context->DrawTextLayout(D2D1::Point2F(0.f, 0.f), layout, inkBrush);
        if (offset + width > cycle)
            context->DrawTextLayout(D2D1::Point2F(cycle / zoom, 0.f), layout, inkBrush);
        context->SetTransform(transform);
        context->PopAxisAlignedClip();
    }
    return geometry.icon;
}

DockContainer::PresentationState DockContainer::CapturePresentationState() const
{
    return {area_, magnificationFocusRect_, scrollOffset_,
        magnificationEntry_, singleMagnification_, entryRemovals_, presentationMotion_, keyboardFocusKey_};
}

bool DockContainer::RestorePresentationState(const PresentationState& state)
{
    if (!EqualRect(&area_, &state.reservedArea))
        return false;

    // BuildSlots clamps the restored viewport against the new model. The
    // focus resolver then checks ownership, pointer leave and suppression
    // before the replacement container's first presentation.
    scrollOffset_ = state.scrollOffset;
    magnificationFocusRect_ = state.focusRect;
    magnificationEntry_ = state.entry;
    singleMagnification_ = state.single;
    if (state.removals) entryRemovals_ = state.removals;
    if (state.motion) presentationMotion_ = state.motion;
    keyboardFocusKey_ = state.keyboardFocusKey;
    ReconcileEntryRemovals();
    return true;
}

void DockContainer::SetReservedArea(RECT area)
{
    if (area_.left == area.left && area_.top == area.top &&
        area_.right == area.right && area_.bottom == area.bottom)
        return;

    area_ = area;
    InvalidateSlots();
    hoveredTitleLayout_ = {};
    hoveredTitleBoundsCacheText_.clear();
    hoveredTitleBoundsCacheAnchor_ = {};
    hoveredTitleBoundsCache_ = {};
    magnificationFocusRect_ = {};
    magnificationEntry_ = {};
    singleMagnification_ = {};
    presentationMotion_->hover = {};
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
    return app_ && (Position() == DockPosition::Left ||
        Position() == DockPosition::Right);
}

bool DockContainer::IsEdgeAttached() const
{
    return app_ && MonitorLayout().edgeAttached;
}

bool DockContainer::UsesSpreadEdge() const
{
    const auto layout = MonitorLayout();
    return layout.edgeAttached && !layout.edgeCentered;
}

bool DockContainer::IsMergedWithStatusBar() const
{
    if (!app_ || !app_->statusBar_ || !IsEdgeAttached()) return false;
    RECT screen = area_;
    OffsetRect(&screen, app_->virtualLeft_, app_->virtualTop_);
    const auto merged = app_->statusBar_->MergedDockArea(MonitorFromRect(&screen, MONITOR_DEFAULTTONULL));
    // The bar negotiates its new bounds before the asynchronous Dock relayout.
    // Keep the logical association through that gap; comparing transient
    // rectangles can briefly reveal a standalone, clipped Dock instead.
    return merged.has_value();
}

bool DockContainer::SharesStatusBarAppearance() const
{
    // Input is deliberately disabled while the shared strip animates. That
    // must not switch the Dock back to a second, independently drawn chrome.
    return IsMergedWithStatusBar();
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
    separatorEntryCount_ = 0;
    folderSeparatorCount_ = 0;
    quickSearchEntryIndex_ = static_cast<size_t>(-1);
    if (entries_)
        for (size_t i = 0; i < entries_->size(); ++i)
        {
            const auto type = (*entries_)[i].type;
            if (type == DockEntryType::Separator) ++(i < mainEntryCount_ ? separatorEntryCount_ : folderSeparatorCount_);
            if (type == DockEntryType::QuickSearch) quickSearchEntryIndex_ = i;
        }
    entryGroupCountGeneration_ = generation;
}

float DockContainer::FixedEntryUnits() const
{
    RefreshEntryGroupCounts();
    float units = static_cast<float>(mainEntryCount_) - static_cast<float>(separatorEntryCount_) * (1.0f - SeparatorSlotUnits());
    if (app_ && ShowsRunningArea() &&
        (app_->dockSettings_.showWindowTitles || !presentationMotion_->groups.empty()))
        for (size_t index = 0; index < mainEntryCount_; ++index)
            units += WindowTitleSlotUnits(EntryTitleGroup(index));
    return units;
}

float DockContainer::FolderEntryUnits() const
{
    RefreshEntryGroupCounts();
    const bool hiddenSearch = app_ && !app_->dockSettings_.showQuickSearchButton &&
        quickSearchEntryIndex_ < mainEntryCount_ + folderEntryCount_;
    const float units = static_cast<float>(folderEntryCount_ - static_cast<size_t>(hiddenSearch)) -
        static_cast<float>(folderSeparatorCount_) * (1.0f - SeparatorSlotUnits());
    const float extent = units + static_cast<float>(HasImplicitSearchSlot()) +
        static_cast<float>(EmptyRunningDragFileSlots()) + AdditionalSlotUnits(true);
    return extent <= 0.0f && app_ ? 0.5f : extent;
}

float DockContainer::SeparatorSlotUnits() const
{
    return static_cast<float>(ScaledSeparatorGap()) / static_cast<float>(ItemPitch());
}

size_t DockContainer::ModelEntryCount() const
{
    return entries_ ? entries_->size() : 0;
}

bool DockContainer::HasImplicitSearchSlot() const
{
    RefreshEntryGroupCounts();
    return app_ && app_->dockSettings_.showQuickSearchButton && quickSearchEntryIndex_ >= ModelEntryCount();
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

bool DockContainer::IsFolderDragSource(Item* source) const
{
    if (!app_) return false;
    if (auto* dockItem = dynamic_cast<DockEntryItem*>(source))
    {
        const size_t index = dockItem->GetEntryIndex();
        if (index >= app_->dockEntries_.size()) return false;
        const auto& entry = app_->dockEntries_[index];
        if (app_->IsRecycleBinDockEntry(entry))
            return !app_->dockSettings_.freeItemOrder ||
                entry.freeOrderSection.value_or(DockEntrySection::Files) == DockEntrySection::Files;
        return entry.type == DockEntryType::QuickSearch ||
            (entry.type == DockEntryType::Separator && entry.freeOrderSection == DockEntrySection::Files) ||
            app_->IsFolderDockEntry(entry);
    }
    if (auto* widget = dynamic_cast<Widget*>(source))
    {
        const DesktopWidget* data = widget->GetWidgetData();
        return data &&
            (data->type == DesktopWidgetType::FolderMapping || data->type == DesktopWidgetType::FileCategories ||
             data->type == DesktopWidgetType::FileGroup);
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
            (app_->widgets_[widgetIndex].type == DesktopWidgetType::FolderMapping ||
             app_->widgets_[widgetIndex].type == DesktopWidgetType::FileCategories);
    }
    const DesktopItem* item = nullptr;
    if (const auto* icon = dynamic_cast<const DesktopIcon*>(source))
        item = icon->GetDesktopItem();
    else if (const auto* frequent = dynamic_cast<const DockFrequentItem*>(source);
        frequent && frequent->GetItemIndex() < app_->items_.size())
        item = &app_->items_[frequent->GetItemIndex()];
    if (item)
    {
        const std::wstring& reference =
            item->layoutKey.empty()
            ? item->parsingName
            : item->layoutKey;
        const DockEntry probe{
            DockEntryType::DesktopItem,
            reference,
            true,
        };
        return app_->IsRecycleBinDockEntry(probe) || app_->ResolveDockFolderTarget(
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
            (app_->widgets_[app_->mouseDownWidgetIndex_].type == DesktopWidgetType::FolderMapping ||
             app_->widgets_[app_->mouseDownWidgetIndex_].type == DesktopWidgetType::FileGroup ||
             app_->widgets_[app_->mouseDownWidgetIndex_].type == DesktopWidgetType::FileCategories);
    }
    return std::all_of(sourceItems.begin(), sourceItems.end(),
        [this](Item* source) { return IsFolderDragSource(source); });
}

bool DockContainer::HasMixedFolderDragSource() const
{
    if (!app_ || app_->dockSettings_.freeItemOrder) return false;
    bool folders = false, main = false;
    if (app_->dragDropController_.IsExternalDragActive())
        for (const auto& path : app_->externalFileDropPaths_)
        {
            const bool folder = snowdesktop::item_location::ResolveFolderTarget(path).kind !=
                snowdesktop::item_location::FolderTargetKind::None;
            (folder ? folders : main) = true;
        }
    for (auto* source : app_->dragSession_.Items())
        (IsFolderDragSource(source) ? folders : main) = true;
    return folders && main;
}

snowdesktop::dock_folder_rules::InsertRange DockContainer::GetDragInsertRange(POINT point) const
{
    const auto main = SortableEntryCount();
    const bool freeOrder = app_ && app_->dockSettings_.freeItemOrder;
    const auto end = SortableFileEnd(entries_, freeOrder);
    const auto folders = end - main;
    if (freeOrder)
    {
        const auto& sources = app_->dragSession_.SourceList().entries;
        const bool searchDrag = std::any_of(sources.begin(), sources.end(),
            [](const auto& source) { return source.fromDock && source.dockEntryType == DockEntryType::QuickSearch; });
        return snowdesktop::dock_folder_rules::GroupInsertRange(searchDrag || GetSectionAtPoint(point) == DockEntrySection::Files, main, folders);
    }
    if (HasMixedFolderDragSource()) return {0, end};
    return snowdesktop::dock_folder_rules::GroupInsertRange(HasOnlyFolderDragSource(), main, folders);
}

int DockContainer::ItemIconSize() const
{
    if (!app_) return kIconSize;
    if (IsMergedWithStatusBar())
    {
        const int height = static_cast<int>(area_.bottom - area_.top);
        const int padding = snowdesktop::MergedDockVerticalPadding(height, ScaledSpacing());
        const int available = std::max(1, height - 2 * padding);
        return std::max(1, static_cast<int>(std::round(available * ClampMergedDockIconScale(MonitorLayout().mergedIconScale))));
    }
    const POINT center{
        (area_.left + area_.right) / 2,
        (area_.top + area_.bottom) / 2
    };
    const GridPage* page = app_->GridPageFromPoint(center);
    const int baseIconSize = page ? app_->GetDockPageItemIconSize(*page) : kIconSize;
    return std::max(1, static_cast<int>(std::round(
        baseIconSize * ClampDockScale(MonitorLayout().thicknessScale))));
}

int DockContainer::ItemPitch() const
{
    return std::max(1, ItemIconSize() + ScaledSpacing());
}

int DockContainer::ScaledSpacing() const
{
    const float scale = app_
        ? ResolveDockScale(MonitorLayout(), IsMergedWithStatusBar()) : 1.0f;
    return std::max(1, static_cast<int>(std::round(kDockSpacing * scale)));
}

int DockContainer::ScaledSeparatorGap() const
{
    const float scale = app_
        ? ResolveDockScale(MonitorLayout(), IsMergedWithStatusBar()) : 1.0f;
    return std::max(1, static_cast<int>(std::round(kDockSeparatorGap * scale)));
}

float DockContainer::RunningSlotUnits() const
{
    if (!app_ || !ShowsRunningArea()) return 0.0f;
    const int pitch = ItemPitch();
    const float iconFraction = IsVertical() ? 0.0f : static_cast<float>(ItemIconSize()) / pitch;
    float units = 0.0f;
    for (size_t index = 0; index < app_->dockUnpinnedRunningApps_.size(); ++index)
    {
        const auto& running = app_->dockUnpinnedRunningApps_[index];
        units += RunningItemSlotUnits(running, iconFraction) +
            WindowTitleSlotUnits(RunningTitleGroup(index)) * running.presence.Amount();
    }
    return units + RunningDividerAmount(app_->dockUnpinnedRunningApps_) * ScaledSeparatorGap() / pitch;
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
            return app_->GetDockPageEdgeMargin(*page, IsVertical());
    }
    return ScaleDockEdgeDistance(IsVertical() ? kGridMarginX : kGridMarginY,
        app_ ? MonitorLayout().edgeDistanceScale : 1.0f);
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

size_t DockContainer::PendingFileCount(bool folders) const
{
    if (!app_ || app_->pendingLandingCaches_.empty()) return 0;
    size_t count = 0;
    const auto monitor = GetMonitor();
    for (const auto& request : app_->pendingLandingCaches_)
        if (request.dockMonitor && request.dockMonitor == monitor && request.waitingForFiles)
        {
            if (request.files.empty() && (request.dockSection == DockEntrySection::Files) == folders) ++count;
            for (const auto& file : request.files)
                if ((request.dockSection ? *request.dockSection == DockEntrySection::Files : file.appearance->isDirectory) == folders)
                    ++count;
        }
    return count;
}

float DockContainer::AdditionalSlotUnits(bool folders) const
{
    return EntryRemovalUnits(folders) + static_cast<float>(PendingFileCount(folders));
}

size_t DockContainer::EmptyRunningDragFixedSlots() const
{
    return app_ && app_->dockRunningDrag_ && SortableEntryCount() == 0 ? 1 : 0;
}

size_t DockContainer::EmptyRunningDragFileSlots() const
{
    if (!app_ || !entries_ || app_->dockSettings_.freeItemOrder || !app_->IsDockRunningDragExplorer())
        return 0;
    const auto begin = entries_->begin() + static_cast<std::ptrdiff_t>(FolderEntryBegin());
    return std::none_of(begin, entries_->end(), [&](const DockEntry& entry) {
        return entry.type != DockEntryType::QuickSearch && !app_->IsRecycleBinDockEntry(entry);
    }) ? 1 : 0;
}

RECT DockContainer::GetBounds() const
{
    ReconcileEntryRemovals();
    return BoundsForRunningUnits(RunningSlotUnits());
}

RECT DockContainer::BoundsForRunningUnits(float runningUnits) const
{
    const size_t frequentCount = app_
        ? app_->GetFrequentDockItemIndices(MonitorLayout()).size() : 0;
    const bool showWindowsButton = app_ && ShowDockWindowsButton(app_->dockSettings_);
    const auto axis = snowdesktop::dock_entry_removal::LayoutDockAxis(
        showWindowsButton, FixedEntryUnits() + static_cast<float>(EmptyRunningDragFixedSlots()) + AdditionalSlotUnits(false),
        runningUnits, static_cast<float>(frequentCount),
        FolderEntryUnits());
    const bool vertical = IsVertical();
    const int iconSize = ItemIconSize();
    const int slotLength = ItemPitch();
    const int spacing = ScaledSpacing();
    const int separatorGap = ScaledSeparatorGap();
    const int desiredLength = snowdesktop::dock_running_animation::AxisLength(axis.units, slotLength) + spacing +
        snowdesktop::dock_running_animation::AxisLength(axis.gaps, separatorGap);
    const int areaWidth = std::max(1, static_cast<int>(area_.right - area_.left));
    const int areaHeight = std::max(1, static_cast<int>(area_.bottom - area_.top));
    const int maxLength = std::max(1,
        (vertical ? areaHeight : areaWidth) - spacing * 2);
    const int length = std::min(desiredLength, maxLength);
    const int desiredThickness = iconSize + spacing * 2;
    const int thickness = std::min(desiredThickness,
        vertical ? areaWidth : areaHeight);
    if (IsMergedWithStatusBar())
    {
        const int left = area_.left + (areaWidth - length) / 2;
        const int top = area_.top + (areaHeight - thickness) / 2;
        return {left, top, left + length, top + thickness};
    }
    if (IsEdgeAttached())
    {
        const int leading = vertical ? area_.top : area_.left;
        const int trailing = vertical ? area_.bottom : area_.right;
        const int centered = leading + (trailing - leading - length) / 2;
        const int start = UsesSpreadEdge() ? leading : centered;
        const int end = UsesSpreadEdge() ? trailing : centered + length;
        switch (Position())
        {
        case DockPosition::Top:
            return RECT{ start, area_.top, end, area_.top + thickness };
        case DockPosition::Left:
            return RECT{ area_.left, start, area_.left + thickness, end };
        case DockPosition::Right:
            return RECT{ area_.right - thickness, start, area_.right, end };
        case DockPosition::Bottom:
        default:
            return RECT{ start, area_.bottom - thickness, end, area_.bottom };
        }
    }
    const int edgeDistance = EdgeMargin();
    const int innerGap = 0;
    if (vertical)
    {
        const int left = app_ && Position() == DockPosition::Left
            ? area_.left + edgeDistance
            : area_.left + innerGap;
        const int top = area_.top + (areaHeight - length) / 2;
        return RECT{ left, top, left + thickness, top + length };
    }
    const int left = area_.left + (areaWidth - length) / 2;
    const int top = app_ && Position() == DockPosition::Top
        ? area_.top + edgeDistance
        : area_.top + innerGap;
    return RECT{ left, top, left + length, top + thickness };
}

// Keep the slot geometry local to this query. Hit testing and drawing can
// inspect every element without re-enumerating the model for each one.
struct DockContainer::MagnificationLayout
{
    RECT bounds{}, viewport{}, windowsButton{};
    int maxScrollOffset = 0;
    bool edgeAnchored = false;
    std::vector<RECT> candidates;
    std::vector<RECT> wideItems;
    size_t leadingCount = 0;

    bool HasWideItem(const RECT& candidate) const
    {
        return std::ranges::any_of(wideItems, [&](const RECT& group) { return EqualRect(&group, &candidate); });
    }

    float WaveAxis(LONG coordinate, bool vertical, int pitch) const
    {
        float axis = static_cast<float>(coordinate);
        for (const auto& group : wideItems)
        {
            const LONG start = vertical ? group.top : group.left;
            const LONG length = (vertical ? group.bottom : group.right) - start;
            if (length <= pitch || coordinate <= start) continue;
            const float fraction = std::min(1.f, static_cast<float>(coordinate - start) / length);
            axis -= (length - pitch) * fraction;
        }
        return axis;
    }

    std::span<const RECT> ForZone(MagnificationZone zone) const
    {
        const std::span<const RECT> rects = candidates;
        return zone == MagnificationZone::Leading
            ? rects.first(leadingCount) : rects.subspan(leadingCount);
    }

    MagnificationZone ZoneOf(const RECT& rect) const
    {
        for (const auto zone : {MagnificationZone::Leading, MagnificationZone::Trailing})
            if (std::ranges::any_of(ForZone(zone), [&](const RECT& candidate) {
                    return EqualRect(&candidate, &rect) != FALSE;
                }))
                return zone;
        return MagnificationZone::None;
    }
};

DockContainer::MagnificationLayout DockContainer::GetMagnificationLayout() const
{
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    MagnificationLayout layout;
    layout.bounds = GetBounds();
    const float runningUnits = RunningSlotUnits();
    layout.viewport = GetScrollViewport(layout.bounds, runningUnits);
    layout.maxScrollOffset = GetMaxScrollOffset(layout.bounds, runningUnits);
    layout.windowsButton = GetWindowsButtonRect(layout.bounds);
    layout.edgeAnchored = layout.maxScrollOffset > 0 ||
        snowdesktop::dock_magnification::UsesEdgeAnchoredMagnification(
            UsesSpreadEdge(), IsMergedWithStatusBar());

    auto& candidates = layout.candidates;
    const size_t fixedCount = SortableEntryCount();
    const size_t folderBegin = FolderEntryBegin();
    const size_t folderEnd = folderBegin + FolderEntryCount();
    const bool vertical = IsVertical();
    const int pitch = ItemPitch();
    candidates.reserve(slots.size() + 1);
    const auto include = [&](const RECT& cell) {
        if (IsRectEmpty(&cell)) return;
        candidates.push_back(cell);
        const LONG length = vertical ? cell.bottom - cell.top : cell.right - cell.left;
        if (length > pitch + 1) layout.wideItems.push_back(cell);
    };
    include(layout.windowsButton);
    const auto includeVisible = [&](const RECT& cell) {
        RECT visible{};
        if (layout.maxScrollOffset <= 0 || IntersectRect(&visible, &cell, &layout.viewport)) include(cell);
    };
    for (size_t index = 0; index < fixedCount && index < slots.size(); ++index)
        if (slots[index])
            includeVisible(slots[index]->GetBounds());
    for (const auto& item : runningItems_)
        if (item && app_ && item->GetRunningIndex() < app_->dockUnpinnedRunningApps_.size() &&
            app_->dockUnpinnedRunningApps_[item->GetRunningIndex()].presence.Interactive())
            includeVisible(item->GetBounds());
    for (const auto& item : frequentItems_)
        if (item)
            includeVisible(item->GetBounds());

    layout.leadingCount = candidates.size();
    for (size_t index = folderBegin; index < folderEnd && index < slots.size(); ++index)
    {
        if (!slots[index]) continue;
        include(slots[index]->GetBounds());
    }
    if (HasImplicitSearchSlot() && !slots.empty())
        include(slots.back()->GetBounds());
    return layout;
}

bool DockContainer::IsMagnificationSuppressed() const
{
    if (!app_)
        return true;
    return snowdesktop::dock_magnification::
        ShouldSuppressMagnification(
            app_->widgetAction_ ==
                DesktopApp::WidgetAction::Resize,
            app_->IsDockContainerInteractionVisible(this));
}

float DockContainer::GetMaximumMagnificationScale() const
{
    if (!app_ || IsMagnificationSuppressed())
        return 1.0f;
    return snowdesktop::dock_magnification::ResolveFocusScale(
        MonitorLayout().hoverEffect,
        MonitorLayout().hoverScale,
        snowdesktop::animation::RuntimeAnimationsEnabled());
}

float DockContainer::GetCurrentMagnificationScale() const
{
    const float maximum = GetMaximumMagnificationScale();
    return app_ && MonitorLayout().hoverEffect == 2
        ? magnificationEntry_.FocusScale(maximum) : maximum;
}

bool DockContainer::IsTitleTransitioning() const
{
    return std::ranges::any_of(presentationMotion_->groups, [](const auto& entry) {
        return entry.second.presence.IsAnimating();
    });
}

bool DockContainer::IsMagnificationAnimating() const
{
    return magnificationEntry_.IsAnimating() || singleMagnification_.IsAnimating() ||
        titleMarqueeAnimating_ || IsTitleTransitioning() || presentationMotion_->hover.Moving();
}

bool DockContainer::AdvanceMagnificationAnimation(double nowMilliseconds)
{
    if (!IsMagnificationAnimating())
        return false;
    // Reconcile leave, drag suppression and preference changes before another
    // frame, even if the pointer stopped moving after entering the Dock.
    if (!app_ || !app_->IsDockContainerInteractionVisible(this))
    {
        titleMarqueeAnimating_ = false;
        titleMarqueeWindow_ = nullptr;
        magnificationFocusRect_ = {};
        magnificationEntry_ = {};
        singleMagnification_ = {};
        presentationMotion_->hover = {};
        if (IsTitleTransitioning())
        {
            for (auto& entry : presentationMotion_->groups)
                entry.second.presence.SetVisible(entry.second.presence.Visible(), nowMilliseconds, false);
            InvalidateSlots();
        }
    }
    else
    {
        if (IsTitleTransitioning())
        {
            for (auto& entry : presentationMotion_->groups)
                entry.second.presence.Advance(nowMilliseconds);
            InvalidateSlots();
        }
        presentationMotion_->hover.Advance(nowMilliseconds,
            48.0 * std::max(.01, snowdesktop::animation::RuntimeDurationScale()));
        ResolveMagnificationFocusRect(app_->lastMousePoint_, GetMagnificationLayout());
        magnificationEntry_.Advance(nowMilliseconds);
        singleMagnification_.Advance(nowMilliseconds);
    }
    return true; // The terminal frame still needs to be presented.
}

int DockContainer::GetLaunchAnimationPadding(bool reserveForLaunch) const
{
    if (!app_)
        return 0;
    const int iconSize = static_cast<int>(std::ceil(
        ItemIconSize() * GetMaximumMagnificationScale()));
    return snowdesktop::dock_launch_animation::PaddingPixels(
        iconSize, MonitorLayout().launchEffect == 1 &&
            snowdesktop::animation::RuntimeAnimationsEnabled(),
        !app_->dockLaunchBounces_.empty(), reserveForLaunch);
}

RECT DockContainer::ResolveMagnificationFocusRect(POINT pointer, const MagnificationLayout& layout) const
{
    if (IsMagnificationSuppressed())
    {
        magnificationFocusRect_ = {};
        magnificationEntry_ = {};
        singleMagnification_ = {};
        presentationMotion_->hover = {};
        return RECT{};
    }

    const auto& candidates = layout.candidates;
    const auto& windowsButton = layout.windowsButton;
    const bool hasOverflow = layout.maxScrollOffset > 0;
    // The currently painted fixed controls can occlude the scrolling lane.
    const RECT viewport = GetVisualScrollViewport(pointer, layout, magnificationFocusRect_);
    const bool pointerInViewport = IsVertical()
        ? pointer.y >= viewport.top && pointer.y < viewport.bottom
        : pointer.x >= viewport.left && pointer.x < viewport.right;
    const auto pointerVisibleFor = [&](const RECT& candidate) {
        return !hasOverflow || pointerInViewport || EqualRect(&candidate, &windowsButton) ||
            layout.ZoneOf(candidate) == MagnificationZone::Trailing;
    };
    const auto previous = std::find_if(
        candidates.begin(), candidates.end(),
        [&](const RECT& candidate) {
            return EqualRect(
                &candidate,
                &magnificationFocusRect_) != FALSE;
        });
    // A pin/reorder can move every cell before this sample. Its old focus
    // still owns hover retention even though no new cell has that rectangle.
    const bool magnificationActive = !IsRectEmpty(&magnificationFocusRect_);

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
            if (!pointerVisibleFor(candidate) || !PtInRect(&candidate, pointer))
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
                    layout.bounds,
                    Position(),
                    ItemIconSize(),
                    magnificationActive,
                    GetMaximumMagnificationScale());
        if (PtInRect(&separatorHoverBounds, pointer))
        {
            RECT nearest{};
            long long nearestDistance =
                std::numeric_limits<long long>::max();
            int nearestAxisDistance =
                std::numeric_limits<int>::max();
            for (const RECT& candidate : candidates)
            {
                if (!pointerVisibleFor(candidate)) continue;
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
                (!layout.edgeAnchored ||
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
                    layout.bounds,
                    Position(),
                    ItemIconSize(),
                    GetMaximumMagnificationScale(), MonitorLayout().hoverEffect == 1);
        if (!PtInRect(&interactive, pointer))
            return RECT{};

        for (const RECT& candidate : candidates)
        {
            if (!pointerVisibleFor(candidate)) continue;
            const RECT magnified = MagnifyElementRect(candidate, candidate, pointer, layout);
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
    if (previous != candidates.end() && pointerVisibleFor(*previous) &&
        EqualRect(&nextFocus, &*previous) == FALSE)
    {
        if (IsRectEmpty(&nextFocus))
        {
            const RECT previousVisual = MagnifyElementRect(*previous, *previous, pointer, layout);
            const RECT retention =
                snowdesktop::dock_magnification::
                    ExpandFocusRetentionBounds(
                        previousVisual);
            if (PtInRect(&retention, pointer))
                nextFocus = *previous;
        }
        else if (layout.ZoneOf(*previous) == layout.ZoneOf(nextFocus) &&
            (!hasOverflow || (EqualRect(&*previous, &windowsButton) != FALSE) ==
                (EqualRect(&nextFocus, &windowsButton) != FALSE)))
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
    // Hit testing also queries the previous mouse point to find dirty bounds.
    // Those historical queries must not restart or cancel the entry timeline.
    if (app_ && pointer.x == app_->lastMousePoint_.x &&
        pointer.y == app_->lastMousePoint_.y)
    {
        const auto settings = MonitorLayout();
        const bool enabled = GetMaximumMagnificationScale() > 1.f;
        const bool wideItemMotion = enabled && !layout.wideItems.empty();
        const double now = snowdesktop::UiAnimationScheduler::MonotonicMilliseconds();
        const double duration = snowdesktop::animation::RuntimeDurationScale();
        const int pointerAxis = IsVertical() ? pointer.y : pointer.x;
        auto& motion = presentationMotion_->hover;
        if (wideItemMotion)
            motion.Observe(nextFocus, pointerAxis, now, 48.0 * std::max(.01, duration),
                magnificationEntry_.IsAnimating() || singleMagnification_.IsAnimating());
        else motion = {};
        magnificationEntry_.SetHovered(!IsRectEmpty(&nextFocus) && settings.hoverEffect == 2 && enabled,
            now, duration * (wideItemMotion ? 1.4 : 1.0), wideItemMotion);
        if (settings.hoverEffect == 1 && enabled)
        {
            // Retarget a relaid-out live hover through SetTarget, preserving
            // its amplitude/deadline. Only an absent replacement drops an
            // owner whose geometry no longer exists.
            const RECT visualOwner = singleMagnification_.CurrentRect();
            if (IsRectEmpty(&nextFocus) && !IsRectEmpty(&visualOwner) && std::none_of(candidates.begin(), candidates.end(),
                [&](const RECT& candidate) { return EqualRect(&candidate, &visualOwner) != FALSE; }))
                singleMagnification_ = {};
            singleMagnification_.SetTarget(nextFocus, now, duration * (wideItemMotion ? 1.75 : 1.0),
                motion.AxisOr(pointerAxis));
        }
        else singleMagnification_ = {};
        if (IsMagnificationAnimating())
            app_->EnsureUiAnimationFrame();
    }
    return nextFocus;
}

snowdesktop::dock_magnification::SingleFocusGeometry
DockContainer::GetSingleMagnificationGeometry(
    const RECT& baseRect, const MagnificationLayout& layout) const
{
    const RECT visualOwner = singleMagnification_.CurrentRect();
    if (IsRectEmpty(&visualOwner) || GetMaximumMagnificationScale() <= 1.0f) return {};
    std::span<const RECT> zoneRects = layout.candidates;
    if (layout.edgeAnchored)
    {
        const auto zone = layout.ZoneOf(baseRect);
        if (zone == MagnificationZone::None || zone != layout.ZoneOf(visualOwner)) return {};
        zoneRects = layout.ForZone(zone);
    }
    std::vector<RECT> candidates(zoneRects.begin(), zoneRects.end());
    if (layout.maxScrollOffset > 0)
    {
        const RECT start = layout.windowsButton;
        const bool startOwner = EqualRect(&visualOwner, &start) != FALSE;
        if ((EqualRect(&baseRect, &start) != FALSE) != startOwner) return {};
        std::erase_if(candidates, [&](const RECT& candidate) {
            return (EqualRect(&candidate, &start) != FALSE) != startOwner;
        });
    }
    return snowdesktop::dock_magnification::ResolveSingleFocusGeometry(
        candidates, IsVertical(), presentationMotion_->hover.AxisOr(singleMagnification_.PointerAxis()),
        ItemIconSize(), singleMagnification_.Scale(), layout.wideItems);
}

RECT DockContainer::MagnifyElementRect(const RECT& baseRect, const RECT& focusRect,
    POINT pointer, const MagnificationLayout& layout) const
{
    if (!app_) return baseRect;
    const auto settings = MonitorLayout();
    if (settings.hoverEffect == 0) return baseRect;
    namespace magnification = snowdesktop::dock_magnification;
    const int iconSize = ItemIconSize();
    const bool vertical = IsVertical();
    const auto leading = [vertical](const RECT& rect) { return vertical ? rect.top : rect.left; };
    const auto trailing = [vertical](const RECT& rect) { return vertical ? rect.bottom : rect.right; };
    const auto center = [&](const RECT& rect) { return (leading(rect) + trailing(rect)) / 2; };
    const auto growthFor = [&](const RECT& rect, float scale) {
        const int iconGrowth = magnification::GrowthForScale(scale, iconSize);
        if (iconGrowth == 0 || !layout.HasWideItem(rect)) return iconGrowth;
        const LONG length = std::max(1L, trailing(rect) - leading(rect) - ScaledSpacing());
        return std::max(iconGrowth, static_cast<int>(std::lround(length * static_cast<float>(iconGrowth) / iconSize)));
    };
    const bool hasOverflow = layout.maxScrollOffset > 0;
    const auto packIsland = [&](const auto& scaleFor, bool centered) {
        const float scale = scaleFor(baseRect);
        // Pack actual pixel widths around the resting rail. If the wave
        // reaches an end, fit it inside the available axis before drawing.
        const LONG pivot = center(layout.bounds);
        const LONG baseCenter = center(baseRect);
        const auto limitedGrowth = [&](const RECT& cell, float cellScale, float factor) {
            if (magnification::GrowthForScale(std::lerp(1.f, cellScale, factor), iconSize) == 0) return 0;
            return static_cast<int>(growthFor(cell, cellScale) * factor);
        };
        struct Packing
        {
            LONG first = LONG_MAX, last = LONG_MIN;
            int total = 0, beforeBase = 0, beforePivot = 0;
        };
        const auto measure = [&](float factor) {
            Packing packing;
            for (const auto& candidate : layout.candidates)
            {
                const LONG start = leading(candidate);
                const LONG end = trailing(candidate);
                packing.first = std::min(packing.first, start);
                packing.last = std::max(packing.last, end);
                const int growth = limitedGrowth(candidate, scaleFor(candidate), factor);
                packing.total += growth;
                if (center(candidate) < baseCenter) packing.beforeBase += growth;
                const float fraction = static_cast<float>(pivot - start) / std::max(1L, end - start);
                packing.beforePivot += static_cast<int>(std::lround(growth * magnification::SmoothStep(fraction)));
            }
            return packing;
        };
        const LONG safeStart = leading(area_) + ScaledSpacing() / 2;
        const LONG safeEnd = trailing(area_) - (ScaledSpacing() + 1) / 2;
        auto packing = measure(1.f);
        const LONG budget = std::max(0L, safeEnd - safeStart - (packing.last - packing.first));
        const float factor = packing.total > budget ? static_cast<float>(budget) / packing.total : 1.f;
        if (factor < 1.f) packing = measure(factor);
        const LONG minimumAnchor = packing.last + packing.total - safeEnd;
        const LONG maximumAnchor = packing.first - safeStart;
        const LONG anchor = minimumAnchor <= maximumAnchor
            ? std::clamp(static_cast<LONG>(packing.beforePivot), minimumAnchor, maximumAnchor) : packing.beforePivot;
        const int axisGrowth = limitedGrowth(baseRect, scale, factor);
        const int shift = packing.beforeBase + axisGrowth / 2 - static_cast<int>(anchor);
        return magnification::MagnifyRect(baseRect, settings.position, std::lerp(1.f, scale, factor),
            iconSize, shift, centered, axisGrowth);
    };
    const auto zone = layout.edgeAnchored ? layout.ZoneOf(baseRect) : MagnificationZone::None;
    const bool inScrollLane = hasOverflow && zone == MagnificationZone::Leading &&
        !EqualRect(&baseRect, &layout.windowsButton);
    const bool fitEdge = layout.edgeAnchored && !hasOverflow && !layout.wideItems.empty() &&
        zone != MagnificationZone::None;
    const auto edgeGrowthFactor = [&](int growth) {
        if (!fitEdge || growth <= 0) return 1.f;
        const bool towardEnd = zone == MagnificationZone::Leading;
        const auto otherZone = towardEnd ? MagnificationZone::Trailing : MagnificationZone::Leading;
        LONG limit = towardEnd ? trailing(area_) : leading(area_);
        for (const auto& candidate : layout.ForZone(otherZone))
            limit = towardEnd ? std::min(limit, leading(candidate)) : std::max(limit, trailing(candidate));
        LONG end = towardEnd ? trailing(baseRect) : leading(baseRect);
        for (const auto& candidate : layout.ForZone(zone))
            end = towardEnd ? std::max(end, trailing(candidate)) : std::min(end, leading(candidate));
        const LONG budget = std::max(0L, (towardEnd ? limit - end : end - limit) - ScaledSpacing());
        return std::min(1.f, static_cast<float>(budget) / growth);
    };
    if (settings.hoverEffect == 1)
    {
        auto geometry = GetSingleMagnificationGeometry(baseRect, layout);
        if (!layout.edgeAnchored && !layout.wideItems.empty())
        {
            return packIsland([&](const RECT& candidate) { return geometry.ScaleFor(candidate, iconSize); }, true);
        }
        const float scale = geometry.ScaleFor(baseRect, iconSize);
        const float leadingScale = geometry.ScaleFor(geometry.leading, iconSize);
        const float trailingScale = geometry.ScaleFor(geometry.trailing, iconSize);
        const int leadingGrowth = growthFor(geometry.leading, leadingScale);
        const int trailingGrowth = growthFor(geometry.trailing, trailingScale);
        const float factor = edgeGrowthFactor(leadingGrowth + trailingGrowth);
        const auto fitGrowth = [&](int growth, float cellScale) {
            return magnification::GrowthForScale(std::lerp(1.f, cellScale, factor), iconSize) > 0
                ? static_cast<int>(growth * factor) : 0;
        };
        geometry.leadingGrowth = fitGrowth(leadingGrowth, leadingScale);
        geometry.trailingGrowth = fitGrowth(trailingGrowth, trailingScale);
        const int axisGrowth = geometry.GrowthFor(baseRect);
        int shift = geometry.AxisShiftFor(baseRect);
        if (fitEdge)
        {
            const int total = geometry.leadingGrowth + geometry.trailingGrowth;
            shift += zone == MagnificationZone::Leading ? total / 2 : -(total - total / 2);
        }
        if (inScrollLane)
        {
            const int growth = geometry.leadingGrowth + geometry.trailingGrowth;
            shift += FitScrollMagnificationShift(growth / 2, growth - growth / 2,
                scrollOffset_, layout.maxScrollOffset);
        }
        return magnification::MagnifyRect(baseRect, settings.position,
            std::lerp(1.f, scale, factor), iconSize, shift, true, axisGrowth);
    }

    const auto& motion = presentationMotion_->hover;
    const RECT& waveFocus = IsRectEmpty(&focusRect) && magnificationEntry_.IsAnimating() ? motion.focus : focusRect;
    if (IsRectEmpty(&waveFocus)) return baseRect;
    const float focusScale = GetCurrentMagnificationScale();
    if (focusScale <= 1.0f) return baseRect;
    const int pitch = ItemPitch();
    int pointerAxis = motion.AxisOr(vertical ? pointer.y : pointer.x);
    float waveAxis = layout.WaveAxis(pointerAxis, vertical, pitch);
    const auto scaleFor = [&](const RECT& candidate) {
        // A wide cell contributes one item's distance to the wave.
        const float distance = layout.WaveAxis(center(candidate), vertical, pitch) - waveAxis;
        return magnification::ScaleForAxisDistance(distance, pitch, focusScale);
    };
    if (!layout.edgeAnchored)
    {
        const float scale = scaleFor(baseRect);
        if (layout.wideItems.empty())
        {
            const int shift = magnification::IslandAxisShift(settings.hoverEffect,
                center(baseRect), center(waveFocus), pointerAxis, pitch, iconSize, focusScale);
            return magnification::MagnifyRect(baseRect, settings.position, scale, iconSize, shift);
        }
        return packIsland(scaleFor, false);
    }

    if (zone == MagnificationZone::None || zone != layout.ZoneOf(waveFocus)) return baseRect;
    const bool startFocused = EqualRect(&waveFocus, &layout.windowsButton) != FALSE;
    const auto inFocusLane = [&](const RECT& candidate) {
        return !hasOverflow || (EqualRect(&candidate, &layout.windowsButton) != FALSE) == startFocused;
    };
    if (!inFocusLane(baseRect)) return baseRect;

    const auto candidates = layout.ForZone(zone);
    // Keep the file lane expanded while the pointer crosses its painted edge.
    if (hasOverflow && zone == MagnificationZone::Trailing)
    {
        pointerAxis = ClampPointerAxisToCenters(pointerAxis, candidates, vertical);
        waveAxis = layout.WaveAxis(pointerAxis, vertical, pitch);
    }

    std::vector<int> growth;
    growth.reserve(candidates.size());
    size_t baseIndex = candidates.size();
    int totalGrowth = 0;
    for (const RECT& candidate : candidates)
    {
        if (!inFocusLane(candidate)) continue;
        if (baseIndex == candidates.size() && EqualRect(&candidate, &baseRect))
            baseIndex = growth.size();
        const int amount = growthFor(candidate, scaleFor(candidate));
        growth.push_back(amount);
        totalGrowth += amount;
    }
    if (baseIndex >= growth.size()) return baseRect;
    const float factor = edgeGrowthFactor(totalGrowth);
    if (factor < 1.f)
    {
        for (size_t index = 0; index < growth.size(); ++index)
        {
            const float fittedScale = std::lerp(1.f, scaleFor(candidates[index]), factor);
            growth[index] = magnification::GrowthForScale(fittedScale, iconSize) > 0
                ? static_cast<int>(growth[index] * factor) : 0;
        }
    }
    int shift = magnification::PackedAxisShift(growth, baseIndex, zone == MagnificationZone::Leading);
    if (inScrollLane)
        shift += FitScrollMagnificationShift(0, totalGrowth, scrollOffset_, layout.maxScrollOffset);
    return magnification::MagnifyRect(baseRect, settings.position, std::lerp(1.f, scaleFor(baseRect), factor), iconSize,
        shift, false, growth[baseIndex]);
}

RECT DockContainer::GetElementVisualRect(
    RECT baseRect, POINT pointer) const
{
    // Hidden controls retain a zero-sized logical insertion boundary. They
    // must not acquire growth, displacement or an invisible drag hit area.
    if (IsRectEmpty(&baseRect)) return baseRect;
    const auto layout = GetMagnificationLayout();
    return MagnifyElementRect(baseRect, ResolveMagnificationFocusRect(pointer, layout), pointer, layout);
}

int DockContainer::GetElementIconSize(const RECT& baseRect, const RECT& visualRect) const
{
    return snowdesktop::dock_magnification::IconSizeForVisualRect(
        baseRect, visualRect, ItemIconSize());
}

RECT DockContainer::EdgeChromeBounds(RECT bounds) const
{
    if (!IsEdgeAttached() || IsMergedWithStatusBar()) return bounds;
    if (IsVertical())
    {
        bounds.top = area_.top;
        bounds.bottom = area_.bottom;
    }
    else
    {
        bounds.left = area_.left;
        bounds.right = area_.right;
    }
    return bounds;
}

RECT DockContainer::GetVisualPanelBounds(POINT pointer) const
{
    if (IsMagnificationSuppressed())
        return EdgeChromeBounds(GetBounds());
    const auto layout = GetMagnificationLayout();
    return GetVisualPanelBounds(pointer, layout, ResolveMagnificationFocusRect(pointer, layout));
}

RECT DockContainer::GetVisualPanelBounds(POINT pointer, const MagnificationLayout& layout, const RECT& focus) const
{
    RECT panel = EdgeChromeBounds(layout.bounds);
    if (IsRectEmpty(&focus) && IsRectEmpty(&singleMagnification_.CurrentRect()) &&
        !magnificationEntry_.IsAnimating())
        return panel;

    const bool hasOverflow = layout.maxScrollOffset > 0;
    const RECT& viewport = layout.viewport;
    const RECT& windowsButton = layout.windowsButton;
    for (const RECT& candidate : layout.candidates)
    {
        RECT visual = MagnifyElementRect(candidate, focus, pointer, layout);
        if (hasOverflow && !EqualRect(&candidate, &windowsButton) &&
            layout.ZoneOf(candidate) == MagnificationZone::Leading)
        {
            // The panel encloses painted content, not the hidden part of a
            // scrolled cell. Fixed controls can still grow at either end.
            if (IsVertical())
            {
                visual.top = std::max(visual.top, viewport.top);
                visual.bottom = std::min(visual.bottom, viewport.bottom);
            }
            else
            {
                visual.left = std::max(visual.left, viewport.left);
                visual.right = std::min(visual.right, viewport.right);
            }
            if (IsRectEmpty(&visual)) continue;
        }
        panel = snowdesktop::dock_magnification::
            ExtendPanelAlongDockAxis(
                panel, visual, Position(),
                ScaledSpacing() / 2);
    }
    if (!IsMergedWithStatusBar())
        panel = snowdesktop::dock_magnification::ClipPanelToWorkArea(panel, area_);
    if (app_->statusBar_ && !IsMergedWithStatusBar())
    {
        const auto monitor = GetMonitor();
        MONITORINFO info{sizeof(info)};
        if (GetMonitorInfoW(monitor, &info))
        {
            RECT workArea = app_->statusBar_->AvailableWorkArea(monitor, info.rcMonitor);
            OffsetRect(&workArea, -app_->virtualLeft_, -app_->virtualTop_);
            panel = snowdesktop::dock_magnification::ClipPanelToWorkArea(panel, workArea);
        }
    }
    return panel;
}

float DockContainer::TitleTooltipScale() const
{
    const RECT bounds = GetBounds();
    const auto* page = app_ ? app_->GridPageFromPoint({(bounds.left + bounds.right) / 2, (bounds.top + bounds.bottom) / 2}) : nullptr;
    return page ? static_cast<float>(page->dpiX) / 96.f : 1.f;
}

RECT DockContainer::CalculateTitleTooltipBounds(const std::wstring& title,
    const RECT& hoveredBounds, IDWriteTextFormat* measurementFormat) const
{
    if (!app_ || title.empty() || !app_->dwriteFactory_) return {};
    const float scale = TitleTooltipScale();
    ComPtr<IDWriteTextFormat> format;
    if (!measurementFormat)
    {
        if (FAILED(snowdesktop::CreateNativeTooltipTextFormat(app_->dwriteFactory_.Get(), &format, scale, false))) return {};
        measurementFormat = format.Get();
    }
    snowdesktop::NativeTooltipTextLayout measured;
    snowdesktop::NativeTooltipLayoutOptions options;
    options.scale = scale; options.centered = true;
    if (FAILED(snowdesktop::MeasureNativeTooltip(app_->dwriteFactory_.Get(), measurementFormat, {}, title,
        300.f * scale, 96.f * scale, measured, options))) return {};
    hoveredTitleLayout_ = measured;
    return PositionTitleTooltipBounds(hoveredBounds, static_cast<int>(std::ceil(measured.width)),
        static_cast<int>(std::ceil(measured.height)));
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
            Position(),
            tooltipWidth,
            tooltipHeight,
            tooltipGap);
    OffsetRect(&tooltip, 0, 4);

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

RECT DockContainer::GetHoveredTitleBounds(POINT pointer) const
{
    if (WindowTitleAtPoint(pointer)) return {};
    if (!app_ || IsMagnificationSuppressed() || app_->IsPointOccludedByOpenPopup(pointer) ||
        app_->dragSession_.IsActive() || app_->dragDropController_.IsTransportActive() ||
        app_->widgetAction_ == DesktopApp::WidgetAction::Move ||
        (app_->dockTrayPopup_ && !app_->popupAnimation_.IsHidden()) ||
        !app_->quickNavigation_->animation.IsHidden() || app_->quickNavigation_->open ||
        app_->HasActiveContextMenuSession())
        return RECT{};

    const auto* host = app_->FindPersistentDockHost(this);
    if (host && app_->statusBar_ && app_->statusBar_->HasInteractionSession(host->monitor))
        return RECT{};

    std::wstring title;
    RECT baseBounds{};
    if (DockEntryItem* entry = EntryAtPoint(pointer))
    {
        const bool popupVisible = app_->popupAnimation_.IsInteractive() ||
            !app_->popupAnimation_.IsHidden();
        bool sameDock = app_->collectionPopupDockHost_ == host;
        if (popupVisible && app_->popupAnchoredToDock_ &&
            sameDock && !host)
        {
            const RECT dockBounds = GetInteractiveBounds();
            sameDock = PtInRect(
                &dockBounds, app_->popupAnchorPoint_) != FALSE;
        }
        if (popupVisible && app_->popupAnchoredToDock_ && sameDock)
        {
            bool isPopupSource = false;
            if (app_->dockFolderPopupOpen_)
            {
                const size_t index = entry->GetEntryIndex();
                if (index < app_->dockEntries_.size())
                {
                    const DockEntry& source = app_->dockEntries_[index];
                    const std::wstring sourceId =
                        std::to_wstring(static_cast<int>(source.type)) +
                        L":" + ToUpperInvariant(source.reference);
                    isPopupSource = sourceId == app_->dockFolderPopupSourceId_;
                }
            }
            else if (app_->popupWidgetIndex_ < app_->widgets_.size() &&
                IsLogicalDockEntryType(entry->GetEntryType()))
            {
                isPopupSource = entry->GetReference() ==
                    app_->widgets_[app_->popupWidgetIndex_].id;
            }
            if (isPopupSource)
                return RECT{};
        }
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

    if (app_->dockWindowPreview_ && !app_->dockWindowPreview_->IsClosing() &&
        app_->dockWindowPreview_->IsVisible() && !app_->dockWindowPreviewKey_.empty())
    {
        DockWindowPreviewTarget target;
        if (app_->ResolveDockWindowPreviewTarget(pointer, target) &&
            target.targetToken == app_->dockWindowPreviewKey_)
            return RECT{};
    }

    const RECT visualBounds =
        GetElementVisualRect(baseBounds, pointer);

    const int position = static_cast<int>(
        Position());
    const bool lightTheme =
        (app_->CurrentDockAppearance(GetMonitor()).contentTheme == 1);
    const float tooltipFontSize = snowdesktop::NativeTooltipFontSize() * TitleTooltipScale();
    const bool cachedMeasurement =
        tooltipFontSize == hoveredTitleBoundsCacheFontSize_ &&
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

    hoveredTitleBoundsCacheFontSize_ = tooltipFontSize;
    hoveredTitleBoundsCacheText_ = title;
    hoveredTitleBoundsCacheAnchor_ =
        visualBounds;
    hoveredTitleBoundsCachePosition_ =
        position;
    hoveredTitleBoundsCacheLightTheme_ =
        lightTheme;
    hoveredTitleLayout_ = {};
    hoveredTitleBoundsCache_ =
        CalculateTitleTooltipBounds(
            title, visualBounds);
    return hoveredTitleBoundsCache_;
}

void DockContainer::ShowHoveredTitle(POINT pointer, HWND owner, snowdesktop::NativeTooltip& tooltip) const
{
    RECT bounds = GetHoveredTitleBounds(pointer);
    if (IsRectEmpty(&bounds) || !hoveredTitleLayout_.layout)
    {
        tooltip.Hide();
        return;
    }
    OffsetRect(&bounds, app_->virtualLeft_, app_->virtualTop_);
    app_->ConfigureFixedTooltip(tooltip, owner);
    tooltip.ShowAt(hoveredTitleBoundsCacheText_, bounds, hoveredTitleLayout_);
}

bool DockContainer::IsFocusedElementRect(
    const RECT& baseRect, POINT pointer) const
{
    const auto layout = GetMagnificationLayout();
    return IsFocusedElementRect(baseRect, pointer, layout, ResolveMagnificationFocusRect(pointer, layout));
}

bool DockContainer::IsFocusedElementRect(const RECT& baseRect, POINT pointer,
    const MagnificationLayout& layout, const RECT& focus) const
{
    if (IsRectEmpty(&focus))
        return PtInRect(&baseRect, pointer) != FALSE;
    if (EqualRect(&baseRect, &focus) == FALSE)
        return false;
    const RECT visualRect = MagnifyElementRect(baseRect, focus, pointer, layout);
    return PtInRect(&visualRect, pointer) != FALSE;
}

bool DockContainer::ContainsInteractivePoint(POINT pt) const
{
    if (!app_ ||
        !app_->IsDockContainerInteractionVisible(this))
    {
        return false;
    }
    const RECT bounds = GetInteractiveBounds();
    return PtInRect(&bounds, pt) != FALSE;
}

RECT DockContainer::GetInteractiveBounds() const
{
    const RECT bounds = EdgeChromeBounds(GetBounds());
    if (IsMagnificationSuppressed())
        return bounds;
    const float scale = GetMaximumMagnificationScale();
    RECT expanded = snowdesktop::dock_magnification::ExpandInteractionBounds(
        bounds, Position(), ItemIconSize(), scale, MonitorLayout().hoverEffect == 1);
    const int itemReach = static_cast<int>(std::ceil(presentationMotion_->widestItemExtra * (scale - 1.f)));
    if (!IsEdgeAttached() && itemReach > 0)
        InflateRect(&expanded, IsVertical() ? 0 : itemReach, IsVertical() ? itemReach : 0);
    return expanded;
}

RECT DockContainer::GetAnimationVisualBounds(bool reserveForLaunch) const
{
    const RECT bounds = GetInteractiveBounds();
    const int padding = GetLaunchAnimationPadding(reserveForLaunch);
    if (padding <= 0)
        return bounds;
    return snowdesktop::dock_magnification::ExpandPerpendicularBounds(
        bounds, Position(), padding, 2.0f);
}

std::vector<RECT> DockContainer::GetOcclusionRects(POINT pointer) const
{
    std::vector<RECT> result;
    if (!app_ || !app_->IsDockContainerInteractionVisible(this))
        return result;
    const auto layout = GetMagnificationLayout();
    const RECT focus = ResolveMagnificationFocusRect(pointer, layout);
    const RECT panel = GetVisualPanelBounds(pointer, layout, focus);
    if (IsRectEmpty(&panel))
        return result;
    result.push_back(panel);
    const RECT viewport = GetVisualScrollViewport(pointer, layout, focus);
    const bool hasOverflow = layout.maxScrollOffset > 0;
    RECT visualViewport = snowdesktop::dock_magnification::
        ExpandPerpendicularBounds(viewport, Position(),
            ItemIconSize(), GetMaximumMagnificationScale(), MonitorLayout().hoverEffect == 1);
    if (const int launchPadding = GetLaunchAnimationPadding(); launchPadding > 0)
        visualViewport = snowdesktop::dock_magnification::ExpandPerpendicularBounds(
            visualViewport, Position(), launchPadding, 2.0f);
    const RECT windowsButton = layout.windowsButton;
    const RECT search = GetSearchRect();
    for (const RECT& base : layout.candidates)
    {
        const bool control = EqualRect(&base, &windowsButton) ||
            EqualRect(&base, &search);
        const bool fixed = control || layout.ZoneOf(base) == MagnificationZone::Trailing;
        RECT intersection{};
        if (!fixed && !IntersectRect(&intersection, &base, &viewport))
            continue;
        const RECT visual = MagnifyElementRect(base, focus, pointer, layout);
        const int iconSize = GetElementIconSize(base, visual);
        RECT icon{
            visual.left + (visual.right - visual.left - iconSize) / 2,
            visual.top + (visual.bottom - visual.top - iconSize) / 2,
            visual.left + (visual.right - visual.left + iconSize) / 2,
            visual.top + (visual.bottom - visual.top + iconSize) / 2};
        size_t launchIndex = static_cast<size_t>(-1);
        for (const auto& item : entryItems_)
        {
            if (!item || item->GetEntryType() != DockEntryType::DesktopItem)
                continue;
            const RECT bounds = item->GetBounds();
            if (EqualRect(&bounds, &base))
            {
                launchIndex = app_->FindItemIndexByKey(item->GetReference());
                break;
            }
        }
        if (launchIndex == static_cast<size_t>(-1))
        {
            for (const auto& item : frequentItems_)
            {
                if (!item) continue;
                const RECT bounds = item->GetBounds();
                if (EqualRect(&bounds, &base))
                {
                    launchIndex = item->GetItemIndex();
                    break;
                }
            }
        }
        if (launchIndex != static_cast<size_t>(-1))
        {
            const float pulse = app_->GetDockLaunchPulseScale(launchIndex, GetMonitor());
            const float centerX = (icon.left + icon.right) * 0.5f;
            const float centerY = (icon.top + icon.bottom) * 0.5f;
            const float offset = app_->GetDockLaunchBounceOffset(launchIndex, iconSize, GetMonitor());
            const float offsetX = Position() == DockPosition::Left
                ? offset : Position() == DockPosition::Right ? -offset : 0.0f;
            const float offsetY = Position() == DockPosition::Top
                ? offset : Position() == DockPosition::Bottom ? -offset : 0.0f;
            icon = RECT{
                static_cast<LONG>(std::floor(centerX + (icon.left - centerX) * pulse + offsetX)),
                static_cast<LONG>(std::floor(centerY + (icon.top - centerY) * pulse + offsetY)),
                static_cast<LONG>(std::ceil(centerX + (icon.right - centerX) * pulse + offsetX)),
                static_cast<LONG>(std::ceil(centerY + (icon.bottom - centerY) * pulse + offsetY))};
        }
        if (!fixed && hasOverflow)
        {
            if (!IntersectRect(&intersection, &icon, &visualViewport))
                continue;
            icon = intersection;
        }
        if (!IsRectEmpty(&icon))
            result.push_back(icon);
    }
    return result;
}

RECT DockContainer::GetVisualScrollViewport(POINT pointer) const
{
    const auto layout = GetMagnificationLayout();
    if (layout.maxScrollOffset <= 0)
        return layout.viewport;
    return GetVisualScrollViewport(pointer, layout, ResolveMagnificationFocusRect(pointer, layout));
}

RECT DockContainer::GetVisualScrollViewport(
    POINT pointer, const MagnificationLayout& layout, const RECT& focus) const
{
    RECT viewport = layout.viewport;
    if (layout.maxScrollOffset <= 0)
        return viewport;
    const RECT leadingVisual = IsRectEmpty(&layout.windowsButton)
        ? RECT{} : MagnifyElementRect(layout.windowsButton, focus, pointer, layout);
    const auto trailing = layout.ForZone(MagnificationZone::Trailing);
    const RECT trailingVisual = trailing.empty() || IsRectEmpty(&trailing.front())
        ? RECT{} : MagnifyElementRect(trailing.front(), focus, pointer, layout);
    viewport = snowdesktop::dock_magnification::FitOverflowViewportToFixedVisuals(
        viewport, Position(), leadingVisual, trailingVisual, ScaledSeparatorGap());

    // Begin the fade at the Start separator, rather than after the entire
    // separator gap. Its fixed boundary must not follow an off-screen item.
    if (!IsRectEmpty(&leadingVisual))
    {
        const int halfSeparatorGap = ScaledSeparatorGap() / 2;
        if (IsVertical())
            viewport.top = std::min(viewport.bottom,
                leadingVisual.bottom + halfSeparatorGap);
        else
            viewport.left = std::min(viewport.right,
                leadingVisual.right + halfSeparatorGap);
    }
    return viewport;
}

RECT DockContainer::GetScrollViewport(const RECT& bounds) const
{
    return GetScrollViewport(bounds, RunningSlotUnits());
}

RECT DockContainer::GetScrollViewport(const RECT& bounds, float runningUnits) const
{
    RECT viewport = bounds;
    const int halfGap = ScaledSpacing() / 2;
    const bool showWindows = app_ && ShowDockWindowsButton(app_->dockSettings_);
    const size_t frequentCount = app_
        ? app_->GetFrequentDockItemIndices(MonitorLayout()).size() : 0;
    const float fixedUnits = FixedEntryUnits() + static_cast<float>(EmptyRunningDragFixedSlots()) + AdditionalSlotUnits(false);
    const float folderUnits = FolderEntryUnits();
    const float firstUnits = fixedUnits > 0 ? fixedUnits : runningUnits > 0 ? runningUnits :
        frequentCount > 0 ? static_cast<float>(frequentCount) : folderUnits;
    const float leadingGap = fixedUnits <= 0.0f && runningUnits > 0.0f ? 1.0f :
        snowdesktop::dock_running_animation::GroupAmount(firstUnits);
    const int leadingLength = showWindows ? ItemPitch() + snowdesktop::dock_running_animation::AxisLength(
        leadingGap, ScaledSeparatorGap()) : 0;
    const float previousUnits = std::max({static_cast<float>(frequentCount), runningUnits, fixedUnits});
    const int mainGap = snowdesktop::dock_running_animation::AxisLength(
        snowdesktop::dock_entry_removal::GapUnits(previousUnits, folderUnits), ScaledSeparatorGap());
    const int trailingLength = snowdesktop::dock_running_animation::AxisLength(
        folderUnits, ItemPitch()) + mainGap;
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
    return GetMaxScrollOffset(bounds, RunningSlotUnits());
}

int DockContainer::GetMaxScrollOffset(const RECT& bounds, float runningUnits) const
{
    const size_t frequentCount = app_
        ? app_->GetFrequentDockItemIndices(MonitorLayout()).size() : 0;
    const auto axis = snowdesktop::dock_entry_removal::LayoutAxis({FixedEntryUnits() + static_cast<float>(EmptyRunningDragFixedSlots()) + AdditionalSlotUnits(false),
        runningUnits, static_cast<float>(frequentCount), 0.0f});
    const long long contentExtent = snowdesktop::dock_running_animation::AxisLength(axis.units, ItemPitch()) +
        snowdesktop::dock_running_animation::AxisLength(axis.gaps, ScaledSeparatorGap());
    const RECT viewport = GetScrollViewport(bounds, runningUnits);
    const int viewportExtent = std::max(0, IsVertical()
        ? static_cast<int>(viewport.bottom - viewport.top)
        : static_cast<int>(viewport.right - viewport.left));
    return static_cast<int>(std::clamp<long long>(
        contentExtent - viewportExtent, 0, std::numeric_limits<int>::max()));
}

RECT DockContainer::GetMinimizedThumbnailAnchor(HWND window, POINT pointer, bool restoring) const
{
    if (!app_ || !window || !ShowsRunningArea()) return {};
    const bool vertical = IsVertical();
    const int pitch = ItemPitch();
    const int spacing = ScaledSpacing();
    const int separatorGap = ScaledSeparatorGap();
    const int iconSize = ItemIconSize();
    const float cardUnits = ThumbnailSlotUnits(vertical ? 0.0f : static_cast<float>(iconSize) / pitch);
    namespace thumbnails = snowdesktop::dock_window_thumbnail;
    if (restoring)
    {
        for (const auto& slot : const_cast<DockContainer*>(this)->GetSlots())
        {
            if (!slot) continue;
            const auto* item = dynamic_cast<const DockRunningItem*>(slot->GetItem());
            if (!item || item->GetRunningIndex() >= app_->dockUnpinnedRunningApps_.size()) continue;
            const auto& app = app_->dockUnpinnedRunningApps_[item->GetRunningIndex()];
            if (!app.minimizedThumbnail || app.window != window || !app.presence.Interactive()) continue;
            const RECT base = item->GetBounds();
            const RECT visual = GetElementVisualRect(base, pointer);
            const int edge = std::max(1, static_cast<int>(std::lround(
                GetElementIconSize(base, visual) * app.presence.Amount())));
            const RECT card = thumbnails::CardAnchor(visual, edge, vertical);
            const RECT viewport = GetVisualScrollViewport(pointer);
            RECT clipped{};
            if (IntersectRect(&clipped, &card, &viewport)) return clipped;
            break;
        }
        return {};
    }

    float icons = 0.0f;
    float cards = 0.0f;
    std::optional<float> beforeCard;
    for (size_t index = 0; index < app_->dockUnpinnedRunningApps_.size(); ++index)
    {
        const auto& app = app_->dockUnpinnedRunningApps_[index];
        if (app.minimizedThumbnail && app.window == window)
        {
            beforeCard = cards;
            cards += cardUnits;
        }
        else if (app.presence.Visible())
        {
            if (app.minimizedThumbnail) cards += cardUnits;
            else icons += 1.0f + WindowTitleSlotUnits(RunningTitleGroup(index));
        }
    }
    // New cards append after existing cards. Project the settled layout
    // without inserting a task or changing the live Dock's scroll position.
    if (!beforeCard)
    {
        beforeCard = cards;
        cards += cardUnits;
    }
    const float divider = icons > 0.0f ? static_cast<float>(separatorGap) / pitch : 0.0f;
    const float runningUnits = icons + divider + cards;
    const RECT bounds = BoundsForRunningUnits(runningUnits);
    const int maxOffset = GetMaxScrollOffset(bounds, runningUnits);
    const auto axis = snowdesktop::dock_entry_removal::LayoutDockAxis(
        ShowDockWindowsButton(app_->dockSettings_),
        FixedEntryUnits() + static_cast<float>(EmptyRunningDragFixedSlots()) + AdditionalSlotUnits(false),
        runningUnits, static_cast<float>(app_->GetFrequentDockItemIndices(MonitorLayout()).size()), FolderEntryUnits());
    const RECT cell = DockAxisCell(bounds, vertical, pitch, spacing, separatorGap,
        std::clamp(scrollOffset_, 0, maxOffset), axis.groups[2], icons + divider + *beforeCard, cardUnits);
    const RECT card = thumbnails::CardAnchor(cell, iconSize, vertical);
    const RECT viewport = GetScrollViewport(bounds, runningUnits);
    RECT clipped{};
    if (IntersectRect(&clipped, &card, &viewport) &&
        clipped.right - clipped.left > 1 && clipped.bottom - clipped.top > 1) return clipped;
    if (viewport.right - viewport.left < 2 || viewport.bottom - viewport.top < 2) return {};
    // A scrolled-out card enters at the edge of the scrollable section.
    if (vertical)
    {
        const LONG edge = cell.top < viewport.top ? viewport.top : viewport.bottom - 2;
        return {card.left, edge, card.right, edge + 2};
    }
    const LONG edge = cell.left < viewport.left ? viewport.left : viewport.right - 2;
    return {edge, card.top, edge + 2, card.bottom};
}

bool DockContainer::IsPointInScrollViewport(POINT point) const
{
    return IsPointInScrollViewport(point, GetVisualScrollViewport(point));
}

bool DockContainer::IsPointInScrollViewport(POINT point, RECT viewport) const
{
    if (!IsMagnificationSuppressed())
    {
        viewport = snowdesktop::dock_magnification::
            ExpandPerpendicularBounds(viewport,
                Position(), ItemIconSize(),
                GetMaximumMagnificationScale(), MonitorLayout().hoverEffect == 1);
    }
    return PtInRect(&viewport, point) != FALSE;
}

bool DockContainer::ScrollByWheelDelta(POINT pointer, int wheelDelta)
{
    const auto layout = GetMagnificationLayout();
    const int maxOffset = layout.maxScrollOffset;
    const RECT viewport = maxOffset > 0
        ? GetVisualScrollViewport(pointer, layout, ResolveMagnificationFocusRect(pointer, layout))
        : layout.viewport;
    if (!IsPointInScrollViewport(pointer, viewport))
        return false;

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

std::vector<DockContainer::KeyboardTarget> DockContainer::KeyboardTargets(bool alongDock)
{
    using Kind = KeyboardTarget::Kind;
    std::vector<KeyboardTarget> targets;
    if (!app_ || !app_->IsDockContainerInteractionVisible(this)) return targets;
    const auto& slots = GetSlots();
    const RECT viewport = GetScrollViewport(GetBounds());
    const bool includeOverflow = alongDock && !keyboardFocusKey_.empty();
    const auto add = [&](Kind kind, size_t index, std::wstring key, RECT bounds, bool scrollable) {
        if (IsRectEmpty(&bounds)) return;
        RECT visible{};
        if (scrollable && !includeOverflow && !IntersectRect(&visible, &bounds, &viewport)) return;
        const bool focused = key == keyboardFocusKey_;
        targets.push_back({kind, index, std::move(key), bounds, focused, scrollable});
    };
    const auto addTitles = [&](RECT bounds, WindowTitleGroup group) {
        if (!group.interactive) return;
        const auto segments = WindowTitleSegments(LayoutWindowTitleGroup(bounds, bounds, group), group);
        for (size_t index = 0; index < segments.size(); ++index)
        {
            const auto handle = reinterpret_cast<std::uintptr_t>(group.windows[index]);
            add(Kind::WindowTitle, handle, L"title:" + std::to_wstring(handle),
                segments[index], true);
        }
    };
    add(Kind::Start, 0, L"start", GetWindowsButtonRect(), false);
    add(Kind::Search, 0, L"search", GetSearchRect(), false);
    const size_t fixedCount = SortableEntryCount();
    for (const auto& slot : slots)
    {
        if (!slot) continue;
        const RECT bounds = slot->GetBounds();
        if (const auto* entry = dynamic_cast<const DockEntryItem*>(slot->GetItem()))
        {
            if (IsDockLayoutEntry(entry->GetEntryType())) continue;
            const size_t index = entry->GetEntryIndex();
            const auto windows = EntryTitleGroup(index);
            add(Kind::Entry, index, L"entry:" + std::to_wstring(static_cast<int>(entry->GetEntryType())) +
                L":" + ToUpperInvariant(entry->GetReference()),
                WindowTitleIconRect(bounds, bounds, windows), index < fixedCount);
            addTitles(bounds, windows);
        }
        else if (const auto* running = dynamic_cast<const DockRunningItem*>(slot->GetItem()))
        {
            const size_t index = running->GetRunningIndex();
            if (index < app_->dockUnpinnedRunningApps_.size() &&
                app_->dockUnpinnedRunningApps_[index].presence.Interactive())
            {
                const auto windows = RunningTitleGroup(index);
                add(Kind::Running, index, L"running:" + running->GetIdentityKey(),
                    WindowTitleIconRect(bounds, bounds, windows), true);
                addTitles(bounds, windows);
            }
        }
        else if (const auto* frequent = dynamic_cast<const DockFrequentItem*>(slot->GetItem()))
        {
            const size_t index = frequent->GetItemIndex();
            if (index < app_->items_.size())
                add(Kind::Frequent, index, L"frequent:" + ToUpperInvariant(app_->items_[index].layoutKey),
                    bounds, true);
        }
    }
    return targets;
}

std::optional<DockContainer::KeyboardTarget> DockContainer::FocusedKeyboardTarget()
{
    if (keyboardFocusKey_.empty()) return std::nullopt;
    auto targets = KeyboardTargets();
    const auto focused = std::find_if(targets.begin(), targets.end(), [](const auto& target) {
        return target.focused;
    });
    if (focused == targets.end()) return std::nullopt;
    return std::move(*focused);
}

void DockContainer::SetKeyboardFocus(const KeyboardTarget& target)
{
    keyboardFocusKey_ = target.key;
    if (!target.scrollable) return;
    const RECT bounds = GetBounds();
    const RECT viewport = GetScrollViewport(bounds);
    const LONG start = IsVertical() ? target.bounds.top : target.bounds.left;
    const LONG end = IsVertical() ? target.bounds.bottom : target.bounds.right;
    const LONG viewStart = IsVertical() ? viewport.top : viewport.left;
    const LONG viewEnd = IsVertical() ? viewport.bottom : viewport.right;
    const LONG adjustment = start < viewStart ? start - viewStart : std::max(0L, end - viewEnd);
    const int offset = std::clamp(scrollOffset_ + static_cast<int>(adjustment), 0, GetMaxScrollOffset(bounds));
    if (offset == scrollOffset_) return;
    scrollOffset_ = offset;
    InvalidateSlots();
}

bool DockContainer::ClearKeyboardFocus()
{
    if (keyboardFocusKey_.empty()) return false;
    keyboardFocusKey_.clear();
    return true;
}

std::vector<std::unique_ptr<Slot>> DockContainer::BuildSlots()
{
    namespace removal = snowdesktop::dock_entry_removal;
    namespace motion = snowdesktop::dock_running_animation;
    std::vector<std::unique_ptr<Slot>> slots;
    runningItems_.clear();
    frequentItems_.clear();
    entryRemovalCells_.clear();
    auto& transitions = *presentationMotion_;
    ++transitions.layoutRevision;
    transitions.widestItemExtra = 0;
    const RECT bounds = GetBounds();
    scrollOffset_ = std::clamp(scrollOffset_, 0, GetMaxScrollOffset(bounds));
    const size_t count = ModelEntryCount();
    // Press/drag sessions can still own these wrappers while scrolling.
    entryItems_.resize(count);
    const size_t fixedCount = SortableEntryCount();
    const size_t folderCount = FolderEntryCount();
    const size_t folderBegin = FolderEntryBegin();
    const size_t runningCount = app_ && ShowsRunningArea() ? app_->dockUnpinnedRunningApps_.size() : 0;
    const auto frequentIndices = app_ ? app_->GetFrequentDockItemIndices(MonitorLayout()) : std::vector<size_t>{};
    const size_t frequentCount = frequentIndices.size();
    const bool showWindowsButton = app_ && ShowDockWindowsButton(app_->dockSettings_);
    const float fixedUnits = FixedEntryUnits() + static_cast<float>(EmptyRunningDragFixedSlots()) + AdditionalSlotUnits(false);
    const float folderUnits = FolderEntryUnits();
    const float implicitSearchUnits = HasImplicitSearchSlot() ? 1.0f : 0.0f;
    const auto axis = removal::LayoutDockAxis(showWindowsButton,
        fixedUnits, RunningSlotUnits(), static_cast<float>(frequentCount),
        folderUnits);
    const int pitch = ItemPitch();
    const int spacing = ScaledSpacing();
    const int halfGap = spacing / 2;
    const float runningIconFraction = IsVertical() ? 0.0f : static_cast<float>(pitch - spacing) / pitch;
    const auto makeCell = [&](size_t group, float begin, float amount = 1.0f) {
        return DockAxisCell(bounds, IsVertical(), pitch, spacing, ScaledSeparatorGap(),
            scrollOffset_, axis.groups[group], begin, amount);
    };
    const auto trailingCell = [&](float before, float amount = 1.0f) {
        const int start = motion::AxisLength(before, pitch);
        const int end = motion::AxisLength(before - amount, pitch);
        if (IsVertical())
            return RECT{bounds.left, bounds.bottom - halfGap - start, bounds.right, bounds.bottom - halfGap - end};
        return RECT{bounds.right - halfGap - start, bounds.top, bounds.right - halfGap - end, bounds.bottom};
    };
    std::vector<RECT> entryCells(count);
    pendingFileCells_.clear();
    const auto keys = EntryRemovalKeys();
    for (const auto group : {removal::Group::Fixed, removal::Group::Folder})
    {
        const bool folder = group == removal::Group::Folder;
        const auto cells = entryRemovals_->Layout(keys, folder ? folderBegin : 0,
            folder ? folderCount : fixedCount, group);
        std::vector<std::pair<size_t, std::shared_ptr<FolderEntry>>> incoming;
        const size_t first = folder ? folderBegin : 0;
        const size_t end = first + (folder ? folderCount : fixedCount);
        if (app_ && !app_->pendingLandingCaches_.empty())
            for (const auto& request : app_->pendingLandingCaches_)
                if (request.waitingForFiles && request.dockMonitor && request.dockMonitor == GetMonitor())
                {
                    const auto boundary = std::clamp(request.dockInsertIndex, first, end);
                    if (request.files.empty() && (request.dockSection == DockEntrySection::Files) == folder)
                        incoming.emplace_back(boundary, nullptr);
                    for (const auto& file : request.files)
                        if ((request.dockSection ? *request.dockSection == DockEntrySection::Files : file.appearance->isDirectory) == folder)
                            incoming.emplace_back(boundary, file.appearance);
                }
        std::stable_sort(incoming.begin(), incoming.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        const auto cellAt = [&](float begin, float amount) {
            return folder ? trailingCell(folderUnits - begin, amount)
                          : makeCell(1, begin, amount);
        };
        size_t inserted = 0;
        float tail = 0.0f;
        float slotReduction = 0.0f;
        for (const auto& cell : cells)
        {
            const float begin = cell.begin - slotReduction;
            float amount = cell.amount;
            if (cell.modelIndex != removal::kNoIndex)
            {
                const auto type = (*entries_)[cell.modelIndex].type;
                if (type == DockEntryType::Separator) amount = SeparatorSlotUnits();
                else if (type == DockEntryType::QuickSearch && !app_->dockSettings_.showQuickSearchButton) amount = 0.0f;
                else if (!folder) amount += WindowTitleSlotUnits(EntryTitleGroup(cell.modelIndex));
            }
            while (cell.modelIndex != removal::kNoIndex && inserted < incoming.size() && incoming[inserted].first <= cell.modelIndex)
            {
                pendingFileCells_.emplace_back(cellAt(begin + static_cast<float>(inserted), 1.f), incoming[inserted].second);
                ++inserted;
            }
            tail = begin + amount;
            slotReduction += cell.amount - amount;
            const RECT rect = cellAt(begin + static_cast<float>(inserted), amount);
            if (cell.modelIndex != removal::kNoIndex) entryCells[cell.modelIndex] = rect;
            else entryRemovalCells_.emplace_back(cell.removalIndex, rect);
        }
        for (; inserted < incoming.size(); ++inserted)
            pendingFileCells_.emplace_back(cellAt(tail + static_cast<float>(inserted), 1.f), incoming[inserted].second);
    }

    slots.reserve(count + runningCount + frequentCount + 1);
    for (size_t i = 0; i < count; ++i)
    {
        auto slot = std::make_unique<Slot>(this, entryCells[i], i);
        if (!entryItems_[i]) entryItems_[i] = std::make_unique<DockEntryItem>(app_, this, i);
        entryItems_[i]->SetBounds(entryCells[i]);
        slot->SetItem(entryItems_[i].get());
        slots.push_back(std::move(slot));
    }
    float runningBegin = 0.0f;
    float runningDivider = app_ ? RunningDividerAmount(app_->dockUnpinnedRunningApps_) * ScaledSeparatorGap() / pitch : 0.0f;
    for (size_t i = 0; i < runningCount; ++i)
    {
        const auto& running = app_->dockUnpinnedRunningApps_[i];
        if (running.minimizedThumbnail)
            runningBegin += std::exchange(runningDivider, 0.0f);
        const float amount = RunningItemSlotUnits(running, runningIconFraction) +
            WindowTitleSlotUnits(RunningTitleGroup(i)) * running.presence.Amount();
        const RECT cell = makeCell(2, runningBegin, amount);
        runningBegin += amount;
        auto slot = std::make_unique<Slot>(this, cell, count + i);
        auto item = std::make_unique<DockRunningItem>(app_, this, i);
        item->SetBounds(cell);
        slot->SetItem(item.get());
        runningItems_.push_back(std::move(item));
        slots.push_back(std::move(slot));
    }
    for (size_t i = 0; i < frequentCount; ++i)
    {
        const RECT cell = makeCell(3, static_cast<float>(i));
        auto slot = std::make_unique<Slot>(this, cell, count + runningCount + i);
        auto item = std::make_unique<DockFrequentItem>(app_, this, frequentIndices[i]);
        item->SetBounds(cell);
        slot->SetItem(item.get());
        frequentItems_.push_back(std::move(item));
        slots.push_back(std::move(slot));
    }
    // Isolated renderers can omit the layout entry. Their fallback slot
    // shares the empty insertion anchor used by the hosted Dock.
    slots.push_back(std::make_unique<Slot>(this, trailingCell(implicitSearchUnits, implicitSearchUnits),
        count + runningCount + frequentCount));
    const bool vertical = IsVertical();
    for (const auto& slot : slots)
    {
        const RECT cell = slot->GetBounds();
        const LONG extent = vertical ? cell.bottom - cell.top : cell.right - cell.left;
        transitions.widestItemExtra = std::max(transitions.widestItemExtra, extent - pitch);
    }
    transitions.primed = true;
    std::erase_if(transitions.groups, [&](const auto& entry) {
        return entry.second.layoutRevision != transitions.layoutRevision || entry.second.presence.IsHidden();
    });
    return slots;
}

size_t DockContainer::GetExternalDropInsertIndex(Slot* slot, HitRegion region) const
{
    if (app_ && app_->dockSettings_.freeItemOrder)
        return GetInsertIndexAtPoint(app_->dragSession_.CurrentPoint());
    const snowdesktop::dock_folder_rules::InsertRange range{0, SortableFileEnd(entries_, false)};
    if (!slot) return range.end;
    return snowdesktop::dock_folder_rules::SlotInsertionIndex(range, slot->GetIndex(), region == HitRegion::SortAfter);
}

size_t DockContainer::InsertIndexFor(Slot* slot, HitRegion region) const
{
    const auto point = app_ && app_->dragSession_.HasContext() ? app_->dragSession_.CurrentPoint() : app_ ? app_->lastMousePoint_ : POINT{};
    if (app_ && app_->dockSettings_.freeItemOrder) return GetInsertIndexAtPoint(point);
    const auto range = GetDragInsertRange(point);
    if (!slot) return range.end;
    return snowdesktop::dock_folder_rules::SlotInsertionIndex(
        range, slot->GetIndex(), region == HitRegion::SortAfter);
}

size_t DockContainer::GetInsertIndexAtPoint(POINT pt) const
{
    const auto range = GetDragInsertRange(pt);
    const size_t begin = range.begin;
    const size_t end = range.end;
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const bool vertical = IsVertical();
    const long axis = vertical ? pt.y : pt.x;
    for (size_t index = begin; index < end && index < slots.size(); ++index)
    {
        const auto base = slots[index]->GetBounds();
        if (IsRectEmpty(&base)) continue;
        const RECT bounds = GetElementVisualRect(base, pt);
        const long center = vertical ? (bounds.top + bounds.bottom) / 2 : (bounds.left + bounds.right) / 2;
        if (axis < center) return index;
    }
    return end;
}

RECT DockContainer::FirstVisibleFileSlot() const
{
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const auto begin = FolderEntryBegin();
    const auto end = begin + FolderEntryCount();
    for (size_t i = begin; i < end && i < slots.size(); ++i)
    {
        const RECT bounds = slots[i]->GetBounds();
        if (!IsRectEmpty(&bounds)) return bounds;
    }
    return HasImplicitSearchSlot() && !slots.empty() ? slots.back()->GetBounds() : RECT{};
}

DockEntrySection DockContainer::GetSectionAtPoint(POINT point) const
{
    const bool vertical = IsVertical();
    const long axis = vertical ? point.y : point.x;
    const RECT base = FirstVisibleFileSlot();
    if (!IsRectEmpty(&base))
    {
        const RECT visual = GetElementVisualRect(base, point);
        const auto start = vertical ? visual.top : visual.left;
        return axis >= start - ScaledSeparatorGap() / 2 ? DockEntrySection::Files : DockEntrySection::Fixed;
    }
    const RECT panel = GetVisualPanelBounds(point);
    const auto end = vertical ? panel.bottom : panel.right;
    const int reserved = snowdesktop::dock_running_animation::AxisLength(FolderEntryUnits(), ItemPitch());
    return axis >= end - ScaledSpacing() / 2 - reserved
        ? DockEntrySection::Files : DockEntrySection::Fixed;
}

std::optional<size_t> DockContainer::GetSeparatorInsertIndex(POINT point) const
{
    const RECT panel = GetVisualPanelBounds(point);
    const bool freeOrder = app_ && app_->dockSettings_.freeItemOrder;
    if (!PtInRect(&panel, point) || IsWindowsButtonPoint(point) || (!freeOrder && IsSearchPoint(point)) ||
        RunningItemAtPoint(point) || FrequentItemAtPoint(point)) return std::nullopt;

    const bool files = GetSectionAtPoint(point) == DockEntrySection::Files;
    const auto main = SortableEntryCount();
    const auto range = snowdesktop::dock_folder_rules::GroupInsertRange(
        files, main, SortableFileEnd(entries_, freeOrder) - main);
    if (const auto* hit = EntryAtPoint(point); hit &&
        (hit->GetEntryIndex() < range.begin || hit->GetEntryIndex() >= range.end || hit->GetEntryType() == DockEntryType::Separator))
        return std::nullopt;

    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const bool vertical = IsVertical();
    const long axis = vertical ? point.y : point.x;
    const auto start = [vertical](const RECT& rect) { return vertical ? rect.top : rect.left; };
    const auto end = [vertical](const RECT& rect) { return vertical ? rect.bottom : rect.right; };
    if (files && !freeOrder)
    {
        for (size_t i = range.end; i < ModelEntryCount() && i < slots.size(); ++i)
        {
            const RECT base = slots[i]->GetBounds();
            if (IsRectEmpty(&base)) continue;
            const RECT visual = GetElementVisualRect(base, point);
            if (axis >= (start(visual) + end(visual)) / 2) return std::nullopt;
            break;
        }
    }
    if (range.begin == range.end) return range.end;
    if (!files && GetMaxScrollOffset(GetBounds()) > 0 && !IsPointInScrollViewport(point)) return std::nullopt;

    long firstStart = LONG_MAX;
    long lastEnd = LONG_MIN;
    size_t boundary = range.end;
    for (size_t i = range.begin; i < range.end; ++i)
    {
        const RECT base = slots[i]->GetBounds();
        if (IsRectEmpty(&base)) continue;
        const RECT rect = GetElementVisualRect(base, point);
        firstStart = std::min(firstStart, start(rect));
        lastEnd = std::max(lastEnd, end(rect));
        const long center = (start(rect) + end(rect)) / 2;
        const int iconSize = GetElementIconSize(base, rect);
        const bool separator = (*entries_)[i].type == DockEntryType::Separator;
        const long begin = separator ? start(rect) : center - iconSize / 2;
        const long finish = separator ? end(rect) : begin + iconSize;
        if (axis >= begin && axis < finish) return std::nullopt;
        if (boundary == range.end && axis < center) boundary = i;
    }
    if (firstStart == LONG_MAX || (axis >= firstStart - ScaledSpacing() && axis < lastEnd + ScaledSpacing()))
        return boundary;
    return std::nullopt;
}

snowdesktop::dock_running_drag::Target DockContainer::GetRunningDragTarget(POINT pt) const
{
    namespace running = snowdesktop::dock_running_drag;
    const RECT bounds = GetInteractiveBounds();
    if (!app_ || !app_->IsDockContainerInteractionVisible(this) ||
        !PtInRect(&bounds, pt) || (!app_->dockSettings_.freeItemOrder && IsSearchPoint(pt)) || IsWindowsButtonPoint(pt))
        return {};
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const auto visualBounds = [&](size_t index) {
        return GetElementVisualRect(slots[index]->GetBounds(), pt);
    };
    const RECT panel = GetVisualPanelBounds(pt);
    if (!PtInRect(&panel, pt) && std::none_of(slots.begin(), slots.end(), [&](const auto& slot) {
        const RECT visual = GetElementVisualRect(slot->GetBounds(), pt);
        return PtInRect(&visual, pt) != FALSE;
    })) return {};
    const size_t count = ModelEntryCount();
    const size_t runningCount = ShowsRunningArea() ? app_->dockUnpinnedRunningApps_.size() : 0;
    if (!runningCount || count + runningCount > slots.size()) return {};
    const auto start = [&](const RECT& rect) { return IsVertical() ? rect.top : rect.left; };
    const auto end = [&](const RECT& rect) { return IsVertical() ? rect.bottom : rect.right; };
    const long axis = IsVertical() ? pt.y : pt.x;
    const size_t mainEnd = SortableEntryCount();
    const size_t filesEnd = SortableFileEnd(entries_, app_->dockSettings_.freeItemOrder);
    if (!app_->dockSettings_.freeItemOrder)
    {
        if (const auto* entry = EntryAtPoint(pt); entry && entry->GetEntryIndex() < count &&
            app_->IsRecycleBinDockEntry((*entries_)[entry->GetEntryIndex()])) return {};
    }
    const long runningBegin = start(visualBounds(count));
    const long lastRunningEnd = end(visualBounds(count + runningCount - 1));
    const RECT firstFile = FirstVisibleFileSlot();
    const long filesBegin = !IsRectEmpty(&firstFile)
        ? std::max(lastRunningEnd, start(GetElementVisualRect(firstFile, pt)) - ScaledSeparatorGap() / 2)
        : std::max(lastRunningEnd, start(visualBounds(slots.size() - 1)) - ScaledSeparatorGap() -
            static_cast<long>(EmptyRunningDragFileSlots()) * ItemPitch());
    running::Target result{ running::AreaAt(axis, runningBegin, filesBegin), 0, 0 };
    if (app_->dockSettings_.freeItemOrder && result.area != running::Area::Running)
    {
        result.section = GetSectionAtPoint(pt);
        result.area = *result.section == DockEntrySection::Files ? running::Area::Files : running::Area::Fixed;
    }
    size_t begin = 0;
    size_t limit = mainEnd;
    if (result.area == running::Area::Running)
    {
        // Frequent icons remain generated launchers; they cannot consume a
        // running application and must not become part of its sort order.
        if (!IsPointInScrollViewport(pt)) return {};
        begin = count;
        const auto& apps = app_->dockUnpinnedRunningApps_;
        const auto firstThumbnail = std::find_if(apps.begin(), apps.end(),
            [](const auto& app) { return app.minimizedThumbnail != nullptr; });
        limit = count + static_cast<size_t>(firstThumbnail - apps.begin());
    }
    else if (result.area == running::Area::Files)
    {
        begin = mainEnd;
        limit = filesEnd;
    }
    else if (mainEnd && !IsPointInScrollViewport(pt)) return {};
    const size_t boundary = snowdesktop::dock_drop_rules::ResolveRedirectedInsertionIndex(
        axis, begin, limit, [&](size_t i) {
            const RECT rect = visualBounds(i);
            return (start(rect) + end(rect)) / 2;
        });
    result.index = result.area == running::Area::Running ? boundary - count : boundary;
    if (boundary < limit) result.axis = start(visualBounds(boundary));
    else if (limit > begin) result.axis = end(visualBounds(limit - 1));
    else result.axis = result.area == running::Area::Files ? filesBegin : runningBegin;
    if (result.section) result.area = running::Area::Fixed;
    return result;
}

HitRegion DockContainer::RedirectDragToNearestInsertion(
    POINT point, Slot*& outSlot)
{
    outSlot = nullptr;
    const auto range = GetDragInsertRange(point);
    const size_t insertIndex = GetInsertIndexAtPoint(point);
    const auto& slots = GetSlots();

    if (insertIndex < range.end &&
        insertIndex < slots.size() && slots[insertIndex])
    {
        outSlot = slots[insertIndex].get();
        return HitRegion::SortBefore;
    }
    if (range.end > range.begin &&
        range.end - 1 < slots.size() && slots[range.end - 1])
    {
        outSlot = slots[range.end - 1].get();
        return HitRegion::SortAfter;
    }

    // The trailing synthetic slot gives an empty sortable
    // group a stable presentation anchor while InsertIndexFor clamps the drop
    // back to that group's sole boundary.
    if (!slots.empty() && slots.back())
    {
        outSlot = slots.back().get();
        return HitRegion::SortAfter;
    }
    return HitRegion::Empty;
}

void DockContainer::DrawInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex) const
{
    if (!context || !app_) return;
    if (app_ && app_->dockRunningDrag_)
    {
        namespace running = snowdesktop::dock_running_drag;
        const POINT pointer = app_->dragSession_.CurrentPoint();
        const auto target = GetRunningDragTarget(pointer);
        if (target.area == running::Area::None) return;
        const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
        const size_t count = ModelEntryCount();
        const size_t main = SortableEntryCount();
        const size_t runningEnd = count + (ShowsRunningArea() ? app_->dockUnpinnedRunningApps_.size() : 0);
        const bool files = target.section == DockEntrySection::Files || target.area == running::Area::Files;
        const size_t begin = target.area == running::Area::Running ? count :
            files ? main : 0;
        const size_t end = target.area == running::Area::Running ? runningEnd :
            files ? SortableFileEnd(entries_, app_->dockSettings_.freeItemOrder) : main;
        const size_t boundary = target.area == running::Area::Running ? count + target.index : target.index;
        const size_t anchor = boundary < end ? boundary : end > begin ? end - 1 :
            files && runningEnd > count ? runningEnd - 1 : count;
        RECT base = anchor < slots.size() ? slots[anchor]->GetBounds() : GetBounds();
        RECT visual = GetElementVisualRect(base, pointer);
        if (boundary > begin && boundary - 1 < slots.size())
        {
            const RECT previousBase = slots[boundary - 1]->GetBounds();
            const RECT previousVisual = GetElementVisualRect(previousBase, pointer);
            if (DockPreviewScale(previousBase, previousVisual, ItemIconSize()) > DockPreviewScale(base, visual, ItemIconSize()))
            { base = previousBase; visual = previousVisual; }
        }
        DrawDockInsertionIndicator(context, base, visual,
            static_cast<float>(target.axis), IsVertical(), ItemIconSize());
        return;
    }
    if (HasMixedFolderDragSource())
    {
        const auto main = SortableEntryCount();
        const auto end = SortableFileEnd(entries_, false);
        DrawPartitionInsertionPreview(context, std::min(insertIndex, main), {0, main}, false);
        DrawPartitionInsertionPreview(context, std::clamp(insertIndex, main, end), {main, end}, true);
        return;
    }
    const POINT pointer = app_->dragSession_.HasContext() ? app_->dragSession_.CurrentPoint() : app_->lastMousePoint_;
    const auto range = GetDragInsertRange(pointer);
    const bool files = app_->dockSettings_.freeItemOrder ?
        range.begin >= FolderEntryBegin() && (range.end > SortableEntryCount() || GetSectionAtPoint(pointer) == DockEntrySection::Files) :
        HasOnlyFolderDragSource();
    DrawPartitionInsertionPreview(context, insertIndex, range, files);
}

void DockContainer::DrawPartitionInsertionPreview(ID2D1DeviceContext* context, size_t insertIndex,
    snowdesktop::dock_folder_rules::InsertRange range, bool folderSource) const
{
    if (!context) return;
    const size_t begin = range.begin;
    const size_t end = range.end;
    insertIndex = std::clamp(insertIndex, begin, end);
    RECT bounds = GetBounds();
    // The insertion line follows the same animated slot edges as hit testing.
    // Fixed pitch arithmetic would leave it behind the magnified icons.
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const auto& session = app_->dragSession_;
    const POINT pointer = session.HasContext() ? session.CurrentPoint() : app_->lastMousePoint_;
    RECT boundary{};
    bool after = false;
    for (size_t i = insertIndex; i < end && i < slots.size() && IsRectEmpty(&boundary); ++i)
        boundary = slots[i]->GetBounds();
    for (size_t i = std::min(insertIndex, slots.size()); i > begin && IsRectEmpty(&boundary);)
    {
        boundary = slots[--i]->GetBounds();
        after = true;
    }
    if (IsRectEmpty(&boundary) && folderSource)
    {
        const RECT firstFile = FirstVisibleFileSlot();
        if (!IsRectEmpty(&firstFile))
        {
            const RECT visual = GetElementVisualRect(firstFile, pointer);
            DrawDockInsertionIndicator(context, firstFile, visual,
                static_cast<float>(IsVertical() ? visual.top : visual.left), IsVertical(), ItemIconSize());
            return;
        }
        const size_t count = ModelEntryCount();
        if (IsEdgeAttached() || GetMaxScrollOffset(bounds) > 0)
        {
            // An empty attached file lane remains at the opposite end even
            // when every trailing control is hidden. Do not borrow the last
            // main/running item, which may be on the far side of the screen.
            const float axis = static_cast<float>((IsVertical() ? bounds.bottom : bounds.right) - ScaledSpacing() / 2);
            DrawDockInsertionIndicator(context, bounds, bounds, axis, IsVertical(), ItemIconSize());
            return;
        }
        const size_t generated = runningItems_.size() + frequentItems_.size();
        if (generated > 0 && count + generated <= slots.size())
            boundary = slots[count + generated - 1]->GetBounds();
        else if (begin > 0 && begin <= slots.size())
            boundary = slots[begin - 1]->GetBounds();
        else
        {
            const RECT start = GetWindowsButtonRect();
            const RECT base = IsRectEmpty(&start) ? bounds : start;
            const RECT visual = IsRectEmpty(&start) ? base : GetElementVisualRect(start, pointer);
            const float axis = static_cast<float>(IsRectEmpty(&start)
                ? (IsVertical() ? bounds.bottom : bounds.right) - ScaledSpacing() / 2
                : (IsVertical() ? visual.bottom : visual.right));
            DrawDockInsertionIndicator(context, base, visual, axis, IsVertical(), ItemIconSize());
            return;
        }
        after = true;
    }
    else if (IsRectEmpty(&boundary))
    {
        // An empty fixed group belongs before generated running/frequent
        // entries, not at the trailing Search control. Use that leading
        // neighbour's real animated edge, just as a nonempty group does.
        const size_t count = ModelEntryCount();
        const size_t generated = runningItems_.size() + frequentItems_.size();
        if (generated > 0 && count < slots.size() && slots[count])
            boundary = slots[count]->GetBounds();
        else if (!IsEdgeAttached())
        {
            boundary = FirstVisibleFileSlot();
            if (IsRectEmpty(&boundary))
            {
                const RECT start = GetWindowsButtonRect();
                if (!IsRectEmpty(&start)) { boundary = start; after = true; }
                const RECT base = IsRectEmpty(&boundary) ? bounds : boundary;
                const RECT visual = GetElementVisualRect(base, pointer);
                const RECT edge = IsRectEmpty(&boundary) ? boundary : visual;
                const float axis = static_cast<float>(IsVertical()
                    ? (after ? edge.bottom : edge.top) : (after ? edge.right : edge.left));
                DrawDockInsertionIndicator(context, base, visual, axis, IsVertical(), ItemIconSize());
                return;
            }
        }
        else
        {
            // Edge-attached file entries remain at the opposite end. With
            // no leading neighbour, retain the fixed section's own viewport
            // boundary rather than borrowing those trailing controls.
            const RECT viewport = GetVisualScrollViewport(pointer);
            RECT base = GetWindowsButtonRect();
            const bool hasStart = !IsRectEmpty(&base);
            if (!hasStart) base = bounds;
            const RECT visual = hasStart ? GetElementVisualRect(base, pointer) : GetVisualPanelBounds(pointer);
            const LONG startShift = hasStart && GetMaxScrollOffset(bounds) <= 0
                ? (IsVertical() ? visual.bottom - base.bottom : visual.right - base.right) : 0;
            const float axis = static_cast<float>((IsVertical() ? viewport.top : viewport.left) + startShift);
            DrawDockInsertionIndicator(context, base, visual,
                axis, IsVertical(), ItemIconSize());
            return;
        }
    }
    RECT base = IsRectEmpty(&boundary) ? bounds : boundary;
    RECT visual = GetElementVisualRect(base, pointer);
    if (!IsRectEmpty(&boundary)) boundary = visual;
    float axis = static_cast<float>(IsVertical()
        ? (after ? boundary.bottom : boundary.top)
        : (after ? boundary.right : boundary.left));
    const auto* dragged = session.IsActive() ? DraggedSeparator(session.SourceList()) : nullptr;
    const auto dividerMove = entries_ ? PreviewSeparatorMove(dragged, *entries_, range, insertIndex, ScaledSeparatorGap()) : std::nullopt;
    const bool vertical = IsVertical();
    struct Neighbor
    {
        RECT base, visual;
        float leading, trailing;
    };
    const auto neighborAt = [&](size_t index) -> std::optional<Neighbor> {
        RECT cell = slots[index]->GetBounds();
        if (IsRectEmpty(&cell)) return std::nullopt;
        const auto& entry = (*entries_)[index];
        const bool divider = entry.type == DockEntryType::Separator;
        if (divider && dragged && entry.reference == dragged->dockReference) return std::nullopt;
        RECT painted = GetElementVisualRect(cell, pointer);
        if (dividerMove) dividerMove->Shift(painted, index, vertical);
        const float center = (vertical ? painted.top + painted.bottom : painted.left + painted.right) * 0.5f;
        const float halfWidth = divider ? 1.0f : GetElementIconSize(cell, painted) * 0.5f;
        return Neighbor{cell, painted, center - halfWidth, center + halfWidth};
    };
    const bool validRange = entries_ && end <= entries_->size() && end <= slots.size();
    const bool dividerBoundary = validRange && (dragged ||
        (insertIndex > begin && (*entries_)[insertIndex - 1].type == DockEntryType::Separator) ||
        (insertIndex < end && (*entries_)[insertIndex].type == DockEntryType::Separator));
    std::optional<Neighbor> preceding, following;
    if (dividerBoundary)
    {
        for (size_t index = insertIndex; index > begin && !preceding;)
            preceding = neighborAt(--index);
        for (size_t index = insertIndex; index < end && !following; ++index)
            following = neighborAt(index);
    }
    if (dividerBoundary)
    {
        const float padding = (ScaledSpacing() + (dividerMove ? dividerMove->width : 0)) * 0.5f;
        if (preceding && following) axis = (preceding->trailing + following->leading) * 0.5f;
        else if (preceding) axis = preceding->trailing + padding;
        else if (following) axis = following->leading - padding;
        else if (dividerMove)
        {
            const auto sourceRect = GetElementVisualRect(slots[dividerMove->source]->GetBounds(), pointer);
            axis = (vertical ? sourceRect.top + sourceRect.bottom : sourceRect.left + sourceRect.right) * 0.5f;
        }
    }
    // Match DrawContents: animated cells may extend beyond their base lane
    // while it fits. Only an actually scrolling lane owns a viewport clip;
    // file items are fixed outside that clip in either layout.
    if (GetMaxScrollOffset(bounds) > 0 && !folderSource)
    {
        const RECT viewport = GetVisualScrollViewport(pointer);
        axis = IsVertical()
            ? std::clamp(axis, static_cast<float>(viewport.top), static_cast<float>(viewport.bottom))
            : std::clamp(axis, static_cast<float>(viewport.left), static_cast<float>(viewport.right));
    }
    // Either adjacent icon can own the hover; use the larger growth budget so
    // a line after the focused icon also extends in single-icon hover mode.
    if (following) { base = following->base; visual = following->visual; }
    if (preceding && DockPreviewScale(preceding->base, preceding->visual, ItemIconSize()) >
            DockPreviewScale(base, visual, ItemIconSize()))
    {
        base = preceding->base;
        visual = preceding->visual;
    }
    else if (!dividerBoundary && insertIndex > begin && insertIndex - 1 < slots.size())
    {
        const RECT previousBase = slots[insertIndex - 1]->GetBounds();
        const RECT previousVisual = GetElementVisualRect(previousBase, pointer);
        if (DockPreviewScale(previousBase, previousVisual, ItemIconSize()) > DockPreviewScale(base, visual, ItemIconSize()))
        {
            base = previousBase;
            visual = previousVisual;
        }
    }
    DrawDockInsertionIndicator(context, base, visual, axis, IsVertical(), ItemIconSize());
}

DropPreviewList DockContainer::FilePinPreview(const DragSourceList& sources, size_t insertIndex, POINT point) const
{
    DropPreviewList preview;
    if (!app_ || sources.Empty() || !std::all_of(sources.entries.begin(), sources.entries.end(),
            [](const auto& entry) { return dynamic_cast<FolderEntryIcon*>(entry.item) != nullptr; })) return preview;
    preview = app_->BuildDropPreviewList(sources, app_->GetDesktopGrid(), nullptr, HitRegion::Empty, MK_ALT, point);
    preview.action = DropAction::Link;
    preview.pinMaterializedItemsToDock = true;
    preview.dockMonitor = GetMonitor();
    preview.dockInsertIndex = insertIndex;
    if (app_->dockSettings_.freeItemOrder) preview.dockSection = GetSectionAtPoint(point);
    return preview;
}

DropReceipt DockContainer::OnItemsDropped(const ItemDrop& drop)
{
    if (!app_ || drop.region == HitRegion::Blocked) return {};
    if (drop.preview.pinMaterializedItemsToDock)
    {
        if (!drop.Allows(DropAction::Link)) return {};
        if (!HasCapacity(drop.items.size())) { MessageBeep(MB_ICONWARNING); return {}; }
        return DropReceipt::Submitted(app_->ExecuteDropPipeline(drop.sources, drop.preview), DropAction::Link);
    }
    return app_->CommitDockDrop(drop.items, drop.sources.origin, this,
        drop.preview.dockInsertIndex, drop.modifiers, drop.allowedEffects, drop.preview.dockSection,
        drop.sources.dropSelectionRevision);
}

void DockContainer::DrawChrome(ID2D1DeviceContext* context, POINT mousePt)
{
    if (!context || SharesStatusBarAppearance()) return;
    RECT bounds = GetVisualPanelBounds(mousePt);
    const auto p = app_ ? app_->CurrentDockAppearance(GetMonitor()) : PersonalizationSettings::DarkPreset();
    const float panelRadius = IsEdgeAttached() ? 0.0f : p.cornerRadius;
    const D2D1_COLOR_F fill = D2D1::ColorF(
        p.widgetBgR, p.widgetBgG, p.widgetBgB, p.widgetAlpha);
    const D2D1_COLOR_F border = D2D1::ColorF(
        p.widgetBorderR, p.widgetBorderG, p.widgetBorderB, p.widgetBorderAlpha);
    const float borderWidth = std::clamp(p.widgetBorderWidth,
        kMinimumWidgetBorderWidth, kMaximumWidgetBorderWidth);
    if (app_)
    {
        PersonalizationSettings fillSettings = p;
        fillSettings.widgetEdgeHighlightEnabled = false;
        app_->DrawWidgetPanelBackground(context, bounds, panelRadius, fill,
            D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f), false, borderWidth,
            &fillSettings, true, reinterpret_cast<std::uintptr_t>(this));
    }

    if (border.a > 0.0f)
    {
        ComPtr<ID2D1SolidColorBrush> borderBrush;
        if (SUCCEEDED(context->CreateSolidColorBrush(border, &borderBrush)) && borderBrush)
        {
            if (IsEdgeAttached())
            {
                D2D1_POINT_2F start{};
                D2D1_POINT_2F end{};
                switch (Position())
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
                context->DrawLine(start, end, borderBrush.Get(), borderWidth);
            }
            else
            {
                const float inset = borderWidth * 0.5f;
                const float borderRadius = std::max(0.0f, panelRadius - inset);
                snowdesktop::smooth_corners::Stroke(context,
                    D2D1::RectF(static_cast<float>(bounds.left) + inset,
                        static_cast<float>(bounds.top) + inset,
                        static_cast<float>(bounds.right) - inset,
                        static_cast<float>(bounds.bottom) - inset),
                    borderRadius, borderBrush.Get(), borderWidth);
            }
        }
    }
    // The reflection belongs to the panel material. Paint it before contents
    // so magnified icons can cross the panel boundary without a rim over them.
    // Edge-attached Docks retain only their flat single boundary.
    if (app_ && !IsEdgeAttached())
        (void)app_->DrawWidgetPanelEdgeHighlight(
            context, bounds, panelRadius, fill, &p);
}

void DockContainer::DrawContents(ID2D1DeviceContext* context)
{
    if (titleMarqueeWindow_ && (!app_ || WindowTitleAtPoint(app_->lastMousePoint_) != titleMarqueeWindow_))
    {
        titleMarqueeWindow_ = nullptr;
        titleMarqueeAnimating_ = false;
    }
    if (app_ && !windowTitleLayouts_.empty() && !IsTitleTransitioning())
        std::erase_if(windowTitleLayouts_, [this](const auto& entry) {
            return !app_->dockTaskWindowTitles_.contains(entry.first);
        });
    if (!context || !app_) return;
    snowdesktop::SystemIconPlaceholderDemand::Paint placeholderPaint(app_->systemIconDemand_,
        snowdesktop::SystemIconPlaceholderDemand::Host::Dock, this);
    const auto& slots = GetSlots();
    const size_t fixedCount = SortableEntryCount();
    const size_t folderBegin = FolderEntryBegin();
    const size_t folderCount = FolderEntryCount();
    const size_t folderEnd = folderBegin + folderCount;
    const auto& session = app_->dragSession_;
    const auto* dragged = session.IsActive() && session.TargetContainer() == this
        ? DraggedSeparator(session.SourceList()) : nullptr;
    std::optional<DockSeparatorMove> separatorMove;
    if (dragged && entries_)
    {
        const auto point = session.CurrentPoint();
        separatorMove = PreviewSeparatorMove(dragged, *entries_, GetDragInsertRange(point),
            GetDropInsertIndex(session.TargetSlot(), session.TargetRegion()), ScaledSeparatorGap());
    }
    const auto monitor = GetMonitor();
    const bool lt = app_->CurrentDockAppearance(monitor).contentTheme == 1;
    const auto layout = GetMagnificationLayout();
    const POINT pointer = app_->lastMousePoint_;
    const RECT magnificationFocus = ResolveMagnificationFocusRect(pointer, layout);
    auto isFocused = [&](const RECT& rect) {
        return !IsRectEmpty(&magnificationFocus) &&
            EqualRect(&rect, &magnificationFocus) != FALSE;
    };
    auto visualRectFor = [&](const RECT& rect) {
        return MagnifyElementRect(rect, magnificationFocus, pointer, layout);
    };

    const RECT windowsButton = layout.windowsButton;
    if (!IsRectEmpty(&windowsButton))
    {
        const RECT windowsVisual = visualRectFor(windowsButton);
        const int backgroundSize = GetElementIconSize(windowsButton, windowsVisual);
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
        auto* custom = app_->GetSmallCustomIcon(context, L"control:start", backgroundSize);
        const float iconScale = app_->GetItemLayoutScale(background);
        const auto drawIcon = [&](ID2D1DeviceContext* drawing) {
            if (custom)
            {
                app_->DrawIconBitmap(drawing, custom, background);
                return;
            }
            const bool withBorder = app_->dockSettings_.startIconBorder ||
                app_->iconOverrides_.contains(L"control:start") ||
                app_->ResolveIconAppearance(L"control:start").enabled;
            if (withBorder)
                app_->DrawDockControlBackground(drawing, background, 0, !lt, 0, monitor);
            const float logoSize = std::max(1.0f, backgroundSize * (withBorder ? 0.58f : 1.0f));
            const float paneGap = std::max(1.5f, logoSize * 0.09f);
            const float paneSize = (logoSize - paneGap) * 0.5f;
            const float logoLeft = (background.left + background.right - logoSize) * 0.5f;
            const float logoTop = (background.top + background.bottom - logoSize) * 0.5f;
            const auto drawSubject = [&](ID2D1DeviceContext* subject) {
                ComPtr<ID2D1SolidColorBrush> windowsBrush;
                if (FAILED(subject->CreateSolidColorBrush(
                        D2D1::ColorF(0.0f, 0.47f, 0.84f, 0.94f), &windowsBrush)))
                    return;
                subject->FillRectangle(D2D1::RectF(logoLeft, logoTop,
                    logoLeft + paneSize, logoTop + paneSize), windowsBrush.Get());
                subject->FillRectangle(D2D1::RectF(logoLeft + paneSize + paneGap, logoTop,
                    logoLeft + logoSize, logoTop + paneSize), windowsBrush.Get());
                subject->FillRectangle(D2D1::RectF(logoLeft, logoTop + paneSize + paneGap,
                    logoLeft + paneSize, logoTop + logoSize), windowsBrush.Get());
                subject->FillRectangle(D2D1::RectF(
                    logoLeft + paneSize + paneGap, logoTop + paneSize + paneGap,
                    logoLeft + logoSize, logoTop + logoSize), windowsBrush.Get());
            };
            app_->DrawDockControlSubjectShadow(drawing, background, iconScale, drawSubject);
            drawSubject(drawing);
        };
        app_->DrawIconWithShadows(context, background, iconScale,
            app_->iconShadows_.DockStrength(), drawIcon);
    }

    const RECT search = GetSearchRect();
    const int maxScrollOffset = layout.maxScrollOffset;
    const bool hasOverflow = maxScrollOffset > 0;
    // Keep the image in the Dock surface when its scroll opacity mask applies.
    if (auto* host = app_->renderingPersistentDockHost_; host && host->container == this)
        host->drawLiveThumbnails = !hasOverflow;
    const RECT scrollViewport = GetVisualScrollViewport(pointer, layout, magnificationFocus);
    RECT scrollVisualViewport = snowdesktop::dock_magnification::
        ExpandPerpendicularBounds(scrollViewport, Position(),
            ItemIconSize(), GetMaximumMagnificationScale(), MonitorLayout().hoverEffect == 1);
    if (const int launchPadding = GetLaunchAnimationPadding(); launchPadding > 0)
        scrollVisualViewport = snowdesktop::dock_magnification::ExpandPerpendicularBounds(
            scrollVisualViewport, Position(), launchPadding, 2.0f);
    const D2D1_RECT_F scrollVisualViewportF = D2D1::RectF(
        static_cast<float>(scrollVisualViewport.left),
        static_cast<float>(scrollVisualViewport.top),
        static_cast<float>(scrollVisualViewport.right),
        static_cast<float>(scrollVisualViewport.bottom));
    bool scrollClipPushed = false;
    std::optional<snowdesktop::ScopedIconBackdropClip> scrollGlassClip;
    if (hasOverflow)
    {
        context->PushAxisAlignedClip(
            scrollVisualViewportF,
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        scrollClipPushed = true;
        scrollGlassClip.emplace(context, scrollVisualViewport);
    }

    bool hasLeadingOverflow =
        hasOverflow && scrollOffset_ > 0;
    bool hasTrailingOverflow =
        hasOverflow && scrollOffset_ < maxScrollOffset;
    if (hasOverflow && (!hasLeadingOverflow || !hasTrailingOverflow))
    {
        const bool vertical = IsVertical();
        const auto begin = [vertical](const RECT& rect) { return vertical ? rect.top : rect.left; };
        const auto end = [vertical](const RECT& rect) { return vertical ? rect.bottom : rect.right; };
        const RECT* first = nullptr;
        const RECT* last = nullptr;
        for (const RECT& candidate : layout.ForZone(MagnificationZone::Leading))
        {
            if (EqualRect(&candidate, &windowsButton)) continue;
            if (!first || begin(candidate) < begin(*first)) first = &candidate;
            if (!last || end(candidate) > end(*last)) last = &candidate;
        }
        // The fixed controls can reduce the painted viewport during hover,
        // even at a logical scroll endpoint.
        if (!hasLeadingOverflow && first)
            hasLeadingOverflow = begin(visualRectFor(*first)) < begin(scrollViewport);
        if (!hasTrailingOverflow && last)
            hasTrailingOverflow = end(visualRectFor(*last)) > end(scrollViewport);
    }
    ComPtr<ID2D1LinearGradientBrush> overflowMask;
    std::optional<snowdesktop::ScopedContentOpacityLayer> overflowMaskLayer;
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
                    overflowMaskLayer.emplace(
                        context, scrollVisualViewportF, 1.0f, overflowMask.Get());
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
        RECT visualBounds = visualRectFor(itemBounds);
        if (separatorMove)
            if (const auto* entry = dynamic_cast<const DockEntryItem*>(item))
                separatorMove->Shift(visualBounds, entry->GetEntryIndex(), IsVertical());
        item->Draw(context, visualBounds, item->IsSelected() ? 2 : 0);
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
    };
    drawScrollablePass(false);
    drawScrollablePass(true);
    DrawEntryRemovals(context, false);

    auto drawSeparatorBetween = [&](
        const RECT& precedingBounds,
        const RECT& followingBounds, float opacity,
        bool anchorToFollowing, bool scrollable) {
        if (IsRectEmpty(&precedingBounds) ||
            IsRectEmpty(&followingBounds))
            return;
        const RECT precedingVisual =
            visualRectFor(precedingBounds);
        RECT followingVisual = visualRectFor(followingBounds);
        if (!anchorToFollowing && hasOverflow &&
            EqualRect(&precedingBounds, &windowsButton))
        {
            if (IsVertical())
                followingVisual.top = precedingVisual.bottom + ScaledSeparatorGap();
            else
                followingVisual.left = precedingVisual.right + ScaledSeparatorGap();
        }
        const float precedingEnd = static_cast<float>(
            IsVertical() ? precedingVisual.bottom : precedingVisual.right);
        const float followingStart = static_cast<float>(
            IsVertical() ? followingVisual.top : followingVisual.left);
        // The file boundary follows the file group's hover geometry while
        // remaining independent of application scrolling.
        const float position = anchorToFollowing
            ? followingStart - static_cast<float>(ScaledSeparatorGap()) * 0.5f
            : (precedingEnd + followingStart) * 0.5f;
        if ((hasOverflow || IsEdgeAttached()) && scrollable)
        {
            const float begin = static_cast<float>(IsVertical() ? scrollViewport.top : scrollViewport.left);
            const float end = static_cast<float>(IsVertical() ? scrollViewport.bottom : scrollViewport.right);
            if (position < begin || position >= end) return;
        }
        const bool vertical = IsVertical();
        DrawDockSeparator(context, vertical ? (followingBounds.left + followingBounds.right) * 0.5f : position,
            vertical ? position : (followingBounds.top + followingBounds.bottom) * 0.5f, vertical, lt, opacity);
    };

    struct Boundary
    {
        RECT first{}, last{};
        float units = 0.0f;
    };
    std::array<Boundary, 5> groups{};
    const auto include = [&](Boundary& group, RECT rect) {
        if (IsRectEmpty(&rect)) return;
        if (IsRectEmpty(&group.first) || (IsVertical() ? rect.top < group.first.top : rect.left < group.first.left))
            group.first = rect;
        if (IsRectEmpty(&group.last) || (IsVertical() ? rect.bottom > group.last.bottom : rect.right > group.last.right))
            group.last = rect;
    };
    include(groups[0], windowsButton);
    groups[0].units = IsRectEmpty(&windowsButton) ? 0.0f : 1.0f;
    groups[1].units = FixedEntryUnits() + AdditionalSlotUnits(false);
    groups[2].units = RunningSlotUnits();
    groups[3].units = static_cast<float>(frequentItems_.size());
    groups[4].units = FolderEntryUnits();
    for (size_t i = 0; i < fixedCount && i < slots.size(); ++i)
        if (slots[i]) include(groups[1], slots[i]->GetBounds());
    Boundary runningIcons, minimizedThumbnails;
    for (const auto& item : runningItems_)
    {
        if (!item) continue;
        const RECT bounds = item->GetBounds();
        include(groups[2], bounds);
        const auto index = item->GetRunningIndex();
        if (index >= app_->dockUnpinnedRunningApps_.size()) continue;
        include(app_->dockUnpinnedRunningApps_[index].minimizedThumbnail
            ? minimizedThumbnails : runningIcons, bounds);
    }
    for (const auto& item : frequentItems_) if (item) include(groups[3], item->GetBounds());
    for (size_t i = folderBegin; i < folderEnd && i < slots.size(); ++i)
        if (slots[i]) include(groups[4], slots[i]->GetBounds());
    if (HasImplicitSearchSlot()) include(groups[4], search);
    const auto& removals = entryRemovals_->Entries();
    for (const auto& [index, rect] : entryRemovalCells_)
        if (index < removals.size())
            include(groups[removals[index].group == snowdesktop::dock_entry_removal::Group::Folder ? 4 : 1], rect);
    const auto drawBoundaries = [&](bool clipped) {
        size_t previous = groups.size();
        float precedingUnits = 0.0f;
        for (size_t i = 0; i < groups.size(); ++i)
        {
            if (IsRectEmpty(&groups[i].first)) continue;
            if (previous != groups.size())
            {
                const bool withinScroll = previous != 0 && i != 4;
                if (withinScroll == clipped)
                    drawSeparatorBetween(groups[previous].last, groups[i].first,
                        previous == 0 ? (i == 2 ? 1.0f : snowdesktop::dock_running_animation::GroupAmount(groups[i].units)) :
                        snowdesktop::dock_entry_removal::GapUnits(precedingUnits, groups[i].units),
                        i == 4, withinScroll);
            }
            previous = i;
            if (i != 0) precedingUnits = std::max(precedingUnits, groups[i].units);
        }
    };
    const float runningDivider = ShowsRunningArea() ? RunningDividerAmount(app_->dockUnpinnedRunningApps_) : 0.f;
    if (runningDivider > 0.0f)
        drawSeparatorBetween(runningIcons.last, minimizedThumbnails.first, runningDivider, false, true);
    drawBoundaries(true);
    overflowMaskLayer.reset();
    if (scrollClipPushed) context->PopAxisAlignedClip();
    scrollGlassClip.reset();

    {
        auto drawFixedFolderPass = [&](bool focusedPass) {
            for (size_t i = folderBegin; i < folderEnd && i < slots.size(); ++i)
            {
                if (!slots[i]) continue;
                Item* item = slots[i]->GetItem();
                if (!item) continue;
                const RECT itemBounds = slots[i]->GetBounds();
                const bool hovered = isFocused(itemBounds);
                if (hovered != focusedPass) continue;
                RECT visual = visualRectFor(itemBounds);
                if (separatorMove) separatorMove->Shift(visual, i, IsVertical());
                item->Draw(context, visual, item->IsSelected() ? 2 : 0);
            }
        };
        drawFixedFolderPass(false);
        drawFixedFolderPass(true);
        DrawEntryRemovals(context, true);
    }
    drawBoundaries(false);

    if (!IsRectEmpty(&search))
    {
        RECT searchVisual = visualRectFor(search);
        if (separatorMove) separatorMove->Shift(searchVisual, quickSearchEntryIndex_, IsVertical());
        const int backgroundSize = GetElementIconSize(search, searchVisual);
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
        auto* custom = app_->GetSmallCustomIcon(context, L"control:search", backgroundSize);
        const float iconScale = app_->GetItemLayoutScale(searchBackground);
        const auto drawIcon = [&](ID2D1DeviceContext* drawing) {
            if (custom)
            {
                app_->DrawIconBitmap(drawing, custom, searchBackground);
                return;
            }
            const bool withBorder = app_->dockSettings_.quickSearchIconBorder ||
                app_->iconOverrides_.contains(L"control:search") ||
                app_->ResolveIconAppearance(L"control:search").enabled;
            if (withBorder)
                app_->DrawDockControlBackground(drawing, searchBackground, 0, !lt, 0, monitor);
            constexpr float ringRadius = 9.2f;
            constexpr float handleEnd = 13.5f;
            constexpr float stroke = 2.35f;
            const float searchScale = static_cast<float>(backgroundSize) /
                (withBorder ? 52.0f : ringRadius + handleEnd + stroke);
            const float centerOffset = (withBorder ? 2.7f : (handleEnd - ringRadius) * 0.5f) * searchScale;
            const auto drawSubject = [&](ID2D1DeviceContext* subject) {
                ComPtr<ID2D1SolidColorBrush> brush;
                if (FAILED(subject->CreateSolidColorBrush(
                        lt ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.72f)
                           : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f), &brush)))
                    return;
                ComPtr<ID2D1Factory> factory;
                ComPtr<ID2D1StrokeStyle> roundedStroke;
                subject->GetFactory(&factory);
                if (factory)
                {
                    const D2D1_STROKE_STYLE_PROPERTIES properties = D2D1::StrokeStyleProperties(
                        D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                        D2D1_LINE_JOIN_ROUND, 10.0f, D2D1_DASH_STYLE_SOLID, 0.0f);
                    factory->CreateStrokeStyle(properties, nullptr, 0, &roundedStroke);
                }
                const float centerX = (searchBackground.left + searchBackground.right) / 2.0f -
                    centerOffset;
                const float centerY = (searchBackground.top + searchBackground.bottom) / 2.0f -
                    centerOffset;
                const float searchStroke = std::max(1.5f, stroke * searchScale);
                subject->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(centerX, centerY),
                    ringRadius * searchScale, ringRadius * searchScale),
                    brush.Get(), searchStroke, roundedStroke.Get());
                subject->DrawLine(D2D1::Point2F(centerX + 6.6f * searchScale,
                        centerY + 6.6f * searchScale),
                    D2D1::Point2F(centerX + handleEnd * searchScale,
                        centerY + handleEnd * searchScale),
                    brush.Get(), searchStroke, roundedStroke.Get());
            };
            app_->DrawDockControlSubjectShadow(drawing, searchBackground, iconScale, drawSubject);
            drawSubject(drawing);
        };
        app_->DrawIconWithShadows(context, searchBackground, iconScale,
            app_->iconShadows_.DockStrength(), drawIcon);
    }
    for (const auto& [cell, file] : pendingFileCells_)
        app_->DrawPendingFile(context, visualRectFor(cell), file.get(), nullptr, false, lt);
    if (const auto focused = FocusedKeyboardTarget())
    {
        RECT outline = visualRectFor(focused->bounds);
        if (focused->scrollable && hasOverflow)
            IntersectRect(&outline, &outline, &scrollVisualViewport);
        app_->DrawWidgetPanelBorder(context, outline, 6.f,
            D2D1::ColorF(0, 0.f), true, 1.6f);
    }
}

std::vector<Item*> DockContainer::GetSelectedItems() const
{
    std::vector<Item*> selected;
    for (const auto& item : entryItems_)
        if (item && item->IsSelected()) selected.push_back(item.get());
    for (const auto& item : frequentItems_)
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
    if (!app_ ||
        !app_->IsDockContainerInteractionVisible(this))
    {
        resetDwell();
        return HitRegion::None;
    }
    RECT dockBounds = GetInteractiveBounds();
    if (!PtInRect(&dockBounds, pt))
    {
        resetDwell();
        return HitRegion::None;
    }
    if (IsWindowsButtonPoint(pt))
    {
        resetDwell();
        return RedirectDragToNearestInsertion(pt, outSlot);
    }
    const bool pointInScrollViewport = IsPointInScrollViewport(pt);
    const bool freeOrder = app_->dockSettings_.freeItemOrder;
    const auto& sources = app_->dragSession_.SourceList().entries;
    const bool insertionOnly = IsDockFrequentDrag(app_->dragSession_.Items()) ||
        std::any_of(sources.begin(), sources.end(),
            [](const auto& source) { return source.fromDock && IsDockLayoutEntry(source.dockEntryType); });
    const auto& slots = GetSlots();
    for (const auto& slot : slots)
    {
        RECT bounds = GetElementVisualRect(slot->GetBounds(), pt);
        if (!PtInRect(&bounds, pt)) continue;
        Item* targetItem = slot->GetItem();
        const auto* dockEntry = dynamic_cast<DockEntryItem*>(targetItem);
        const bool recycleBinSlot = dockEntry && app_ &&
            dockEntry->GetEntryIndex() < app_->dockEntries_.size() &&
            app_->IsRecycleBinDockEntry(
                app_->dockEntries_[dockEntry->GetEntryIndex()]);
        const bool fixedTrailingSlot = !targetItem || (dockEntry && dockEntry->GetEntryIndex() >= FolderEntryBegin());
        if (!pointInScrollViewport && !fixedTrailingSlot)
            continue;
        outSlot = slot.get();
        if (!targetItem)
        {
            resetDwell();
            return RedirectDragToNearestInsertion(pt, outSlot);
        }
        if (dynamic_cast<DockFrequentItem*>(targetItem) ||
            dynamic_cast<DockRunningItem*>(targetItem))
        {
            // Generated frequent and running items are not sortable. Keep the
            // Dock as the target, but redirect to the closest boundary in the
            // payload's sortable main/folder group.
            resetDwell();
            return RedirectDragToNearestInsertion(pt, outSlot);
        }

        RECT handoffRect = bounds;
        InflateRect(&handoffRect, -16, -10);
        if (recycleBinSlot && !insertionOnly &&
            (!freeOrder || (!dynamic_cast<DockContainer*>(app_->dragSession_.Source()) && PtInRect(&handoffRect, pt))))
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
        const bool groupTarget = folderDockItem && IsGroupDockEntryType(folderDockItem->GetEntryType());
        const auto& sourceList = app_->dragSession_.SourceList();
        const auto groupSurface = folderDockItem && folderDockItem->GetEntryType() == DockEntryType::FileGroup
            ? snowdesktop::slot_contract::SlotSurfaceKind::FileGroup
            : snowdesktop::slot_contract::SlotSurfaceKind::CollectionGroup;
        const bool groupAcceptsSource = groupTarget && (app_->dragDropController_.IsExternalDragActive() ||
            snowdesktop::slot_contract::AcceptsSlotDrop(sourceList.SourceSurfaceKind(), sourceList.SlotPayloadKind(),
                groupSurface, snowdesktop::slot_contract::ClassifyRelation(sourceList.SourceSurfaceKind(), groupSurface, false)));
        const bool dockMetadataReorder = !groupAcceptsSource &&
            snowdesktop::dock_drop_rules::
                ShouldPreferMetadataReorder(
                    dynamic_cast<DockContainer*>(
                        app_->dragSession_.Source()) != nullptr,
                    HasOnlyFolderDragSource(),
                    folderTarget);
        const bool collectionTarget =
            folderDockItem &&
            IsLogicalDockEntryType(folderDockItem->GetEntryType());
        const bool canHandoff = !insertionOnly && !dockMetadataReorder &&
            !(freeOrder && dynamic_cast<DockContainer*>(app_->dragSession_.Source())) &&
            targetItem && !targetItem->IsSelected() &&
            (groupAcceptsSource || (!groupTarget && snowdesktop::dock_drop_rules::
                SupportsHandoffTarget(
                    app_->dragSession_.SourceList().
                        hasWidgets,
                    folderTarget,
                    collectionTarget))) &&
            PtInRect(&handoffRect, pt);
        if (canHandoff && app_)
        {
            const size_t index = slot->GetIndex();
            if (folderTarget && !collectionTarget)
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
            if (groupAcceptsSource) return HitRegion::Handoff;
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
    const RECT panel = GetVisualPanelBounds(pt);
    if (!PtInRect(&panel, pt)) return HitRegion::None;
    return RedirectDragToNearestInsertion(pt, outSlot);
}

std::wstring DockContainer::GetDragHint(Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems, Container* origin, int mods) const
{
    if (app_ && app_->dockRunningDrag_) return app_->GetDockRunningDragHint();
    if (region == HitRegion::Blocked) return L"";
    if (region == HitRegion::Handoff && slot && slot->GetItem())
        return _LFW("core.drag.release_handle", slot->GetItem()->GetTitle());
    if (origin != this && !HasCapacity(sourceItems.empty() ? 1 : sourceItems.size()))
        return _LW("core.drag.dock_full");
    const DragSourceList source = app_
        ? app_->BuildDragSourceList(sourceItems, origin) : DragSourceList{};
    const bool pinFrequent = IsDockFrequentDrag(sourceItems);
    const auto hint = _LW(snowdesktop::drag_hint_rules::DockInsertionHintKey(
        origin == this && !pinFrequent,
        app_ && app_->dragDropController_.IsExternalDragActive(),
        source.hasWidgets || source.hasCollectionGroupEntries ||
            source.hasFileGroupSourceLabels,
        (mods & MK_CONTROL) != 0));
    if (app_ && !app_->dockSettings_.freeItemOrder && app_->dragSession_.HasContext())
    {
        const bool belongsToFiles = HasOnlyFolderDragSource();
        const bool overFiles = GetSectionAtPoint(app_->dragSession_.CurrentPoint()) == DockEntrySection::Files;
        if (overFiles != belongsToFiles || HasMixedFolderDragSource())
            return _LFW("core.drag.dock_section_redirect", hint);
    }
    return hint;
}

void DockContainer::DrawDropPreview(ID2D1DeviceContext* ctx, Slot* slot, HitRegion region)
{
    if (app_ && app_->dockRunningDrag_)
    {
        app_->DrawDockRunningDragPreview(ctx, this);
        return;
    }
    if (!slot || !ctx || region == HitRegion::Blocked) return;
    const auto& slots = GetSlots();
    if (!slots.empty() && slot == slots.back().get())
    {
        DrawInsertionPreview(
            ctx,
            GetDragInsertRange(app_->dragSession_.CurrentPoint()).end);
        return;
    }
    if (region == HitRegion::Handoff)
    {
        const RECT base = slot->GetBounds();
        const RECT bounds = GetElementVisualRect(base, app_->lastMousePoint_);
        const float scale = DockPreviewScale(base, bounds, ItemIconSize());
        app_->DrawD2DRoundedRectangle(ctx, bounds, 12.0f * scale,
            D2D1::ColorF(0.28f, 0.80f, 0.48f, 0.18f),
            D2D1::ColorF(0.28f, 0.90f, 0.52f, 0.88f), 2.0f * scale);
        return;
    }
    DrawInsertionPreview(
        ctx, InsertIndexFor(slot, region));
}

RECT DockContainer::GetSearchRect() const
{
    if (!app_ || !app_->dockSettings_.showQuickSearchButton) return RECT{};
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    if (quickSearchEntryIndex_ < ModelEntryCount() && quickSearchEntryIndex_ < slots.size())
        return slots[quickSearchEntryIndex_]->GetBounds();
    return slots.empty() ? RECT{} : slots.back()->GetBounds();
}

RECT DockContainer::GetWindowsButtonRect() const
{
    if (!app_ || !ShowDockWindowsButton(app_->dockSettings_))
        return RECT{};
    return GetWindowsButtonRect(GetBounds());
}

RECT DockContainer::GetWindowsButtonRect(const RECT& bounds) const
{
    if (!app_ || !ShowDockWindowsButton(app_->dockSettings_))
        return RECT{};
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
    return !IsRectEmpty(&rect) && IsFocusedElementRect(rect, pt);
}

DockEntryItem* DockContainer::EntryAtPoint(POINT pt) const
{
    const auto& slots = const_cast<DockContainer*>(this)->GetSlots();
    const size_t fixedCount = SortableEntryCount();
    const auto layout = GetMagnificationLayout();
    const RECT focus = ResolveMagnificationFocusRect(pt, layout);
    if (IsPointInScrollViewport(pt, GetVisualScrollViewport(pt, layout, focus)))
    {
        for (size_t i = 0; i < fixedCount && i < slots.size(); ++i)
        {
            RECT bounds = slots[i]->GetBounds();
            if (IsFocusedElementRect(bounds, pt, layout, focus))
                return dynamic_cast<DockEntryItem*>(slots[i]->GetItem());
        }
    }
    {
        const size_t end = FolderEntryBegin() + FolderEntryCount();
        for (size_t i = FolderEntryBegin();
             i < end && i < slots.size(); ++i)
        {
            RECT bounds = slots[i]->GetBounds();
            if (IsFocusedElementRect(bounds, pt, layout, focus))
                return dynamic_cast<DockEntryItem*>(slots[i]->GetItem());
        }
    }
    return nullptr;
}

DockRunningItem* DockContainer::RunningItemAtPoint(POINT pt) const
{
    const auto layout = GetMagnificationLayout();
    const RECT focus = ResolveMagnificationFocusRect(pt, layout);
    if (!IsPointInScrollViewport(pt, GetVisualScrollViewport(pt, layout, focus))) return nullptr;
    for (const auto& item : runningItems_)
    {
        if (!item) continue;
        if (!app_ || item->GetRunningIndex() >= app_->dockUnpinnedRunningApps_.size() ||
            !app_->dockUnpinnedRunningApps_[item->GetRunningIndex()].presence.Interactive()) continue;
        RECT bounds = item->GetBounds();
        if (IsFocusedElementRect(bounds, pt, layout, focus)) return item.get();
    }
    return nullptr;
}

DockFrequentItem* DockContainer::FrequentItemAtPoint(POINT pt) const
{
    const auto layout = GetMagnificationLayout();
    const RECT focus = ResolveMagnificationFocusRect(pt, layout);
    if (!IsPointInScrollViewport(pt, GetVisualScrollViewport(pt, layout, focus))) return nullptr;
    for (const auto& item : frequentItems_)
    {
        if (!item) continue;
        RECT bounds = item->GetBounds();
        if (IsFocusedElementRect(bounds, pt, layout, focus)) return item.get();
    }
    return nullptr;
}
