/**
 * @file file_group.cpp
 * @brief 文件组组件：托管桌面文件分类与映射文件夹。
 */

#include "widget.h"
#include "slot.h"
#include "types.h"
#include "app.h"
#include "drop_model.h"
#include "collection_group_rules.h"
#include "../menu_fluent_glyphs.h"
#include "../search_match.h"
#include "widget_preview_scene.h"
#include "../l10n.h"

#include <algorithm>
#include <unordered_set>

namespace
{

size_t FindFileGroupSourceIndex(
    FileGroup* group, const std::wstring& id)
{
    if (!group || !group->GetApp())
        return static_cast<size_t>(-1);
    const auto& widgets = group->GetApp()->GetWidgets();
    for (size_t i = 0; i < widgets.size(); ++i)
        if (widgets[i].id == id)
            return i;
    return static_cast<size_t>(-1);
}

bool IsFileGroupSourceType(DesktopWidgetType type)
{
    return type == DesktopWidgetType::FileCategories ||
        type == DesktopWidgetType::FolderMapping;
}

DesktopWidget* FindFileGroupSource(
    FileGroup* group, const std::wstring& id)
{
    if (!group) return nullptr;
    if (auto* scene = group->GetPreviewScene())
        return scene->FindWidget(id);
    if (!group->GetApp()) return nullptr;
    const size_t index = FindFileGroupSourceIndex(group, id);
    auto& widgets = group->GetApp()->GetWidgets();
    return index < widgets.size() ? &widgets[index] : nullptr;
}

RECT FileGroupSourceTabsRect(FileGroup* group)
{
    return group
        ? group->GetCategorizedTabsRect(
            snowdesktop::collection_group_rules::
                ShouldShowFileGroupSourceTabs(
                    group->GetWidgetData() &&
                        group->GetWidgetData()->
                            showSearchBox,
                    group->GetSearchText().empty()) &&
            !group->GetVisibleSourceIds().empty())
        : RECT{};
}

std::wstring FileGroupSourceTabText(
    FileGroup* group, size_t tabIndex)
{
    if (!group || !group->GetApp()) return L"";
    const auto& sources = group->GetVisibleSourceIds();
    if (tabIndex >= sources.size()) return L"";
    const DesktopWidget* child = FindFileGroupSource(
        group, sources[tabIndex]);
    if (!child) return L"";
    const size_t count =
        child->type == DesktopWidgetType::FolderMapping
            ? child->folderEntries.size()
            : child->itemKeys.size();
    return child->title + L" " + std::to_wstring(count);
}

std::vector<int> FileGroupSourceTabWidths(
    FileGroup* group, int availableWidth)
{
    std::vector<std::wstring> labels;
    if (!group) return {};
    const auto& sources = group->GetVisibleSourceIds();
    labels.reserve(sources.size());
    for (size_t i = 0; i < sources.size(); ++i)
        labels.push_back(FileGroupSourceTabText(group, i));
    return group->BuildCategorizedTabWidths(
        labels, availableWidth);
}

int TotalTabWidth(const std::vector<int>& widths)
{
    int total = 0;
    for (int width : widths) total += width;
    return total;
}

RECT FileGroupSourceTabLayoutRect(
    FileGroup* group, size_t tabIndex)
{
    if (!group || !group->GetWidgetData()) return {};
    const auto& sources = group->GetVisibleSourceIds();
    if (tabIndex >= sources.size()) return {};
    RECT tabs = FileGroupSourceTabsRect(group);
    if (IsRectEmptyRect(tabs)) return {};
    const std::vector<int> widths =
        FileGroupSourceTabWidths(
            group, tabs.right - tabs.left);
    if (tabIndex >= widths.size()) return {};
    DesktopWidget* data = group->GetWidgetData();
    data->tabScrollOffset =
        snowdesktop::collection_group_rules::
            ClampIndependentTabScroll(
                data->tabScrollOffset,
                TotalTabWidth(widths),
                tabs.right - tabs.left);
    LONG left = tabs.left - data->tabScrollOffset;
    for (size_t i = 0; i < tabIndex; ++i)
        left += widths[i];
    RECT result = MakeRect(
        left, tabs.top,
        left + widths[tabIndex], tabs.bottom);
    InflateRect(
        &result, -group->Cu(2.0f), -group->Cu(2.0f));
    return result;
}

RECT FileGroupSourceTabRect(
    FileGroup* group, size_t tabIndex)
{
    RECT result = FileGroupSourceTabLayoutRect(
        group, tabIndex);
    if (IsRectEmptyRect(result)) return {};
    RECT tabs = FileGroupSourceTabsRect(group);
    const auto clipped =
        snowdesktop::collection_group_rules::ClipToViewport(
            {
                result.left, result.top,
                result.right, result.bottom
            },
            {
                tabs.left, tabs.top,
                tabs.right, tabs.bottom
            });
    if (!clipped)
        return RECT{};
    return MakeRect(
        clipped->left, clipped->top,
        clipped->right, clipped->bottom);
}

size_t FileGroupSourceTabIndexAtPoint(
    FileGroup* group, POINT point)
{
    if (!group) return static_cast<size_t>(-1);
    const auto& sources = group->GetVisibleSourceIds();
    for (size_t i = 0; i < sources.size(); ++i)
    {
        RECT rect = FileGroupSourceTabRect(group, i);
        if (!IsRectEmptyRect(rect) && PtInRect(&rect, point))
            return i;
    }
    return static_cast<size_t>(-1);
}

struct FileGroupButtonRects
{
    RECT date{};
    RECT list{};
    RECT open{};
};

FileGroupButtonRects GetFileGroupButtonRects(
    FileGroup* group, bool includeOpen)
{
    FileGroupButtonRects result;
    if (!group) return result;
    RECT handle = group->GetMoveHandleRect();
    const float scale = group->GetBarScale();
    const int size = group->Cu(14.0f * scale);
    const int gap = group->Cu(4.0f * scale);
    const int between = group->Cu(7.0f * scale);
    const int resizeReserve = group->Cu(20.0f * scale);
    const int height = handle.bottom - handle.top;
    LONG right = handle.right - resizeReserve - gap;
    if (includeOpen)
    {
        result.open = MakeRect(
            right - size,
            handle.top + (height - size) / 2,
            right,
            handle.top + (height + size) / 2);
        right = result.open.left - between;
    }
    result.list = MakeRect(
        right - size,
        handle.top + (height - size) / 2,
        right,
        handle.top + (height + size) / 2);
    right = result.list.left - between;
    result.date = MakeRect(
        right - size,
        handle.top + (height - size) / 2,
        right,
        handle.top + (height + size) / 2);
    return result;
}

/**
 * 临时把活动子组件投影到文件组外框。子组件的数据所有权和自身
 * 持久化显示状态不变；退出作用域时恢复。
 */
class HostedFileSourceScope
{
public:
    HostedFileSourceScope(
        FileGroup* group, ScrollingItemWidget* source)
        : group_(group), source_(source)
    {
        groupData_ = group_ ? group_->GetWidgetData() : nullptr;
        sourceData_ = source_ ? source_->GetWidgetData() : nullptr;
        if (!groupData_ || !sourceData_) return;

        savedGridSpan_ = sourceData_->gridSpan;
        savedGridCell_ = sourceData_->gridCell;
        savedBounds_ = sourceData_->bounds;
        savedCellScale_ = sourceData_->cellScale;
        savedListMode_ = sourceData_->listMode;
        savedDateHeaders_ = sourceData_->dateHeaders;
        savedShowCategories_ = sourceData_->showFileCategories;
        savedShowSearch_ = sourceData_->showSearchBox;
        savedPrivacy_ = sourceData_->privacyMode;
        savedScrollOffset_ = sourceData_->scrollOffset;
        savedSearchText_ = source_->GetSearchText();
        savedSearchFocused_ = source_->IsSearchFocused();
        savedSearchCursor_ =
            source_->GetSearchCursorPosition();
        savedSearchSelectionAnchor_ =
            source_->GetSearchSelectionAnchor();
        savedSearchComposition_ =
            source_->GetSearchCompositionText();
        savedSearchCompositionCursor_ =
            source_->GetSearchCompositionCursor();

        sourceData_->gridSpan = groupData_->gridSpan;
        // Hosted item geometry (notably icon-mode cell height) resolves its
        // grid page from gridCell.pageId.  Inherit the parent page while the
        // child is hosted so cross-monitor DPI/cell sizing follows the file
        // group instead of the child's last standalone position.
        sourceData_->gridCell = groupData_->gridCell;
        sourceData_->bounds = groupData_->bounds;
        sourceData_->cellScale = groupData_->cellScale;
        sourceData_->listMode = groupData_->listMode;
        sourceData_->dateHeaders = groupData_->dateHeaders;
        sourceData_->showFileCategories =
            groupData_->showFileCategories;
        sourceData_->showSearchBox = groupData_->showSearchBox;
        sourceData_->privacyMode = groupData_->privacyMode;
        sourceData_->scrollOffset = groupData_->scrollOffset;

        if (savedSearchText_ != group_->GetSearchText())
        {
            source_->SetSearchText(group_->GetSearchText());
            searchTextChanged_ = true;
        }
        source_->SetSearchFocused(
            group_->IsSearchFocused());
        source_->SetSearchEditingState(
            group_->GetSearchCursorPosition(),
            group_->GetSearchSelectionAnchor(),
            group_->GetSearchCompositionText(),
            group_->GetSearchCompositionCursor());

        RECT frame = group_->GetFrameRect();
        source_->SetHostedFrame(&frame);
        const bool searching =
            groupData_->showSearchBox &&
            !group_->GetSearchText().empty();
        source_->SetCategorizedHostOptions(
            1,
            true, groupData_->showSearchBox,
            true,
            snowdesktop::collection_group_rules::
                ShouldShowInnerCategoryTabs(
                    groupData_->showFileCategories,
                    groupData_->showSearchBox,
                    !searching),
            true);
        active_ = true;
    }

