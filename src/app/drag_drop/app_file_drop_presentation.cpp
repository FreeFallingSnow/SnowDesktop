#include "app/app.h"
#include "common/utils.h"
#include "icons/shell_icon_bitmap.h"
#include "widgets/widget_folder_search.h"

using snowdesktop::file_drop_presentation::Layout;

void DesktopApp::CaptureFileDropAppearance(PendingLandingCache& cache, const DragSourceList& sources)
{
    cache.originWidgetId = sources.originWidgetId;
    cache.files.reserve(sources.entries.size());
    for (const auto& source : sources.entries)
    {
        auto file = std::shared_ptr<FolderEntry>(new FolderEntry, [this](FolderEntry* entry) {
            if (entry->iconBitmap) EraseD2DIconCacheForBitmap(entry->iconBitmap);
            delete entry;
        });
        const auto* icon = dynamic_cast<const FolderEntryIcon*>(source.item);
        const auto* folder = icon ? icon->GetFolderEntry() : nullptr;
        const auto* desktopIcon = dynamic_cast<const DesktopIcon*>(source.item);
        const auto index = !desktopIcon && !folder
            ? FindItemIndexByKey(source.desktopKey.empty() ? source.filePath : source.desktopKey) : items_.size();
        const auto* desktop = desktopIcon ? desktopIcon->GetDesktopItem() : index < items_.size() ? &items_[index] : nullptr;
        if (folder)
        {
            file->name = folder->name;
            file->displayName = folder->DisplayName();
            file->typeName = folder->typeName;
            file->isDirectory = folder->isDirectory;
            file->lastWriteTime = folder->lastWriteTime;
            file->fileSize = folder->fileSize;
            file->isShortcut = folder->isShortcut;
            file->isApplicationShortcut = folder->isApplicationShortcut;
            file->iconIsMediaThumbnail = folder->iconIsMediaThumbnail;
            file->iconState = folder->iconState;
            file->sysIconIndex = folder->sysIconIndex;
        }
        else if (desktop)
        {
            file->displayName = desktop->name;
            file->sysIconIndex = desktop->sysIconIndex;
            file->typeName = desktop->typeName;
            file->fileSize = desktop->fileSize;
            file->lastWriteTime = desktop->modifiedTime.value_or(FILETIME{});
            file->isShortcut = desktop->isShortcut;
            file->isApplicationShortcut = desktop->isApplicationShortcut;
            file->iconIsMediaThumbnail = desktop->iconIsMediaThumbnail;
            file->iconState = desktop->iconState;
        }
        const HBITMAP bitmap = folder ? folder->iconBitmap : desktop ? desktop->iconBitmap :
            source.item ? source.item->GetIconBitmap() : nullptr;
        if (bitmap)
        {
            file->iconBitmap = static_cast<HBITMAP>(CopyImage(bitmap, IMAGE_BITMAP, 0, 0, LR_CREATEDIBSECTION));
            if (file->iconBitmap)
            {
                BITMAP info{};
                if (GetObjectW(file->iconBitmap, sizeof(info), &info))
                    file->iconBitmapSize = {info.bmWidth, info.bmHeight};
            }
        }
        file->fullPath = source.filePath;
        if (file->name.empty()) file->name = FileNameFromPath(source.filePath);
        if (file->displayName.empty()) file->displayName = source.displayName.empty() ? file->name : source.displayName;
        file->isCut = true;
        file->selected = false;
        cache.files.push_back({source.sourceIndex, source.filePath, source.desktopKey, file,
            source.item ? source.item->GetBounds() : desktop ? desktop->bounds : RECT{}});
        if (!hwnd_ || file->iconBitmap || file->sysIconIndex >= 0 || file->fullPath.empty()) continue;
        const auto id = cache.visualId;
        shellVisualWork_.Submit(L"drop-icon:" + std::to_wstring(id) + L":" + std::to_wstring(source.sourceIndex),
            [path = file->fullPath] {
                auto pixels = std::make_shared<snowdesktop::BackgroundBitmap>();
                Pidl pidl(ILCreateFromPathW(path.c_str()));
                if (pidl.get())
                    pixels->bitmap = GetHighResolutionShellIconBitmap(pidl.get(), -1, pixels->size, true, 96);
                // A move can win the race to the old name. Its file-type icon
                // remains available without reading the vanished file.
                if (!pixels->bitmap)
                {
                    SHFILEINFOW info{};
                    if (SHGetFileInfoW(path.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info),
                            SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES) && info.hIcon)
                    {
                        pixels->bitmap = CreateAlphaBitmapFromIcon(info.hIcon, 48, 48, pixels->size);
                        DestroyIcon(info.hIcon);
                    }
                }
                return pixels;
            }, [this, id, weak = std::weak_ptr<FolderEntry>(file)](auto pixels) {
                const auto target = weak.lock();
                if (!target || !pixels || !pixels->bitmap) return;
                target->iconBitmap = std::exchange(pixels->bitmap, nullptr);
                target->iconBitmapSize = pixels->size;
                for (const auto& pending : pendingLandingCaches_)
                    if (pending.visualId == id) { InvalidateFileDropVisual(pending); break; }
            }, hwnd_, kBackgroundShellReadyMessage);
    }
}

