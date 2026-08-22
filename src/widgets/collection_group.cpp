/**
 * @file collection_group.cpp
 * @brief 集合组组件实现：搜索框、集合标签和文件滚动列表。
 */

#include "widget.h"
#include "slot.h"
#include "../core/transient_drag_slot.h"
#include "types.h"
#include "app.h"
#include "drop_model.h"
#include "collection_group_rules.h"
#include "search_match.h"
#include "widget_chrome_rules.h"
#include "widget_preview_scene.h"
#include "../l10n.h"
#include "../item_render_layer_rules.h"
#include "../widget_item_layout.h"
#include <algorithm>
#include <unordered_set>

namespace
{
size_t FindCollectionWidgetIndex(
    CollectionGroup* widget, const std::wstring& id)
{
    if (!widget || !widget->GetApp())
        return static_cast<size_t>(-1);
    const auto& widgets = widget->GetApp()->GetWidgets();
    for (size_t i = 0; i < widgets.size(); ++i)
        if (widgets[i].id == id) return i;
    return static_cast<size_t>(-1);
}

DesktopWidget* FindCollectionWidget(
    CollectionGroup* widget, const std::wstring& id)
{
    if (!widget) return nullptr;
    if (auto* scene = widget->GetPreviewScene())
        return scene->FindWidget(id);
    if (!widget->GetApp()) return nullptr;
    const size_t index = FindCollectionWidgetIndex(widget, id);
    auto& widgets = widget->GetApp()->GetWidgets();
    return index < widgets.size() ? &widgets[index] : nullptr;
}

DesktopWidget* CollectionGroupActiveCollection(
    CollectionGroup* widget)
{
    if (!widget || !widget->GetApp()) return nullptr;
    const std::wstring id = widget->GetActiveCollectionId();
    DesktopWidget* child = FindCollectionWidget(widget, id);
    return child && child->type == DesktopWidgetType::Collection
        ? child : nullptr;
}

RECT CollectionGroupTabsRect(CollectionGroup* widget)
{
    return widget
        ? widget->GetCategorizedTabsRect(
            !widget->
                GetVisibleCollectionIds().empty())
        : RECT{};
}

RECT CollectionGroupContentRect(CollectionGroup* widget)
{
    if (!widget) return {};
    RECT body = widget->GetBodyRect();
    InflateRect(&body, -widget->Cu(4.0f), -widget->Cu(8.0f));
    RECT search = widget->GetSearchBoxRect();
    if (!IsRectEmptyRect(search))
        body.top = std::min<LONG>(
            body.bottom,
            search.bottom + widget->Cu(8.0f));
    RECT tabs = CollectionGroupTabsRect(widget);
    if (!IsRectEmptyRect(tabs))
        body.top = std::min<LONG>(
            body.bottom, tabs.bottom + widget->Cu(8.0f));
    return widget->ApplyDetailsHeaderToViewport(body);
}

std::wstring CollectionGroupActiveCategory(CollectionGroup* widget)
{
    DesktopWidget* data = widget ? widget->GetWidgetData() : nullptr;
    if (!data) return L"";
    const auto& children = widget->GetVisibleCollectionIds();
    return snowdesktop::collection_group_rules::
        ResolveActiveItem(
            children, data->activeCategoryId);
}

std::wstring CollectionGroupTabTitle(
    CollectionGroup* widget, size_t tabIndex)
{
    if (!widget || !widget->GetApp())
        return L"";
    const auto& children = widget->GetVisibleCollectionIds();
    if (tabIndex >= children.size()) return L"";
    DesktopWidget* child = FindCollectionWidget(
        widget, children[tabIndex]);
    if (!child) return L"";
    return widget->GetApp()->ShouldUseDemoCollectionIdentity(child)
        ? widget->GetApp()->GetDemoCollectionCategoryTitle(*child)
        : child->title;
}

std::wstring CollectionGroupTabDisplayText(
    CollectionGroup* widget, size_t tabIndex)
{
    return CollectionGroupTabTitle(widget, tabIndex);
}

std::vector<int> CollectionGroupTabWidths(
    CollectionGroup* widget, int availableWidth)
{
    if (!widget) return {};
    const size_t count =
        widget->GetVisibleCollectionIds().size();
    std::vector<std::wstring> labels;
    labels.reserve(count);
    for (size_t i = 0; i < count; ++i)
        labels.push_back(
            CollectionGroupTabDisplayText(
                widget, i));
    return widget->BuildCategorizedTabWidths(
        labels, availableWidth);
}

int CollectionGroupTabsTotalWidth(
    const std::vector<int>& widths)
{
    int total = 0;
    for (int width : widths)
        total += width;
    return total;
}

RECT CollectionGroupTabLayoutRect(
    CollectionGroup* widget, size_t tabIndex)
{
    if (!widget || !widget->GetWidgetData()) return {};
    const auto& children = widget->GetVisibleCollectionIds();
    if (tabIndex >= children.size()) return {};
    RECT tabs = CollectionGroupTabsRect(widget);
    if (IsRectEmptyRect(tabs)) return {};
    const std::vector<int> widths =
        CollectionGroupTabWidths(
            widget, tabs.right - tabs.left);
    if (tabIndex >= widths.size()) return {};
    const int maxScroll = std::max(
        0, CollectionGroupTabsTotalWidth(widths) -
            static_cast<int>(tabs.right - tabs.left));
    DesktopWidget* data = widget->GetWidgetData();
    data->tabScrollOffset = std::clamp(
        data->tabScrollOffset, 0, maxScroll);
    LONG left = tabs.left - data->tabScrollOffset;
    for (size_t i = 0; i < tabIndex; ++i)
        left += widths[i];
    const int width = widths[tabIndex];
    RECT result = MakeRect(
        left, tabs.top,
        left + width, tabs.bottom);
    InflateRect(
        &result,
        -widget->Cu(2.0f),
        -widget->Cu(2.0f));
    return result;
}

RECT CollectionGroupTabRect(
    CollectionGroup* widget, size_t tabIndex)
{
    RECT result = CollectionGroupTabLayoutRect(
        widget, tabIndex);
    if (IsRectEmptyRect(result)) return {};
    RECT tabs = CollectionGroupTabsRect(widget);
    const auto clipped =
        snowdesktop::collection_group_rules::
            ClipToViewport(
                {
                    result.left, result.top,
                    result.right, result.bottom
                },
                {
                    tabs.left, tabs.top,
                    tabs.right, tabs.bottom
                });
    return clipped
        ? MakeRect(
            clipped->left, clipped->top,
            clipped->right, clipped->bottom)
        : RECT{};
}

size_t CollectionGroupTabIndexAtPoint(
    CollectionGroup* widget, POINT point)
{
    if (!widget) return static_cast<size_t>(-1);
    const size_t count =
        widget->GetVisibleCollectionIds().size();
    for (size_t i = 0; i < count; ++i)
    {
        RECT tab = CollectionGroupTabRect(widget, i);
        if (!IsRectEmptyRect(tab) && PtInRect(&tab, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

snowdesktop::widget_item_layout::Layout CollectionGroupLocalLayout(
    CollectionGroup* widget)
{
    if (!widget || !widget->GetWidgetData() || !widget->GetApp())
        return {};
    DesktopWidget* data = widget->GetWidgetData();
    const RECT content = CollectionGroupContentRect(widget);
    const auto metrics = widget->GetItemVisualMetrics();
    const float spacing = widget->GetLayoutSpacingScale();
    if (data->listMode)
        return snowdesktop::widget_item_layout::ResolveList(
            content,
            std::max(widget->GetListRowHeight(),
                metrics.minimumListHeight), spacing);
    return snowdesktop::widget_item_layout::ResolveGrid(
        content, std::max(1, data->gridSpan.columns), 0,
        metrics.minimumGridWidth, metrics.minimumGridHeight,
        spacing);
}

RECT CollectionGroupItemRect(CollectionGroup* widget, size_t index)
{
    if (!widget || !widget->GetWidgetData()) return {};
    DesktopWidget* data = widget->GetWidgetData();
    const int scroll = std::clamp(
        widget->GetScrollOffset(), 0,
        widget->GetMaxScrollOffset());
    (void)data;
    return snowdesktop::widget_item_layout::ItemRect(
        CollectionGroupLocalLayout(widget), index, scroll);
}

int CollectionGroupContentHeight(
    CollectionGroup* widget, size_t itemCount)
{
    DesktopWidget* data =
        widget ? widget->GetWidgetData() : nullptr;
    if (!data) return 0;
    return snowdesktop::widget_item_layout::ContentHeight(
        CollectionGroupLocalLayout(widget), itemCount);
}

RECT CollectionGroupListToggleRect(CollectionGroup* widget)
{
    if (!widget) return {};
    RECT handle = widget->GetMoveHandleRect();
    const float scale = widget->GetBarScale();
    const int size = widget->Cu(14.0f * scale);
    const int gap = widget->Cu(4.0f * scale);
    const int resizeReserve =
        widget->Cu(20.0f * scale);
    return MakeRect(
        handle.right - resizeReserve - gap - size,
        handle.top +
            (handle.bottom - handle.top - size) / 2,
        handle.right - resizeReserve - gap,
        handle.top +
            (handle.bottom - handle.top + size) / 2);
}

}

CollectionGroupEntryItem::CollectionGroupEntryItem(
    DesktopWidget* collection, Container* container,
    DesktopApp* app)
    : collection_(collection), container_(container), app_(app)
{
}

std::wstring CollectionGroupEntryItem::GetTitle() const
{
    return collection_ ? collection_->title : L"";
}

std::wstring CollectionGroupEntryItem::GetPath() const
{
    return L"";
}

HBITMAP CollectionGroupEntryItem::GetIconBitmap() const
{
    return nullptr;
}

RECT CollectionGroupEntryItem::GetBounds() const
{
    return bounds_;
}

void CollectionGroupEntryItem::SetBounds(RECT bounds)
{
    bounds_ = bounds;
}

bool CollectionGroupEntryItem::IsSelected() const
{
    return collection_ && collection_->selected;
}

void CollectionGroupEntryItem::SetSelected(bool selected)
{
    if (collection_) collection_->selected = selected;
}

Container* CollectionGroupEntryItem::GetContainer() const
{
    return container_;
}

void CollectionGroupEntryItem::Draw(
    ID2D1DeviceContext* context, RECT rect, int state)
{
    if (!app_ || !context || !collection_) return;
    const bool light = app_->IsLightContentTheme();
    app_->DrawD2DRoundedRectangle(
        context, rect, 8.0f,
        state >= 2
            ? D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.28f)
            : (light
                ? D2D1::ColorF(0.94f, 0.95f, 0.97f, 0.96f)
                : D2D1::ColorF(0.12f, 0.14f, 0.18f, 0.96f)),
        D2D1::ColorF(0.39f, 0.66f, 1.0f, 0.72f));
    RECT text = rect;
    InflateRect(&text, -10, -4);
    app_->DrawD2DText(context, collection_->title, text,
        app_->listItemTextFormat_.Get(),
        light
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
}

ComPtr<IDataObject> CollectionGroupEntryItem::CreateDataObject()
{
    return nullptr;
}

const std::wstring& CollectionGroupEntryItem::GetCollectionId() const
{
    static const std::wstring empty;
    return collection_ ? collection_->id : empty;
}

const std::vector<std::wstring>&
CollectionGroup::GetVisibleCollectionIds() const
{
    visibleCollectionIds_.clear();
    if (!data_ || !app_) return visibleCollectionIds_;
    for (const auto& id : data_->childWidgetIds)
    {
        if (auto* scene = GetPreviewScene())
        {
            const DesktopWidget* child = scene->FindWidget(id);
            if (child && child->type == DesktopWidgetType::Collection)
                visibleCollectionIds_.push_back(id);
            continue;
        }
        const size_t index = app_->FindWidgetIndexById(id);
        if (index < app_->widgets_.size() &&
            app_->widgets_[index].type ==
                DesktopWidgetType::Collection)
            visibleCollectionIds_.push_back(id);
    }
    return visibleCollectionIds_;
}

const std::vector<std::wstring>&
CollectionGroup::GetVisibleItemKeys() const
{
    visibleItemKeys_.clear();
    if (!data_ || !app_) return visibleItemKeys_;

    const std::wstring active =
        CollectionGroupActiveCategory(
            const_cast<CollectionGroup*>(this));
    const std::wstring query = data_->showSearchBox
        ? searchText_ : L"";
    visibleCategoryId_ = active;
    visibleSearchText_ = query;

    std::unordered_set<std::wstring> seen;
    auto* previewScene = GetPreviewScene();
    const DesktopWidget* child = previewScene
        ? previewScene->FindWidget(active) : nullptr;
    const size_t childIndex = previewScene
        ? static_cast<size_t>(-1)
        : app_->FindWidgetIndexById(active);
    if (child || (!previewScene && childIndex < app_->widgets_.size()))
    {
        const DesktopWidget& activeCollection = child
            ? *child : app_->widgets_[childIndex];
        const auto& itemKeys = child
            ? child->itemKeys : app_->widgets_[childIndex].itemKeys;
        for (size_t rawIndex = 0; rawIndex < itemKeys.size(); ++rawIndex)
        {
            const auto& rawKey = itemKeys[rawIndex];
            const std::wstring key =
                ToUpperInvariant(rawKey);
            if (!seen.insert(key).second) continue;
            if (auto* scene = GetPreviewScene())
            {
                const auto* sample = scene->FindItem(rawKey);
                if (!sample || (!query.empty() &&
                    !NameMatchesQuery(sample->title, query)))
                    continue;
            }
            else
            {
                const size_t itemIndex = app_->FindItemIndexByKey(key);
                if (itemIndex >= app_->items_.size()) continue;
                const std::wstring displayName =
                    app_->ShouldUseDemoCollectionIdentity(&activeCollection)
                    ? app_->GetDemoCollectionIdentityTitle(
                        activeCollection, rawKey)
                    : app_->items_[itemIndex].name;
                if (!query.empty() &&
                    !NameMatchesQuery(displayName, query))
                    continue;
            }
            visibleItemKeys_.push_back(rawKey);
        }
    }

    return visibleItemKeys_;
}

std::wstring CollectionGroup::GetActiveCollectionId() const
{
    return CollectionGroupActiveCategory(
        const_cast<CollectionGroup*>(this));
}

void CollectionGroup::InvalidateFilterCache()
{
    visibleCollectionIds_.clear();
    visibleItemKeys_.clear();
    visibleSearchText_.clear();
    visibleCategoryId_.clear();
    InvalidateSlots();
}

RECT CollectionGroup::GetSearchBoxRect() const
{
    return GetCategorizedSearchBoxRect(
        data_ && data_->showSearchBox);
}

RECT CollectionGroup::GetContentViewportRect() const
{
    return CollectionGroupContentRect(
        const_cast<CollectionGroup*>(this));
}

const DesktopWidget* CollectionGroup::GetDetailsSortData() const
{
    return CollectionGroupActiveCollection(
        const_cast<CollectionGroup*>(this));
}

std::wstring CollectionGroup::CategoryIdAtPoint(POINT pt) const
{
    const size_t tabIndex =
        CollectionGroupTabIndexAtPoint(
            const_cast<CollectionGroup*>(this), pt);
    if (tabIndex == static_cast<size_t>(-1))
        return L"";
    const auto& children = GetVisibleCollectionIds();
    return tabIndex < children.size()
        ? children[tabIndex]
        : L"";
}

bool CollectionGroup::TryScrollTabs(POINT pt, int delta)
{
    if (!data_) return false;
    RECT tabs = CollectionGroupTabsRect(this);
    if (IsRectEmptyRect(tabs) || !PtInRect(&tabs, pt))
        return false;
    const std::vector<int> widths =
        CollectionGroupTabWidths(
            this, tabs.right - tabs.left);
    const int maxScroll = std::max(
        0, CollectionGroupTabsTotalWidth(widths) -
            static_cast<int>(tabs.right - tabs.left));
    if (maxScroll <= 0) return false;
    data_->tabScrollOffset = std::clamp(
        data_->tabScrollOffset - delta / 2, 0, maxScroll);
    return true;
}

RECT CollectionGroup::GetTabRectById(
    const std::wstring& collectionId) const
{
    const auto& children = GetVisibleCollectionIds();
    auto it = std::find(
        children.begin(), children.end(), collectionId);
    if (it == children.end()) return {};
    return CollectionGroupTabRect(
        const_cast<CollectionGroup*>(this),
        static_cast<size_t>(
            std::distance(children.begin(), it)));
}

void CollectionGroup::EnsureTabVisible(size_t tabIndex)
{
    if (!data_) return;
    const auto& children = GetVisibleCollectionIds();
    if (tabIndex >= children.size()) return;

    RECT tabs = CollectionGroupTabsRect(this);
    if (IsRectEmptyRect(tabs)) return;
    const int viewportWidth =
        std::max<LONG>(0, tabs.right - tabs.left);
    const std::vector<int> widths =
        CollectionGroupTabWidths(
            this, viewportWidth);
    if (tabIndex >= widths.size()) return;
    const int maxScroll = std::max(
        0, CollectionGroupTabsTotalWidth(widths) -
            viewportWidth);

    LONG rawLeft = tabs.left -
        data_->tabScrollOffset;
    for (size_t i = 0; i < tabIndex; ++i)
        rawLeft += widths[i];
    const LONG rawRight =
        rawLeft + widths[tabIndex];
    if (rawLeft < tabs.left)
        data_->tabScrollOffset -=
            static_cast<int>(tabs.left - rawLeft);
    else if (rawRight > tabs.right)
        data_->tabScrollOffset +=
            static_cast<int>(rawRight - tabs.right);
    data_->tabScrollOffset = std::clamp(
        data_->tabScrollOffset, 0, maxScroll);
}

CollectionGroupEntryItem*
CollectionGroup::GetTabItemAtPoint(POINT pt) const
{
    const size_t tabIndex =
        CollectionGroupTabIndexAtPoint(
            const_cast<CollectionGroup*>(this), pt);
    if (tabIndex == static_cast<size_t>(-1) ||
        !data_ || !app_)
        return nullptr;
    const auto& children = GetVisibleCollectionIds();
    if (tabIndex >= children.size()) return nullptr;
    const size_t childIndex =
        app_->FindWidgetIndexById(children[tabIndex]);
    if (childIndex >= app_->widgets_.size()) return nullptr;
    tabItemCache_ =
        std::make_unique<CollectionGroupEntryItem>(
            &app_->widgets_[childIndex],
            const_cast<CollectionGroup*>(this), app_);
    tabItemCache_->SetBounds(
        CollectionGroupTabRect(
            const_cast<CollectionGroup*>(this), tabIndex));
    return tabItemCache_.get();
}

size_t CollectionGroup::GetSlotCount() const
{
    return GetVisibleItemKeys().size();
}

int CollectionGroup::GetItemHeight() const
{
    return CollectionGroupLocalLayout(
        const_cast<CollectionGroup*>(this)).vertical.cell;
}

int CollectionGroup::GetItemWidth() const
{
    return CollectionGroupLocalLayout(
        const_cast<CollectionGroup*>(this)).horizontal.cell;
}

bool CollectionGroup::SingleColumn() const
{
    return data_ && data_->listMode;
}

Item* CollectionGroup::GetSlotItem(size_t idx) const
{
    const auto& keys = GetVisibleItemKeys();
    if (!app_ || idx >= keys.size()) return nullptr;
    if (auto* scene = GetPreviewScene())
    {
        DesktopItem* sample = scene->FindDesktopItem(keys[idx]);
        if (!sample) return nullptr;
        auto item = std::make_unique<DesktopIcon>(
            sample,
            const_cast<CollectionGroup*>(this), app_);
        Item* result = item.get();
        slotItemCache_.push_back(std::move(item));
        return result;
    }
    const size_t itemIndex =
        app_->FindItemIndexByKey(keys[idx]);
    if (itemIndex >= app_->items_.size()) return nullptr;
    auto item = std::make_unique<DesktopIcon>(
        &app_->items_[itemIndex],
        const_cast<CollectionGroup*>(this), app_);
    Item* result = item.get();
    slotItemCache_.push_back(std::move(item));
    return result;
}

std::vector<std::unique_ptr<Slot>>
CollectionGroup::BuildSlots()
{
    slotItemCache_.clear();
    std::vector<std::unique_ptr<Slot>> slots;
    if (!data_ || !app_) return slots;
    const size_t count = GetVisibleItemKeys().size();
    if (count == 0) return slots;

    RECT content = GetContentViewportRect();
    const int visibleHeight =
        std::max<int>(1, content.bottom - content.top);
    const int scroll = GetScrollOffset();
    const auto range = snowdesktop::widget_item_layout::VisibleRange(
        CollectionGroupLocalLayout(this), count,
        scroll, visibleHeight);
    const size_t firstIndex = range.first;
    const size_t lastIndex = range.second;

    slots.reserve(lastIndex - firstIndex);
    for (size_t i = firstIndex; i < lastIndex; ++i)
    {
        RECT bounds = CollectionGroupItemRect(this, i);
        if (IsRectEmptyRect(bounds)) continue;
        auto slot =
            std::make_unique<Slot>(this, bounds, i);
        Item* item = GetSlotItem(i);
        if (item) item->SetBounds(bounds);
        slot->SetItem(item);
        slots.push_back(std::move(slot));
    }
    return slots;
}

Item* CollectionGroup::GetMemberItem(size_t idx) const
{
    if (!app_) return nullptr;
    const auto& keys = GetVisibleItemKeys();
    if (idx >= keys.size()) return nullptr;
    const size_t itemIndex =
        app_->FindItemIndexByKey(keys[idx]);
    if (itemIndex >= app_->items_.size()) return nullptr;
    auto item = std::make_unique<DesktopIcon>(
        &app_->items_[itemIndex],
        const_cast<CollectionGroup*>(this), app_);
    item->SetBounds(
        CollectionGroupItemRect(
            const_cast<CollectionGroup*>(this), idx));
    Item* result = item.get();
    dragSourceCache_.push_back(std::move(item));
    return result;
}

std::vector<Item*> CollectionGroup::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_ || !app_) return result;

    for (size_t i = 0; i < data_->childWidgetIds.size(); ++i)
    {
        const size_t childIndex =
            app_->FindWidgetIndexById(data_->childWidgetIds[i]);
        if (childIndex >= app_->widgets_.size() ||
            !app_->widgets_[childIndex].selected)
            continue;
        auto item =
            std::make_unique<CollectionGroupEntryItem>(
                &app_->widgets_[childIndex],
                const_cast<CollectionGroup*>(this), app_);
        item->SetBounds(CollectionGroupTabRect(
            const_cast<CollectionGroup*>(this), i));
        Item* raw = item.get();
        dragSourceCache_.push_back(std::move(item));
        result.push_back(raw);
    }
    if (!result.empty()) return result;

    const auto& keys = GetVisibleItemKeys();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const size_t itemIndex =
            app_->FindItemIndexByKey(keys[i]);
        if (itemIndex >= app_->items_.size() ||
            !app_->items_[itemIndex].selected)
            continue;
        if (Item* item = GetMemberItem(i))
            result.push_back(item);
    }
    return result;
}

std::vector<size_t>
CollectionGroup::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_ || !app_) return result;
    for (size_t i = 0; i < data_->childWidgetIds.size(); ++i)
    {
        const size_t childIndex =
            app_->FindWidgetIndexById(data_->childWidgetIds[i]);
        if (childIndex < app_->widgets_.size() &&
            app_->widgets_[childIndex].selected)
            result.push_back(i);
    }
    return result;
}

