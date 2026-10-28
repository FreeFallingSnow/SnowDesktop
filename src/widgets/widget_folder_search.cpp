#include "core/widget.h"
#include "widget_folder_search.h"
#include "app/app.h"
#include "common/l10n.h"
#include "navigation/search_match.h"
#include "widget/runtime/widget_item_layout.h"
#include "layout/collection_popup_layout.h"
#include "layout/item_render_layer_rules.h"
#include <unordered_set>
#include <shlwapi.h>

namespace
{
using State = snowdesktop::WidgetFolderSearchState;

std::wstring RootPath(std::wstring path)
{
    std::replace(path.begin(), path.end(), L'/', L'\\');
    if (!path.empty() && path.back() != L'\\') path += L'\\';
    return path;
}

bool Inside(const std::wstring& path, const std::wstring& root)
{
    const auto prefix = RootPath(root);
    return path.size() > prefix.size() &&
        _wcsnicmp(path.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// Keep the backing entry alive even if a later search replaces the view while
// an item is held by an OLE drag or a popup.
class SearchFolderIcon : public FolderEntryIcon
{
public:
    SearchFolderIcon(std::shared_ptr<FolderEntry> entry,
        Container* owner, DesktopApp* app)
        : FolderEntryIcon(entry.get(), owner, app), entry_(std::move(entry)) {}
private:
    std::shared_ptr<FolderEntry> entry_;
};

snowdesktop::widget_item_layout::Layout SearchLayout(
    const ScrollingItemWidget& widget)
{
    const auto* data = widget.GetWidgetData();
    const RECT content = widget.GetContentViewportRect();
    const auto metrics = widget.GetItemVisualMetrics();
    if (data->listMode)
        return snowdesktop::widget_item_layout::ResolveList(content,
            std::max(widget.GetListRowHeight(), metrics.minimumListHeight),
            widget.GetListSpacing(), snowdesktop::list_detail_rules::ListColumns(*data));
    return snowdesktop::widget_item_layout::ResolveGrid(content,
        widget.IsPopupHosted()
            ? snowdesktop::collection_popup_layout::ResolveGridColumnCount(
                content.right - content.left, metrics.minimumGridWidth)
            : std::max(1, data->gridSpan.columns), 0,
        metrics.minimumGridWidth, metrics.minimumGridHeight,
        widget.GetScrollingIconSpacing());
}
}

void ScrollingItemWidget::EnsureFolderSearch() const
{
    if (!data_ || !app_ || !data_->showSearchBox || searchText_.empty() ||
        IsPreviewRendering() || (data_->type != DesktopWidgetType::FileCategories &&
        data_->type != DesktopWidgetType::FolderMapping &&
        data_->type != DesktopWidgetType::FileGroup))
    {
        if (folderSearch_ && !folderSearch_->identity.empty())
        {
            if (folderSearch_->worker) folderSearch_->worker->Stop();
            folderSearch_.reset();
        }
        return;
    }
    if (!folderSearch_) folderSearch_ = std::make_shared<State>();
    auto& state = *folderSearch_;
    std::vector<const DesktopWidget*> sources;
    if (auto* group = dynamic_cast<const FileGroup*>(this))
    {
        for (const auto& id : group->GetVisibleSourceIds())
        {
            const auto index = app_->FindWidgetIndexById(id);
            if (index < app_->widgets_.size()) sources.push_back(&app_->widgets_[index]);
        }
    }
    else sources.push_back(data_);

    std::wstring sourceIdentity;
    for (const auto* source : sources)
    {
        sourceIdentity += source->id + L"\n" + source->title + L"\n" + source->sourceFolderPath + L"\n";
        for (const auto& key : source->itemKeys) sourceIdentity += key + L"\n";
        for (const auto& entry : source->folderEntries) sourceIdentity += entry.fullPath + L"\n" + entry.name + L"\n";
    }
    std::wstring identity = searchText_ + L"\n" + sourceIdentity;
    if (state.identity != identity)
    {
        const bool sourcesChanged = state.sourceIdentity != sourceIdentity || !state.worker;
        state.sourceIdentity = std::move(sourceIdentity);
        state.identity = std::move(identity);
        state.modelIdentity.clear();
        state.query = searchText_;
        state.rows.clear(); state.sections.clear(); state.results.clear();
        const_cast<ScrollingItemWidget*>(this)->InvalidateSlots();
        state.available = false;
        state.pending = true;
        ++state.generation;
        if (sourcesChanged)
        {
            if (state.worker) state.worker->Stop();
            state.sources.clear();
            for (const auto* source : sources)
            {
                State::Source scope;
                scope.id = source->id;
                scope.title = source->title.empty()
                    ? (source->type == DesktopWidgetType::FolderMapping
                        ? _LW("widget.folder_mapping") : _LW("widget.desktop_files")) : source->title;
                if (source->type == DesktopWidgetType::FolderMapping)
                {
                    if (!source->sourceFolderPath.empty()) scope.roots.push_back(source->sourceFolderPath);
                }
                else for (const auto& key : source->itemKeys)
                {
                    const size_t index = app_->FindItemIndexByKey(key);
                    if (index >= app_->items_.size()) continue;
                    const auto& item = app_->items_[index];
                    const DWORD attrs = GetFileAttributesW(item.parsingName.c_str());
                    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY))
                        scope.roots.push_back(item.parsingName);
                }
                state.sources.push_back(std::move(scope));
            }
            const auto scopes = state.sources;
            const auto provider = state.search;
            state.icons = std::make_shared<std::unordered_map<std::wstring, int>>();
            const auto icons = state.icons;
            state.worker = std::make_unique<snowdesktop::QuickNavigationEverythingSearchAsync>(
                [scopes, provider, icons](const std::wstring& query, DWORD limit) {
                    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
                    struct ComScope { HRESULT result; ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); } } comScope{com};
                    snowdesktop::QuickNavigationEverythingSearchResponse response;
                    auto client = provider ? nullptr : std::make_unique<EverythingSearchClient>();
                    std::unordered_set<std::wstring> seen;
                    bool queried = false;
                    for (const auto& scope : scopes)
                    {
                        if (scope.roots.empty()) continue;
                        queried = true;
                        std::wstring scoped = L"<";
                        for (const auto& root : scope.roots)
                        {
                            if (scoped.size() > 1) scoped += L"|";
                            scoped += L"\"" + RootPath(root) + L"\"";
                        }
                        scoped += L"> <" + query + L">";
                        auto part = provider ? provider(scoped, limit)
                            : snowdesktop::QuickNavigationEverythingSearchResponse{client->Search(scoped, limit), client->LastError()};
                        if (part.error != ERROR_SUCCESS) { response.error = part.error; return response; }
                        for (auto& result : part.results)
                            if (seen.insert(ToUpperInvariant(result.path)).second)
                            {
                                const wchar_t* extension = PathFindExtensionW(result.path.c_str());
                                const std::wstring key = result.isDirectory ? L"folder" : ToUpperInvariant(extension);
                                if (!icons->contains(key))
                                {
                                    SHFILEINFOW info{};
                                    const auto loaded = SHGetFileInfoW(result.isDirectory ? L"folder" : extension,
                                        result.isDirectory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL,
                                        &info, sizeof(info), SHGFI_SYSICONINDEX | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
                                    icons->emplace(key, loaded ? info.iIcon : -1);
                                }
                                response.results.push_back(std::move(result));
                            }
                    }
                    if (!queried)
                    {
                        if (provider) response.error = provider(query, 1).error;
                        else { client->Search(query, 1); response.error = client->LastError(); }
                    }
                    return response;
                });
        }
        // Each source is queried independently so a busy mapped folder cannot
        // consume the result budget of another source in the group.
        if (!state.worker->Submit(app_->hwnd_, snowdesktop::WidgetFolderSearchReadyMessage(),
                {state.generation, state.query, MAXDWORD})) state.pending = false;
    }
    bool changed = false;
    if (state.worker)
        if (auto completed = state.worker->TakeCompleted(); completed &&
            completed->generation == state.generation && completed->query == state.query)
        {
            state.pending = false;
            state.available = completed->error == ERROR_SUCCESS;
            state.results = std::move(completed->results);
            changed = true;
        }
    if (!state.available) return;
    std::wstring modelIdentity = state.identity + std::to_wstring(static_cast<int>(data_->contentSortColumn)) +
        std::to_wstring(data_->contentSortAscending);
    for (const auto* source : sources) modelIdentity += source->activeCategoryId + std::to_wstring(source->showFileCategories);
    if (!changed && state.modelIdentity == modelIdentity) return;
    state.modelIdentity = std::move(modelIdentity);

    // Rebuild lightweight references from current native filters/sort order.
    // The recursive backing entries are retained to preserve selection.
    std::unordered_map<std::wstring, std::shared_ptr<FolderEntry>> previous;
    for (const auto& row : state.rows)
        if (row.recursive) previous.emplace(row.sourceId + L"\n" + ToUpperInvariant(row.recursive->fullPath), row.recursive);
    std::vector<State::Row> rows;
    std::vector<State::Section> sections;
    std::unordered_set<std::wstring> directPaths;
    const auto addDirect = [&](const DesktopWidget& source, std::vector<State::Row>& destination) {
        if (const auto* group = dynamic_cast<const FileGroup*>(this))
        {
            for (const auto& result : group->GetGroupSearchResults())
            {
                if (result.sourceId != source.id) continue;
                if (result.folderMapping)
                    directPaths.insert(ToUpperInvariant(source.folderEntries[result.folderEntryIndex].fullPath));
                else
                {
                    const size_t index = app_->FindItemIndexByKey(result.desktopKey);
                    if (index < app_->items_.size()) directPaths.insert(ToUpperInvariant(app_->items_[index].parsingName));
                }
                destination.push_back({source.id, result.desktopKey, result.folderEntryIndex, {}});
            }
            return;
        }
        if (source.type == DesktopWidgetType::FileCategories)
        {
            const auto* categories = dynamic_cast<const FileCategories*>(this);
            for (const auto& key : categories->GetSearchResultKeys())
            {
                const size_t index = app_->FindItemIndexByKey(key);
                if (index >= app_->items_.size() || !NameMatchesQuery(app_->items_[index].name, searchText_)) continue;
                directPaths.insert(ToUpperInvariant(app_->items_[index].parsingName));
                destination.push_back({source.id, key, 0, {}});
            }
        }
        else for (size_t i : dynamic_cast<const FolderMapping*>(this)->GetVisibleEntryIndices())
        {
            const auto& entry = source.folderEntries[i];
            if (!NameMatchesQuery(entry.name, searchText_)) continue;
            directPaths.insert(ToUpperInvariant(entry.fullPath));
            destination.push_back({source.id, {}, i, {}});
        }
    };
    for (const auto* source : sources)
    {
        const size_t first = rows.size();
        addDirect(*source, rows);
        if (rows.size() > first)
            sections.push_back({sources.size() > 1 ? (source->title.empty()
                ? (source->type == DesktopWidgetType::FolderMapping ? _LW("widget.folder_mapping") : _LW("widget.desktop_files"))
                : source->title) : L"", first, rows.size() - first});
    }
    for (const auto& scope : state.sources)
    {
        const size_t first = rows.size();
        for (const auto& result : state.results)
        {
            if (directPaths.contains(ToUpperInvariant(result.path)) ||
                !std::any_of(scope.roots.begin(), scope.roots.end(), [&](const auto& root) { return Inside(result.path, root); })) continue;
            const std::wstring key = scope.id + L"\n" + ToUpperInvariant(result.path);
            auto& entry = previous[key];
            if (!entry)
            {
                entry = std::make_shared<FolderEntry>();
                entry->name = result.name;
                entry->fullPath = result.path;
                entry->isDirectory = result.isDirectory;
                entry->lastWriteTime = result.dateModified;
                const std::wstring iconKey = result.isDirectory ? L"folder" : ToUpperInvariant(PathFindExtensionW(result.path.c_str()));
                if (const auto icon = state.icons->find(iconKey); icon != state.icons->end()) entry->sysIconIndex = icon->second;
            }
            rows.push_back({scope.id, {}, 0, entry});
        }
        if (rows.size() > first) sections.push_back({scope.title, first, rows.size() - first});
    }
    if (rows.size() != state.rows.size()) changed = true;
    if (!changed)
        for (size_t i = 0; i < rows.size(); ++i)
            if (rows[i].sourceId != state.rows[i].sourceId || rows[i].desktopKey != state.rows[i].desktopKey ||
                rows[i].folderIndex != state.rows[i].folderIndex || rows[i].recursive != state.rows[i].recursive) { changed = true; break; }
    state.rows = std::move(rows); state.sections = std::move(sections);
    if (changed) const_cast<ScrollingItemWidget*>(this)->InvalidateSlots();
}