bool DesktopApp::IsPendingMovedFile(const std::wstring& path, const std::wstring& key) const
{
    for (const auto& pending : pendingLandingCaches_)
        if (!pending.movedSourceKeys.empty() &&
            (pending.movedSourceKeys.contains(ToUpperInvariant(path)) ||
             (!key.empty() && key != path && pending.movedSourceKeys.contains(ToUpperInvariant(key))))) return true;
    return false;
}

const Layout& DesktopApp::FileDropMemberLayout(const DesktopWidget& widget, const WidgetContainer* view) const
{
    static const Layout empty;
    if (pendingLandingCaches_.empty() || (view && view->IsPreviewRendering())) return empty;
    const auto* model = view ? view->GetContentViewData() : &widget;
    if (!model) return empty;
    const auto generation = view ? view->GetSlotGeneration() : dockFolderPopupContainer_ ? dockFolderPopupContainer_->GetSlotGeneration() : 0;
    const size_t count = model->type == DesktopWidgetType::FolderMapping ? model->folderEntries.size() : model->itemKeys.size();
    auto& cached = fileDropLayouts_[view];
    if (cached.model == model && cached.generation == generation && cached.memberCount == count &&
        cached.folderPath == model->sourceFolderPath) return cached.layout;
    cached = {model, generation, count, model->sourceFolderPath, {}};
    auto& layout = cached.layout;
    const WidgetContainer* content = view;
    if (const auto* group = dynamic_cast<const FileGroup*>(view)) content = group->GetActiveSourceContainer();
    // A search can mix members of several folders with recursive Shell hits.
    // Keep their view indices independent from the backing folder's indices.
    struct SearchMember { const DesktopWidget* owner; size_t index; std::wstring key; };
    std::vector<SearchMember> searchMembers;
    const auto* scrolling = dynamic_cast<const ScrollingItemWidget*>(view);
    const auto* groupView = dynamic_cast<const FileGroup*>(view);
    const auto search = scrolling ? scrolling->folderSearch_.get() : nullptr;
    const bool recursive = search && (search->available || search->pending);
    const bool combined = groupView && groupView->IsGroupSearchActive();
    const auto appendSearchMember = [&](const auto& ownerId, size_t member, const auto& key, const FolderEntry* hit) {
        const size_t index = FindWidgetIndexById(ownerId);
        const auto* owner = index < widgets_.size() ? &widgets_[index] : nullptr;
        if (!key.empty() && owner)
            member = std::find(owner->itemKeys.begin(), owner->itemKeys.end(), key) - owner->itemKeys.begin();
        searchMembers.push_back({owner, member, hit ? hit->fullPath :
            key.empty() && owner && member < owner->folderEntries.size() ? owner->folderEntries[member].fullPath : key});
    };
    if (recursive)
        for (const auto& row : search->rows) appendSearchMember(row.sourceId, row.folderIndex, row.desktopKey, row.recursive.get());
    else if (combined)
        for (const auto& row : groupView->GetGroupSearchResults())
            appendSearchMember(row.sourceId, row.folderEntryIndex, row.desktopKey, nullptr);
    const auto* mapping = dynamic_cast<const FolderMapping*>(content);
    if (!view && folderPreview_ && &widget == GetOpenPopupWidget())
        mapping = dynamic_cast<const FolderMapping*>(GetCategorizedPopupView());
    const auto* categories = dynamic_cast<const FileCategories*>(content);
    const auto* collections = dynamic_cast<const CollectionGroup*>(view);
    const bool folder = model->type == DesktopWidgetType::FolderMapping;
    const auto* visible = mapping ? &mapping->GetVisibleEntryIndices() : nullptr;
    const auto& keys = collections ? collections->GetVisibleItemKeys() : categories ? categories->GetSearchResultKeys() :
        !view && !folder ? GetPopupItemKeysView(widget) : model->itemKeys;
    const bool searching = recursive || combined;
    const size_t length = searching ? searchMembers.size() : folder ? (visible ? visible->size() : count) : keys.size();
    const auto boundary = [&](size_t raw, const std::wstring& owner) -> size_t {
        if (searching)
        {
            size_t end = length;
            for (size_t i = 0; i < length; ++i)
                if (searchMembers[i].owner && searchMembers[i].owner->id == owner)
                {
                    if (searchMembers[i].index >= raw) return i;
                    end = i + 1;
                }
            return end;
        }
        if (raw >= count) return length;
        if (visible) return std::find(visible->begin(), visible->end(), raw) - visible->begin();
        if (!folder && &keys != &model->itemKeys)
            return std::find(keys.begin(), keys.end(), model->itemKeys[raw]) - keys.begin();
        return raw;
    };
    const auto seesOwner = [&](const std::wstring& id) {
        return id == model->id || (searching && std::any_of(searchMembers.begin(), searchMembers.end(),
            [&](const auto& row) { return row.owner && row.owner->id == id; }));
    };
    std::unordered_set<std::wstring> removed;
    for (const auto& request : pendingLandingCaches_)
    {
        removed.insert(request.movedSourceKeys.begin(), request.movedSourceKeys.end());
        if (!request.visualId || request.dockMonitor) continue;
        for (const auto& entry : request.entries)
            if (entry.kind == DropLandingKind::WidgetIndex && seesOwner(entry.widgetId))
            {
                const auto* file = request.FindFile(entry.sourceIndex);
                // All members of one batch share the insertion boundary.
                layout.gaps.push_back({boundary(request.entries.front().insertIndex, entry.widgetId),
                    file ? file->appearance : nullptr});
                if (!entry.createdPath.empty()) removed.insert(ToUpperInvariant(entry.createdPath));
            }
        for (const auto& group : request.folderPlacements)
        {
            if (searching)
            {
                if (!seesOwner(group.widgetId)) continue;
            }
            else if (!folder || !PathsEqualInsensitive(group.sourceFolderPath, model->sourceFolderPath) ||
                (model->id == kDockFolderPopupWidgetId ? group.popupSourceId != dockFolderPopupSourceId_ :
                    group.widgetId != model->id)) continue;
            const auto start = boundary(group.insertIndex, group.widgetId);
            if (request.files.empty()) layout.gaps.push_back({start, nullptr});
            else if (group.sourcePaths.empty())
                for (const auto& file : request.files) layout.gaps.push_back({start, file.appearance});
            else
            {
                std::unordered_map<std::wstring, std::shared_ptr<FolderEntry>> byPath;
                for (const auto& file : request.files) byPath.emplace(ToUpperInvariant(file.sourcePath), file.appearance);
                for (const auto& path : group.sourcePaths)
                    if (const auto file = byPath.find(ToUpperInvariant(path)); file != byPath.end())
                        layout.gaps.push_back({start, file->second});
            }
            for (const auto& path : group.createdPaths) removed.insert(ToUpperInvariant(path));
        }
    }
    if (!removed.empty())
        for (size_t i = 0; i < length; ++i)
        {
            const auto& key = searching ? searchMembers[i].key : folder ? model->folderEntries[visible ? (*visible)[i] : i].fullPath : keys[i];
            if (removed.contains(ToUpperInvariant(key))) layout.hidden.push_back(i);
        }
    layout.Arrange();
    return layout;
}

void DesktopApp::DrawPendingFile(ID2D1DeviceContext* context, const RECT& cell, FolderEntry* file,
    WidgetContainer* view, bool listMode, bool light)
{
    if (IsRectEmpty(&cell)) return;
    if (!file)
    {
        // Delayed OLE sources may not have published even a filename yet.
        DrawPlaceholderIcon(context, -1, GetItemIconRect(cell), 0.4f, false);
        return;
    }
    if (listMode)
    {
        ScrollingItemWidget* scrolling = dynamic_cast<ScrollingItemWidget*>(view);
        if (scrolling)
        {
            scrolling->DrawListItem(context, cell, file->iconBitmap, file->sysIconIndex,
                file->DisplayName(), false, file->iconIsMediaThumbnail, {}, nullptr,
                {file->typeName, file->lastWriteTime, file->fileSize, file->isDirectory, true}, light, true, file->fullPath);
            return;
        }
    }
    FolderEntryIcon icon(file, view, this);
    icon.Draw(context, cell, 0, light, true);
}