void CollectionGroup::ReorderMembers(
    const std::vector<size_t>& indices,
    size_t insertBefore)
{
    if (!data_ || indices.empty()) return;
    data_->childWidgetIds =
        snowdesktop::collection_group_rules::
            ReorderItems(
                data_->childWidgetIds,
                indices, insertBefore);
    InvalidateFilterCache();
}

size_t CollectionGroup::GetDropInsertIndex(
    Slot* targetSlot, HitRegion region) const
{
    if (!data_) return 0;
    if (targetSlot && targetSlot == tabDropSlot_.get())
    {
        size_t index = std::min(
            targetSlot->GetIndex(),
            data_->childWidgetIds.size());
        if (region == HitRegion::SortAfter)
            index = std::min(
                index + 1, data_->childWidgetIds.size());
        return index;
    }

    DesktopWidget* active =
        CollectionGroupActiveCollection(
            const_cast<CollectionGroup*>(this));
    if (!active) return 0;

    size_t visibleInsert = targetSlot
        ? targetSlot->GetIndex()
        : GetVisibleItemKeys().size();
    if (targetSlot && region == HitRegion::SortAfter)
        ++visibleInsert;
    const auto& visible = GetVisibleItemKeys();
    if (visibleInsert < visible.size())
    {
        const std::wstring anchor =
            ToUpperInvariant(visible[visibleInsert]);
        for (size_t i = 0;
            i < active->itemKeys.size(); ++i)
            if (ToUpperInvariant(active->itemKeys[i]) ==
                anchor)
                return i;
    }
    return active->itemKeys.size();
}