    ~HostedFileSourceScope()
    {
        if (!active_) return;
        groupData_->scrollOffset = std::max(
            0, sourceData_->scrollOffset);
        source_->ClearCategorizedHostOptions();
        source_->SetHostedFrame(nullptr);
        sourceData_->gridSpan = savedGridSpan_;
        sourceData_->gridCell = savedGridCell_;
        sourceData_->bounds = savedBounds_;
        sourceData_->cellScale = savedCellScale_;
        sourceData_->listMode = savedListMode_;
        sourceData_->dateHeaders = savedDateHeaders_;
        sourceData_->showFileCategories = savedShowCategories_;
        sourceData_->showSearchBox = savedShowSearch_;
        sourceData_->privacyMode = savedPrivacy_;
        sourceData_->scrollOffset = savedScrollOffset_;
        if (searchTextChanged_)
            source_->SetSearchText(savedSearchText_);
        source_->SetSearchFocused(savedSearchFocused_);
        source_->SetSearchEditingState(
            savedSearchCursor_,
            savedSearchSelectionAnchor_,
            savedSearchComposition_,
            savedSearchCompositionCursor_);
    }

    explicit operator bool() const { return active_; }

private:
    FileGroup* group_ = nullptr;
    ScrollingItemWidget* source_ = nullptr;
    DesktopWidget* groupData_ = nullptr;
    DesktopWidget* sourceData_ = nullptr;
    GridSpan savedGridSpan_{};
    GridCell savedGridCell_{};
    RECT savedBounds_{};
    float savedCellScale_ = 1.0f;
    bool savedListMode_ = false;
    bool savedDateHeaders_ = false;
    bool savedShowCategories_ = false;
    bool savedShowSearch_ = false;
    bool savedPrivacy_ = false;
    int savedScrollOffset_ = 0;
    std::wstring savedSearchText_;
    size_t savedSearchCursor_ = 0;
    size_t savedSearchSelectionAnchor_ = 0;
    std::wstring savedSearchComposition_;
    size_t savedSearchCompositionCursor_ = 0;
    bool savedSearchFocused_ = false;
    bool searchTextChanged_ = false;
    bool active_ = false;
};

std::unique_ptr<Item> CloneHostedItem(
    Item* item, Container* container,
    DesktopApp* app)
{
    if (auto* desktop = dynamic_cast<DesktopIcon*>(item))
        return std::make_unique<DesktopIcon>(
            desktop->GetDesktopItem(), container, app);
    if (auto* folder = dynamic_cast<FolderEntryIcon*>(item))
        return std::make_unique<FolderEntryIcon>(
            folder->GetFolderEntry(), container, app);
    return nullptr;
}

int FileGroupSearchCellHeight(FileGroup* group)
{
    if (!group || !group->GetApp() ||
        !group->GetApp()->GetDesktopGrid())
        return kMinCellHeight;
    DesktopWidget* data = group->GetWidgetData();
    for (const auto& page :
         group->GetApp()->GetDesktopGrid()->GetPages())
        if (data && page.id == data->gridCell.pageId)
            return page.cellHeight;
    return kMinCellHeight;
}

int FileGroupSearchContentHeight(
    FileGroup* group, size_t count)
{
    DesktopWidget* data =
        group ? group->GetWidgetData() : nullptr;
    if (!data) return 0;
    if (data->listMode)
        return static_cast<int>(count) *
            group->Cu(38.0f);
    const int columns =
        std::max(1, data->gridSpan.columns);
    const int rows = static_cast<int>(
        (count + static_cast<size_t>(columns) - 1) /
        static_cast<size_t>(columns));
    return rows * FileGroupSearchCellHeight(group);
}

RECT FileGroupSearchItemRect(
    FileGroup* group, size_t index)
{
    DesktopWidget* data =
        group ? group->GetWidgetData() : nullptr;
    if (!data) return {};
    RECT content = group->GetContentViewportRect();
    const int scroll = std::clamp(
        data->scrollOffset, 0,
        group->GetMaxScrollOffset());
    if (data->listMode)
    {
        const int height = group->Cu(38.0f);
        RECT result = MakeRect(
            content.left,
            content.top +
                static_cast<LONG>(index * height) -
                scroll,
            content.right,
            content.top +
                static_cast<LONG>((index + 1) * height) -
                scroll);
        InflateRect(
            &result, -group->Cu(4.0f),
            -group->Cu(2.0f));
        return result;
    }

    const int columns =
        std::max(1, data->gridSpan.columns);
    const int column = static_cast<int>(
        index % static_cast<size_t>(columns));
    const int row = static_cast<int>(
        index / static_cast<size_t>(columns));
    const int width = std::max<int>(
        1, (content.right - content.left) / columns);
    const int height = FileGroupSearchCellHeight(group);
    return MakeRect(
        content.left + column * width,
        content.top + row * height - scroll,
        column + 1 == columns
            ? content.right
            : content.left + (column + 1) * width,
        content.top + (row + 1) * height - scroll);
}

} // namespace

FileGroupEntryItem::FileGroupEntryItem(
    DesktopWidget* child, Container* container,
    DesktopApp* app)
    : child_(child), container_(container), app_(app)
{
}

std::wstring FileGroupEntryItem::GetTitle() const
{
    return child_ ? child_->title : L"";
}

std::wstring FileGroupEntryItem::GetPath() const
{
    return child_ ? child_->sourceFolderPath : L"";
}

HBITMAP FileGroupEntryItem::GetIconBitmap() const
{
    return nullptr;
}

RECT FileGroupEntryItem::GetBounds() const
{
    return bounds_;
}

void FileGroupEntryItem::SetBounds(RECT bounds)
{
    bounds_ = bounds;
}