bool ScrollingItemWidget::UsesFolderSearch() const
{
    EnsureFolderSearch();
    return folderSearch_ && folderSearch_->available;
}

size_t ScrollingItemWidget::FolderSearchCount() const { return folderSearch_->rows.size(); }

int ScrollingItemWidget::FolderSearchContentHeight() const
{
    int height = 0;
    const auto layout = SearchLayout(*this);
    for (const auto& section : folderSearch_->sections)
    {
        if (!section.title.empty()) height += Cu(data_->listMode ? 28.0f : 36.0f);
        height += snowdesktop::widget_item_layout::ContentHeight(layout, section.count);
    }
    return height;
}

RECT ScrollingItemWidget::FolderSearchItemRect(size_t index) const
{
    int y = 0;
    const auto layout = SearchLayout(*this);
    for (const auto& section : folderSearch_->sections)
    {
        if (!section.title.empty()) y += Cu(data_->listMode ? 28.0f : 36.0f);
        if (index >= section.first && index - section.first < section.count)
        {
            RECT cell = snowdesktop::widget_item_layout::ItemRect(layout, index - section.first, GetScrollOffset());
            OffsetRect(&cell, 0, y);
            return cell;
        }
        y += snowdesktop::widget_item_layout::ContentHeight(layout, section.count);
    }
    return {};
}