int CollectionGroup::GetTotalContentHeight() const
{
    return CollectionGroupContentHeight(
        const_cast<CollectionGroup*>(this),
        GetVisibleItemKeys().size());
}

int CollectionGroup::GetVisibleContentHeight() const
{
    RECT content = GetContentViewportRect();
    return std::max<int>(
        0, content.bottom - content.top);
}

int CollectionGroup::GetMaxScrollOffset() const
{
    return std::max(
        0, GetTotalContentHeight() -
            GetVisibleContentHeight() +
            Cu(kMinCellHeight / 2.0f));
}

void CollectionGroup::ApplyMarqueeSelection(
    const RECT& contentRect)
{
    if (!data_ || !app_) return;

    const std::wstring active =
        GetActiveCollectionId();
    const size_t childIndex =
        app_->FindWidgetIndexById(active);
    if (childIndex < app_->widgets_.size())
    {
        for (const auto& key :
            app_->widgets_[childIndex].itemKeys)
        {
            const size_t itemIndex =
                app_->FindItemIndexByKey(key);
            if (itemIndex < app_->items_.size())
                app_->items_[itemIndex].selected =
                    false;
        }
    }

    const auto& keys = GetVisibleItemKeys();
    const int scroll = GetScrollOffset();
    for (size_t i = 0; i < keys.size(); ++i)
    {
        const size_t itemIndex =
            app_->FindItemIndexByKey(keys[i]);
        if (itemIndex >= app_->items_.size())
            continue;
        RECT itemRect =
            CollectionGroupItemRect(this, i);
        app_->items_[itemIndex].selected =
            snowdesktop::collection_group_rules::
                MarqueeSelectsViewportItem(
                    {
                        itemRect.left, itemRect.top,
                        itemRect.right, itemRect.bottom
                    },
                    scroll,
                    {
                        contentRect.left,
                        contentRect.top,
                        contentRect.right,
                        contentRect.bottom
                    });
    }
}