bool FileGroupEntryItem::IsSelected() const
{
    return child_ && child_->selected;
}

void FileGroupEntryItem::SetSelected(bool selected)
{
    if (child_) child_->selected = selected;
}

Container* FileGroupEntryItem::GetContainer() const
{
    return container_;
}

void FileGroupEntryItem::Draw(
    ID2D1DeviceContext* context, RECT rect, int state)
{
    if (!app_ || !context || !child_) return;
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
    app_->DrawD2DText(
        context, child_->title, text,
        app_->listItemTextFormat_.Get(),
        light
            ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.88f)
            : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f));
}

ComPtr<IDataObject> FileGroupEntryItem::CreateDataObject()
{
    return nullptr;
}

const std::wstring& FileGroupEntryItem::GetChildWidgetId() const
{
    static const std::wstring empty;
    return child_ ? child_->id : empty;
}

const std::vector<std::wstring>&
FileGroup::GetVisibleSourceIds() const
{
    visibleSourceIds_.clear();
    if (!data_ || !app_) return visibleSourceIds_;
    for (const auto& id : data_->childWidgetIds)
    {
        if (auto* scene = GetPreviewScene())
        {
            const DesktopWidget* child = scene->FindWidget(id);
            if (child && IsFileGroupSourceType(child->type))
                visibleSourceIds_.push_back(id);
            continue;
        }
        const size_t index = app_->FindWidgetIndexById(id);
        if (index < app_->widgets_.size() &&
            IsFileGroupSourceType(app_->widgets_[index].type))
            visibleSourceIds_.push_back(id);
    }
    return visibleSourceIds_;
}

std::wstring FileGroup::GetActiveSourceId() const
{
    return snowdesktop::collection_group_rules::ResolveActiveItem(
        GetVisibleSourceIds(),
        data_ ? data_->activeCategoryId : L"");
}

ScrollingItemWidget* FileGroup::GetActiveSourceContainer() const
{
    return GetSourceContainerById(GetActiveSourceId());
}

ScrollingItemWidget* FileGroup::GetSourceContainerById(
    const std::wstring& childId) const
{
    if (!app_ || childId.empty()) return nullptr;
    auto* previewScene = GetPreviewScene();
    DesktopWidget* child = previewScene
        ? previewScene->FindWidget(childId) : nullptr;
    if (!child && !previewScene)
    {
        const size_t childIndex = app_->FindWidgetIndexById(childId);
        if (childIndex >= app_->widgets_.size()) return nullptr;
        child = &app_->widgets_[childIndex];
    }
    if (!child) return nullptr;
    if (!IsFileGroupSourceType(child->type))
        return nullptr;

    auto cached = hostedSourceCache_.find(childId);
    if (cached != hostedSourceCache_.end() &&
        cached->second &&
        cached->second->GetWidgetData() == child)
    {
        cached->second->SetRenderOptions(GetRenderOptions());
        return cached->second.get();
    }

    std::unique_ptr<ScrollingItemWidget> source;
    if (child->type == DesktopWidgetType::FileCategories)
        source = std::make_unique<FileCategories>(
            child, app_);
    else
        source = std::make_unique<FolderMapping>(
            child, app_);
    ScrollingItemWidget* result = source.get();
    result->SetRenderOptions(GetRenderOptions());
    hostedSourceCache_[childId] = std::move(source);
    return result;
}

std::vector<Item*>
FileGroup::GetHostedSelectedItemsForSource(
    const std::wstring& childId) const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    auto* source = GetSourceContainerById(childId);
    if (!source) return result;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    if (!hosted) return result;
    for (Item* item : source->GetSelectedItems())
    {
        auto clone = CloneHostedItem(
            item, const_cast<FileGroup*>(this), app_);
        if (!clone) continue;
        clone->SetBounds(item->GetBounds());
        result.push_back(clone.get());
        dragSourceCache_.push_back(std::move(clone));
    }
    return result;
}

std::vector<std::wstring>
FileGroup::GetHostedVisibleCategoryIds() const
{
    std::vector<std::wstring> result;
    auto* source = GetActiveSourceContainer();
    if (!source) return result;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    if (!hosted) return result;
    if (auto* categories =
            dynamic_cast<FileCategories*>(source))
    {
        const auto& ids =
            categories->CachedVisibleCategoryIds();
        result.assign(ids.begin(), ids.end());
    }
    else if (auto* mapping =
                 dynamic_cast<FolderMapping*>(source))
    {
        const auto& ids =
            mapping->GetVisibleCategoryIds();
        result.assign(ids.begin(), ids.end());
    }
    return result;
}

std::vector<std::wstring>
FileGroup::GetHostedVisibleItemKeys() const
{
    std::vector<std::wstring> result;
    auto* source = GetActiveSourceContainer();
    auto* categories =
        dynamic_cast<FileCategories*>(source);
    if (!categories) return result;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    if (!hosted) return result;
    const auto& keys =
        categories->GetSearchResultKeys();
    result.assign(keys.begin(), keys.end());
    return result;
}

std::vector<size_t>
FileGroup::GetHostedVisibleFolderIndices() const
{
    std::vector<size_t> result;
    auto* source = GetActiveSourceContainer();
    auto* mapping =
        dynamic_cast<FolderMapping*>(source);
    if (!mapping) return result;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    if (!hosted) return result;
    const auto& indices =
        mapping->GetVisibleEntryIndices();
    result.assign(indices.begin(), indices.end());
    return result;
}

bool FileGroup::IsGroupSearchActive() const
{
    return data_ && data_->showSearchBox &&
        !GetSearchText().empty();
}

const std::vector<FileGroup::SearchResultRef>&
FileGroup::GetGroupSearchResults() const
{
    const std::wstring query =
        IsGroupSearchActive() ? GetSearchText() : L"";
    if (groupSearchCacheValid_ &&
        groupSearchCacheQuery_ == query)
        return groupSearchResults_;

    groupSearchResults_.clear();
    groupSearchCacheQuery_ = query;
    groupSearchCacheValid_ = true;
    if (query.empty() || !app_)
        return groupSearchResults_;

    std::unordered_set<std::wstring> desktopKeys;
    for (const auto& sourceId : GetVisibleSourceIds())
    {
        const size_t sourceIndex =
            app_->FindWidgetIndexById(sourceId);
        if (sourceIndex >= app_->widgets_.size())
            continue;
        const DesktopWidget& source =
            app_->widgets_[sourceIndex];
        if (source.type ==
            DesktopWidgetType::FileCategories)
        {
            for (const auto& key : source.itemKeys)
            {
                const size_t itemIndex =
                    app_->FindItemIndexByKey(key);
                if (itemIndex >= app_->items_.size())
                    continue;
                const DesktopItem& item =
                    app_->items_[itemIndex];
                if (!NameMatchesQuery(item.name, query))
                    continue;
                const std::wstring normalized =
                    ToUpperInvariant(item.layoutKey);
                if (!desktopKeys.insert(normalized).second)
                    continue;
                SearchResultRef result;
                result.sourceId = sourceId;
                result.folderMapping = false;
                result.desktopKey = item.layoutKey;
                groupSearchResults_.push_back(
                    std::move(result));
            }
        }
        else if (source.type ==
                 DesktopWidgetType::FolderMapping)
        {
            for (size_t i = 0;
                 i < source.folderEntries.size(); ++i)
            {
                if (!NameMatchesQuery(
                        source.folderEntries[i].name,
                        query))
                    continue;
                SearchResultRef result;
                result.sourceId = sourceId;
                result.folderMapping = true;
                result.folderEntryIndex = i;
                groupSearchResults_.push_back(
                    std::move(result));
            }
        }
    }
    return groupSearchResults_;
}

