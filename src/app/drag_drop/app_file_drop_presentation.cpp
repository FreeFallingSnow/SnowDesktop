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

void DesktopApp::InvalidateFileRemovalViews()
{
    // A viewport can now need a different set of model slots. Keep the
    // release target valid while a native/OLE drop still owns its bindings.
    if (!dragSession_.HasContext())
    {
        for (const auto& container : containers_)
            if (auto* view = dynamic_cast<WidgetContainer*>(container.get()))
            {
                view->InvalidateSlots();
                if (auto* group = dynamic_cast<FileGroup*>(view))
                    if (auto* source = group->GetActiveSourceContainer()) source->InvalidateSlots();
            }
        if (dockFolderPopupContainer_) dockFolderPopupContainer_->InvalidateSlots();
    }
    fileDropLayouts_.clear();
    if (!hwnd_) return;
    InvalidateDragStaticScene(false);
    InvalidateRect(hwnd_, nullptr, FALSE);
    InvalidateCollectionPopupContent();
    InvalidateFloatingPopupWindow(false);
    InvalidateFloatingDockWindow(false);
}

void DesktopApp::BeginFileRemovalVisibility(const std::vector<std::wstring>& paths)
{
    bool changed = false;
    for (const auto& path : paths)
    {
        auto [it, inserted] = fileRemovalVisibility_.try_emplace(ToUpperInvariant(path));
        ++it->second.requests;
        changed |= inserted;
    }
    if (changed) InvalidateFileRemovalViews();
}

void DesktopApp::HoldReleasingFileSources(DropAction action)
{
    if (action != DropAction::Move || !releasingFileSources_.empty() ||
        dynamic_cast<DockContainer*>(dragSession_.Source())) return;
    releasingFileSources_ = dragSession_.SourceList().FilePaths();
    BeginFileRemovalVisibility(releasingFileSources_);
}

void DesktopApp::FinishFileRemovalVisibility(const std::vector<std::wstring>& paths,
    const std::vector<std::wstring>& departed)
{
    if (paths.empty()) return;
    std::unordered_set<std::wstring> folders;
    bool desktop = false;
    if (!departed.empty())
    {
        // Only reread source folders represented by a live directory view.
        // Collection members share the desktop model, including on other pages.
        std::unordered_set<std::wstring> displayedFolders;
        const auto addFolder = [&](const DesktopWidget& model) {
            if (model.type == DesktopWidgetType::FolderMapping && !model.sourceFolderPath.empty())
                displayedFolders.insert(snowdesktop::shell_refresh::FolderKey(model.sourceFolderPath));
        };
        for (const auto& widget : widgets_) addFolder(widget);
        if (dockFolderPopupOpen_) addFolder(dockFolderPopupWidget_);
        for (const auto& preview : inactiveInlineFolderPreviews_) addFolder(preview.second.model.data);
        std::unordered_set<std::wstring> departedKeys;
        for (const auto& path : departed)
        {
            const auto key = ToUpperInvariant(path);
            const auto it = fileRemovalVisibility_.find(key);
            if (it == fileRemovalVisibility_.end()) continue;
            departedKeys.insert(key);
            auto& state = it->second;
            if (FindItemIndexByKey(path) < items_.size())
            {
                state.awaitingDesktop = true;
                desktop = true;
            }
            auto folder = snowdesktop::shell_refresh::FolderKey(std::filesystem::path(path).parent_path().wstring());
            if (displayedFolders.contains(folder))
            {
                state.awaitingFolder = folder;
                folders.insert(std::move(folder));
            }
        }
        // Recursive search owns its previous result rows independently of the
        // directory model while a new query is pending.
        for (const auto& container : containers_)
            if (const auto* view = dynamic_cast<ScrollingItemWidget*>(container.get()); view && view->folderSearch_)
            {
                auto& search = *view->folderSearch_;
                if (std::erase_if(search.results, [&](const auto& row) { return departedKeys.contains(ToUpperInvariant(row.path)); }))
                {
                    search.modelIdentity.clear();
                    search.refreshRequested = true;
                }
            }
    }
    for (const auto& path : paths)
    {
        const auto it = fileRemovalVisibility_.find(ToUpperInvariant(path));
        if (it == fileRemovalVisibility_.end()) continue;
        auto& state = it->second;
        if (--state.requests == 0 && !state.awaitingDesktop && state.awaitingFolder.empty())
            fileRemovalVisibility_.erase(it);
    }
    InvalidateFileRemovalViews();
    if (desktop) RequestShellRefresh();
    if (!folders.empty()) RequestFolderRefresh({folders.begin(), folders.end()});
}