WidgetHit CollectionGroup::HitTestWidget(POINT pt) const
{
    WidgetHit base =
        WidgetContainer::HitTestWidget(pt);
    if (base == WidgetHit::None) return base;
    RECT search = GetSearchBoxRect();
    if (!IsRectEmptyRect(search) &&
        PtInRect(&search, pt))
        return WidgetHit::SearchBox;
    if (!CategoryIdAtPoint(pt).empty())
        return WidgetHit::CategoryTab;
    const WidgetHit details = HitTestDetailsHeader(
        pt, GetContentViewportRect());
    if (details != WidgetHit::None) return details;
    if (base == WidgetHit::MoveHandle)
    {
        RECT listToggle =
            CollectionGroupListToggleRect(
                const_cast<CollectionGroup*>(this));
        if (PtInRect(&listToggle, pt))
            return WidgetHit::ListToggleBtn;
    }
    return base;
}

HitRegion CollectionGroup::HitTestDrag(
    POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt))
        return HitRegion::None;

    const size_t tabIndex =
        CollectionGroupTabIndexAtPoint(this, pt);
    if (tabIndex != static_cast<size_t>(-1))
    {
        RECT tab = CollectionGroupTabRect(this, tabIndex);
        outSlot = BindTransientDragSlot(
            tabDropSlot_, this, tab, tabIndex,
            SlotFeedbackRole::CollectionGroupTab);
        return pt.x < tab.left +
                (tab.right - tab.left) / 2
            ? HitRegion::SortBefore
            : HitRegion::SortAfter;
    }

    if (app_ &&
        app_->dragSession_.SourceList().
            hasCollectionGroupEntries)
        return HitRegion::Empty;

    return WidgetContainer::HitTestDrag(pt, outSlot);
}