Item* FileGroup::CreateGroupSearchItem(
    size_t index, bool dragCache) const
{
    if (!app_) return nullptr;
    const auto& results = GetGroupSearchResults();
    if (index >= results.size()) return nullptr;
    const SearchResultRef& result = results[index];
    std::unique_ptr<Item> item;
    if (!result.folderMapping)
    {
        const size_t desktopIndex =
            app_->FindItemIndexByKey(result.desktopKey);
        if (desktopIndex >= app_->items_.size())
            return nullptr;
        item = std::make_unique<DesktopIcon>(
            &app_->items_[desktopIndex],
            const_cast<FileGroup*>(this), app_);
    }
    else
    {
        const size_t sourceIndex =
            app_->FindWidgetIndexById(result.sourceId);
        if (sourceIndex >= app_->widgets_.size() ||
            result.folderEntryIndex >=
                app_->widgets_[sourceIndex].
                    folderEntries.size())
            return nullptr;
        item = std::make_unique<FolderEntryIcon>(
            &app_->widgets_[sourceIndex].
                folderEntries[result.folderEntryIndex],
            const_cast<FileGroup*>(this), app_);
    }
    Item* raw = item.get();
    if (dragCache)
        dragSourceCache_.push_back(std::move(item));
    else
        slotItemCache_.push_back(std::move(item));
    return raw;
}

ScrollingItemWidget*
FileGroup::GetSourceContainerForItem(
    const Item* item) const
{
    if (!item) return nullptr;
    if (!IsGroupSearchActive())
        return GetActiveSourceContainer();

    const auto& results = GetGroupSearchResults();
    if (const auto* desktop =
            dynamic_cast<const DesktopIcon*>(item))
    {
        const DesktopItem* sourceItem =
            desktop->GetDesktopItem();
        if (!sourceItem) return nullptr;
        const std::wstring key =
            ToUpperInvariant(sourceItem->layoutKey);
        for (const auto& result : results)
            if (!result.folderMapping &&
                ToUpperInvariant(result.desktopKey) == key)
                return GetSourceContainerById(
                    result.sourceId);
    }
    else if (const auto* folder =
                 dynamic_cast<const FolderEntryIcon*>(item))
    {
        const FolderEntry* sourceEntry =
            folder->GetFolderEntry();
        for (const auto& result : results)
        {
            if (!result.folderMapping)
                continue;
            const size_t sourceIndex =
                app_->FindWidgetIndexById(
                    result.sourceId);
            if (sourceIndex >= app_->widgets_.size() ||
                result.folderEntryIndex >=
                    app_->widgets_[sourceIndex].
                        folderEntries.size())
                continue;
            if (&app_->widgets_[sourceIndex].
                    folderEntries[
                        result.folderEntryIndex] ==
                sourceEntry)
                return GetSourceContainerById(
                    result.sourceId);
        }
    }
    return GetActiveSourceContainer();
}

void FileGroup::InvalidateHostedView()
{
    visibleSourceIds_.clear();
    groupSearchCacheValid_ = false;
    groupSearchResults_.clear();
    dropPreviewValid_ = false;
    dropPreviewSourceTab_ = false;
    InvalidateSlots();
    for (auto& [id, source] : hostedSourceCache_)
    {
        if (!source) continue;
        if (auto* categories =
                dynamic_cast<FileCategories*>(source.get()))
            categories->InvalidateCategoryCache();
        else if (auto* mapping =
                     dynamic_cast<FolderMapping*>(source.get()))
            mapping->InvalidateFilterCache();
        else
            source->InvalidateSlots();
    }
}

RECT FileGroup::GetSearchBoxRect() const
{
    return GetCategorizedSearchBoxRect(
        data_ && data_->showSearchBox);
}

std::wstring FileGroup::SourceIdAtPoint(POINT pt) const
{
    const size_t index = FileGroupSourceTabIndexAtPoint(
        const_cast<FileGroup*>(this), pt);
    const auto& sources = GetVisibleSourceIds();
    return index < sources.size() ? sources[index] : L"";
}

std::wstring FileGroup::CategoryIdAtPoint(POINT pt) const
{
    auto* source = GetActiveSourceContainer();
    if (!source) return L"";
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted ? source->CategoryIdAtPoint(pt) : L"";
}

RECT FileGroup::GetSourceTabRectById(
    const std::wstring& childId) const
{
    const auto& sources = GetVisibleSourceIds();
    auto it = std::find(
        sources.begin(), sources.end(), childId);
    return it == sources.end()
        ? RECT{}
        : FileGroupSourceTabRect(
            const_cast<FileGroup*>(this),
            static_cast<size_t>(std::distance(
                sources.begin(), it)));
}

void FileGroup::EnsureSourceTabVisible(size_t tabIndex)
{
    if (!data_) return;
    RECT tabs = FileGroupSourceTabsRect(this);
    const auto& sources = GetVisibleSourceIds();
    if (IsRectEmptyRect(tabs) || tabIndex >= sources.size()) return;
    const std::vector<int> widths =
        FileGroupSourceTabWidths(
            this, tabs.right - tabs.left);
    LONG rawLeft = tabs.left;
    for (size_t i = 0; i < tabIndex; ++i)
        rawLeft += widths[i];
    LONG rawRight = rawLeft + widths[tabIndex];
    const int viewport = tabs.right - tabs.left;
    if (rawLeft - data_->tabScrollOffset < tabs.left)
        data_->tabScrollOffset =
            std::max<int>(0, rawLeft - tabs.left);
    else if (rawRight - data_->tabScrollOffset > tabs.right)
        data_->tabScrollOffset =
            std::max<int>(0, rawRight - tabs.right);
    data_->tabScrollOffset =
        snowdesktop::collection_group_rules::
            ClampIndependentTabScroll(
                data_->tabScrollOffset,
                TotalTabWidth(widths), viewport);
}

FileGroupEntryItem*
FileGroup::GetSourceTabItemAtPoint(POINT pt) const
{
    const size_t tabIndex =
        FileGroupSourceTabIndexAtPoint(
            const_cast<FileGroup*>(this), pt);
    const auto& sources = GetVisibleSourceIds();
    if (!app_ || tabIndex >= sources.size()) return nullptr;
    const size_t childIndex =
        app_->FindWidgetIndexById(sources[tabIndex]);
    if (childIndex >= app_->widgets_.size()) return nullptr;
    sourceTabItemCache_ =
        std::make_unique<FileGroupEntryItem>(
            &app_->widgets_[childIndex],
            const_cast<FileGroup*>(this), app_);
    sourceTabItemCache_->SetBounds(
        FileGroupSourceTabRect(
            const_cast<FileGroup*>(this), tabIndex));
    return sourceTabItemCache_.get();
}