void DesktopApp::ApplyFileRemovalRefresh(const snowdesktop::shell_refresh::Snapshot& snapshot)
{
    if (fileRemovalVisibility_.empty()) return;
    const bool desktop = !snapshot.foldersOnly && snapshot.desktopComplete && !snapshot.desktopIncremental;
    bool removed = false;
    for (auto it = fileRemovalVisibility_.begin(); it != fileRemovalVisibility_.end();)
    {
        auto& state = it->second;
        if (desktop) state.awaitingDesktop = false;
        const auto folder = snapshot.folders.find(state.awaitingFolder);
        if (folder != snapshot.folders.end() && folder->second.complete) state.awaitingFolder.clear();
        if (!state.requests && !state.awaitingDesktop && state.awaitingFolder.empty())
        {
            it = fileRemovalVisibility_.erase(it);
            removed = true;
        }
        else ++it;
    }
    if (removed) InvalidateFileRemovalViews();
}

bool DesktopApp::IsPendingMovedFile(const std::wstring& path, const std::wstring& key) const
{
    if (!fileRemovalVisibility_.empty() && (fileRemovalVisibility_.contains(ToUpperInvariant(path)) ||
        (!key.empty() && key != path && fileRemovalVisibility_.contains(ToUpperInvariant(key))))) return true;
    for (const auto& pending : pendingLandingCaches_)
        if (!pending.movedSourceKeys.empty() &&
            (pending.movedSourceKeys.contains(ToUpperInvariant(path)) ||
             (!key.empty() && key != path && pending.movedSourceKeys.contains(ToUpperInvariant(key))))) return true;
    return false;
}

const Layout& DesktopApp::FileDropMemberLayout(const DesktopWidget& widget, const WidgetContainer* view) const
{
    static const Layout empty;
    if ((pendingLandingCaches_.empty() && fileRemovalVisibility_.empty()) || (view && view->IsPreviewRendering())) return empty;
    const auto* model = view ? view->GetContentViewData() : &widget;
    if (!model) return empty;
    const auto generation = view ? view->GetSlotGeneration() : dockFolderPopupContainer_ ? dockFolderPopupContainer_->GetSlotGeneration() : 0;
    const size_t count = model->type == DesktopWidgetType::FolderMapping ? model->folderEntries.size() : model->itemKeys.size();
    auto& cached = fileDropLayouts_[view];
    if (cached.model == model && cached.generation == generation && cached.memberCount == count &&
        cached.folderPath == model->sourceFolderPath) return cached.layout;
    cached = {model, generation, count, model->sourceFolderPath, {}};
    auto& layout = cached.layout;
    auto members = [&] {
        if (view) return view->VisibleDropMembers(*model);
        if (folderPreview_ && &widget == GetOpenPopupWidget())
            if (const auto* popup = GetCategorizedPopupView()) return popup->VisibleDropMembers(*model);
        WidgetContainer::DropMembers popup(*model);
        if (model->type != DesktopWidgetType::FolderMapping) popup.keys = &GetPopupItemKeysView(widget);
        return popup;
    }();
    std::unordered_set<std::wstring> removed;
    const auto popupId = model->id == kDockFolderPopupWidgetId ? PendingFolderPopupId() : std::wstring{};
    for (const auto& [key, state] : fileRemovalVisibility_) removed.insert(key);
    for (const auto& request : pendingLandingCaches_)
    {
        removed.insert(request.movedSourceKeys.begin(), request.movedSourceKeys.end());
        if (!request.visualId || request.dockMonitor) continue;
        for (const auto& entry : request.entries)
            if (entry.kind == DropLandingKind::WidgetIndex && members.ContainsOwner(entry.widgetId))
            {
                const auto* file = request.FindFile(entry.sourceIndex);
                // All members of one batch share the insertion boundary.
                layout.gaps.push_back({members.VisibleBoundary(request.entries.front().insertIndex, entry.widgetId),
                    file ? file->appearance : nullptr});
                if (!entry.createdPath.empty()) removed.insert(ToUpperInvariant(entry.createdPath));
            }
        for (const auto& group : request.folderPlacements)
        {
            if (members.search)
            {
                if (!members.ContainsOwner(group.widgetId)) continue;
            }
            else if (model->type != DesktopWidgetType::FolderMapping || !PathsEqualInsensitive(group.sourceFolderPath, model->sourceFolderPath) ||
                (model->id == kDockFolderPopupWidgetId ? group.popupSourceId != popupId :
                    group.widgetId != model->id)) continue;
            const auto start = members.VisibleBoundary(group.insertIndex, group.widgetId);
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
        for (size_t i = 0; i < members.Count(); ++i)
        {
            const auto& key = members.KeyAt(i);
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