void CollectionGroup::DrawDropPreview(
    ID2D1DeviceContext* ctx, Slot* slot,
    HitRegion region)
{
    if (!ctx) return;
    if (app_ &&
        app_->dragSession_.SourceList().
            hasCollectionGroupEntries &&
        region == HitRegion::Empty)
    {
        RECT target = GetFrameRect();
        const int padding = Cu(3.0f);
        InflateRect(&target, padding, padding);
        app_->DrawD2DRoundedRectangle(
            ctx, target, static_cast<float>(Cu(10.0f)),
            D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.14f),
            D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.92f),
            static_cast<float>(Cu(2.5f)));
        return;
    }
    WidgetContainer::DrawDropPreview(
        ctx, slot, region);
}

std::wstring CollectionGroup::GetDragHint(
    Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems,
    Container* origin, int mods) const
{
    if (region == HitRegion::None ||
        region == HitRegion::Blocked ||
        sourceItems.empty())
        return L"";
    const bool allCollections = std::all_of(
        sourceItems.begin(), sourceItems.end(),
        [](Item* item) {
            return dynamic_cast<
                CollectionGroupEntryItem*>(item) != nullptr;
        });
    if (allCollections)
        return origin == this
            ? _LW("widget.collection_group.reorder_hint")
            : _LW("widget.collection_group.add_hint");
    return WidgetContainer::GetDragHint(
        slot, region, sourceItems, origin, mods);
}