std::vector<std::unique_ptr<Slot>> FileGroup::BuildSlots()
{
    slotItemCache_.clear();
    std::vector<std::unique_ptr<Slot>> result;
    if (IsGroupSearchActive())
    {
        const auto& results = GetGroupSearchResults();
        if (results.empty()) return result;
        RECT content = GetContentViewportRect();
        const int visibleHeight = std::max<int>(
            1, content.bottom - content.top);
        const int scroll = std::clamp(
            data_->scrollOffset, 0,
            GetMaxScrollOffset());
        size_t first = 0;
        size_t last = results.size();
        if (data_->listMode)
        {
            const int height = std::max(
                1, Cu(38.0f));
            first = static_cast<size_t>(
                std::max(0, scroll / height - 1));
            last = std::min(
                results.size(),
                static_cast<size_t>(
                    (scroll + visibleHeight +
                     height - 1) / height + 1));
        }
        else
        {
            const int columns =
                std::max(1, data_->gridSpan.columns);
            const int height = std::max(
                1, FileGroupSearchCellHeight(this));
            const int firstRow =
                std::max(0, scroll / height - 1);
            const int lastRow =
                (scroll + visibleHeight +
                 height - 1) / height + 1;
            first = std::min(
                results.size(),
                static_cast<size_t>(
                    firstRow * columns));
            last = std::min(
                results.size(),
                static_cast<size_t>(
                    lastRow * columns));
        }
        result.reserve(last - first);
        for (size_t index = first;
             index < last; ++index)
        {
            RECT cell =
                FileGroupSearchItemRect(this, index);
            auto slot = std::make_unique<Slot>(
                this, cell, index);
            Item* item =
                CreateGroupSearchItem(index, false);
            if (item) item->SetBounds(cell);
            slot->SetItem(item);
            result.push_back(std::move(slot));
        }
        return result;
    }

    auto* source = GetActiveSourceContainer();
    if (!source) return result;
    HostedFileSourceScope hosted(this, source);
    if (!hosted) return result;

    std::vector<std::unique_ptr<Slot>> sourceSlots =
        source->BuildSlots();
    result.reserve(sourceSlots.size());
    for (const auto& sourceSlot : sourceSlots)
    {
        if (!sourceSlot) continue;
        auto slot = std::make_unique<Slot>(
            source, sourceSlot->GetBounds(),
            sourceSlot->GetIndex());
        if (auto clone = CloneHostedItem(
                sourceSlot->GetItem(), this, app_))
        {
            clone->SetBounds(sourceSlot->GetBounds());
            Item* raw = clone.get();
            slotItemCache_.push_back(std::move(clone));
            slot->SetItem(raw);
        }
        result.push_back(std::move(slot));
    }
    return result;
}

size_t FileGroup::GetSlotCount() const
{
    if (IsGroupSearchActive())
        return GetGroupSearchResults().size();
    auto* source = GetActiveSourceContainer();
    if (!source) return 0;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted ? source->GetSlotCount() : 0;
}

int FileGroup::GetItemHeight() const
{
    if (IsGroupSearchActive())
        return data_ && data_->listMode
            ? Cu(38.0f)
            : FileGroupSearchCellHeight(
                const_cast<FileGroup*>(this));
    auto* source = GetActiveSourceContainer();
    if (!source) return Cu(38.0f);
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted ? source->GetItemHeight() : Cu(38.0f);
}

int FileGroup::GetItemWidth() const
{
    if (IsGroupSearchActive())
    {
        RECT content = GetContentViewportRect();
        return data_ && data_->listMode
            ? std::max<int>(
                1, content.right - content.left)
            : std::max<int>(
                1, (content.right - content.left) /
                    std::max(
                        1, data_->gridSpan.columns));
    }
    auto* source = GetActiveSourceContainer();
    if (!source) return 1;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted ? source->GetItemWidth() : 1;
}

bool FileGroup::SingleColumn() const
{
    return data_ && data_->listMode;
}

Item* FileGroup::GetSlotItem(size_t idx) const
{
    const auto& slots =
        const_cast<FileGroup*>(this)->GetSlots();
    for (const auto& slot : slots)
        if (slot && slot->GetIndex() == idx)
            return slot->GetItem();
    return nullptr;
}

Item* FileGroup::GetMemberItem(size_t idx) const
{
    if (IsGroupSearchActive())
        return CreateGroupSearchItem(idx, true);
    Item* item = GetSlotItem(idx);
    auto* source = GetActiveSourceContainer();
    if (!item || !source) return nullptr;
    auto clone = CloneHostedItem(
        item, const_cast<FileGroup*>(this), app_);
    if (!clone) return nullptr;
    clone->SetBounds(item->GetBounds());
    Item* raw = clone.get();
    dragSourceCache_.push_back(std::move(clone));
    return raw;
}

std::vector<Item*> FileGroup::GetSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> result;
    if (!data_ || !app_) return result;

    const auto& sources = GetVisibleSourceIds();
    if (IsGroupSearchActive())
    {
        const auto& results = GetGroupSearchResults();
        for (size_t i = 0; i < results.size(); ++i)
        {
            Item* item = CreateGroupSearchItem(i, true);
            if (item && item->IsSelected())
                result.push_back(item);
        }
        return result;
    }
    for (size_t i = 0; i < sources.size(); ++i)
    {
        const size_t childIndex =
            app_->FindWidgetIndexById(sources[i]);
        if (childIndex >= app_->widgets_.size() ||
            !app_->widgets_[childIndex].selected)
            continue;
        auto entry = std::make_unique<FileGroupEntryItem>(
            &app_->widgets_[childIndex],
            const_cast<FileGroup*>(this), app_);
        entry->SetBounds(
            FileGroupSourceTabRect(
                const_cast<FileGroup*>(this), i));
        Item* raw = entry.get();
        dragSourceCache_.push_back(std::move(entry));
        result.push_back(raw);
    }
    if (!result.empty()) return result;

    auto* source = GetActiveSourceContainer();
    if (!source) return result;
    const auto& slots =
        const_cast<FileGroup*>(this)->GetSlots();
    for (const auto& slot : slots)
    {
        if (!slot || !slot->GetItem() ||
            !slot->GetItem()->IsSelected())
            continue;
        auto clone = CloneHostedItem(
            slot->GetItem(),
            const_cast<FileGroup*>(this), app_);
        if (!clone) continue;
        clone->SetBounds(slot->GetBounds());
        Item* raw = clone.get();
        dragSourceCache_.push_back(std::move(clone));
        result.push_back(raw);
    }
    return result;
}

std::vector<size_t>
FileGroup::GetSelectedMemberIndices() const
{
    std::vector<size_t> result;
    if (!data_ || !app_) return result;
    if (IsGroupSearchActive())
    {
        const auto& results = GetGroupSearchResults();
        for (size_t i = 0; i < results.size(); ++i)
        {
            Item* item =
                CreateGroupSearchItem(i, true);
            if (item && item->IsSelected())
                result.push_back(i);
        }
        return result;
    }
    const auto& sources = GetVisibleSourceIds();
    for (size_t i = 0; i < sources.size(); ++i)
    {
        const size_t childIndex =
            app_->FindWidgetIndexById(sources[i]);
        if (childIndex < app_->widgets_.size() &&
            app_->widgets_[childIndex].selected)
            result.push_back(i);
    }
    return result;
}

void FileGroup::ReorderMembers(
    const std::vector<size_t>& indices,
    size_t insertBefore)
{
    if (!data_ || indices.empty()) return;
    if (IsGroupSearchActive()) return;
    data_->childWidgetIds =
        snowdesktop::collection_group_rules::ReorderItems(
            data_->childWidgetIds, indices, insertBefore);
    InvalidateHostedView();
}

size_t FileGroup::GetDropInsertIndex(
    Slot* targetSlot, HitRegion region) const
{
    if (!data_) return 0;
    if (targetSlot && targetSlot == sourceTabDropSlot_.get())
    {
        if (app_ &&
            app_->dragSession_.SourceList().
                UsesFileGroupSourceInsertion())
        {
            size_t index = std::min(
                targetSlot->GetIndex(),
                data_->childWidgetIds.size());
            if (region == HitRegion::SortAfter)
                index = std::min(
                    index + 1,
                    data_->childWidgetIds.size());
            return index;
        }
        auto* source = GetActiveSourceContainer();
        if (!source) return 0;
        HostedFileSourceScope hosted(
            const_cast<FileGroup*>(this), source);
        return hosted
            ? source->GetDropInsertIndex(
                nullptr, HitRegion::SortAfter)
            : 0;
    }
    if (IsGroupSearchActive())
    {
        auto* source = GetActiveSourceContainer();
        if (!source) return 0;
        HostedFileSourceScope hosted(
            const_cast<FileGroup*>(this), source);
        return hosted
            ? source->GetDropInsertIndex(
                nullptr, HitRegion::SortAfter)
            : 0;
    }
    auto* source = GetActiveSourceContainer();
    if (!source) return 0;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted
        ? source->GetDropInsertIndex(targetSlot, region)
        : 0;
}