Item* ScrollingItemWidget::FolderSearchItem(size_t index, bool dragCache) const
{
    if (index >= FolderSearchCount()) return nullptr;
    const auto& row = folderSearch_->rows[index];
    auto* owner = const_cast<ScrollingItemWidget*>(this);
    std::unique_ptr<Item> item;
    if (row.recursive) item = std::make_unique<SearchFolderIcon>(row.recursive, owner, app_);
    else if (!row.desktopKey.empty())
    {
        const size_t i = app_->FindItemIndexByKey(row.desktopKey);
        if (i < app_->items_.size()) item = std::make_unique<DesktopIcon>(&app_->items_[i], owner, app_);
    }
    else
    {
        const size_t i = app_->FindWidgetIndexById(row.sourceId);
        if (i < app_->widgets_.size() && row.folderIndex < app_->widgets_[i].folderEntries.size())
            item = std::make_unique<FolderEntryIcon>(&app_->widgets_[i].folderEntries[row.folderIndex], owner, app_);
    }
    if (!item) return nullptr;
    item->SetBounds(FolderSearchItemRect(index));
    auto* raw = item.get();
    (dragCache ? dragSourceCache_ : slotItemCache_).push_back(std::move(item));
    return raw;
}

std::vector<std::unique_ptr<Slot>> ScrollingItemWidget::BuildFolderSearchSlots()
{
    slotItemCache_.clear();
    std::vector<std::unique_ptr<Slot>> slots;
    const RECT viewport = GetContentViewportRect();
    for (size_t i = 0; i < FolderSearchCount(); ++i)
    {
        const RECT cell = FolderSearchItemRect(i);
        if (cell.bottom <= viewport.top || cell.top >= viewport.bottom) continue;
        auto slot = std::make_unique<Slot>(this, cell, i);
        slot->SetItem(FolderSearchItem(i, false));
        slots.push_back(std::move(slot));
    }
    return slots;
}