void CollectionGroup::OnItemsDropped(
    const std::vector<Item*>& sourceItems,
    Container* origin, Slot* targetSlot,
    HitRegion region, int mods)
{
    if (!app_ || !data_) return;
    const bool allCollections = std::all_of(
        sourceItems.begin(), sourceItems.end(),
        [](Item* item) {
            return dynamic_cast<
                CollectionGroupEntryItem*>(item) != nullptr;
        });
    const DragSourceList& sessionSource =
        app_->dragSession_.SourceList();
    DragSourceList sourceList =
        !sessionSource.Empty() &&
            app_->dragSession_.Source() == origin
        ? sessionSource
        : app_->BuildDragSourceList(
            sourceItems, origin);
    DropPreviewList preview =
        app_->BuildDropPreviewList(
            sourceList, this, targetSlot, region,
            mods, app_->dragSession_.CurrentPoint());
    if (allCollections &&
        CategoryIdAtPoint(
            app_->dragSession_.CurrentPoint()).empty())
    {
        preview.insertIndex =
            data_->childWidgetIds.size();
        for (size_t i = 0;
            i < preview.landings.size(); ++i)
            preview.landings[i].insertIndex =
                preview.insertIndex + i;
    }
    app_->ExecuteDropPipeline(sourceList, preview);
}