int FileGroup::GetTotalContentHeight() const
{
    if (IsGroupSearchActive())
        return FileGroupSearchContentHeight(
            const_cast<FileGroup*>(this),
            GetGroupSearchResults().size());
    auto* source = GetActiveSourceContainer();
    if (!source) return 0;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted ? source->GetTotalContentHeight() : 0;
}

int FileGroup::GetVisibleContentHeight() const
{
    RECT content = GetContentViewportRect();
    return std::max<int>(0, content.bottom - content.top);
}

int FileGroup::GetMaxScrollOffset() const
{
    if (IsGroupSearchActive())
    {
        const int visible =
            GetVisibleContentHeight();
        return std::max(
            0, GetTotalContentHeight() -
                visible +
                Cu(kMinCellHeight / 2.0f));
    }
    auto* source = GetActiveSourceContainer();
    if (!source) return 0;
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted ? source->GetMaxScrollOffset() : 0;
}

RECT FileGroup::GetContentViewportRect() const
{
    if (IsGroupSearchActive())
    {
        RECT body = GetBodyRect();
        InflateRect(
            &body, -Cu(4.0f), -Cu(8.0f));
        RECT search = GetSearchBoxRect();
        if (!IsRectEmptyRect(search))
            body.top = std::min<LONG>(
                body.bottom,
                search.bottom + Cu(4.0f));
        return body;
    }
    auto* source = GetActiveSourceContainer();
    if (!source)
    {
        RECT body = GetBodyRect();
        InflateRect(
            &body, -Cu(4.0f), -Cu(8.0f));
        RECT search = GetSearchBoxRect();
        if (!IsRectEmptyRect(search))
            body.top = std::min<LONG>(
                body.bottom, search.bottom + Cu(4.0f));
        RECT tabs = FileGroupSourceTabsRect(
            const_cast<FileGroup*>(this));
        if (!IsRectEmptyRect(tabs))
            body.top = std::min<LONG>(
                body.bottom, tabs.bottom + Cu(8.0f));
        return body;
    }
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted
        ? source->GetContentViewportRect()
        : RECT{};
}

void FileGroup::ApplyMarqueeSelection(
    const RECT& contentRect)
{
    if (IsGroupSearchActive())
    {
        const auto& results = GetGroupSearchResults();
        for (const auto& result : results)
        {
            if (!result.folderMapping)
            {
                const size_t index =
                    app_->FindItemIndexByKey(
                        result.desktopKey);
                if (index < app_->items_.size())
                    app_->items_[index].selected = false;
            }
            else
            {
                const size_t sourceIndex =
                    app_->FindWidgetIndexById(
                        result.sourceId);
                if (sourceIndex <
                        app_->widgets_.size() &&
                    result.folderEntryIndex <
                        app_->widgets_[sourceIndex].
                            folderEntries.size())
                    app_->widgets_[sourceIndex].
                        folderEntries[
                            result.folderEntryIndex].
                        selected = false;
            }
        }
        const int scroll = GetScrollOffset();
        for (size_t i = 0;
             i < results.size(); ++i)
        {
            RECT item =
                FileGroupSearchItemRect(this, i);
            OffsetRect(&item, 0, scroll);
            if (!RectsIntersect(item, contentRect))
                continue;
            Item* selected =
                CreateGroupSearchItem(i, true);
            if (selected)
                selected->SetSelected(true);
        }
        return;
    }
    auto* source = GetActiveSourceContainer();
    if (!source) return;
    HostedFileSourceScope hosted(this, source);
    if (hosted)
        source->ApplyMarqueeSelection(contentRect);
}

bool FileGroup::TryScrollTabs(POINT pt, int delta)
{
    if (!data_ || !app_) return false;
    RECT sourceTabs = FileGroupSourceTabsRect(this);
    if (!IsRectEmptyRect(sourceTabs) &&
        PtInRect(&sourceTabs, pt))
    {
        const auto widths = FileGroupSourceTabWidths(
            this, sourceTabs.right - sourceTabs.left);
        const int maxScroll = std::max(
            0, TotalTabWidth(widths) -
                static_cast<int>(
                    sourceTabs.right - sourceTabs.left));
        if (maxScroll <= 0) return false;
        const int old = data_->tabScrollOffset;
        data_->tabScrollOffset =
            snowdesktop::collection_group_rules::
                ClampIndependentTabScroll(
                    old - delta / 2,
                    TotalTabWidth(widths),
                    sourceTabs.right -
                        sourceTabs.left);
        if (old != data_->tabScrollOffset)
        {
            InvalidateHostedView();
            InvalidateRect(app_->hwnd_, nullptr, FALSE);
            return true;
        }
        return false;
    }

    auto* source = GetActiveSourceContainer();
    if (!source) return false;
    HostedFileSourceScope hosted(this, source);
    return hosted && source->TryScrollTabs(pt, delta);
}

WidgetHit FileGroup::HitTestWidget(POINT pt) const
{
    WidgetHit base = WidgetContainer::HitTestWidget(pt);
    if (base == WidgetHit::None) return base;
    RECT search = GetSearchBoxRect();
    if (!IsRectEmptyRect(search) && PtInRect(&search, pt))
        return WidgetHit::SearchBox;
    if (!SourceIdAtPoint(pt).empty())
        return WidgetHit::SourceTab;

    auto* source = GetActiveSourceContainer();
    if (source)
    {
        HostedFileSourceScope hosted(
            const_cast<FileGroup*>(this), source);
        if (hosted)
        {
            WidgetHit childHit = source->HitTestWidget(pt);
            if (childHit == WidgetHit::CategoryTab ||
                childHit == WidgetHit::DateHeaderToggleBtn ||
                childHit == WidgetHit::ListToggleBtn ||
                childHit == WidgetHit::OpenFolderBtn)
                return childHit;
        }
    }
    return base;
}

HitRegion FileGroup::HitTestDrag(
    POINT pt, Slot*& outSlot)
{
    outSlot = nullptr;
    dropPreviewValid_ = false;
    dropPreviewSourceTab_ = false;
    RECT frame = GetFrameRect();
    if (!PtInRect(&frame, pt))
        return HitRegion::None;

    const size_t sourceTab =
        FileGroupSourceTabIndexAtPoint(this, pt);
    if (sourceTab != static_cast<size_t>(-1))
    {
        RECT tab = FileGroupSourceTabRect(this, sourceTab);
        sourceTabDropSlot_ = std::make_unique<Slot>(
            this, tab, sourceTab);
        outSlot = sourceTabDropSlot_.get();
        dropPreviewBounds_ = tab;
        dropPreviewIndex_ = sourceTab;
        dropPreviewValid_ = true;
        dropPreviewSourceTab_ = true;
        return pt.x < tab.left +
                (tab.right - tab.left) / 2
            ? HitRegion::SortBefore
            : HitRegion::SortAfter;
    }

    if (app_ &&
        app_->dragSession_.SourceList().
            UsesFileGroupSourceInsertion())
        return HitRegion::Empty;

    if (IsGroupSearchActive())
    {
        Slot* searchSlot = nullptr;
        const HitRegion result =
            WidgetContainer::HitTestDrag(
                pt, searchSlot);
        if (!searchSlot) return result;
        hostedDropItem_.reset();
        hostedDropSlot_ = std::make_unique<Slot>(
            this, searchSlot->GetBounds(),
            searchSlot->GetIndex());
        if (auto item = CloneHostedItem(
                searchSlot->GetItem(), this, app_))
        {
            item->SetBounds(
                searchSlot->GetBounds());
            hostedDropItem_ = std::move(item);
            hostedDropSlot_->SetItem(
                hostedDropItem_.get());
        }
        dropPreviewBounds_ =
            searchSlot->GetBounds();
        dropPreviewIndex_ =
            searchSlot->GetIndex();
        dropPreviewValid_ = true;
        outSlot = hostedDropSlot_.get();
        return result;
    }

    auto* source = GetActiveSourceContainer();
    if (!source) return HitRegion::Empty;
    HostedFileSourceScope hosted(this, source);
    if (!hosted) return HitRegion::Empty;

    Slot* sourceSlot = nullptr;
    const HitRegion result =
        source->HitTestDrag(pt, sourceSlot);
    if (!sourceSlot)
        return result;

    hostedDropItem_.reset();
    hostedDropSlot_ = std::make_unique<Slot>(
        this, sourceSlot->GetBounds(),
        sourceSlot->GetIndex());
    if (auto item = CloneHostedItem(
            sourceSlot->GetItem(), this, app_))
    {
        item->SetBounds(sourceSlot->GetBounds());
        hostedDropItem_ = std::move(item);
        hostedDropSlot_->SetItem(
            hostedDropItem_.get());
    }
    dropPreviewBounds_ = sourceSlot->GetBounds();
    dropPreviewIndex_ = sourceSlot->GetIndex();
    dropPreviewValid_ = true;
    outSlot = hostedDropSlot_.get();
    return result;
}