std::vector<size_t> ScrollingItemWidget::FolderSearchSelectedIndices() const
{
    std::vector<size_t> selected;
    for (size_t i = 0; i < FolderSearchCount(); ++i)
    {
        const auto& row = folderSearch_->rows[i];
        bool isSelected = row.recursive && row.recursive->selected;
        if (!row.desktopKey.empty())
        {
            const size_t index = app_->FindItemIndexByKey(row.desktopKey);
            isSelected = index < app_->items_.size() && app_->items_[index].selected;
        }
        else if (!row.recursive)
        {
            const size_t index = app_->FindWidgetIndexById(row.sourceId);
            isSelected = index < app_->widgets_.size() && row.folderIndex < app_->widgets_[index].folderEntries.size() &&
                app_->widgets_[index].folderEntries[row.folderIndex].selected;
        }
        if (isSelected) selected.push_back(i);
    }
    return selected;
}

std::vector<Item*> ScrollingItemWidget::FolderSearchSelectedItems() const
{
    dragSourceCache_.clear();
    std::vector<Item*> selected;
    for (size_t i : FolderSearchSelectedIndices())
        if (auto* item = FolderSearchItem(i, true)) selected.push_back(item);
    return selected;
}

void ScrollingItemWidget::ApplyFolderSearchMarquee(const RECT& contentRect)
{
    dragSourceCache_.clear();
    for (size_t i = 0; i < FolderSearchCount(); ++i)
        if (auto* item = FolderSearchItem(i, true))
        {
            RECT cell = item->GetBounds(); OffsetRect(&cell, 0, GetScrollOffset());
            item->SetSelected(RectsIntersect(cell, contentRect));
        }
}