void CollectionGroup::DrawContent(
    ID2D1DeviceContext* context, RECT)
{
    if (!data_ || !app_ || !context) return;
    const bool light = app_->IsLightContentTheme();
    const bool privacyActive =
        data_->privacyMode &&
        !app_->dragSession_.IsActive() &&
        !app_->dragDropController_.IsExternalDragActive() &&
        !PtInRect(
            &data_->bounds, app_->lastMousePoint_);
    DrawSearchBox(context);

    const auto& children = GetVisibleCollectionIds();
    RECT tabs = CollectionGroupTabsRect(this);
    if (!IsRectEmptyRect(tabs))
    {
        const std::wstring active =
            CollectionGroupActiveCategory(this);
        const size_t tabCount = children.size();
        context->PushAxisAlignedClip(
            app_->ToD2DRect(tabs),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (size_t i = 0; i < tabCount; ++i)
        {
            RECT layoutTab = CollectionGroupTabLayoutRect(this, i);
            RECT hitTab = CollectionGroupTabRect(this, i);
            if (IsRectEmptyRect(layoutTab) ||
                IsRectEmptyRect(hitTab))
                continue;
            const std::wstring& id = children[i];
            const bool selected = id == active;
            const bool hovered =
                !IsPreviewRendering() &&
                PtInRect(&hitTab, app_->lastMousePoint_) != FALSE;
            DrawCategorizedTab(
                context,
                hitTab,
                layoutTab,
                CollectionGroupTabDisplayText(
                    this, i),
                selected, hovered,
                snowdesktop::widget_chrome_rules::
                    UsesCategorizedControlAccentOutline(
                        app_->keyboardNavVisualFocus_,
                        app_->keyboardNavCollectionGroupTabs_ &&
                            app_->keyboardNavMemberIndex_ ==
                                static_cast<int>(i) &&
                            app_->OwnsWidgetKeyboardNavigation(this)));
        }
        context->PopAxisAlignedClip();
    }

    RECT content = GetContentViewportRect();
    DrawDetailsHeader(context, content);
    if (children.empty())
    {
        IDWriteTextFormat* centered =
            GetCuTextFormat(13.0f, false, true);
        app_->DrawD2DText(
            context,
            _LW("widget.collection_group.empty"),
            content,
            centered
                ? centered
                : app_->listItemTextFormat_.Get(),
            light
                ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.68f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.66f),
            DWRITE_WORD_WRAPPING_WRAP);
        return;
    }

    const auto& keys = GetVisibleItemKeys();
    if (keys.empty())
    {
        IDWriteTextFormat* centered =
            GetCuTextFormat(13.0f, false, true);
        app_->DrawD2DText(
            context,
            searchText_.empty()
                ? _LW("widget.collection_group.empty_items")
                : _LW("widget.categories.no_results"),
            content,
            centered
                ? centered
                : app_->listItemTextFormat_.Get(),
            light
                ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.68f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.66f),
            DWRITE_WORD_WRAPPING_WRAP);
        return;
    }

    context->PushAxisAlignedClip(
        app_->ToD2DRect(content),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const std::wstring activeCollectionId =
        CollectionGroupActiveCategory(this);
    const DesktopWidget* activeCollection = nullptr;
    if (auto* scene = GetPreviewScene())
        activeCollection = scene->FindWidget(activeCollectionId);
    else
    {
        const size_t activeIndex = app_->FindWidgetIndexById(
            activeCollectionId);
        if (activeIndex < app_->widgets_.size())
            activeCollection = &app_->widgets_[activeIndex];
    }

    std::vector<std::pair<Item*, RECT>>
        foregroundTitles;
    for (const auto& slot : GetSlots())
    {
        if (!slot || !slot->GetItem()) continue;
        RECT row = slot->GetBounds();
        if (row.bottom <= content.top ||
            row.top >= content.bottom)
            continue;
        auto* icon =
            dynamic_cast<DesktopIcon*>(slot->GetItem());
        if (!icon) continue;
        DesktopItem* item = icon->GetDesktopItem();
        if (!item) continue;
        if (data_->listMode)
        {
            if (privacyActive)
                DrawPrivacyPlaceholder(
                    context, row, item->name, false);
            else
            {
                const bool useDemoIdentity =
                    app_->ShouldUseDemoCollectionIdentity(activeCollection);
                const std::wstring_view demoIdentity =
                    !useDemoIdentity ? std::wstring_view{} :
                    (item->layoutKey.empty()
                        ? std::wstring_view(item->parsingName)
                        : std::wstring_view(item->layoutKey));
                DrawListItem(
                    context, row, item->iconBitmap,
                    item->sysIconIndex, item->name,
                    item->selected, item->iconIsMediaThumbnail,
                    demoIdentity, activeCollection,
                    { item->typeName, item->modifiedTime,
                      item->fileSize, false });
            }
        }
        else if (privacyActive)
        {
            DrawPrivacyPlaceholder(
                context, row, item->name, false);
        }
        else
        {
            RECT body = GetBodyRect();
            const bool hovered =
                !IsPreviewRendering() &&
                !item->selected &&
                PtInRect(
                    &row, app_->lastMousePoint_) &&
                PtInRect(
                    &body, app_->lastMousePoint_);
            const auto titleLayers =
                snowdesktop::item_render_layer_rules::
                    ResolveTitleLayerPlan(
                        item->selected);
            icon->Draw(
                context, row,
                item->selected
                    ? 2
                    : (hovered ? 1 : 0),
                light, titleLayers.drawWithItem,
                false, activeCollection);
            if (titleLayers.drawInForeground)
                foregroundTitles.emplace_back(icon, row);
        }
    }
    for (const auto& [item, bounds] : foregroundTitles)
        item->DrawTitle(
            context, bounds, true, 1.0f,
            light, activeCollection);
    context->PopAxisAlignedClip();
}