void FileGroup::DrawDropPreview(
    ID2D1DeviceContext* context, Slot* slot,
    HitRegion region)
{
    if (!context) return;
    if (app_ &&
        app_->dragSession_.SourceList().
            UsesFileGroupSourceInsertion())
    {
        if (dropPreviewSourceTab_ &&
            dropPreviewValid_ &&
            (region == HitRegion::SortBefore ||
             region == HitRegion::SortAfter))
        {
            const RECT bounds = dropPreviewBounds_;
            const float width =
                static_cast<float>(Cu(3.0f));
            const float x =
                static_cast<float>(
                    region == HitRegion::SortBefore
                        ? bounds.left
                        : bounds.right) -
                width / 2.0f;
            ComPtr<ID2D1SolidColorBrush> brush;
            if (SUCCEEDED(
                    context->CreateSolidColorBrush(
                        D2D1::ColorF(
                            0.39f, 0.66f, 1.0f,
                            0.92f),
                        &brush)) &&
                brush)
            {
                context->FillRectangle(
                    D2D1::RectF(
                        x,
                        static_cast<float>(
                            bounds.top + Cu(2.0f)),
                        x + width,
                        static_cast<float>(
                            bounds.bottom - Cu(2.0f))),
                    brush.Get());
            }
            return;
        }
        RECT target = GetFrameRect();
        InflateRect(&target, Cu(3.0f), Cu(3.0f));
        app_->DrawD2DRoundedRectangle(
            context, target, static_cast<float>(Cu(10.0f)),
            D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.14f),
            D2D1::ColorF(1.0f, 0.72f, 0.12f, 0.92f),
            static_cast<float>(Cu(2.5f)));
        return;
    }
    if (dropPreviewValid_)
    {
        Slot stableSlot(
            this, dropPreviewBounds_,
            dropPreviewIndex_);
        const float itemPad = SingleColumn()
            ? static_cast<float>(Cu(2.0f))
            : 0.0f;
        stableSlot.DrawDropIndicator(
            context, region, itemPad);
        return;
    }
    (void)slot;
}

std::wstring FileGroup::GetDragHint(
    Slot* slot, HitRegion region,
    const std::vector<Item*>& sourceItems,
    Container* origin, int mods) const
{
    if (region == HitRegion::None ||
        region == HitRegion::Blocked ||
        sourceItems.empty())
        return L"";
    const bool allSources = std::all_of(
        sourceItems.begin(), sourceItems.end(),
        [](Item* item) {
            return dynamic_cast<FileGroupEntryItem*>(item) != nullptr;
        });
    if (allSources)
        return origin == this
            ? _LW("widget.file_group.reorder_hint")
            : _LW("widget.file_group.add_hint");
    auto* source = GetActiveSourceContainer();
    if (!source)
        return WidgetContainer::GetDragHint(
            slot, region, sourceItems, origin, mods);
    if (data_ && data_->dateHeaders && app_ &&
        app_->dragSession_.SourceList().origin == source &&
        app_->dragSession_.Action() == DropAction::Move)
        return _LW("widget.desktop.sort_after_date");
    HostedFileSourceScope hosted(
        const_cast<FileGroup*>(this), source);
    return hosted
        ? source->GetDragHint(
            slot, region, sourceItems, origin, mods)
        : L"";
}

void FileGroup::OnItemsDropped(
    const std::vector<Item*>& sourceItems,
    Container* origin, Slot* targetSlot,
    HitRegion region, int mods)
{
    if (!app_ || !data_ || sourceItems.empty()) return;
    const bool allFolderMappings =
        std::all_of(
            sourceItems.begin(),
            sourceItems.end(),
            [&](Item* item) {
                std::wstring id;
                if (auto* dockItem =
                        dynamic_cast<
                            DockEntryItem*>(item))
                {
                    if (dockItem->GetEntryType() !=
                            DockEntryType::
                                FolderMapping)
                        return false;
                    id = dockItem->GetReference();
                }
                else if (auto* groupEntry =
                             dynamic_cast<
                                 FileGroupEntryItem*>(
                                 item))
                {
                    id =
                        groupEntry->
                            GetChildWidgetId();
                }
                else
                {
                    return false;
                }
                const size_t childIndex =
                    app_->FindWidgetIndexById(id);
                return childIndex <
                        app_->widgets_.size() &&
                    app_->widgets_[childIndex].
                        type ==
                        DesktopWidgetType::
                            FolderMapping;
            });
    if (allFolderMappings)
    {
        size_t insertIndex =
            GetDropInsertIndex(
                targetSlot, region);
        if (SourceIdAtPoint(
                app_->dragSession_.
                    CurrentPoint()).empty())
            insertIndex =
                data_->childWidgetIds.size();
        const size_t groupIndex =
            app_->FindWidgetIndexById(
                data_->id);
        app_->MoveFolderMappingsToFileGroup(
            sourceItems, groupIndex,
            insertIndex);
        return;
    }

    const bool allSources = std::all_of(
        sourceItems.begin(), sourceItems.end(),
        [](Item* item) {
            return dynamic_cast<FileGroupEntryItem*>(item) != nullptr;
        });
    if (allSources)
    {
        const DragSourceList& session =
            app_->dragSession_.SourceList();
        DragSourceList sourceList =
            snowdesktop::collection_group_rules::
                SelectDragSource(
                    !session.Empty(),
                    app_->dragSession_.Source() ==
                        origin) ==
                snowdesktop::collection_group_rules::
                    DragSourceSelection::Captured
            ? session
            : app_->BuildDragSourceList(sourceItems, origin);
        DropPreviewList preview =
            app_->BuildDropPreviewList(
                sourceList, this, targetSlot, region,
                mods, app_->dragSession_.CurrentPoint());
        if (SourceIdAtPoint(
                app_->dragSession_.CurrentPoint()).empty())
        {
            preview.insertIndex = data_->childWidgetIds.size();
            for (size_t i = 0; i < preview.landings.size(); ++i)
                preview.landings[i].insertIndex =
                    preview.insertIndex + i;
        }
        // ExecuteDropPipeline may call LayoutItems(), which rebuilds the
        // runtime container tree and destroys this FileGroup.  Treat the
        // pipeline call as terminal and never access this afterwards.
        app_->ExecuteDropPipeline(sourceList, preview);
        return;
    }

    auto* source = GetActiveSourceContainer();
    if (!source) return;
    DesktopApp* app = app_;
    DragSourceList sourceList;
    DropPreviewList preview;
    {
        // Restore the hosted child before executing the drop.  The pipeline
        // may rebuild all runtime containers, so the scope must not outlive
        // the call boundary.
        HostedFileSourceScope hosted(this, source);
        if (!hosted) return;
        const DragSourceList& session =
            app->dragSession_.SourceList();
        sourceList =
            snowdesktop::collection_group_rules::
                SelectDragSource(
                    !session.Empty(),
                    app->dragSession_.Source() ==
                        origin) ==
                snowdesktop::collection_group_rules::
                    DragSourceSelection::Captured
            ? session
            : app->BuildDragSourceList(
                sourceItems, origin);
        preview =
            app->BuildDropPreviewList(
                sourceList, source,
                IsGroupSearchActive()
                    ? nullptr : targetSlot,
                region, mods,
                app->dragSession_.CurrentPoint());
    }
    // This must remain the final operation: executing can invalidate source,
    // this FileGroup, and every other runtime container.
    app->ExecuteDropPipeline(sourceList, preview);
}