std::wstring ScrollingItemWidget::FolderSearchSourceId(const Item* item) const
{
    if (!item || !folderSearch_) return {};
    for (const auto& row : folderSearch_->rows)
    {
        if (row.recursive && row.recursive->fullPath == item->GetPath()) return row.sourceId;
        if (!row.desktopKey.empty())
        {
            const size_t index = app_->FindItemIndexByKey(row.desktopKey);
            if (index < app_->items_.size() && app_->items_[index].parsingName == item->GetPath()) return row.sourceId;
        }
        else if (!row.recursive)
        {
            const size_t index = app_->FindWidgetIndexById(row.sourceId);
            if (index < app_->widgets_.size() && row.folderIndex < app_->widgets_[index].folderEntries.size() &&
                app_->widgets_[index].folderEntries[row.folderIndex].fullPath == item->GetPath()) return row.sourceId;
        }
    }
    return {};
}

void ScrollingItemWidget::DrawFolderSearch(ID2D1DeviceContext* context)
{
    const RECT content = GetContentViewportRect();
    DrawDetailsHeader(context, content);
    const bool light = UsesLightContentTheme();
    const bool privacy = data_->privacyMode && !app_->dragSession_.IsActive() &&
        !app_->dragDropController_.IsExternalDragActive() && !PtInRect(&data_->bounds, app_->lastMousePoint_);
    const auto& slots = GetSlots();
    const snowdesktop::ScrollContentClip clip(context, scrollContentFadeCache_, content,
        GetScrollOffset(), GetTotalContentHeight(), static_cast<float>(Cu(16.0f)));
    if (FolderSearchCount() == 0)
    {
        app_->DrawD2DText(context, _LW("widget.categories.no_results"), content,
            GetCuTextFormat(13.0f, false, true), light ? D2D1::ColorF(0, 0, 0, 0.72f) : D2D1::ColorF(1, 1, 1, 0.52f));
        return;
    }
    int y = content.top - GetScrollOffset();
    const auto layout = SearchLayout(*this);
    for (const auto& section : folderSearch_->sections)
    {
        if (!section.title.empty())
        {
            const int height = Cu(data_->listMode ? 28.0f : 36.0f);
            RECT label{content.left + Cu(8.0f), y, content.right, y + height};
            app_->DrawD2DText(context, section.title, label,
                light ? GetCuTextFormatWeight(13.0f, DWRITE_FONT_WEIGHT_LIGHT, false) : GetCuTextFormat(13.0f, false, false),
                light ? D2D1::ColorF(0, 0, 0, 0.72f) : D2D1::ColorF(1, 1, 1, 0.52f));
            y += height;
        }
        y += snowdesktop::widget_item_layout::ContentHeight(layout, section.count);
    }
    std::vector<std::pair<Item*, RECT>> foreground;
    for (const auto& slot : slots)
    {
        auto* item = slot->GetItem(); if (!item) continue;
        const RECT cell = slot->GetBounds();
        ListItemDetails details;
        int systemIcon = -1;
        bool thumbnail = false;
        bool drawTitle = true;
        if (auto* desktop = dynamic_cast<DesktopIcon*>(item))
        {
            const auto* source = desktop->GetDesktopItem();
            systemIcon = source->sysIconIndex; thumbnail = source->iconIsMediaThumbnail;
            details = {source->typeName, source->modifiedTime, source->fileSize, false};
            drawTitle = !app_->IsRenamingItem(source);
        }
        else if (auto* folder = dynamic_cast<FolderEntryIcon*>(item))
        {
            const auto* source = folder->GetFolderEntry();
            systemIcon = source->sysIconIndex; thumbnail = source->iconIsMediaThumbnail;
            details = {source->typeName, source->lastWriteTime, source->fileSize, source->isDirectory};
            drawTitle = !app_->IsRenamingItem(source);
        }
        if (privacy) DrawPrivacyPlaceholder(context, cell, item->GetTitle(), details.isDirectory);
        else if (data_->listMode) DrawListItem(context, cell, item->GetIconBitmap(), systemIcon,
            item->GetTitle(), item->IsSelected(), thumbnail, {}, nullptr, details, std::nullopt, drawTitle, item->GetPath());
        else
        {
            const auto layers = snowdesktop::item_render_layer_rules::ResolveTitleLayerPlan(item->IsSelected());
            const int visual = item->IsSelected() ? 2 : (PtInRect(&cell, app_->lastMousePoint_) ? 1 : 0);
            if (auto* desktop = dynamic_cast<DesktopIcon*>(item)) desktop->Draw(context, cell, visual, light, layers.drawWithItem);
            else if (auto* folder = dynamic_cast<FolderEntryIcon*>(item)) folder->Draw(context, cell, visual, light, layers.drawWithItem);
            if (layers.drawInForeground) foreground.emplace_back(item, cell);
        }
    }
    for (const auto& [item, bounds] : foreground) item->DrawTitle(context, bounds, true, 1.0f, light);
}