RECT CollectionGroup::GetMemberLayoutRect(size_t index) const
{
    return CollectionGroupItemRect(
        const_cast<CollectionGroup*>(this), index);
}

void CollectionGroup::DrawButtons(
    ID2D1DeviceContext* context, RECT, bool)
{
    if (!data_ || !app_ || !context) return;
    const bool light = app_->IsLightContentTheme();
    const float scale = GetBarScale();
    IDWriteTextFormat* format =
        GetCuFluentTextFormat(14.0f * scale);
    IDWriteTextFormat* fallback = format
        ? format
        : (app_->fluentIconTextFormat_
            ? app_->fluentIconTextFormat_.Get()
            : app_->listItemTextFormat_.Get());

    RECT listToggle =
        CollectionGroupListToggleRect(this);
    const bool listHot =
        !IsPreviewRendering() && PtInRect(
            &listToggle, app_->lastMousePoint_) != FALSE;
    app_->DrawD2DText(
        context,
        data_->listMode ? L"\uF462" : L"\uF4ED",
        listToggle, fallback,
        light
            ? D2D1::ColorF(
                0.10f, 0.12f, 0.16f,
                listHot ? 0.85f : 0.50f)
            : D2D1::ColorF(
                1.0f, 1.0f, 1.0f,
                listHot ? 0.95f : 0.60f));
}