void FileGroup::DrawContent(
    ID2D1DeviceContext* context, RECT)
{
    if (!data_ || !app_ || !context) return;
    if (IsGroupSearchActive())
    {
        DrawSearchBox(context);
        const auto& results =
            GetGroupSearchResults();
        RECT content = GetContentViewportRect();
        const bool light =
            app_->IsLightContentTheme();
        if (results.empty())
        {
            IDWriteTextFormat* centered =
                GetCuTextFormat(13.0f, false, true);
            app_->DrawD2DText(
                context,
                _LW("widget.categories.no_results"),
                content,
                centered ? centered :
                    app_->listItemTextFormat_.Get(),
                light
                    ? D2D1::ColorF(
                        0.0f, 0.0f, 0.0f, 0.68f)
                    : D2D1::ColorF(
                        1.0f, 1.0f, 1.0f, 0.66f),
                DWRITE_WORD_WRAPPING_WRAP);
            return;
        }

        const bool privacyActive =
            data_->privacyMode &&
            !app_->dragSession_.IsActive() &&
            !app_->dragDropController_.IsExternalDragActive() &&
            !PtInRect(
                &data_->bounds,
                app_->lastMousePoint_);
        context->PushAxisAlignedClip(
            app_->ToD2DRect(content),
            D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        for (const auto& slot : GetSlots())
        {
            if (!slot || !slot->GetItem())
                continue;
            RECT cell = slot->GetBounds();
            if (cell.bottom <= content.top ||
                cell.top >= content.bottom)
                continue;
            Item* item = slot->GetItem();
            bool isDirectory = false;
            bool iconIsMediaThumbnail = false;
            int sysIconIndex = -1;
            if (auto* desktop =
                    dynamic_cast<DesktopIcon*>(item))
            {
                DesktopItem* sourceItem =
                    desktop->GetDesktopItem();
                if (sourceItem)
                {
                    sysIconIndex =
                        sourceItem->sysIconIndex;
                    iconIsMediaThumbnail =
                        sourceItem->iconIsMediaThumbnail;
                }
            }
            else if (auto* folder =
                         dynamic_cast<
                             FolderEntryIcon*>(item))
            {
                FolderEntry* sourceEntry =
                    folder->GetFolderEntry();
                if (sourceEntry)
                {
                    isDirectory =
                        sourceEntry->isDirectory;
                    sysIconIndex =
                        sourceEntry->sysIconIndex;
                    iconIsMediaThumbnail =
                        sourceEntry->iconIsMediaThumbnail;
                }
            }
            if (privacyActive)
                DrawPrivacyPlaceholder(
                    context, cell,
                    item->GetTitle(), isDirectory);
            else if (data_->listMode)
                DrawListItem(
                    context, cell,
                    item->GetIconBitmap(),
                    sysIconIndex,
                    item->GetTitle(),
                    item->IsSelected(),
                    iconIsMediaThumbnail);
            else
            {
                const bool hovered =
                    !item->IsSelected() &&
                    PtInRect(
                        &cell,
                        app_->lastMousePoint_);
                item->Draw(
                    context, cell,
                    item->IsSelected()
                        ? 2 : (hovered ? 1 : 0));
            }
        }
        context->PopAxisAlignedClip();
        return;
    }

    auto* source = GetActiveSourceContainer();
    if (source)
    {
        HostedFileSourceScope hosted(this, source);
        if (hosted)
            source->DrawContent(context, source->GetBodyRect());
    }
    else
    {
        DrawSearchBox(context);
        RECT content = GetContentViewportRect();
        IDWriteTextFormat* centered =
            GetCuTextFormat(13.0f, false, true);
        app_->DrawD2DText(
            context, _LW("widget.file_group.empty"),
            content,
            centered ? centered :
                app_->listItemTextFormat_.Get(),
            app_->IsLightContentTheme()
                ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.68f)
                : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.66f),
            DWRITE_WORD_WRAPPING_WRAP);
    }

    const auto& sources = GetVisibleSourceIds();
    RECT tabs = FileGroupSourceTabsRect(this);
    if (IsRectEmptyRect(tabs)) return;
    const std::wstring active = GetActiveSourceId();
    context->PushAxisAlignedClip(
        app_->ToD2DRect(tabs),
        D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < sources.size(); ++i)
    {
        RECT layoutTab =
            FileGroupSourceTabLayoutRect(this, i);
        RECT tab = FileGroupSourceTabRect(this, i);
        if (IsRectEmptyRect(layoutTab) ||
            IsRectEmptyRect(tab))
            continue;
        DrawCategorizedTab(
            context, tab, layoutTab,
            FileGroupSourceTabText(this, i),
            sources[i] == active,
            !IsPreviewRendering() &&
                PtInRect(&tab, app_->lastMousePoint_) != FALSE);
    }
    context->PopAxisAlignedClip();
}

void FileGroup::DrawButtons(
    ID2D1DeviceContext* context, RECT, bool)
{
    if (!data_ || !app_ || !context) return;
    const std::wstring activeId = GetActiveSourceId();
    const DesktopWidget* previewSource = GetPreviewScene()
        ? GetPreviewScene()->FindWidget(activeId) : nullptr;
    const size_t sourceIndex = previewSource
        ? static_cast<size_t>(-1)
        : app_->FindWidgetIndexById(activeId);
    const bool includeOpen = previewSource
        ? previewSource->type == DesktopWidgetType::FolderMapping
        : (sourceIndex < app_->widgets_.size() &&
            app_->widgets_[sourceIndex].type ==
                DesktopWidgetType::FolderMapping);
    const FileGroupButtonRects buttons =
        GetFileGroupButtonRects(this, includeOpen);
    const bool light = app_->IsLightContentTheme();
    IDWriteTextFormat* format =
        GetCuFluentTextFormat(14.0f * GetBarScale());
    IDWriteTextFormat* fallback = format
        ? format
        : (app_->fluentIconTextFormat_
            ? app_->fluentIconTextFormat_.Get()
            : app_->listItemTextFormat_.Get());
    auto draw = [&](RECT rect, const wchar_t* glyph, bool active) {
        if (IsRectEmptyRect(rect)) return;
        const bool hot =
            !IsPreviewRendering() &&
            PtInRect(&rect, app_->lastMousePoint_) != FALSE;
        app_->DrawD2DText(
            context, glyph, rect, fallback,
            light
                ? (active
                    ? D2D1::ColorF(
                        0.10f, 0.12f, 0.16f,
                        hot ? 0.85f : 0.50f)
                    : D2D1::ColorF(
                        0.10f, 0.12f, 0.16f,
                        hot ? 0.45f : 0.25f))
                : (active
                    ? D2D1::ColorF(
                        1.0f, 1.0f, 1.0f,
                        hot ? 0.95f : 0.60f)
                    : D2D1::ColorF(
                        1.0f, 1.0f, 1.0f,
                        hot ? 0.50f : 0.28f)));
    };
    draw(buttons.date,
        snowdesktop::menu_fluent_glyphs::kDateHeader,
        data_->dateHeaders);
    draw(buttons.list,
        data_->listMode ? L"\uF462" : L"\uF4ED", true);
    if (includeOpen) draw(buttons.open, L"\uF42E", true);
}
